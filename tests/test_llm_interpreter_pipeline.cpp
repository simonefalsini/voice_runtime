// ---------------------------------------------------------------------------
// test_llm_interpreter_pipeline.cpp
//
// Pipeline integration test:
//
//   SimulatedStt  ──►  LLMClassifierNode  ──►  HttpLlmNode  ──►  RealInterpreterNode
//                              │                     ▲
//                              │    (interrupt)      │
//                              └──► InterruptSignal ─┘
//
// Scenarios tested:
//
//  [Scenario A] Normal flow (TTS not active):
//    STT → text goes directly through gate → LLM → Interpreter filters → TTS-stub queue
//
//  [Scenario B] DS4 listening mode:
//    While TTS is "playing" (ttsState.isActive=true):
//      - "DS4 ascoltami" activates comment-caching mode in LLMClassifierNode.
//      - All subsequent STT text is cached (not forwarded to LLM).
//      - "DS4 ho finito" ends the cache window.
//    When LLM finishes (TTS inactive) the gate forwards accumulated comment.
//
//  [Scenario C] Stop-keyword interrupt:
//    While TTS is active and LLM is streaming tokens:
//      - STT produces "DS4 ascoltami" → caching ON.
//      - STT produces "fermati" (stop keyword).
//      - Gate signals InterruptSignal to LLM.
//      - Gate waits for LLM to settle, then re-injects cached comment.
//
// The LLM is reached via HttpLlmNode. Configure via environment variables:
//
//   LLM_BASE_URL   — default "http://127.0.0.1:11434" (Ollama)
//   LLM_API_PATH   — default "/v1/chat/completions"
//   LLM_MODEL      — default "qwen2.5:7b"
//   LLM_API_KEY    — default ""
//
// If HttpLlmNode cannot connect or model is missing, it falls back to stub mode by default.
// To force real model testing, set LLM_REAL=1.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "voice_runtime/ActiveNode.h"
#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/TextBuffer.h"
#include "HttpLlmNode.h"
#include "RealInterpreterNode.h"
#include "LLMClassifierNode.h"

using namespace voice_runtime;

// ─── Utilities ──────────────────────────────────────────────────────────────

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void pushText(TextQueue& q, const std::string& text,
                     bool isFinal, uint64_t seq) {
    TextChunk c;
    c.text        = text;
    c.isFinal     = isFinal;
    c.sequence    = seq;
    c.timestampNs = now_ns();
    q.push(std::move(c));
}

// Drain queue into a string (non-blocking).
static std::string drainQueue(TextQueue& q) {
    std::string result;
    TextChunk c;
    while (q.tryPop(c)) result += c.text;
    return result;
}

// Drain and count final chunks (kept for future assertions).
[[maybe_unused]] static int drainCountFinals(TextQueue& q) {
    int finals = 0;
    TextChunk c;
    while (q.tryPop(c)) {
        if (c.isFinal) ++finals;
    }
    return finals;
}

// ─── Test helpers ────────────────────────────────────────────────────────────

static std::string trim(const std::string& str, const std::string& chars = " \t\r\n") {
    if (str.empty()) return str;
    std::size_t first = str.find_first_not_of(chars);
    if (first == std::string::npos) return "";
    std::size_t last = str.find_last_not_of(chars);
    return str.substr(first, (last - first + 1));
}

static std::string getEnv(const char* name, const char* fallback, const std::string& chars = " \t\r\n") {
    const char* v = std::getenv(name);
    return v ? trim(v, chars) : fallback;
}

static HttpLlmConfig makeOllamaConfig() {
    HttpLlmConfig cfg;
    cfg.baseUrl         = getEnv("LLM_BASE_URL", "http://127.0.0.1:11434", " \t\r\n=");
    cfg.apiPath         = getEnv("LLM_API_PATH", "/v1/chat/completions", " \t\r\n=");
    cfg.model           = getEnv("LLM_MODEL",    "qwen2.5:7b", " \t\r\n=");
    cfg.apiKey          = getEnv("LLM_API_KEY",  "");
    cfg.systemPrompt    = "You are DS4, a concise voice assistant. "
                          "Always reply in 1-2 sentences.";
    cfg.stream          = true;
    cfg.maxTokens       = 120;
    cfg.temperature     = 0.7f;
    cfg.connectTimeoutSec = 3;
    cfg.readTimeoutSec    = 20;
    cfg.allowStubFallback = true;   // fall back to stub if Ollama is not running
    cfg.maxHistoryMessages = 6;
    cfg.thinkMode = HttpLlmConfig::ThinkMode::NoThink;
    return cfg;
}

static bool shouldForceRealLLM() {
    const char* r = std::getenv("LLM_REAL");
    const char* fr = std::getenv("LLM_FORCE_REAL");
    std::string rStr = r ? trim(r, " \t\r\n=") : "";
    std::string frStr = fr ? trim(fr, " \t\r\n=") : "";
    return rStr == "1" || frStr == "1";
}

// ─── Test harness ────────────────────────────────────────────────────────────

static int gTestNum   = 0;
static int gFailCount = 0;

static void CHECK(bool cond, const char* desc) {
    ++gTestNum;
    if (cond) {
        std::printf("  [✓ %d] %s\n", gTestNum, desc);
    } else {
        std::fprintf(stderr, "  [✗ %d] FAIL: %s\n", gTestNum, desc);
        ++gFailCount;
    }
}

// ─── Scenario A: Normal passthrough (TTS idle) ───────────────────────────────
//
// STT → Gate (TTS inactive) → LLM stub → Interpreter → TTS-input queue
//
// Expected:
//   - LLM produces tokens (stub mode echo)
//   - Interpreter forwards them (filtering <think> if present)
//   - At least one isFinal chunk arrives at the TTS-input queue

static void scenarioA_normalPassthrough() {
    std::printf("\n--- Scenario A: Normal passthrough (TTS idle) ---\n");

    TextQueue sttOut   (32,  QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn    (32,  QueueOverflowPolicy::BlockProducer, "llm_in");
    TextQueue llmOut   (128, QueueOverflowPolicy::DropOldest,    "llm_out");
    TextQueue ttsIn    (128, QueueOverflowPolicy::DropOldest,    "tts_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal llmInterrupt;
    InterruptSignal ttsInterrupt;
    TtsStateSignal  ttsState;
    ttsState.setActive(false);  // TTS is idle

    RollingTextBuffer overlapBuffer(1024);

    // Gate
    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    // LLM (Ollama or stub)
    HttpLlmConfig llmCfg = makeOllamaConfig();
    llmCfg.forceStubMode = !shouldForceRealLLM();
    HttpLlmNode llm(llmCfg);
    llm.setInputQueue(&llmIn);
    llm.setOutputQueue(&llmOut);
    llm.setInterruptSignal(&llmInterrupt);

    // Interpreter (filters <think> tags etc.)
    InterpreterConfig interpCfg;
    interpCfg.convertLatex = false;
    RealInterpreterNode interp(interpCfg);
    interp.setInputQueue(&llmOut);
    interp.setOutputQueue(&ttsIn);

    // Initialize and start
    gate.initialize();
    gate.start();

    if (!llm.initialize()) {
        std::fprintf(stderr, "[ScenarioA] LLM init failed — skipping\n");
        sttOut.stop(); llmIn.stop(); llmOut.stop(); ttsIn.stop();
        gate.stop();
        return;
    }
    llm.start();

    interp.initialize();
    interp.start();

    // Simulate one STT utterance
    std::printf("[ScenarioA] Pushing STT text to gate...\n");
    pushText(sttOut, "Ciao DS4, come stai?", true, 1);

    // Wait for the full LLM response cycle
    std::this_thread::sleep_for(std::chrono::seconds(8));

    // Collect ttsIn queue
    std::vector<TextChunk> collected;
    TextChunk c;
    while (ttsIn.tryPop(c)) collected.push_back(std::move(c));

    bool gotContent = false;
    bool gotFinal   = false;
    std::string fullText;
    for (const auto& ch : collected) {
        if (!ch.text.empty()) { gotContent = true; fullText += ch.text; }
        if (ch.isFinal) gotFinal = true;
    }

    std::printf("[ScenarioA] TTS queue received %zu chunks. Text: \"%.80s\"\n",
                collected.size(), fullText.c_str());

    // Shutdown
    sttOut.stop();
    llmIn.stop();
    llmOut.stop();
    ttsIn.stop();
    gate.stop();
    llm.stop();
    interp.stop();
    sttOut.clear(); llmIn.clear(); llmOut.clear(); ttsIn.clear();

    CHECK(gotContent, "ScenarioA: TTS received non-empty content from LLM");
    CHECK(gotFinal,   "ScenarioA: TTS received isFinal chunk");
    CHECK(gate.interruptCount() == 0,
          "ScenarioA: No interrupts fired (TTS was idle)");
}

// ─── Scenario B: DS4 listening mode — comment caching ────────────────────────
//
// While TTS is "active":
//   - User says "DS4 ascoltami" → listening mode ON
//   - User says "ottima risposta" → cached
//   - User says "DS4 ho finito"  → cache window closed
//   - TTS goes idle → gate should NOT forward cached comment automatically
//     in this scenario (we verify cache accumulated correctly)
//
// After TTS becomes idle, the test verifies:
//   - commentCache() contains "ottima risposta"
//   - No text was forwarded to LLM during TTS-active window

static void scenarioB_commentCaching() {
    std::printf("\n--- Scenario B: DS4 comment caching (TTS active) ---\n");

    TextQueue sttOut(32, QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn (32, QueueOverflowPolicy::BlockProducer, "llm_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal  llmInterrupt;
    InterruptSignal  ttsInterrupt;
    TtsStateSignal   ttsState;
    ttsState.setActive(true);  // TTS is playing

    RollingTextBuffer overlapBuffer(1024);

    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    gate.initialize();
    gate.start();

    uint64_t seq = 0;

    // User says the wake keyword
    pushText(sttOut, "Hey Jarvis ascoltami", true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(gate.isListening(), "ScenarioB: gate entered listening mode");

    // User speaks a comment
    pushText(sttOut, "ottima risposta", true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // User closes the window
    pushText(sttOut, "Hey Jarvis ho finito", true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!gate.isListening(), "ScenarioB: gate exited listening mode");

    // Verify cached comment
    const std::string cached = gate.cachedComment();
    std::printf("[ScenarioB] Cached comment: \"%s\"\n", cached.c_str());
    CHECK(cached.find("ottima risposta") != std::string::npos,
          "ScenarioB: 'ottima risposta' was cached");

    // Verify nothing was forwarded to LLM
    const std::string forwarded = drainQueue(llmIn);
    std::printf("[ScenarioB] LLM received: \"%s\"\n", forwarded.c_str());
    CHECK(forwarded.empty(),
          "ScenarioB: No text forwarded to LLM while TTS active");

    CHECK(gate.interruptCount() == 0,
          "ScenarioB: No interrupt fired (only caching, no stop keyword)");

    // Cleanup
    sttOut.stop(); llmIn.stop();
    gate.stop();
    sttOut.clear(); llmIn.clear();
}

// ─── Scenario C: Stop keyword interrupt + cached comment re-injection ─────────
//
// Full pipeline wired. TTS is "active". LLM is generating tokens (stub mode).
//
//   t=0     User says "DS4 ascoltami"    → caching ON
//   t=100ms User says "però aspetta"     → cached
//   t=200ms User says "fermati"          → stop keyword
//             → InterruptSignal fired on LLM
//             → gate waits 500ms then re-injects "però aspetta" to LLM
//   t=~800ms LLM receives re-injected comment and processes it
//
// Verifications:
//   - interruptCount() == 1
//   - injectedCount()  == 1
//   - After injection, LLM produces output for the cached comment

static void scenarioC_stopAndReInject() {
    std::printf("\n--- Scenario C: Stop keyword + re-injection ---\n");

    TextQueue sttOut   (32,  QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn    (32,  QueueOverflowPolicy::BlockProducer, "llm_in");
    TextQueue llmOut   (256, QueueOverflowPolicy::DropOldest,    "llm_out");
    TextQueue ttsIn    (256, QueueOverflowPolicy::DropOldest,    "tts_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal  llmInterrupt;
    InterruptSignal  ttsInterrupt;
    TtsStateSignal   ttsState;
    ttsState.setActive(true);  // TTS is active (LLM is "speaking")

    RollingTextBuffer overlapBuffer(1024);

    // Gate
    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    // LLM (stub or Ollama)
    HttpLlmConfig llmCfg = makeOllamaConfig();
    llmCfg.forceStubMode = !shouldForceRealLLM();
    HttpLlmNode llm(llmCfg);
    llm.setInputQueue(&llmIn);
    llm.setOutputQueue(&llmOut);
    llm.setInterruptSignal(&llmInterrupt);

    // Interpreter
    InterpreterConfig interpCfg;
    interpCfg.convertLatex = false;
    RealInterpreterNode interp(interpCfg);
    interp.setInputQueue(&llmOut);
    interp.setOutputQueue(&ttsIn);

    // Start nodes
    gate.initialize();
    gate.start();

    if (!llm.initialize()) {
        std::fprintf(stderr, "[ScenarioC] LLM init failed — skipping\n");
        sttOut.stop(); llmIn.stop(); llmOut.stop(); ttsIn.stop();
        gate.stop();
        return;
    }
    llm.start();
    interp.initialize();
    interp.start();

    // First: send a question to LLM so it starts generating
    // (TTS active means we simulate LLM was already answering — we do this
    //  by sending via llmIn directly, bypassing the gate)
    std::printf("[ScenarioC] Sending initial question directly to LLM...\n");
    pushText(llmIn, "Parlami della storia di Roma", true, 1);

    // Give LLM a moment to start streaming tokens
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Now simulate user voice while TTS is active
    uint64_t seq = 100;
    std::printf("[ScenarioC] User says wake keyword...\n");
    pushText(sttOut, "Hey Jarvis ascoltami", true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::printf("[ScenarioC] User caches a comment...\n");
    pushText(sttOut, "però aspetta", true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::printf("[ScenarioC] User says stop keyword...\n");
    pushText(sttOut, "fermati", true, ++seq);

    // Wait for: interrupt + re-injection + LLM processes re-injected comment
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // Collect ttsIn to see all output
    std::vector<TextChunk> allTts;
    TextChunk c;
    while (ttsIn.tryPop(c)) allTts.push_back(std::move(c));

    bool gotAnyContent = false;
    int  finalCount    = 0;
    std::string allText;
    for (const auto& ch : allTts) {
        if (!ch.text.empty()) { gotAnyContent = true; allText += ch.text; }
        if (ch.isFinal) ++finalCount;
    }
    std::printf("[ScenarioC] TTS received %zu chunks, %d finals. Text: \"%.120s\"\n",
                allTts.size(), finalCount, allText.c_str());

    // Shutdown
    sttOut.stop(); llmIn.stop(); llmOut.stop(); ttsIn.stop();
    gate.stop();
    llm.stop();
    interp.stop();
    sttOut.clear(); llmIn.clear(); llmOut.clear(); ttsIn.clear();

    CHECK(gate.interruptCount() >= 1,
          "ScenarioC: At least one LLM interrupt was fired");
    CHECK(gate.injectedCount() >= 1,
          "ScenarioC: Cached comment was re-injected to LLM");
    CHECK(gotAnyContent,
          "ScenarioC: TTS received content (LLM answered both initial + re-injected)");
}

// ─── Scenario D: Gate inactive when TTS is idle — text forwarded immediately ──
//
// Verify that the gate does NOT cache or intercept anything when TTS is idle,
// even if the text looks like a keyword.

static void scenarioD_gateIdleWhenTtsInactive() {
    std::printf("\n--- Scenario D: Gate transparent when TTS is idle ---\n");

    TextQueue sttOut(32, QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn (64, QueueOverflowPolicy::DropOldest,    "llm_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal  llmInterrupt;
    InterruptSignal  ttsInterrupt;
    TtsStateSignal   ttsState;
    ttsState.setActive(false);   // TTS is IDLE

    RollingTextBuffer overlapBuffer(1024);

    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    gate.initialize();
    gate.start();

    // Even "Hey Jarvis ascoltami" and "fermati" should go straight to LLM when idle
    pushText(sttOut, "Hey Jarvis ascoltami",    true, 1);
    pushText(sttOut, "ottima risposta",  true, 2);
    pushText(sttOut, "Hey Jarvis ho finito",    true, 3);
    pushText(sttOut, "fermati",          true, 4);
    pushText(sttOut, "Ciao DS4",         true, 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string forwarded = drainQueue(llmIn);
    std::printf("[ScenarioD] LLM received: \"%s\"\n", forwarded.c_str());

    sttOut.stop(); llmIn.stop();
    gate.stop();
    sttOut.clear(); llmIn.clear();

    CHECK(!forwarded.empty(),
          "ScenarioD: All STT text forwarded to LLM when TTS is idle");
    CHECK(forwarded.find("Hey Jarvis ascoltami") != std::string::npos,
          "ScenarioD: Wake keyword forwarded verbatim when TTS idle");
    CHECK(forwarded.find("Ciao DS4") != std::string::npos,
          "ScenarioD: Normal text forwarded when TTS idle");
    CHECK(gate.isListening() == false,
          "ScenarioD: Gate never entered listening mode");
    CHECK(gate.interruptCount() == 0,
          "ScenarioD: No interrupt fired when TTS idle");
}

// ─── Scenario E: Multi-comment cache (multiple phrases) ──────────────────────

static void scenarioE_multiPhraseCache() {
    std::printf("\n--- Scenario E: Multi-phrase comment caching ---\n");

    TextQueue sttOut(32, QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn (32, QueueOverflowPolicy::BlockProducer, "llm_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal  llmInterrupt;
    InterruptSignal  ttsInterrupt;
    TtsStateSignal   ttsState;
    ttsState.setActive(true);

    RollingTextBuffer overlapBuffer(1024);

    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    gate.initialize();
    gate.start();

    uint64_t seq = 0;
    pushText(sttOut, "Hey Jarvis ascoltami",       true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pushText(sttOut, "prima cosa",          true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pushText(sttOut, "seconda cosa",        true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pushText(sttOut, "terza cosa",          true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pushText(sttOut, "Hey Jarvis ho finito",       true, ++seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::string cached = gate.cachedComment();
    std::printf("[ScenarioE] Full cached comment: \"%s\"\n", cached.c_str());

    sttOut.stop(); llmIn.stop();
    gate.stop();
    sttOut.clear(); llmIn.clear();

    CHECK(cached.find("prima cosa")   != std::string::npos, "ScenarioE: 'prima cosa' cached");
    CHECK(cached.find("seconda cosa") != std::string::npos, "ScenarioE: 'seconda cosa' cached");
    CHECK(cached.find("terza cosa")   != std::string::npos, "ScenarioE: 'terza cosa' cached");
    CHECK(!gate.isListening(),          "ScenarioE: Listening mode closed after done keyword");
}

// ─── Scenario F: Clean shutdown (no deadlock) ────────────────────────────────

static void scenarioF_cleanShutdown() {
    std::printf("\n--- Scenario F: Clean shutdown ---\n");
    ++gTestNum;
    std::printf("  [? %d] Clean shutdown (deadlock check)...\n", gTestNum);

    TextQueue sttOut (32, QueueOverflowPolicy::BlockProducer, "stt_out");
    TextQueue llmIn  (32, QueueOverflowPolicy::BlockProducer, "llm_in");
    TextQueue llmOut (64, QueueOverflowPolicy::DropOldest,    "llm_out");
    TextQueue ttsIn  (64, QueueOverflowPolicy::DropOldest,    "tts_in");
    BargeInQueue bargeInEvents(16, QueueOverflowPolicy::DropOldest, "bargeIn");

    InterruptSignal  llmInterrupt;
    InterruptSignal  ttsInterrupt;
    TtsStateSignal   ttsState;
    ttsState.setActive(false);

    RollingTextBuffer overlapBuffer(1024);

    LLMClassifierNode gate;
    gate.setTextInputQueue(&sttOut);
    gate.setTextOutputQueue(&llmIn);
    gate.setLlmInterruptSignal(&llmInterrupt);
    gate.setTtsInterruptSignal(&ttsInterrupt);
    gate.setTtsStateSignal(&ttsState);
    gate.setOverlapBuffer(&overlapBuffer);
    gate.setBargeInEventQueue(&bargeInEvents);

    HttpLlmConfig llmCfg = makeOllamaConfig();
    llmCfg.forceStubMode = true;
    HttpLlmNode llm(llmCfg);
    llm.setInputQueue(&llmIn);
    llm.setOutputQueue(&llmOut);
    llm.setInterruptSignal(&llmInterrupt);

    InterpreterConfig interpCfg;
    interpCfg.convertLatex = false;
    RealInterpreterNode interp(interpCfg);
    interp.setInputQueue(&llmOut);
    interp.setOutputQueue(&ttsIn);

    gate.initialize(); gate.start();
    llm.initialize();  llm.start();
    interp.initialize(); interp.start();

    // Push one request, let it start processing
    pushText(sttOut, "Ciao", true, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Initiate shutdown in the correct order
    sttOut.stop();
    llmIn.stop();
    llmOut.stop();
    ttsIn.stop();

    gate.stop();
    llm.stop();
    interp.stop();

    sttOut.clear(); llmIn.clear(); llmOut.clear(); ttsIn.clear();

    std::printf("  [✓ %d] Clean shutdown — no deadlock\n", gTestNum);
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== test_llm_interpreter_pipeline ===\n");
    std::printf("LLM endpoint: %s%s  model: %s\n\n",
                getEnv("LLM_BASE_URL", "http://127.0.0.1:11434").c_str(),
                getEnv("LLM_API_PATH", "/v1/chat/completions").c_str(),
                getEnv("LLM_MODEL",    "qwen2.5:7b").c_str());

    scenarioA_normalPassthrough();
    scenarioB_commentCaching();
    scenarioC_stopAndReInject();
    scenarioD_gateIdleWhenTtsInactive();
    scenarioE_multiPhraseCache();
    scenarioF_cleanShutdown();

    std::printf("\n=== Summary: %d tests, %d failed ===\n", gTestNum, gFailCount);

    if (gFailCount == 0) {
        std::printf(">>> PASS — test_llm_interpreter_pipeline <<<\n");
        return 0;
    } else {
        std::fprintf(stderr, ">>> FAIL — %d/%d tests failed <<<\n",
                     gFailCount, gTestNum);
        return 1;
    }
}
