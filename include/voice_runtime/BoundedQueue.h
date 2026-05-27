#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include "AudioTypes.h"

namespace voice_runtime {

enum class QueueOverflowPolicy {
    BlockProducer,
    DropNewest,
    DropOldest
};

template <typename T>
class BoundedQueue {
public:
    BoundedQueue(std::size_t capacity, QueueOverflowPolicy policy)
        : capacity_(capacity), policy_(policy) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(T item) {
        T droppedItem{};
        bool hasDroppedItem = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_) return false;

            if (policy_ == QueueOverflowPolicy::BlockProducer) {
                notFull_.wait(lock, [&] { return queue_.size() < capacity_ || stopped_; });
                if (stopped_) return false;
            } else if (queue_.size() >= capacity_) {
                if (policy_ == QueueOverflowPolicy::DropNewest) {
                    ++stats_.dropped;
                    return false;
                }

                droppedItem = std::move(queue_.front());
                queue_.pop_front();
                hasDroppedItem = true;
                ++stats_.dropped;
            }

            queue_.push_back(std::move(item));
            ++stats_.produced;
            if (queue_.size() > stats_.highWatermark) stats_.highWatermark = queue_.size();
        }

        if (hasDroppedItem) {
            droppedItem = T{};
        }

        notEmpty_.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        ++stats_.consumed;
        lock.unlock();
        notFull_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void clear() {
        std::deque<T> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old.swap(queue_);
        }
        notFull_.notify_all();
    }

    void stopAndClear() {
        std::deque<T> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            old.swap(queue_);
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    RuntimeStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    std::size_t capacity_ = 0;
    QueueOverflowPolicy policy_ = QueueOverflowPolicy::BlockProducer;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    bool stopped_ = false;
    RuntimeStats stats_;
};

} // namespace voice_runtime
