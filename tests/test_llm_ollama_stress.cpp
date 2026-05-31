// ---------------------------------------------------------------------------
// test_llm_ollama_stress.cpp — Stress test for HttpLlmNode with Ollama
//
// Sends 40 sequential requests (simple arithmetic and capital city questions)
// to verify streaming, thinking (CoT) parsing, and response correctness.
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/TextBuffer.h"
#include "HttpLlmNode.h"

using namespace voice_runtime;

// ─── ANSI colors ─────────────────────────────────────────────────────────────
static constexpr const char* kReset   = "\033[0m";
static constexpr const char* kBold    = "\033[1m";
static constexpr const char* kGreen   = "\033[32m";
static constexpr const char* kYellow  = "\033[33m";
static constexpr const char* kCyan    = "\033[36m";
static constexpr const char* kGray    = "\033[90m";
static constexpr const char* kRed     = "\033[31m";

struct TestCase {
    std::string prompt;
    std::vector<std::string> expectedKeywords;
};

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return s;
}

static bool containsKeyword(const std::string& text, const std::vector<std::string>& keywords) {
    std::string lowerText = toLower(text);
    for (const auto& kw : keywords) {
        if (lowerText.find(toLower(kw)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::string getEnv(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

int main() {
    const std::string llmBaseUrl = getEnv("LLM_BASE_URL", "http://127.0.0.1:11434");
    const std::string llmApiPath = getEnv("LLM_API_PATH", "/v1/chat/completions");
    const std::string llmModel   = getEnv("LLM_MODEL",    "qwen3.6:35b");

    std::printf("\n%s%s╔══════════════════════════════════════════════════════════╗%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s║  🧠  HttpLlmNode Ollama Stress Test — 40 Requests        ║%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s╚══════════════════════════════════════════════════════════╝%s\n\n",
                kBold, kCyan, kReset);
    std::printf("  LLM endpoint: %s%s%s%s\n", kBold, llmBaseUrl.c_str(), llmApiPath.c_str(), kReset);
    std::printf("  Model:        %s%s%s\n\n", kYellow, llmModel.c_str(), kReset);

    // Define 40 test cases: 20 simple math and 20 capitals
    std::vector<TestCase> testCases = {
        // --- 20 Math Questions ---
        { "Quanto fa 2 + 2? Rispondi solo con il numero.", {"4"} },
        { "Quanto fa 5 + 5? Rispondi solo con il numero.", {"10"} },
        { "Quanto fa 10 + 15? Rispondi solo con il numero.", {"25"} },
        { "Quanto fa 20 + 30? Rispondi solo con il numero.", {"50"} },
        { "Quanto fa 50 + 50? Rispondi solo con il numero.", {"100"} },
        { "Quanto fa 7 - 3? Rispondi solo con il numero.", {"4"} },
        { "Quanto fa 15 - 5? Rispondi solo con il numero.", {"10"} },
        { "Quanto fa 50 - 20? Rispondi solo con il numero.", {"30"} },
        { "Quanto fa 100 - 1? Rispondi solo con il numero.", {"99"} },
        { "Quanto fa 3 * 3? Rispondi solo con il numero.", {"9"} },
        { "Quanto fa 4 * 5? Rispondi solo con il numero.", {"20"} },
        { "Quanto fa 6 * 7? Rispondi solo con il numero.", {"42"} },
        { "Quanto fa 10 * 10? Rispondi solo con il numero.", {"100"} },
        { "Quanto fa 8 / 2? Rispondi solo con il numero.", {"4"} },
        { "Quanto fa 20 / 4? Rispondi solo con il numero.", {"5"} },
        { "Quanto fa 100 / 10? Rispondi solo con il numero.", {"10"} },
        { "Quanto fa 12 + 8? Rispondi solo con il numero.", {"20"} },
        { "Quanto fa 9 - 6? Rispondi solo con il numero.", {"3"} },
        { "Quanto fa 7 * 8? Rispondi solo con il numero.", {"56"} },
        { "Quanto fa 144 / 12? Rispondi solo con il numero.", {"12"} },

        // --- 20 Capital Cities ---
        { "Qual è la capitale della Francia?", {"paris", "parigi"} },
        { "Qual è la capitale dell'Italia?", {"roma", "rome"} },
        { "Qual è la capitale della Germania?", {"berlin", "berlino"} },
        { "Qual è la capitale del Regno Unito?", {"london", "londra"} },
        { "Qual è la capitale della Spagna?", {"madrid"} },
        { "Qual è la capitale del Giappone?", {"tokyo"} },
        { "Qual è la capitale degli Stati Uniti?", {"washington"} },
        { "Qual è la capitale dell'Egitto?", {"cairo"} },
        { "Qual è la capitale della Russia?", {"mosca", "moscow"} },
        { "Qual è la capitale della Cina?", {"pechino", "beijing"} },
        { "Qual è la capitale dell'India?", {"delhi", "nuova delhi"} },
        { "Qual è la capitale dell'Australia?", {"canberra"} },
        { "Qual è la capitale del Canada?", {"ottawa"} },
        { "Qual è la capitale del Brasile?", {"brasilia"} },
        { "Qual è la capitale del Portogallo?", {"lisbon", "lisbona"} },
        { "Qual è la capitale della Grecia?", {"atene", "athens"} },
        { "Qual è la capitale della Turchia?", {"ankara"} },
        { "Qual è la capitale dell'Olanda?", {"amsterdam"} },
        { "Qual è la capitale del Belgio?", {"brussels", "bruxelles"} },
        { "Qual è la capitale dell'Austria?", {"vienna"} }
    };

    // Configure the HttpLlmNode
    HttpLlmConfig cfg;
    cfg.baseUrl = llmBaseUrl;
    cfg.apiPath = llmApiPath;
    cfg.model = llmModel;
    cfg.stream = true;
    cfg.temperature = 0.0f; // low temperature for deterministic answers
    cfg.connectTimeoutSec = 5;
    cfg.readTimeoutSec = 60;
    cfg.allowStubFallback = false; // we want to test the real connection

    HttpLlmNode node(cfg);
    if (!node.initialize()) {
        std::fprintf(stderr, "%s[ERROR] Failed to initialize HttpLlmNode. Make sure Ollama is running.%s\n", kRed, kReset);
        return 1;
    }

    TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "llm_in");
    TextQueue textOut(1024, QueueOverflowPolicy::DropOldest, "llm_out");

    node.setInputQueue(&textIn);
    node.setOutputQueue(&textOut);

    node.start();

    int passed = 0;
    for (size_t i = 0; i < testCases.size(); ++i) {
        const auto& tc = testCases[i];
        std::printf("%s[Case %zu/%zu] %s\"%s\"%s\n", kBold, i + 1, testCases.size(), kCyan, tc.prompt.c_str(), kReset);
        std::fflush(stdout);

        // Push request
        TextChunk input;
        input.text = tc.prompt;
        input.isFinal = true;
        input.sequence = i + 1;
        input.timestampNs = now_ns();
        textIn.push(std::move(input));

        // Read streaming chunks
        std::string accumulatedClean;
        std::string accumulatedThink;
        bool done = false;

        while (!done) {
            TextChunk chunk;
            if (textOut.pop(chunk)) {
                if (chunk.isFinal) {
                    done = true;
                }
                if (!chunk.text.empty()) {
                    // Check if reasoning chunk (wrapped in <think>...</think>)
                    if (chunk.text.rfind("<think>", 0) == 0 && chunk.text.size() >= 15 && 
                        chunk.text.substr(chunk.text.size() - 8) == "</think>") {
                        accumulatedThink += chunk.text.substr(7, chunk.text.size() - 15);
                    } else {
                        accumulatedClean += chunk.text;
                    }
                }
            } else {
                break;
            }
        }

        // Clean up whitespace/newlines at start/end
        auto trim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), s.end());
        };
        trim(accumulatedClean);
        trim(accumulatedThink);

        if (!accumulatedThink.empty()) {
            std::printf("  %sThinking: \"%s\"%s\n", kGray, accumulatedThink.c_str(), kReset);
        }
        std::printf("  %sResponse: \"%s\"%s\n", kYellow, accumulatedClean.c_str(), kReset);

        // Verification
        bool ok = containsKeyword(accumulatedClean, tc.expectedKeywords);
        if (ok) {
            std::printf("  -> %sPASS%s (found expected keyword)\n", kGreen, kReset);
            passed++;
        } else {
            std::printf("  -> %sFAIL%s (expected one of: ", kRed, kReset);
            for (size_t k = 0; k < tc.expectedKeywords.size(); ++k) {
                std::printf("\"%s\"%s", tc.expectedKeywords[k].c_str(), k + 1 < tc.expectedKeywords.size() ? ", " : "");
            }
            std::printf(")\n");
        }
        std::printf("\n");
        std::fflush(stdout);
    }

    // Stop node and queues
    textIn.stop();
    textOut.stop();
    node.stop();

    std::printf("=== Test execution completed: %s%d/%zu passed%s ===\n",
                passed == (int)testCases.size() ? kGreen : kYellow,
                passed, testCases.size(), kReset);

    if (passed < 35) {
        std::printf("%sToo many test cases failed!%s\n", kRed, kReset);
        return 1;
    }

    std::printf("%s>>> PASS — test_llm_ollama_stress <<<%s\n", kGreen, kReset);
    return 0;
}
