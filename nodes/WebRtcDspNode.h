#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

#ifndef VOICE_RUNTIME_ENABLE_WEBRTC_APM
#define VOICE_RUNTIME_ENABLE_WEBRTC_APM 0
#endif

#ifndef VOICE_RUNTIME_ENABLE_WEBRTC_VAD
#define VOICE_RUNTIME_ENABLE_WEBRTC_VAD VOICE_RUNTIME_ENABLE_WEBRTC_APM
#endif

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
#include "api/audio/audio_frame.h"
#include "api/scoped_refptr.h"
#include "modules/audio_processing/include/audio_processing.h"
#endif

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
#include "common_audio/vad/include/webrtc_vad.h"
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// WebRtcDspConfig
// ---------------------------------------------------------------------------

struct WebRtcDspConfig {
    AudioFormat format;

    bool enableEchoCancellation      = true;
    bool enableNoiseSuppression      = true;
    bool enableAgc1                  = false;
    bool enableAgc2                  = true;
    bool enableHighPassFilter        = true;
    bool enableVad                   = true;

    // Se true il nodo invia a valle solo frame classificati come speech.
    // Di default false: lo STT riceve tutto l'audio pulito e il VAD emette eventi.
    bool gateOutputWithVad           = false;

    // WebRTC VAD aggressiveness: 0 = quality, 3 = very aggressive.
    int vadMode                      = 2;
    float vadSpeechThreshold         = 0.5f;
    int vadHangoverMs                = 300;

    // Stima iniziale della latenza render->capture. Alcune versioni APM espongono
    // set_stream_delay_ms(), altre gestiscono internamente il delay estimator.
    int estimatedRenderDelayMs       = 40;

    // Pool locale per i frame clean prodotti dal nodo.
    std::size_t outputPoolSize       = 256;

    // Solo per build senza WebRTC: permette test di wiring come pass-through.
    // In produzione deve restare false.
    bool allowPassthroughWithoutWebRtc = false;
};

// ---------------------------------------------------------------------------
// WebRtcDspNode
//   - IAecDspNode: capture mic + render reference -> clean audio
//   - IVadNode: eventi SpeechStart/SpeechEnd e, opzionalmente, gating audio
// ---------------------------------------------------------------------------

class WebRtcDspNode final
    : public ActiveNodeBase
    , public IAecDspNode
    , public IVadNode
{
public:
    explicit WebRtcDspNode(WebRtcDspConfig config)
        : cfg_(std::move(config))
        , pool_(cfg_.outputPoolSize)
    {}

    const char* name() const override { return "WebRtcDSP"; }

    bool initialize() override {
        if (!validateFormat(cfg_.format)) return false;

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
        apm_ = webrtc::AudioProcessingBuilder().Create();
        if (!apm_) return false;

        webrtc::AudioProcessing::Config apmCfg;
        apmCfg.echo_canceller.enabled = cfg_.enableEchoCancellation;
        apmCfg.echo_canceller.mobile_mode = false;
        apmCfg.high_pass_filter.enabled = cfg_.enableHighPassFilter;
        apmCfg.noise_suppression.enabled = cfg_.enableNoiseSuppression;

        apmCfg.gain_controller1.enabled = cfg_.enableAgc1;
        apmCfg.gain_controller2.enabled = cfg_.enableAgc2;

        // Alcune versioni di libwebrtc espongono anche voice_detection nella Config.
        // Per evitare dipendenze fragili, il VAD del nodo usa WebRtcVad_Process.
        apm_->ApplyConfig(apmCfg);

#if defined(VOICE_RUNTIME_WEBRTC_HAS_SET_STREAM_DELAY_MS)
        apm_->set_stream_delay_ms(cfg_.estimatedRenderDelayMs);
#endif

        renderFrame_ = std::make_unique<webrtc::AudioFrame>();
        captureFrame_ = std::make_unique<webrtc::AudioFrame>();

        const int samples = cfg_.format.totalSamplesPerFrame();
        renderScratchI16_.resize(static_cast<std::size_t>(samples));
        captureScratchI16_.resize(static_cast<std::size_t>(samples));

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
        if (cfg_.enableVad) {
            vad_.reset(WebRtcVad_Create());
            if (!vad_) return false;
            if (WebRtcVad_Init(vad_.get()) != 0) return false;
            if (WebRtcVad_set_mode(vad_.get(), std::max(0, std::min(3, cfg_.vadMode))) != 0)
                return false;
        }
#endif
        initialized_ = true;
        return true;
#else
        initialized_ = cfg_.allowPassthroughWithoutWebRtc;
        return initialized_;
#endif
    }

    // IAecDspNode ------------------------------------------------------------

    void setCaptureInputQueue(AudioFrameQueue* q) override { micIn_ = q; }
    void setRenderInputQueue(AudioFrameQueue* q)  override { refIn_ = q; }
    void setOutputQueue(AudioFrameQueue* q)       override { out_   = q; }

    void setVadEventQueue(VadEventQueue* q) override { evq_ = q; }
    void setSpeechThreshold(float threshold) override {
        cfg_.vadSpeechThreshold = std::max(0.0f, std::min(1.0f, threshold));
    }
    bool isSpeaking() const override {
        return speaking_.load(std::memory_order_acquire);
    }

    // IVadNode ---------------------------------------------------------------
    // Usabile anche come VAD standalone: input=capture, output=clean/gated.

    void setInputQueue(AudioFrameQueue* in) override { micIn_ = in; }
    void setEventQueue(VadEventQueue* ev) override { evq_ = ev; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        if (!initialized_) return;

        while (running()) {
            drainRenderQueue();

            AudioFrameHandle mic;
            if (!micIn_ || !micIn_->pop(mic)) break;

            // Render arrivato tra pop capture e processing: lo consumiamo prima
            // del frame capture corrente per ridurre il delay percepito dall'AEC.
            drainRenderQueue();

            auto clean = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
            if (!clean) {
                if (!running()) break;
                continue;
            }

            const bool processed = processCaptureFrame(*mic, *clean);
            if (!processed) continue;

            clean->sequence = seq_++;
            clean->timestampNs = mic->timestampNs;

            const bool speech = evaluateVad(*clean);
            updateVadState(speech, clean->timestampNs);

            if (out_) {
                if (!cfg_.gateOutputWithVad || isSpeaking()) {
                    out_->push(std::move(clean));
                }
            }
        }
    }

private:
    static bool validateFormat(const AudioFormat& f) {
        if (f.channels <= 0 || f.channels > 2) return false;
        if (f.frameMs != 10 && f.frameMs != 20 && f.frameMs != 30) return false;
        switch (f.sampleRate) {
            case 8000:
            case 16000:
            case 32000:
            case 48000:
                return true;
            default:
                return false;
        }
    }

    void drainRenderQueue() {
        if (!refIn_) return;
        AudioFrameHandle ref;
        while (refIn_->tryPop(ref)) {
            processRenderFrame(*ref);
            ref.reset();
        }
    }

    bool processRenderFrame(const AudioFrame& frame) {
#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
        if (!apm_) return false;
        if (!copyToWebRtcFrame(frame, *renderFrame_, renderScratchI16_)) return false;
        return apm_->ProcessReverseStream(renderFrame_.get()) ==
               webrtc::AudioProcessing::kNoError;
#else
        (void)frame;
        return true;
#endif
    }

    bool processCaptureFrame(const AudioFrame& input, AudioFrame& output) {
        output.format = cfg_.format;
        output.resizeForFormat();

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
        if (!apm_) return false;
        if (!copyToWebRtcFrame(input, *captureFrame_, captureScratchI16_)) return false;

#if defined(VOICE_RUNTIME_WEBRTC_HAS_SET_STREAM_DELAY_MS)
        apm_->set_stream_delay_ms(cfg_.estimatedRenderDelayMs);
#endif

        const int rc = apm_->ProcessStream(captureFrame_.get());
        if (rc != webrtc::AudioProcessing::kNoError) return false;

        copyFromWebRtcFrame(*captureFrame_, output);
        return true;
#else
        if (!cfg_.allowPassthroughWithoutWebRtc) return false;
        output = input;
        output.format = cfg_.format;
        return true;
#endif
    }

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    bool copyToWebRtcFrame(const AudioFrame& src,
                           webrtc::AudioFrame& dst,
                           std::vector<int16_t>& scratch) {
        if (src.format.sampleRate != cfg_.format.sampleRate ||
            src.format.channels != cfg_.format.channels ||
            src.format.frameMs != cfg_.format.frameMs) {
            return false;
        }

        const int samples = src.format.totalSamplesPerFrame();
        const int samplesPerChannel = src.format.samplesPerChannelPerFrame();

        const int16_t* srcI16 = nullptr;
        if (src.format.sampleFormat == SampleFormat::Int16) {
            if (static_cast<int>(src.pcm16.size()) < samples) return false;
            srcI16 = src.pcm16.data();
        } else {
            if (static_cast<int>(src.pcmF32.size()) < samples) return false;
            if (static_cast<int>(scratch.size()) < samples)
                scratch.resize(static_cast<std::size_t>(samples));
            for (int i = 0; i < samples; ++i) {
                const float clamped = std::max(-1.0f, std::min(1.0f, src.pcmF32[static_cast<std::size_t>(i)]));
                scratch[static_cast<std::size_t>(i)] = static_cast<int16_t>(clamped * 32767.0f);
            }
            srcI16 = scratch.data();
        }

        dst.sample_rate_hz_ = src.format.sampleRate;
        dst.num_channels_ = static_cast<size_t>(src.format.channels);
        dst.samples_per_channel_ = static_cast<size_t>(samplesPerChannel);
        dst.timestamp_ = static_cast<uint32_t>(src.sequence & 0xFFFFFFFFu);
        std::memcpy(dst.mutable_data(), srcI16,
                    static_cast<std::size_t>(samples) * sizeof(int16_t));
        return true;
    }

    void copyFromWebRtcFrame(const webrtc::AudioFrame& src, AudioFrame& dst) {
        const int samples = dst.format.totalSamplesPerFrame();
        const int16_t* data = src.data();
        if (dst.format.sampleFormat == SampleFormat::Int16) {
            if (static_cast<int>(dst.pcm16.size()) < samples)
                dst.pcm16.resize(static_cast<std::size_t>(samples));
            std::memcpy(dst.pcm16.data(), data,
                        static_cast<std::size_t>(samples) * sizeof(int16_t));
        } else {
            if (static_cast<int>(dst.pcmF32.size()) < samples)
                dst.pcmF32.resize(static_cast<std::size_t>(samples));
            for (int i = 0; i < samples; ++i)
                dst.pcmF32[static_cast<std::size_t>(i)] = data[i] / 32768.0f;
        }
    }
#endif

    bool evaluateVad(const AudioFrame& frame) {
        if (!cfg_.enableVad) return true;

        const int samplesPerChannel = frame.format.samplesPerChannelPerFrame();
        const int totalSamples = frame.format.totalSamplesPerFrame();

        const int16_t* data = nullptr;
        if (frame.format.sampleFormat == SampleFormat::Int16) {
            if (static_cast<int>(frame.pcm16.size()) < totalSamples) return false;
            data = frame.pcm16.data();
        } else {
            if (static_cast<int>(frame.pcmF32.size()) < totalSamples) return false;
            if (vadScratchI16_.size() < static_cast<std::size_t>(totalSamples))
                vadScratchI16_.resize(static_cast<std::size_t>(totalSamples));
            for (int i = 0; i < totalSamples; ++i) {
                const float clamped = std::max(-1.0f, std::min(1.0f, frame.pcmF32[static_cast<std::size_t>(i)]));
                vadScratchI16_[static_cast<std::size_t>(i)] = static_cast<int16_t>(clamped * 32767.0f);
            }
            data = vadScratchI16_.data();
        }

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
        if (vad_) {
            const int vadResult = WebRtcVad_Process(
                vad_.get(), frame.format.sampleRate, data, samplesPerChannel);
            if (vadResult >= 0) return vadResult == 1;
        }
#endif
        return energyVad(data, totalSamples);
    }

    bool energyVad(const int16_t* data, int samples) const {
        if (!data || samples <= 0) return false;
        double acc = 0.0;
        for (int i = 0; i < samples; ++i) {
            const double v = static_cast<double>(data[i]) / 32768.0;
            acc += v * v;
        }
        const double rms = std::sqrt(acc / samples);
        return rms >= static_cast<double>(cfg_.vadSpeechThreshold) * 0.05;
    }

    void updateVadState(bool speechNow, uint64_t timestampNs) {
        if (!cfg_.enableVad) return;

        if (speechNow) {
            lastSpeechNs_ = timestampNs;
            if (!speaking_.exchange(true, std::memory_order_acq_rel)) {
                emitVadEvent(VadEvent::Type::SpeechStart, timestampNs, 1.0f);
            }
            return;
        }

        const bool wasSpeaking = speaking_.load(std::memory_order_acquire);
        if (!wasSpeaking) return;

        const uint64_t hangoverNs =
            static_cast<uint64_t>(std::max(0, cfg_.vadHangoverMs)) * 1000000ULL;
        if (timestampNs >= lastSpeechNs_ && (timestampNs - lastSpeechNs_) >= hangoverNs) {
            speaking_.store(false, std::memory_order_release);
            emitVadEvent(VadEvent::Type::SpeechEnd, timestampNs, 0.0f);
        }
    }

    void emitVadEvent(VadEvent::Type type, uint64_t timestampNs, float confidence) {
        if (!evq_) return;
        VadEvent ev;
        ev.type = type;
        ev.timestampNs = timestampNs;
        ev.confidence = confidence;
        evq_->push(ev);
    }

private:
    WebRtcDspConfig cfg_;

    AudioFrameQueue* micIn_ = nullptr;
    AudioFrameQueue* refIn_ = nullptr;
    AudioFrameQueue* out_ = nullptr;
    VadEventQueue* evq_ = nullptr;

    SharedBufferPool<AudioFrame> pool_;
    uint64_t seq_ = 0;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> speaking_{false};
    uint64_t lastSpeechNs_ = 0;

    std::vector<int16_t> renderScratchI16_;
    std::vector<int16_t> captureScratchI16_;
    std::vector<int16_t> vadScratchI16_;

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    rtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    std::unique_ptr<webrtc::AudioFrame> renderFrame_;
    std::unique_ptr<webrtc::AudioFrame> captureFrame_;
#endif

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
    struct VadDeleter {
        void operator()(VadInst* p) const noexcept {
            if (p) WebRtcVad_Free(p);
        }
    };
    std::unique_ptr<VadInst, VadDeleter> vad_;
#endif
};

} // namespace voice_runtime
