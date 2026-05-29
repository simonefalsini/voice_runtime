#pragma once

#include <iostream>
#include <fstream>
#include <mutex>
#include <string>

// Feature detection — Qwen3-ASR and GGML availability
#if __has_include(<qwen3_asr.h>) || __has_include("qwen3_asr.h")
#  define VOICE_RUNTIME_HAS_GGML_LOG 1
#  include <ggml/ggml.h>
#else
#  define VOICE_RUNTIME_HAS_GGML_LOG 0
#endif

namespace voice_runtime {

class LibraryLogRedirector {
public:
    static LibraryLogRedirector& instance() {
        static LibraryLogRedirector inst;
        return inst;
    }

    void start(const std::string& logPath = "voice_runtime_libraries.log") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) return;

        // Open log file with truncation so we get a fresh log on each run
        logFile_.open(logPath, std::ios::out | std::ios::trunc);
        if (!logFile_.is_open()) {
            std::cerr << "[Warning] Failed to open library log file: " << logPath << std::endl;
            return;
        }

        // Redirect C++ std::cout and std::cerr
        oldCoutBuf_ = std::cout.rdbuf(logFile_.rdbuf());
        oldCerrBuf_ = std::cerr.rdbuf(logFile_.rdbuf());

#if VOICE_RUNTIME_HAS_GGML_LOG
        // Redirect GGML logging
        ggml_log_set(ggmlLogCallback, this);
#endif

        active_ = true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return;

        // Restore C++ streams
        if (oldCoutBuf_) {
            std::cout.rdbuf(oldCoutBuf_);
            oldCoutBuf_ = nullptr;
        }
        if (oldCerrBuf_) {
            std::cerr.rdbuf(oldCerrBuf_);
            oldCerrBuf_ = nullptr;
        }

#if VOICE_RUNTIME_HAS_GGML_LOG
        // Restore default GGML logger
        ggml_log_set(nullptr, nullptr);
#endif

        if (logFile_.is_open()) {
            logFile_.close();
        }

        active_ = false;
    }

    void logRaw(const char* text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (logFile_.is_open()) {
            logFile_ << text;
            logFile_.flush();
        }
    }

private:
    LibraryLogRedirector() = default;
    ~LibraryLogRedirector() {
        stop();
    }

    LibraryLogRedirector(const LibraryLogRedirector&) = delete;
    LibraryLogRedirector& operator=(const LibraryLogRedirector&) = delete;

#if VOICE_RUNTIME_HAS_GGML_LOG
    static void ggmlLogCallback(enum ggml_log_level level, const char* text, void* user_data) {
        (void)level;
        if (user_data) {
            auto* self = static_cast<LibraryLogRedirector*>(user_data);
            self->logRaw(text);
        }
    }
#endif

    std::mutex mutex_;
    std::ofstream logFile_;
    std::streambuf* oldCoutBuf_ = nullptr;
    std::streambuf* oldCerrBuf_ = nullptr;
    bool active_ = false;
};

} // namespace voice_runtime
