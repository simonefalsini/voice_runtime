#pragma once

// ---------------------------------------------------------------------------
// Qwen3SttNode.h
//
// Speech-to-Text node using Qwen3-ASR for the voice_runtime pipeline.
//
// Accumulates audio frames into a contiguous int16 buffer.  Transcription
// is triggered when:
//   1. No frame received for `transcriptionTimeoutMs` (timed pop timeout)
//   2. Buffer reaches 80% of `maxSpeechSamples` (forced flush)
//   3. Minimum buffer size is `minSpeechSamples` (skip tiny segments)
//
// When compiled without qwen3_asr.h the node operates in stub mode,
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
// Feature detection — Qwen3-ASR availability
// ---------------------------------------------------------------------------

#if __has_include(<qwen3_asr.h>)
#  include <qwen3_asr.h>
#  define VOICE_RUNTIME_HAS_QWEN3_ASR 1
#elif __has_include("qwen3_asr.h")
#  include "qwen3_asr.h"
#  define VOICE_RUNTIME_HAS_QWEN3_ASR 1
#else
#  define VOICE_RUNTIME_HAS_QWEN3_ASR 0
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Qwen3SttConfig {
    std::string modelPath;
    std::string mmprojPath;

    /// Timeout in ms: if no frame arrives for this long, trigger transcription
    int transcriptionTimeoutMs = 500;

    /// Minimum samples before transcription is attempted.
    /// The Qwen3-ASR encoder requires enough mel frames for 3 cascaded
    /// conv2d layers (stride 2 each).  1 second (16000 samples) is the
    /// safe minimum; shorter buffers can crash ggml_conv_2d.
    int minSpeechSamples = 16000;

    /// Maximum samples before forced transcription (30s @ 16kHz)
    int maxSpeechSamples = 480000;

    /// Number of CPU threads for the ASR inference
    int nThreads = 4;

    /// Strip "language XYZ" prefix emitted by Qwen3 ASR
    bool stripLanguagePrefix = true;
};

// ---------------------------------------------------------------------------
// Qwen3SttNode
// ---------------------------------------------------------------------------

class Qwen3SttNode final
    : public ActiveNodeBase
    , public ISpeechToTextNode
{
public:
    explicit Qwen3SttNode(Qwen3SttConfig config)
        : config_(std::move(config))
    {}

    const char* name() const override { return "Qwen3STT"; }

    // ---- ISpeechToTextNode --------------------------------------------------

    void setInputQueue(AudioFrameQueue* q)  override { in_  = q; }
    void setOutputQueue(TextQueue* q)       override { out_ = q; }

    // ---- IActiveNode --------------------------------------------------------

    bool initialize() override {
#if VOICE_RUNTIME_HAS_QWEN3_ASR
        if (config_.modelPath.empty()) {
            std::cout << "[Qwen3STT] No model path — running in stub mode\n";
            stubMode_ = true;
            return true;
        }
        asr_ = std::make_unique<qwen3_asr::Qwen3ASR>();
        if (!asr_->load_model(config_.modelPath, config_.mmprojPath)) {
            std::cerr << "[Qwen3STT] Failed to load model: " << asr_->get_error() << "\n";
            std::cerr << "[Qwen3STT] Falling back to stub mode\n";
            asr_.reset();
            stubMode_ = true;
            return true;
        }
        std::cout << "[Qwen3STT] Model loaded (" << config_.modelPath << ")\n";
        stubMode_ = false;
        return true;
#else
        std::cout << "[Qwen3STT] Stub mode — qwen3_asr.h not available\n";
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
            const bool got = in_ && in_->popWithTimeout(frame, timeout);

            if (got) {
                appendFrame(*frame);
                warnedTooSmall_ = false;

                // Diagnostic print every 50 frames (500 ms of active speech)
                if (++frameCount % 50 == 0) {
                    std::cout << "[Qwen3STT] Speech buffer: " << speechBuffer_.size()
                              << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s)\n" << std::flush;
                }

                // Forced transcription when buffer is ≥ 80% full
                if (static_cast<int>(speechBuffer_.size()) >=
                    static_cast<int>(config_.maxSpeechSamples * 0.8)) {
                    std::cout << "[Qwen3STT] Buffer 80% full, forcing transcription...\n" << std::flush;
                    doTranscribe();
                    frameCount = 0;
                }
            } else {
                // Timeout (no frame) — treat as end-of-utterance
                if (!running()) break;

                if (static_cast<int>(speechBuffer_.size()) >=
                    config_.minSpeechSamples) {
                    std::cout << "[Qwen3STT] Silence timeout: transcribing " << speechBuffer_.size()
                              << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s)...\n" << std::flush;
                    doTranscribe();
                    frameCount = 0;
                    warnedTooSmall_ = false;
                } else if (!speechBuffer_.empty() && !warnedTooSmall_) {
                    std::cout << "[Qwen3STT] Silence timeout: buffer too small to transcribe (" << speechBuffer_.size()
                              << " samples, " << static_cast<double>(speechBuffer_.size()) / 16000.0
                              << "s < min " << static_cast<double>(config_.minSpeechSamples) / 16000.0 << "s), keeping buffer...\n" << std::flush;
                    warnedTooSmall_ = true;
                }
            }
        }

        // Flush residual buffer on shutdown
        if (static_cast<int>(speechBuffer_.size()) >=
            config_.minSpeechSamples) {
            std::cout << "[Qwen3STT] Shutdown: flushing " << speechBuffer_.size() << " samples...\n" << std::flush;
            doTranscribe();
        } else if (!speechBuffer_.empty()) {
            std::cout << "[Qwen3STT] Shutdown: discarding remaining " << speechBuffer_.size() << " samples (too small to transcribe)\n" << std::flush;
        }
    }

private:
    // ---- Audio accumulation -------------------------------------------------

    void appendFrame(const AudioFrame& frame) {
        if (frame.format.sampleFormat != SampleFormat::Int16) {
            // Only Int16 supported for now
            return;
        }
        speechBuffer_.insert(speechBuffer_.end(),
                             frame.pcm16.begin(), frame.pcm16.end());
    }

    // ---- Transcription ------------------------------------------------------

    void doTranscribe() {
        if (speechBuffer_.empty()) return;

        const std::size_t numSamples = speechBuffer_.size();
        std::string text = transcribeBuffer();
        speechBuffer_.clear();

        if (text.empty()) return;

        if (config_.stripLanguagePrefix) {
            text = stripLangPrefix(text);
        }

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

        (void)numSamples; // may be used for logging later
    }

    std::string transcribeBuffer() {
        // Hard minimum: the ASR encoder needs at least ~0.5s of audio
        // to produce enough mel frames for the conv2d layers.
        // Use 8000 samples (0.5s @ 16kHz) as absolute floor.
        static constexpr std::size_t kAbsoluteMinSamples = 8000;
        if (speechBuffer_.size() < kAbsoluteMinSamples) {
            std::cout << "[Qwen3STT] Skipping too-short buffer: " << speechBuffer_.size()
                      << " samples (min " << kAbsoluteMinSamples << ")\n" << std::flush;
            return {};
        }

#if VOICE_RUNTIME_HAS_QWEN3_ASR
        if (!stubMode_ && asr_) {
            // Convert int16 → float [-1, 1] as expected by Qwen3ASR
            std::vector<float> fSamples(speechBuffer_.size());
            constexpr float kScale = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < speechBuffer_.size(); ++i) {
                fSamples[i] = static_cast<float>(speechBuffer_[i]) * kScale;
            }

            std::cout << "[Qwen3STT] Transcribing " << speechBuffer_.size()
                      << " samples (" << static_cast<double>(speechBuffer_.size()) / 16000.0 << "s)...\n" << std::flush;

            qwen3_asr::transcribe_params params;
            params.n_threads      = config_.nThreads;
            params.print_progress = false;
            params.print_timing   = false;

            std::unique_lock<std::mutex> lock(g_ggml_mutex);
            auto result = asr_->transcribe(fSamples.data(),
                                           static_cast<int>(fSamples.size()),
                                           params);
            if (result.success && !result.text.empty()) {
                return result.text;
            }
            if (!result.success) {
                std::cerr << "[Qwen3STT] Transcription failed: " << result.error_msg << "\n";
            }
            return {};
        }
#endif
        // Stub mode
        return "[STT stub: " + std::to_string(speechBuffer_.size()) +
               " samples received]";
    }

    // ---- Language prefix stripping (ported from DS4) -------------------------

    static std::string stripLangPrefix(std::string text) {
        static const char* kLangs[] = {
            "Italian", "English", "Spanish", "French", "German",
            "Chinese", "Portuguese", "Japanese", "Hindi", "None"
        };

        if (text.compare(0, 9, "language ") == 0) {
            for (const char* lang : kLangs) {
                const std::string prefix = std::string("language ") + lang;
                if (text.compare(0, prefix.size(), prefix) == 0) {
                    text = text.substr(prefix.size());
                    // Trim leading whitespace
                    const auto pos = text.find_first_not_of(" \t");
                    if (pos != std::string::npos && pos > 0) {
                        text = text.substr(pos);
                    }
                    break;
                }
            }
        }
        return text;
    }

    // ---- Utility ------------------------------------------------------------

    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
    }

    // ---- Members ------------------------------------------------------------

    Qwen3SttConfig          config_;
    AudioFrameQueue*        in_  = nullptr;
    TextQueue*              out_ = nullptr;
    uint64_t                outSeq_ = 0;
    bool                    stubMode_ = false;
    bool                    warnedTooSmall_ = false;

    /// Contiguous accumulation buffer for the current utterance
    std::vector<int16_t>    speechBuffer_;

#if VOICE_RUNTIME_HAS_QWEN3_ASR
    std::unique_ptr<qwen3_asr::Qwen3ASR> asr_;
#endif
};

} // namespace voice_runtime
