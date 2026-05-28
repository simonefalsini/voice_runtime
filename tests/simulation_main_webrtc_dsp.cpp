#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "voice_runtime/VoiceRuntime.h"
#include "voice_runtime/WebRtcDspNode.h"
#include "SimulatedNodes.h"

using namespace voice_runtime;
using namespace voice_runtime::test;

int main() {
    RuntimeConfig cfg;

    cfg.pipelineFormat.sampleRate   = 16000;
    cfg.pipelineFormat.channels     = 1;
    cfg.pipelineFormat.frameMs      = 10;
    cfg.pipelineFormat.sampleFormat = SampleFormat::Int16;

    cfg.micRawFormat = cfg.pipelineFormat;
    cfg.sttInputFormat = cfg.pipelineFormat;
    cfg.dspInputFormat = cfg.pipelineFormat;

    cfg.enableMicAdapter = false;
    cfg.enableSttAdapter = false;
    cfg.enableVad = true;       // VAD integrato nel WebRtcDspNode
    cfg.enableBargeIn = true;
    cfg.vadSpeechThreshold = 0.001f;
    cfg.printMetrics = true;
    cfg.metricsIntervalMs = std::chrono::milliseconds(2000);

    WebRtcDspConfig dspCfg;
    dspCfg.format = cfg.pipelineFormat;
    dspCfg.outputPoolSize = 256;
    dspCfg.enableEchoCancellation = true;
    dspCfg.enableNoiseSuppression = true;
    dspCfg.enableAgc1 = false;
    dspCfg.enableAgc2 = true;
    dspCfg.enableHighPassFilter = true;
    dspCfg.enableVad = true;
    dspCfg.vadMode = 2;
    dspCfg.vadSpeechThreshold = cfg.vadSpeechThreshold;
    dspCfg.vadHangoverMs = 300;
    dspCfg.gateOutputWithVad = false;

#if !VOICE_RUNTIME_ENABLE_WEBRTC_APM
    // Solo per poter eseguire questa simulazione senza libwebrtc installata.
    // In produzione lasciare false: il nodo deve inizializzarsi solo con WebRTC reale.
    dspCfg.allowPassthroughWithoutWebRtc = true;
#endif

    auto mic    = std::make_unique<SimulatedMicrophoneNode>(cfg.micRawFormat, 100, 256);
    auto dsp    = std::make_unique<WebRtcDspNode>(dspCfg);
    auto stt    = std::make_unique<SimulatedSttNode>(1);
    auto barge  = std::make_unique<SimulatedBargeInNode>();
    auto llm    = std::make_unique<SimulatedLlmNode>(350);
    auto tts    = std::make_unique<SimulatedTtsNode>(cfg.pipelineFormat, 256, 25);
    auto out    = std::make_unique<SimulatedAudioOutputNode>(1);

    VoiceRuntime rt(cfg, std::move(mic), std::move(dsp), std::move(stt),
                    std::move(barge), std::move(llm), std::move(tts), std::move(out));

    if (!rt.initialize()) {
        std::fprintf(stderr, "Runtime initialization failed\n");
        return 1;
    }

    std::printf("=== voice_runtime WebRTC DSP simulation ===\n");
#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
    std::printf("WebRTC APM enabled\n");
#else
    std::printf("WebRTC APM disabled: pass-through mode for wiring test\n");
#endif

    rt.start();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    rt.stop();

    std::printf("Done. clean=%llu vadEvents=%llu\n",
                static_cast<unsigned long long>(rt.cleanQueue().stats().produced),
                static_cast<unsigned long long>(rt.vadEventQueue().stats().produced));
    return 0;
}
