#pragma once

// ---------------------------------------------------------------------------
// AntirezSttNode.h
//
// Speech-to-Text node using antirez/qwen-asr for the voice_runtime pipeline.
//
// Accumulates audio frames into a contiguous int16 buffer. Transcription
// is triggered when:
//   1. No frame received for `transcriptionTimeoutMs` (timed pop timeout)
//   2. Buffer reaches 80% of `maxSpeechSamples` (forced flush)
//   3. Minimum buffer size is `minSpeechSamples` (skip tiny segments)
//
// When compiled without qwen_asr.h the node operates in stub mode,
// emitting "[STT stub: N samples received]" text chunks.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

// ---------------------------------------------------------------------------
// Feature detection — qwen-asr availability
// ---------------------------------------------------------------------------

#if __has_include(<qwen_asr.h>)
extern "C" {
#  include <qwen_asr.h>
}
#  define VOICE_RUNTIME_HAS_QWEN_ASR 1
#elif __has_include("qwen_asr.h")
extern "C" {
#  include "qwen_asr.h"
}
#  define VOICE_RUNTIME_HAS_QWEN_ASR 1
#else
#  define VOICE_RUNTIME_HAS_QWEN_ASR 0
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct AntirezSttConfig {
    std::string modelPath;

    /// Timeout in ms: if no frame arrives for this long, trigger transcription
    int transcriptionTimeoutMs = 500;

    /// Minimum samples before transcription is attempted (1s = 16000 samples)
    int minSpeechSamples = 16000;

    /// Maximum samples before forced transcription (30s @ 16kHz)
    int maxSpeechSamples = 480000;
};

// ---------------------------------------------------------------------------
// AntirezSttNode
// ---------------------------------------------------------------------------

class AntirezSttNode final
    : public ActiveNodeBase
    , public ISpeechToTextNode
{
public:
    explicit AntirezSttNode(AntirezSttConfig config)
        : config_(std::move(config))
    {}

    ~AntirezSttNode() override {
#if VOICE_RUNTIME_HAS_QWEN_ASR
        if (ctx_) {
            qwen_free(ctx_);
            ctx_ = nullptr;
        }
#endif
    }

    const char* name() const override { return "AntirezSTT"; }

    // ---- ISpeechToTextNode --------------------------------------------------

    void setInputQueue(AudioFrameQueue* q)  override { in_  = q; }
    void setOutputQueue(TextQueue* q)       override { out_ = q; }

    // ---- IActiveNode --------------------------------------------------------

    bool initialize() override {
#if VOICE_RUNTIME_HAS_QWEN_ASR
        if (config_.modelPath.empty()) {
            std::cout << "[AntirezSTT] No model path — running in stub mode\n";
            stubMode_ = true;
            return true;
        }
        
        ctx_ = qwen_load(config_.modelPath.c_str());
        if (!ctx_) {
            std::cerr << "[AntirezSTT] Failed to load model directory: " << config_.modelPath << "\n";
            std::cerr << "[AntirezSTT] Falling back to stub mode\n";
            stubMode_ = true;
            return true;
        }
        
        std::cout << "[AntirezSTT] Model loaded successfully from (" << config_.modelPath << ")\n";
        stubMode_ = false;
        return true;
#else
        std::cout << "[AntirezSTT] Stub mode — qwen_asr.h not available\n";
        stubMode_ = true;
        return true;
#endif
    }

protected:
    // ---- ActiveNodeBase -----------------------------------------------------

    void runLoop() override {
        const auto timeout =
            std::chrono::milliseconds(config_.transcriptionTimeoutMs);
        int frameCount = 0;
        warnedTooSmall_ = false;

        while (running()) {
            AudioFrameHandle frame;
            auto tStart = std::chrono::steady_clock::now();
            const bool got = in_ && in_->popWithTimeout(frame, timeout);
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tStart).count();

            if (got) {
                appendFrame(*frame);
                warnedTooSmall_ = false;

                // Diagnostic print every 50 frames
                if (++frameCount % 50 == 0) {
                    std::cout << "[AntirezSTT] Speech buffer: " << speechBuffer_.size()
                              << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s)\n" << std::flush;
                }

                // Forced transcription when buffer is ≥ 80% full
                if (static_cast<int>(speechBuffer_.size()) >=
                    static_cast<int>(config_.maxSpeechSamples * 0.8)) {
                    std::cout << "[AntirezSTT] Buffer 80% full, forcing transcription...\n" << std::flush;
                    doTranscribe();
                    frameCount = 0;
                }
            } else {
                if (!running()) break;

                if (elapsedMs < 10) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                if (static_cast<int>(speechBuffer_.size()) >=
                    config_.minSpeechSamples) {
                    std::cout << "[AntirezSTT] Silence timeout: transcribing " << speechBuffer_.size()
                              << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s)...\n" << std::flush;
                    doTranscribe();
                    frameCount = 0;
                    warnedTooSmall_ = false;
                } else if (!speechBuffer_.empty() && !warnedTooSmall_) {
                    std::cout << "[AntirezSTT] Silence timeout: buffer too small to transcribe (" << speechBuffer_.size()
                              << " samples, " << static_cast<double>(speechBuffer_.size()) / 16000.0
                              << "s < min " << static_cast<double>(config_.minSpeechSamples) / 16000.0 << "s), keeping buffer...\n" << std::flush;
                    warnedTooSmall_ = true;
                }
            }
        }

        // Flush residual buffer on shutdown
        if (static_cast<int>(speechBuffer_.size()) >=
            config_.minSpeechSamples) {
            std::cout << "[AntirezSTT] Shutdown: flushing " << speechBuffer_.size() << " samples...\n" << std::flush;
            doTranscribe();
        } else if (!speechBuffer_.empty()) {
            std::cout << "[AntirezSTT] Shutdown: discarding remaining " << speechBuffer_.size() << " samples (too small to transcribe)\n" << std::flush;
        }
    }

private:
    // ---- Audio accumulation -------------------------------------------------

    void appendFrame(const AudioFrame& frame) {
        if (frame.format.sampleFormat != SampleFormat::Int16) {
            return;
        }
        speechBuffer_.insert(speechBuffer_.end(),
                             frame.pcm16.begin(), frame.pcm16.end());
    }

    // ---- Transcription ------------------------------------------------------

    void doTranscribe() {
        if (speechBuffer_.empty()) return;

        std::string text = transcribeBuffer();
        speechBuffer_.clear();

        if (text.empty()) return;

        // Emit result
        if (out_) {
            TextChunk chunk;
            chunk.text        = std::move(text);
            chunk.isFinal     = true;
            chunk.sequence    = ++outSeq_;
            chunk.timestampNs = nowNs();
            out_->push(std::move(chunk));
        }
    }

    std::string transcribeBuffer() {
        static constexpr std::size_t kAbsoluteMinSamples = 8000;
        if (speechBuffer_.size() < kAbsoluteMinSamples) {
            std::cout << "[AntirezSTT] Skipping too-short buffer: " << speechBuffer_.size()
                      << " samples (min " << kAbsoluteMinSamples << ")\n" << std::flush;
            return {};
        }

#if VOICE_RUNTIME_HAS_QWEN_ASR
        if (!stubMode_ && ctx_) {
            // Convert int16 → float [-1, 1]
            std::vector<float> fSamples(speechBuffer_.size());
            constexpr float kScale = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < speechBuffer_.size(); ++i) {
                fSamples[i] = static_cast<float>(speechBuffer_[i]) * kScale;
            }

            std::cout << "[AntirezSTT] Transcribing " << speechBuffer_.size()
                      << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s) with qwen-asr C engine...\n" << std::flush;

            // Transcribe using raw audio samples
            char* res = qwen_transcribe_audio(ctx_, fSamples.data(), static_cast<int>(fSamples.size()));
            if (res) {
                std::string text(res);
                free(res);
                return text;
            }
            std::cerr << "[AntirezSTT] Transcription returned empty or null\n";
            return {};
        }
#endif
        // Stub mode
        return "[STT stub: " + std::to_string(speechBuffer_.size()) +
               " samples received]";
    }

    // ---- Utility ------------------------------------------------------------

    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
    }

    // ---- Members ------------------------------------------------------------

    AntirezSttConfig        config_;
    AudioFrameQueue*        in_  = nullptr;
    TextQueue*              out_ = nullptr;
    uint64_t                outSeq_ = 0;
    bool                    stubMode_ = false;
    bool                    warnedTooSmall_ = false;

    /// Contiguous accumulation buffer for the current utterance
    std::vector<int16_t>    speechBuffer_;

#if VOICE_RUNTIME_HAS_QWEN_ASR
    qwen_ctx_t*             ctx_ = nullptr;
#endif
};

} // namespace voice_runtime
