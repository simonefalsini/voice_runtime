// ---------------------------------------------------------------------------
// test_live_transcription.cpp
//
// Live microphone → VAD → STT transcription test.
//
// Pipeline:
//   [MiniaudioMic 16kHz/mono] → micQueue → [SileroVAD] → vadQueue
//       → [Qwen3STT] → textQueue → [console print]
//
// The VAD gates audio so that only speech segments are sent to the STT.
// Transcriptions are printed to stdout in real-time.
//
// Usage:
//   ./test_live_transcription [duration_seconds]
//
// Default duration: 30 seconds.  Press Ctrl+C to stop early.
//
// Required model files (relative to working directory):
//   models/vad/ggml-silero-v6.2.0.bin
//   models/stt/Qwen3-ASR-0.6B-Q8_0.gguf
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"

#include "MiniaudioMicNode.h"
#include "SileroVadNode.h"
#include "Qwen3SttNode.h"

using namespace voice_runtime;

// ---------------------------------------------------------------------------
// Signal handler for graceful Ctrl+C
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static void sigHandler(int /*sig*/) {
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// ANSI colors for terminal output
// ---------------------------------------------------------------------------

static constexpr const char* kColorReset  = "\033[0m";
static constexpr const char* kColorGreen  = "\033[32m";
static constexpr const char* kColorYellow = "\033[33m";
static constexpr const char* kColorCyan   = "\033[36m";
static constexpr const char* kColorGray   = "\033[90m";
static constexpr const char* kColorBold   = "\033[1m";

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Duration in seconds
    int durationSec = 30;
    if (argc > 1) {
        durationSec = std::atoi(argv[1]);
        if (durationSec <= 0) durationSec = 30;
    }

    // Model paths
    const std::string vadModelPath = "models/vad/ggml-silero-v6.2.0.bin";
    std::string sttModelPath = "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    if (argc > 2) {
        sttModelPath = argv[2];
    }
    
    std::string mmprojPath = "";
    if (argc > 3) {
        mmprojPath = argv[3];
    } else {
        if (sttModelPath.find("1.7B") != std::string::npos) {
            auto pos = sttModelPath.find_last_of("/\\");
            std::string dir = (pos == std::string::npos) ? "" : sttModelPath.substr(0, pos + 1);
            mmprojPath = dir + "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        }
    }

    std::printf("\n%s╔══════════════════════════════════════════════════════╗%s\n",
                kColorCyan, kColorReset);
    std::printf("%s║  🎤  Live Transcription Test                         ║%s\n",
                kColorCyan, kColorReset);
    std::printf("%s╚══════════════════════════════════════════════════════╝%s\n\n",
                kColorCyan, kColorReset);

    std::printf("  Duration:   %d seconds (Ctrl+C to stop early)\n", durationSec);
    std::printf("  VAD model:  %s\n", vadModelPath.c_str());
    std::printf("  STT model:  %s\n", sttModelPath.c_str());
    if (!mmprojPath.empty()) {
        std::printf("  MMProj:     %s\n", mmprojPath.c_str());
    }
    std::printf("\n");

    // Install signal handler
    std::signal(SIGINT, sigHandler);

    // -----------------------------------------------------------------------
    // Audio format — pipeline internal: 16kHz, mono, 10ms, Int16
    // -----------------------------------------------------------------------

    const AudioFormat pipelineFmt{16000, 1, 10, SampleFormat::Int16};

    // -----------------------------------------------------------------------
    // Queues
    // -----------------------------------------------------------------------

    // Mic → VAD
    AudioFrameQueue micQueue(256, QueueOverflowPolicy::DropOldest, "micQueue");
    // VAD → STT (only speech frames pass through)
    AudioFrameQueue vadQueue(512, QueueOverflowPolicy::DropOldest, "vadQueue");
    // VAD events (for display)
    VadEventQueue   vadEventQueue(64, QueueOverflowPolicy::DropOldest, "vadEvents");
    // STT → text output
    TextQueue       textQueue(64, QueueOverflowPolicy::DropOldest, "textQueue");

    // -----------------------------------------------------------------------
    // Nodes
    // -----------------------------------------------------------------------

    // 1. Microphone node — captures at 16kHz mono directly (no adapter needed)
    MiniaudioMicConfig micCfg;
    micCfg.format        = pipelineFmt;
    micCfg.poolSize      = 256;
    micCfg.enableDcRemoval = true;

    MiniaudioMicNode mic(micCfg);
    mic.setOutputQueue(&micQueue);

    // 2. VAD node — Silero model with energy fallback
    SileroVadConfig vadCfg;
    vadCfg.modelPath      = vadModelPath;
    vadCfg.format         = pipelineFmt;
    vadCfg.poolSize       = 256;
    vadCfg.thresholdStart = 0.5f;
    vadCfg.thresholdStop  = 0.35f;
    vadCfg.hangoverChunks = 8;   // ~320ms hangover (8 chunks × 4 frames × 10ms)

    SileroVadNode vad(vadCfg);
    vad.setInputQueue(&micQueue);
    vad.setOutputQueue(&vadQueue);
    vad.setEventQueue(&vadEventQueue);

    // 3. STT node — Qwen3 ASR (or stub if model not found)
    Qwen3SttConfig sttCfg;
    sttCfg.modelPath             = sttModelPath;
    sttCfg.mmprojPath            = mmprojPath;
    sttCfg.transcriptionTimeoutMs = 600;   // flush after 600ms silence
    sttCfg.minSpeechSamples      = 16000;  // 1s minimum (encoder needs enough frames)
    sttCfg.maxSpeechSamples      = 480000; // 30s maximum
    sttCfg.nThreads              = 4;
    sttCfg.stripLanguagePrefix   = true;

    Qwen3SttNode stt(sttCfg);
    stt.setInputQueue(&vadQueue);
    stt.setOutputQueue(&textQueue);

    // -----------------------------------------------------------------------
    // Initialize
    // -----------------------------------------------------------------------

    // Start library log redirector
    LibraryLogRedirector::instance().start();

    std::printf("%s[Init]%s Initializing microphone... ", kColorGray, kColorReset);
    if (!mic.initialize()) {
        std::fprintf(stderr, "\n%sFAIL: Microphone initialization failed%s\n",
                     kColorYellow, kColorReset);
        return 1;
    }
    std::printf("OK\n");

    std::printf("%s[Init]%s Initializing VAD... ", kColorGray, kColorReset);
    if (!vad.initialize()) {
        std::fprintf(stderr, "\n%sFAIL: VAD initialization failed%s\n",
                     kColorYellow, kColorReset);
        return 1;
    }
    std::printf("OK\n");

    std::printf("%s[Init]%s Initializing STT... ", kColorGray, kColorReset);
    if (!stt.initialize()) {
        std::fprintf(stderr, "\n%sFAIL: STT initialization failed%s\n",
                     kColorYellow, kColorReset);
        return 1;
    }
    std::printf("OK\n");

    // -----------------------------------------------------------------------
    // Start pipeline
    // -----------------------------------------------------------------------

    std::printf("\n%s───── Listening ─────%s\n\n", kColorGreen, kColorReset);
    std::fflush(stdout);

    vad.start();
    stt.start();
    mic.start();  // start mic last — it begins producing immediately

    // -----------------------------------------------------------------------
    // Main loop — consume text and VAD events, print to console
    // -----------------------------------------------------------------------

    auto startTime = std::chrono::steady_clock::now();
    int transcriptionCount = 0;
    bool isSpeaking = false;

    while (g_running.load()) {
        // Check duration
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= durationSec) break;

        // Drain VAD events
        VadEvent ev;
        while (vadEventQueue.tryPop(ev)) {
            if (ev.type == VadEvent::Type::SpeechStart) {
                isSpeaking = true;
                std::printf("%s● Speech detected%s (conf=%.2f)\n",
                            kColorGreen, kColorReset, ev.confidence);
                std::fflush(stdout);
            } else if (ev.type == VadEvent::Type::SpeechEnd) {
                isSpeaking = false;
                std::printf("%s○ Silence%s (conf=%.2f)\n",
                            kColorGray, kColorReset, ev.confidence);
                std::fflush(stdout);
            }
        }

        // Drain text output
        TextChunk chunk;
        while (textQueue.tryPop(chunk)) {
            if (!chunk.text.empty()) {
                transcriptionCount++;
                std::printf("\n  %s%s▶ [%d]%s %s%s%s\n\n",
                            kColorBold, kColorCyan,
                            transcriptionCount,
                            kColorReset,
                            kColorBold, chunk.text.c_str(), kColorReset);
                std::fflush(stdout);
            }
        }

        // Sleep to avoid busy-waiting (50ms poll interval)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -----------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------

    std::printf("\n%s───── Stopping ─────%s\n\n", kColorYellow, kColorReset);

    // Stop queues first to unblock threads (except textQueue so STT can push final flush)
    micQueue.stop();
    vadQueue.stop();
    vadEventQueue.stop();

    // Stop nodes (upstream -> downstream)
    mic.stop();
    vad.stop();
    stt.stop();

    // Drain remaining transcriptions from textQueue (including final shutdown flush)
    TextChunk chunk;
    while (textQueue.tryPop(chunk)) {
        if (!chunk.text.empty()) {
            transcriptionCount++;
            std::printf("\n  %s%s▶ [%d]%s %s%s%s\n\n",
                        kColorBold, kColorCyan,
                        transcriptionCount,
                        kColorReset,
                        kColorBold, chunk.text.c_str(), kColorReset);
            std::fflush(stdout);
        }
    }

    // Stop and clear textQueue at the very end
    textQueue.stop();

    micQueue.clear();
    vadQueue.clear();
    vadEventQueue.clear();
    textQueue.clear();

    // Stop library log redirector
    LibraryLogRedirector::instance().stop();

    // -----------------------------------------------------------------------
    // Final stats
    // -----------------------------------------------------------------------

    std::printf("%s=== Final Stats ===%s\n", kColorCyan, kColorReset);
    std::printf("  micQueue:      prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(micQueue.stats().produced),
                static_cast<unsigned long long>(micQueue.stats().consumed),
                static_cast<unsigned long long>(micQueue.stats().dropped),
                micQueue.stats().highWatermark);
    std::printf("  vadQueue:      prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(vadQueue.stats().produced),
                static_cast<unsigned long long>(vadQueue.stats().consumed),
                static_cast<unsigned long long>(vadQueue.stats().dropped),
                vadQueue.stats().highWatermark);
    std::printf("  textQueue:     prod=%-6llu  cons=%-6llu\n",
                static_cast<unsigned long long>(textQueue.stats().produced),
                static_cast<unsigned long long>(textQueue.stats().consumed));
    std::printf("  Transcriptions: %d\n", transcriptionCount);

    std::printf("\n%sDone.%s\n", kColorGreen, kColorReset);
    return 0;
}
