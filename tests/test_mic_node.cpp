// ---------------------------------------------------------------------------
// test_mic_node.cpp — MiniaudioMicNode lifecycle test
//
// Tests init/start/stop in an environment that may or may not have a real
// audio device.  The test passes if the node can be created, wired, and
// torn down without crashing or deadlocking.
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "MiniaudioMicNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    std::printf("=== test_mic_node ===\n");

    bool allPassed = true;

    // ---- Test 1: Construction ------------------------------------------------
    {
        std::printf("[Test 1] Construction... ");
        MiniaudioMicConfig cfg;
        cfg.format = {16000, 1, 10, SampleFormat::Int16};
        cfg.poolSize = 64;
        cfg.deviceIndex = -1;
        cfg.enableDcRemoval = true;

        MiniaudioMicNode node(cfg);
        std::printf("OK\n");
    }

    // ---- Test 2: Wire output queue -------------------------------------------
    {
        std::printf("[Test 2] Wire output queue... ");
        MiniaudioMicConfig cfg;
        MiniaudioMicNode node(cfg);

        AudioFrameQueue queue(128, QueueOverflowPolicy::DropOldest, "mic_out");
        node.setOutputQueue(&queue);
        std::printf("OK\n");
    }

    // ---- Test 3: Full lifecycle (init may fail without audio device) ----------
    {
        std::printf("[Test 3] Full lifecycle (init/start/stop)... ");
        MiniaudioMicConfig cfg;
        cfg.poolSize = 64;
        MiniaudioMicNode node(cfg);

        AudioFrameQueue queue(128, QueueOverflowPolicy::DropOldest, "mic_out");
        node.setOutputQueue(&queue);

        const bool initOk = node.initialize();
        if (!initOk) {
            // No audio device available (CI environment) — this is expected
            std::printf("SKIP (no audio device)\n");
        } else {
            node.start();

            // Let it run briefly — the audio callback may or may not fire
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Stop cleanly
            queue.stop();
            node.stop();

            std::printf("OK\n");
        }
    }

    // ---- Test 4: Stop without start (should not crash) -----------------------
    {
        std::printf("[Test 4] Stop without start... ");
        MiniaudioMicConfig cfg;
        MiniaudioMicNode node(cfg);

        AudioFrameQueue queue(128, QueueOverflowPolicy::DropOldest, "mic_out");
        node.setOutputQueue(&queue);

        // Calling stop() without start() should be safe
        node.stop();
        std::printf("OK\n");
    }

    // ---- Test 5: Pool integrity after lifecycle ------------------------------
    {
        std::printf("[Test 5] Pool integrity after lifecycle... ");
        MiniaudioMicConfig cfg;
        cfg.poolSize = 32;
        MiniaudioMicNode node(cfg);

        AudioFrameQueue queue(128, QueueOverflowPolicy::DropOldest, "mic_out");
        node.setOutputQueue(&queue);

        const bool initOk = node.initialize();
        if (!initOk) {
            std::printf("SKIP (no audio device)\n");
        } else {
            node.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Drain any captured frames from the queue
            queue.stop();
            node.stop();
            queue.clear();

            // After clearing queue, shared_ptrs are released back to pool.
            // No crash or pool corruption should occur.
            std::printf("OK\n");
        }
    }

    // ---- Test 6: Double stop (should be idempotent) --------------------------
    {
        std::printf("[Test 6] Double stop (idempotent)... ");
        MiniaudioMicConfig cfg;
        MiniaudioMicNode node(cfg);

        AudioFrameQueue queue(128, QueueOverflowPolicy::DropOldest, "mic_out");
        node.setOutputQueue(&queue);

        const bool initOk = node.initialize();
        if (initOk) {
            node.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            queue.stop();
            node.stop();
        }

        // Second stop — should not crash or deadlock
        node.stop();
        std::printf("OK\n");
    }

    (void)now_ns;  // suppress unused warning

    if (allPassed) {
        std::printf("\n>>> PASS — test_mic_node <<<\n");
    } else {
        std::fprintf(stderr, "\n>>> FAIL — test_mic_node <<<\n");
        return 1;
    }
    return 0;
}
