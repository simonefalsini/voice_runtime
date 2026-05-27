#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace voice_runtime {

class IActiveNode {
public:
    virtual ~IActiveNode() = default;
    virtual bool initialize() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual const char* name() const = 0;
};

class ActiveNodeBase : public virtual IActiveNode {
public:
    ~ActiveNodeBase() override { stop(); }

    void start() override {
        running_.store(true);
        thread_ = std::thread([this] { this->runLoop(); });
    }

    void stop() override {
        running_.store(false);
        wake();
        if (thread_.joinable()) thread_.join();
    }

protected:
    bool running() const { return running_.load(); }
    virtual void runLoop() = 0;
    virtual void wake() {}

private:
    std::atomic<bool> running_ = false;
    std::thread thread_;
};

} // namespace voice_runtime
