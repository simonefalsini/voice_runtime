// Test STT transcription from a separate thread (like our pipeline does).
// If this crashes but test_stt_crash doesn't, it confirms a threading issue.

#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <thread>
#include <atomic>

#if __has_include("qwen3_asr.h")
#include "qwen3_asr.h"
#define HAS_QWEN3_ASR 1
#else
#define HAS_QWEN3_ASR 0
#endif

#if HAS_QWEN3_ASR

// Load Silero VAD GGML in another thread to reproduce exact pipeline scenario
#include "ggml.h"
#include "ggml-cpu.h"

static void dummy_ggml_work(std::atomic<bool>& running) {
    // Simulate VAD thread: repeatedly create and compute small ggml graphs
    // on the CPU backend, just like SileroVadNode does
    while (running.load()) {
        ggml_backend_t cpu = ggml_backend_cpu_init();
        if (!cpu) break;
        
        struct ggml_init_params params = {
            /*.mem_size   =*/ 1024 * 1024,
            /*.mem_buffer =*/ nullptr,
            /*.no_alloc   =*/ false,
        };
        ggml_context* ctx = ggml_init(params);
        if (!ctx) { ggml_backend_free(cpu); break; }
        
        // Small compute: just a 512-element tensor multiply
        ggml_tensor* a = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 512);
        ggml_tensor* b = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 512);
        ggml_tensor* c = ggml_mul(ctx, a, b);
        ggml_set_name(c, "out");
        ggml_set_output(c);
        
        ggml_cgraph* gf = ggml_new_graph(ctx);
        ggml_build_forward_expand(gf, c);
        
        // Fill with data
        float* ad = (float*)a->data;
        float* bd = (float*)b->data;
        for (int i = 0; i < 512; i++) { ad[i] = 1.0f; bd[i] = 2.0f; }
        
        ggml_backend_graph_compute(cpu, gf);
        
        ggml_free(ctx);
        ggml_backend_free(cpu);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

#endif

int main(int argc, char* argv[]) {
#if !HAS_QWEN3_ASR
    (void)argc; (void)argv;
    std::fprintf(stderr, "Qwen3-ASR not available\n");
    return 1;
#else
    const char* modelPath = "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    if (argc > 1) modelPath = argv[1];
    
    std::string mmprojPath = "";
    if (argc > 2) {
        mmprojPath = argv[2];
    } else {
        std::string mp(modelPath);
        if (mp.find("1.7B") != std::string::npos) {
            auto pos = mp.find_last_of("/\\");
            std::string dir = (pos == std::string::npos) ? "" : mp.substr(0, pos + 1);
            mmprojPath = dir + "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        }
    }
    
    std::printf("=== STT Threading Crash Test ===\n");
    std::printf("Model:  %s\n", modelPath);
    if (!mmprojPath.empty()) {
        std::printf("Mmproj: %s\n", mmprojPath.c_str());
    }
    std::printf("\n");
    
    // Load model in main thread (like our pipeline)
    std::printf("[1] Loading model in main thread... ");
    std::fflush(stdout);
    qwen3_asr::Qwen3ASR asr;
    if (!asr.load_model(modelPath, mmprojPath)) {
        std::fprintf(stderr, "FAIL: %s\n", asr.get_error().c_str());
        return 1;
    }
    std::printf("OK\n");
    
    // Start dummy GGML thread (simulates VAD)
    std::atomic<bool> vadRunning{true};
    std::printf("[2] Starting dummy GGML/CPU thread (simulates VAD)... ");
    std::fflush(stdout);
    std::thread vadThread(dummy_ggml_work, std::ref(vadRunning));
    std::printf("OK\n");
    
    // Generate audio
    const int sampleRate = 16000;
    const int numSamples = 5 * sampleRate; // 5 seconds
    std::vector<float> audio(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        audio[i] = 0.3f * std::sin(2.0f * M_PI * 200.0f * t)
                 + 0.2f * std::sin(2.0f * M_PI * 400.0f * t)
                 + 0.05f * (static_cast<float>(std::rand()) / RAND_MAX * 2.0f - 1.0f);
    }
    
    // Transcribe in a DIFFERENT thread (like our STT node)
    std::printf("[3] Transcribing in STT thread... \n");
    std::fflush(stdout);
    
    std::atomic<bool> sttDone{false};
    std::string resultText;
    bool resultOk = false;
    
    std::thread sttThread([&]() {
        qwen3_asr::transcribe_params params;
        params.n_threads = 4;
        params.print_progress = true;
        params.print_timing = true;
        
        auto result = asr.transcribe(audio.data(), numSamples, params);
        resultOk = result.success;
        resultText = result.text;
        if (!result.success) {
            std::fprintf(stderr, "STT error: %s\n", result.error_msg.c_str());
        }
        sttDone.store(true);
    });
    
    sttThread.join();
    
    // Stop VAD thread
    vadRunning.store(false);
    vadThread.join();
    
    std::printf("\n[4] Result: \"%s\"\n", resultText.c_str());
    std::printf("    Success: %s\n", resultOk ? "yes" : "no");
    std::printf("\n=== Test passed (no crash) ===\n");
    return 0;
#endif
}
