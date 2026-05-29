// ---------------------------------------------------------------------------
// test_vad_node.cpp — SileroVadNode test with energy-based fallback
//
// No GGML model file is provided, so the node falls back to the RMS
// energy-based VAD.  We verify:
//   1. Silence frames produce no SpeechStart
//   2. Loud frames trigger SpeechStart
//   3. Silence after speech triggers SpeechEnd (after hangover)
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "SileroVadNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Create an AudioFrameHandle filled with a specific pattern
static AudioFrameHandle makeFrame(SharedBufferPool<AudioFrame>& pool,
                                   const AudioFormat& fmt,
                                   uint64_t seq,
                                   bool loud) {
    auto frame = pool.acquireWithTimeout(std::chrono::milliseconds(100));
    assert(frame && "Pool exhausted during test frame creation");

    frame->format = fmt;
    frame->resizeForFormat();
    frame->sequence = seq;
    frame->timestampNs = now_ns();

    const int samples = fmt.totalSamplesPerFrame();

    if (loud) {
        // Generate a 1kHz sine wave at ~0.5 amplitude
        // This should produce high RMS → speech detection
        constexpr float kFreq = 1000.0f;
        constexpr float kAmplitude = 0.5f;
        for (int i = 0; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(fmt.sampleRate);
            const float val = kAmplitude * std::sin(2.0f * 3.14159265f * kFreq * t);
            frame->pcm16[static_cast<std::size_t>(i)] =
                static_cast<int16_t>(val * 32767.0f);
        }
    } else {
        // Silence — all zeros
        for (int i = 0; i < samples; ++i) {
            frame->pcm16[static_cast<std::size_t>(i)] = 0;
        }
    }

    return frame;
}

int main() {
    std::printf("=== test_vad_node ===\n");

    // Configuration
    const AudioFormat fmt{16000, 1, 10, SampleFormat::Int16};
    const int samplesPerFrame = fmt.totalSamplesPerFrame(); // 160

    SileroVadConfig cfg;
    cfg.modelPath = "";  // No model → energy fallback
    cfg.format = fmt;
    cfg.poolSize = 128;
    cfg.thresholdStart = 0.6f;
    cfg.thresholdStop = 0.45f;
    cfg.hangoverChunks = 5;  // 5 inference chunks hangover
    // Since the VAD accumulates 4 frames per inference, hangover of 5 chunks
    // means 5 × 4 = 20 frames of silence needed after speech.

    SileroVadNode node(cfg);

    // Queues
    AudioFrameQueue inQueue(512, QueueOverflowPolicy::DropOldest, "vad_in");
    AudioFrameQueue outQueue(512, QueueOverflowPolicy::DropOldest, "vad_out");
    VadEventQueue eventQueue(64, QueueOverflowPolicy::DropOldest, "vad_events");

    // Pool for generating test frames
    SharedBufferPool<AudioFrame> testPool(512);

    // Wire
    node.setInputQueue(&inQueue);
    node.setOutputQueue(&outQueue);
    node.setEventQueue(&eventQueue);

    // Initialize
    const bool initOk = node.initialize();
    assert(initOk && "VAD node initialization failed");
    std::printf("[Info] VAD initialized (energy fallback expected)\n");

    // Start the node
    node.start();

    uint64_t seq = 0;

    // ---- Phase 1: Push 50 frames of silence ---------------------------------
    std::printf("[Phase 1] Pushing 50 silent frames...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, /*loud=*/false);
        inQueue.push(std::move(frame));
    }
    // Give the VAD time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check: no SpeechStart event should have been emitted
    VadEvent ev;
    bool gotSpeechStartDuringSilence = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechStart) {
            gotSpeechStartDuringSilence = true;
        }
    }
    if (gotSpeechStartDuringSilence) {
        std::fprintf(stderr, "[Phase 1] FAIL: SpeechStart during silence!\n");
        inQueue.stop();
        node.stop();
        std::fprintf(stderr, "\n>>> FAIL — test_vad_node <<<\n");
        return 1;
    }
    std::printf("[Phase 1] OK — no SpeechStart during silence\n");

    // ---- Phase 2: Push 50 frames of loud audio ------------------------------
    std::printf("[Phase 2] Pushing 50 loud frames...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, /*loud=*/true);
        inQueue.push(std::move(frame));
    }
    // Give the VAD time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Check: SpeechStart should have been emitted
    bool gotSpeechStart = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechStart) {
            gotSpeechStart = true;
            std::printf("[Phase 2] SpeechStart received (conf=%.2f)\n",
                        ev.confidence);
        }
    }
    if (!gotSpeechStart) {
        std::fprintf(stderr, "[Phase 2] FAIL: No SpeechStart during loud audio!\n");
        inQueue.stop();
        node.stop();
        std::fprintf(stderr, "\n>>> FAIL — test_vad_node <<<\n");
        return 1;
    }
    std::printf("[Phase 2] OK — SpeechStart detected\n");

    // Verify node reports speaking state
    if (!node.isSpeaking()) {
        std::fprintf(stderr, "[Phase 2] FAIL: isSpeaking() returned false during speech!\n");
        inQueue.stop();
        node.stop();
        std::fprintf(stderr, "\n>>> FAIL — test_vad_node <<<\n");
        return 1;
    }
    std::printf("[Phase 2] OK — isSpeaking() = true\n");

    // ---- Phase 3: Push many frames of silence (enough for hangover) ---------
    // hangoverChunks=5, each chunk is 4 frames → need > 20 silent frames
    // Push 50 to be safe.
    std::printf("[Phase 3] Pushing 50 silent frames (waiting for hangover)...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, /*loud=*/false);
        inQueue.push(std::move(frame));
    }
    // Wait for hangover to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    bool gotSpeechEnd = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechEnd) {
            gotSpeechEnd = true;
            std::printf("[Phase 3] SpeechEnd received (conf=%.2f)\n",
                        ev.confidence);
        }
    }
    if (!gotSpeechEnd) {
        std::fprintf(stderr, "[Phase 3] FAIL: No SpeechEnd after silence hangover!\n");
        inQueue.stop();
        node.stop();
        std::fprintf(stderr, "\n>>> FAIL — test_vad_node <<<\n");
        return 1;
    }
    std::printf("[Phase 3] OK — SpeechEnd detected after hangover\n");

    // ---- Shutdown -----------------------------------------------------------
    inQueue.stop();
    outQueue.stop();
    eventQueue.stop();
    node.stop();
    inQueue.clear();
    outQueue.clear();
    eventQueue.clear();

    (void)samplesPerFrame;

    std::printf("\n>>> PASS — test_vad_node <<<\n");
    return 0;
}
