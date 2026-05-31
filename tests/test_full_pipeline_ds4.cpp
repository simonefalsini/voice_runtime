// ---------------------------------------------------------------------------
// test_full_pipeline_ds4.cpp
//
// Pipeline completa con AEC, STT, LLMClassifierNode, LLM e TTS orchestrata
// tramite la classe VoiceRuntime (Pipeline lineare unica).
//
// ┌──────────────────────────────────────────────────────────────────────────┐
// │  PIPELINE LINEARE UNICA (docs/ARCHITECTURE.md)                           │
// │                                                                          │
// │  Mic → micRawQ → [MicAdapter?] → micQ → [WebRtcDspNode: AEC/VAD]         │
// │                                             │ (cleanQ)                   │
// │                                             ▼                            │
// │  [sttAdaptedQ?] ◄── [SttAdapter?] ◄─────────┘                            │
// │  │                                                                       │
// │  ▼                                                                       │
// │  [Qwen3STT] ──sttTextQ──► [LLMClassifierNode] ──classOutQ──►            │
// │                                                                          │
// │  [HttpLlmNode] ──llmOutQ──► [RealInterpreterNode] ──ttsInQ──►            │
// │                                                                          │
// │  [KokoroTTS] ──speakerQ──► [MiniaudioOutput]                             │
// │         │                                                                │
// │         └──────ttsRefQ──► [WebRtcDspNode: AEC render reference]          │
// └──────────────────────────────────────────────────────────────────────────┘
//
// Comportamento LLMClassifierNode (vocal commands):
//   - TTS inattivo:   ogni frase STT → LLM direttamente (passthrough).
//   - TTS attivo:
//       "DS4 ascoltami" → caching ON (tutto ciò che l'utente dice viene cachato).
//       "DS4 ho finito" → caching OFF.
//       "fermati" / "DS4 stop" / "stop" → interrompe LLM/TTS + re-inietta il cache.
//
// LLM: HttpLlmNode — si connette a Ollama o OpenAI-compatible.
//      Se non raggiungibile → fallback stub.
//
// Usage:
//   ./test_full_pipeline_ds4 [duration_sec]
//
// Environment variables:
//   LLM_BASE_URL          — default "http://127.0.0.1:11434"
//   LLM_API_PATH          — default "/v1/chat/completions"
//   LLM_MODEL             — default "qwen2.5:7b"
//   LLM_API_KEY           — default ""
//   VAD_MODE              — override VAD mode (0-3)
//   VAD_HANGOVER          — override hangover ms
//   VAD_SPEECH_THRESHOLD  — override speech threshold (0.0-1.0)
//   GATE_WITH_VAD         — 0/1 to disable/enable VAD gating
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "voice_runtime/ActiveNode.h"
#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"
#include "voice_runtime/TextBuffer.h"
#include "voice_runtime/VoiceRuntime.h"

#include "MiniaudioMicNode.h"
#include "MiniaudioOutputNode.h"
#include "WebRtcDspNode.h"
#include "SileroVadNode.h"
#include "Qwen3SttNode.h"
#include "KokoroTtsNode.h"
#include "HttpLlmNode.h"
#include "RealInterpreterNode.h"
#include "LLMClassifierNode.h"

using namespace voice_runtime;

// ─── Signal handling ─────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void sigHandler(int) { g_running.store(false); }

// ─── ANSI colors ─────────────────────────────────────────────────────────────

static constexpr const char* kReset   = "\033[0m";
static constexpr const char* kBold    = "\033[1m";
static constexpr const char* kGreen   = "\033[32m";
static constexpr const char* kYellow  = "\033[33m";
static constexpr const char* kCyan    = "\033[36m";
static constexpr const char* kGray    = "\033[90m";
static constexpr const char* kMagenta = "\033[35m";
static constexpr const char* kRed     = "\033[31m";

// ─── Utilities ───────────────────────────────────────────────────────────────

static std::string getEnv(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    int durationSec = (argc > 1) ? std::atoi(argv[1]) : 0; // 0 = indefinito
    if (durationSec < 0) durationSec = 0;

    // Percorsi modelli
    const std::string vadModelPath = "models/vad/ggml-silero-v6.2.0.bin";
    const std::string sttModelPath = (argc > 2)
        ? argv[2]
        : "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    const std::string ttsModelPath = "models/tts/kokoro-v1.1-zh.onnx";
    const std::string voicesPath   = "models/tts/voices.bin";
    const std::string espeakData   = "models/espeak-ng-data";

    // LLM config via env
    const std::string llmBaseUrl  = getEnv("LLM_BASE_URL", "http://127.0.0.1:11434");
    const std::string llmApiPath  = getEnv("LLM_API_PATH", "/v1/chat/completions");
    const std::string llmModel    = getEnv("LLM_MODEL",    "qwen3.6:35b");
    const std::string llmApiKey   = getEnv("LLM_API_KEY",  "");

    // ── Header ───────────────────────────────────────────────────────────────

    std::printf("\n%s%s╔══════════════════════════════════════════════════════════╗%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s║  🤖  Jarvis Full Pipeline — AEC + STT + LLM + TTS        ║%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s╚══════════════════════════════════════════════════════════╝%s\n\n",
                kBold, kCyan, kReset);
    std::printf("  LLM:         %s%s%s%s\n",
                kBold, llmBaseUrl.c_str(), llmApiPath.c_str(), kReset);
    std::printf("  Modello:     %s%s%s\n", kYellow, llmModel.c_str(), kReset);
    std::printf("  STT:         %s\n", sttModelPath.c_str());
    std::printf("  TTS:         %s\n", ttsModelPath.c_str());
    std::printf("  Durata:      %s\n\n",
                durationSec > 0
                    ? (std::to_string(durationSec) + " secondi").c_str()
                    : "indefinita (Ctrl+C per fermare)");
    std::printf("  %sPipeline (docs/ARCHITECTURE.md):%s\n", kBold, kReset);
    std::printf("    Mic → WebRtcDSP(AEC/VAD) → STT → LLMClassifier → LLM → Interpreter → TTS\n");
    std::printf("    TTS → speaker + AEC reference\n\n");
    std::printf("  %sVocal Commands (local filter):%s\n", kBold, kReset);
    std::printf("    %s\"Hey Jarvis ascoltami\"%s → attiva caching commenti (non inviato a LLM)\n", kMagenta, kReset);
    std::printf("    %s\"Hey Jarvis ho finito\"%s → chiudi finestra caching (non inviato a LLM)\n", kMagenta, kReset);
    std::printf("    %s\"fermati\" / \"Jarvis stop\"%s → interrompi LLM + re-inietta commento\n\n",
                kRed, kReset);
    std::fflush(stdout);

    std::signal(SIGINT, sigHandler);

    // ── Configurazione Pipeline ───────────────────────────────────────────────

    const AudioFormat micFormat    { 16000, 1, 10, SampleFormat::Int16 };
    const AudioFormat speakerFormat{ 24000, 1, 10, SampleFormat::Int16 };
    const AudioFormat aecRefFormat { 16000, 1, 10, SampleFormat::Int16 };

    RuntimeConfig cfg;
    cfg.pipelineFormat = micFormat;
    cfg.micRawFormat   = micFormat;
    cfg.sttInputFormat = micFormat;

    // Capacità delle code
    cfg.audioPoolBuffers            = 512;
    cfg.micRawQueueCapacity         = 64;
    cfg.micQueueCapacity            = 128;
    cfg.vadGatedQueueCapacity       = 128;
    cfg.ttsReferenceQueueCapacity   = 32;   // AEC3 Delay Estimator richiede <=32 frame
    cfg.speakerQueueCapacity         = 256;
    cfg.cleanAudioQueueCapacity     = 512;
    cfg.sttTextQueueCapacity        = 64;
    cfg.classifierOutQueueCapacity   = 32;
    cfg.llmOutputQueueCapacity       = 256;
    cfg.ttsInputQueueCapacity        = 128;
    cfg.vadEventQueueCapacity        = 64;
    cfg.bargeInEventQueueCapacity    = 16;

    cfg.audioOverflowPolicy         = QueueOverflowPolicy::DropOldest;
    cfg.textOverflowPolicy          = QueueOverflowPolicy::DropOldest;

    cfg.enableVad                   = true;
    cfg.enableIntegratedDspVad      = false; // External VAD (Silero)
    cfg.vadSpeechThreshold          = 0.5f;

    cfg.enableBargeIn               = true;
    cfg.overlapCommentBufferBytes    = 32 * 1024;
    cfg.overlapBufferUnbounded      = false;
    cfg.aecWarmupGracePeriodMs      = 200;

    cfg.enableMicAdapter            = false;
    cfg.enableSttAdapter            = false;

    cfg.llmDiskSpoolPath            = "/tmp/jarvis_full_pipeline_llm.log";
    cfg.llmMemoryWindowBytes        = 64 * 1024;

    cfg.printMetrics                = false; // Stampiamo il nostro status custom nel main thread

    // Override da env
    if (const char* e = std::getenv("VAD_SPEECH_THRESHOLD")) {
        cfg.vadSpeechThreshold = std::stof(e);
    }

    // ── Configurazione singoli nodi ──────────────────────────────────────────

    MiniaudioMicConfig micCfg;
    micCfg.format           = micFormat;
    micCfg.poolSize         = 256;
    micCfg.enableDcRemoval  = true;

    WebRtcDspConfig dspCfg;
    dspCfg.format                    = micFormat;
    dspCfg.outputPoolSize            = 256;
    dspCfg.enableEchoCancellation    = true;
    dspCfg.enableNoiseSuppression    = false;   // lasciare off per evitare distorsioni
    dspCfg.enableHighPassFilter      = true;
    dspCfg.enableAgc2                = false;
    dspCfg.enableVad                 = true;
    dspCfg.gateOutputWithVad         = true;    // Gating attivo verso STT
    dspCfg.vadMode                   = 1;       // 0=very aggressive, 3=loose
    dspCfg.vadHangoverMs             = 500;
    dspCfg.estimatedRenderDelayMs    = 50;
    dspCfg.aecWarmupGracePeriodMs    = cfg.aecWarmupGracePeriodMs;
    dspCfg.allowPassthroughWithoutWebRtc = true;

    if (const char* e = std::getenv("VAD_MODE"))     dspCfg.vadMode     = std::atoi(e);
    if (const char* e = std::getenv("VAD_HANGOVER")) dspCfg.vadHangoverMs = std::atoi(e);
    if (const char* e = std::getenv("GATE_WITH_VAD"))dspCfg.gateOutputWithVad = std::atoi(e)!=0;
    dspCfg.vadSpeechThreshold = cfg.vadSpeechThreshold;

    Qwen3SttConfig sttCfg;
    sttCfg.modelPath              = sttModelPath;
    sttCfg.transcriptionTimeoutMs = 600;
    sttCfg.minSpeechSamples       = 16000;
    sttCfg.maxSpeechSamples       = 480000;
    sttCfg.nThreads               = 4;
    sttCfg.stripLanguagePrefix    = true;

    HttpLlmConfig llmCfg;
    llmCfg.baseUrl            = llmBaseUrl;
    llmCfg.apiPath            = llmApiPath;
    llmCfg.model              = llmModel;
    llmCfg.apiKey             = llmApiKey;
    llmCfg.systemPrompt       =
        "Sei Jarvis, un assistente vocale conciso e naturale. "
        "Rispondi sempre in 2-3 frasi brevi. "
        "Evita elenchi puntati e markdown. "
        "Usa un tono conversazionale.";
    llmCfg.stream             = true;
    llmCfg.maxTokens          = 2048;
    llmCfg.temperature        = 0.7f;
    llmCfg.topP               = 0.9f;
    llmCfg.connectTimeoutSec  = 5;
    llmCfg.readTimeoutSec     = 90;
    llmCfg.allowStubFallback  = true;
    llmCfg.maxHistoryMessages = 10;
    llmCfg.thinkMode          = HttpLlmConfig::ThinkMode::NoThink;

    InterpreterConfig interpCfg;
    interpCfg.filterThinkTags = true;
    interpCfg.filterToolCalls = true;
    interpCfg.convertLatex    = false;

    KokoroTtsConfig ttsCfg;
    ttsCfg.modelPath      = ttsModelPath;
    ttsCfg.voicesPath     = voicesPath;
    ttsCfg.dictDir        = "models/tts/dict";
    ttsCfg.vocabPath      = "models/tts/dict/vocab.txt";
    ttsCfg.espeakDataPath = espeakData;
    ttsCfg.speakerFormat  = speakerFormat;
    ttsCfg.aecRefFormat   = aecRefFormat;
    ttsCfg.speakerPoolSize= 512;
    ttsCfg.aecRefPoolSize = 512;
    ttsCfg.defaultVoice   = "af_bella";
    ttsCfg.defaultLanguage= "en";
    ttsCfg.speed          = 1.0f;

    MiniaudioOutputConfig outCfg;
    outCfg.format             = speakerFormat;
    outCfg.ringBufferCapacity = 4800; // 200ms

    // ── Costruzione Nodi (smart pointers) ────────────────────────────────────

    auto mic      = std::make_unique<MiniaudioMicNode>(micCfg);
    auto dsp      = std::make_unique<WebRtcDspNode>(dspCfg);
    auto stt      = std::make_unique<Qwen3SttNode>(sttCfg);
    auto gate     = std::make_unique<LLMClassifierNode>();
    auto llm      = std::make_unique<HttpLlmNode>(llmCfg);
    auto interp   = std::make_unique<RealInterpreterNode>(interpCfg);
    auto tts      = std::make_unique<KokoroTtsNode>(ttsCfg);
    auto audioOut = std::make_unique<MiniaudioOutputNode>(outCfg);

    // Salviamo un puntatore raw al gate per tracciare lo stato nel loop
    LLMClassifierNode* gatePtr = gate.get();

    // ── Costruzione Silero VAD (External VAD) ───────────────────────────────
    SileroVadConfig sileroVadCfg;
    sileroVadCfg.modelPath      = vadModelPath;
    sileroVadCfg.format         = micFormat;
    sileroVadCfg.thresholdStart = cfg.vadSpeechThreshold;
    sileroVadCfg.thresholdStop  = cfg.vadSpeechThreshold - 0.15f;
    sileroVadCfg.hangoverChunks = 25; // 250ms
    sileroVadCfg.poolSize       = 128;
    auto vad = std::make_unique<SileroVadNode>(sileroVadCfg);

    // ── Creazione Runtime Orchestratore ──────────────────────────────────────

    VoiceRuntime rt(
        cfg,
        std::move(mic),
        std::move(dsp),
        std::move(stt),
        std::move(gate),
        std::move(llm),
        std::move(interp),
        std::move(tts),
        std::move(audioOut)
    );
    rt.setVadNode(std::move(vad));

    // ── Inizializzazione e Avvio ─────────────────────────────────────────────

    LibraryLogRedirector::instance().start();

    std::printf("%s[Init]%s Inizializzazione pipeline lineare unica... ", kGray, kReset);
    std::fflush(stdout);
    if (!rt.initialize()) {
        std::printf("%sFAIL%s\n", kRed, kReset);
        std::fprintf(stderr, "\n%sErrore durante l'inizializzazione del runtime — uscita%s\n",
                     kRed, kReset);
        return 1;
    }
    std::printf("%sOK%s\n", kGreen, kReset);

    std::printf("\n%s[Config]%s WebRTC: mode=%d hangover=%dms gate=%s | VAD: Silero VAD (external)\n",
                kGray, kReset,
                dspCfg.vadMode, dspCfg.vadHangoverMs,
                dspCfg.gateOutputWithVad ? "ON" : "OFF");
    std::printf("%s[Config]%s LLM: %s%s%s (stub fallback: %s)\n\n",
                kGray, kReset,
                kYellow, llmModel.c_str(), kReset,
                llmCfg.allowStubFallback ? "yes" : "no");
    std::fflush(stdout);

    rt.start();

    std::printf("%s%s┌─────────────────────────────────────────────────────────┐%s\n",
                kBold, kGreen, kReset);
    std::printf("%s%s│  ✅  Pipeline avviata — parla con Jarvis!               │%s\n",
                kBold, kGreen, kReset);
    std::printf("%s%s└─────────────────────────────────────────────────────────┘%s\n\n",
                kBold, kGreen, kReset);
    std::printf("  Keyword: %s\"Hey Jarvis ascoltami\"%s per iniziare a cachare commenti\n",
                kMagenta, kReset);
    std::printf("           %s\"fermati\" / \"Jarvis stop\"%s per interrompere e ri-partire col commento\n",
                kRed, kReset);
    std::printf("           %s\"Hey Jarvis ho finito\"%s per chiudere la finestra di ascolto\n\n",
                kMagenta, kReset);
    std::fflush(stdout);

    // ── Loop di Esecuzione ───────────────────────────────────────────────────

    auto startTime = std::chrono::steady_clock::now();
    int  lastStatusSec = -1;

    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();

        if (durationSec > 0 && elapsed >= durationSec) break;

        // Gestione degli eventi VAD prodotti
        VadEvent ev;
        while (rt.vadEventQueue().tryPop(ev)) {
            if (ev.type == VadEvent::Type::SpeechStart) {
                std::printf("  %s●%s Speech\n", kGreen, kReset);
            } else {
                std::printf("  %s○%s Silenzio\n", kGray, kReset);
            }
            std::fflush(stdout);
        }

        // Status periodico ogni 10 secondi
        int sec = static_cast<int>(elapsed);
        if (sec != lastStatusSec && sec % 10 == 0 && sec > 0) {
            lastStatusSec = sec;
            std::printf("%s  [%ds]%s mic=%llu dsp→stt=%llu text=%llu llmOut=%llu "
                        "tts=%s interrupts=%d injected=%d cache=\"%s\"\n",
                        kGray, sec, kReset,
                        (unsigned long long)rt.micQueue().stats().produced,
                        (unsigned long long)rt.cleanQueue().stats().produced,
                        (unsigned long long)rt.sttTextQueue().stats().produced,
                        (unsigned long long)rt.llmOutputQueue().stats().produced,
                        rt.ttsState().isActive() ? "ACTIVE" : "idle",
                        gatePtr->interruptCount(),
                        gatePtr->injectedCount(),
                        gatePtr->cachedComment().c_str());
            std::fflush(stdout);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ── Shutdown Orchestrato ─────────────────────────────────────────────────

    std::printf("\n%s%s───── Arresto ordinato pipeline (docs/ARCHITECTURE.md) ─────%s\n\n",
                kBold, kYellow, kReset);
    std::fflush(stdout);

    rt.stop();

    LibraryLogRedirector::instance().stop();

    // ── Statistiche Finali ───────────────────────────────────────────────────

    std::printf("%s=== Stats finali ===%s\n", kCyan, kReset);
    auto printQ = [](const char* name, const auto& q) {
        auto s = q.stats();
        std::printf("  %-14s prod=%-6llu cons=%-6llu drop=%-4llu hwm=%zu\n",
                    name,
                    (unsigned long long)s.produced,
                    (unsigned long long)s.consumed,
                    (unsigned long long)s.dropped,
                    s.highWatermark);
    };
    printQ("micQueue",     rt.micQueue());
    printQ("cleanQueue",   rt.cleanQueue());
    printQ("aecRefQueue",  rt.ttsReferenceQueue());
    printQ("speakerQueue", rt.speakerQueue());
    printQ("sttTextQ",     rt.sttTextQueue());
    printQ("llmInQ",       rt.classifierOutQueue());
    printQ("llmOutQ",      rt.llmOutputQueue());
    printQ("ttsInQ",       rt.ttsInputQueue());

    std::printf("\n  %sLLMClassifier%s: interrupts=%d  injected=%d\n",
                kBold, kReset,
                gatePtr->interruptCount(), gatePtr->injectedCount());
    std::printf("  LLM log: %s\n", rt.llmDiskBuffer().path().c_str());
    std::printf("\n%s%sDone.%s\n", kBold, kGreen, kReset);
    return 0;
}
