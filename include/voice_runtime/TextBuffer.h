#pragma once

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace voice_runtime {

class RollingTextBuffer {
public:
    explicit RollingTextBuffer(std::size_t maxBytes)
        : maxBytes_(maxBytes) {}

    void append(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_ += text;
        spillIfNeeded();
    }

    std::string snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.size();
    }

private:
    void spillIfNeeded() {
        if (data_.size() <= maxBytes_) return;
        const std::size_t excess = data_.size() - maxBytes_;
        data_.erase(0, excess);
    }

    std::size_t maxBytes_;
    std::string data_;
    mutable std::mutex mutex_;
};

class DiskBackedTextBuffer {
public:
    DiskBackedTextBuffer(std::string path, std::size_t memoryWindowBytes)
        : path_(std::move(path)), memory_(memoryWindowBytes) {
        std::ofstream clear(path_, std::ios::binary | std::ios::trunc);
    }

    void append(const std::string& text) {
        {
            std::ofstream f(path_, std::ios::binary | std::ios::app);
            f << text;
        }
        memory_.append(text);
    }

    std::string memorySnapshot() const {
        return memory_.snapshot();
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    RollingTextBuffer memory_;
};

} // namespace voice_runtime
