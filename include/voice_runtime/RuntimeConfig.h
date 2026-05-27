#pragma once

#include <cstddef>
#include <string>
#include "AudioTypes.h"
#include "BoundedQueue.h"

namespace voice_runtime {

struct RuntimeConfig {
    AudioFormat audioFormat;

    std::size_t audioPoolBuffers = 512;
    std::size_t micQueueCapacity = 64;
    std::size_t ttsReferenceQueueCapacity = 64;
    std::size_t speakerQueueCapacity = 64;
    std::size_t cleanAudioQueueCapacity = 64;

    std::size_t sttTextQueueCapacity = 256;
    std::size_t llmTextQueueCapacity = 256;

    QueueOverflowPolicy audioOverflowPolicy = QueueOverflowPolicy::BlockProducer;
    QueueOverflowPolicy textOverflowPolicy = QueueOverflowPolicy::DropOldest;

    std::size_t llmMemoryWindowBytes = 256 * 1024;
    std::string llmDiskSpoolPath = "voice_runtime_llm_input.log";
};

} // namespace voice_runtime
