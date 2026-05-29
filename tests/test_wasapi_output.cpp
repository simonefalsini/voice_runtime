// ---------------------------------------------------------------------------
// test_wasapi_output.cpp
//
// Standalone test for WasapiOutputNode.
//
// Generates 2 seconds of a 440Hz sine tone at 24kHz mono Int16, pushes
// it onto the speaker queue, and verifies that no frames were dropped
// (i.e., the output node drained the queue completely).
//
// Usage:
//   .\test_wasapi_output.exe [duration_sec]
//   Default duration: 2 seconds (+ 0.5s drain wait)
//
// Expected output:
//   [WasapiOutput] Streaming — source: 24000Hz 1ch 10ms (SHARED)
//   [Info] Pushed 200 frames (2.0s of 440Hz tone)
//   [PASS] Output complete — dropped=0
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
#  error "test_wasapi_output.cpp is only available on Windows"
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

#include "WasapiOutputNode.h"

using namespace voice_runtime;

static std::atomic<bool> g_running{true};
static void sigHandler(int) { g_running.store(false); }

int main(int argc, char* argv[]) {
    int durationSec = (argc > 1) ? std::atoi(argv[1]) : 2;
    if (durationSec <= 0) durationSec = 2;

    std::printf("\n=== test_wasapi_output (%ds @ 440Hz) ===\n\n", durationSec);
    std::signal(SIGINT, sigHandler);

    // ---- Config & queues ------------------------------------------------
    WasapiOutputConfig cfg;
    cfg.format           = {24000, 1, 10, SampleFormat::Int16};
    cfg.prebufferFrames  = 5;

    AudioFrameQueue speakerQueue(512, QueueOverflowPolicy::BlockProducer, "speakerQueue");

    WasapiOutputNode out(cfg);
    out.setInputQueue(&speakerQueue);

    if (!out.initialize()) {
        std::fprintf(stderr, "[FAIL] WasapiOutputNode initialization failed\n");
        return 1;
    }

    out.start();

    // ---- Generate and push sine tone frames ------------------------------
    SharedBufferPool<AudioFrame> pool(512);

    const AudioFormat fmt = cfg.format;
    const int samplesPerFrame = fmt.samplesPerChannelPerFrame(); // 240 @ 24kHz/10ms
    const int totalFrames = durationSec * 1000 / fmt.frameMs;
    constexpr float kFreq = 440.0f;
    constexpr float kAmplitude = 0.5f;

    uint64_t seq = 0;
    float phase = 0.0f;
    const float phaseStep = 2.0f * 3.14159265f * kFreq / static_cast<float>(fmt.sampleRate);

    for (int f = 0; f < totalFrames && g_running.load(); ++f) {
        auto frame = pool.acquireWithTimeout(std::chrono::milliseconds(100));
        if (!frame) {
            std::fprintf(stderr, "[WARN] Pool exhausted at frame %d\n", f);
            continue;
        }

        frame->format = fmt;
        frame->resizeForFormat();
        frame->sequence = seq++;
        frame->timestampNs = 0;

        for (int i = 0; i < samplesPerFrame; ++i) {
            frame->pcm16[static_cast<std::size_t>(i)] =
                static_cast<int16_t>(std::sin(phase) * kAmplitude * 32767.0f);
            phase += phaseStep;
        }

        speakerQueue.push(std::move(frame));
    }

    std::printf("  [Info] Pushed %d frames (%.1fs of %.0fHz tone)\n",
                totalFrames, static_cast<float>(durationSec), static_cast<double>(kFreq));
    std::fflush(stdout);

    // ---- Wait for drain -------------------------------------------------
    std::printf("  [Info] Waiting for drain...\n");
    std::fflush(stdout);

    const auto drainStart = std::chrono::steady_clock::now();
    while (g_running.load()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - drainStart).count();
        if (elapsed > 1500) break; // max 1.5s drain wait
        if (speakerQueue.size() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Extra 200ms to let the hardware buffer flush
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ---- Shutdown -------------------------------------------------------
    speakerQueue.stop();
    out.stop();
    speakerQueue.clear();

    // ---- Results --------------------------------------------------------
    const auto stats = speakerQueue.stats();
    std::printf("\n=== Results ===\n");
    std::printf("  Pushed frames:  %d\n", totalFrames);
    std::printf("  Queue produced: %llu\n",
                static_cast<unsigned long long>(stats.produced));
    std::printf("  Queue consumed: %llu\n",
                static_cast<unsigned long long>(stats.consumed));
    std::printf("  Queue dropped:  %llu\n",
                static_cast<unsigned long long>(stats.dropped));

    if (stats.dropped > 0) {
        std::fprintf(stderr,
            "\n[FAIL] %llu frames were dropped — output node did not drain the queue fully\n",
            static_cast<unsigned long long>(stats.dropped));
        return 1;
    }

    std::printf("\n[PASS] Output complete — dropped=0\n");
    return 0;
}
