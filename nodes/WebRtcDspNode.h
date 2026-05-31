#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
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
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
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

  bool enableEchoCancellation = true;
  bool enableNoiseSuppression = true;
  bool enableAgc1 = false;
  bool enableAgc2 = true;
  bool enableHighPassFilter = true;
  bool enableVad = true;

  // Se true il nodo invia a valle solo frame classificati come speech.
  // Di default false: lo STT riceve tutto l'audio pulito e il VAD emette
  // eventi.
  bool gateOutputWithVad = false;

  // WebRTC VAD aggressiveness: 0 = quality, 3 = very aggressive.
  int vadMode = 2;
  float vadSpeechThreshold = 0.5f;
  int vadHangoverMs = 300;

  // Stima iniziale della latenza render->capture. Alcune versioni APM espongono
  // set_stream_delay_ms(), altre gestiscono internamente il delay estimator.
  int estimatedRenderDelayMs = 80;

  // Pool locale per i frame clean prodotti dal nodo.
  std::size_t outputPoolSize = 256;

  // Grace period (ms) after TTS activation during which processed frames are
  // discarded to let the AEC model stabilize on the new echo reference.
  int aecWarmupGracePeriodMs = 200;

  // Solo per build senza WebRTC: permette test di wiring come pass-through.
  // In produzione deve restare false.
  bool allowPassthroughWithoutWebRtc = false;
};

// ---------------------------------------------------------------------------
// WebRtcDspNode
//   - IAecDspNode: capture mic + render reference -> clean audio
//   - Internal VAD: emits SpeechStart/SpeechEnd events, optional gating
//   - TtsStateSignal-driven: pass-through when TTS inactive, AEC when active
// ---------------------------------------------------------------------------

class WebRtcDspNode final : public virtual ActiveNodeBase,
                            public virtual IAecDspNode,
                            public virtual IVadNode {
public:
  explicit WebRtcDspNode(WebRtcDspConfig config)
      : cfg_(std::move(config)), pool_(cfg_.outputPoolSize) {}

  const char *name() const override { return "WebRtcDSP"; }

  bool initialize() override {
    if (!validateFormat(cfg_.format))
      return false;

    // Compute warm-up frame count from config
    warmupFramesRequired_ =
        cfg_.format.frameMs > 0
            ? (cfg_.aecWarmupGracePeriodMs / cfg_.format.frameMs)
            : 20;

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    webrtc::AudioProcessing::Config apmCfg;
    apmCfg.echo_canceller.enabled = cfg_.enableEchoCancellation;
    apmCfg.high_pass_filter.enabled = cfg_.enableHighPassFilter;
    apmCfg.noise_suppression.enabled = cfg_.enableNoiseSuppression;

    apmCfg.gain_controller1.enabled = cfg_.enableAgc1;
    apmCfg.gain_controller2.enabled = cfg_.enableAgc2;

    apm_ = webrtc::BuiltinAudioProcessingBuilder(apmCfg).Build(
        webrtc::CreateEnvironment());
    if (!apm_)
      return false;

    apm_->set_stream_delay_ms(cfg_.estimatedRenderDelayMs);

    const int samples = cfg_.format.totalSamplesPerFrame();
    renderScratchI16_.resize(static_cast<std::size_t>(samples));
    captureScratchI16_.resize(static_cast<std::size_t>(samples));
    renderDestI16_.resize(static_cast<std::size_t>(samples));
    captureDestI16_.resize(static_cast<std::size_t>(samples));

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
    if (cfg_.enableVad) {
      vad_.reset(WebRtcVad_Create());
      if (!vad_)
        return false;
      if (WebRtcVad_Init(vad_.get()) != 0)
        return false;
      if (WebRtcVad_set_mode(vad_.get(),
                             std::max(0, std::min(3, cfg_.vadMode))) != 0)
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

  // IAecDspNode & IVadNode --------------------------------------------------

  void setCaptureInputQueue(AudioFrameQueue *q) override { micIn_ = q; }
  void setRenderInputQueue(AudioFrameQueue *q) override { refIn_ = q; }
  void setOutputQueue(AudioFrameQueue *q) override { out_ = q; }

  void setVadEventQueue(VadEventQueue *q) override { evq_ = q; }
  void setSpeechThreshold(float threshold) override {
    cfg_.vadSpeechThreshold = std::max(0.0f, std::min(1.0f, threshold));
  }
  bool isSpeaking() const override {
    return speaking_.load(std::memory_order_acquire);
  }
  void setTtsStateSignal(const TtsStateSignal *signal) override {
    ttsState_ = signal;
  }

  void setAECStateSignal(TtsStateSignal *signal) override {
    AECState_ = signal;
  }

  // IVadNode specific
  void setInputQueue(AudioFrameQueue *in) override { setCaptureInputQueue(in); }
  void setEventQueue(VadEventQueue *q) override { setVadEventQueue(q); }

protected:
  void wake() override { pool_.stop(); }

  void runLoop() override {
    if (!initialized_)
      return;

    try {
      while (running()) {

        AudioFrameHandle refFrame;
        AudioFrameHandle mic;

        bool isRest = takeRenderQueue(refFrame);

        if (!isRest) {
          // register the time from the first restFrame and the First audio
          // Frame
          ttsActiveTimestampNs_ = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
        }

        const bool ttsActive = (ttsState_ && ttsState_->isActive()) || isRest;
        // Detect transition: TTS starts playing
        if (ttsActive) {

          if (AECState_)
            AECState_->setActive(true);

          if (!micIn_ || !micIn_->pop(mic))
            break;

          if (!wasTtsActive_) {

            warmupFrameCount_ = 0;
            warmupComplete_ = false;
            // Record the moment TTS becomes active so drainRenderQueue() can
            // measure how long it takes for the first render frame to arrive.
            // Measure and log the delay from TTS activation to first render
            // frame arriving at the APM. Use this to tune aecRefQueue capacity:
            //   optimal capacity = ceil(delay_ms / frame_ms) + small_margin
            if (ttsActiveTimestampNs_ > 0) {
              const uint64_t nowNs = static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
              const uint64_t delayMs =
                  (nowNs - ttsActiveTimestampNs_) / 1'000'000ULL;
              cfg_.estimatedRenderDelayMs = (int)delayMs;
              std::fprintf(
                  stderr,
                  "[WebRtcDSP] First render frame received %llums after "
                  "TTS activation"
                  " — suggested aecRefQueue capacity: %llu frames (at "
                  "%dms/frame)\n",
                  static_cast<unsigned long long>(delayMs),
                  static_cast<unsigned long long>(
                      (delayMs + cfg_.format.frameMs - 1) /
                      static_cast<unsigned>(cfg_.format.frameMs)) +
                      4,
                  cfg_.format.frameMs);
              ttsActiveTimestampNs_ = 0;

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
              apm_->set_stream_delay_ms(cfg_.estimatedRenderDelayMs);
#endif
            }
          }
          wasTtsActive_ = ttsActive;

        } else {
          // Reset TTS activation
          wasTtsActive_ = false;
          if (AECState_)
            AECState_->setActive(false);

          ttsActiveTimestampNs_ = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());

          std::this_thread::sleep_for(std::chrono::milliseconds(10));

          continue;
        }

        // Always call drainRenderQueue() to keep render and capture in sync
        if (refFrame)
          processRenderFrame(*refFrame);

        if (!warmupComplete_) {

          ++warmupFrameCount_;
          if (warmupFrameCount_ >= warmupFramesRequired_) {
            warmupComplete_ = true;
          }
        }

        auto clean = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
        if (!clean) {
          if (!running())
            break;
          continue;
        }

        // Always process capture frame so WebRTC APM runs continuously and
        // converges
        const bool processed = processCaptureFrame(*mic, *clean);
        if (!processed)
          continue;

        clean->format = mic->format;
        clean->timestampNs = mic->timestampNs;
        clean->sequence = outputSeq_++;

        const bool speech = evaluateVad(*clean);
        updateVadState(speech, clean->timestampNs);

        if (out_) {
          // Discard frames during warm-up period to avoid sending echo spikes
          // to STT
          const bool inWarmup = ttsActive && !warmupComplete_;
          if (!inWarmup) {
            if (!cfg_.gateOutputWithVad || isSpeaking()) {
              out_->push(std::move(clean));
            }
          }
        }

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
        static int statsCounter = 0;
        if (++statsCounter >= 100) {
          statsCounter = 0;
          if (apm_) {
            auto stats = apm_->GetStatistics();
            std::cout
                << "[WebRtcDSP Stats] ttsActive=" << (ttsActive ? "1" : "0")
                << " delay_ms="
                << (stats.delay_ms ? std::to_string(*stats.delay_ms) : "N/A")
                << " delay_median_ms="
                << (stats.delay_median_ms
                        ? std::to_string(*stats.delay_median_ms)
                        : "N/A")
                << " erle="
                << (stats.echo_return_loss_enhancement
                        ? std::to_string(*stats.echo_return_loss_enhancement)
                        : "N/A")
                << " erl="
                << (stats.echo_return_loss
                        ? std::to_string(*stats.echo_return_loss)
                        : "N/A")
                << " div_filter="
                << (stats.divergent_filter_fraction
                        ? std::to_string(*stats.divergent_filter_fraction)
                        : "N/A")
                << std::endl;
          }
        }
#endif
      }
    } catch (const std::runtime_error &e) {
      std::fprintf(stderr, "[WebRtcDSP] Fatal: %s — node stopped.\n", e.what());
      // Node exits runLoop; the pipeline will detect the missing output and
      // stop cleanly.
    }
  }

private:
  static bool validateFormat(const AudioFormat &f) {
    if (f.channels <= 0 || f.channels > 2)
      return false;
    if (f.frameMs != 10 && f.frameMs != 20 && f.frameMs != 30)
      return false;
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

  bool takeRenderQueue(AudioFrameHandle &ref) {
    // Keep WebRTC reverse/capture streams in lockstep: one 10 ms render
    // reference frame per 10 ms microphone frame. Draining a prefilled TTS
    // queue here would advance the APM render timeline ahead of capture.
    if (refIn_ && refIn_->tryPop(ref)) {
      // Validate format only on the first render frame received.
      // The format is fixed at startup and never changes after that,
      // so per-frame validation is unnecessary overhead.
      if (!renderFormatValidated_) {
        validateRenderFormat(
            ref->format); // throws std::runtime_error on mismatch

        renderFormatValidated_ = true;
      }
      return true;
    }

    return false;
  }

  bool processRenderFrame(const AudioFrame &frame) {
#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    if (!apm_)
      return false;
    // Format already validated once in drainRenderQueue() on the first render
    // frame. No per-frame check needed: format is fixed at startup.

    const int samples = frame.format.totalSamplesPerFrame();
    const int16_t *srcI16 = nullptr;
    if (frame.format.sampleFormat == SampleFormat::Int16) {
      srcI16 = frame.pcm16.data();
    } else {
      if (renderScratchI16_.size() < static_cast<std::size_t>(samples))
        renderScratchI16_.resize(static_cast<std::size_t>(samples));
      for (int i = 0; i < samples; ++i) {
        const float clamped = std::max(
            -1.0f, std::min(1.0f, frame.pcmF32[static_cast<std::size_t>(i)]));
        renderScratchI16_[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(clamped * 32767.0f);
      }
      srcI16 = renderScratchI16_.data();
    }

    if (renderDestI16_.size() < static_cast<std::size_t>(samples))
      renderDestI16_.resize(static_cast<std::size_t>(samples));
    webrtc::StreamConfig streamCfg(frame.format.sampleRate,
                                   frame.format.channels);
    return apm_->ProcessReverseStream(srcI16, streamCfg, streamCfg,
                                      renderDestI16_.data()) ==
           webrtc::AudioProcessing::kNoError;
#else
    (void)frame;
    return true;
#endif
  }

  bool processCaptureFrame(const AudioFrame &input, AudioFrame &output) {
    output.format = cfg_.format;
    output.resizeForFormat();

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    if (!apm_)
      return false;

    const int samples = input.format.totalSamplesPerFrame();
    const int16_t *srcI16 = nullptr;
    if (input.format.sampleFormat == SampleFormat::Int16) {
      srcI16 = input.pcm16.data();
    } else {
      if (captureScratchI16_.size() < static_cast<std::size_t>(samples))
        captureScratchI16_.resize(static_cast<std::size_t>(samples));
      for (int i = 0; i < samples; ++i) {
        const float clamped = std::max(
            -1.0f, std::min(1.0f, input.pcmF32[static_cast<std::size_t>(i)]));
        captureScratchI16_[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(clamped * 32767.0f);
      }
      srcI16 = captureScratchI16_.data();
    }

    if (captureDestI16_.size() < static_cast<std::size_t>(samples))
      captureDestI16_.resize(static_cast<std::size_t>(samples));
    webrtc::StreamConfig streamCfg(input.format.sampleRate,
                                   input.format.channels);

    const int rc = apm_->ProcessStream(srcI16, streamCfg, streamCfg,
                                       captureDestI16_.data());
    if (rc != webrtc::AudioProcessing::kNoError)
      return false;

    if (output.format.sampleFormat == SampleFormat::Int16) {
      std::memcpy(output.pcm16.data(), captureDestI16_.data(),
                  static_cast<std::size_t>(samples) * sizeof(int16_t));
    } else {
      if (output.pcmF32.size() < static_cast<std::size_t>(samples))
        output.pcmF32.resize(static_cast<std::size_t>(samples));
      for (int i = 0; i < samples; ++i)
        output.pcmF32[static_cast<std::size_t>(i)] =
            captureDestI16_[static_cast<std::size_t>(i)] / 32768.0f;
    }
    return true;
#else
    if (!cfg_.allowPassthroughWithoutWebRtc)
      return false;
    output = input;
    output.format = cfg_.format;
    return true;
#endif
  }

  void validateRenderFormat(const AudioFormat &f) const {
    if (f.sampleRate == cfg_.format.sampleRate &&
        f.channels == cfg_.format.channels &&
        f.frameMs == cfg_.format.frameMs &&
        f.sampleFormat == SampleFormat::Int16) {
      return;
    }

    // Format mismatch is a configuration error — it will never self-correct at
    // runtime. Throw so the caller can log and stop the node cleanly instead of
    // aborting the process.
    throw std::runtime_error(
        std::string("[WebRtcDSP] render format mismatch: got ") +
        std::to_string(f.sampleRate) + "Hz/" + std::to_string(f.channels) +
        "ch/" + std::to_string(f.frameMs) + "ms/" +
        (f.sampleFormat == SampleFormat::Int16 ? "Int16" : "Float32") +
        " — expected " + std::to_string(cfg_.format.sampleRate) + "Hz/" +
        std::to_string(cfg_.format.channels) + "ch/" +
        std::to_string(cfg_.format.frameMs) + "ms/Int16");
  }

  bool evaluateVad(const AudioFrame &frame) {
    if (!cfg_.enableVad)
      return true;

    const int samplesPerChannel = frame.format.samplesPerChannelPerFrame();
    (void)samplesPerChannel; // used only with VOICE_RUNTIME_ENABLE_WEBRTC_VAD
    const int totalSamples = frame.format.totalSamplesPerFrame();

    const int16_t *data = nullptr;
    if (frame.format.sampleFormat == SampleFormat::Int16) {
      if (static_cast<int>(frame.pcm16.size()) < totalSamples)
        return false;
      data = frame.pcm16.data();
    } else {
      if (static_cast<int>(frame.pcmF32.size()) < totalSamples)
        return false;
      if (vadScratchI16_.size() < static_cast<std::size_t>(totalSamples))
        vadScratchI16_.resize(static_cast<std::size_t>(totalSamples));
      for (int i = 0; i < totalSamples; ++i) {
        const float clamped = std::max(
            -1.0f, std::min(1.0f, frame.pcmF32[static_cast<std::size_t>(i)]));
        vadScratchI16_[static_cast<std::size_t>(i)] =
            static_cast<int16_t>(clamped * 32767.0f);
      }
      data = vadScratchI16_.data();
    }

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
    if (vad_) {
      const int vadResult = WebRtcVad_Process(
          vad_.get(), frame.format.sampleRate, data, samplesPerChannel);
      if (vadResult >= 0)
        return vadResult == 1;
    }
#endif
    return energyVad(data, totalSamples);
  }

  bool energyVad(const int16_t *data, int samples) const {
    if (!data || samples <= 0)
      return false;
    double acc = 0.0;
    for (int i = 0; i < samples; ++i) {
      const double v = static_cast<double>(data[i]) / 32768.0;
      acc += v * v;
    }
    const double rms = std::sqrt(acc / samples);
    return rms >= static_cast<double>(cfg_.vadSpeechThreshold) * 0.05;
  }

  void updateVadState(bool speechNow, uint64_t timestampNs) {
    if (!cfg_.enableVad)
      return;

    if (speechNow) {
      lastSpeechNs_ = timestampNs;
      if (!speaking_.exchange(true, std::memory_order_acq_rel)) {
        emitVadEvent(VadEvent::Type::SpeechStart, timestampNs, 1.0f);
      }
      return;
    }

    const bool wasSpeaking = speaking_.load(std::memory_order_acquire);
    if (!wasSpeaking)
      return;

    const uint64_t hangoverNs =
        static_cast<uint64_t>(std::max(0, cfg_.vadHangoverMs)) * 1000000ULL;
    if (timestampNs >= lastSpeechNs_ &&
        (timestampNs - lastSpeechNs_) >= hangoverNs) {
      speaking_.store(false, std::memory_order_release);
      emitVadEvent(VadEvent::Type::SpeechEnd, timestampNs, 0.0f);
    }
  }

  void emitVadEvent(VadEvent::Type type, uint64_t timestampNs,
                    float confidence) {
    if (!evq_)
      return;

    VadEvent ev;
    ev.type = type;
    ev.timestampNs = timestampNs;
    ev.confidence = confidence;
    evq_->push(ev);
  }

private:
  WebRtcDspConfig cfg_;

  bool wasTtsActive_ = false;

  AudioFrameQueue *micIn_ = nullptr;
  AudioFrameQueue *refIn_ = nullptr;
  AudioFrameQueue *out_ = nullptr;
  VadEventQueue *evq_ = nullptr;

  SharedBufferPool<AudioFrame> pool_;
  uint64_t outputSeq_ = 0;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> speaking_{false};
  uint64_t lastSpeechNs_ = 0;

  const TtsStateSignal *ttsState_ = nullptr;
  TtsStateSignal *AECState_ = nullptr;

  // AEC warm-up gate
  int warmupFramesRequired_ = 0;
  int warmupFrameCount_ = 0;
  bool warmupComplete_ = false;

  std::vector<int16_t> renderScratchI16_;
  std::vector<int16_t> captureScratchI16_;
  std::vector<int16_t> renderDestI16_;
  std::vector<int16_t> captureDestI16_;
  std::vector<int16_t> vadScratchI16_;

  // Set to true after the first render frame has been validated.
  // validateRenderFormat() is called only once: format is fixed at startup.
  bool renderFormatValidated_ = false;

  // Timestamp (steady_clock ns) recorded when ttsActive first becomes true.
  // Used to measure the delay from TTS activation to first render frame
  // arriving at the APM — printed once in drainRenderQueue() to help calibrate
  // aecRefQueue capacity.
  uint64_t ttsActiveTimestampNs_ = 0;

#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
#endif

#if VOICE_RUNTIME_ENABLE_WEBRTC_VAD
  struct VadDeleter {
    void operator()(VadInst *p) const noexcept {
      if (p)
        WebRtcVad_Free(p);
    }
  };
  std::unique_ptr<VadInst, VadDeleter> vad_;
#endif
};

} // namespace voice_runtime
