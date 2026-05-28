#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "AudioFormatAdapter.h"
#include "Interfaces.h"
#include "MetricsReporter.h"
#include "RuntimeConfig.h"
#include "SharedBufferPool.h"
#include "TextBuffer.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// RuntimeState — macchina a stati esplicita
// ---------------------------------------------------------------------------

enum class RuntimeState : int {
    Idle     = 0,
    Running  = 1,
    Stopping = 2,
    Stopped  = 3
};

// ---------------------------------------------------------------------------
// VoiceRuntime
// ---------------------------------------------------------------------------

class VoiceRuntime {
public:
    VoiceRuntime(RuntimeConfig                          config,
                 std::unique_ptr<IMicrophoneNode>       microphone,
                 std::unique_ptr<IAecDspNode>           aec,
                 std::unique_ptr<ISpeechToTextNode>     stt,
                 std::unique_ptr<IBargeInContextNode>   bargeIn,
                 std::unique_ptr<ILanguageModelNode>    llm,
                 std::unique_ptr<ITextToSpeechNode>     tts,
                 std::unique_ptr<IAudioOutputNode>      output)
        : cfg_(std::move(config))
        , audioPool_(cfg_.audioPoolBuffers)
        // --- code audio ---
        , micRawQueue_  (cfg_.micRawQueueCapacity,       cfg_.audioOverflowPolicy, "micRaw")
        , micQueue_     (cfg_.micQueueCapacity,          cfg_.audioOverflowPolicy, "micAdapted")
        , vadGatedQueue_(cfg_.vadGatedQueueCapacity,     cfg_.audioOverflowPolicy, "vadGated")
        , ttsRefQueue_  (cfg_.ttsReferenceQueueCapacity, cfg_.audioOverflowPolicy, "ttsRef")
        , speakerQueue_ (cfg_.speakerQueueCapacity,      cfg_.audioOverflowPolicy, "speaker")
        , cleanQueue_   (cfg_.cleanAudioQueueCapacity,   cfg_.audioOverflowPolicy, "cleanAudio")
        , sttAdaptedQueue_(cfg_.sttAdaptedQueueCapacity, cfg_.audioOverflowPolicy, "sttAdapted")
        // --- code testo ed eventi ---
        , sttTextQueue_ (cfg_.sttTextQueueCapacity,      cfg_.textOverflowPolicy,  "sttText")
        , llmTextQueue_ (cfg_.llmTextQueueCapacity,      cfg_.textOverflowPolicy,  "llmText")
        , vadEventQueue_(cfg_.vadEventQueueCapacity,     QueueOverflowPolicy::DropOldest, "vadEvents")
        , bargeInQueue_ (cfg_.bargeInEventQueueCapacity, QueueOverflowPolicy::DropOldest, "bargeIn")
        // --- buffer LLM ---
        , llmDiskBuffer_(cfg_.llmDiskSpoolPath, cfg_.llmMemoryWindowBytes)
        // --- nodi ---
        , microphone_(std::move(microphone))
        , aec_        (std::move(aec))
        , stt_        (std::move(stt))
        , bargeIn_    (std::move(bargeIn))
        , llm_        (std::move(llm))
        , tts_        (std::move(tts))
        , output_     (std::move(output))
    {
        buildAdapters();
    }

    ~VoiceRuntime() { stop(); }

    // -----------------------------------------------------------------------
    // initialize
    // -----------------------------------------------------------------------

    bool initialize() {
        const int expected = static_cast<int>(RuntimeState::Idle);
        if (state_.load() != expected) return false;

        // Mic → [micAdapter?] → VAD/AEC
        const bool needMicAdapter =
            cfg_.enableMicAdapter &&
            (cfg_.micRawFormat != cfg_.pipelineFormat);

        if (needMicAdapter && micAdapter_) {
            microphone_->setOutputQueue(&micRawQueue_);
            micAdapter_->setInputQueue(&micRawQueue_);
            micAdapter_->setOutputQueue(&micQueue_);
        } else {
            microphone_->setOutputQueue(&micQueue_);
        }

        // VAD (opzionale) → AEC
        if (cfg_.enableVad && vad_) {
            vad_->setInputQueue(&micQueue_);
            vad_->setOutputQueue(&vadGatedQueue_);
            vad_->setEventQueue(&vadEventQueue_);
            vad_->setSpeechThreshold(cfg_.vadSpeechThreshold);
            aec_->setCaptureInputQueue(&vadGatedQueue_);
        } else {
            aec_->setCaptureInputQueue(&micQueue_);
        }

        aec_->setRenderInputQueue(&ttsRefQueue_);
        aec_->setOutputQueue(&cleanQueue_);

        // Se il nodo AEC/DSP integra anche VAD, gli passiamo gli eventi qui.
        // Nei nodi che non supportano VAD questi metodi sono no-op.
        if (cfg_.enableVad) {
            aec_->setVadEventQueue(&vadEventQueue_);
            aec_->setSpeechThreshold(cfg_.vadSpeechThreshold);
        }

        // AEC → [sttAdapter?] → STT
        const bool needSttAdapter =
            cfg_.enableSttAdapter &&
            (cfg_.pipelineFormat != cfg_.sttInputFormat);

        if (needSttAdapter && sttAdapter_) {
            sttAdapter_->setInputQueue(&cleanQueue_);
            sttAdapter_->setOutputQueue(&sttAdaptedQueue_);
            stt_->setInputQueue(&sttAdaptedQueue_);
        } else {
            stt_->setInputQueue(&cleanQueue_);
        }

        stt_->setOutputQueue(&sttTextQueue_);

        // STT → [BargeIn?] → LLM
        if (cfg_.enableBargeIn && bargeIn_) {
            bargeIn_->setTextInputQueue(&sttTextQueue_);
            bargeIn_->setTextOutputQueue(&llmTextQueue_);
            bargeIn_->setBargeInEventQueue(&bargeInQueue_);
            bargeIn_->setTtsInterruptSignal(&ttsInterrupt_);
            bargeIn_->setLlmInterruptSignal(&llmInterrupt_);
            llm_->setInputQueue(&llmTextQueue_);
        } else {
            llm_->setInputQueue(&sttTextQueue_);
        }

        llm_->setOutputQueue(&llmTextQueue_);
        llm_->setPersistentInputBuffer(&llmDiskBuffer_);
        llm_->setInterruptSignal(&llmInterrupt_);

        tts_->setInputQueue(&llmTextQueue_);
        tts_->setSpeakerOutputQueue(&speakerQueue_);
        tts_->setAecReferenceOutputQueue(&ttsRefQueue_);
        tts_->setInterruptSignal(&ttsInterrupt_);

        output_->setInputQueue(&speakerQueue_);

        const bool ok =
            microphone_->initialize() &&
            aec_->initialize()        &&
            stt_->initialize()        &&
            llm_->initialize()        &&
            tts_->initialize()        &&
            output_->initialize()     &&
            (!vad_      || vad_->initialize())      &&
            (!bargeIn_  || bargeIn_->initialize())  &&
            (!micAdapter_|| micAdapter_->initialize()) &&
            (!sttAdapter_|| sttAdapter_->initialize());

        return ok;
    }

    // -----------------------------------------------------------------------
    // start
    // -----------------------------------------------------------------------

    void start() {
        int expected = static_cast<int>(RuntimeState::Idle);
        if (!state_.compare_exchange_strong(expected,
                static_cast<int>(RuntimeState::Running)))
            return;

        if (cfg_.printMetrics) setupMetrics();

        output_->start();
        aec_->start();
        stt_->start();
        if (cfg_.enableBargeIn && bargeIn_) bargeIn_->start();
        llm_->start();
        tts_->start();
        if (cfg_.enableVad && vad_) vad_->start();
        if (micAdapter_) micAdapter_->start();
        if (sttAdapter_) sttAdapter_->start();
        microphone_->start();

        if (metrics_) metrics_->start();
    }

    // -----------------------------------------------------------------------
    // stop
    // -----------------------------------------------------------------------

    void stop() {
        int expected = static_cast<int>(RuntimeState::Running);
        if (!state_.compare_exchange_strong(expected,
                static_cast<int>(RuntimeState::Stopping)))
            return;

        if (metrics_) metrics_->stop();

        // 1. Chiudi tutte le code — sblocca producer e consumer
        stopAllQueues();

        // 2. Ferma i nodi
        microphone_->stop();
        if (micAdapter_) micAdapter_->stop();
        if (vad_)        vad_->stop();
        tts_->stop();
        if (cfg_.enableBargeIn && bargeIn_) bargeIn_->stop();
        llm_->stop();
        stt_->stop();
        if (sttAdapter_) sttAdapter_->stop();
        aec_->stop();
        output_->stop();

        // 3. Svuota le code (rilascia shared_ptr prima che i pool vengano distrutti)
        clearAllQueues();

        // 4. Ferma i pool condivisi del runtime
        audioPool_.stop();

        llmDiskBuffer_.flush();

        state_.store(static_cast<int>(RuntimeState::Stopped));
    }

    // -----------------------------------------------------------------------
    // VAD node — può essere iniettato dall'esterno (es. SileroVAD reale)
    // -----------------------------------------------------------------------

    void setVadNode(std::unique_ptr<IVadNode> vad) {
        vad_ = std::move(vad);
    }

    // -----------------------------------------------------------------------
    // Accessori
    // -----------------------------------------------------------------------

    AudioFrameQueue&     micRawQueue()         { return micRawQueue_;   }
    AudioFrameQueue&     micQueue()            { return micQueue_;      }
    AudioFrameQueue&     vadGatedQueue()       { return vadGatedQueue_; }
    AudioFrameQueue&     ttsReferenceQueue()   { return ttsRefQueue_;   }
    AudioFrameQueue&     speakerQueue()        { return speakerQueue_;  }
    AudioFrameQueue&     cleanQueue()          { return cleanQueue_;    }
    AudioFrameQueue&     sttAdaptedQueue()     { return sttAdaptedQueue_;}
    TextQueue&           sttTextQueue()        { return sttTextQueue_;  }
    TextQueue&           llmTextQueue()        { return llmTextQueue_;  }
    VadEventQueue&       vadEventQueue()       { return vadEventQueue_; }
    BargeInQueue&        bargeInQueue()        { return bargeInQueue_;  }
    DiskBackedTextBuffer& llmDiskBuffer()      { return llmDiskBuffer_; }
    InterruptSignal&     ttsInterrupt()        { return ttsInterrupt_;  }
    InterruptSignal&     llmInterrupt()        { return llmInterrupt_;  }
    SharedBufferPool<AudioFrame>& audioPool()  { return audioPool_;     }

    RuntimeState runtimeState() const {
        return static_cast<RuntimeState>(state_.load());
    }

private:
    void buildAdapters() {
        constexpr std::size_t adapterPoolSize = 64;

        if (cfg_.enableMicAdapter &&
            cfg_.micRawFormat != cfg_.pipelineFormat)
        {
            micAdapter_ = std::make_unique<AudioFormatAdapterNode>(
                "MicAdapter",
                cfg_.micRawFormat,
                cfg_.pipelineFormat,
                adapterPoolSize);
        }

        if (cfg_.enableSttAdapter &&
            cfg_.pipelineFormat != cfg_.sttInputFormat)
        {
            sttAdapter_ = std::make_unique<AudioFormatAdapterNode>(
                "SttAdapter",
                cfg_.pipelineFormat,
                cfg_.sttInputFormat,
                adapterPoolSize);
        }
    }

    void setupMetrics() {
        metrics_ = std::make_unique<MetricsReporter>(cfg_.metricsIntervalMs);
        metrics_->addQueue(micRawQueue_);
        metrics_->addQueue(micQueue_);
        metrics_->addQueue(vadGatedQueue_);
        metrics_->addQueue(cleanQueue_);
        metrics_->addQueue(sttAdaptedQueue_);
        metrics_->addQueue(sttTextQueue_);
        metrics_->addQueue(llmTextQueue_);
        metrics_->addQueue(speakerQueue_);
        metrics_->addQueue(ttsRefQueue_);
        metrics_->addQueue(vadEventQueue_);
        metrics_->addQueue(bargeInQueue_);

        metrics_->setExtraLineCallback([this]() -> std::string {
            char buf[256];
            const std::size_t poolFree = audioPool_.freeCount();
            const std::size_t poolCap  = audioPool_.capacity();
            const std::size_t diskBytes= llmDiskBuffer_.memorySnapshot().size();
            std::snprintf(buf, sizeof(buf),
                "pool=%zu/%zu free | llmMem=%zu B | dspVad=%s | ttsInt=%s | llmInt=%s",
                poolFree, poolCap, diskBytes,
                aec_ && aec_->isSpeaking() ? "speech" : "silence",
                ttsInterrupt_.check() ? "ACTIVE" : "idle",
                llmInterrupt_.check() ? "ACTIVE" : "idle");
            return buf;
        });
    }

    void stopAllQueues() {
        micRawQueue_.stop();
        micQueue_.stop();
        vadGatedQueue_.stop();
        ttsRefQueue_.stop();
        speakerQueue_.stop();
        cleanQueue_.stop();
        sttAdaptedQueue_.stop();
        sttTextQueue_.stop();
        llmTextQueue_.stop();
        vadEventQueue_.stop();
        bargeInQueue_.stop();
    }

    void clearAllQueues() {
        micRawQueue_.clear();
        micQueue_.clear();
        vadGatedQueue_.clear();
        ttsRefQueue_.clear();
        speakerQueue_.clear();
        cleanQueue_.clear();
        sttAdaptedQueue_.clear();
        sttTextQueue_.clear();
        llmTextQueue_.clear();
        vadEventQueue_.clear();
        bargeInQueue_.clear();
    }

    // -----------------------------------------------------------------------
    // Dati membro
    // -----------------------------------------------------------------------

    std::atomic<int>    state_{static_cast<int>(RuntimeState::Idle)};
    RuntimeConfig       cfg_;
    SharedBufferPool<AudioFrame> audioPool_;

    // Code audio
    AudioFrameQueue     micRawQueue_;
    AudioFrameQueue     micQueue_;
    AudioFrameQueue     vadGatedQueue_;
    AudioFrameQueue     ttsRefQueue_;
    AudioFrameQueue     speakerQueue_;
    AudioFrameQueue     cleanQueue_;
    AudioFrameQueue     sttAdaptedQueue_;

    // Code testo/eventi
    TextQueue           sttTextQueue_;
    TextQueue           llmTextQueue_;
    VadEventQueue       vadEventQueue_;
    BargeInQueue        bargeInQueue_;

    // Buffer LLM
    DiskBackedTextBuffer llmDiskBuffer_;

    // Segnali di interrupt
    InterruptSignal     ttsInterrupt_;
    InterruptSignal     llmInterrupt_;

    // Nodi
    std::unique_ptr<IMicrophoneNode>       microphone_;
    std::unique_ptr<IAudioFormatAdapterNode> micAdapter_;
    std::unique_ptr<IVadNode>              vad_;
    std::unique_ptr<IAecDspNode>           aec_;
    std::unique_ptr<IAudioFormatAdapterNode> sttAdapter_;
    std::unique_ptr<ISpeechToTextNode>     stt_;
    std::unique_ptr<IBargeInContextNode>   bargeIn_;
    std::unique_ptr<ILanguageModelNode>    llm_;
    std::unique_ptr<ITextToSpeechNode>     tts_;
    std::unique_ptr<IAudioOutputNode>      output_;

    // Metriche
    std::unique_ptr<MetricsReporter>       metrics_;
};

} // namespace voice_runtime
