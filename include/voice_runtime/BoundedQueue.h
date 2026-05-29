#pragma once

#include <atomic>
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

    // nodo che usa questa coda. Il callback è fisso per l'intera esecuzione e
    // viene letto senza lock in invokeDropCallback (safe per precondizione).
    void setDropCallback(DropCallback cb) {
        std::lock_guard<std::mutex> lk(mutex_);
        dropCallback_ = std::move(cb);
        hasDropCallback_.store(dropCallback_ != nullptr, std::memory_order_release);
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
                    // FIX 4.6: atomic increment — no mutex needed for counter
                    atomicDropped_.fetch_add(1, std::memory_order_relaxed);
                    dropped = true;
                    // item verrà distrutto fuori dal lock
                } else { // DropOldest
                    evicted    = std::move(queue_.front());
                    queue_.pop_front();
                    hasEvicted = true;
                    // FIX 4.6: atomic increment
                    atomicDropped_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (!dropped) {
                queue_.push_back(std::move(item));
                // FIX 4.6: atomic increment — readers can sample without the mutex
                atomicProduced_.fetch_add(1, std::memory_order_relaxed);
                // highWatermark still needs mutex (depends on queue_.size())
                if (queue_.size() > hwm_) hwm_ = queue_.size();
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
        // FIX 4.6: atomic increment
        atomicConsumed_.fetch_add(1, std::memory_order_relaxed);
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
        // FIX 4.6: atomic increment
        atomicConsumed_.fetch_add(1, std::memory_order_relaxed);
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
        // FIX 4.6: atomic increment
        atomicConsumed_.fetch_add(1, std::memory_order_relaxed);
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

    // FIX 4.6: produced/consumed/dropped letti da atomici senza acquisire il mutex.
    // Solo highWatermark richiede il lock (dipende da queue_.size()).
    // Questo permette al MetricsReporter di campionare le statistiche senza
    // bloccare producer/consumer della coda.
    RuntimeStats stats() const {
        RuntimeStats s;
        s.produced      = atomicProduced_.load(std::memory_order_relaxed);
        s.consumed      = atomicConsumed_.load(std::memory_order_relaxed);
        s.dropped       = atomicDropped_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            s.highWatermark = hwm_;
        }
        return s;
    }

    QueueSnapshot snapshot() const {
        QueueSnapshot s;
        s.name      = name_;
        s.capacity  = capacity_;
        s.produced  = atomicProduced_.load(std::memory_order_relaxed);
        s.consumed  = atomicConsumed_.load(std::memory_order_relaxed);
        s.dropped   = atomicDropped_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            s.currentSize   = queue_.size();
            s.highWatermark = hwm_;
        }
        return s;
    }

private:
    // FIX 4.4: invokeDropCallback senza re-acquisire il mutex.
    // Precondizione: setDropCallback() deve essere chiamato prima di start().
    // Il callback è fisso per tutta l'esecuzione; letto senza lock è safe.
    void invokeDropCallback(T item) {
        if (!hasDropCallback_.load(std::memory_order_acquire)) return;
        dropCallback_(std::move(item));
    }

    std::size_t          capacity_;
    QueueOverflowPolicy  policy_;
    const char*          name_;
    std::deque<T>        queue_;
    mutable std::mutex   mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    bool                 stopped_ = false;

    // FIX 4.6: contatori atomici — letti senza lock da MetricsReporter e stats()
    std::atomic<uint64_t> atomicProduced_{0};
    std::atomic<uint64_t> atomicConsumed_{0};
    std::atomic<uint64_t> atomicDropped_{0};

    // highWatermark sotto mutex (dipende da queue_.size())
    std::size_t          hwm_ = 0;

    // FIX 4.4: flag atomico — evita re-lock in invokeDropCallback
    std::atomic<bool>    hasDropCallback_{false};
    DropCallback         dropCallback_;
};

} // namespace voice_runtime
