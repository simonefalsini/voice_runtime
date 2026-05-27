#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

namespace voice_runtime {

enum class SampleFormat {
    Int16,
    Float32
};

struct AudioFormat {
    int sampleRate = 16000;
    int channels = 1;
    int frameMs = 10;
    SampleFormat sampleFormat = SampleFormat::Int16;

    int samplesPerChannelPerFrame() const {
        return sampleRate * frameMs / 1000;
    }

    int totalSamplesPerFrame() const {
        return samplesPerChannelPerFrame() * channels;
    }
};

struct AudioFrame {
    AudioFormat format;
    std::vector<int16_t> pcm16;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;
    void* userData = nullptr;

    void resizeForFormat() {
        pcm16.resize(static_cast<std::size_t>(format.totalSamplesPerFrame()));
    }
};

using AudioFrameHandle = std::shared_ptr<AudioFrame>;

struct TextChunk {
    std::string text;
    bool isFinal = false;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;
};

struct RuntimeStats {
    uint64_t produced = 0;
    uint64_t consumed = 0;
    uint64_t dropped = 0;
    uint64_t recycled = 0;
    std::size_t highWatermark = 0;
};

} // namespace voice_runtime
