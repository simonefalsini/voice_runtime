#pragma once

// ---------------------------------------------------------------------------
// RealInterpreterNode.h
//
// Header-only production interpreter node for the voice_runtime pipeline.
// Sits between the LLM output queue and the TTS input queue.
//
// Responsibilities:
//   1. Filter out <think>...</think> reasoning blocks (DS4 CoT model)
//   2. Filter out tool-call blocks:
//      - <｜DSML｜tool_calls>...</｜DSML｜tool_calls>
//      - <tool_calls>...</tool_calls>
//   3. Filter out [TOOL_CALL] prefixed chunks
//   4. Preserve [lang=XX] language markers
//   5. Optionally convert LaTeX to spoken text (TextProcessing::latex_to_speech)
//   6. Forward clean text to TTS
//
// The fundamental challenge is that text arrives as a STREAM of small chunks
// from the LLM.  Tags like "<think>" may be split across multiple chunks
// (e.g. chunk1="<thi", chunk2="nk>").  The state machine buffers partial
// tag matches in a pending buffer and only emits text once it can confirm
// the bytes are NOT part of a tag.
//
// State machine design adapted from DS4 ds4_cli.c token_printer (lines 339-461).
// The DS4 C code uses bytes_has_prefix / bytes_is_partial_prefix for streaming
// tag detection with a fixed pending buffer.  This C++ version generalises the
// approach to handle multiple tag pairs and uses std::string for the buffer.
//
// Namespace: voice_runtime
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "TextProcessing.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// InterpreterConfig
// ---------------------------------------------------------------------------

struct InterpreterConfig {
    bool filterThinkTags  = true;   // Remove <think>...</think> blocks
    bool filterToolCalls  = true;   // Remove tool-call blocks and [TOOL_CALL] lines
    bool convertLatex     = true;   // Convert LaTeX to spoken text
    bool detectLanguage   = true;   // Preserve [lang=XX] markers
    std::string preferredLanguage = "en";
};

// ---------------------------------------------------------------------------
// ThinkTagFilter — streaming state machine for tag filtering
//
// Processes a stream of text chunks and removes tagged blocks.
// Handles partial tag matches across chunk boundaries using a pending buffer.
//
// Tracked tags (open/close pairs):
//   <think>           / </think>
//   <tool_calls>      / </tool_calls>
//   <｜DSML｜tool_calls> / </｜DSML｜tool_calls>  (DeepSeek-specific)
//
// Also handles single-line [TOOL_CALL] prefixes.
//
// Design notes (from DS4 token_printer):
//   - pending_ accumulates bytes that MIGHT be the start of a tag
//   - On each process() call, pending_ + new chunk are scanned together
//   - Full tag matches toggle state (inThink_ / inToolCalls_)
//   - Partial prefix matches at end of buffer → held in pending_
//   - On isFinal: flush pending_ as literal text (no tag can span beyond EOS)
// ---------------------------------------------------------------------------

class ThinkTagFilter {
public:
    // -- Configuration -------------------------------------------------------

    void setFilterThinkTags(bool v) { filterThink_ = v; }
    void setFilterToolCalls(bool v) { filterTools_ = v; }

    // -- State query ---------------------------------------------------------

    bool inThink()     const { return inThink_; }
    bool inToolCalls() const { return inToolCalls_; }

    // -- Reset (e.g. between conversation turns) -----------------------------

    void reset() {
        inThink_     = false;
        inToolCalls_ = false;
        pending_.clear();
    }

    // -----------------------------------------------------------------------
    // process()
    //
    // Main entry point.  Accepts a new chunk of streaming text.
    // Returns the portion of text that should be forwarded to TTS.
    //
    // When isFinal is true, any remaining pending bytes are flushed as
    // literal text (the LLM won't send more data to complete a partial tag).
    // -----------------------------------------------------------------------

    std::string process(const std::string& chunk, bool isFinal = false) {
        // Concatenate pending buffer with the new chunk.
        // This mirrors the DS4 token_printer_process() which allocates
        // a temporary buffer = pending + new text, then scans linearly.
        std::string buf = pending_ + chunk;
        pending_.clear();

        std::string output;
        output.reserve(buf.size());

        std::size_t i = 0;
        const std::size_t total = buf.size();

        while (i < total) {
            const char* cur = buf.data() + i;
            const std::size_t rem = total - i;

            // ----- Check for opening tags -----------------------------------

            // <think>
            if (filterThink_ && !inThink_) {
                if (hasPrefix(cur, rem, kThinkOpen)) {
                    inThink_ = true;
                    i += kThinkOpen.size();
                    continue;
                }
            }

            // </think>
            if (filterThink_ && inThink_) {
                if (hasPrefix(cur, rem, kThinkClose)) {
                    inThink_ = false;
                    i += kThinkClose.size();
                    continue;
                }
            }

            // <｜DSML｜tool_calls>
            if (filterTools_ && !inToolCalls_) {
                if (hasPrefix(cur, rem, kDsmlToolOpen)) {
                    inToolCalls_ = true;
                    i += kDsmlToolOpen.size();
                    continue;
                }
            }

            // </｜DSML｜tool_calls>
            if (filterTools_ && inToolCalls_) {
                if (hasPrefix(cur, rem, kDsmlToolClose)) {
                    inToolCalls_ = false;
                    i += kDsmlToolClose.size();
                    continue;
                }
            }

            // <tool_calls>
            if (filterTools_ && !inToolCalls_) {
                if (hasPrefix(cur, rem, kToolCallsOpen)) {
                    inToolCalls_ = true;
                    i += kToolCallsOpen.size();
                    continue;
                }
            }

            // </tool_calls>
            if (filterTools_ && inToolCalls_) {
                if (hasPrefix(cur, rem, kToolCallsClose)) {
                    inToolCalls_ = false;
                    i += kToolCallsClose.size();
                    continue;
                }
            }

            // ----- Partial prefix detection ---------------------------------
            // If current position starts with '<' and we're NOT at the final
            // chunk, check whether the remaining bytes could be the beginning
            // of any tag we track.  If so, stash them in pending_ and stop.
            //
            // This is the C++ equivalent of DS4's:
            //   if (!finish && cur[0] == '<' &&
            //       (bytes_is_partial_prefix(cur, rem, think_open) ||
            //        bytes_is_partial_prefix(cur, rem, think_close)))
            //   { memcpy(p->pending, cur, rem); break; }

            if (!isFinal && cur[0] == '<') {
                if (isPartialPrefixOfAnyTag(cur, rem)) {
                    // Stash remaining bytes — they might complete a tag
                    // in the next chunk.
                    pending_.assign(cur, rem);
                    break;  // done with this chunk
                }
            }

            // ----- Emit character -------------------------------------------
            // Only emit if we are NOT inside a suppressed block.

            if (!inThink_ && !inToolCalls_) {
                output.push_back(buf[i]);
            }
            ++i;
        }

        return output;
    }

private:
    // -- Tag constants -------------------------------------------------------
    // Using std::string_view for zero-copy prefix matching.

    // Think tags
    static constexpr std::string_view kThinkOpen  = "<think>";
    static constexpr std::string_view kThinkClose = "</think>";

    // DSML tool-call tags (DeepSeek multi-lingual marker)
    // Note: ｜ is U+FF5C (fullwidth vertical bar), NOT ASCII |
    // String concatenation breaks the hex escape from the following 'D' char,
    // which would otherwise be consumed as part of the hex literal (\x9CD).
    static constexpr std::string_view kDsmlToolOpen  = "<\xEF\xBD\x9C" "DSML\xEF\xBD\x9C" "tool_calls>";
    static constexpr std::string_view kDsmlToolClose = "</\xEF\xBD\x9C" "DSML\xEF\xBD\x9C" "tool_calls>";

    // Plain tool-call tags
    static constexpr std::string_view kToolCallsOpen  = "<tool_calls>";
    static constexpr std::string_view kToolCallsClose = "</tool_calls>";

    // All tags we need to check for partial prefix matching.
    // Collected in an array so isPartialPrefixOfAnyTag() is concise.
    static constexpr std::string_view kAllTags[] = {
        kThinkOpen, kThinkClose,
        kDsmlToolOpen, kDsmlToolClose,
        kToolCallsOpen, kToolCallsClose,
    };

    // -- State ---------------------------------------------------------------

    bool inThink_     = false;
    bool inToolCalls_ = false;
    bool filterThink_ = true;
    bool filterTools_ = true;

    // Pending buffer: holds bytes that might be the start of a tag but
    // we don't have enough data yet to confirm.  Flushed on isFinal.
    std::string pending_;

    // -- Prefix matching helpers ---------------------------------------------
    // Direct C++ equivalents of DS4's bytes_has_prefix / bytes_is_partial_prefix.

    /// Returns true if the buffer starting at `p` with length `n` begins
    /// with the full `prefix` string.
    static bool hasPrefix(const char* p, std::size_t n, std::string_view prefix) {
        return n >= prefix.size()
            && std::memcmp(p, prefix.data(), prefix.size()) == 0;
    }

    /// Returns true if the buffer `p[0..n)` is a proper prefix of `tag`,
    /// i.e. n < tag.size() and the n bytes match the first n bytes of tag.
    /// This means we might be seeing the beginning of `tag` but we need
    /// more data to be sure.
    static bool isPartialPrefix(const char* p, std::size_t n, std::string_view tag) {
        return n < tag.size()
            && std::memcmp(p, tag.data(), n) == 0;
    }

    /// Returns true if the remaining bytes could be the start of ANY tracked tag.
    /// Also returns true if they fully match a tag (hasPrefix), because
    /// the main loop will handle full matches on the next iteration after
    /// we re-enter from pending_.
    bool isPartialPrefixOfAnyTag(const char* p, std::size_t n) const {
        for (auto tag : kAllTags) {
            // Only check tags whose filtering is enabled
            if (!shouldCheckTag(tag)) continue;
            if (isPartialPrefix(p, n, tag) || hasPrefix(p, n, tag)) {
                return true;
            }
        }
        return false;
    }

    /// Returns whether the given tag should be checked based on current config.
    bool shouldCheckTag(std::string_view tag) const {
        if (tag == kThinkOpen || tag == kThinkClose)
            return filterThink_;
        // All other tracked tags are tool-call related
        return filterTools_;
    }
};

// ---------------------------------------------------------------------------
// RealInterpreterNode
//
// Production interpreter node.  Sits between LLM output and TTS input.
//
// Processing pipeline per chunk:
//   1. Skip [TOOL_CALL] prefixed chunks entirely
//   2. Run ThinkTagFilter state machine (removes <think> and tool-call blocks)
//   3. Optionally convert LaTeX to spoken text
//   4. Forward non-empty result to TTS
//
// [lang=XX] markers are preserved as-is — they are consumed downstream by
// the TTS node to select the appropriate voice/language.
//
// Thread model: single thread via ActiveNodeBase, blocks on input queue pop().
// ---------------------------------------------------------------------------

class RealInterpreterNode final
    : public ActiveNodeBase
    , public IInterpreterNode
{
public:
    explicit RealInterpreterNode(InterpreterConfig config = {})
        : config_(std::move(config))
    {
        filter_.setFilterThinkTags(config_.filterThinkTags);
        filter_.setFilterToolCalls(config_.filterToolCalls);
    }

    const char* name()   const override { return "Interpreter"; }
    bool initialize()          override { return true; }

    void setInputQueue(TextQueue* q)  override { in_  = q; }
    void setOutputQueue(TextQueue* q) override { out_ = q; }

    // -- Runtime configuration (thread-safe only before start()) -------------

    const InterpreterConfig& config() const { return config_; }

    /// Reset the filter state machine.  Call between conversation turns if
    /// the LLM guarantees that tags don't span across turns.
    void resetFilterState() { filter_.reset(); }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk chunk;
            if (!in_ || !in_->pop(chunk)) break;

            // ----- Step 1: Skip [TOOL_CALL] prefixed chunks -----------------
            // Some LLMs emit tool calls as plain-text lines prefixed with
            // "[TOOL_CALL]".  These are never meant for TTS.
            if (config_.filterToolCalls && isToolCallLine(chunk.text)) {
                // If this is a final chunk, we still need to flush the filter
                // and forward a final marker so downstream knows the turn ended.
                if (chunk.isFinal) {
                    std::string flushed = filter_.process("", /*isFinal=*/true);
                    if (!flushed.empty() && out_) {
                        out_->push(TextChunk{
                            .text        = std::move(flushed),
                            .isFinal     = true,
                            .sequence    = outputSeq_++,
                            .timestampNs = chunk.timestampNs
                        });
                    } else if (out_) {
                        // Forward an empty final chunk so TTS knows the turn ended
                        out_->push(TextChunk{
                            .text        = "",
                            .isFinal     = true,
                            .sequence    = outputSeq_++,
                            .timestampNs = chunk.timestampNs
                        });
                    }
                }
                continue;
            }

            // ----- Step 2: Run the streaming tag filter ----------------------
            std::string filtered = filter_.process(chunk.text, chunk.isFinal);

            // ----- Step 3: LaTeX → spoken text (optional) -------------------
            if (config_.convertLatex && !filtered.empty()) {
                filtered = text_processing::latex_to_speech(filtered);
            }

            // ----- Step 4: Forward non-empty result to TTS ------------------
            // Always forward the final chunk even if empty, so TTS can detect
            // end-of-turn.  For non-final chunks, only forward if there's
            // actual text to speak.
            if (!filtered.empty() || chunk.isFinal) {
                if (out_) {
                    out_->push(TextChunk{
                        .text        = std::move(filtered),
                        .isFinal     = chunk.isFinal,
                        .sequence    = outputSeq_++,
                        .timestampNs = chunk.timestampNs
                    });
                }
            }
        }
    }

private:
    // -- [TOOL_CALL] detection -----------------------------------------------
    // Checks if a chunk starts with "[TOOL_CALL]" (case-sensitive).
    // This is a simple prefix check — the entire chunk is discarded.

    static bool isToolCallLine(const std::string& text) {
        constexpr std::string_view prefix = "[TOOL_CALL]";
        if (text.size() < prefix.size()) return false;
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    InterpreterConfig config_;
    ThinkTagFilter    filter_;
    TextQueue*        in_        = nullptr;
    TextQueue*        out_       = nullptr;
    uint64_t          outputSeq_ = 0;
};

} // namespace voice_runtime
