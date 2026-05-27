#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>
#include "AudioTypes.h"

namespace voice_runtime {

template <typename T>
class BufferPool {
public:
    explicit BufferPool(std::size_t capacity)
        : buffers_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i) {
            free_.push(&buffers_[i]);
        }
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    T* acquireBlocking() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !free_.empty() || stopped_; });
        if (stopped_) {
            return nullptr;
        }
        T* item = free_.front();
        free_.pop();
        ++stats_.consumed;
        return item;
    }

    T* tryAcquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty() || stopped_) {
            ++stats_.dropped;
            return nullptr;
        }
        T* item = free_.front();
        free_.pop();
        ++stats_.consumed;
        return item;
    }

    void release(T* item) {
        if (!item) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push(item);
            ++stats_.recycled;
            const std::size_t used = buffers_.size() - free_.size();
            if (used > stats_.highWatermark) stats_.highWatermark = used;
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    RuntimeStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    std::size_t capacity() const { return buffers_.size(); }

private:
    std::vector<T> buffers_;
    std::queue<T*> free_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    RuntimeStats stats_;
};

} // namespace voice_runtime
