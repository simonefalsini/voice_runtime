#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Audio format
// ---------------------------------------------------------------------------

enum class SampleFormat {
    Int16,
    Float32
};

struct AudioFormat {
    int sampleRate  = 16000;
    int channels    = 1;
    int frameMs     = 10;
    SampleFormat sampleFormat = SampleFormat::Int16;

    int samplesPerChannelPerFrame() const {
        return sampleRate * frameMs / 1000;
    }

    int totalSamplesPerFrame() const {
        return samplesPerChannelPerFrame() * channels;
    }

    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate
            && channels  == o.channels
            && frameMs   == o.frameMs
            && sampleFormat == o.sampleFormat;
    }

    bool operator!=(const AudioFormat& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Audio frame
// ---------------------------------------------------------------------------

struct AudioFrame {
    AudioFormat      format;
    std::vector<int16_t> pcm16;
    std::vector<float>   pcmF32;   // usato quando format.sampleFormat == Float32
    uint64_t         sequence    = 0;
    uint64_t         timestampNs = 0;
    void*            userData    = nullptr;

    void resizeForFormat() {
        if (format.sampleFormat == SampleFormat::Int16) {
            pcm16.resize(static_cast<std::size_t>(format.totalSamplesPerFrame()));
            pcmF32.clear();
        } else {
            pcmF32.resize(static_cast<std::size_t>(format.totalSamplesPerFrame()));
            pcm16.clear();
        }
    }
};

using AudioFrameHandle = std::shared_ptr<AudioFrame>;

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

struct TextChunk {
    std::string text;
    bool        isFinal     = false;
    uint64_t    sequence    = 0;
    uint64_t    timestampNs = 0;
};

// ---------------------------------------------------------------------------
// VAD
// ---------------------------------------------------------------------------

struct VadEvent {
    enum class Type { SpeechStart, SpeechEnd };
    Type     type;
    uint64_t timestampNs = 0;
    float    confidence  = 1.0f;
};

// ---------------------------------------------------------------------------
// Barge-in
// ---------------------------------------------------------------------------

struct BargeInEvent {
    enum class Source { Keyboard, ContextDetected };
    Source   source;
    uint64_t timestampNs = 0;
    std::string reason;   // testo che ha triggerato il barge-in (se ContextDetected)
};

// ---------------------------------------------------------------------------
// Interrupt signal — condiviso tra BargeInNode, TTS e LLM
// ---------------------------------------------------------------------------

struct InterruptSignal {
    std::atomic<bool> requested{false};

    void request() noexcept {
        requested.store(true, std::memory_order_release);
    }
    void clear() noexcept {
        requested.store(false, std::memory_order_release);
    }
    bool check() const noexcept {
        return requested.load(std::memory_order_acquire);
    }
};

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

struct RuntimeStats {
    uint64_t    produced      = 0;
    uint64_t    consumed      = 0;
    uint64_t    dropped       = 0;
    uint64_t    recycled      = 0;
    std::size_t highWatermark = 0;
};

// ---------------------------------------------------------------------------
// Metriche periodiche — snapshot di una coda per il MetricsReporter
// ---------------------------------------------------------------------------

struct QueueSnapshot {
    const char* name         = "";
    std::size_t currentSize  = 0;
    std::size_t capacity     = 0;
    uint64_t    produced     = 0;
    uint64_t    consumed     = 0;
    uint64_t    dropped      = 0;
    std::size_t highWatermark= 0;
};

} // namespace voice_runtime
