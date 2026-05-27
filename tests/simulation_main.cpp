#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include "voice_runtime/VoiceRuntime.h"
#include "SimulatedNodes.h"

using namespace voice_runtime;
using namespace voice_runtime::test;

static void printStats(const char* name, const RuntimeStats& s) {
    std::cout << name
              << " produced=" << s.produced
              << " consumed=" << s.consumed
              << " dropped=" << s.dropped
              << " highWatermark=" << s.highWatermark
              << " recycled=" << s.recycled
              << "\n";
}

int main() {
    RuntimeConfig cfg;
    cfg.audioFormat.sampleRate = 16000;
    cfg.audioFormat.channels = 1;
    cfg.audioFormat.frameMs = 10;
    cfg.micQueueCapacity = 128;
    cfg.ttsReferenceQueueCapacity = 128;
    cfg.speakerQueueCapacity = 128;
    cfg.cleanAudioQueueCapacity = 128;
    cfg.sttTextQueueCapacity = 512;
    cfg.llmTextQueueCapacity = 256;
    cfg.llmDiskSpoolPath = "/tmp/voice_runtime_llm_input.log";

    VoiceRuntime rt(
        cfg,
        std::make_unique<SimulatedMicrophoneNode>(cfg.audioFormat, 100, 256),
        std::make_unique<SimulatedAecNode>(256, 1),
        std::make_unique<SimulatedSttNode>(1),
        std::make_unique<SimulatedLlmNode>(350),
        std::make_unique<SimulatedTtsNode>(cfg.audioFormat, 256, 25),
        std::make_unique<SimulatedAudioOutputNode>(1)
    );

    if (!rt.initialize()) {
        std::cerr << "Runtime initialization failed\n";
        return 1;
    }

    rt.start();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    rt.stop();

    printStats("micQueue", rt.micQueue().stats());
    printStats("ttsReferenceQueue", rt.ttsReferenceQueue().stats());
    printStats("speakerQueue", rt.speakerQueue().stats());
    printStats("cleanQueue", rt.cleanQueue().stats());
    printStats("sttTextQueue", rt.sttTextQueue().stats());
    printStats("llmTextQueue", rt.llmTextQueue().stats());
    std::cout << "LLM memory window bytes=" << rt.llmDiskBuffer().memorySnapshot().size() << "\n";
    std::cout << "LLM disk spool=" << rt.llmDiskBuffer().path() << "\n";
    return 0;
}
