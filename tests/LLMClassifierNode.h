#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "voice_runtime/ActiveNode.h"
#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/TextBuffer.h"

namespace voice_runtime {

class LLMClassifierNode final : public virtual ActiveNodeBase,
                                public virtual IClassifierBargeInNode {
public:
  // ── Keywords configured by default (Jarvis case-insensitive) ───────────────
  std::string wakeKeyword = "hey jarvis ascoltami";
  std::string doneKeyword = "hey jarvis ho finito";
  std::vector<std::string> stopKeywords = {"jarvis stop", "stop", "fermati"};

  LLMClassifierNode() = default;
  ~LLMClassifierNode() override { stop(); }

  const char *name() const override { return "LLMClassifier"; }
  bool initialize() override { return true; }

  // ── IClassifierBargeInNode Interface ───────────────────────────────────────
  void setTextInputQueue(TextQueue *sttText) override { sttIn_ = sttText; }
  void setTextOutputQueue(TextQueue *llmText) override { llmOut_ = llmText; }
  void setBargeInEventQueue(BargeInQueue *events) override {
    bargeInEvents_ = events;
  }
  void setTtsInterruptSignal(InterruptSignal *signal) override {
    ttsInterrupt_ = signal;
  }
  void setLlmInterruptSignal(InterruptSignal *signal) override {
    llmInterrupt_ = signal;
  }
  void setTtsStateSignal(const TtsStateSignal *signal) override {
    ttsState_ = signal;
  }
  void setOverlapBuffer(RollingTextBuffer *buf) override {
    overlapBuffer_ = buf;
  }

  void triggerManualBargeIn() override {
    if (ttsState_ && ttsState_->isActive()) {
      std::printf(
          "  [LLMClassifier] Manual Barge-In triggered via UI/Keyboard\n");
      std::fflush(stdout);
      doBargeIn(BargeInEvent::Source::Keyboard, "manual");
    }
  }

  // ── Diagnostic Queries (Useful for tests) ──────────────────────────────────
  bool isListening() const { return listening_.load(); }
  int interruptCount() const { return interruptCount_.load(); }
  int injectedCount() const { return injectedCount_.load(); }
  std::string cachedComment() const {
    if (overlapBuffer_) {
      return overlapBuffer_->snapshot();
    }
    return "";
  }

protected:
  void runLoop() override {
    bool wasTtsActive = false;

    while (running()) {
      TextChunk chunk;
      bool popped = false;
      if (sttIn_) {
        popped = sttIn_->popWithTimeout(chunk, std::chrono::milliseconds(50));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      const bool ttsActive = ttsState_ && ttsState_->isActive();

      // Detect TTS active -> inactive transition (ON -> OFF)
      if (wasTtsActive && !ttsActive) {
        flushOverlapBuffer();
      }
      wasTtsActive = ttsActive;

      if (!popped) {
        if (!running())
          break;
        continue;
      }

      if (chunk.text.empty())
        continue;

      const std::string &text = chunk.text;
      std::string lowerText = toLower(text);

      // 1. TTS is active (or caching is active)
      if (ttsActive || listening_) {
        // Priority A: stop keywords -> barge-in + inject cached comment
        if (containsAny(lowerText, stopKeywords)) {
          std::printf("  [LLMClassifier] Stop keyword detected: \"%s\" — "
                      "interrupting and injecting cache\n",
                      text.c_str());
          std::fflush(stdout);
          doBargeIn(BargeInEvent::Source::ContextDetected, text);
          injectCachedComment();
          continue;
        }

        // Priority B: done keyword -> close cache window
        if (contains(lowerText, doneKeyword)) {
          std::printf("  [LLMClassifier] Done keyword: closing cache window\n");
          std::fflush(stdout);
          listening_.store(false);
          continue;
        }

        // Priority C: wake keyword -> open cache window
        if (contains(lowerText, wakeKeyword)) {
          std::printf("  [LLMClassifier] Wake keyword: entering listening mode "
                      "(cache cleared)\n");
          std::fflush(stdout);
          listening_.store(true);
          if (overlapBuffer_) {
            overlapBuffer_->clear();
          }
          continue;
        }

        // Normal caching of user speech when in listening mode
        if (listening_) {
          std::printf("  [LLMClassifier] Caching user comment: \"%s\"\n",
                      text.c_str());
          std::fflush(stdout);
          if (overlapBuffer_) {
            std::string current = overlapBuffer_->snapshot();
            if (!current.empty()) {
              overlapBuffer_->append(" ");
            }
            overlapBuffer_->append(text);
          }
          continue;
        }

        // If TTS is active but no keyword and not listening, discard the text
        std::printf(
            "  [LLMClassifier] Discarded (TTS active, no keyword): \"%s\"\n",
            text.c_str());
        std::fflush(stdout);
      }
      // 2. TTS is inactive and we're not in listening mode -> transparent
      // pass-through
      else {
        std::printf("  [LLMClassifier → LLM] \"%s\"\n", text.c_str());
        std::fflush(stdout);
        if (llmOut_) {
          llmOut_->push(std::move(chunk));
        }
      }
    }

    // Final cleanup on node stop
    if (wasTtsActive) {
      flushOverlapBuffer();
    }
  }

private:
  static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
  }

  static bool contains(const std::string &text, const std::string &kw) {
    if (kw.empty())
      return false;
    return text.find(kw) != std::string::npos;
  }

  static bool containsAny(const std::string &text,
                          const std::vector<std::string> &kws) {
    for (const auto &kw : kws) {
      if (contains(text, kw))
        return true;
    }
    return false;
  }

  void doBargeIn(BargeInEvent::Source src, const std::string &reason) {
    if (ttsInterrupt_) {
      ttsInterrupt_->request();
    }
    if (llmInterrupt_) {
      llmInterrupt_->request();
    }
    ++interruptCount_;

    if (bargeInEvents_) {
      BargeInEvent ev;
      ev.source = src;
      ev.timestampNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      ev.reason = reason;
      bargeInEvents_->push(std::move(ev));
    }
  }

  void injectCachedComment() {
    std::string comment;
    if (overlapBuffer_) {
      comment = overlapBuffer_->snapshot();
      overlapBuffer_->clear();
    }
    listening_.store(false);

    if (comment.empty()) {
      std::printf("  [LLMClassifier] No cached comment to inject\n");
      std::fflush(stdout);
      return;
    }

    // Wait briefly for LLM/TTS to settle after interrupt (max 500 ms)
    for (int i = 0; i < 10 && running(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf(
        "  [LLMClassifier] Injecting cached comment into LLM queue: \"%s\"\n",
        comment.c_str());
    std::fflush(stdout);

    if (llmOut_) {
      TextChunk inject;
      inject.text = comment;
      inject.isFinal = true;
      inject.sequence = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      inject.timestampNs = inject.sequence;
      llmOut_->push(std::move(inject));
    }
    ++injectedCount_;
  }

  void flushOverlapBuffer() {
    if (!overlapBuffer_)
      return;
    std::string content = overlapBuffer_->snapshot();
    if (content.empty())
      return;
    overlapBuffer_->clear();

    std::printf(
        "  [LLMClassifier] TTS finished. Flushing cached comment: \"%s\"\n",
        content.c_str());
    std::fflush(stdout);

    if (llmOut_) {
      TextChunk inject;
      inject.text = content;
      inject.isFinal = true;
      inject.sequence = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      inject.timestampNs = inject.sequence;
      llmOut_->push(std::move(inject));
    }
    ++injectedCount_;
  }

  TextQueue *sttIn_ = nullptr;
  TextQueue *llmOut_ = nullptr;
  BargeInQueue *bargeInEvents_ = nullptr;
  InterruptSignal *ttsInterrupt_ = nullptr;
  InterruptSignal *llmInterrupt_ = nullptr;
  const TtsStateSignal *ttsState_ = nullptr;
  RollingTextBuffer *overlapBuffer_ = nullptr;

  std::atomic<bool> listening_{false};
  std::atomic<int> interruptCount_{0};
  std::atomic<int> injectedCount_{0};
};

} // namespace voice_runtime
