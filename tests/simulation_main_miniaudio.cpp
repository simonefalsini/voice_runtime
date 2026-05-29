// ---------------------------------------------------------------------------
// simulation_main_miniaudio.cpp
//   Quick smoke test for MiniaudioMicNode and MiniaudioOutputNode.
//
//   Wiring (loopback):
//     MiniaudioMic → queue → MiniaudioOutput
//
//   Captures from the default microphone, pushes frames through a bounded
//   queue, and plays them back through the default speaker.  This creates a
//   real-time audio loopback useful for verifying that the nodes work.
//
//   Duration: 5 seconds, then clean shutdown.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "MiniaudioMicNode.h"
#include "MiniaudioOutputNode.h"

using namespace voice_runtime;

static void printStats(const char* label, const RuntimeStats& s) {
    std::printf("  %-22s prod=%-8llu cons=%-8llu drop=%-6llu hwm=%zu\n",
                label,
                static_cast<unsigned long long>(s.produced),
                static_cast<unsigned long long>(s.consumed),
                static_cast<unsigned long long>(s.dropped),
                s.highWatermark);
}

int main() {
    std::printf("=== Miniaudio loopback test ===\n");
    std::printf("Capturing from default mic → playing to default speaker\n");
    std::printf("Duration: 5 seconds\n\n");

    // Both nodes use 16kHz mono Int16 for this test
    // (in production, output would typically be 24kHz for TTS)
    AudioFormat format;
    format.sampleRate    = 16000;
    format.channels      = 1;
    format.frameMs       = 10;
    format.sampleFormat  = SampleFormat::Int16;

    // Queue between mic and output
    AudioFrameQueue loopbackQueue(128, QueueOverflowPolicy::DropOldest, "loopback");

    // Mic node
    MiniaudioMicConfig micCfg;
    micCfg.format          = format;
    micCfg.poolSize        = 256;
    micCfg.enableDcRemoval = true;

    auto mic = std::make_unique<MiniaudioMicNode>(micCfg);

    // Output node
    MiniaudioOutputConfig outCfg;
    outCfg.format             = format;
    outCfg.ringBufferCapacity = 16000;  // 1 second of audio

    auto out = std::make_unique<MiniaudioOutputNode>(outCfg);

    // Wire
    mic->setOutputQueue(&loopbackQueue);
    out->setInputQueue(&loopbackQueue);

    // Initialize
    if (!mic->initialize()) {
        std::fprintf(stderr, "Mic initialization failed\n");
        return 1;
    }
    if (!out->initialize()) {
        std::fprintf(stderr, "Output initialization failed\n");
        return 1;
    }

    // Start
    mic->start();
    out->start();

    std::printf("Loopback running...\n");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Shutdown
    std::printf("\nStopping...\n");

    // Stop queue first to unblock consumers
    loopbackQueue.stop();

    // Stop nodes
    mic->stop();
    out->stop();

    // Clear queue to release any remaining handles
    loopbackQueue.clear();

    // Stats
    std::printf("\n=== Final Stats ===\n");
    printStats("loopbackQueue", loopbackQueue.stats());

    std::printf("\nDone.\n");
    return 0;
}
