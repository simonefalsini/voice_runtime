// ---------------------------------------------------------------------------
// test_wasapi_mic.cpp
//
// Standalone test for WasapiMicNode.
//
// Captures audio from the default Windows microphone for 5 seconds,
// prints the RMS level of each frame, and verifies that at least 450 frames
// (90% of expected 500 @ 10ms) were produced without errors.
//
// Usage:
//   .\test_wasapi_mic.exe [duration_sec]
//   Default duration: 5 seconds
//
// Expected output:
//   [WasapiMic] Streaming — target: 16000Hz 1ch 10ms (SHARED)
//   [Frame 0001] RMS=0.0042
//   ...
//   [PASS] Captured 493 frames in 5s
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
#  error "test_wasapi_mic.cpp is only available on Windows"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"

#include "WasapiMicNode.h"

using namespace voice_runtime;

static std::atomic<bool> g_running{true};
static void sigHandler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    int durationSec = (argc > 1) ? std::atoi(argv[1]) : 5;
    if (durationSec <= 0) durationSec = 5;

    std::printf("\n=== test_wasapi_mic (%ds) ===\n\n", durationSec);
    std::signal(SIGINT, sigHandler);

    // ---- Config & queues ------------------------------------------------
    WasapiMicConfig cfg;
    cfg.format          = {16000, 1, 10, SampleFormat::Int16};
    cfg.enableDcRemoval = true;
    cfg.poolSize        = 256;

    AudioFrameQueue micQueue(512, QueueOverflowPolicy::DropOldest, "micQueue");

    WasapiMicNode mic(cfg);
    mic.setOutputQueue(&micQueue);

    if (!mic.initialize()) {
        std::fprintf(stderr, "[FAIL] WasapiMicNode initialization failed\n");
        return 1;
    }

    mic.start();

    // ---- Drain and print frames ------------------------------------------
    const auto startTime = std::chrono::steady_clock::now();
    int frameCount = 0;
    int printEvery = 10; // print every N frames to avoid flooding

    while (g_running.load()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= durationSec) break;

        AudioFrameHandle frame;
        if (micQueue.tryPop(frame)) {
            ++frameCount;

            // Compute RMS
            const int n = static_cast<int>(frame->pcm16.size());
            double acc = 0.0;
            for (int i = 0; i < n; ++i) {
                const double v = static_cast<double>(frame->pcm16[i]) / 32768.0;
                acc += v * v;
            }
            const double rms = (n > 0) ? std::sqrt(acc / n) : 0.0;

            if (frameCount % printEvery == 1) {
                std::printf("  [Frame %04d] RMS=%.4f\n", frameCount, rms);
                std::fflush(stdout);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // ---- Shutdown -------------------------------------------------------
    micQueue.stop();
    mic.stop();
    micQueue.clear();

    // ---- Results --------------------------------------------------------
    const int expectedMin = (durationSec * 1000 / 10) * 90 / 100; // 90% of expected
    std::printf("\n=== Results ===\n");
    std::printf("  Duration:    %ds\n", durationSec);
    std::printf("  Frames:      %d (expected >= %d)\n", frameCount, expectedMin);

    const auto stats = micQueue.stats();
    std::printf("  Produced:    %llu\n", static_cast<unsigned long long>(stats.produced));
    std::printf("  Consumed:    %llu\n", static_cast<unsigned long long>(stats.consumed));
    std::printf("  Dropped:     %llu\n", static_cast<unsigned long long>(stats.dropped));

    if (frameCount < expectedMin) {
        std::fprintf(stderr, "\n[FAIL] Only %d frames captured (expected >= %d)\n",
                     frameCount, expectedMin);
        return 1;
    }

    std::printf("\n[PASS] Captured %d frames in %ds\n", frameCount, durationSec);
    return 0;
}
