#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include "SharedBufferPool.h"
#include "Interfaces.h"
#include "RuntimeConfig.h"

namespace voice_runtime {

class VoiceRuntime {
public:
    VoiceRuntime(RuntimeConfig config,
                 std::unique_ptr<IMicrophoneNode> microphone,
                 std::unique_ptr<IAecDspNode> aec,
                 std::unique_ptr<ISpeechToTextNode> stt,
                 std::unique_ptr<ILanguageModelNode> llm,
                 std::unique_ptr<ITextToSpeechNode> tts,
                 std::unique_ptr<IAudioOutputNode> output)
        : config_(std::move(config)),
          audioPool_(config_.audioPoolBuffers),
          micQueue_(config_.micQueueCapacity, config_.audioOverflowPolicy),
          ttsRefQueue_(config_.ttsReferenceQueueCapacity, config_.audioOverflowPolicy),
          speakerQueue_(config_.speakerQueueCapacity, config_.audioOverflowPolicy),
          cleanQueue_(config_.cleanAudioQueueCapacity, config_.audioOverflowPolicy),
          sttTextQueue_(config_.sttTextQueueCapacity, config_.textOverflowPolicy),
          llmTextQueue_(config_.llmTextQueueCapacity, config_.textOverflowPolicy),
          llmDiskBuffer_(config_.llmDiskSpoolPath, config_.llmMemoryWindowBytes),
          microphone_(std::move(microphone)),
          aec_(std::move(aec)),
          stt_(std::move(stt)),
          llm_(std::move(llm)),
          tts_(std::move(tts)),
          output_(std::move(output)) {}

    ~VoiceRuntime() {
        stop();
    }

    bool initialize() {
        microphone_->setOutputQueue(&micQueue_);
        aec_->setCaptureInputQueue(&micQueue_);
        aec_->setRenderInputQueue(&ttsRefQueue_);
        aec_->setOutputQueue(&cleanQueue_);
        stt_->setInputQueue(&cleanQueue_);
        stt_->setOutputQueue(&sttTextQueue_);
        llm_->setInputQueue(&sttTextQueue_);
        llm_->setOutputQueue(&llmTextQueue_);
        llm_->setPersistentInputBuffer(&llmDiskBuffer_);
        tts_->setInputQueue(&llmTextQueue_);
        tts_->setSpeakerOutputQueue(&speakerQueue_);
        tts_->setAecReferenceOutputQueue(&ttsRefQueue_);
        output_->setInputQueue(&speakerQueue_);

        return microphone_->initialize() && aec_->initialize() && stt_->initialize() &&
               llm_->initialize() && tts_->initialize() && output_->initialize();
    }

    void start() {
        if (running_.exchange(true)) {
            return;
        }

        output_->start();
        aec_->start();
        stt_->start();
        llm_->start();
        tts_->start();
        microphone_->start();
    }

    void stop() {
        if (!running_.exchange(false) && stoppedOnce_.exchange(true)) {
            return;
        }

        // Close queues first. This wakes every producer blocked on push() and
        // every consumer blocked on pop(), independently from node ordering.
        micQueue_.stop();
        ttsRefQueue_.stop();
        speakerQueue_.stop();
        cleanQueue_.stop();
        sttTextQueue_.stop();
        llmTextQueue_.stop();

        microphone_->stop();
        tts_->stop();
        llm_->stop();
        stt_->stop();
        aec_->stop();
        output_->stop();

        // Release queued handles before node-owned pools are destroyed.
        micQueue_.clear();
        ttsRefQueue_.clear();
        speakerQueue_.clear();
        cleanQueue_.clear();
        sttTextQueue_.clear();
        llmTextQueue_.clear();

        audioPool_.stop();
    }

    AudioFrameQueue& micQueue() { return micQueue_; }
    AudioFrameQueue& ttsReferenceQueue() { return ttsRefQueue_; }
    AudioFrameQueue& speakerQueue() { return speakerQueue_; }
    AudioFrameQueue& cleanQueue() { return cleanQueue_; }
    TextQueue& sttTextQueue() { return sttTextQueue_; }
    TextQueue& llmTextQueue() { return llmTextQueue_; }
    DiskBackedTextBuffer& llmDiskBuffer() { return llmDiskBuffer_; }

private:
    std::atomic<bool> running_ = false;
    std::atomic<bool> stoppedOnce_ = false;

    RuntimeConfig config_;
    SharedBufferPool<AudioFrame> audioPool_;

    AudioFrameQueue micQueue_;
    AudioFrameQueue ttsRefQueue_;
    AudioFrameQueue speakerQueue_;
    AudioFrameQueue cleanQueue_;
    TextQueue sttTextQueue_;
    TextQueue llmTextQueue_;
    DiskBackedTextBuffer llmDiskBuffer_;

    std::unique_ptr<IMicrophoneNode> microphone_;
    std::unique_ptr<IAecDspNode> aec_;
    std::unique_ptr<ISpeechToTextNode> stt_;
    std::unique_ptr<ILanguageModelNode> llm_;
    std::unique_ptr<ITextToSpeechNode> tts_;
    std::unique_ptr<IAudioOutputNode> output_;
};

} // namespace voice_runtime
