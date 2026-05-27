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
class SharedBufferPool {
private:
    struct State {
        explicit State(std::size_t capacity)
            : buffers(capacity) {
            for (auto& b : buffers) {
                free.push(&b);
            }
        }

        std::vector<T> buffers;
        std::queue<T*> free;
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool stopped = false;
        RuntimeStats stats;
    };

public:
    using Handle = std::shared_ptr<T>;

    explicit SharedBufferPool(std::size_t capacity)
        : state_(std::make_shared<State>(capacity)) {
        if (capacity == 0) {
            throw std::invalid_argument("SharedBufferPool capacity must be greater than zero");
        }
    }

    SharedBufferPool(const SharedBufferPool&) = delete;
    SharedBufferPool& operator=(const SharedBufferPool&) = delete;

    SharedBufferPool(SharedBufferPool&&) noexcept = default;
    SharedBufferPool& operator=(SharedBufferPool&&) noexcept = default;

    ~SharedBufferPool() {
        stop();
    }

    Handle acquireBlocking() {
        auto state = state_;
        if (!state) {
            return {};
        }

        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&] { return state->stopped || !state->free.empty(); });

        if (state->stopped || state->free.empty()) {
            return {};
        }

        T* raw = state->free.front();
        state->free.pop();
        ++state->stats.consumed;
        updateHighWatermarkLocked(*state);

        return makeHandle(raw, std::move(state));
    }

    Handle tryAcquire() {
        auto state = state_;
        if (!state) {
            return {};
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopped || state->free.empty()) {
            ++state->stats.dropped;
            return {};
        }

        T* raw = state->free.front();
        state->free.pop();
        ++state->stats.consumed;
        updateHighWatermarkLocked(*state);

        return makeHandle(raw, std::move(state));
    }

    void stop() {
        auto state = state_;
        if (!state) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopped = true;
        }
        state->cv.notify_all();
    }

    RuntimeStats stats() const {
        auto state = state_;
        if (!state) {
            return {};
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        return state->stats;
    }

    std::size_t capacity() const {
        auto state = state_;
        return state ? state->buffers.size() : 0;
    }

private:
    static Handle makeHandle(T* raw, std::shared_ptr<State> state) {
        return Handle(raw, [state = std::move(state)](T* item) mutable {
            releaseToState(state, item);
        });
    }

    static void releaseToState(const std::shared_ptr<State>& state, T* item) {
        if (!state || !item) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->free.push(item);
            ++state->stats.recycled;
        }
        state->cv.notify_one();
    }

    static void updateHighWatermarkLocked(State& state) {
        const std::size_t used = state.buffers.size() - state.free.size();
        if (used > state.stats.highWatermark) {
            state.stats.highWatermark = used;
        }
    }

    std::shared_ptr<State> state_;
};

} // namespace voice_runtime
