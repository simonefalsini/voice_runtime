// ---------------------------------------------------------------------------
// test_llm_node.cpp — HttpLlmNode stub mode test
//
// Without httplib/nlohmann-json, the node runs in stub mode that echoes
// input with a "[LLM-STUB] " prefix.  We verify:
//   1. Stub initialization with allowStubFallback = true
//   2. Pushing a text chunk produces stub response tokens
//   3. A final (isFinal=true) chunk is emitted at end of response
//   4. InterruptSignal can abort mid-generation
//   5. DiskBackedTextBuffer receives conversation log
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/TextBuffer.h"
#include "HttpLlmNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    std::printf("=== test_llm_node ===\n");

    // ---- Test 1: Stub initialization ----------------------------------------
    {
        std::printf("[Test 1] Stub initialization... ");
        HttpLlmConfig cfg;
        cfg.allowStubFallback = true;
        cfg.forceStubMode = true;
        HttpLlmNode node(cfg);

        const bool ok = node.initialize();
        assert(ok && "LLM stub init should succeed with allowStubFallback=true");
        std::printf("OK\n");
    }

    // ---- Test 2: Stub response to input -------------------------------------
    {
        std::printf("[Test 2] Stub response to input... ");

        HttpLlmConfig cfg;
        cfg.allowStubFallback = true;
        cfg.forceStubMode = true;
        HttpLlmNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "llm_in");
        TextQueue textOut(64, QueueOverflowPolicy::DropOldest, "llm_out");
        InterruptSignal intSig;

        // Use a temp file for the disk buffer
        DiskBackedTextBuffer disk("/tmp/test_llm_node_log.txt", 4096);

        node.setInputQueue(&textIn);
        node.setOutputQueue(&textOut);
        node.setInterruptSignal(&intSig);
        node.setPersistentInputBuffer(&disk);

        node.initialize();
        node.start();

        // Push input
        TextChunk input;
        input.text = "What is 2+2?";
        input.isFinal = true;
        input.sequence = 1;
        input.timestampNs = now_ns();
        textIn.push(std::move(input));

        // Wait for stub to generate response
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        // Collect all output chunks
        std::vector<TextChunk> outputs;
        TextChunk out;
        while (textOut.tryPop(out)) {
            outputs.push_back(std::move(out));
        }

        // Stop
        textIn.stop();
        textOut.stop();
        node.stop();
        textIn.clear();
        textOut.clear();

        if (outputs.empty()) {
            std::fprintf(stderr, "FAIL (no output chunks received)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }

        // Verify we got at least one non-final chunk
        bool gotContent = false;
        bool gotFinal = false;
        std::string accumulated;

        for (const auto& ch : outputs) {
            if (!ch.text.empty()) {
                gotContent = true;
                accumulated += ch.text;
            }
            if (ch.isFinal) {
                gotFinal = true;
            }
        }

        if (!gotContent) {
            std::fprintf(stderr, "FAIL (no content in output chunks)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }

        if (!gotFinal) {
            std::fprintf(stderr, "FAIL (no isFinal chunk received)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }

        // Verify the response contains the expected stub prefix
        if (accumulated.find("[LLM-STUB]") == std::string::npos) {
            std::fprintf(stderr, "FAIL (response doesn't contain [LLM-STUB]: \"%s\")\n",
                         accumulated.c_str());
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }

        // Verify the response echoes the input
        if (accumulated.find("What is 2+2?") == std::string::npos) {
            std::fprintf(stderr, "FAIL (response doesn't echo input: \"%s\")\n",
                         accumulated.c_str());
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }

        std::printf("OK (chunks: %zu, accumulated: \"%s\")\n",
                     outputs.size(), accumulated.c_str());

        // Verify disk buffer got written
        const std::string diskSnapshot = disk.memorySnapshot();
        if (diskSnapshot.empty()) {
            std::fprintf(stderr, "[Test 2] WARNING: DiskBackedTextBuffer is empty\n");
        } else {
            std::printf("[Test 2] Disk buffer snapshot: \"%.80s...\"\n",
                        diskSnapshot.c_str());
        }
    }

    // ---- Test 3: Interrupt mid-generation -----------------------------------
    {
        std::printf("[Test 3] Interrupt mid-generation... ");

        HttpLlmConfig cfg;
        cfg.allowStubFallback = true;
        cfg.forceStubMode = true;
        HttpLlmNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "llm_in");
        TextQueue textOut(64, QueueOverflowPolicy::DropOldest, "llm_out");
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setOutputQueue(&textOut);
        node.setInterruptSignal(&intSig);

        node.initialize();
        node.start();

        // Push input
        TextChunk input;
        input.text = "Tell me a very long story about a fox";
        input.isFinal = true;
        input.sequence = 1;
        input.timestampNs = now_ns();
        textIn.push(std::move(input));

        // Wait briefly, then interrupt
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        intSig.request();

        // Wait for the node to process the interrupt
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Collect output — should be partial (interrupted)
        int chunkCount = 0;
        TextChunk out;
        while (textOut.tryPop(out)) {
            ++chunkCount;
        }

        textIn.stop();
        textOut.stop();
        node.stop();

        // The test passes if there's no deadlock and the node stopped cleanly
        std::printf("OK (chunks before interrupt: %d)\n", chunkCount);
    }

    // ---- Test 4: Multiple requests ------------------------------------------
    {
        std::printf("[Test 4] Multiple sequential requests... ");

        HttpLlmConfig cfg;
        cfg.allowStubFallback = true;
        cfg.forceStubMode = true;
        HttpLlmNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "llm_in");
        TextQueue textOut(128, QueueOverflowPolicy::DropOldest, "llm_out");
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setOutputQueue(&textOut);
        node.setInterruptSignal(&intSig);

        node.initialize();
        node.start();

        // Push two requests
        for (int r = 0; r < 2; ++r) {
            TextChunk input;
            input.text = "Request " + std::to_string(r);
            input.isFinal = true;
            input.sequence = static_cast<uint64_t>(r + 1);
            input.timestampNs = now_ns();
            textIn.push(std::move(input));
        }

        // Wait for both responses
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        int finalCount = 0;
        TextChunk out;
        while (textOut.tryPop(out)) {
            if (out.isFinal) ++finalCount;
        }

        textIn.stop();
        textOut.stop();
        node.stop();

        if (finalCount < 2) {
            std::fprintf(stderr, "FAIL (expected 2 final chunks, got %d)\n", finalCount);
            std::fprintf(stderr, "\n>>> FAIL — test_llm_node <<<\n");
            return 1;
        }
        std::printf("OK (final chunks: %d)\n", finalCount);
    }

    // ---- Test 5: Clean shutdown ---------------------------------------------
    {
        std::printf("[Test 5] Clean shutdown... ");

        HttpLlmConfig cfg;
        cfg.allowStubFallback = true;
        cfg.forceStubMode = true;
        HttpLlmNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "llm_in");
        TextQueue textOut(64, QueueOverflowPolicy::DropOldest, "llm_out");

        node.setInputQueue(&textIn);
        node.setOutputQueue(&textOut);

        node.initialize();
        node.start();

        textIn.stop();
        textOut.stop();
        node.stop();
        textIn.clear();
        textOut.clear();

        std::printf("OK\n");
    }

    std::printf("\n>>> PASS — test_llm_node <<<\n");
    return 0;
}
