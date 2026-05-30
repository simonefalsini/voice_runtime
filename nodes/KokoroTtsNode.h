#pragma once

// ---------------------------------------------------------------------------
// KokoroTtsNode.h
//
// Text-to-Speech node using Kokoro ONNX for the voice_runtime pipeline.
//
// Receives TextChunk items, optionally parses [lang=XX] markers,
// performs G2P via espeak-ng, runs Kokoro inference, and pushes:
//   • 24 kHz Int16 frames to the speaker output queue
//   • 16 kHz Int16 frames (downsampled) to the AEC reference queue
//
// Sets TtsStateSignal active during synthesis, checks InterruptSignal
// between frames to support barge-in.
//
// When compiled without kokoro.h the node produces a sine-wave stub
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
// Feature detection — Kokoro
// ---------------------------------------------------------------------------

#if __has_include(<Kokoro.h>)
#  include <Kokoro.h>
#  define VOICE_RUNTIME_HAS_KOKORO 1
#elif __has_include("Kokoro.h")
#  include "Kokoro.h"
#  define VOICE_RUNTIME_HAS_KOKORO 1
#elif __has_include(<kokoro.h>)
#  include <kokoro.h>
#  define VOICE_RUNTIME_HAS_KOKORO 1
#else
#  define VOICE_RUNTIME_HAS_KOKORO 0
#endif

// ---------------------------------------------------------------------------
// Feature detection — espeak-ng (G2P)
// ---------------------------------------------------------------------------

#if __has_include(<espeak-ng/speak_lib.h>)
#  include <espeak-ng/speak_lib.h>
#  define VOICE_RUNTIME_HAS_ESPEAK 1
#elif __has_include("espeak-ng/speak_lib.h")
#  include "espeak-ng/speak_lib.h"
#  define VOICE_RUNTIME_HAS_ESPEAK 1
#else
#  define VOICE_RUNTIME_HAS_ESPEAK 0
#endif

// TextProcessing utilities for phoneme cleaning (utf8/wstring, clean_phonemes)
#include "TextProcessing.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct KokoroTtsConfig {
    std::string modelPath;
    std::string voicesPath     = "models/tts/voices.bin";
    std::string dictDir        = "models/tts/dict";
    std::string vocabPath      = "models/tts/dict/vocab.txt";
    std::string espeakDataPath = "models/espeak-ng-data";

    /// Speaker output format — native Kokoro sample rate
    AudioFormat speakerFormat  = {24000, 1, 10, SampleFormat::Int16};
    /// AEC reference format — downsampled to match microphone
    AudioFormat aecRefFormat   = {16000, 1, 10, SampleFormat::Int16};

    std::size_t speakerPoolSize = 256;
    std::size_t aecRefPoolSize  = 256;

    std::string defaultVoice    = "af_bella";
    std::string defaultLanguage = "en";
    float       speed           = 1.0f;
};

// ---------------------------------------------------------------------------
// KokoroTtsNode
// ---------------------------------------------------------------------------

class KokoroTtsNode final
    : public ActiveNodeBase
    , public ITextToSpeechNode
{
public:
    explicit KokoroTtsNode(KokoroTtsConfig config)
        : config_(std::move(config))
        , speakerPool_(config_.speakerPoolSize)
        , aecRefPool_(config_.aecRefPoolSize)
    {
        buildVoiceTable();
    }

    const char* name() const override { return "KokoroTTS"; }

    // ---- ITextToSpeechNode --------------------------------------------------

    void setInputQueue(TextQueue* q)                       override { textIn_    = q; }
    void setSpeakerOutputQueue(AudioFrameQueue* q)         override { spkOut_    = q; }
    void setAecReferenceOutputQueue(AudioFrameQueue* q)    override { aecRefOut_ = q; }
    void setInterruptSignal(InterruptSignal* s)            override { intSig_    = s; }
    void setTtsStateSignal(TtsStateSignal* s)              override { ttsState_  = s; }

    // ---- IActiveNode --------------------------------------------------------

    bool initialize() override {
#if VOICE_RUNTIME_HAS_KOKORO
        if (config_.modelPath.empty()) {
            std::printf("[KokoroTTS] No model path — running in stub mode\n");
            stubMode_ = true;
        } else {
            // Pre-check required files to avoid cppjieba FATAL/abort on missing dicts
            bool filesOk = true;
            auto checkFile = [&filesOk](const std::string& path, const char* desc) {
                if (path.empty()) return;
                std::FILE* f = std::fopen(path.c_str(), "rb");
                if (!f) {
                    std::fprintf(stderr,
                        "[KokoroTTS] Missing %s: %s\n", desc, path.c_str());
                    filesOk = false;
                } else {
                    std::fclose(f);
                }
            };
            checkFile(config_.modelPath,  "model");
            checkFile(config_.voicesPath, "voices");
            checkFile(config_.vocabPath,  "vocab");
            // Check jieba dict (required by cppjieba, aborts if missing)
            const std::string jiebaDict = config_.dictDir + "/jieba.dict.utf8";
            checkFile(jiebaDict, "jieba dict");

            if (!filesOk) {
                std::fprintf(stderr,
                    "[KokoroTTS] Required files missing — falling back to stub mode\n");
                stubMode_ = true;
            } else {
                try {
                    kokoro_ = std::make_unique<Kokoro>(config_.modelPath,
                                                       config_.voicesPath,
                                                       config_.dictDir,
                                                       config_.vocabPath);
                    std::printf("[KokoroTTS] Model loaded (%s)\n",
                                config_.modelPath.c_str());
                    stubMode_ = false;
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                        "[KokoroTTS] Failed to load model: %s — falling back to stub mode\n",
                        e.what());
                    kokoro_.reset();
                    stubMode_ = true;
                }
            }
        }
#else
        std::printf("[KokoroTTS] Stub mode — kokoro.h not available\n");
        stubMode_ = true;
#endif

#if VOICE_RUNTIME_HAS_ESPEAK
        if (!stubMode_) {
            const int sr = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                                             config_.espeakDataPath.c_str(), 0);
            if (sr < 0) {
                std::fprintf(stderr, "[KokoroTTS] espeak-ng init failed\n");
                espeakReady_ = false;
            } else {
                espeakReady_ = true;
                std::printf("[KokoroTTS] espeak-ng initialized (data: %s)\n",
                            config_.espeakDataPath.c_str());
            }
        }
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

            // Resolve voice/espeak from language
            std::string voiceName, espeakVoice;
            resolveVoice(lang, voiceName, espeakVoice);

            // Synthesize
            std::vector<float> audio;
            int sampleRate = 24000;
            synthesize(text, lang, voiceName, espeakVoice, audio, sampleRate);

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

            // If this is the final sentence of the paragraph, wait for the speaker to drain,
            // then mark TTS inactive.
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
    // ---- Voice table (ported from DS4 TTSPipeline) --------------------------

    struct VoiceEntry {
        std::string voiceName;
        std::string espeakVoice;
        bool        useEspeak;  // false for zh
    };

    void buildVoiceTable() {
        voiceTable_["en"] = {"af_bella",  "en-us", true};
        voiceTable_["it"] = {"if_sara",   "it",    true};
        voiceTable_["es"] = {"ef_dora",   "es",    true};
        voiceTable_["fr"] = {"ff_siwis",  "fr",    true};
        voiceTable_["de"] = {"de_martin", "de",    true};
        voiceTable_["zh"] = {"af_bella",  "",      false};
        voiceTable_["ja"] = {"jf_alpha",  "ja",    true};
        voiceTable_["pt"] = {"pf_dora",   "pt",    true};
        voiceTable_["hi"] = {"hf_alpha",  "hi",    true};
    }

    void resolveVoice(const std::string& lang,
                      std::string& voiceName,
                      std::string& espeakVoice) const {
        auto it = voiceTable_.find(lang);
        if (it != voiceTable_.end()) {
            voiceName   = it->second.voiceName;
            espeakVoice = it->second.espeakVoice;
        } else {
            voiceName   = config_.defaultVoice;
            espeakVoice = "en-us";
        }
    }

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

    // ---- Synthesis ----------------------------------------------------------

    void synthesize(const std::string& text,
                    const std::string& lang,
                    const std::string& voiceName,
                    const std::string& espeakVoice,
                    std::vector<float>& outAudio,
                    int& outSampleRate) {
        auto tStart = std::chrono::steady_clock::now();
        double g2pMs = 0.0;
        double onnxMs = 0.0;

#if VOICE_RUNTIME_HAS_KOKORO
        if (!stubMode_ && kokoro_) {
            std::string textToSynth = text;
            bool isPhonemes = false;

            // G2P via espeak (skip for Chinese)
            auto it = voiceTable_.find(lang);
            const bool useEspeak = (it != voiceTable_.end()) ? it->second.useEspeak : true;

#if VOICE_RUNTIME_HAS_ESPEAK
            if (espeakReady_ && useEspeak && !espeakVoice.empty()) {
                auto tG2pStart = std::chrono::steady_clock::now();
                std::string phonemes = textToPhonemes(text, espeakVoice);
                if (!phonemes.empty()) {
                    preserveTerminalPunctuation(text, phonemes);
                    textToSynth = phonemes;
                    isPhonemes  = true;
                }
                auto tG2pEnd = std::chrono::steady_clock::now();
                g2pMs = std::chrono::duration<double, std::milli>(tG2pEnd - tG2pStart).count();
            }
#endif
            (void)espeakVoice;

            auto tOnnxStart = std::chrono::steady_clock::now();
            auto result = kokoro_->create(textToSynth, voiceName,
                                          config_.speed, isPhonemes, true);
            auto tOnnxEnd = std::chrono::steady_clock::now();
            onnxMs = std::chrono::duration<double, std::milli>(tOnnxEnd - tOnnxStart).count();

            outAudio      = std::move(result.first);
            outSampleRate = result.second;
            if (outSampleRate <= 0) outSampleRate = 24000;

            double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
            std::printf("  \033[90m[TTS Timing]\033[0m Sentence: \"%s\" (%zu chars) | G2P: %.1f ms | ONNX: %.1f ms | Total Synth: %.1f ms\n",
                        text.c_str(), text.length(), g2pMs, onnxMs, totalMs);
            std::fflush(stdout);
            return;
        }
#endif
        // Stub: generate a 440Hz sine wave
        (void)lang; (void)voiceName; (void)espeakVoice;
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
        std::printf("[KokoroTTS] Stub synthesized %d samples for \"%s\"\n",
                    totalSamples, text.c_str());
    }

    // ---- G2P via espeak-ng --------------------------------------------------

#if VOICE_RUNTIME_HAS_ESPEAK
    std::string textToPhonemes(const std::string& text,
                               const std::string& espeakVoice) {
        if (espeak_SetVoiceByName(espeakVoice.c_str()) != EE_OK) {
            std::fprintf(stderr,
                         "[KokoroTTS] espeak: failed to set voice %s\n",
                         espeakVoice.c_str());
            return {};
        }

        std::string allPhonemes;
        const void* textPtr = text.c_str();
        constexpr int kPhonemeMode = 0x02; // espeakPHONEMES_IPA

        while (textPtr && *static_cast<const char*>(textPtr)) {
            const char* ph = espeak_TextToPhonemes(
                &textPtr, espeakCHARS_UTF8, kPhonemeMode);
            if (ph) {
                if (!allPhonemes.empty()) allPhonemes += " ";
                allPhonemes += ph;
            } else {
                break;
            }
        }

        if (allPhonemes.empty()) return {};

        // Clean phonemes for Kokoro (reuse TextProcessing utilities)
        auto wph = text_processing::utf8_to_wstring(allPhonemes);
        auto cleaned = text_processing::clean_phonemes_cpp(wph);
        return text_processing::wstring_to_utf8(cleaned);
    }
#endif

    static void preserveTerminalPunctuation(const std::string& text,
                                            std::string& phonemes) {
        auto textLast = text.find_last_not_of(" \t\r\n");
        if (textLast == std::string::npos || phonemes.empty()) return;

        const char terminal = text[textLast];
        if (terminal != '.' && terminal != '?' && terminal != '!' &&
            terminal != ';' && terminal != ',') {
            return;
        }

        auto phonemeLast = phonemes.find_last_not_of(" \t\r\n");
        if (phonemeLast != std::string::npos) {
            const char existing = phonemes[phonemeLast];
            if (existing == '.' || existing == '?' || existing == '!' ||
                existing == ';' || existing == ',') {
                return;
            }
        }

        phonemes.push_back(terminal);
    }

    // ---- Frame segmentation & output ----------------------------------------

    /// Segment synthesized audio into 10ms frames and push to both queues.
    /// Returns true if interrupted.
    bool pushFrames(const std::vector<float>& audio, int sampleRate) {
        // Speaker frames: native rate (typically 24kHz)
        const int spkSamplesPerFrame = config_.speakerFormat.samplesPerChannelPerFrame();
        // AEC ref frames: downsampled rate (typically 16kHz)
        const int refSamplesPerFrame = config_.aecRefFormat.samplesPerChannelPerFrame();

        // Downsample full audio to 16kHz for AEC reference
        std::vector<int16_t> audio16k;
        if (aecRefOut_) {
            audio16k = downsample(audio, sampleRate,
                                  config_.aecRefFormat.sampleRate);
        }

        // Convert speaker audio to the configured output rate before framing.
        // A mislabeled sample rate sounds like recognizable but badly distorted speech.
        const std::vector<float> audioSpkF32 =
            resampleFloat(audio, sampleRate, config_.speakerFormat.sampleRate);

        std::vector<int16_t> audioSpk(audioSpkF32.size());
        for (std::size_t i = 0; i < audioSpkF32.size(); ++i) {
            float clamped = std::max(-1.0f, std::min(1.0f, audioSpkF32[i]));
            audioSpk[i] = static_cast<int16_t>(clamped * 32767.0f);
        }

        // Push speaker frames (round up to avoid discarding the final samples)
        const std::size_t totalSpkFrames =
            (audioSpk.size() + spkSamplesPerFrame - 1) / static_cast<std::size_t>(spkSamplesPerFrame);
        const std::size_t totalRefFrames =
            audio16k.empty() ? 0 :
            (audio16k.size() + refSamplesPerFrame - 1) / static_cast<std::size_t>(refSamplesPerFrame);

        std::size_t spkIdx = 0;
        for (std::size_t f = 0; f < totalSpkFrames; ++f) {
            // Check interrupt between frames
            if (intSig_ && intSig_->check()) {
                intSig_->clear();
                std::printf("[KokoroTTS] Interrupted after %zu/%zu frames\n",
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

                // Push AEC reference first (timing requirement)
                if (aecRefOut_ && !audio16k.empty()) {
                    // Determine corresponding ref frame index
                    // Ratio: for every speaker frame we advance proportionally
                    // in the ref buffer.
                    const std::size_t refFrameIdx =
                        f * totalRefFrames / totalSpkFrames;
                    const std::size_t refStart =
                        refFrameIdx * static_cast<std::size_t>(refSamplesPerFrame);

                    auto refFrame = aecRefPool_.acquireWithTimeout(
                        std::chrono::milliseconds(50));
                    if (refFrame) {
                        refFrame->format      = config_.aecRefFormat;
                        refFrame->resizeForFormat();
                        refFrame->sequence    = refSeq_++;
                        refFrame->timestampNs = frame->timestampNs;

                        const std::size_t refCount =
                            static_cast<std::size_t>(refSamplesPerFrame);
                        const std::size_t refAvail =
                            std::min(refCount, audio16k.size() - refStart);
                        std::memcpy(refFrame->pcm16.data(),
                                    audio16k.data() + refStart,
                                    refAvail * sizeof(int16_t));
                        if (refAvail < refCount) {
                            std::memset(refFrame->pcm16.data() + refAvail, 0, (refCount - refAvail) * sizeof(int16_t));
                        }

                        aecRefOut_->push(std::move(refFrame));
                    }
                }

                if (spkOut_) spkOut_->push(std::move(frame));
            }
        }

        return false;
    }

    // ---- Simple linear-interpolation downsampler ----------------------------
    //  Converts from srcRate to dstRate (e.g. 24kHz → 16kHz, ratio 2:3).
    //  No anti-alias filter — acceptable for the AEC reference path.

    static std::vector<int16_t> downsample(const std::vector<float>& src,
                                            int srcRate, int dstRate) {
        if (srcRate <= 0 || dstRate <= 0 || src.empty()) return {};
        if (srcRate == dstRate) {
            // No conversion needed — just quantize
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

    // ---- Utility ------------------------------------------------------------

    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
    }

    // ---- Members ------------------------------------------------------------

    KokoroTtsConfig         config_;
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

    std::unordered_map<std::string, VoiceEntry> voiceTable_;

#if VOICE_RUNTIME_HAS_KOKORO
    std::unique_ptr<Kokoro> kokoro_;
#endif

#if VOICE_RUNTIME_HAS_ESPEAK
    bool espeakReady_ = false;
#endif
};

} // namespace voice_runtime
