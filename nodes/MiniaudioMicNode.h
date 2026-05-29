#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/miniaudio.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// MiniaudioMicConfig
// ---------------------------------------------------------------------------

struct MiniaudioMicConfig {
    AudioFormat format = {16000, 1, 10, SampleFormat::Int16};
    std::size_t poolSize   = 256;
    int         deviceIndex = -1;   // -1 = system default
    bool        enableDcRemoval = true;
};

// ---------------------------------------------------------------------------
// MiniaudioMicNode
//   Callback-driven microphone capture node.
//
//   miniaudio captures in float32.  The audio callback:
//     1. Acquires an AudioFrame from the pool (non-blocking tryAcquire).
//     2. Optionally removes DC offset (mean subtraction, from DS4).
//     3. Converts float32 → Int16.
//     4. Pushes the frame to the output queue.
//
//   runLoop() idles on a condition variable — all real work happens in the
//   callback on miniaudio's audio thread.
//
//   Lifecycle:
//     initialize() → ma_device_init
//     start()      → ActiveNodeBase::start() (spawns idle thread)
//                    then ma_device_start
//     stop()       → ma_device_stop + ma_device_uninit
//                    then ActiveNodeBase::stop() (joins thread)
// ---------------------------------------------------------------------------

class MiniaudioMicNode final
    : public ActiveNodeBase
    , public IMicrophoneNode
{
public:
    explicit MiniaudioMicNode(MiniaudioMicConfig config)
        : cfg_(std::move(config))
        , pool_(cfg_.poolSize)
    {}

    const char* name() const override { return "MiniaudioMic"; }

    bool initialize() override {
        ma_device_config devCfg = ma_device_config_init(ma_device_type_capture);
        devCfg.capture.format   = ma_format_f32;
        devCfg.capture.channels = static_cast<ma_uint32>(cfg_.format.channels);
        devCfg.sampleRate       = static_cast<ma_uint32>(cfg_.format.sampleRate);
        devCfg.dataCallback     = dataCallback;
        devCfg.pUserData        = this;

        // Period size: one frame worth of samples
        devCfg.periodSizeInFrames = static_cast<ma_uint32>(
            cfg_.format.samplesPerChannelPerFrame());

        if (cfg_.deviceIndex >= 0) {
            // Device selection would require enumeration; for now, non-default
            // devices are not supported.  The config field is reserved for
            // future use.
            std::fprintf(stderr, "[MiniaudioMic] Warning: deviceIndex != -1 "
                         "not yet implemented, using default device\n");
        }

        ma_result result = ma_device_init(nullptr, &devCfg, &device_);
        if (result != MA_SUCCESS) {
            std::fprintf(stderr, "[MiniaudioMic] ma_device_init failed: %d\n",
                         static_cast<int>(result));
            return false;
        }

        deviceInitialized_ = true;
        return true;
    }

    void setOutputQueue(AudioFrameQueue* q) override { out_ = q; }

    // Override start: spawn the idle thread, then start the device
    void start() override {
        ActiveNodeBase::start();

        if (deviceInitialized_) {
            ma_result result = ma_device_start(&device_);
            if (result != MA_SUCCESS) {
                std::fprintf(stderr, "[MiniaudioMic] ma_device_start failed: %d\n",
                             static_cast<int>(result));
            }
        }
    }

    // Override stop: stop device first, then join idle thread
    void stop() override {
        if (deviceInitialized_) {
            ma_device_stop(&device_);
            ma_device_uninit(&device_);
            deviceInitialized_ = false;
        }
        // Unblock the idle loop and join the thread
        pool_.stop();
        ActiveNodeBase::stop();
    }

protected:
    void wake() override {
        pool_.stop();
        // Unblock the idle loop
        {
            std::lock_guard<std::mutex> lk(idleMtx_);
            idleWake_ = true;
        }
        idleCv_.notify_one();
    }

    // The runLoop does nothing — all work is in the callback.
    // We block on a CV so the ActiveNodeBase thread doesn't spin.
    void runLoop() override {
        std::unique_lock<std::mutex> lk(idleMtx_);
        idleCv_.wait(lk, [this] { return !running() || idleWake_; });
    }

private:
    // -----------------------------------------------------------------------
    // miniaudio data callback — runs on the audio thread
    // -----------------------------------------------------------------------
    static void dataCallback(ma_device* pDevice, void* /*pOutput*/,
                             const void* pInput, ma_uint32 frameCount) {
        auto* self = static_cast<MiniaudioMicNode*>(pDevice->pUserData);
        if (!pInput || frameCount == 0) return;

        const float* input = static_cast<const float*>(pInput);
        const int channels = self->cfg_.format.channels;
        const int frameSamples = self->cfg_.format.totalSamplesPerFrame();
        const int frameSamplesPerChannel = self->cfg_.format.samplesPerChannelPerFrame();

        // miniaudio may deliver more samples than one frame.
        // Process in frame-sized chunks.
        ma_uint32 samplesDelivered = frameCount;  // per-channel samples
        ma_uint32 offset = 0;

        while (offset < samplesDelivered) {
            const ma_uint32 remaining = samplesDelivered - offset;
            if (static_cast<int>(remaining) < frameSamplesPerChannel) {
                // Partial frame at end — drop it.  miniaudio aligns callbacks
                // to the period size, so this should be rare.
                break;
            }

            // Try to acquire a frame from the pool (non-blocking)
            auto frame = self->pool_.tryAcquire();
            if (!frame) {
                // Pool exhausted — drop this chunk and move on
                offset += static_cast<ma_uint32>(frameSamplesPerChannel);
                continue;
            }

            frame->format = self->cfg_.format;
            frame->resizeForFormat();
            frame->sequence    = self->seq_++;
            frame->timestampNs = nowNs();

            const float* chunk = input + static_cast<std::size_t>(offset) *
                                         static_cast<std::size_t>(channels);

            // DC offset removal (from DS4 listening_loop)
            if (self->cfg_.enableDcRemoval) {
                const int totalSamples = frameSamples;
                float sum = 0.0f;
                for (int i = 0; i < totalSamples; ++i) {
                    sum += chunk[i];
                }
                const float mean = sum / static_cast<float>(totalSamples);

                // Convert float32 → Int16 with DC removal
                for (int i = 0; i < totalSamples; ++i) {
                    const float val = chunk[i] - mean;
                    const float clamped = std::max(-1.0f, std::min(1.0f, val));
                    frame->pcm16[static_cast<std::size_t>(i)] =
                        static_cast<int16_t>(clamped * 32767.0f);
                }
            } else {
                // Convert float32 → Int16 without DC removal
                for (int i = 0; i < frameSamples; ++i) {
                    const float clamped = std::max(-1.0f, std::min(1.0f, chunk[i]));
                    frame->pcm16[static_cast<std::size_t>(i)] =
                        static_cast<int16_t>(clamped * 32767.0f);
                }
            }

            if (self->out_) self->out_->push(std::move(frame));

            offset += static_cast<ma_uint32>(frameSamplesPerChannel);
        }
    }

    // -----------------------------------------------------------------------
    // Utility
    // -----------------------------------------------------------------------
    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    MiniaudioMicConfig cfg_;
    AudioFrameQueue*   out_ = nullptr;
    uint64_t           seq_ = 0;
    SharedBufferPool<AudioFrame> pool_;

    ma_device device_{};
    bool      deviceInitialized_ = false;

    // Idle loop synchronization
    std::mutex              idleMtx_;
    std::condition_variable idleCv_;
    bool                    idleWake_ = false;
};

} // namespace voice_runtime
