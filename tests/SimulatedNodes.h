#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
//   Generates synthetic audio frames at the configured frame rate.
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
//   - When TTS is active (ttsState_->isActive()): pass-through mode
//     → all frames forwarded, NO VadEvents emitted.
//   - When TTS is inactive: normal VAD behaviour
//     → energy-based gating with hangover, SpeechStart/SpeechEnd events.
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

    void setTtsStateSignal(const TtsStateSignal* signal) override { ttsState_ = signal; }

    bool isSpeaking() const { return speaking_.load(std::memory_order_acquire); }

protected:
    void runLoop() override {
        initStartNs();
        scheduleNext();

        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;

            // TTS active → pass-through (no gating, no events)
            if (ttsState_ && ttsState_->isActive()) {
                if (out_) out_->push(std::move(frame));
                continue;
            }

            // Normal VAD mode
            const uint64_t now = frame->timestampNs;

            // Check if it's time to toggle state
            if (now >= nextTransitionNs_) {
                toggleSpeech(now);
                scheduleNext();
            }

            if (speaking_.load(std::memory_order_acquire)) {
                if (out_) out_->push(std::move(frame));
            }
            // During silence the frame is simply released (pool recycle)
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
        // Silence: 500..2000ms   |   Speech: 1000..4000ms
        const int loMs = currentlySpeaking ? 1000 : 500;
        const int hiMs = currentlySpeaking ? 4000 : 2000;
        std::uniform_int_distribution<int> dist(loMs, hiMs);
        const uint64_t durationNs =
            static_cast<uint64_t>(dist(rng_)) * 1'000'000ULL;
        nextTransitionNs_ = nowNs() + durationNs;
    }

    AudioFrameQueue*       in_  = nullptr;
    AudioFrameQueue*       out_ = nullptr;
    VadEventQueue*         evq_ = nullptr;
    const TtsStateSignal*  ttsState_ = nullptr;
    float                  threshold_        = 0.5f;
    std::atomic<bool>      speaking_{false};
    uint64_t               nextTransitionNs_ = 0;
    uint64_t               startNs_          = 0;
    std::mt19937           rng_;
};

// ---------------------------------------------------------------------------
// SimulatedAecNode
//   - When TTS is inactive: pass-through (forward capture frames, zero-copy).
//   - When TTS is active: process (drain render queue with tryPop, simulate
//     echo cancellation, allocate from pool).
//   - Uses tryPop() for render queue — never blocking on reference frames.
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

    void setTtsStateSignal(const TtsStateSignal* signal) override { ttsState_ = signal; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            AudioFrameHandle mic;
            if (!micIn_ || !micIn_->pop(mic)) break;

            const bool ttsActive = ttsState_ && ttsState_->isActive();

            if (!ttsActive) {
                // Pass-through: no AEC needed, zero-copy forward
                mic->sequence = seq_++;
                if (out_) out_->push(std::move(mic));
                continue;
            }

            // TTS active → drain all available render/reference frames (non-blocking)
            drainRenderQueue();

            // Acquire a clean output frame from the pool
            auto clean = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
            if (!clean) { if (!running()) break; continue; }

            // Simulate echo cancellation: copy mic data into clean frame
            *clean           = *mic;
            clean->sequence  = seq_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(processMs_));

            if (out_) out_->push(std::move(clean));
        }
    }

private:
    void drainRenderQueue() {
        AudioFrameHandle ref;
        while (refIn_ && refIn_->tryPop(ref)) {
            // In a real implementation: feed ref into WebRTC APM reverse stream
            // Here: just consume (ref is released when handle goes out of scope)
        }
    }

    AudioFrameQueue*      micIn_ = nullptr;
    AudioFrameQueue*      refIn_ = nullptr;
    AudioFrameQueue*      out_   = nullptr;
    const TtsStateSignal* ttsState_ = nullptr;
    uint64_t              seq_   = 0;
    SharedBufferPool<AudioFrame> pool_;
    int                   processMs_;
};

// ---------------------------------------------------------------------------
// SimulatedSttNode
//   Accumulates N audio frames then emits a simulated text chunk.
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

            // Every 50 frames produce an utterance
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
// SimulatedClassifierBargeInNode
//   Replaces SimulatedBargeInNode with TTS-state-aware classification.
//
//   When TTS inactive (pass-through):
//     - Forwards all text from sttText to llmText directly.
//     - No classification, no barge-in.
//
//   When TTS active:
//     - Reads text from sttText queue.
//     - Classifies: keyword match → barge-in, otherwise → overlapBuffer.
//     - Does NOT forward to llmText.
//     - Keyboard 'b' → manual barge-in.
//
//   On TTS transition ON→OFF:
//     - Flushes overlapBuffer content to llmText as a single TextChunk
//       prefixed with "[utente durante risposta]: ".
// ---------------------------------------------------------------------------

class SimulatedClassifierBargeInNode final
    : public ActiveNodeBase
    , public IClassifierBargeInNode
{
public:
    SimulatedClassifierBargeInNode() {
        keywords_ = {"stop", "interrompi", "basta", "fermati"};
    }

    const char* name()   const override { return "SimClassifierBargeIn"; }
    bool initialize()          override { return true; }

    void setTextInputQueue(TextQueue* q)            override { textIn_  = q; }
    void setTextOutputQueue(TextQueue* q)           override { textOut_ = q; }
    void setBargeInEventQueue(BargeInQueue* q)      override { evq_     = q; }
    void setTtsInterruptSignal(InterruptSignal* s)  override { ttsInt_  = s; }
    void setLlmInterruptSignal(InterruptSignal* s)  override { llmInt_  = s; }
    void setTtsStateSignal(const TtsStateSignal* s) override { ttsState_= s; }
    void setOverlapBuffer(RollingTextBuffer* buf)   override { overlapBuffer_ = buf; }

    void triggerManualBargeIn() override {
        if (ttsState_ && ttsState_->isActive()) {
            doBargeIn(BargeInEvent::Source::Keyboard, "manual");
        }
    }

    void start() override {
        kbRunning_.store(true, std::memory_order_release);
        ActiveNodeBase::start();
        // Separate keyboard polling thread
        kbThread_ = std::thread([this] { keyboardLoop(); });
    }

    void stop() override {
        kbRunning_.store(false, std::memory_order_release);
        ActiveNodeBase::stop();
        if (kbThread_.joinable()) kbThread_.join();
    }

protected:
    void runLoop() override {
        bool wasTtsActive = false;

        while (running()) {
            TextChunk chunk;
            if (!textIn_ || !textIn_->pop(chunk)) break;

            const bool ttsActive = ttsState_ && ttsState_->isActive();

            // Detect TTS OFF transition → flush overlap buffer
            if (wasTtsActive && !ttsActive) {
                flushOverlapBuffer();
            }
            wasTtsActive = ttsActive;

            if (!ttsActive) {
                // Pass-through: forward to LLM
                if (textOut_) textOut_->push(std::move(chunk));
                continue;
            }

            // TTS active: classify
            if (isBargeInKeyword(chunk.text)) {
                doBargeIn(BargeInEvent::Source::ContextDetected, chunk.text);
            } else if (overlapBuffer_) {
                overlapBuffer_->append(chunk.text + "\n");
            }
        }

        // Final flush if TTS was still active when we exit
        if (wasTtsActive) {
            flushOverlapBuffer();
        }
    }

private:
    void keyboardLoop() {
#if !defined(_WIN32)
        // Put stdin in non-blocking mode
        const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif
        while (kbRunning_.load(std::memory_order_acquire) && running()) {
#if defined(_WIN32)
            if (_kbhit()) {
                const int c = _getch();
                if (c == 'b' || c == 'B') {
                    triggerManualBargeIn();
                }
            }
#else
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'b' || c == 'B') {
                    triggerManualBargeIn();
                }
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    bool isBargeInKeyword(const std::string& text) const {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const auto& kw : keywords_)
            if (lower.find(kw) != std::string::npos) return true;
        return false;
    }

    void doBargeIn(BargeInEvent::Source src, const std::string& reason) {
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

    void flushOverlapBuffer() {
        if (!overlapBuffer_) return;
        std::string content = overlapBuffer_->snapshot();
        if (content.empty()) return;

        overlapBuffer_->clear();

        if (textOut_) {
            TextChunk flush;
            flush.text        = "[utente durante risposta]: " + content;
            flush.isFinal     = true;
            flush.sequence    = ++flushSeq_;
            flush.timestampNs = nowNs();
            textOut_->push(std::move(flush));
        }
    }

    TextQueue*            textIn_  = nullptr;
    TextQueue*            textOut_ = nullptr;
    BargeInQueue*         evq_     = nullptr;
    InterruptSignal*      ttsInt_  = nullptr;
    InterruptSignal*      llmInt_  = nullptr;
    const TtsStateSignal* ttsState_      = nullptr;
    RollingTextBuffer*    overlapBuffer_ = nullptr;

    std::set<std::string> keywords_;
    std::thread           kbThread_;
    std::atomic<bool>     kbRunning_{false};
    uint64_t              flushSeq_ = 0;
};

// ---------------------------------------------------------------------------
// SimulatedLlmNode
//   Simulates LLM response generation with interrupt support.
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

            // Simulate LLM latency with interrupt check every 50ms
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
// SimulatedInterpreterNode
//   Simple pass-through for simulation: forwards all text from input to
//   output.  In production this would parse JSON, extract spoken text,
//   and filter commands.
// ---------------------------------------------------------------------------

class SimulatedInterpreterNode final
    : public ActiveNodeBase
    , public IInterpreterNode
{
public:
    const char* name()   const override { return "SimInterpreter"; }
    bool initialize()          override { return true; }

    void setInputQueue(TextQueue* q)  override { in_  = q; }
    void setOutputQueue(TextQueue* q) override { out_ = q; }

protected:
    void runLoop() override {
        while (running()) {
            TextChunk chunk;
            if (!in_ || !in_->pop(chunk)) break;
            if (out_) out_->push(std::move(chunk));
        }
    }

private:
    TextQueue* in_  = nullptr;
    TextQueue* out_ = nullptr;
};

// ---------------------------------------------------------------------------
// SimulatedTtsNode
//   - Manages TtsStateSignal: sets active BEFORE pushing any frame for a
//     text chunk, sets inactive AFTER the last frame (or on interrupt).
//   - Pushes to both speakerOut and aecRefOut.
//   - Fix #11: explicit copy for ref, move for speaker.
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
    void setTtsStateSignal(TtsStateSignal* signal)         override { ttsState_ = signal; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            TextChunk text;
            if (!in_ || !in_->pop(text)) break;

            // Mark TTS active BEFORE pushing any frame
            if (ttsState_) ttsState_->setActive(true);

            bool interrupted = false;
            for (int i = 0; i < framesPerText_ && running(); ++i) {
                // Check interrupt
                if (intSig_ && intSig_->check()) {
                    intSig_->clear();
                    std::printf("[TTS] Interrupted after %d frames\n", i);
                    std::fflush(stdout);
                    interrupted = true;
                    break;
                }

                auto frame = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
                if (!frame) {
                    if (!running()) { interrupted = true; break; }
                    continue;
                }

                frame->format      = format_;
                frame->resizeForFormat();
                frame->sequence    = seq_++;
                frame->timestampNs = nowNs();
                for (auto& s : frame->pcm16) s = 500;

                // Push to both queues — reference first (AEC timing)
                // Fix #11: explicit copy for ref, move for speaker
                if (refOut_) {
                    auto refCopy = std::make_shared<AudioFrame>(*frame);
                    refOut_->push(std::move(refCopy));
                }
                if (spkOut_) spkOut_->push(std::move(frame));

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(format_.frameMs));
            }

            // Mark TTS inactive AFTER the last frame (or on interrupt)
            if (ttsState_) ttsState_->setActive(false);

            (void)interrupted;  // consumed above; explicit for clarity
        }
    }

private:
    AudioFormat      format_;
    TextQueue*       in_       = nullptr;
    AudioFrameQueue* spkOut_   = nullptr;
    AudioFrameQueue* refOut_   = nullptr;
    InterruptSignal* intSig_   = nullptr;
    TtsStateSignal*  ttsState_ = nullptr;
    uint64_t         seq_      = 0;
    SharedBufferPool<AudioFrame> pool_;
    int              framesPerText_;
};

// ---------------------------------------------------------------------------
// SimulatedAudioOutputNode
//   Consumes frames from speaker queue, simulates playback delay.
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
