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
//   Pipeline lineare unica:
//   Mic → [MicAdapter?] → VAD → AEC → [SttAdapter?] → STT
//       → Classifier/BargeIn → LLM → Interpreter → TTS → Output
//
//   I nodi commutano tra attivo e pass-through in base a TtsStateSignal.
// ---------------------------------------------------------------------------

class VoiceRuntime {
public:
    VoiceRuntime(RuntimeConfig                              config,
                 std::unique_ptr<IMicrophoneNode>           microphone,
                 std::unique_ptr<IAecDspNode>               aec,
                 std::unique_ptr<ISpeechToTextNode>         stt,
                 std::unique_ptr<IClassifierBargeInNode>    classifier,
                 std::unique_ptr<ILanguageModelNode>        llm,
                 std::unique_ptr<IInterpreterNode>          interpreter,
                 std::unique_ptr<ITextToSpeechNode>         tts,
                 std::unique_ptr<IAudioOutputNode>          output)
        : cfg_(std::move(config))
        , audioPool_(cfg_.audioPoolBuffers)
        // --- code audio ---
        , micRawQueue_  (cfg_.micRawQueueCapacity,       cfg_.audioOverflowPolicy, "micRaw")
        , micQueue_     (cfg_.micQueueCapacity,          cfg_.audioOverflowPolicy, "micAdapted")
        , vadGatedQueue_(cfg_.vadGatedQueueCapacity,     cfg_.audioOverflowPolicy, "vadOut")
        , ttsRefQueue_  (16, QueueOverflowPolicy::BlockProducer, "ttsRef")
        , speakerQueue_ (16, QueueOverflowPolicy::BlockProducer, "speaker")
        , cleanQueue_   (cfg_.cleanAudioQueueCapacity,   cfg_.audioOverflowPolicy, "cleanAudio")
        , sttAdaptedQueue_(cfg_.sttAdaptedQueueCapacity, cfg_.audioOverflowPolicy, "sttAdapted")
        // --- code testo ed eventi ---
        , sttTextQueue_       (cfg_.sttTextQueueCapacity,       cfg_.textOverflowPolicy,  "sttText")
        , classifierOutQueue_ (cfg_.classifierOutQueueCapacity, cfg_.textOverflowPolicy,  "classOut")
        , llmOutputQueue_     (cfg_.llmOutputQueueCapacity,     cfg_.textOverflowPolicy,  "llmOut")
        , ttsInputQueue_      (cfg_.ttsInputQueueCapacity,      cfg_.textOverflowPolicy,  "ttsIn")
        , vadEventQueue_      (cfg_.vadEventQueueCapacity,      QueueOverflowPolicy::DropOldest, "vadEvents")
        , bargeInQueue_       (cfg_.bargeInEventQueueCapacity,  QueueOverflowPolicy::DropOldest, "bargeIn")
        // --- buffer LLM ---
        , llmDiskBuffer_(cfg_.llmDiskSpoolPath, cfg_.llmMemoryWindowBytes)
        // --- buffer overlap ---
        , overlapBuffer_(cfg_.overlapCommentBufferBytes, cfg_.overlapBufferUnbounded)
        // --- nodi ---
        , microphone_  (std::move(microphone))
        , aec_         (std::move(aec))
        , stt_         (std::move(stt))
        , classifier_  (std::move(classifier))
        , llm_         (std::move(llm))
        , interpreter_ (std::move(interpreter))
        , tts_         (std::move(tts))
        , output_      (std::move(output))
    {
        buildAdapters();
    }

    ~VoiceRuntime() { stop(); }

    // -----------------------------------------------------------------------
    // initialize — wiring della pipeline lineare
    // -----------------------------------------------------------------------

    bool initialize() {
        const int expected = static_cast<int>(RuntimeState::Idle);
        if (state_.load() != expected) return false;

        // ===================================================================
        // 1. Mic → [MicAdapter?] → micQueue
        // ===================================================================

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

        // ===================================================================
        // 2. micQueue → VAD → vadGatedQueue (or integrated VAD in DSP)
        // ===================================================================

        const bool useIntegratedVad = cfg_.enableIntegratedDspVad && (dynamic_cast<IVadNode*>(aec_.get()) != nullptr);
        const bool useExternalVad = cfg_.enableVad && vad_ && !useIntegratedVad;

        if (useExternalVad) {
            vad_->setInputQueue(&micQueue_);
            vad_->setOutputQueue(&vadGatedQueue_);
            vad_->setEventQueue(&vadEventQueue_);
            vad_->setSpeechThreshold(cfg_.vadSpeechThreshold);
            vad_->setTtsStateSignal(&ttsState_);
        }

        // ===================================================================
        // 3. vadGatedQueue/micQueue → AEC → cleanQueue
        // ===================================================================

        if (useExternalVad) {
            aec_->setCaptureInputQueue(&vadGatedQueue_);
        } else {
            aec_->setCaptureInputQueue(&micQueue_);
        }

        aec_->setRenderInputQueue(&ttsRefQueue_);
        aec_->setOutputQueue(&cleanQueue_);
        aec_->setTtsStateSignal(&ttsState_);

        // Se il nodo AEC/DSP integra anche VAD, gli passiamo gli eventi.
        if (cfg_.enableVad) {
            aec_->setVadEventQueue(&vadEventQueue_);
            aec_->setSpeechThreshold(cfg_.vadSpeechThreshold);
        }

        // ===================================================================
        // 4. cleanQueue → [SttAdapter?] → STT → sttTextQueue
        // ===================================================================

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

        // ===================================================================
        // 5. sttTextQueue → Classifier/BargeIn → classifierOutQueue
        // ===================================================================

        classifier_->setTextInputQueue(&sttTextQueue_);
        classifier_->setTextOutputQueue(&classifierOutQueue_);
        classifier_->setBargeInEventQueue(&bargeInQueue_);
        classifier_->setTtsInterruptSignal(&ttsInterrupt_);
        classifier_->setLlmInterruptSignal(&llmInterrupt_);
        classifier_->setTtsStateSignal(&ttsState_);
        classifier_->setOverlapBuffer(&overlapBuffer_);

        // ===================================================================
        // 6. classifierOutQueue → LLM → llmOutputQueue
        // ===================================================================

        llm_->setInputQueue(&classifierOutQueue_);
        llm_->setOutputQueue(&llmOutputQueue_);
        llm_->setPersistentInputBuffer(&llmDiskBuffer_);
        llm_->setInterruptSignal(&llmInterrupt_);

        // ===================================================================
        // 7. llmOutputQueue → Interpreter → ttsInputQueue
        // ===================================================================

        interpreter_->setInputQueue(&llmOutputQueue_);
        interpreter_->setOutputQueue(&ttsInputQueue_);

        // ===================================================================
        // 8. ttsInputQueue → TTS → speakerQueue + ttsRefQueue
        // ===================================================================

        tts_->setInputQueue(&ttsInputQueue_);
        tts_->setSpeakerOutputQueue(&speakerQueue_);
        tts_->setAecReferenceOutputQueue(&ttsRefQueue_);
        tts_->setInterruptSignal(&ttsInterrupt_);
        tts_->setTtsStateSignal(&ttsState_);

        // ===================================================================
        // 9. speakerQueue → AudioOutput
        // ===================================================================

        output_->setInputQueue(&speakerQueue_);

        // Inizializza tutti i nodi
        // ===================================================================

        const bool ok =
            microphone_->initialize() &&
            aec_->initialize()        &&
            stt_->initialize()        &&
            classifier_->initialize() &&
            llm_->initialize()        &&
            interpreter_->initialize()&&
            tts_->initialize()        &&
            output_->initialize()     &&
            (!useExternalVad || vad_->initialize())       &&
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

        const bool useIntegratedVad = cfg_.enableIntegratedDspVad && (dynamic_cast<IVadNode*>(aec_.get()) != nullptr);
        const bool useExternalVad = cfg_.enableVad && vad_ && !useIntegratedVad;

        // Avvia i nodi dal fondo della pipeline verso la sorgente
        output_->start();
        tts_->start();
        interpreter_->start();
        llm_->start();
        classifier_->start();
        stt_->start();
        aec_->start();
        if (useExternalVad) vad_->start();
        if (sttAdapter_) sttAdapter_->start();
        if (micAdapter_) micAdapter_->start();
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

        // 1. Ferma metriche
        if (metrics_) metrics_->stop();

        // 2. Chiudi tutte le code — sblocca producer e consumer
        stopAllQueues();

        const bool useIntegratedVad = cfg_.enableIntegratedDspVad && (dynamic_cast<IVadNode*>(aec_.get()) != nullptr);
        const bool useExternalVad = cfg_.enableVad && vad_ && !useIntegratedVad;

        // 3. Ferma i nodi (dalla sorgente al fondo)
        microphone_->stop();
        if (micAdapter_) micAdapter_->stop();
        if (useExternalVad) vad_->stop();
        aec_->stop();
        if (sttAdapter_) sttAdapter_->stop();
        stt_->stop();
        classifier_->stop();
        llm_->stop();
        interpreter_->stop();
        tts_->stop();
        output_->stop();

        // 4. Svuota le code (rilascia shared_ptr prima che i pool vengano distrutti)
        clearAllQueues();

        // 5. Ferma i pool condivisi del runtime
        audioPool_.stop();

        // 6. Flush buffer su disco
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

    AudioFrameQueue&     micRawQueue()           { return micRawQueue_;        }
    AudioFrameQueue&     micQueue()              { return micQueue_;           }
    AudioFrameQueue&     vadGatedQueue()         { return vadGatedQueue_;      }
    AudioFrameQueue&     ttsReferenceQueue()     { return ttsRefQueue_;        }
    AudioFrameQueue&     speakerQueue()          { return speakerQueue_;       }
    AudioFrameQueue&     cleanQueue()            { return cleanQueue_;         }
    AudioFrameQueue&     sttAdaptedQueue()       { return sttAdaptedQueue_;    }
    TextQueue&           sttTextQueue()          { return sttTextQueue_;       }
    TextQueue&           classifierOutQueue()    { return classifierOutQueue_; }
    TextQueue&           llmOutputQueue()        { return llmOutputQueue_;     }
    TextQueue&           ttsInputQueue()         { return ttsInputQueue_;      }
    VadEventQueue&       vadEventQueue()         { return vadEventQueue_;      }
    BargeInQueue&        bargeInQueue()          { return bargeInQueue_;       }
    DiskBackedTextBuffer& llmDiskBuffer()        { return llmDiskBuffer_;      }
    RollingTextBuffer&   overlapBuffer()         { return overlapBuffer_;      }
    InterruptSignal&     ttsInterrupt()          { return ttsInterrupt_;       }
    InterruptSignal&     llmInterrupt()          { return llmInterrupt_;       }
    TtsStateSignal&      ttsState()              { return ttsState_;           }
    SharedBufferPool<AudioFrame>& audioPool()    { return audioPool_;          }

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
        metrics_->addQueue(classifierOutQueue_);
        metrics_->addQueue(llmOutputQueue_);
        metrics_->addQueue(ttsInputQueue_);
        metrics_->addQueue(speakerQueue_);
        metrics_->addQueue(ttsRefQueue_);
        metrics_->addQueue(vadEventQueue_);
        metrics_->addQueue(bargeInQueue_);

        metrics_->setExtraLineCallback([this]() -> std::string {
            char buf[256];
            const std::size_t poolFree = audioPool_.freeCount();
            const std::size_t poolCap  = audioPool_.capacity();
            const std::size_t diskBytes= llmDiskBuffer_.memorySnapshot().size();
            const bool ttsActive = ttsState_.isActive();
            std::snprintf(buf, sizeof(buf),
                "pool=%zu/%zu free | llmMem=%zu B | tts=%s | ttsInt=%s | llmInt=%s",
                poolFree, poolCap, diskBytes,
                ttsActive ? "ACTIVE" : "idle",
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
        classifierOutQueue_.stop();
        llmOutputQueue_.stop();
        ttsInputQueue_.stop();
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
        classifierOutQueue_.clear();
        llmOutputQueue_.clear();
        ttsInputQueue_.clear();
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
    AudioFrameQueue     vadGatedQueue_;     // VAD → AEC
    AudioFrameQueue     ttsRefQueue_;
    AudioFrameQueue     speakerQueue_;
    AudioFrameQueue     cleanQueue_;        // AEC → STT
    AudioFrameQueue     sttAdaptedQueue_;

    // Code testo/eventi
    TextQueue           sttTextQueue_;       // STT → Classifier
    TextQueue           classifierOutQueue_; // Classifier → LLM
    TextQueue           llmOutputQueue_;     // LLM → Interpreter
    TextQueue           ttsInputQueue_;      // Interpreter → TTS
    VadEventQueue       vadEventQueue_;
    BargeInQueue        bargeInQueue_;

    // Buffer LLM
    DiskBackedTextBuffer llmDiskBuffer_;

    // Buffer overlap — commenti utente durante TTS
    RollingTextBuffer   overlapBuffer_;

    // Segnali
    InterruptSignal     ttsInterrupt_;
    InterruptSignal     llmInterrupt_;
    TtsStateSignal      ttsState_;

    // Nodi
    std::unique_ptr<IMicrophoneNode>             microphone_;
    std::unique_ptr<IAudioFormatAdapterNode>     micAdapter_;
    std::unique_ptr<IVadNode>                    vad_;
    std::unique_ptr<IAecDspNode>                 aec_;
    std::unique_ptr<IAudioFormatAdapterNode>     sttAdapter_;
    std::unique_ptr<ISpeechToTextNode>           stt_;
    std::unique_ptr<IClassifierBargeInNode>      classifier_;
    std::unique_ptr<ILanguageModelNode>          llm_;
    std::unique_ptr<IInterpreterNode>            interpreter_;
    std::unique_ptr<ITextToSpeechNode>           tts_;
    std::unique_ptr<IAudioOutputNode>            output_;

    // Metriche
    std::unique_ptr<MetricsReporter>             metrics_;
};

} // namespace voice_runtime
