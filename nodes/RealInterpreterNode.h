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
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "TextProcessing.h"
#include "LanguageDetector.h"

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
    enum class PrintState {
        None,
        Think,
        Tool,
        Response
    };

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
        printState_  = PrintState::None;
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
                    if (printState_ != PrintState::None) {
                        std::printf("]\033[0m");
                    }
                    printState_ = PrintState::Think;
                    std::printf("\033[90m[think: ");
                    std::fflush(stdout);
                    continue;
                }
            }

            // </think>
            if (filterThink_ && inThink_) {
                if (hasPrefix(cur, rem, kThinkClose)) {
                    inThink_ = false;
                    i += kThinkClose.size();
                    std::printf("]\033[0m");
                    printState_ = PrintState::None;
                    std::fflush(stdout);
                    continue;
                }
            }

            // <｜DSML｜tool_calls>
            if (filterTools_ && !inToolCalls_) {
                if (hasPrefix(cur, rem, kDsmlToolOpen)) {
                    inToolCalls_ = true;
                    i += kDsmlToolOpen.size();
                    if (printState_ != PrintState::None) {
                        std::printf("]\033[0m");
                    }
                    printState_ = PrintState::Tool;
                    std::printf("\033[33m[tool: ");
                    std::fflush(stdout);
                    continue;
                }
            }

            // </｜DSML｜tool_calls>
            if (filterTools_ && inToolCalls_) {
                if (hasPrefix(cur, rem, kDsmlToolClose)) {
                    inToolCalls_ = false;
                    i += kDsmlToolClose.size();
                    std::printf("]\033[0m");
                    printState_ = PrintState::None;
                    std::fflush(stdout);
                    continue;
                }
            }

            // <tool_calls>
            if (filterTools_ && !inToolCalls_) {
                if (hasPrefix(cur, rem, kToolCallsOpen)) {
                    inToolCalls_ = true;
                    i += kToolCallsOpen.size();
                    if (printState_ != PrintState::None) {
                        std::printf("]\033[0m");
                    }
                    printState_ = PrintState::Tool;
                    std::printf("\033[33m[tool: ");
                    std::fflush(stdout);
                    continue;
                }
            }

            // </tool_calls>
            if (filterTools_ && inToolCalls_) {
                if (hasPrefix(cur, rem, kToolCallsClose)) {
                    inToolCalls_ = false;
                    i += kToolCallsClose.size();
                    std::printf("]\033[0m");
                    printState_ = PrintState::None;
                    std::fflush(stdout);
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

            if (inThink_) {
                if (printState_ != PrintState::Think) {
                    if (printState_ != PrintState::None) std::printf("]\033[0m");
                    printState_ = PrintState::Think;
                    std::printf("\033[90m[think: ");
                }
                std::printf("%c", buf[i]);
            } else if (inToolCalls_) {
                if (printState_ != PrintState::Tool) {
                    if (printState_ != PrintState::None) std::printf("]\033[0m");
                    printState_ = PrintState::Tool;
                    std::printf("\033[33m[tool: ");
                }
                std::printf("%c", buf[i]);
            } else {
                if (printState_ != PrintState::Response) {
                    if (printState_ != PrintState::None) std::printf("]\033[0m");
                    printState_ = PrintState::Response;
                    std::printf("\033[36m[risposta: ");
                }
                std::printf("%c", buf[i]);
                output.push_back(buf[i]);
            }
            std::fflush(stdout);
            ++i;
        }

        if (isFinal) {
            if (printState_ != PrintState::None) {
                std::printf("]\033[0m");
                printState_ = PrintState::None;
            }
            std::printf("\n");
            std::fflush(stdout);
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
    PrintState printState_ = PrintState::None;

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
    void resetFilterState() {
        filter_.reset();
        sentenceBuffer_.clear();
    }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk chunk;
            if (!in_ || !in_->pop(chunk)) break;

            // ----- Step 1: Skip [TOOL_CALL] prefixed chunks -----------------
            if (config_.filterToolCalls && isToolCallLine(chunk.text)) {
                if (chunk.isFinal) {
                    std::string flushed = filter_.process("", /*isFinal=*/true);
                    if (!flushed.empty()) {
                        sentenceBuffer_ += flushed;
                    }
                    flushRemainingSentences(chunk.timestampNs, /*forceFinal=*/true);
                }
                continue;
            }

            // ----- Step 2: Run the streaming tag filter ----------------------
            std::string filtered = filter_.process(chunk.text, chunk.isFinal);

            // ----- Step 3: LaTeX → spoken text (optional) -------------------
            if (config_.convertLatex && !filtered.empty()) {
                filtered = text_processing::latex_to_speech(filtered);
            }

            if (!filtered.empty()) {
                sentenceBuffer_ += filtered;
            }

            // ----- Step 4: Handle final chunk (split and flush all) ----------
            if (chunk.isFinal) {
                flushRemainingSentences(chunk.timestampNs, true);
                filter_.reset();
            }
        }
    }

private:
    void flushRemainingSentences(uint64_t timestampNs, bool forceFinal) {
        // We buffer the entire response and split it into sentences only at the end.
        // This ensures TTS receives complete sentences with punctuation and never starves.
        
        std::vector<std::string> sentences;
        std::string current;
        
        for (std::size_t i = 0; i < sentenceBuffer_.size(); ++i) {
            char c = sentenceBuffer_[i];
            current.push_back(c);
            if (c == '.' || c == '?' || c == '!' || c == '\n') {
                auto first = current.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    auto last = current.find_last_not_of(" \t\r\n");
                    std::string trimmed = current.substr(first, (last - first + 1));
                    if (!trimmed.empty()) {
                        sentences.push_back(std::move(trimmed));
                    }
                }
                current.clear();
            }
        }
        
        if (!current.empty()) {
            auto first = current.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                auto last = current.find_last_not_of(" \t\r\n");
                std::string trimmed = current.substr(first, (last - first + 1));
                if (!trimmed.empty()) {
                    // Append punctuation if missing
                    char lastChar = trimmed.back();
                    if (lastChar != '.' && lastChar != '?' && lastChar != '!') {
                        trimmed += ".";
                    }
                    sentences.push_back(std::move(trimmed));
                }
            }
        }
        
        sentenceBuffer_.clear();

        if (sentences.empty()) {
            // Push an empty final chunk so downstream knows it's over
            if (out_ && forceFinal) {
                out_->push(TextChunk{
                    .text        = "",
                    .isFinal     = true,
                    .sequence    = outputSeq_++,
                    .timestampNs = timestampNs
                });
            }
            return;
        }

        std::size_t seq = 1;
        for (const auto& sentence : sentences) {
            std::string lang = config_.preferredLanguage;
            if (config_.detectLanguage) {
                lang = detector_.detect(sentence, config_.preferredLanguage);
            }
            std::string prefixed = "[lang=" + lang + "] " + sentence;

            if (out_) {
                out_->push(TextChunk{
                    .text        = std::move(prefixed),
                    .isFinal     = (forceFinal && seq == sentences.size()),
                    .sequence    = outputSeq_++,
                    .timestampNs = timestampNs
                });
            }
            seq++;
        }
    }


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
    std::string       sentenceBuffer_;
    LanguageDetector  detector_;
};

} // namespace voice_runtime
