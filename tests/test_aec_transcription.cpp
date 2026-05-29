// ---------------------------------------------------------------------------
// test_aec_transcription.cpp
//
// Echo-cancellation transcription test.
//
// Pipeline (in-series, mutually-active):
//   [Mic] → micQ → [SileroVAD] → vadQ → [WebRtcDSP] → cleanQ → [STT] → textQ
//   [TTS] → speakerQ → [MiniaudioOutput]
//   [TTS] → aecRefQ  → [WebRtcDSP render input]
//
//   TtsStateSignal coordinates all nodes:
//     TTS inactive → SileroVAD does speech gating, WebRtcDSP passes through
//     TTS active   → SileroVAD passes through, WebRtcDSP does AEC + internal VAD
//
// Flow:
//   Phase 1: User reads the first line of file1.  SileroVAD gates audio.
//            WebRTC AEC is pass-through (TTS inactive).
//            STT transcribes only speech segments.
//
//   Phase 2: Once the first transcription arrives, TTS starts reading file2
//            through speakers.  SileroVAD switches to pass-through.
//            WebRTC AEC activates and removes TTS echo.
//            STT transcribes only the user's voice (not TTS echo).
//
// Usage:
//   ./test_aec_transcription user_reading.txt tts_playback.txt [duration_sec]
//
// Default duration: 60 seconds.  Press Ctrl+C to stop early.
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"

#include "MiniaudioMicNode.h"
#include "MiniaudioOutputNode.h"
#include "SileroVadNode.h"
#include "WebRtcDspNode.h"
#include "Qwen3SttNode.h"
#include "KokoroTtsNode.h"

using namespace voice_runtime;

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static void sigHandler(int /*sig*/) {
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// ANSI colors
// ---------------------------------------------------------------------------

static constexpr const char* kReset   = "\033[0m";
static constexpr const char* kBold    = "\033[1m";
static constexpr const char* kGreen   = "\033[32m";
static constexpr const char* kYellow  = "\033[33m";
static constexpr const char* kCyan    = "\033[36m";
static constexpr const char* kGray    = "\033[90m";
static constexpr const char* kMagenta = "\033[35m";

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

static std::vector<std::string> loadLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file.is_open()) return lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

static std::string loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: %s <user_text_file> <tts_text_file> [duration_sec] [stt_model_path] [mmproj_path]\n\n"
            "  user_text_file: text for the user to read aloud (shown on screen)\n"
            "  tts_text_file:  text sent to TTS (played through speakers)\n"
            "  duration_sec:   optional, default 60\n"
            "  stt_model_path: optional, path to STT model\n"
            "  mmproj_path:    optional, path to audio encoder mmproj\n",
            argv[0]);
        return 1;
    }

    const std::string userTextPath = argv[1];
    const std::string ttsTextPath  = argv[2];
    int durationSec = (argc > 3) ? std::atoi(argv[3]) : 60;
    if (durationSec <= 0) durationSec = 60;

    // Load files
    auto userLines = loadLines(userTextPath);
    std::string ttsText = loadFile(ttsTextPath);

    if (userLines.empty()) {
        std::fprintf(stderr, "Error: user text file is empty or not found: %s\n",
                     userTextPath.c_str());
        return 1;
    }
    if (ttsText.empty()) {
        std::fprintf(stderr, "Error: TTS text file is empty or not found: %s\n",
                     ttsTextPath.c_str());
        return 1;
    }

    while (!ttsText.empty() && (ttsText.back() == '\n' || ttsText.back() == '\r'))
        ttsText.pop_back();

    // Model paths
    const std::string vadModelPath = "models/vad/ggml-silero-v6.2.0.bin";
    std::string sttModelPath = "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    if (argc > 4) {
        sttModelPath = argv[4];
    }
    std::string mmprojPath = "";
    if (argc > 5) {
        mmprojPath = argv[5];
    } else {
        if (sttModelPath.find("1.7B") != std::string::npos) {
            auto pos = sttModelPath.find_last_of("/\\");
            std::string dir = (pos == std::string::npos) ? "" : sttModelPath.substr(0, pos + 1);
            mmprojPath = dir + "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        }
    }
    const std::string ttsModelPath = "models/tts/kokoro-v1.1-zh.onnx";
    const std::string voicesPath   = "models/tts/voices-v1.0.bin";
    const std::string espeakData   = "models/espeak-ng-data";

    // Print header
    std::printf("\n%s%s╔══════════════════════════════════════════════════════════╗%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s║  🔊  Echo Cancellation Transcription Test                 ║%s\n",
                kBold, kCyan, kReset);
    std::printf("%s%s╚══════════════════════════════════════════════════════════╝%s\n\n",
                kBold, kCyan, kReset);

    std::printf("  Duration:    %d seconds (Ctrl+C to stop early)\n", durationSec);
    std::printf("  User text:   %s (%zu lines)\n", userTextPath.c_str(), userLines.size());
    std::printf("  TTS text:    %s (%zu chars)\n", ttsTextPath.c_str(), ttsText.size());
    std::printf("  VAD model:   %s\n", vadModelPath.c_str());
    std::printf("  STT model:   %s\n", sttModelPath.c_str());
    if (!mmprojPath.empty()) {
        std::printf("  MMProj:      %s\n", mmprojPath.c_str());
    }
    std::printf("  TTS model:   %s\n", ttsModelPath.c_str());
    std::printf("\n");
    std::printf("  %sPipeline:%s Mic → WebRtcDSP(AEC+VAD) → STT\n",
                kBold, kReset);
    std::printf("            TTS  → speakers + AEC reference\n");
    std::printf("  %sMode:%s    WebRTC integrated AEC/VAD mode\n\n",
                kBold, kReset);

    std::signal(SIGINT, sigHandler);

    // -----------------------------------------------------------------------
    // Audio formats
    // -----------------------------------------------------------------------

    const AudioFormat micFormat{16000, 1, 10, SampleFormat::Int16};
    const AudioFormat speakerFormat{24000, 1, 10, SampleFormat::Int16};
    const AudioFormat aecRefFormat{16000, 1, 10, SampleFormat::Int16};

    // -----------------------------------------------------------------------
    // Shared TTS state signal
    // -----------------------------------------------------------------------

    TtsStateSignal ttsState;

    // -----------------------------------------------------------------------
    // Queues
    // -----------------------------------------------------------------------

    // Mic → WebRtcDSP
    AudioFrameQueue micQueue(512, QueueOverflowPolicy::DropOldest, "micQueue");
    // WebRtcDSP → STT (cleaned audio)
    AudioFrameQueue cleanQueue(512, QueueOverflowPolicy::DropOldest, "cleanQueue");
    // VAD events (for display)
    VadEventQueue vadEventQueue(64, QueueOverflowPolicy::DropOldest, "vadEvents");
    // TTS → AEC (render reference, 16kHz).
    // FIX Bug3: capacity reduced from 256 to 32 frames (320ms @ 10ms/frame).
    // A large capacity allows the TTS to pre-fill the queue with seconds of
    // reference audio ahead of playback, creating an offset the static
    // estimatedRenderDelayMs cannot compensate. 32 frames keeps the buffering
    // within the range of AEC3's internal delay estimator (~300ms).
    AudioFrameQueue aecRefQueue(32, QueueOverflowPolicy::BlockProducer, "aecRefQueue");
    // TTS → speaker output (24kHz)
    AudioFrameQueue speakerQueue(256, QueueOverflowPolicy::BlockProducer, "speakerQueue");
    // TTS text input
    TextQueue ttsInputQueue(128, QueueOverflowPolicy::DropOldest, "ttsInputQueue");
    // STT text output
    TextQueue textQueue(64, QueueOverflowPolicy::DropOldest, "textQueue");

    // -----------------------------------------------------------------------
    // Nodes
    // -----------------------------------------------------------------------

    // 1. Microphone (16kHz mono)
    MiniaudioMicConfig micCfg;
    micCfg.format = micFormat;
    micCfg.poolSize = 256;
    micCfg.enableDcRemoval = true;
    MiniaudioMicNode mic(micCfg);
    mic.setOutputQueue(&micQueue);

    // 2. WebRTC DSP Node — does AEC + internal VAD
    WebRtcDspConfig dspCfg;
    dspCfg.format = micFormat;
    dspCfg.enableEchoCancellation = true;
    dspCfg.enableNoiseSuppression = false;
    dspCfg.enableHighPassFilter = true;
    dspCfg.enableAgc2 = false;
    dspCfg.enableVad = true;           // Integrated VAD
    dspCfg.gateOutputWithVad = true;   // Gate output during silence
    dspCfg.vadMode = 1;                // Less aggressive than default 2
    dspCfg.vadHangoverMs = 500;        // Longer hangover to prevent segment splitting
    dspCfg.estimatedRenderDelayMs = 50; // Match 50ms buffer + hardware offset

    // Optional environment variable overrides for debugging VAD
    if (const char* gateEnv = std::getenv("GATE_WITH_VAD")) {
        dspCfg.gateOutputWithVad = (std::atoi(gateEnv) != 0);
    }
    if (const char* modeEnv = std::getenv("VAD_MODE")) {
        dspCfg.vadMode = std::atoi(modeEnv);
    }
    if (const char* hangoverEnv = std::getenv("VAD_HANGOVER")) {
        dspCfg.vadHangoverMs = std::atoi(hangoverEnv);
    }

    dspCfg.outputPoolSize = 256;
    dspCfg.aecWarmupGracePeriodMs = 200;
    dspCfg.allowPassthroughWithoutWebRtc = true;

    WebRtcDspNode dsp(dspCfg);
    dsp.setCaptureInputQueue(&micQueue);    // Input from mic directly
    dsp.setRenderInputQueue(&aecRefQueue);  // TTS reference for AEC
    dsp.setOutputQueue(&cleanQueue);        // Clean audio → STT
    dsp.setVadEventQueue(&vadEventQueue);   // VAD event output
    dsp.setTtsStateSignal(&ttsState);

    // 3. STT (Qwen3)
    Qwen3SttConfig sttCfg;
    sttCfg.modelPath = sttModelPath;
    sttCfg.mmprojPath = mmprojPath;
    sttCfg.transcriptionTimeoutMs = 600;
    sttCfg.minSpeechSamples = 16000;
    sttCfg.maxSpeechSamples = 480000;
    sttCfg.nThreads = 4;
    sttCfg.stripLanguagePrefix = true;
    Qwen3SttNode stt(sttCfg);
    stt.setInputQueue(&cleanQueue);
    stt.setOutputQueue(&textQueue);

    // 5. TTS (Kokoro)
    KokoroTtsConfig ttsCfg;
    ttsCfg.modelPath = ttsModelPath;
    ttsCfg.voicesPath = voicesPath;
    ttsCfg.espeakDataPath = espeakData;
    ttsCfg.speakerFormat = speakerFormat;
    ttsCfg.aecRefFormat = aecRefFormat;
    ttsCfg.speakerPoolSize = 512;
    ttsCfg.aecRefPoolSize = 512;
    ttsCfg.defaultVoice = "af_bella";
    ttsCfg.defaultLanguage = "en";
    ttsCfg.speed = 1.0f;
    KokoroTtsNode tts(ttsCfg);
    tts.setInputQueue(&ttsInputQueue);
    tts.setSpeakerOutputQueue(&speakerQueue);
    tts.setAecReferenceOutputQueue(&aecRefQueue);
    tts.setTtsStateSignal(&ttsState);

    // 6. Audio output (speaker, 24kHz)
    MiniaudioOutputConfig outCfg;
    outCfg.format = speakerFormat;
    outCfg.ringBufferCapacity = 4800;  // 200ms buffer
    MiniaudioOutputNode audioOut(outCfg);
    audioOut.setInputQueue(&speakerQueue);

    // -----------------------------------------------------------------------
    // Start library log redirector
    // -----------------------------------------------------------------------

    LibraryLogRedirector::instance().start();

    // -----------------------------------------------------------------------
    // Initialize all nodes
    // -----------------------------------------------------------------------

    std::printf("%s[Init]%s Microphone... ", kGray, kReset);
    std::fflush(stdout);
    if (!mic.initialize()) { std::printf("FAIL\n"); return 1; }
    std::printf("OK\n");

    std::printf("%s[Init]%s WebRTC DSP... ", kGray, kReset);
    std::fflush(stdout);
    if (!dsp.initialize()) { std::printf("FAIL\n"); return 1; }
    std::printf("OK%s\n",
#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
                " (real AEC)"
#else
                " (pass-through/stub)"
#endif
    );

    std::printf("%s[Init]%s STT... ", kGray, kReset);
    std::fflush(stdout);
    if (!stt.initialize()) { std::printf("FAIL\n"); return 1; }
    std::printf("OK\n");

    std::printf("%s[Init]%s TTS... ", kGray, kReset);
    std::fflush(stdout);
    if (!tts.initialize()) { std::printf("FAIL\n"); return 1; }
    std::printf("OK\n");

    std::printf("%s[Init]%s Audio output... ", kGray, kReset);
    std::fflush(stdout);
    if (!audioOut.initialize()) { std::printf("FAIL\n"); return 1; }
    std::printf("OK\n");

    std::printf("%s[Config]%s WebRTC VAD Mode: %d | Hangover: %d ms | Gating: %s\n",
                kGray, kReset, dspCfg.vadMode, dspCfg.vadHangoverMs,
                dspCfg.gateOutputWithVad ? "ON" : "OFF (continuous audio)");
    std::fflush(stdout);

    // -----------------------------------------------------------------------
    // Display user text
    // -----------------------------------------------------------------------

    std::printf("\n%s════════════════════════════════════════════════════════════%s\n",
                kCyan, kReset);
    std::printf("%s  📖  Testo da leggere ad alta voce:%s\n", kBold, kReset);
    std::printf("%s════════════════════════════════════════════════════════════%s\n\n",
                kCyan, kReset);

    for (std::size_t i = 0; i < userLines.size(); ++i) {
        std::printf("  %s%zu.%s %s\n", kYellow, i + 1, kReset, userLines[i].c_str());
    }

    std::printf("\n%s════════════════════════════════════════════════════════════%s\n\n",
                kCyan, kReset);

    std::printf("%s%s  ▶ Leggi la riga 1 ad alta voce per iniziare...%s\n\n",
                kBold, kGreen, kReset);
    std::fflush(stdout);

    // -----------------------------------------------------------------------
    // Start pipeline (downstream → upstream order)
    // -----------------------------------------------------------------------

    audioOut.start();
    tts.start();
    stt.start();
    dsp.start();
    mic.start();  // mic last — starts producing immediately

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------

    auto startTime = std::chrono::steady_clock::now();
    int transcriptionCount = 0;
    std::vector<std::string> transcriptions;
    bool ttsStarted = false;
    int currentUserLine = 0;

    while (g_running.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= durationSec) break;

        // Drain VAD events (redirected to library log via std::cout)
        VadEvent ev;
        while (vadEventQueue.tryPop(ev)) {
            if (ev.type == VadEvent::Type::SpeechStart) {
                std::cout << "  ● Speech detected (conf=" << ev.confidence << ")\n" << std::flush;
            } else if (ev.type == VadEvent::Type::SpeechEnd) {
                std::cout << "  ○ Silence (conf=" << ev.confidence << ")\n" << std::flush;
            }
        }

        // Drain STT transcriptions
        TextChunk chunk;
        while (textQueue.tryPop(chunk)) {
            if (chunk.text.empty()) continue;

            transcriptionCount++;
            transcriptions.push_back(chunk.text);

            const char* modeLabel = ttsState.isActive()
                ? "🔇 AEC+STT" : "🎤 VAD+STT";

            std::printf("\n  %s%s[%d] %s:%s %s%s%s\n\n",
                        kBold, kCyan,
                        transcriptionCount,
                        modeLabel,
                        kReset,
                        kBold, chunk.text.c_str(), kReset);
            std::fflush(stdout);

            // Track user reading progress
            if (currentUserLine < static_cast<int>(userLines.size())) {
                currentUserLine++;
                if (currentUserLine < static_cast<int>(userLines.size())) {
                    std::printf("  %s▶ Ora leggi la riga %d:%s %s%s%s\n\n",
                                kGreen, currentUserLine + 1, kReset,
                                kYellow,
                                userLines[static_cast<std::size_t>(currentUserLine)].c_str(),
                                kReset);
                    std::fflush(stdout);
                }
            }

        }

        // Start TTS after first transcription OR after 3 seconds of inactivity
        if (!ttsStarted && (transcriptionCount >= 1 || elapsed >= 3)) {
            ttsStarted = true;
            std::printf("  %s%s🔊 Avvio TTS — WebRTC AEC attivo%s\n\n",
                        kBold, kMagenta, kReset);
            std::fflush(stdout);

            std::string defaultMarker = "";
            std::string cleanText = ttsText;
            
            // Trim leading whitespace
            auto startPos = cleanText.find_first_not_of(" \t\r\n");
            if (startPos != std::string::npos) {
                cleanText = cleanText.substr(startPos);
            }
            
            if (cleanText.size() >= 8 && cleanText[0] == '[') {
                auto endPos = cleanText.find(']');
                if (endPos != std::string::npos && endPos <= 10) {
                    std::string marker = cleanText.substr(1, endPos - 1);
                    if (marker.compare(0, 5, "lang=") == 0) {
                        defaultMarker = "[" + marker + "] ";
                        cleanText = cleanText.substr(endPos + 1);
                    }
                }
            }

            // Split into sentences based on punctuation .?! and newline
            std::vector<std::string> sentences;
            std::string current;
            for (std::size_t i = 0; i < cleanText.size(); ++i) {
                char c = cleanText[i];
                current.push_back(c);
                if (c == '.' || c == '?' || c == '!' || c == '\n') {
                    // Trim whitespace
                    auto first = current.find_first_not_of(" \t\r\n");
                    if (first != std::string::npos) {
                        auto last = current.find_last_not_of(" \t\r\n");
                        std::string trimmed = current.substr(first, (last - first + 1));
                        if (!trimmed.empty()) {
                            sentences.push_back(defaultMarker + trimmed);
                        }
                    }
                    current.clear();
                }
            }
            if (!current.empty()) {
                auto first = current.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    auto last = current.find_last_not_of(" \t\r\n");
                    std::string trimmed = current.substr(first, (last - first + 1));
                    if (!trimmed.empty()) {
                        sentences.push_back(defaultMarker + trimmed);
                    }
                }
            }

            if (sentences.empty()) {
                TextChunk ttsChunk;
                ttsChunk.text = ttsText;
                ttsChunk.isFinal = true;
                ttsChunk.sequence = 1;
                ttsChunk.timestampNs = 0;
                ttsInputQueue.push(std::move(ttsChunk));
            } else {
                std::size_t seq = 1;
                for (const auto& sentence : sentences) {
                    TextChunk ttsChunk;
                    ttsChunk.text = sentence;
                    ttsChunk.isFinal = (seq == sentences.size());
                    ttsChunk.sequence = seq++;
                    ttsChunk.timestampNs = 0;
                    ttsInputQueue.push(std::move(ttsChunk));
                }
            }
        }

        // Status line every 5 seconds (redirected to library log via std::cout)
        static int lastStatusSec = -1;
        int sec = static_cast<int>(elapsed);
        if (sec != lastStatusSec && sec % 5 == 0) {
            lastStatusSec = sec;
            std::cout << "  [" << sec << "s] mic=" << micQueue.stats().produced
                      << "  dsp→stt=" << cleanQueue.stats().produced
                      << "  aecRef=" << aecRefQueue.stats().produced
                      << "  text=" << textQueue.stats().produced
                      << "  tts=" << (ttsState.isActive() ? "ACTIVE" : "idle")
                      << "  mode=" << (ttsState.isActive() ? "AEC" : "VAD") << "\n" << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -----------------------------------------------------------------------
    // Shutdown (reverse startup order)
    // -----------------------------------------------------------------------

    std::printf("\n%s───── Stopping ─────%s\n\n", kYellow, kReset);
    std::fflush(stdout);

    // Stop queues first to unblock threads (except textQueue so STT can push final flush)
    micQueue.stop();
    cleanQueue.stop();
    aecRefQueue.stop();
    speakerQueue.stop();
    ttsInputQueue.stop();
    vadEventQueue.stop();

    // Stop nodes (upstream -> downstream)
    mic.stop();
    dsp.stop();
    stt.stop();

    // Drain remaining transcriptions from textQueue (including final shutdown flush)
    TextChunk chunk;
    while (textQueue.tryPop(chunk)) {
        if (!chunk.text.empty()) {
            transcriptionCount++;
            transcriptions.push_back(chunk.text);
        }
    }

    tts.stop();
    audioOut.stop();

    // Stop and clear textQueue at the very end
    textQueue.stop();

    micQueue.clear();
    aecRefQueue.clear();
    cleanQueue.clear();
    vadEventQueue.clear();
    speakerQueue.clear();
    ttsInputQueue.clear();
    textQueue.clear();

    // Stop library log redirector
    LibraryLogRedirector::instance().stop();

    // -----------------------------------------------------------------------
    // Final stats
    // -----------------------------------------------------------------------

    std::printf("%s=== Final Stats ===%s\n", kCyan, kReset);
    std::printf("  micQueue:      prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(micQueue.stats().produced),
                static_cast<unsigned long long>(micQueue.stats().consumed),
                static_cast<unsigned long long>(micQueue.stats().dropped),
                micQueue.stats().highWatermark);
    std::printf("  cleanQueue:    prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(cleanQueue.stats().produced),
                static_cast<unsigned long long>(cleanQueue.stats().consumed),
                static_cast<unsigned long long>(cleanQueue.stats().dropped),
                cleanQueue.stats().highWatermark);
    std::printf("  aecRefQueue:   prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(aecRefQueue.stats().produced),
                static_cast<unsigned long long>(aecRefQueue.stats().consumed),
                static_cast<unsigned long long>(aecRefQueue.stats().dropped),
                aecRefQueue.stats().highWatermark);
    std::printf("  speakerQueue:  prod=%-6llu  cons=%-6llu  drop=%-4llu  hwm=%zu\n",
                static_cast<unsigned long long>(speakerQueue.stats().produced),
                static_cast<unsigned long long>(speakerQueue.stats().consumed),
                static_cast<unsigned long long>(speakerQueue.stats().dropped),
                speakerQueue.stats().highWatermark);
    std::printf("  textQueue:     prod=%-6llu  cons=%-6llu\n",
                static_cast<unsigned long long>(textQueue.stats().produced),
                static_cast<unsigned long long>(textQueue.stats().consumed));
    std::printf("  Transcriptions: %d\n", transcriptionCount);
    std::printf("  TTS activated:  %s\n", ttsStarted ? "yes" : "no");

    std::printf("\n%s=== Transcribed Speech Summary ===%s\n", kBold, kReset);
    if (transcriptions.empty()) {
        std::printf("  No speech was transcribed.\n");
    } else {
        for (std::size_t i = 0; i < transcriptions.size(); ++i) {
            std::printf("  %s[%zu]%s \"%s\"\n", kGreen, i + 1, kReset, transcriptions[i].c_str());
        }
    }
    std::printf("==================================\n");

    std::printf("\n%sDone.%s\n", kGreen, kReset);
    return 0;
}
