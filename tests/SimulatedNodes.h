#pragma once

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

namespace voice_runtime::test {

inline uint64_t nowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

class SimulatedMicrophoneNode final : public ActiveNodeBase, public IMicrophoneNode {
public:
    SimulatedMicrophoneNode(AudioFormat format, int framesPerSecond, std::size_t poolSize)
        : format_(format), periodMs_(1000 / framesPerSecond), pool_(poolSize) {}

    const char* name() const override { return "SimulatedMicrophone"; }
    bool initialize() override { return true; }
    void setOutputQueue(AudioFrameQueue* micOut) override { out_ = micOut; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            auto frame = pool_.acquireBlocking();
            if (!frame) break;
            frame->format = format_;
            frame->resizeForFormat();
            frame->sequence = sequence_++;
            frame->timestampNs = nowNs();
            for (auto& s : frame->pcm16) s = static_cast<int16_t>(frame->sequence % 100);
            if (out_) out_->push(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs_));
        }
    }

private:
    AudioFormat format_;
    int periodMs_ = 10;
    uint64_t sequence_ = 0;
    AudioFrameQueue* out_ = nullptr;
    SharedBufferPool<AudioFrame> pool_;
};

class SimulatedAecNode final : public ActiveNodeBase, public IAecDspNode {
public:
    SimulatedAecNode(std::size_t poolSize, int processMs)
        : pool_(poolSize), processMs_(processMs) {}

    const char* name() const override { return "SimulatedAEC"; }
    bool initialize() override { return true; }
    void setCaptureInputQueue(AudioFrameQueue* micIn) override { micIn_ = micIn; }
    void setRenderInputQueue(AudioFrameQueue* ttsRefIn) override { refIn_ = ttsRefIn; }
    void setOutputQueue(AudioFrameQueue* cleanOut) override { out_ = cleanOut; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            AudioFrameHandle mic;
            if (!micIn_ || !micIn_->pop(mic)) break;

            AudioFrameHandle ref;
            if (refIn_ && refIn_->size() > 0) {
                refIn_->pop(ref);
            }

            auto clean = pool_.acquireBlocking();
            if (!clean) break;
            *clean = *mic;
            clean->sequence = sequence_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));
            if (out_) out_->push(clean);
        }
    }

private:
    AudioFrameQueue* micIn_ = nullptr;
    AudioFrameQueue* refIn_ = nullptr;
    AudioFrameQueue* out_ = nullptr;
    uint64_t sequence_ = 0;
    SharedBufferPool<AudioFrame> pool_;
    int processMs_ = 1;
};

class SimulatedSttNode final : public ActiveNodeBase, public ISpeechToTextNode {
public:
    explicit SimulatedSttNode(int processMs) : processMs_(processMs) {}
    const char* name() const override { return "SimulatedSTT"; }
    bool initialize() override { return true; }
    void setInputQueue(AudioFrameQueue* cleanAudioIn) override { in_ = cleanAudioIn; }
    void setOutputQueue(TextQueue* textOut) override { out_ = textOut; }

protected:
    void runLoop() override {
        while (running()) {
            AudioFrameHandle audio;
            if (!in_ || !in_->pop(audio)) break;
            ++frames_;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));
            if (frames_ % 50 == 0 && out_) {
                TextChunk c;
                c.text = "Utterance block " + std::to_string(frames_ / 50) + "\n";
                c.isFinal = true;
                c.sequence = frames_ / 50;
                c.timestampNs = nowNs();
                out_->push(c);
            }
        }
    }

private:
    AudioFrameQueue* in_ = nullptr;
    TextQueue* out_ = nullptr;
    uint64_t frames_ = 0;
    int processMs_ = 1;
};

class SimulatedLlmNode final : public ActiveNodeBase, public ILanguageModelNode {
public:
    explicit SimulatedLlmNode(int processMsPerChunk) : processMs_(processMsPerChunk) {}
    const char* name() const override { return "SimulatedLLM"; }
    bool initialize() override { return true; }
    void setInputQueue(TextQueue* textIn) override { in_ = textIn; }
    void setOutputQueue(TextQueue* textOut) override { out_ = textOut; }
    void setPersistentInputBuffer(DiskBackedTextBuffer* buffer) override { disk_ = buffer; }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk input;
            if (!in_ || !in_->pop(input)) break;
            if (disk_) disk_->append(input.text);
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));
            if (out_) {
                TextChunk response;
                response.text = "Response to: " + input.text;
                response.isFinal = true;
                response.sequence = ++sequence_;
                response.timestampNs = nowNs();
                out_->push(response);
            }
        }
    }

private:
    TextQueue* in_ = nullptr;
    TextQueue* out_ = nullptr;
    DiskBackedTextBuffer* disk_ = nullptr;
    uint64_t sequence_ = 0;
    int processMs_ = 250;
};

class SimulatedTtsNode final : public ActiveNodeBase, public ITextToSpeechNode {
public:
    SimulatedTtsNode(AudioFormat format, std::size_t poolSize, int framesPerText)
        : format_(format), pool_(poolSize), framesPerText_(framesPerText) {}

    const char* name() const override { return "SimulatedTTS"; }
    bool initialize() override { return true; }
    void setInputQueue(TextQueue* textIn) override { in_ = textIn; }
    void setSpeakerOutputQueue(AudioFrameQueue* speakerOut) override { speakerOut_ = speakerOut; }
    void setAecReferenceOutputQueue(AudioFrameQueue* aecRefOut) override { refOut_ = aecRefOut; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            TextChunk text;
            if (!in_ || !in_->pop(text)) break;
            for (int i = 0; i < framesPerText_ && running(); ++i) {
                auto frame = pool_.acquireBlocking();
                if (!frame) break;
                frame->format = format_;
                frame->resizeForFormat();
                frame->sequence = sequence_++;
                frame->timestampNs = nowNs();
                for (auto& s : frame->pcm16) s = 500;
                if (refOut_) refOut_->push(frame);
                if (speakerOut_) speakerOut_->push(frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(format_.frameMs));
            }
        }
    }

private:
    AudioFormat format_;
    TextQueue* in_ = nullptr;
    AudioFrameQueue* speakerOut_ = nullptr;
    AudioFrameQueue* refOut_ = nullptr;
    uint64_t sequence_ = 0;
    SharedBufferPool<AudioFrame> pool_;
    int framesPerText_ = 20;
};

class SimulatedAudioOutputNode final : public ActiveNodeBase, public IAudioOutputNode {
public:
    explicit SimulatedAudioOutputNode(int processMs) : processMs_(processMs) {}
    const char* name() const override { return "SimulatedAudioOutput"; }
    bool initialize() override { return true; }
    void setInputQueue(AudioFrameQueue* speakerIn) override { in_ = speakerIn; }

protected:
    void runLoop() override {
        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;
            ++played_;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));
        }
    }

private:
    AudioFrameQueue* in_ = nullptr;
    uint64_t played_ = 0;
    int processMs_ = 1;
};

} // namespace voice_runtime::test
