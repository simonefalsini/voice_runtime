// Minimal crash reproduction test:
// Load the STT model, generate synthetic speech-like audio, and transcribe it.
// This isolates whether the crash is in the ASR library or in our pipeline threading.

#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdlib>

#if __has_include("qwen3_asr.h")
#include "qwen3_asr.h"
#define HAS_QWEN3_ASR 1
#else
#define HAS_QWEN3_ASR 0
#endif

int main(int argc, char* argv[]) {
#if !HAS_QWEN3_ASR
    (void)argc; (void)argv;
    std::fprintf(stderr, "Qwen3-ASR not available\n");
    return 1;
#else
    const char* modelPath = "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    if (argc > 1) modelPath = argv[1];
    
    float durationSec = 5.0f;
    if (argc > 2) durationSec = std::atof(argv[2]);
    
    // mmproj path: explicit arg, or auto-detect for 1.7B models
    std::string mmprojPath = "";
    if (argc > 3) {
        mmprojPath = argv[3];
    } else {
        std::string mp(modelPath);
        if (mp.find("1.7B") != std::string::npos) {
            auto pos = mp.find_last_of("/\\");
            std::string dir = (pos == std::string::npos) ? "" : mp.substr(0, pos + 1);
            mmprojPath = dir + "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        }
    }
    
    std::printf("=== STT Crash Reproduction Test ===\n");
    std::printf("Model:  %s\n", modelPath);
    if (!mmprojPath.empty()) std::printf("Mmproj: %s\n", mmprojPath.c_str());
    std::printf("Duration: %.1f seconds\n\n", durationSec);
    
    // Load model
    std::printf("[1/3] Loading model... ");
    std::fflush(stdout);
    qwen3_asr::Qwen3ASR asr;
    if (!asr.load_model(modelPath, mmprojPath)) {
        std::fprintf(stderr, "FAIL: %s\n", asr.get_error().c_str());
        return 1;
    }
    std::printf("OK\n");
    
    // Generate synthetic audio (mix of sine waves to simulate speech-like signal)
    const int sampleRate = 16000;
    int numSamples = static_cast<int>(durationSec * sampleRate);
    std::printf("[2/3] Generating %d synthetic samples (%.1fs)... ", numSamples, durationSec);
    std::fflush(stdout);
    
    std::vector<float> audio(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        // Mix of frequencies to create speech-like spectrum
        audio[i] = 0.3f * std::sin(2.0f * M_PI * 200.0f * t)   // fundamental
                 + 0.2f * std::sin(2.0f * M_PI * 400.0f * t)   // harmonic
                 + 0.1f * std::sin(2.0f * M_PI * 800.0f * t)   // harmonic
                 + 0.05f * std::sin(2.0f * M_PI * 1600.0f * t) // harmonic
                 + 0.02f * (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f); // noise
    }
    std::printf("OK\n");
    
    // Transcribe
    std::printf("[3/3] Transcribing... ");
    std::fflush(stdout);
    
    qwen3_asr::transcribe_params params;
    params.n_threads = 4;
    params.print_progress = true;
    params.print_timing = true;
    
    auto result = asr.transcribe(audio.data(), numSamples, params);
    if (!result.success) {
        std::fprintf(stderr, "FAIL: %s\n", result.error_msg.c_str());
        return 1;
    }
    
    std::printf("\nResult: \"%s\"\n", result.text.c_str());
    std::printf("\n=== Test passed (no crash) ===\n");
    return 0;
#endif
}
