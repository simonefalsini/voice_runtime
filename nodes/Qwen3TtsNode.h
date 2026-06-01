#pragma once

// ---------------------------------------------------------------------------
// Qwen3TtsNode.h
//
// Text-to-Speech node using Qwen3 TTS for the voice_runtime pipeline.
//
// Receives TextChunk items, optionally parses [lang=XX] markers,
// runs Qwen3-TTS inference, and pushes:
//   • 24 kHz Int16 frames to the speaker output queue
//   • 16 kHz Int16 frames (downsampled) to the AEC reference queue
//
// Sets TtsStateSignal active during synthesis, checks InterruptSignal
// between frames to support barge-in.
//
// Synchronizes GGML operations via g_ggml_mutex.
//
// When compiled without qwen3_tts.h the node produces a sine-wave stub
// for testing the pipeline without the real TTS engine.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

// ---------------------------------------------------------------------------
// Feature detection — Qwen3-TTS
// ---------------------------------------------------------------------------

#if __has_include(<qwen3_tts.h>)
#  include <qwen3_tts.h>
#  define VOICE_RUNTIME_HAS_QWEN3_TTS 1
#elif __has_include("qwen3-tts/qwen3_tts.h")
#  include "qwen3-tts/qwen3_tts.h"
#  define VOICE_RUNTIME_HAS_QWEN3_TTS 1
#else
#  define VOICE_RUNTIME_HAS_QWEN3_TTS 0
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Qwen3TtsConfig {
    std::string modelDir       = "models"; // Directory containing GGUF files
    std::string voicePath      = "";       // Optional reference WAV file for voice cloning

    /// Speaker output format — native Qwen3 TTS sample rate
    AudioFormat speakerFormat  = {24000, 1, 10, SampleFormat::Int16};
    /// AEC reference format — downsampled to match microphone
    AudioFormat aecRefFormat   = {16000, 1, 10, SampleFormat::Int16};

    std::size_t speakerPoolSize = 256;
    std::size_t aecRefPoolSize  = 256;

    std::string defaultLanguage = "en";
    
    // Sampling parameters
    float       temperature = 0.5f; // Default to 0.5f to avoid greedy repetition loop and ensure speed
    int32_t     top_k = 50;
    float       top_p = 1.0f;
    int32_t     nThreads = 4;
};

// ---------------------------------------------------------------------------
// Qwen3TtsNode
// ---------------------------------------------------------------------------

class Qwen3TtsNode final
    : public ActiveNodeBase
    , public ITextToSpeechNode
{
public:
    explicit Qwen3TtsNode(Qwen3TtsConfig config)
        : config_(std::move(config))
        , speakerPool_(config_.speakerPoolSize)
        , aecRefPool_(config_.aecRefPoolSize)
    {
    }

    const char* name() const override { return "Qwen3TTS"; }

    // ---- ITextToSpeechNode --------------------------------------------------

    void setInputQueue(TextQueue* q)                       override { textIn_    = q; }
    void setSpeakerOutputQueue(AudioFrameQueue* q)         override { spkOut_    = q; }
    void setAecReferenceOutputQueue(AudioFrameQueue* q)    override { aecRefOut_ = q; }
    void setInterruptSignal(InterruptSignal* s)            override { intSig_    = s; }
    void setTtsStateSignal(TtsStateSignal* s)              override { ttsState_  = s; }

    // ---- IActiveNode --------------------------------------------------------

    bool initialize() override {
#if VOICE_RUNTIME_HAS_QWEN3_TTS
        if (config_.modelDir.empty()) {
            std::printf("[Qwen3TTS] No model directory — running in stub mode\n");
            stubMode_ = true;
        } else {
            // Check if GGUF files exist in model directory
            std::string ttsModel = config_.modelDir + "/qwen3-tts-0.6b-f16.gguf";
            std::string tokModel = config_.modelDir + "/qwen3-tts-tokenizer-f16.gguf";
            
            std::FILE* f1 = std::fopen(ttsModel.c_str(), "rb");
            std::FILE* f2 = std::fopen(tokModel.c_str(), "rb");
            bool filesOk = f1 && f2;
            if (f1) std::fclose(f1);
            if (f2) std::fclose(f2);

            if (!filesOk) {
                std::fprintf(stderr,
                    "[Qwen3TTS] Required GGUF files missing from %s — falling back to stub mode\n",
                    config_.modelDir.c_str());
                stubMode_ = true;
            } else {
                try {
                    tts_ = std::make_unique<qwen3_tts::Qwen3TTS>();
                    std::unique_lock<std::mutex> lock(g_ggml_mutex);
                    if (!tts_->load_models(config_.modelDir)) {
                        std::fprintf(stderr, "[Qwen3TTS] load_models failed: %s — falling back to stub mode\n",
                                     tts_->get_error().c_str());
                        tts_.reset();
                        stubMode_ = true;
                    } else {
                        std::printf("[Qwen3TTS] Model loaded successfully from (%s)\n",
                                    config_.modelDir.c_str());
                        stubMode_ = false;

                        // Load reference voice for cloning (use config voice path, or fallback to default_voice.wav or readme_clone_input.wav)
                        std::string voicePath = config_.voicePath;
                        if (voicePath.empty()) {
                            std::string defaultPath1 = config_.modelDir + "/tts/default_voice.wav";
                            std::string defaultPath2 = "deps/qwen3-tts.cpp/examples/readme_clone_input.wav";
                            std::string defaultPath3 = "models/tts/default_voice.wav";
                            std::FILE* ft1 = std::fopen(defaultPath1.c_str(), "rb");
                            std::FILE* ft2 = std::fopen(defaultPath2.c_str(), "rb");
                            std::FILE* ft3 = std::fopen(defaultPath3.c_str(), "rb");
                            if (ft1) {
                                voicePath = defaultPath1;
                                std::fclose(ft1);
                            } else if (ft2) {
                                voicePath = defaultPath2;
                                std::fclose(ft2);
                            } else if (ft3) {
                                voicePath = defaultPath3;
                                std::fclose(ft3);
                            }
                            if (ft3 && !ft1 && !ft2) std::fclose(ft3);
                        }

                        if (!voicePath.empty()) {
                            std::printf("[Qwen3TTS] Loading reference voice: %s\n", voicePath.c_str());
                            speakerEmbedding_.clear();
                            if (!tts_->extract_speaker_embedding(voicePath, speakerEmbedding_)) {
                                std::fprintf(stderr, "[Qwen3TTS] Failed to extract speaker embedding from %s: %s\n",
                                             voicePath.c_str(), tts_->get_error().c_str());
                                speakerEmbedding_.clear();
                            } else {
                                std::printf("[Qwen3TTS] Speaker embedding successfully extracted (%zu floats)\n",
                                            speakerEmbedding_.size());
                            }
                        } else {
                            std::printf("[Qwen3TTS] WARNING: No reference voice WAV found — voice characteristics may vary stochastically\n");
                        }
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                        "[Qwen3TTS] Failed to load model: %s — falling back to stub mode\n",
                        e.what());
                    tts_.reset();
                    stubMode_ = true;
                }
            }
        }
#else
        std::printf("[Qwen3TTS] Stub mode — qwen3_tts.h not available\n");
        stubMode_ = true;
#endif
        return true;
    }

protected:
    void wake() override {
        speakerPool_.stop();
        aecRefPool_.stop();
    }

    void runLoop() override {
        while (running()) {
            TextChunk chunk;
            if (!textIn_ || !textIn_->pop(chunk)) break;

            // Parse optional [lang=XX] marker
            std::string text = chunk.text;
            std::string lang = config_.defaultLanguage;
            parseLangMarker(text, lang);

            if (text.empty()) continue;

            // Resolve language ID for codec
            int32_t languageId = mapLanguageToId(lang);

            // Synthesize
            std::vector<float> audio;
            int sampleRate = 24000;
            synthesize(text, languageId, audio, sampleRate);

            if (audio.empty()) continue;

            // Normalize audio to 0.95 peak amplitude to ensure healthy ERL for AEC
            float maxVal = 0.0f;
            for (float v : audio) {
                maxVal = std::max(maxVal, std::abs(v));
            }
            if (maxVal > 0.0001f) {
                float scale = 0.95f / maxVal;
                for (float& v : audio) {
                    v *= scale;
                }
            }
            
            // Clear stale frames ONLY if we were completely inactive or interrupted
            const bool wasInactive = !ttsState_ || !ttsState_->isActive();
            if (wasInactive || (intSig_ && intSig_->check())) {
                if (aecRefOut_) aecRefOut_->clear();
                if (spkOut_) spkOut_->clear();
            }

            // Mark TTS active
            if (ttsState_) ttsState_->setActive(true);

            // Segment and push frames
            const bool interrupted = pushFrames(audio, sampleRate);

            if (interrupted) {
                if (textIn_) textIn_->clear();
                if (ttsState_) ttsState_->setActive(false);
                continue;
            }

            // If this is the final sentence, wait for the speaker to drain, then mark inactive
            if (chunk.isFinal) {
                while (running() && spkOut_ && spkOut_->size() > 0) {
                    // Check interrupt while waiting
                    if (intSig_ && intSig_->check()) {
                        intSig_->clear();
                        if (aecRefOut_) aecRefOut_->clear();
                        if (spkOut_) spkOut_->clear();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (ttsState_) ttsState_->setActive(false);
            }
        }
    }

private:
    // ---- [lang=XX] marker parsing -------------------------------------------

    static void parseLangMarker(std::string& text, std::string& lang) {
        // Trim leading whitespace
        auto pos = text.find_first_not_of(" \t\r\n");
        if (pos != std::string::npos && pos > 0) {
            text = text.substr(pos);
        }

        if (text.size() >= 8 && text[0] == '[') {
            auto end = text.find(']');
            if (end != std::string::npos && end <= 10) {
                std::string marker = text.substr(1, end - 1);
                if (marker.compare(0, 5, "lang=") == 0) {
                    lang = marker.substr(5);
                    text = text.substr(end + 1);
                    // Strip leading space after marker
                    if (!text.empty() && text[0] == ' ') {
                        text = text.substr(1);
                    }
                }
            }
        }
    }

    int32_t mapLanguageToId(const std::string& lang) {
        // 2050=en, 2069=ru, 2055=zh, 2058=ja, 2064=ko, 2053=de, 2061=fr, 2054=es
        if (lang == "en") return 2050;
        if (lang == "ru") return 2069;
        if (lang == "zh") return 2055;
        if (lang == "ja") return 2058;
        if (lang == "ko") return 2064;
        if (lang == "de") return 2053;
        if (lang == "fr") return 2061;
        if (lang == "es") return 2054;
        if (lang == "it") return 2054; // Fallback Italian to Spanish/English since Qwen3 might not have native Italian
        return 2050;
    }

    // ---- Synthesis ----------------------------------------------------------

    void synthesize(const std::string& text,
                    int32_t languageId,
                    std::vector<float>& outAudio,
                    int& outSampleRate) {
        auto tStart = std::chrono::steady_clock::now();

#if VOICE_RUNTIME_HAS_QWEN3_TTS
        if (!stubMode_ && tts_) {
            qwen3_tts::tts_params params;
            params.temperature = config_.temperature;
            params.top_k = config_.top_k;
            params.top_p = config_.top_p;
            params.n_threads = config_.nThreads;
            params.print_progress = false;
            params.print_timing = false;
            params.language_id = languageId;

            qwen3_tts::tts_result result;
            {
                std::unique_lock<std::mutex> lock(g_ggml_mutex);
                if (!speakerEmbedding_.empty()) {
                    result = tts_->synthesize_with_embedding(text, speakerEmbedding_, params);
                } else {
                    result = tts_->synthesize(text, params);
                }
            }

            if (result.success) {
                outAudio      = std::move(result.audio);
                outSampleRate = result.sample_rate;
                if (outSampleRate <= 0) outSampleRate = 24000;

                double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
                std::printf("  \033[90m[Qwen3TTS Timing]\033[0m Sentence: \"%s\" (%zu chars) | Total Synth: %.1f ms\n",
                            text.c_str(), text.length(), totalMs);
                std::fflush(stdout);
                return;
            } else {
                std::fprintf(stderr, "[Qwen3TTS] Synthesis failed: %s\n", result.error_msg.c_str());
            }
        }
#endif
        // Stub: generate a 440Hz sine wave
        outSampleRate = 24000;
        const int numSamples = static_cast<int>(
            0.02f * static_cast<float>(text.size()) *
            static_cast<float>(outSampleRate));
        const int totalSamples = std::max(outSampleRate / 2, numSamples);
        outAudio.resize(static_cast<std::size_t>(totalSamples));
        constexpr float kFreq = 440.0f;
        constexpr float kAmplitude = 0.3f;
        for (int i = 0; i < totalSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(outSampleRate);
            outAudio[static_cast<std::size_t>(i)] =
                kAmplitude * std::sin(2.0f * 3.14159265f * kFreq * t);
        }
        std::printf("[Qwen3TTS] Stub synthesized %d samples for \"%s\"\n",
                    totalSamples, text.c_str());
    }

    // ---- Frame segmentation & output ----------------------------------------

    bool pushFrames(const std::vector<float>& audio, int sampleRate) {
        const int spkSamplesPerFrame = config_.speakerFormat.samplesPerChannelPerFrame();
        const int refSamplesPerFrame = config_.aecRefFormat.samplesPerChannelPerFrame();

        // Downsample full audio to 16kHz for AEC reference
        std::vector<int16_t> audio16k;
        if (aecRefOut_) {
            audio16k = downsample(audio, sampleRate,
                                  config_.aecRefFormat.sampleRate);
        }

        // Convert speaker audio to the configured output rate before framing.
        const std::vector<float> audioSpkF32 =
            resampleFloat(audio, sampleRate, config_.speakerFormat.sampleRate);

        std::vector<int16_t> audioSpk(audioSpkF32.size());
        for (std::size_t i = 0; i < audioSpkF32.size(); ++i) {
            float clamped = std::max(-1.0f, std::min(1.0f, audioSpkF32[i]));
            audioSpk[i] = static_cast<int16_t>(clamped * 32767.0f);
        }

        // Push speaker frames
        const std::size_t totalSpkFrames =
            (audioSpk.size() + spkSamplesPerFrame - 1) / static_cast<std::size_t>(spkSamplesPerFrame);

        std::size_t spkIdx = 0;
        std::size_t refIdx = 0;
        for (std::size_t f = 0; f < totalSpkFrames; ++f) {
            // Check interrupt between frames
            if (intSig_ && intSig_->check()) {
                intSig_->clear();
                std::printf("[Qwen3TTS] Interrupted after %zu/%zu frames\n",
                            f, totalSpkFrames);
                if (aecRefOut_) aecRefOut_->clear();
                if (spkOut_) spkOut_->clear();
                return true;
            }
            if (!running()) return true;

            // Speaker frame
            {
                auto frame = speakerPool_.acquireWithTimeout(
                    std::chrono::milliseconds(50));
                if (!frame) {
                    if (!running()) return true;
                    --f; // retry
                    continue;
                }

                frame->format      = config_.speakerFormat;
                frame->resizeForFormat();
                frame->sequence    = spkSeq_++;
                frame->timestampNs = nowNs();

                const std::size_t count = static_cast<std::size_t>(spkSamplesPerFrame);
                const std::size_t avail = std::min(count,
                    audioSpk.size() - spkIdx);
                std::memcpy(frame->pcm16.data(),
                            audioSpk.data() + spkIdx,
                            avail * sizeof(int16_t));
                if (avail < count) {
                    std::memset(frame->pcm16.data() + avail, 0, (count - avail) * sizeof(int16_t));
                }
                spkIdx += count;

                // Push AEC reference first
                if (aecRefOut_ && !audio16k.empty()) {
                    auto refFrame = aecRefPool_.acquireWithTimeout(
                        std::chrono::milliseconds(50));
                    if (refFrame) {
                        refFrame->format      = config_.aecRefFormat;
                        refFrame->resizeForFormat();
                        refFrame->sequence    = refSeq_++;
                        refFrame->timestampNs = frame->timestampNs;

                        const std::size_t refCount =
                            static_cast<std::size_t>(refSamplesPerFrame);
                        
                        if (refIdx < audio16k.size()) {
                            const std::size_t refAvail =
                                std::min(refCount, audio16k.size() - refIdx);
                            std::memcpy(refFrame->pcm16.data(),
                                        audio16k.data() + refIdx,
                                        refAvail * sizeof(int16_t));
                            if (refAvail < refCount) {
                                std::memset(refFrame->pcm16.data() + refAvail, 0, (refCount - refAvail) * sizeof(int16_t));
                            }
                            refIdx += refCount;
                        } else {
                            std::memset(refFrame->pcm16.data(), 0, refCount * sizeof(int16_t));
                        }

                        aecRefOut_->push(std::move(refFrame));
                    }
                }

                if (spkOut_) spkOut_->push(std::move(frame));
            }
        }

        return false;
    }

    // ---- Downsamplers copied from KokoroTtsNode -----------------------------

    static std::vector<int16_t> downsample(const std::vector<float>& src,
                                            int srcRate, int dstRate) {
        if (srcRate <= 0 || dstRate <= 0 || src.empty()) return {};
        if (srcRate == dstRate) {
            std::vector<int16_t> out(src.size());
            for (std::size_t i = 0; i < src.size(); ++i) {
                float c = std::max(-1.0f, std::min(1.0f, src[i]));
                out[i] = static_cast<int16_t>(c * 32767.0f);
            }
            return out;
        }

        const double ratio = static_cast<double>(srcRate) /
                             static_cast<double>(dstRate);
        const std::size_t outLen = static_cast<std::size_t>(
            static_cast<double>(src.size()) / ratio);
        std::vector<int16_t> out(outLen);

        for (std::size_t i = 0; i < outLen; ++i) {
            const double srcPos = static_cast<double>(i) * ratio;
            const std::size_t idx0 = static_cast<std::size_t>(srcPos);
            const std::size_t idx1 = std::min(idx0 + 1, src.size() - 1);
            const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));

            const float sample = src[idx0] * (1.0f - frac) + src[idx1] * frac;
            const float clamped = std::max(-1.0f, std::min(1.0f, sample));
            out[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
        return out;
    }

    static std::vector<float> resampleFloat(const std::vector<float>& src,
                                            int srcRate, int dstRate) {
        if (srcRate <= 0 || dstRate <= 0 || src.empty()) return {};
        if (srcRate == dstRate) return src;

        const double ratio = static_cast<double>(srcRate) /
                             static_cast<double>(dstRate);
        const std::size_t outLen = static_cast<std::size_t>(
            static_cast<double>(src.size()) / ratio);
        std::vector<float> out(outLen);

        for (std::size_t i = 0; i < outLen; ++i) {
            const double srcPos = static_cast<double>(i) * ratio;
            const std::size_t idx0 = static_cast<std::size_t>(srcPos);
            const std::size_t idx1 = std::min(idx0 + 1, src.size() - 1);
            const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));
            out[i] = src[idx0] * (1.0f - frac) + src[idx1] * frac;
        }
        return out;
    }

    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
    }

    // ---- Members ------------------------------------------------------------

    Qwen3TtsConfig          config_;
    TextQueue*              textIn_    = nullptr;
    AudioFrameQueue*        spkOut_    = nullptr;
    AudioFrameQueue*        aecRefOut_ = nullptr;
    InterruptSignal*        intSig_    = nullptr;
    TtsStateSignal*         ttsState_  = nullptr;

    SharedBufferPool<AudioFrame> speakerPool_;
    SharedBufferPool<AudioFrame> aecRefPool_;

    uint64_t                spkSeq_ = 0;
    uint64_t                refSeq_ = 0;
    bool                    stubMode_ = false;

    std::vector<float>      speakerEmbedding_;

#if VOICE_RUNTIME_HAS_QWEN3_TTS
    std::unique_ptr<qwen3_tts::Qwen3TTS> tts_;
#endif
};

} // namespace voice_runtime
