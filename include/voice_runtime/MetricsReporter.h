#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// IMetricSource — qualsiasi oggetto che sa produrre un QueueSnapshot
// ---------------------------------------------------------------------------

struct IMetricSource {
    virtual ~IMetricSource() = default;
    virtual QueueSnapshot snapshot() const = 0;
};

// Adapter per BoundedQueue<T>
template <typename T>
struct QueueMetricSource final : IMetricSource {
    const BoundedQueue<T>& q;
    explicit QueueMetricSource(const BoundedQueue<T>& queue) : q(queue) {}
    QueueSnapshot snapshot() const override { return q.snapshot(); }
};

// ---------------------------------------------------------------------------
// MetricsReporter
// ---------------------------------------------------------------------------

class MetricsReporter {
public:
    using ExtraLineCallback = std::function<std::string()>;

    explicit MetricsReporter(std::chrono::milliseconds interval)
        : interval_(interval) {}

    ~MetricsReporter() { stop(); }

    template <typename T>
    void addQueue(const BoundedQueue<T>& q) {
        sources_.push_back(std::make_unique<QueueMetricSource<T>>(q));
    }

    // Callback opzionale per righe extra (stato VAD, pool, LLM, ecc.)
    void setExtraLineCallback(ExtraLineCallback cb) {
        extraLine_ = std::move(cb);
    }

    void start() {
        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_.store(false);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        auto nextWake = std::chrono::steady_clock::now() + interval_;
        elapsed_ = std::chrono::steady_clock::now();

        while (running_.load()) {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait_until(lk, nextWake);
            lk.unlock();

            if (!running_.load()) break;

            printTable();
            nextWake += interval_;
        }
    }

    void printTable() {
        const auto now = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(now - elapsed_).count();

        // Header
        std::printf("\n[t=%6.1fs] %-22s %6s %6s %8s %8s %8s %6s\n",
                    t, "Queue", "size", "cap", "prod", "cons", "drop", "hwm");
        std::printf("           %s\n",
            "-----------------------------------------------------------------");

        for (const auto& src : sources_) {
            const QueueSnapshot s = src->snapshot();
            std::printf("           %-22s %6zu %6zu %8llu %8llu %8llu %6zu\n",
                        s.name,
                        s.currentSize,
                        s.capacity,
                        static_cast<unsigned long long>(s.produced),
                        static_cast<unsigned long long>(s.consumed),
                        static_cast<unsigned long long>(s.dropped),
                        s.highWatermark);
        }

        if (extraLine_) {
            const std::string extra = extraLine_();
            if (!extra.empty())
                std::printf("           %s\n", extra.c_str());
        }

        std::fflush(stdout);
    }

    std::chrono::milliseconds    interval_;
    std::vector<std::unique_ptr<IMetricSource>> sources_;
    ExtraLineCallback            extraLine_;
    std::atomic<bool>            running_{false};
    std::thread                  thread_;
    std::mutex                   mutex_;
    std::condition_variable      cv_;
    std::chrono::steady_clock::time_point elapsed_;
};

} // namespace voice_runtime
