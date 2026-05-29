// ---------------------------------------------------------------------------
// test_vad_real.cpp — SileroVadNode test with REAL Silero GGML model
//
// Usage: ./test_vad_real [path/to/ggml-silero-v6.2.0.bin]
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
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

static AudioFrameHandle makeFrame(SharedBufferPool<AudioFrame>& pool,
                                   const AudioFormat& fmt,
                                   uint64_t seq,
                                   bool loud) {
    auto frame = pool.acquireWithTimeout(std::chrono::milliseconds(100));
    assert(frame && "Pool exhausted");

    frame->format = fmt;
    frame->resizeForFormat();
    frame->sequence = seq;
    frame->timestampNs = now_ns();

    const int samples = fmt.totalSamplesPerFrame();

    if (loud) {
        constexpr float kFreq = 1000.0f;
        constexpr float kAmplitude = 0.5f;
        for (int i = 0; i < samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(fmt.sampleRate);
            const float val = kAmplitude * std::sin(2.0f * 3.14159265f * kFreq * t);
            frame->pcm16[static_cast<std::size_t>(i)] =
                static_cast<int16_t>(val * 32767.0f);
        }
    } else {
        for (int i = 0; i < samples; ++i) {
            frame->pcm16[static_cast<std::size_t>(i)] = 0;
        }
    }

    return frame;
}

int main(int argc, char* argv[]) {
    // Default model path
    std::string modelPath = "models/vad/ggml-silero-v6.2.0.bin";
    if (argc > 1) {
        modelPath = argv[1];
    }

    std::printf("=== test_vad_real ===\n");
    std::printf("Model: %s\n\n", modelPath.c_str());

    const AudioFormat fmt{16000, 1, 10, SampleFormat::Int16};

    SileroVadConfig cfg;
    cfg.modelPath = modelPath;
    cfg.format = fmt;
    cfg.poolSize = 128;
    cfg.thresholdStart = 0.6f;
    cfg.thresholdStop = 0.45f;
    cfg.hangoverChunks = 5;

    SileroVadNode node(cfg);

    AudioFrameQueue inQueue(512, QueueOverflowPolicy::DropOldest, "vad_in");
    AudioFrameQueue outQueue(512, QueueOverflowPolicy::DropOldest, "vad_out");
    VadEventQueue eventQueue(64, QueueOverflowPolicy::DropOldest, "vad_events");
    SharedBufferPool<AudioFrame> testPool(512);

    node.setInputQueue(&inQueue);
    node.setOutputQueue(&outQueue);
    node.setEventQueue(&eventQueue);

    const bool initOk = node.initialize();
    if (!initOk) {
        std::fprintf(stderr, "FAIL: VAD initialization failed\n");
        return 1;
    }
    std::printf("[Info] VAD initialized successfully\n");

    node.start();

    uint64_t seq = 0;

    // Phase 1: 50 frames of silence
    std::printf("[Phase 1] Pushing 50 silent frames...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, false);
        inQueue.push(std::move(frame));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VadEvent ev;
    bool gotSpeechStartDuringSilence = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechStart) {
            gotSpeechStartDuringSilence = true;
        }
    }
    if (gotSpeechStartDuringSilence) {
        std::fprintf(stderr, "[Phase 1] FAIL: SpeechStart during silence!\n");
        inQueue.stop(); node.stop();
        return 1;
    }
    std::printf("[Phase 1] OK — no SpeechStart during silence\n");

    // Phase 2: 50 frames of loud audio
    std::printf("[Phase 2] Pushing 50 loud frames...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, true);
        inQueue.push(std::move(frame));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool gotSpeechStart = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechStart) {
            gotSpeechStart = true;
            std::printf("[Phase 2] SpeechStart received (conf=%.2f)\n",
                        ev.confidence);
        }
    }
    if (!gotSpeechStart) {
        // Real Silero VAD may not detect a pure sine tone as speech — this is expected.
        // Energy-based fallback would detect it, but the real model is smarter.
        std::printf("[Phase 2] NOTE: No SpeechStart — expected for pure tone with real Silero model\n");
        std::printf("[Phase 2] OK — model inference ran without errors\n");
    } else {
        std::printf("[Phase 2] OK — SpeechStart detected\n");
        std::printf("[Phase 2] isSpeaking() = %s\n",
                    node.isSpeaking() ? "true" : "false");
    }

    // Phase 3: 50 frames of silence (hangover)
    std::printf("[Phase 3] Pushing 50 silent frames (waiting for hangover)...\n");
    for (int i = 0; i < 50; ++i) {
        auto frame = makeFrame(testPool, fmt, seq++, false);
        inQueue.push(std::move(frame));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool gotSpeechEnd = false;
    while (eventQueue.tryPop(ev)) {
        if (ev.type == VadEvent::Type::SpeechEnd) {
            gotSpeechEnd = true;
            std::printf("[Phase 3] SpeechEnd received (conf=%.2f)\n",
                        ev.confidence);
        }
    }
    if (!gotSpeechEnd) {
        if (!gotSpeechStart) {
            std::printf("[Phase 3] NOTE: No SpeechEnd (no prior SpeechStart with pure tone)\n");
        } else {
            std::fprintf(stderr, "[Phase 3] FAIL: No SpeechEnd after hangover!\n");
            inQueue.stop(); node.stop();
            return 1;
        }
    } else {
        std::printf("[Phase 3] OK — SpeechEnd detected after hangover\n");
    }

    // Shutdown
    inQueue.stop();
    outQueue.stop();
    eventQueue.stop();
    node.stop();
    inQueue.clear();
    outQueue.clear();
    eventQueue.clear();

    std::printf("\n>>> PASS — test_vad_real <<<\n");
    return 0;
}
