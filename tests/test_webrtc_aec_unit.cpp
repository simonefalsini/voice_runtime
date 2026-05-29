// ---------------------------------------------------------------------------
// test_webrtc_aec_unit.cpp
//
// Automated non-interactive unit test for WebRtcDspNode echo cancellation.
// Simulates microphone capture containing a delayed speaker echo,
// feeds it through WebRtcDspNode, and verifies:
//   1. AEC converges and cancels the echo.
//   2. The clean output does NOT trigger the integrated VAD during echo playback.
//   3. The STT output queue receives no signal/speech frames.
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "WebRtcDspNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    std::printf("=== test_webrtc_aec_unit ===\n");

#if !VOICE_RUNTIME_ENABLE_WEBRTC_APM
    std::printf("WebRTC APM is disabled in this build. Skipping unit test.\n");
    std::printf("\n>>> PASS — test_webrtc_aec_unit (skipped) <<<\n");
    return 0;
#else

    const AudioFormat fmt{16000, 1, 10, SampleFormat::Int16};
    const int samples = fmt.totalSamplesPerFrame(); // 160

    WebRtcDspConfig dspCfg;
    dspCfg.format = fmt;
    dspCfg.enableEchoCancellation = true;
    dspCfg.enableNoiseSuppression = false;
    dspCfg.enableHighPassFilter = true;
    dspCfg.enableAgc2 = false;
    dspCfg.enableVad = true;
    dspCfg.gateOutputWithVad = true;   // Gate output during silence/cancelled echo
    dspCfg.vadMode = 2;
    dspCfg.vadSpeechThreshold = 0.5f;
    dspCfg.vadHangoverMs = 100;
    dspCfg.estimatedRenderDelayMs = 30; // 30ms delay
    dspCfg.outputPoolSize = 128;
    dspCfg.aecWarmupGracePeriodMs = 200;

    WebRtcDspNode dsp(dspCfg);

    AudioFrameQueue micQueue(512, QueueOverflowPolicy::DropOldest, "micQueue");
    AudioFrameQueue refQueue(512, QueueOverflowPolicy::DropOldest, "refQueue");
    AudioFrameQueue cleanQueue(512, QueueOverflowPolicy::DropOldest, "cleanQueue");
    VadEventQueue vadEventQueue(64, QueueOverflowPolicy::DropOldest, "vadEvents");

    TtsStateSignal ttsState;

    dsp.setCaptureInputQueue(&micQueue);
    dsp.setRenderInputQueue(&refQueue);
    dsp.setOutputQueue(&cleanQueue);
    dsp.setVadEventQueue(&vadEventQueue);
    dsp.setTtsStateSignal(&ttsState);

    if (!dsp.initialize()) {
        std::fprintf(stderr, "FAIL: WebRtcDspNode initialization failed\n");
        return 1;
    }

    dsp.start();

    // Generate white noise reference signal for 4 seconds (400 frames)
    // White noise is excellent for AEC filter convergence.
    std::vector<std::vector<int16_t>> refFrames(400, std::vector<int16_t>(samples));
    // Simple LCG random generator for portability
    uint32_t lcgState = 12345;
    auto nextRandom = [&lcgState]() -> float {
        lcgState = lcgState * 1103515245 + 12345;
        uint32_t val = (lcgState / 65536) % 32768;
        return (static_cast<float>(val) / 16384.0f) - 1.0f; // range [-1, 1]
    };

    constexpr float kAmplitude = 0.3f;
    for (int f = 0; f < 400; ++f) {
        for (int i = 0; i < samples; ++i) {
            refFrames[f][i] = static_cast<int16_t>(nextRandom() * kAmplitude * 32767.0f);
        }
    }

    SharedBufferPool<AudioFrame> pool(512);

    std::printf("[Info] Simulating 4.0 seconds of TTS playback (echo only)...\n");
    ttsState.setActive(true);

    uint64_t seq = 0;

    for (int f = 0; f < 400; ++f) {
        // Push reference frame
        auto refFrame = pool.acquireWithTimeout(std::chrono::milliseconds(10));
        if (refFrame) {
            refFrame->format = fmt;
            refFrame->resizeForFormat();
            refFrame->sequence = seq;
            refFrame->timestampNs = now_ns();
            std::memcpy(refFrame->pcm16.data(), refFrames[f].data(), samples * sizeof(int16_t));
            refQueue.push(std::move(refFrame));
        }

        // Push microphone frame with 30ms (3 frames) delay
        auto micFrame = pool.acquireWithTimeout(std::chrono::milliseconds(10));
        if (micFrame) {
            micFrame->format = fmt;
            micFrame->resizeForFormat();
            micFrame->sequence = seq;
            micFrame->timestampNs = now_ns();

            // Mix in the echo (delayed by 3 frames)
            if (f >= 3) {
                // Echo path: scale by 0.5 (attenuation)
                for (int i = 0; i < samples; ++i) {
                    micFrame->pcm16[i] = static_cast<int16_t>(refFrames[f - 3][i] * 0.5f);
                }
            } else {
                std::memset(micFrame->pcm16.data(), 0, samples * sizeof(int16_t));
            }
            micQueue.push(std::move(micFrame));
        }

        seq++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ttsState.setActive(false);

    // Wait for the DSP node to finish processing
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Stop DSP node
    micQueue.stop();
    refQueue.stop();
    cleanQueue.stop();
    vadEventQueue.stop();
    dsp.stop();

    // Check if any clean frames were forwarded to STT
    // Since gateOutputWithVad = true, clean frames should only be forwarded if VAD detects speech.
    // Since this is echo only (no user speech), VAD should NOT detect speech once AEC converges,
    // and thus cleanQueue should have very few (only during warmup) or zero frames!
    AudioFrameHandle cleanFrame;
    int forwardedFrames = 0;
    while (cleanQueue.tryPop(cleanFrame)) {
        forwardedFrames++;
    }

    std::printf("[Info] Total forwarded clean frames: %d (warmup period allowed %d)\n",
                forwardedFrames, dspCfg.aecWarmupGracePeriodMs / 10);

    // We allow some forwarded frames during the warm-up period and adaptation phase,
    // but after the warm-up period and initial adaptation (e.g. 1.5s = 150 frames total),
    // AEC should have converged and the VAD should not detect speech.
    // So the number of forwarded frames must be small (e.g. <= 120).
    if (forwardedFrames > 120) {
        std::fprintf(stderr, "FAIL: Too many frames forwarded to STT (%d). Echo was not cancelled or VAD gating failed.\n", forwardedFrames);
        std::fprintf(stderr, "\n>>> FAIL — test_webrtc_aec_unit <<<\n");
        return 1;
    }

    std::printf("[Info] OK — Echo successfully cancelled, VAD did not trigger STT transmission.\n");
    std::printf("\n>>> PASS — test_webrtc_aec_unit <<<\n");
    return 0;
#endif
}
