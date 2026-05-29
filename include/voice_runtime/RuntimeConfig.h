#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include "AudioTypes.h"
#include "BoundedQueue.h"

namespace voice_runtime {

struct RuntimeConfig {
    // -----------------------------------------------------------------------
    // Formati audio
    // -----------------------------------------------------------------------

    // Formato nativo del microfono (es. 48kHz stereo Float32)
    AudioFormat micRawFormat;

    // Formato interno della pipeline (default: 16kHz mono Int16)
    AudioFormat pipelineFormat;

    // Formato richiesto dall'STT (se diverso da pipelineFormat viene inserito un adapter)
    AudioFormat sttInputFormat;


    // -----------------------------------------------------------------------
    // Pool e code — audio
    // -----------------------------------------------------------------------

    std::size_t audioPoolBuffers             = 512;
    std::size_t micRawQueueCapacity          = 64;   // raw mic → adapter
    std::size_t micQueueCapacity             = 128;  // adapted mic → VAD
    std::size_t vadGatedQueueCapacity        = 128;  // VAD → AEC
    std::size_t ttsReferenceQueueCapacity    = 128;
    std::size_t speakerQueueCapacity         = 128;
    std::size_t cleanAudioQueueCapacity      = 128;
    std::size_t sttAdaptedQueueCapacity      = 64;   // AEC → STT adapter → STT

    // -----------------------------------------------------------------------
    // Code — testo ed eventi
    // -----------------------------------------------------------------------

    std::size_t sttTextQueueCapacity         = 512;
    std::size_t llmTextQueueCapacity         = 256;
    std::size_t vadEventQueueCapacity        = 64;
    std::size_t bargeInEventQueueCapacity    = 16;
    std::size_t classifierOutQueueCapacity   = 256;
    std::size_t llmOutputQueueCapacity       = 256;   // LLM → Interpreter
    std::size_t ttsInputQueueCapacity        = 256;   // Interpreter → TTS

    // -----------------------------------------------------------------------
    // Politiche di overflow
    // -----------------------------------------------------------------------

    QueueOverflowPolicy audioOverflowPolicy  = QueueOverflowPolicy::DropOldest;
    QueueOverflowPolicy textOverflowPolicy   = QueueOverflowPolicy::DropOldest;

    // -----------------------------------------------------------------------
    // LLM persistence
    // -----------------------------------------------------------------------

    std::size_t llmMemoryWindowBytes         = 256 * 1024;
    std::string llmDiskSpoolPath             = "voice_runtime_llm_input.log";

    // -----------------------------------------------------------------------
    // VAD
    // -----------------------------------------------------------------------

    bool  enableVad                          = true;
    bool  enableIntegratedDspVad             = false;
    float vadSpeechThreshold                 = 0.5f;

    // -----------------------------------------------------------------------
    // Barge-in
    // -----------------------------------------------------------------------

    bool  enableBargeIn                      = true;

    // -----------------------------------------------------------------------
    // Overlap buffer — commenti utente durante TTS
    // -----------------------------------------------------------------------

    std::size_t overlapCommentBufferBytes    = 32 * 1024;
    bool overlapBufferUnbounded              = false;  // true = mantieni tutto

    // -----------------------------------------------------------------------
    // AEC warm-up grace period
    // -----------------------------------------------------------------------

    int aecWarmupGracePeriodMs               = 200;

    // -----------------------------------------------------------------------
    // Adapter — inseriti automaticamente se i formati differiscono
    // -----------------------------------------------------------------------

    bool  enableMicAdapter                   = true;
    bool  enableSttAdapter                   = true;

    // -----------------------------------------------------------------------
    // Metriche periodiche
    // -----------------------------------------------------------------------

    std::chrono::milliseconds metricsIntervalMs{1000};
    bool  printMetrics                       = true;

    // -----------------------------------------------------------------------
    // Costruttore — imposta default coerenti
    // -----------------------------------------------------------------------

    RuntimeConfig() {
        // Pipeline interna: 16kHz mono Int16, frame 10ms
        pipelineFormat.sampleRate   = 16000;
        pipelineFormat.channels     = 1;
        pipelineFormat.frameMs      = 10;
        pipelineFormat.sampleFormat = SampleFormat::Int16;

        // Per default il mic ha lo stesso formato della pipeline
        micRawFormat  = pipelineFormat;
        sttInputFormat = pipelineFormat;

    }
};

} // namespace voice_runtime
