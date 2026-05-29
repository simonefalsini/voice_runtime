#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
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
    using DropCallback = std::function<void(T)>;

    BoundedQueue(std::size_t capacity, QueueOverflowPolicy policy,
                 const char* name = "")
        : capacity_(capacity), policy_(policy), name_(name) {}

    BoundedQueue(const BoundedQueue&)            = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // -----------------------------------------------------------------------
    // Configurazione
    // -----------------------------------------------------------------------

    void setDropCallback(DropCallback cb) {
        std::lock_guard<std::mutex> lk(mutex_);
        dropCallback_ = std::move(cb);
    }

    const char* name() const noexcept { return name_; }

    // -----------------------------------------------------------------------
    // push — ritorna true se l'item è stato accodato
    // -----------------------------------------------------------------------

    bool push(T item) {
        T       evicted{};
        bool    hasEvicted  = false;
        bool    dropped     = false;

        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (stopped_) return false;

            if (policy_ == QueueOverflowPolicy::BlockProducer) {
                notFull_.wait(lk, [&] {
                    return queue_.size() < capacity_ || stopped_;
                });
                if (stopped_) return false;

            } else if (queue_.size() >= capacity_) {
                if (policy_ == QueueOverflowPolicy::DropNewest) {
                    ++stats_.dropped;
                    dropped = true;
                    // item verrà distrutto fuori dal lock
                } else { // DropOldest
                    evicted    = std::move(queue_.front());
                    queue_.pop_front();
                    hasEvicted = true;
                    ++stats_.dropped;
                }
            }

            if (!dropped) {
                queue_.push_back(std::move(item));
                ++stats_.produced;
                if (queue_.size() > stats_.highWatermark)
                    stats_.highWatermark = queue_.size();
            }
        } // ← mutex rilasciato qui

        // Distruzione/callback fuori dal lock
        if (dropped) {
            invokeDropCallback(std::move(item));
            return false;
        }
        if (hasEvicted) {
            invokeDropCallback(std::move(evicted));
        }

        notEmpty_.notify_one();
        return true;
    }

    // -----------------------------------------------------------------------
    // pop — bloccante; ritorna false se stopped e la coda è vuota
    // -----------------------------------------------------------------------

    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mutex_);
        notEmpty_.wait(lk, [&] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        ++stats_.consumed;
        lk.unlock();
        notFull_.notify_one();
        return true;
    }


    // -----------------------------------------------------------------------
    // tryPop — non bloccante; ritorna false se vuota oppure stopped e vuota
    // -----------------------------------------------------------------------

    bool tryPop(T& out) {
        std::unique_lock<std::mutex> lk(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        ++stats_.consumed;
        lk.unlock();
        notFull_.notify_one();
        return true;
    }

    // -----------------------------------------------------------------------
    // popWithTimeout — blocks up to `timeout`; returns false on timeout/stop
    // -----------------------------------------------------------------------

    bool popWithTimeout(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mutex_);
        const bool ok = notEmpty_.wait_for(lk, timeout, [&] {
            return !queue_.empty() || stopped_;
        });
        if (!ok || queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        ++stats_.consumed;
        lk.unlock();
        notFull_.notify_one();
        return true;
    }

    // -----------------------------------------------------------------------
    // Controllo ciclo di vita
    // -----------------------------------------------------------------------

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stopped_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void clear() {
        std::deque<T> old;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            old.swap(queue_);
        }
        notFull_.notify_all();
        // old distrutto fuori dal lock
    }

    void stopAndClear() {
        std::deque<T> old;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stopped_ = true;
            old.swap(queue_);
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const noexcept { return capacity_; }

    RuntimeStats stats() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return stats_;
    }

    QueueSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mutex_);
        QueueSnapshot s;
        s.name          = name_;
        s.currentSize   = queue_.size();
        s.capacity      = capacity_;
        s.produced      = stats_.produced;
        s.consumed      = stats_.consumed;
        s.dropped       = stats_.dropped;
        s.highWatermark = stats_.highWatermark;
        return s;
    }

private:
    void invokeDropCallback(T item) {
        DropCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = dropCallback_;
        }
        if (cb) cb(std::move(item));
    }

    std::size_t          capacity_;
    QueueOverflowPolicy  policy_;
    const char*          name_;
    std::deque<T>        queue_;
    mutable std::mutex   mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    bool                 stopped_      = false;
    RuntimeStats         stats_;
    DropCallback         dropCallback_;
};

} // namespace voice_runtime
