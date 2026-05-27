#pragma once

#include <atomic>
#include <string>
#include <thread>

// Platform-specific thread naming
#if defined(__APPLE__)
#  include <pthread.h>
#elif defined(__linux__) || defined(__ANDROID__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <processthreadsapi.h>
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Thread utilities — platform abstraction
// ---------------------------------------------------------------------------

namespace detail {

inline void setCurrentThreadName(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);                  // macOS: solo sul thread corrente
#elif defined(__linux__) || defined(__ANDROID__)
    pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
    // SetThreadDescription richiede Windows 10 1607+
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (len > 0) {
        std::wstring wname(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), len);
        SetThreadDescription(GetCurrentThread(), wname.c_str());
    }
#endif
}

// realtimePriority: -1 = default, 0..99 = SCHED_FIFO level (Linux only)
// Su macOS/Android/iOS viene ignorato (richiederebbe entitlement/root).
// Su Windows mappa su THREAD_PRIORITY_TIME_CRITICAL se > 0.
inline void applyThreadPriority(int realtimePriority) {
    if (realtimePriority < 0) return;

#if defined(__linux__) && !defined(__ANDROID__)
    sched_param sp{};
    sp.sched_priority = realtimePriority;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#elif defined(_WIN32)
    if (realtimePriority > 0)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    (void)realtimePriority; // not supported without special entitlements
#endif
}

} // namespace detail

// ---------------------------------------------------------------------------
// IActiveNode
// ---------------------------------------------------------------------------

class IActiveNode {
public:
    virtual ~IActiveNode() = default;
    virtual bool        initialize()   = 0;
    virtual void        start()        = 0;
    virtual void        stop()         = 0;
    virtual const char* name() const   = 0;
};

// ---------------------------------------------------------------------------
// ActiveNodeBase
// ---------------------------------------------------------------------------

class ActiveNodeBase : public virtual IActiveNode {
public:
    ~ActiveNodeBase() override { stop(); }

    // realtimePriority: -1 = OS default, >0 = real-time (Linux SCHED_FIFO)
    void start() override { startWithPriority(-1); }

    void startWithPriority(int realtimePriority) {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this, realtimePriority] {
            detail::setCurrentThreadName(name());
            detail::applyThreadPriority(realtimePriority);
            this->runLoop();
        });
    }

    void stop() override {
        running_.store(false, std::memory_order_release);
        wake();
        if (thread_.joinable()) thread_.join();
    }

protected:
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    virtual void runLoop() = 0;
    virtual void wake() {}

private:
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace voice_runtime
