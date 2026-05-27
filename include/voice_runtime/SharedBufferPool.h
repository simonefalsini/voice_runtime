#pragma once

#include <chrono>
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
class SharedBufferPool {
private:
    struct State {
        explicit State(std::size_t capacity) : buffers(capacity) {
            for (auto& b : buffers) free.push(&b);
        }

        std::vector<T>      buffers;
        std::queue<T*>      free;
        mutable std::mutex  mutex;
        std::condition_variable cv;
        bool                stopped = false;
        RuntimeStats        stats;
    };

public:
    using Handle = std::shared_ptr<T>;

    explicit SharedBufferPool(std::size_t capacity)
        : state_(std::make_shared<State>(capacity)) {
        if (capacity == 0)
            throw std::invalid_argument("SharedBufferPool: capacity must be > 0");
    }

    SharedBufferPool(const SharedBufferPool&)            = delete;
    SharedBufferPool& operator=(const SharedBufferPool&) = delete;
    SharedBufferPool(SharedBufferPool&&) noexcept        = default;
    SharedBufferPool& operator=(SharedBufferPool&&) noexcept = default;

    ~SharedBufferPool() { stop(); }

    // Bloccante senza timeout
    Handle acquireBlocking() {
        auto state = state_;
        if (!state) return {};

        std::unique_lock<std::mutex> lk(state->mutex);
        state->cv.wait(lk, [&] {
            return state->stopped || !state->free.empty();
        });
        if (state->stopped || state->free.empty()) return {};

        return makeHandle(popAndStat(*state), state);
    }

    // Bloccante con timeout — ritorna Handle vuoto se scade
    Handle acquireWithTimeout(std::chrono::milliseconds timeout) {
        auto state = state_;
        if (!state) return {};

        std::unique_lock<std::mutex> lk(state->mutex);
        const bool ok = state->cv.wait_for(lk, timeout, [&] {
            return state->stopped || !state->free.empty();
        });
        if (!ok || state->stopped || state->free.empty()) {
            ++state->stats.dropped;
            return {};
        }
        return makeHandle(popAndStat(*state), state);
    }

    // Non bloccante
    Handle tryAcquire() {
        auto state = state_;
        if (!state) return {};

        std::lock_guard<std::mutex> lk(state->mutex);
        if (state->stopped || state->free.empty()) {
            ++state->stats.dropped;
            return {};
        }
        return makeHandle(popAndStat(*state), state);
    }

    void stop() {
        auto state = state_;
        if (!state) return;
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            state->stopped = true;
        }
        state->cv.notify_all();
    }

    RuntimeStats stats() const {
        auto state = state_;
        if (!state) return {};
        std::lock_guard<std::mutex> lk(state->mutex);
        return state->stats;
    }

    std::size_t capacity() const {
        auto state = state_;
        return state ? state->buffers.size() : 0;
    }

    std::size_t freeCount() const {
        auto state = state_;
        if (!state) return 0;
        std::lock_guard<std::mutex> lk(state->mutex);
        return state->free.size();
    }

private:
    static T* popAndStat(State& s) {
        T* raw = s.free.front();
        s.free.pop();
        ++s.stats.consumed;
        const std::size_t used = s.buffers.size() - s.free.size();
        if (used > s.stats.highWatermark) s.stats.highWatermark = used;
        return raw;
    }

    static Handle makeHandle(T* raw, std::shared_ptr<State> state) {
        return Handle(raw, [st = std::move(state)](T* item) mutable {
            if (!st || !item) return;
            {
                std::lock_guard<std::mutex> lk(st->mutex);
                st->free.push(item);
                ++st->stats.recycled;
            }
            st->cv.notify_one();
        });
    }

    std::shared_ptr<State> state_;
};

} // namespace voice_runtime
