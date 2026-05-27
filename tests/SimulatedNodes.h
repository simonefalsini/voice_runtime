#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

// Per il barge-in da tastiera (non-blocking stdin)
#if defined(_WIN32)
#  include <conio.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <termios.h>
#endif

namespace voice_runtime::test {

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

inline uint64_t nowNs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// SimulatedMicrophoneNode
// ---------------------------------------------------------------------------

class SimulatedMicrophoneNode final
    : public ActiveNodeBase
    , public IMicrophoneNode
{
public:
    SimulatedMicrophoneNode(AudioFormat format, int framesPerSecond, std::size_t poolSize)
        : format_(format)
        , periodMs_(1000 / framesPerSecond)
        , pool_(poolSize)
    {}

    const char* name()   const override { return "SimMicrophone"; }
    bool initialize()          override { return true; }
    void setOutputQueue(AudioFrameQueue* q) override { out_ = q; }

protected:
    void wake()    override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            auto frame = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
            if (!frame) { if (!running()) break; continue; }

            frame->format      = format_;
            frame->resizeForFormat();
            frame->sequence    = seq_++;
            frame->timestampNs = nowNs();
            for (auto& s : frame->pcm16)
                s = static_cast<int16_t>(frame->sequence % 100);

            if (out_) out_->push(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(periodMs_));
        }
    }

private:
    AudioFormat      format_;
    int              periodMs_;
    uint64_t         seq_ = 0;
    AudioFrameQueue* out_ = nullptr;
    SharedBufferPool<AudioFrame> pool_;
};

// ---------------------------------------------------------------------------
// SimulatedVadNode
//   Macchina a stati: Silence ↔ Speaking
//   Durante Silence i frame vengono scartati (non pushati a valle).
//   Emette VadEvent::SpeechStart / SpeechEnd con timestamp.
// ---------------------------------------------------------------------------

class SimulatedVadNode final
    : public ActiveNodeBase
    , public IVadNode
{
public:
    explicit SimulatedVadNode(uint32_t seed = 42) : rng_(seed) {}

    const char* name()   const override { return "SimVAD"; }
    bool initialize()          override { return true; }

    void setInputQueue(AudioFrameQueue* in)        override { in_  = in; }
    void setOutputQueue(AudioFrameQueue* out)      override { out_ = out; }
    void setEventQueue(VadEventQueue* ev)          override { evq_ = ev; }
    void setSpeechThreshold(float thr)             override { threshold_ = thr; }

    bool isSpeaking() const { return speaking_.load(std::memory_order_acquire); }

protected:
    void runLoop() override {
        initStartNs();
        scheduleNext();

        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;

            const uint64_t now = frame->timestampNs;

            // Controlla se è il momento di cambiare stato
            if (now >= nextTransitionNs_) {
                toggleSpeech(now);
                scheduleNext();
            }

            if (speaking_.load(std::memory_order_acquire)) {
                if (out_) out_->push(std::move(frame));
            }
            // Durante il silenzio il frame viene semplicemente rilasciato (pool recycle)
        }
    }

private:
    void toggleSpeech(uint64_t nowNs) {
        const bool wasSpeaking = speaking_.load();
        speaking_.store(!wasSpeaking, std::memory_order_release);

        VadEvent ev;
        ev.type        = wasSpeaking ? VadEvent::Type::SpeechEnd
                                     : VadEvent::Type::SpeechStart;
        ev.timestampNs = nowNs;
        ev.confidence  = threshold_;

        const char* label = wasSpeaking ? "SpeechEnd" : "SpeechStart";
        const double relSec = static_cast<double>(nowNs - startNs_) / 1e9;
        std::printf("[VAD] %s at +%.3fs\n", label, relSec);
        std::fflush(stdout);

        if (evq_) evq_->push(ev);
    }

    void initStartNs() {
        if (startNs_ == 0) startNs_ = nowNs();
    }

    void scheduleNext() {
        const bool currentlySpeaking = speaking_.load();
        // Silenzio: 500..2000ms   |   Speech: 1000..4000ms
        const int loMs = currentlySpeaking ? 1000 : 500;
        const int hiMs = currentlySpeaking ? 4000 : 2000;
        std::uniform_int_distribution<int> dist(loMs, hiMs);
        const uint64_t durationNs =
            static_cast<uint64_t>(dist(rng_)) * 1'000'000ULL;
        nextTransitionNs_ = nowNs() + durationNs;
    }

    AudioFrameQueue* in_  = nullptr;
    AudioFrameQueue* out_ = nullptr;
    VadEventQueue*   evq_ = nullptr;
    float            threshold_        = 0.5f;
    std::atomic<bool> speaking_{false};
    uint64_t         nextTransitionNs_ = 0;
    uint64_t         startNs_          = 0;
    std::mt19937     rng_;
};

// ---------------------------------------------------------------------------
// SimulatedAecNode
// ---------------------------------------------------------------------------

class SimulatedAecNode final
    : public ActiveNodeBase
    , public IAecDspNode
{
public:
    SimulatedAecNode(std::size_t poolSize, int processMs)
        : pool_(poolSize), processMs_(processMs) {}

    const char* name()   const override { return "SimAEC"; }
    bool initialize()          override { return true; }

    void setCaptureInputQueue(AudioFrameQueue* q) override { micIn_ = q; }
    void setRenderInputQueue(AudioFrameQueue* q)  override { refIn_ = q; }
    void setOutputQueue(AudioFrameQueue* q)       override { out_   = q; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            AudioFrameHandle mic;
            if (!micIn_ || !micIn_->pop(mic)) break;

            // Consuma un frame di reference se disponibile (senza bloccare)
            if (refIn_ && refIn_->size() > 0) {
                AudioFrameHandle ref;
                refIn_->pop(ref);
                // In una implementazione reale: AEC3 usa ref per cancellare mic
            }

            auto clean = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
            if (!clean) { if (!running()) break; continue; }

            *clean           = *mic;
            clean->sequence  = seq_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));

            if (out_) out_->push(std::move(clean));
        }
    }

private:
    AudioFrameQueue* micIn_ = nullptr;
    AudioFrameQueue* refIn_ = nullptr;
    AudioFrameQueue* out_   = nullptr;
    uint64_t         seq_   = 0;
    SharedBufferPool<AudioFrame> pool_;
    int              processMs_;
};

// ---------------------------------------------------------------------------
// SimulatedSttNode
// ---------------------------------------------------------------------------

class SimulatedSttNode final
    : public ActiveNodeBase
    , public ISpeechToTextNode
{
public:
    explicit SimulatedSttNode(int processMs) : processMs_(processMs) {}

    const char* name()   const override { return "SimSTT"; }
    bool initialize()          override { return true; }
    void setInputQueue(AudioFrameQueue* q)  override { in_  = q; }
    void setOutputQueue(TextQueue* q)       override { out_ = q; }

protected:
    void runLoop() override {
        while (running()) {
            AudioFrameHandle audio;
            if (!in_ || !in_->pop(audio)) break;
            ++frames_;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));

            // Ogni 50 frame produce un utterance
            if (frames_ % 50 == 0 && out_) {
                TextChunk c;
                c.text        = "Utterance " + std::to_string(frames_ / 50);
                c.isFinal     = true;
                c.sequence    = frames_ / 50;
                c.timestampNs = nowNs();
                out_->push(c);
            }
        }
    }

private:
    AudioFrameQueue* in_      = nullptr;
    TextQueue*       out_     = nullptr;
    uint64_t         frames_  = 0;
    int              processMs_;
};

// ---------------------------------------------------------------------------
// SimulatedBargeInNode
//   - Thread A: processing testo STT → forward a LLM, rileva pattern
//   - Thread B: polling stdin non-bloccante (tasto 'b')
// ---------------------------------------------------------------------------

class SimulatedBargeInNode final
    : public ActiveNodeBase
    , public IBargeInContextNode
{
public:
    SimulatedBargeInNode() {
        // Parole chiave per barge-in da contesto
        keywords_ = {"stop", "interrompi", "aspetta",
                     "fermati", "basta", "silenzio"};
    }

    const char* name()   const override { return "SimBargeIn"; }
    bool initialize()          override { return true; }

    void setTextInputQueue(TextQueue* q)          override { in_   = q; }
    void setTextOutputQueue(TextQueue* q)         override { out_  = q; }
    void setBargeInEventQueue(BargeInQueue* q)    override { evq_  = q; }
    void setTtsInterruptSignal(InterruptSignal* s)override { ttsInt_ = s; }
    void setLlmInterruptSignal(InterruptSignal* s)override { llmInt_ = s; }

    void triggerManualBargeIn() override {
        fireBargeIn(BargeInEvent::Source::Keyboard, "manual");
    }

    void start() override {
        ActiveNodeBase::start();
        // Thread tastiera separato
        kbThread_ = std::thread([this] { kbLoop(); });
    }

    void stop() override {
        ActiveNodeBase::stop();
        kbRunning_.store(false);
        if (kbThread_.joinable()) kbThread_.join();
    }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk chunk;
            if (!in_ || !in_->pop(chunk)) break;

            // Analisi parole chiave
            if (containsKeyword(chunk.text)) {
                fireBargeIn(BargeInEvent::Source::ContextDetected, chunk.text);
            }

            if (out_) out_->push(chunk);
        }
    }

private:
    void kbLoop() {
        kbRunning_.store(true);
#if !defined(_WIN32)
        // Metti stdin in modalità non-bloccante
        const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif
        while (kbRunning_.load() && running()) {
#if defined(_WIN32)
            if (_kbhit()) {
                const int c = _getch();
                if (c == 'b' || c == 'B') triggerManualBargeIn();
            }
#else
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'b' || c == 'B') triggerManualBargeIn();
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    bool containsKeyword(const std::string& text) const {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const auto& kw : keywords_)
            if (lower.find(kw) != std::string::npos) return true;
        return false;
    }

    void fireBargeIn(BargeInEvent::Source src, const std::string& reason) {
        BargeInEvent ev;
        ev.source      = src;
        ev.timestampNs = nowNs();
        ev.reason      = reason;

        const char* srcLabel = (src == BargeInEvent::Source::Keyboard)
                               ? "Keyboard" : "Context";
        std::printf("[BargeIn] source=%s reason=\"%s\"\n",
                    srcLabel, reason.c_str());
        std::fflush(stdout);

        if (ttsInt_) ttsInt_->request();
        if (llmInt_) llmInt_->request();
        if (evq_)    evq_->push(ev);
    }

    TextQueue*       in_     = nullptr;
    TextQueue*       out_    = nullptr;
    BargeInQueue*    evq_    = nullptr;
    InterruptSignal* ttsInt_ = nullptr;
    InterruptSignal* llmInt_ = nullptr;

    std::set<std::string> keywords_;
    std::thread           kbThread_;
    std::atomic<bool>     kbRunning_{false};
};

// ---------------------------------------------------------------------------
// SimulatedLlmNode
// ---------------------------------------------------------------------------

class SimulatedLlmNode final
    : public ActiveNodeBase
    , public ILanguageModelNode
{
public:
    explicit SimulatedLlmNode(int processMsPerChunk) : processMs_(processMsPerChunk) {}

    const char* name()   const override { return "SimLLM"; }
    bool initialize()          override { return true; }

    void setInputQueue(TextQueue* q)                     override { in_    = q; }
    void setOutputQueue(TextQueue* q)                    override { out_   = q; }
    void setPersistentInputBuffer(DiskBackedTextBuffer* b) override { disk_ = b; }
    void setInterruptSignal(InterruptSignal* s)          override { intSig_= s; }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk input;
            if (!in_ || !in_->pop(input)) break;

            if (disk_) disk_->append(input.text + "\n");

            // Simula latenza LLM con check interrupt ogni 50ms
            const int steps = processMs_ / 50;
            for (int i = 0; i < steps && running(); ++i) {
                if (intSig_ && intSig_->check()) {
                    intSig_->clear();
                    std::printf("[LLM] Interrupted\n");
                    std::fflush(stdout);
                    goto next_input;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (out_) {
                TextChunk resp;
                resp.text        = "Response to: " + input.text;
                resp.isFinal     = true;
                resp.sequence    = ++seq_;
                resp.timestampNs = nowNs();
                out_->push(resp);
            }

            next_input:;
        }
    }

private:
    TextQueue*            in_     = nullptr;
    TextQueue*            out_    = nullptr;
    DiskBackedTextBuffer* disk_   = nullptr;
    InterruptSignal*      intSig_ = nullptr;
    uint64_t              seq_    = 0;
    int                   processMs_;
};

// ---------------------------------------------------------------------------
// SimulatedTtsNode
// ---------------------------------------------------------------------------

class SimulatedTtsNode final
    : public ActiveNodeBase
    , public ITextToSpeechNode
{
public:
    SimulatedTtsNode(AudioFormat format, std::size_t poolSize, int framesPerText)
        : format_(format), pool_(poolSize), framesPerText_(framesPerText) {}

    const char* name()   const override { return "SimTTS"; }
    bool initialize()          override { return true; }

    void setInputQueue(TextQueue* q)                       override { in_       = q; }
    void setSpeakerOutputQueue(AudioFrameQueue* q)         override { spkOut_   = q; }
    void setAecReferenceOutputQueue(AudioFrameQueue* q)    override { refOut_   = q; }
    void setInterruptSignal(InterruptSignal* s)            override { intSig_   = s; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            TextChunk text;
            if (!in_ || !in_->pop(text)) break;

            for (int i = 0; i < framesPerText_ && running(); ++i) {
                // Check interrupt
                if (intSig_ && intSig_->check()) {
                    intSig_->clear();
                    std::printf("[TTS] Interrupted after %d frames\n", i);
                    std::fflush(stdout);
                    break;
                }

                auto frame = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
                if (!frame) continue;

                frame->format      = format_;
                frame->resizeForFormat();
                frame->sequence    = seq_++;
                frame->timestampNs = nowNs();
                for (auto& s : frame->pcm16) s = 500;

                // Push a entrambe le code — reference prima (AEC timing)
                if (refOut_) refOut_->push(frame);
                if (spkOut_) spkOut_->push(std::move(frame));

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(format_.frameMs));
            }
        }
    }

private:
    AudioFormat      format_;
    TextQueue*       in_      = nullptr;
    AudioFrameQueue* spkOut_  = nullptr;
    AudioFrameQueue* refOut_  = nullptr;
    InterruptSignal* intSig_  = nullptr;
    uint64_t         seq_     = 0;
    SharedBufferPool<AudioFrame> pool_;
    int              framesPerText_;
};

// ---------------------------------------------------------------------------
// SimulatedAudioOutputNode
// ---------------------------------------------------------------------------

class SimulatedAudioOutputNode final
    : public ActiveNodeBase
    , public IAudioOutputNode
{
public:
    explicit SimulatedAudioOutputNode(int processMs) : processMs_(processMs) {}

    const char* name()   const override { return "SimAudioOut"; }
    bool initialize()          override { return true; }
    void setInputQueue(AudioFrameQueue* q) override { in_ = q; }

protected:
    void runLoop() override {
        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;
            ++played_;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));
        }
    }

private:
    AudioFrameQueue* in_     = nullptr;
    uint64_t         played_ = 0;
    int              processMs_;
};

} // namespace voice_runtime::test
