#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "voice_runtime/VoiceRuntime.h"
#include "SimulatedNodes.h"

using namespace voice_runtime;
using namespace voice_runtime::test;

// ---------------------------------------------------------------------------
// Stampa stats finali
// ---------------------------------------------------------------------------

static void printFinalStats(const char* label, const RuntimeStats& s) {
    std::printf("  %-22s prod=%-8llu cons=%-8llu drop=%-6llu hwm=%-6zu recycled=%llu\n",
                label,
                static_cast<unsigned long long>(s.produced),
                static_cast<unsigned long long>(s.consumed),
                static_cast<unsigned long long>(s.dropped),
                s.highWatermark,
                static_cast<unsigned long long>(s.recycled));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("=== voice_runtime simulation ===\n");
    std::printf("Press 'b' + Enter to trigger manual barge-in (only during TTS)\n");
    std::printf("Running for 15 seconds...\n\n");

    // -----------------------------------------------------------------------
    // Configurazione
    // -----------------------------------------------------------------------

    RuntimeConfig cfg;

    // Formato pipeline interno
    cfg.pipelineFormat.sampleRate   = 16000;
    cfg.pipelineFormat.channels     = 1;
    cfg.pipelineFormat.frameMs      = 10;
    cfg.pipelineFormat.sampleFormat = SampleFormat::Int16;

    // Simula mic nativo a 48kHz stereo → adapter converte a 16kHz mono
    cfg.micRawFormat.sampleRate     = 48000;
    cfg.micRawFormat.channels       = 2;
    cfg.micRawFormat.frameMs        = 10;
    cfg.micRawFormat.sampleFormat   = SampleFormat::Int16;

    // STT richiede stesso formato della pipeline (nessun adapter STT)
    cfg.sttInputFormat = cfg.pipelineFormat;

    // Code
    cfg.audioPoolBuffers            = 512;
    cfg.micRawQueueCapacity         = 64;
    cfg.micQueueCapacity            = 128;
    cfg.vadGatedQueueCapacity       = 128;
    cfg.ttsReferenceQueueCapacity   = 128;
    cfg.speakerQueueCapacity        = 128;
    cfg.cleanAudioQueueCapacity     = 128;
    cfg.sttTextQueueCapacity        = 512;

    cfg.audioOverflowPolicy         = QueueOverflowPolicy::DropOldest;
    cfg.textOverflowPolicy          = QueueOverflowPolicy::DropOldest;

    cfg.enableVad                   = true;
    cfg.vadSpeechThreshold          = 0.5f;
    cfg.enableBargeIn               = true;
    cfg.enableMicAdapter            = true;
    cfg.enableSttAdapter            = false; // stesso formato

    cfg.llmDiskSpoolPath            = "/tmp/voice_runtime_llm_input.log";
    cfg.llmMemoryWindowBytes        = 64 * 1024;

    cfg.metricsIntervalMs           = std::chrono::milliseconds(2000);
    cfg.printMetrics                = true;

    // -----------------------------------------------------------------------
    // Costruzione runtime
    // -----------------------------------------------------------------------

    auto mic    = std::make_unique<SimulatedMicrophoneNode>(cfg.micRawFormat, 100, 256);
    auto aec    = std::make_unique<SimulatedAecNode>(256, 1);
    auto stt    = std::make_unique<SimulatedSttNode>(1);
    auto classf = std::make_unique<SimulatedClassifierBargeInNode>();
    auto llm    = std::make_unique<SimulatedLlmNode>(350);
    auto interp = std::make_unique<SimulatedInterpreterNode>();
    auto tts    = std::make_unique<SimulatedTtsNode>(cfg.pipelineFormat, 256, 25);
    auto out    = std::make_unique<SimulatedAudioOutputNode>(1);

    VoiceRuntime rt(
        cfg,
        std::move(mic),
        std::move(aec),
        std::move(stt),
        std::move(classf),
        std::move(llm),
        std::move(interp),
        std::move(tts),
        std::move(out)
    );

    // VAD simulato
    rt.setVadNode(std::make_unique<SimulatedVadNode>(/*seed=*/12345));

    // -----------------------------------------------------------------------
    // Avvio
    // -----------------------------------------------------------------------

    if (!rt.initialize()) {
        std::fprintf(stderr, "Runtime initialization failed\n");
        return 1;
    }

    rt.start();

    std::this_thread::sleep_for(std::chrono::seconds(15));

    rt.stop();

    // -----------------------------------------------------------------------
    // Stats finali
    // -----------------------------------------------------------------------

    std::printf("\n=== Final Stats ===\n");
    printFinalStats("micRawQueue",      rt.micRawQueue().stats());
    printFinalStats("micQueue",         rt.micQueue().stats());
    printFinalStats("vadGatedQueue",    rt.vadGatedQueue().stats());
    printFinalStats("cleanQueue",       rt.cleanQueue().stats());
    printFinalStats("sttAdapted",       rt.sttAdaptedQueue().stats());
    printFinalStats("sttTextQueue",     rt.sttTextQueue().stats());
    printFinalStats("classifierOut",    rt.classifierOutQueue().stats());
    printFinalStats("llmOutputQueue",   rt.llmOutputQueue().stats());
    printFinalStats("ttsInputQueue",    rt.ttsInputQueue().stats());
    printFinalStats("speakerQueue",     rt.speakerQueue().stats());
    printFinalStats("ttsRefQueue",      rt.ttsReferenceQueue().stats());

    std::printf("\nAudio pool: consumed=%llu recycled=%llu hwm=%zu\n",
                static_cast<unsigned long long>(rt.audioPool().stats().consumed),
                static_cast<unsigned long long>(rt.audioPool().stats().recycled),
                rt.audioPool().stats().highWatermark);

    std::printf("LLM memory window: %zu bytes\n",
                rt.llmDiskBuffer().memorySnapshot().size());
    std::printf("LLM disk spool:    %s\n",
                rt.llmDiskBuffer().path().c_str());
    std::printf("TTS state:         %s\n",
                rt.ttsState().isActive() ? "ACTIVE" : "idle");

    std::printf("\nDone.\n");
    return 0;
}
