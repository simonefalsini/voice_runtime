#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>
#include <cassert>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"
#include "Qwen3SttNode.h"

using namespace voice_runtime;

struct WavHeader {
    char riff[4];
    uint32_t overallSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtLength;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char dataHeader[4];
    uint32_t dataSize;
};

static std::vector<int16_t> downsample_24k_to_16k(const std::vector<int16_t>& src) {
    std::vector<int16_t> dst;
    double ratio = 24000.0 / 16000.0;
    std::size_t outLen = static_cast<std::size_t>(src.size() / ratio);
    dst.resize(outLen);
    for (std::size_t i = 0; i < outLen; ++i) {
        double srcPos = i * ratio;
        std::size_t idx0 = static_cast<std::size_t>(srcPos);
        std::size_t idx1 = std::min(idx0 + 1, src.size() - 1);
        double frac = srcPos - idx0;
        double val = src[idx0] * (1.0 - frac) + src[idx1] * frac;
        dst[i] = static_cast<int16_t>(val);
    }
    return dst;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <wav_file_path> [stt_model_path] [mmproj_path]\n", argv[0]);
        return 1;
    }

    std::string wavPath = argv[1];
    std::string sttModelPath = "models/stt/Qwen3-ASR-0.6B-Q8_0.gguf";
    if (argc > 2) {
        sttModelPath = argv[2];
    }
    std::string mmprojPath = "";
    if (argc > 3) {
        mmprojPath = argv[3];
    } else {
        if (sttModelPath.find("1.7B") != std::string::npos) {
            auto pos = sttModelPath.find_last_of("/\\");
            std::string dir = (pos == std::string::npos) ? "" : sttModelPath.substr(0, pos + 1);
            mmprojPath = dir + "mmproj-Qwen3-ASR-1.7B-Q8_0.gguf";
        }
    }

    // Read WAV file
    std::ifstream file(wavPath, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "Error: Failed to open WAV file: %s\n", wavPath.c_str());
        return 1;
    }

    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header)) {
        std::fprintf(stderr, "Error: Invalid WAV header size\n");
        return 1;
    }

    if (std::strncmp(header.riff, "RIFF", 4) != 0 || std::strncmp(header.wave, "WAVE", 4) != 0) {
        std::fprintf(stderr, "Error: File is not a valid RIFF/WAVE file\n");
        return 1;
    }

    if (header.audioFormat != 1 || header.numChannels != 1 || header.bitsPerSample != 16) {
        std::fprintf(stderr, "Error: Only 16-bit Mono PCM WAV files are supported (got format=%d, channels=%d, bits=%d)\n",
                     header.audioFormat, header.numChannels, header.bitsPerSample);
        return 1;
    }

    std::printf("Loaded WAV file: %s (Rate: %d Hz, Samples: %u)\n",
                wavPath.c_str(), header.sampleRate, header.dataSize / 2);

    std::vector<int16_t> rawPcm(header.dataSize / 2);
    file.read(reinterpret_cast<char*>(rawPcm.data()), header.dataSize);
    file.close();

    std::vector<int16_t> pcm16k;
    if (header.sampleRate == 24000) {
        std::printf("Downsampling from 24kHz to 16kHz...\n");
        pcm16k = downsample_24k_to_16k(rawPcm);
    } else if (header.sampleRate == 16000) {
        pcm16k = std::move(rawPcm);
    } else {
        std::fprintf(stderr, "Error: Unsupported sample rate %d Hz (must be 16kHz or 24kHz)\n", header.sampleRate);
        return 1;
    }

    // Configure STT Node
    Qwen3SttConfig sttCfg;
    sttCfg.modelPath = sttModelPath;
    sttCfg.mmprojPath = mmprojPath;
    sttCfg.nThreads = 4;
    sttCfg.transcriptionTimeoutMs = 600;
    sttCfg.minSpeechSamples = 16000;
    sttCfg.maxSpeechSamples = 480000;
    sttCfg.stripLanguagePrefix = true;

    Qwen3SttNode stt(sttCfg);
    AudioFrameQueue inQueue(256, QueueOverflowPolicy::BlockProducer, "stt_in");
    TextQueue outQueue(64, QueueOverflowPolicy::DropOldest, "stt_out");
    SharedBufferPool<AudioFrame> pool(1024);

    stt.setInputQueue(&inQueue);
    stt.setOutputQueue(&outQueue);

    if (!stt.initialize()) {
        std::fprintf(stderr, "Error: Failed to initialize STT Node\n");
        return 1;
    }

    stt.start();

    // Push audio in 10ms chunks (160 samples at 16kHz)
    const AudioFormat format{16000, 1, 10, SampleFormat::Int16};
    const std::size_t count = 160;
    std::size_t offset = 0;
    uint64_t seq = 0;

    std::printf("Feeding audio to STT...\n");

    int consecutiveSilenceFrames = 0;

    while (offset < pcm16k.size()) {
        auto frame = pool.acquireWithTimeout(std::chrono::milliseconds(100));
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        frame->format = format;
        frame->resizeForFormat();
        frame->sequence = seq++;
        frame->timestampNs = 0;

        std::size_t avail = std::min(count, pcm16k.size() - offset);
        std::memcpy(frame->pcm16.data(), pcm16k.data() + offset, avail * sizeof(int16_t));
        if (avail < count) {
            std::memset(frame->pcm16.data() + avail, 0, (count - avail) * sizeof(int16_t));
        }

        // Measure peak energy in this frame
        int16_t maxVal = 0;
        for (std::size_t i = 0; i < avail; ++i) {
            int16_t absVal = std::abs(frame->pcm16[i]);
            if (absVal > maxVal) {
                maxVal = absVal;
            }
        }

        // Absolute silence or low amplitude threshold
        bool isSilent = (maxVal < 150);
        if (isSilent) {
            consecutiveSilenceFrames++;
        } else {
            consecutiveSilenceFrames = 0;
        }

        inQueue.push(std::move(frame));
        offset += count;

        // If we see 300ms of silence (30 frames), pause feeding to trigger STT timeout
        if (consecutiveSilenceFrames >= 30) {
            std::printf("[Feeder] Silence gap detected (%d ms): pausing feed to trigger STT timeout...\n", consecutiveSilenceFrames * 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            consecutiveSilenceFrames = 0; // Reset
        }
    }

    // Wait for the queue to be fully drained by the STT node
    std::printf("Waiting for STT node to drain input queue...\n");
    while (inQueue.size() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Give the STT node a moment to process the last popped frame
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop queue to signal EOF
    inQueue.stop();

    // Allow the flush on shutdown to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::printf("Stopping STT node...\n");
    stt.stop();

    std::printf("Retrieving transcriptions...\n");

    TextChunk textChunk;
    bool found = false;
    while (outQueue.tryPop(textChunk)) {
        if (!textChunk.text.empty()) {
            std::printf("\n>>> Transcription: \"%s\"\n\n", textChunk.text.c_str());
            found = true;
        }
    }

    if (!found) {
        std::printf("\n>>> Transcription: [No speech detected or transcribed]\n\n");
    }

    inQueue.clear();
    outQueue.clear();

    return 0;
}
