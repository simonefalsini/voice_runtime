#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <condition_variable>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/miniaudio.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// MiniaudioOutputConfig
// ---------------------------------------------------------------------------

struct MiniaudioOutputConfig {
    AudioFormat format = {24000, 1, 10, SampleFormat::Int16};
    int         deviceIndex = -1;   // -1 = system default

    // Ring buffer capacity in samples. (Retained in config for backwards compatibility)
    std::size_t ringBufferCapacity = 12000;
};

// ---------------------------------------------------------------------------
// MiniaudioOutputNode
//   Callback-driven audio output node consuming directly from the queue.
//
//   Eliminates intermediate ring buffers and worker thread processing
//   to avoid priority inversion and CPU starvation under heavy loads.
//
//   Lifecycle:
//     initialize() → ma_device_init (playback)
//     start()      → ma_device_start, then ActiveNodeBase::start()
//     stop()       → ma_device_stop + ma_device_uninit,
//                    then ActiveNodeBase::stop()
// ---------------------------------------------------------------------------

class MiniaudioOutputNode final
    : public ActiveNodeBase
    , public IAudioOutputNode
{
public:
    explicit MiniaudioOutputNode(MiniaudioOutputConfig config)
        : cfg_(std::move(config))
    {}

    const char* name() const override { return "MiniaudioOut"; }

    bool initialize() override {
        ma_device_config devCfg = ma_device_config_init(ma_device_type_playback);
        devCfg.playback.format   = ma_format_f32;
        devCfg.playback.channels = static_cast<ma_uint32>(cfg_.format.channels);
        devCfg.sampleRate        = static_cast<ma_uint32>(cfg_.format.sampleRate);
        devCfg.dataCallback      = dataCallback;
        devCfg.pUserData         = this;

        // Period size: one frame worth of samples
        devCfg.periodSizeInFrames = static_cast<ma_uint32>(
            cfg_.format.samplesPerChannelPerFrame());

        if (cfg_.deviceIndex >= 0) {
            std::fprintf(stderr, "[MiniaudioOut] Warning: deviceIndex != -1 "
                         "not yet implemented, using default device\n");
        }

        ma_result result = ma_device_init(nullptr, &devCfg, &device_);
        if (result != MA_SUCCESS) {
            std::fprintf(stderr, "[MiniaudioOut] ma_device_init failed: %d\n",
                         static_cast<int>(result));
            return false;
        }

        deviceInitialized_ = true;
        return true;
    }

    void setInputQueue(AudioFrameQueue* q) override { in_ = q; }

    // Override start: start device, then spawn the runLoop thread
    void start() override {
        if (deviceInitialized_) {
            ma_result result = ma_device_start(&device_);
            if (result != MA_SUCCESS) {
                std::fprintf(stderr, "[MiniaudioOut] ma_device_start failed: %d\n",
                             static_cast<int>(result));
            }
        }
        ActiveNodeBase::start();
    }

    // Override stop: stop device first (stops callback), then join thread
    void stop() override {
        if (deviceInitialized_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            deviceInitialized_ = false;
        }
        currentFrame_.reset();
        currentFrameOffset_ = 0;
        ActiveNodeBase::stop();
    }

protected:
    void wake() override {
        {
            std::lock_guard<std::mutex> lk(idleMtx_);
            idleWake_ = true;
        }
        idleCv_.notify_one();
    }

    void runLoop() override {
        std::unique_lock<std::mutex> lk(idleMtx_);
        idleCv_.wait(lk, [this] { return !running() || idleWake_; });
    }

private:
    // -----------------------------------------------------------------------
    // miniaudio data callback — runs on the audio thread
    // Pops frames directly from the queue and feeds the speaker.
    // -----------------------------------------------------------------------
    static void dataCallback(ma_device* pDevice, void* pOutput,
                             const void* /*pInput*/, ma_uint32 frameCount) {
        auto* self = static_cast<MiniaudioOutputNode*>(pDevice->pUserData);
        if (!pOutput || frameCount == 0) return;

        float* out = static_cast<float*>(pOutput);
        const std::size_t channels = static_cast<std::size_t>(
            self->cfg_.format.channels);
        const std::size_t totalSamplesNeeded = static_cast<std::size_t>(frameCount) * channels;

        std::size_t samplesWritten = 0;
        while (samplesWritten < totalSamplesNeeded) {
            // Check if we need a new frame
            if (!self->currentFrame_ || self->currentFrameOffset_ >= static_cast<std::size_t>(self->currentFrame_->format.totalSamplesPerFrame())) {
                self->currentFrame_.reset();
                self->currentFrameOffset_ = 0;

                AudioFrameHandle nextFrame;
                if (self->in_ && self->in_->tryPop(nextFrame)) {
                    self->currentFrame_ = std::move(nextFrame);
                } else {
                    // Queue empty: fill the rest of the period with silence
                    std::fill(out + samplesWritten, out + totalSamplesNeeded, 0.0f);
                    break;
                }
            }

            const std::size_t frameSamples = self->currentFrame_->format.totalSamplesPerFrame();
            const std::size_t available = frameSamples - self->currentFrameOffset_;
            const std::size_t toWrite = std::min(available, totalSamplesNeeded - samplesWritten);

            if (self->currentFrame_->format.sampleFormat == SampleFormat::Int16) {
                const int16_t* src = self->currentFrame_->pcm16.data() + self->currentFrameOffset_;
                for (std::size_t i = 0; i < toWrite; ++i) {
                    out[samplesWritten + i] = static_cast<float>(src[i]) / 32768.0f;
                }
            } else {
                const float* src = self->currentFrame_->pcmF32.data() + self->currentFrameOffset_;
                std::memcpy(out + samplesWritten, src, toWrite * sizeof(float));
            }

            samplesWritten += toWrite;
            self->currentFrameOffset_ += toWrite;
        }
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    MiniaudioOutputConfig cfg_;
    AudioFrameQueue*      in_ = nullptr;

    ma_device device_{};
    bool      deviceInitialized_ = false;

    // Current frame state (only accessed by the callback audio thread)
    AudioFrameHandle currentFrame_;
    std::size_t      currentFrameOffset_ = 0;

    // Idle loop synchronization
    std::mutex              idleMtx_;
    std::condition_variable idleCv_;
    bool                    idleWake_ = false;
};

} // namespace voice_runtime
