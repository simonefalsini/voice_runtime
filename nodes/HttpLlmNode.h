#pragma once

// ---------------------------------------------------------------------------
// HttpLlmNode — OpenAI-compatible chat-completion client for voice_runtime
//
// Implements ILanguageModelNode.  Connects to any OpenAI-compatible API via
// HTTP using cpp-httplib for HTTP and nlohmann/json for JSON.
//
// Two compilation modes:
//   1. VOICE_RUNTIME_HTTP_LLM_AVAILABLE  – real HTTP + SSE streaming.
//   2. Fallback                          – echo/stub for testing.
//
// The macro is auto-detected from header availability but can be overridden.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "voice_runtime/Interfaces.h"

// ── Detect library availability ────────────────────────────────────────────

#ifndef VOICE_RUNTIME_HTTP_LLM_AVAILABLE

// httplib
#  if __has_include("httplib.h")
#    define VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB 1
#    include "httplib.h"
#  elif __has_include(<httplib.h>)
#    define VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB 1
#    include <httplib.h>
#  elif __has_include(<cpp-httplib/httplib.h>)
#    define VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB 1
#    include <cpp-httplib/httplib.h>
#  else
#    define VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB 0
#  endif

// nlohmann/json
#  if __has_include(<nlohmann/json.hpp>)
#    define VOICE_RUNTIME_HTTP_LLM_HAS_JSON 1
#    include <nlohmann/json.hpp>
#  elif __has_include("nlohmann/json.hpp")
#    define VOICE_RUNTIME_HTTP_LLM_HAS_JSON 1
#    include "nlohmann/json.hpp"
#  elif __has_include("json.hpp")
#    define VOICE_RUNTIME_HTTP_LLM_HAS_JSON 1
#    include "json.hpp"
#  elif __has_include(<json.hpp>)
#    define VOICE_RUNTIME_HTTP_LLM_HAS_JSON 1
#    include <json.hpp>
#  else
#    define VOICE_RUNTIME_HTTP_LLM_HAS_JSON 0
#  endif

#  if VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB && VOICE_RUNTIME_HTTP_LLM_HAS_JSON
#    define VOICE_RUNTIME_HTTP_LLM_AVAILABLE 1
#  else
#    define VOICE_RUNTIME_HTTP_LLM_AVAILABLE 0
#  endif

#endif // !defined(VOICE_RUNTIME_HTTP_LLM_AVAILABLE)

// If VOICE_RUNTIME_HTTP_LLM_AVAILABLE was set to 1 externally but we haven't
// included the headers yet, include them now.
#if VOICE_RUNTIME_HTTP_LLM_AVAILABLE
#  if !defined(VOICE_RUNTIME_HTTP_LLM_HAS_HTTPLIB)
#    if __has_include("httplib.h")
#      include "httplib.h"
#    elif __has_include(<httplib.h>)
#      include <httplib.h>
#    elif __has_include(<cpp-httplib/httplib.h>)
#      include <cpp-httplib/httplib.h>
#    endif
#  endif
#  if !defined(VOICE_RUNTIME_HTTP_LLM_HAS_JSON)
#    if __has_include(<nlohmann/json.hpp>)
#      include <nlohmann/json.hpp>
#    elif __has_include("nlohmann/json.hpp")
#      include "nlohmann/json.hpp"
#    elif __has_include("json.hpp")
#      include "json.hpp"
#    endif
#  endif
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// HttpLlmConfig
// ---------------------------------------------------------------------------

struct HttpLlmConfig {
    std::string baseUrl      = "http://127.0.0.1:8080";
    std::string apiPath      = "/v1/chat/completions";
    std::string model        = "default";
    std::string apiKey;
    std::string systemPrompt = "You are a helpful assistant.";

    float temperature = 0.7f;
    float topP        = 0.9f;
    float minP        = 0.05f;
    int   maxTokens   = 4096;
    bool  stream      = true;

    int connectTimeoutSec = 5;
    int readTimeoutSec    = 60;

    enum class ThinkMode { Think, NoThink };
    ThinkMode thinkMode = ThinkMode::Think;

    // Maximum number of messages kept in conversation history (0 = unlimited).
    // System prompt is always preserved; oldest user/assistant pairs are
    // trimmed when the limit is exceeded.
    int maxHistoryMessages = 0;

    // Optional raw JSON string containing tool definitions.
    // Passed through as-is in the API request "tools" field.
    std::string toolsJson;

    // Retry policy on transient failures (connection refused, 5xx, etc.).
    int  maxRetries      = 2;
    int  retryBaseMs     = 500;  // exponential back-off base

    // When true and httplib is unavailable, the node runs in stub mode that
    // echoes input with a "[LLM-STUB] " prefix.  Useful for pipeline testing.
    bool allowStubFallback = true;

    // Force stub mode even when httplib is available (for testing)
    bool forceStubMode = false;
};

// ---------------------------------------------------------------------------
// HttpLlmNode
// ---------------------------------------------------------------------------

class HttpLlmNode final
    : public ActiveNodeBase
    , public ILanguageModelNode
{
public:
    explicit HttpLlmNode(HttpLlmConfig config)
        : cfg_(std::move(config))
    {}

    const char* name() const override { return "HttpLLM"; }

    bool initialize() override {
#if VOICE_RUNTIME_HTTP_LLM_AVAILABLE
        if (cfg_.forceStubMode) {
            logErr("initialize: forceStubMode=true — running in stub mode");
            stubMode_ = true;
            initialized_ = true;
            return true;
        }
        client_ = makeClient();
        if (!client_) {
            if (cfg_.allowStubFallback) {
                logErr("initialize: failed to create HTTP client — falling back to stub mode");
                stubMode_ = true;
                initialized_ = true;
                return true;
            }
            logErr("initialize: failed to create HTTP client for %s",
                   cfg_.baseUrl.c_str());
            return false;
        }
        initConversationHistory();
        initialized_ = true;
        return true;
#else
        if (cfg_.allowStubFallback) {
            logErr("initialize: httplib/json not available — running in stub mode");
            stubMode_ = true;
            initialized_ = true;
            return true;
        }
        logErr("initialize: httplib/json not available and stub fallback disabled");
        return false;
#endif
    }

    // ILanguageModelNode ─────────────────────────────────────────────────────

    void setInputQueue(TextQueue* q)                      override { in_     = q; }
    void setOutputQueue(TextQueue* q)                     override { out_    = q; }
    void setPersistentInputBuffer(DiskBackedTextBuffer* b) override { disk_   = b; }
    void setInterruptSignal(InterruptSignal* s)           override { intSig_ = s; }

protected:
    void runLoop() override {
        if (!initialized_) return;

        while (running()) {
            TextChunk input;
            if (!in_ || !in_->pop(input)) break;

            // Log input to disk buffer
            if (disk_) disk_->append("[USER] " + input.text + "\n");

            // Clear any stale interrupt before starting a new request
            if (intSig_) intSig_->clear();

#if VOICE_RUNTIME_HTTP_LLM_AVAILABLE
            if (!stubMode_) {
                if (handleRealRequest(input)) {
                    continue;
                }
                if (!cfg_.allowStubFallback) {
                    // No fallback: push empty final chunk to prevent hanging
                    if (out_) {
                        TextChunk fin;
                        fin.text        = "";
                        fin.isFinal     = true;
                        fin.sequence    = ++outSeq_;
                        fin.timestampNs = nowNs();
                        out_->push(std::move(fin));
                    }
                    continue;
                }
                logErr("Real request failed — falling back to stub response");
            }
#endif
            // Stub / simulation fallback
            handleStubRequest(input);
        }
    }

private:
    // ── Timestamp helper ────────────────────────────────────────────────────

    static uint64_t nowNs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // ── Logging ─────────────────────────────────────────────────────────────

    static void logErr(const char* msg) {
        std::fprintf(stderr, "[HttpLLM] %s\n", msg);
        std::fflush(stderr);
    }

    template <typename... Args>
    static void logErr(const char* fmt, Args&&... args) {
        std::fprintf(stderr, "[HttpLLM] ");
        // NOLINTNEXTLINE(format-security)
        std::fprintf(stderr, fmt, std::forward<Args>(args)...);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }

    // ── Stub fallback ───────────────────────────────────────────────────────

    void handleStubRequest(const TextChunk& input) {
        // Simulate token-by-token streaming with ~5 tokens
        const std::string response = "[LLM-STUB] Response to: " + input.text;

        // Split into word-ish tokens
        std::vector<std::string> tokens;
        {
            std::string tok;
            for (char ch : response) {
                tok += ch;
                if (ch == ' ' || ch == '.' || ch == ',' || ch == ':') {
                    tokens.push_back(tok);
                    tok.clear();
                }
            }
            if (!tok.empty()) tokens.push_back(tok);
        }

        std::string accumulated;
        for (std::size_t i = 0; i < tokens.size() && running(); ++i) {
            // Check interrupt
            if (intSig_ && intSig_->check()) {
                intSig_->clear();
                logErr("stub: interrupted after %zu tokens", i);
                return;
            }

            accumulated += tokens[i];

            TextChunk chunk;
            chunk.text        = tokens[i];
            chunk.isFinal     = false;
            chunk.sequence    = ++outSeq_;
            chunk.timestampNs = nowNs();
            if (out_) out_->push(std::move(chunk));

            // Simulate generation latency
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        // Final chunk
        {
            TextChunk fin;
            fin.text        = "";
            fin.isFinal     = true;
            fin.sequence    = ++outSeq_;
            fin.timestampNs = nowNs();
            if (out_) out_->push(std::move(fin));
        }

        if (disk_) disk_->append("[ASSISTANT] " + accumulated + "\n");
    }

    // ── Real HTTP implementation ────────────────────────────────────────────

#if VOICE_RUNTIME_HTTP_LLM_AVAILABLE

    // JSON alias for convenience
    using json = nlohmann::json;

    std::unique_ptr<httplib::Client> makeClient() const {
        auto cli = std::make_unique<httplib::Client>(cfg_.baseUrl);
        if (!cli) return nullptr;

        cli->set_connection_timeout(cfg_.connectTimeoutSec, 0);
        cli->set_read_timeout(cfg_.readTimeoutSec, 0);
        cli->set_write_timeout(cfg_.connectTimeoutSec, 0);

        // Follow redirects
        cli->set_follow_location(true);

        return cli;
    }

    void initConversationHistory() {
        messages_ = json::array();
        if (!cfg_.systemPrompt.empty()) {
            messages_.push_back({
                {"role", "system"},
                {"content", cfg_.systemPrompt}
            });
        }
    }

    void addUserMessage(const std::string& text) {
        std::lock_guard<std::mutex> lk(historyMutex_);
        messages_.push_back({
            {"role", "user"},
            {"content", text}
        });
        trimHistory();
    }

    void addAssistantMessage(const std::string& text) {
        std::lock_guard<std::mutex> lk(historyMutex_);
        messages_.push_back({
            {"role", "assistant"},
            {"content", text}
        });
        trimHistory();
    }

    void trimHistory() {
        // historyMutex_ must be held by caller.
        if (cfg_.maxHistoryMessages <= 0) return;
        const auto limit = static_cast<std::size_t>(cfg_.maxHistoryMessages);

        // Preserve the system prompt (always messages_[0] if present).
        const std::size_t systemCount =
            (!messages_.empty() && messages_[0].value("role", "") == "system") ? 1 : 0;
        const std::size_t nonSystem = messages_.size() - systemCount;
        if (nonSystem <= limit) return;

        const std::size_t toRemove = nonSystem - limit;
        messages_.erase(messages_.begin() + static_cast<json::difference_type>(systemCount),
                        messages_.begin() + static_cast<json::difference_type>(systemCount + toRemove));
    }

    json buildRequestBody() const {
        json body;
        body["model"]       = cfg_.model;
        body["stream"]      = cfg_.stream;
        body["temperature"] = cfg_.temperature;
        body["top_p"]       = cfg_.topP;

        if (cfg_.maxTokens > 0) {
            body["max_tokens"] = cfg_.maxTokens;
        }

        // min_p is supported by some providers (e.g. vLLM, llama.cpp)
        if (cfg_.minP > 0.0f) {
            body["min_p"] = cfg_.minP;
        }

        {
            std::lock_guard<std::mutex> lk(historyMutex_);
            body["messages"] = messages_;
        }

        // Tool definitions
        if (!cfg_.toolsJson.empty()) {
            try {
                body["tools"] = json::parse(cfg_.toolsJson);
            } catch (const json::parse_error& e) {
                logErr("buildRequestBody: failed to parse toolsJson: %s", e.what());
            }
        }

        // ThinkMode — provider-specific; common with some open-source APIs
        if (cfg_.thinkMode == HttpLlmConfig::ThinkMode::NoThink) {
            // Convention used by some providers
            body["think"] = false;
        }

        return body;
    }

    httplib::Headers buildHeaders() const {
        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        if (!cfg_.apiKey.empty()) {
            headers.emplace("Authorization", "Bearer " + cfg_.apiKey);
        }
        // SSE: prevent buffering
        headers.emplace("Accept", "text/event-stream");
        headers.emplace("Cache-Control", "no-cache");
        return headers;
    }

    bool handleRealRequest(const TextChunk& input) {
        addUserMessage(input.text);

        const json body    = buildRequestBody();
        const auto headers = buildHeaders();
        const std::string bodyStr = body.dump();

        bool success = false;

        for (int attempt = 0; attempt <= cfg_.maxRetries && running(); ++attempt) {
            if (attempt > 0) {
                const int delayMs = cfg_.retryBaseMs * (1 << (attempt - 1));
                logErr("retrying request (attempt %d/%d) in %dms",
                       attempt + 1, cfg_.maxRetries + 1, delayMs);
                // Interruptible sleep
                for (int elapsed = 0; elapsed < delayMs && running(); elapsed += 50) {
                    if (intSig_ && intSig_->check()) {
                        intSig_->clear();
                        logErr("interrupted during retry backoff");
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        std::min(50, delayMs - elapsed)));
                }
                if (!running()) return false;

                // Recreate client in case the connection was dropped
                client_ = makeClient();
                if (!client_) {
                    logErr("failed to recreate HTTP client");
                    continue;
                }
            }

            if (cfg_.stream) {
                success = doStreamingRequest(bodyStr, headers);
            } else {
                success = doBlockingRequest(bodyStr, headers);
            }

            if (success) break;

            // Check interrupt between retries
            if (intSig_ && intSig_->check()) {
                intSig_->clear();
                logErr("interrupted after failed attempt");
                return false;
            }
        }

        if (!success) {
            logErr("all request attempts failed for input: %.80s...", input.text.c_str());
            // Remove the user message we added since we got no response
            {
                std::lock_guard<std::mutex> lk(historyMutex_);
                if (!messages_.empty() &&
                    messages_.back().value("role", "") == "user") {
                    messages_.erase(messages_.end() - 1);
                }
            }
        }
        return success;
    }

    // ── Streaming SSE request ───────────────────────────────────────────────
    //
    // Note: cpp-httplib does not support Post() with a ContentReceiver callback.
    // We perform a normal Post, then parse the SSE body line-by-line.
    // For true per-chunk streaming, a lower-level socket approach would be needed.
    // This is sufficient for correctness and works well for non-interactive use.

    bool doStreamingRequest(const std::string& bodyStr,
                            const httplib::Headers& headers) {
        std::string fullResponse;
        std::string lineBuffer;
        bool interrupted = false;
        bool receivedDone = false;
        int responseStatus = 200;
        std::string errorBody;

        // Check for interrupt before request
        if (intSig_ && intSig_->check()) {
            intSig_->clear();
            logErr("streaming: interrupted before request");
            return true;
        }

        // Prepare request
        httplib::Request req;
        req.method = "POST";
        req.path = cfg_.apiPath;
        req.headers = headers;
        req.body = bodyStr;

        req.response_handler = [&](const httplib::Response &response) -> bool {
            responseStatus = response.status;
            return true;
        };

        req.content_receiver = [&](const char *data, size_t data_len, uint64_t /*offset*/, uint64_t /*total*/) -> bool {
            if (responseStatus != 200) {
                errorBody.append(data, data_len);
                return true;
            }
            lineBuffer.append(data, data_len);

            // Extract lines
            std::size_t pos = 0;
            while (true) {
                auto nlPos = lineBuffer.find('\n', pos);
                if (nlPos == std::string::npos) {
                    break;
                }

                std::string line = lineBuffer.substr(pos, nlPos - pos);
                pos = nlPos + 1;

                // Strip trailing \r
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                // Skip empty lines
                if (line.empty()) continue;

                // Process "data: " lines
                if (line.rfind("data: ", 0) != 0) continue;

                std::string payload = line.substr(6);

                // End of stream
                if (payload == "[DONE]") {
                    receivedDone = true;
                    TextChunk fin;
                    fin.text        = "";
                    fin.isFinal     = true;
                    fin.sequence    = ++outSeq_;
                    fin.timestampNs = nowNs();
                    if (out_) out_->push(std::move(fin));
                    continue;
                }

                // Parse JSON chunk
                try {
                    auto j = json::parse(payload);
                    processStreamChunk(j, fullResponse);
                } catch (const std::exception& e) {
                    // Ignore parse/type errors of incomplete or malformed JSON
                }
            }

            if (pos > 0) {
                lineBuffer.erase(0, pos);
            }

            // Check interrupt mid-parse
            if (intSig_ && intSig_->check()) {
                intSig_->clear();
                interrupted = true;
                logErr("streaming: interrupted during parse");
                return false; // stop receiving
            }
            if (!running()) {
                return false; // stop receiving
            }

            return true; // continue receiving
        };

        // Perform the POST request using send()
        auto result = client_->send(req);

        // Check for interrupt after request
        if (intSig_ && intSig_->check()) {
            intSig_->clear();
            interrupted = true;
            logErr("streaming: interrupted after request");
        }

        if (interrupted) {
            return true;
        }

        if (!result) {
            logErr("streaming: request failed (error: %s)",
                   httplib::to_string(result.error()).c_str());
            return false;
        }

        if (result->status != 200) {
            logErr("streaming: HTTP %d — %s",
                   result->status, errorBody.c_str());
            return false;
        }

        // Handle remaining partial line if connection closed without [DONE]
        if (!lineBuffer.empty()) {
            std::string line = lineBuffer;
            if (line.rfind("data: ", 0) == 0) {
                std::string payload = line.substr(6);
                if (payload != "[DONE]") {
                    try {
                        auto j = json::parse(payload);
                        processStreamChunk(j, fullResponse);
                    } catch (...) {}
                }
            }
        }

        // If we didn't receive [DONE] but the connection closed cleanly,
        // still emit a final if we have content
        if (!receivedDone && !fullResponse.empty()) {
            TextChunk fin;
            fin.text        = "";
            fin.isFinal     = true;
            fin.sequence    = ++outSeq_;
            fin.timestampNs = nowNs();
            if (out_) out_->push(std::move(fin));
        }

        // Add completed response to conversation history
        if (!fullResponse.empty()) {
            addAssistantMessage(fullResponse);
            if (disk_) disk_->append("[ASSISTANT] " + fullResponse + "\n");
        }

        return true;
    }

    void processStreamChunk(const json& j, std::string& fullResponse) {
        // Standard OpenAI SSE format:
        //   {"choices":[{"delta":{"content":"token"}}]}

        if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            return;
        }

        const auto& choice = j["choices"][0];
        if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object()) {
            return;
        }

        const auto& delta = choice["delta"];

        // Text content
        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string token = delta["content"].get<std::string>();
            if (!token.empty()) {
                fullResponse += token;

                TextChunk chunk;
                chunk.text        = token;
                chunk.isFinal     = false;
                chunk.sequence    = ++outSeq_;
                chunk.timestampNs = nowNs();
                if (out_) out_->push(std::move(chunk));
            }
        }

        // Tool calls — emit as special text chunk for InterpreterNode
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            const std::string toolCallStr = delta["tool_calls"].dump();
            const std::string tagged = "[TOOL_CALL]" + toolCallStr;
            fullResponse += tagged;

            TextChunk chunk;
            chunk.text        = tagged;
            chunk.isFinal     = false;
            chunk.sequence    = ++outSeq_;
            chunk.timestampNs = nowNs();
            if (out_) out_->push(std::move(chunk));
        }

        // Reasoning / thinking content (some providers, e.g. OpenAI/DeepSeek uses reasoning_content, Ollama uses reasoning)
        std::string reasoning;
        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            reasoning = delta["reasoning_content"].get<std::string>();
        } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
            reasoning = delta["reasoning"].get<std::string>();
        }

        if (!reasoning.empty()) {
            // Pass through <think> tags for downstream InterpreterNode
            const std::string tagged = "<think>" + reasoning + "</think>";
            fullResponse += tagged;

            TextChunk chunk;
            chunk.text        = tagged;
            chunk.isFinal     = false;
            chunk.sequence    = ++outSeq_;
            chunk.timestampNs = nowNs();
            if (out_) out_->push(std::move(chunk));
        }
    }

    // ── Non-streaming (blocking) request ────────────────────────────────────

    bool doBlockingRequest(const std::string& bodyStr,
                           const httplib::Headers& headers) {
        auto result = client_->Post(cfg_.apiPath, headers, bodyStr, "application/json");

        if (!result) {
            logErr("blocking: request failed (error: %s)",
                   httplib::to_string(result.error()).c_str());
            return false;
        }

        if (result->status != 200) {
            logErr("blocking: HTTP %d — %.200s",
                   result->status, result->body.c_str());
            return false;
        }

        try {
            auto j = json::parse(result->body);
            if (j.contains("choices") && j["choices"].is_array() &&
                !j["choices"].empty()) {
                const auto& choice = j["choices"][0];
                if (choice.contains("message") &&
                    choice["message"].contains("content")) {
                    const std::string content =
                        choice["message"]["content"].get<std::string>();

                    // Emit as a single chunk + final
                    if (!content.empty()) {
                        TextChunk chunk;
                        chunk.text        = content;
                        chunk.isFinal     = false;
                        chunk.sequence    = ++outSeq_;
                        chunk.timestampNs = nowNs();
                        if (out_) out_->push(std::move(chunk));
                    }

                    TextChunk fin;
                    fin.text        = "";
                    fin.isFinal     = true;
                    fin.sequence    = ++outSeq_;
                    fin.timestampNs = nowNs();
                    if (out_) out_->push(std::move(fin));

                    addAssistantMessage(content);
                    if (disk_) disk_->append("[ASSISTANT] " + content + "\n");
                }

                // Handle tool_calls in non-streaming
                if (choice.contains("message") &&
                    choice["message"].contains("tool_calls")) {
                    const std::string tc =
                        choice["message"]["tool_calls"].dump();
                    const std::string tagged = "[TOOL_CALL]" + tc;

                    TextChunk chunk;
                    chunk.text        = tagged;
                    chunk.isFinal     = false;
                    chunk.sequence    = ++outSeq_;
                    chunk.timestampNs = nowNs();
                    if (out_) out_->push(std::move(chunk));

                    addAssistantMessage(tagged);
                }
            }
        } catch (const json::parse_error& e) {
            logErr("blocking: JSON parse error: %s", e.what());
            return false;
        }

        return true;
    }

#endif // VOICE_RUNTIME_HTTP_LLM_AVAILABLE

    // ── Members ─────────────────────────────────────────────────────────────

    HttpLlmConfig         cfg_;
    TextQueue*            in_     = nullptr;
    TextQueue*            out_    = nullptr;
    DiskBackedTextBuffer* disk_   = nullptr;
    InterruptSignal*      intSig_ = nullptr;

    uint64_t              outSeq_      = 0;
    bool                  initialized_ = false;
    bool                  stubMode_    = false;

#if VOICE_RUNTIME_HTTP_LLM_AVAILABLE

    std::unique_ptr<httplib::Client> client_;
    json                             messages_;
    mutable std::mutex               historyMutex_;
#endif
};

} // namespace voice_runtime
