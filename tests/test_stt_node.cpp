// ---------------------------------------------------------------------------
// test_stt_node.cpp — Qwen3SttNode stub mode test
//
// Without qwen3_asr.h, the node operates in stub mode emitting
// "[STT stub: N samples received]".  We verify:
//   1. Node initializes in stub mode
//   2. Pushing audio frames accumulates in the speech buffer
//   3. After transcriptionTimeoutMs with no new frames, a text chunk is emitted
//   4. The output contains the expected stub format
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"
#include "Qwen3SttNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static AudioFrameHandle makeFrame(SharedBufferPool<AudioFrame>& pool,
                                   const AudioFormat& fmt,
                                   uint64_t seq) {
    auto frame = pool.acquireWithTimeout(std::chrono::milliseconds(100));
    assert(frame && "Pool exhausted");

    frame->format = fmt;
    frame->resizeForFormat();
    frame->sequence = seq;
    frame->timestampNs = now_ns();

    // Fill with a simple pattern
    const int samples = fmt.totalSamplesPerFrame();
    for (int i = 0; i < samples; ++i) {
        frame->pcm16[static_cast<std::size_t>(i)] = static_cast<int16_t>(i % 1000);
    }
    return frame;
}

int main() {
    std::printf("=== test_stt_node ===\n");
    LibraryLogRedirector::instance().start();

    const AudioFormat fmt{16000, 1, 10, SampleFormat::Int16};
    const int samplesPerFrame = fmt.totalSamplesPerFrame(); // 160

    // ---- Test 1: Initialization in stub mode --------------------------------
    {
        std::printf("[Test 1] Initialization in stub mode... ");
        Qwen3SttConfig cfg;
        cfg.transcriptionTimeoutMs = 200;
        cfg.minSpeechSamples = 100;  // Low threshold for testing

        Qwen3SttNode node(cfg);
        const bool ok = node.initialize();
        assert(ok && "STT stub init should succeed");
        std::printf("OK\n");
    }

    // ---- Test 2: Push frames and receive stub transcription ------------------
    {
        std::printf("[Test 2] Push 50 frames, wait for stub transcription... ");

        Qwen3SttConfig cfg;
        cfg.transcriptionTimeoutMs = 300;
        cfg.minSpeechSamples = 100;  // Low for quick test

        Qwen3SttNode node(cfg);

        AudioFrameQueue inQueue(256, QueueOverflowPolicy::DropOldest, "stt_in");
        TextQueue outQueue(64, QueueOverflowPolicy::DropOldest, "stt_out");
        SharedBufferPool<AudioFrame> pool(256);

        node.setInputQueue(&inQueue);
        node.setOutputQueue(&outQueue);

        const bool initOk = node.initialize();
        assert(initOk);

        node.start();

        // Push 50 frames — 50 × 160 = 8000 samples total
        for (int i = 0; i < 50; ++i) {
            auto frame = makeFrame(pool, fmt, static_cast<uint64_t>(i));
            inQueue.push(std::move(frame));
        }

        // Wait for timeout to trigger transcription
        // (timeout is 300ms, so wait a bit longer)
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // Check output
        TextChunk result;
        bool got = outQueue.tryPop(result);

        if (!got) {
            // The node might still be processing — wait a bit more
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            got = outQueue.tryPop(result);
        }

        // Stop the node
        inQueue.stop();
        outQueue.stop();
        node.stop();
        inQueue.clear();
        outQueue.clear();

        if (!got) {
            std::fprintf(stderr, "FAIL (no output received)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_stt_node <<<\n");
            return 1;
        }

        // Verify the stub output format
        const std::size_t expectedSamples = 50 * static_cast<std::size_t>(samplesPerFrame);
        const std::string expectedPrefix = "[STT stub:";

        if (result.text.find(expectedPrefix) == std::string::npos) {
            std::fprintf(stderr, "FAIL (unexpected text: \"%s\")\n",
                         result.text.c_str());
            std::fprintf(stderr, "\n>>> FAIL — test_stt_node <<<\n");
            return 1;
        }

        assert(result.isFinal && "Stub output should be marked as final");

        std::printf("OK (got: \"%s\")\n", result.text.c_str());
        (void)expectedSamples;
    }

    // ---- Test 3: Minimum samples threshold ----------------------------------
    {
        std::printf("[Test 3] Minimum samples threshold... ");

        Qwen3SttConfig cfg;
        cfg.transcriptionTimeoutMs = 200;
        cfg.minSpeechSamples = 100000;  // Very high — should suppress output

        Qwen3SttNode node(cfg);

        AudioFrameQueue inQueue(256, QueueOverflowPolicy::DropOldest, "stt_in");
        TextQueue outQueue(64, QueueOverflowPolicy::DropOldest, "stt_out");
        SharedBufferPool<AudioFrame> pool(256);

        node.setInputQueue(&inQueue);
        node.setOutputQueue(&outQueue);
        node.initialize();
        node.start();

        // Push only 10 frames (1600 samples < 100000 min)
        for (int i = 0; i < 10; ++i) {
            auto frame = makeFrame(pool, fmt, static_cast<uint64_t>(i));
            inQueue.push(std::move(frame));
        }

        // Wait for timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        TextChunk result;
        const bool got = outQueue.tryPop(result);

        inQueue.stop();
        outQueue.stop();
        node.stop();
        inQueue.clear();
        outQueue.clear();

        // Should NOT have received output (below min threshold)
        if (got) {
            std::fprintf(stderr, "FAIL (got output below min threshold)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_stt_node <<<\n");
            return 1;
        }
        std::printf("OK (no output below min threshold)\n");
    }

    // ---- Test 4: Clean shutdown without deadlock -----------------------------
    {
        std::printf("[Test 4] Clean shutdown without deadlock... ");

        Qwen3SttConfig cfg;
        cfg.transcriptionTimeoutMs = 100;

        Qwen3SttNode node(cfg);

        AudioFrameQueue inQueue(256, QueueOverflowPolicy::DropOldest, "stt_in");
        TextQueue outQueue(64, QueueOverflowPolicy::DropOldest, "stt_out");

        node.setInputQueue(&inQueue);
        node.setOutputQueue(&outQueue);
        node.initialize();
        node.start();

        // Stop immediately — should not deadlock
        inQueue.stop();
        outQueue.stop();
        node.stop();
        inQueue.clear();
        outQueue.clear();

        std::printf("OK\n");
    }

    std::printf("\n>>> PASS — test_stt_node <<<\n");
    LibraryLogRedirector::instance().stop();
    return 0;
}
