// ---------------------------------------------------------------------------
// test_interpreter_node.cpp — RealInterpreterNode think-tag filtering
//
// Tests the streaming tag filter state machine:
//   1. Plain text passes through unchanged
//   2. <think>...</think> blocks are removed
//   3. Split tags across chunk boundaries are handled correctly
//   4. [TOOL_CALL] prefixed chunks are suppressed
//   5. <tool_calls>...</tool_calls> blocks are removed
//   6. [lang=XX] markers are preserved
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
#include "RealInterpreterNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Helper: push a text chunk, optionally marking as final
static void pushChunk(TextQueue& q, const std::string& text, bool isFinal,
                      uint64_t seq) {
    TextChunk chunk;
    chunk.text = text;
    chunk.isFinal = isFinal;
    chunk.sequence = seq;
    chunk.timestampNs = now_ns();
    q.push(std::move(chunk));
}

// Helper: collect all output chunks within a timeout, return concatenated text
static std::string collectOutput(TextQueue& q, int timeoutMs,
                                 std::vector<TextChunk>* allChunks = nullptr) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    std::string result;
    TextChunk out;
    while (q.tryPop(out)) {
        result += out.text;
        if (allChunks) allChunks->push_back(std::move(out));
    }
    return result;
}

int main() {
    std::printf("=== test_interpreter_node ===\n");

    int testNum = 0;
    int failCount = 0;

    auto checkTest = [&](const char* name, const std::string& got,
                         const std::string& expected) {
        ++testNum;
        if (got == expected) {
            std::printf("[Test %d] %s... OK\n", testNum, name);
        } else {
            std::fprintf(stderr, "[Test %d] %s... FAIL\n", testNum, name);
            std::fprintf(stderr, "  Expected: \"%s\"\n", expected.c_str());
            std::fprintf(stderr, "  Got:      \"%s\"\n", got.c_str());
            ++failCount;
        }
    };

    // ---- Test 1: Plain text passthrough -------------------------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;  // Disable LaTeX conversion for clean testing
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "Hello world", true, 1);

        std::string result = collectOutput(out, 200);

        in.stop();
        out.stop();
        node.stop();

        checkTest("Plain text passthrough", result, "Hello world");
    }

    // ---- Test 2: <think>reasoning</think>Hello ------------------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "<think>reasoning</think>Hello", true, 1);

        std::string result = collectOutput(out, 200);

        in.stop();
        out.stop();
        node.stop();

        checkTest("Think tag removal (single chunk)", result, "Hello");
    }

    // ---- Test 3: Streaming split think tags ---------------------------------
    // Chunks: "<th", "ink>thinking</thi", "nk>Answer"
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        uint64_t seq = 0;
        pushChunk(in, "<th", false, ++seq);
        // Small delay to ensure processing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pushChunk(in, "ink>thinking</thi", false, ++seq);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pushChunk(in, "nk>Answer", true, ++seq);

        std::string result = collectOutput(out, 300);

        in.stop();
        out.stop();
        node.stop();

        checkTest("Streaming split think tags", result, "Answer");
    }

    // ---- Test 4: [TOOL_CALL]{...} suppression -------------------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "[TOOL_CALL]{\"name\":\"search\",\"args\":{\"q\":\"test\"}}", true, 1);

        std::vector<TextChunk> allChunks;
        std::string result = collectOutput(out, 200, &allChunks);

        in.stop();
        out.stop();
        node.stop();

        // [TOOL_CALL] lines should be completely suppressed.
        // However, the final chunk marker may still produce an empty final chunk.
        // Filter out empty strings.
        std::string nonEmpty;
        for (const auto& ch : allChunks) {
            nonEmpty += ch.text;
        }

        checkTest("[TOOL_CALL] suppression", nonEmpty, "");
    }

    // ---- Test 5: <tool_calls>some tool</tool_calls>Result -------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "<tool_calls>some tool</tool_calls>Result", true, 1);

        std::string result = collectOutput(out, 200);

        in.stop();
        out.stop();
        node.stop();

        checkTest("<tool_calls> block removal", result, "Result");
    }

    // ---- Test 6: [lang=it] marker preserved ---------------------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "[lang=it]Ciao mondo", true, 1);

        std::string result = collectOutput(out, 200);

        in.stop();
        out.stop();
        node.stop();

        // [lang=XX] markers are NOT filtered by the interpreter — they pass
        // through to the TTS node which consumes them.
        checkTest("[lang=it] marker preserved", result, "[lang=it]Ciao mondo");
    }

    // ---- Test 7: Multiple think blocks in one chunk -------------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "<think>first</think>Hello <think>second</think>World", true, 1);

        std::string result = collectOutput(out, 200);

        in.stop();
        out.stop();
        node.stop();

        checkTest("Multiple think blocks", result, "Hello World");
    }

    // ---- Test 8: Empty text with isFinal passes through ---------------------
    {
        InterpreterConfig cfg;
        cfg.convertLatex = false;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        pushChunk(in, "", true, 1);

        std::vector<TextChunk> chunks;
        collectOutput(out, 200, &chunks);

        in.stop();
        out.stop();
        node.stop();

        // An empty final chunk should still be forwarded so TTS can detect end-of-turn
        bool gotFinal = false;
        for (const auto& ch : chunks) {
            if (ch.isFinal) gotFinal = true;
        }
        ++testNum;
        if (gotFinal) {
            std::printf("[Test %d] Empty final chunk forwarded... OK\n", testNum);
        } else {
            std::fprintf(stderr, "[Test %d] Empty final chunk forwarded... FAIL\n", testNum);
            ++failCount;
        }
    }

    // ---- Test 9: Clean shutdown without deadlock -----------------------------
    {
        ++testNum;
        std::printf("[Test %d] Clean shutdown... ", testNum);

        InterpreterConfig cfg;
        RealInterpreterNode node(cfg);

        TextQueue in(32, QueueOverflowPolicy::BlockProducer, "interp_in");
        TextQueue out(32, QueueOverflowPolicy::DropOldest, "interp_out");

        node.setInputQueue(&in);
        node.setOutputQueue(&out);
        node.initialize();
        node.start();

        in.stop();
        out.stop();
        node.stop();
        in.clear();
        out.clear();

        std::printf("OK\n");
    }

    // ---- Summary ------------------------------------------------------------
    if (failCount == 0) {
        std::printf("\n>>> PASS — test_interpreter_node (%d tests) <<<\n", testNum);
        return 0;
    } else {
        std::fprintf(stderr, "\n>>> FAIL — test_interpreter_node (%d/%d failed) <<<\n",
                     failCount, testNum);
        return 1;
    }
}
