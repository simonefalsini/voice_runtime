#pragma once

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace voice_runtime {

// ---------------------------------------------------------------------------
// RollingTextBuffer — finestra in memoria con sovrascrittura FIFO
// ---------------------------------------------------------------------------

class RollingTextBuffer {
public:
    explicit RollingTextBuffer(std::size_t maxBytes) : maxBytes_(maxBytes) {}

    void append(const std::string& text) {
        std::lock_guard<std::mutex> lk(mutex_);
        data_ += text;
        if (data_.size() > maxBytes_) {
            const std::size_t excess = data_.size() - maxBytes_;
            data_.erase(0, excess);
        }
    }

    std::string snapshot() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return data_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return data_.size();
    }

private:
    std::size_t          maxBytes_;
    std::string          data_;
    mutable std::mutex   mutex_;
};

// ---------------------------------------------------------------------------
// DiskBackedTextBuffer
//   - file handle aperto nel costruttore e chiuso nel distruttore
//   - nessuna apertura/chiusura per ogni append
//   - flush esplicito tramite flush() o alla distruzione
// ---------------------------------------------------------------------------

class DiskBackedTextBuffer {
public:
    DiskBackedTextBuffer(std::string path, std::size_t memoryWindowBytes)
        : path_(std::move(path))
        , memory_(memoryWindowBytes)
        , file_(path_, std::ios::binary | std::ios::trunc)
    {
        if (!file_.is_open()) {
            // Non lanciamo eccezioni nel costruttore — il chiamante può
            // verificare isOpen() prima di usare il buffer.
        }
    }

    ~DiskBackedTextBuffer() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    // Non copiabile, non spostabile (fstream non è spostabile su tutti i compilatori)
    DiskBackedTextBuffer(const DiskBackedTextBuffer&)            = delete;
    DiskBackedTextBuffer& operator=(const DiskBackedTextBuffer&) = delete;

    bool isOpen() const { return file_.is_open(); }

    void append(const std::string& text) {
        {
            std::lock_guard<std::mutex> lk(fileMutex_);
            if (file_.is_open()) {
                file_ << text;
                // Flush periodico ogni writesBeforeFlush_ scritture
                if (++writeCount_ >= writesBeforeFlush_) {
                    file_.flush();
                    writeCount_ = 0;
                }
            }
        }
        memory_.append(text);
    }

    void flush() {
        std::lock_guard<std::mutex> lk(fileMutex_);
        if (file_.is_open()) file_.flush();
    }

    std::string memorySnapshot() const { return memory_.snapshot(); }
    const std::string& path() const    { return path_; }

    // Quante scritture prima di un flush esplicito (default 16)
    void setWritesBeforeFlush(uint32_t n) { writesBeforeFlush_ = n ? n : 1; }

private:
    std::string          path_;
    RollingTextBuffer    memory_;
    std::ofstream        file_;
    mutable std::mutex   fileMutex_;
    uint32_t             writeCount_        = 0;
    uint32_t             writesBeforeFlush_ = 16;
};

} // namespace voice_runtime
