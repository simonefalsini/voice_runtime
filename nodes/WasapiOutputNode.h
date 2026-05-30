#pragma once

// ---------------------------------------------------------------------------
// WasapiOutputNode.h
//
// Windows-native audio output node using WASAPI (Windows Audio Session API).
// Implements IAudioOutputNode — drop-in replacement for MiniaudioOutputNode on Windows.
//
// Features:
//   • Event-driven render (IAudioRenderClient + SetEventHandle) — zero busy-wait
//   • SHARED mode (default) with EXCLUSIVE mode opt-in via config
//   • Automatic format conversion: Int16/Float32 source → device native format
//     - Supports WAVE_FORMAT_PCM and WAVE_FORMAT_IEEE_FLOAT device formats
//     - Mono → stereo upmix if device requires it
//     - Linear resampling if source rate ≠ device rate
//   • Underrun prevention: fills with silence when the AudioFrameQueue is empty
//   • COM lifecycle owned by the worker thread (CoInitializeEx / CoUninitialize)
//
// Usage:
//   WasapiOutputConfig cfg;
//   cfg.format = {24000, 1, 10, SampleFormat::Int16};
//   WasapiOutputNode out(cfg);
//   out.setInputQueue(&speakerQueue);
//   out.initialize();
//   out.start();
//   ...
//   out.stop();
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
#  error "WasapiOutputNode.h is only available on Windows"
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace voice_runtime {

// ---------------------------------------------------------------------------
// WasapiOutputConfig
// ---------------------------------------------------------------------------

struct WasapiOutputConfig {
    /// Source audio format consumed from the AudioFrameQueue.
    AudioFormat format = {24000, 1, 10, SampleFormat::Int16};

    /// SHARED mode (false, default) or EXCLUSIVE mode (true).
    bool enableExclusiveMode = false;

    /// Pre-buffer size in source frames before starting playback.
    /// Provides a cushion against transient queue underruns.
    std::size_t prebufferFrames = 5;

    /// Device ID (L"" = default render endpoint).
    std::wstring deviceId = L"";
};

// ---------------------------------------------------------------------------
// WasapiOutputNode
// ---------------------------------------------------------------------------

class WasapiOutputNode final
    : public ActiveNodeBase
    , public IAudioOutputNode
{
public:
    explicit WasapiOutputNode(WasapiOutputConfig config)
        : cfg_(std::move(config))
    {}

    ~WasapiOutputNode() override = default;

    const char* name() const override { return "WasapiOutput"; }

    void setInputQueue(AudioFrameQueue* q) override { in_ = q; }

    bool initialize() override {
        initialized_.store(true, std::memory_order_release);
        return true;
    }

protected:
    void wake() override {
        if (wakeEvent_) SetEvent(wakeEvent_);
    }

    void runLoop() override {
        if (!initialized_.load(std::memory_order_acquire)) return;

        // COM initialization
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
            std::fprintf(stderr, "[WasapiOutput] CoInitializeEx failed: 0x%08lX\n", hrCo);
            return;
        }
        const bool coInitialized = SUCCEEDED(hrCo);

        auto cleanup = [&] {
            if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
            if (audioClient_)  { audioClient_->Release();  audioClient_  = nullptr; }
            if (device_)       { device_->Release();        device_       = nullptr; }
            if (enumerator_)   { enumerator_->Release();    enumerator_   = nullptr; }
            if (renderEvent_)  { CloseHandle(renderEvent_); renderEvent_  = nullptr; }
            if (wakeEvent_)    { CloseHandle(wakeEvent_);   wakeEvent_    = nullptr; }
            if (pwfx_)         { CoTaskMemFree(pwfx_);      pwfx_         = nullptr; }
            if (coInitialized) CoUninitialize();
        };

        // ---- Device enumeration -----------------------------------------------
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] CoCreateInstance failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        if (cfg_.deviceId.empty()) {
            hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        } else {
            hr = enumerator_->GetDevice(cfg_.deviceId.c_str(), &device_);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] GetDefaultAudioEndpoint failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- IAudioClient activation ------------------------------------------
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] Activate(IAudioClient) failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- Format negotiation -----------------------------------------------
        hr = audioClient_->GetMixFormat(&pwfx_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] GetMixFormat failed: 0x%08lX\n", hr);
            cleanup(); return;
        }
        devSampleRate_ = static_cast<int>(pwfx_->nSamplesPerSec);
        devChannels_   = static_cast<int>(pwfx_->nChannels);
        devIsFloat_    = isFloatFormat(pwfx_);

        std::printf("[WasapiOutput] Device: %dHz %dch %s\n",
                    devSampleRate_, devChannels_,
                    devIsFloat_ ? "Float32" : "Int16");

        // ---- Stream initialization --------------------------------------------
        const AUDCLNT_SHAREMODE shareMode = cfg_.enableExclusiveMode
            ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

        const REFERENCE_TIME bufDuration = 2000000; // 200ms in 100-ns units
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        WAVEFORMATEX* pInitFmt = cfg_.enableExclusiveMode
            ? buildTargetFormat() : pwfx_;

        hr = audioClient_->Initialize(shareMode, flags, bufDuration, 0, pInitFmt, nullptr);
        if (cfg_.enableExclusiveMode && pInitFmt != pwfx_) {
            CoTaskMemFree(pInitFmt);
            pInitFmt = nullptr;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] Initialize failed: 0x%08lX%s\n", hr,
                         cfg_.enableExclusiveMode
                             ? " (try SHARED mode or match device sample rate)" : "");
            cleanup(); return;
        }

        UINT32 hwBufFrames = 0;
        audioClient_->GetBufferSize(&hwBufFrames);

        // ---- Event handles ---------------------------------------------------
        renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        wakeEvent_   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!renderEvent_ || !wakeEvent_) {
            std::fprintf(stderr, "[WasapiOutput] CreateEvent failed\n");
            cleanup(); return;
        }

        hr = audioClient_->SetEventHandle(renderEvent_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] SetEventHandle failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- IAudioRenderClient ----------------------------------------------
        hr = audioClient_->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(&renderClient_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] GetService(IAudioRenderClient) failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- Pre-buffer silence to avoid click at start ----------------------
        {
            BYTE* pData = nullptr;
            hr = renderClient_->GetBuffer(hwBufFrames, &pData);
            if (SUCCEEDED(hr)) {
                std::memset(pData, 0,
                            hwBufFrames * pwfx_->nBlockAlign);
                renderClient_->ReleaseBuffer(hwBufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }

        // ---- Start streaming -------------------------------------------------
        hr = audioClient_->Start();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiOutput] Start failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        std::printf("[WasapiOutput] Streaming — source: %dHz %dch %dms %s\n",
                    cfg_.format.sampleRate, cfg_.format.channels, cfg_.format.frameMs,
                    cfg_.enableExclusiveMode ? "(EXCLUSIVE)" : "(SHARED)");

        const int srcFrameSamples = cfg_.format.samplesPerChannelPerFrame();
        const bool needsResample = (cfg_.format.sampleRate != devSampleRate_);

        // ---- Render loop -----------------------------------------------------
        while (running()) {
            const HANDLE events[2] = { renderEvent_, wakeEvent_ };
            const DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, 200 /*ms*/);
            if (waitRes == WAIT_FAILED || waitRes == WAIT_OBJECT_0 + 1) break;
            if (waitRes == WAIT_TIMEOUT) continue;

            // Determine how many device frames we can write
            UINT32 padding = 0;
            hr = audioClient_->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;

            const UINT32 framesAvail = hwBufFrames - padding;
            if (framesAvail == 0) continue;

            BYTE* pRenderData = nullptr;
            hr = renderClient_->GetBuffer(framesAvail, &pRenderData);
            if (FAILED(hr)) continue;

            // Fill renderBuf_ with source frames from the queue
            // renderBuf_ holds Float32 samples at device rate, device channels
            const std::size_t devSamplesNeeded =
                static_cast<std::size_t>(framesAvail) * static_cast<std::size_t>(devChannels_);

            renderBuf_.resize(devSamplesNeeded, 0.0f);

            // Consume source frames
            std::size_t devSamplesWritten = 0;
            while (devSamplesWritten < devSamplesNeeded && running()) {
                // Fill srcBuf_ (Int16/Float32, source rate, source channels) from queue
                if (srcBuf_.empty()) {
                    AudioFrameHandle frame;
                    if (in_ && in_->tryPop(frame)) {
                        srcBuf_ = toFloat32Mono(*frame);
                    } else {
                        break; // queue empty — will fill remaining with silence
                    }
                }

                // Resample srcBuf_ (source rate, mono) → devRate mono if needed
                if (needsResample && !srcBuf_.empty()) {
                    resampleToDevice(srcBuf_, srcResampledBuf_,
                                     cfg_.format.sampleRate, devSampleRate_);
                    srcBuf_.swap(srcResampledBuf_);
                    srcResampledBuf_.clear();
                }

                // Consume from srcBuf_ and write to renderBuf_ (upmix if needed)
                while (!srcBuf_.empty() && devSamplesWritten < devSamplesNeeded) {
                    const float sample = srcBuf_.front();
                    srcBuf_.erase(srcBuf_.begin());
                    for (int ch = 0; ch < devChannels_; ++ch) {
                        renderBuf_[devSamplesWritten++] = sample;
                    }
                }
            }

            // Fill any remaining gap with silence
            for (std::size_t i = devSamplesWritten; i < devSamplesNeeded; ++i) {
                renderBuf_[i] = 0.0f;
            }

            // Write to device buffer
            if (devIsFloat_) {
                std::memcpy(pRenderData, renderBuf_.data(),
                            devSamplesNeeded * sizeof(float));
            } else {
                auto* dst16 = reinterpret_cast<int16_t*>(pRenderData);
                for (std::size_t i = 0; i < devSamplesNeeded; ++i) {
                    const float clamped = std::max(-1.0f, std::min(1.0f, renderBuf_[i]));
                    dst16[i] = static_cast<int16_t>(clamped * 32767.0f);
                }
            }

            renderClient_->ReleaseBuffer(framesAvail, 0);
        }

        // ---- Teardown --------------------------------------------------------
        audioClient_->Stop();
        cleanup();
    }

private:
    // ---- Format helpers ------------------------------------------------------

    static bool isFloatFormat(const WAVEFORMATEX* wfx) noexcept {
        if (!wfx) return false;
        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
            static const GUID kGuidFloat = {
                0x00000003, 0x0000, 0x0010,
                {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}
            };
            return IsEqualGUID(wfxe->SubFormat, kGuidFloat) != 0;
        }
        return false;
    }

    WAVEFORMATEX* buildTargetFormat() const {
        auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!wfx) return nullptr;
        wfx->wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        wfx->nChannels       = static_cast<WORD>(devChannels_);
        wfx->nSamplesPerSec  = static_cast<DWORD>(cfg_.format.sampleRate);
        wfx->wBitsPerSample  = 32;
        wfx->nBlockAlign     = wfx->nChannels * 4;
        wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
        wfx->cbSize          = 0;
        return wfx;
    }

    // ---- Convert an AudioFrame to Float32 mono at source sample rate ---------

    std::vector<float> toFloat32Mono(const AudioFrame& frame) const {
        const int total = frame.format.totalSamplesPerFrame();
        const int ch    = frame.format.channels;
        const int mono  = total / ch;
        std::vector<float> out(static_cast<std::size_t>(mono));

        if (frame.format.sampleFormat == SampleFormat::Int16) {
            for (int i = 0; i < mono; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < ch; ++c)
                    sum += static_cast<float>(frame.pcm16[i * ch + c]) / 32768.0f;
                out[static_cast<std::size_t>(i)] = sum / static_cast<float>(ch);
            }
        } else {
            for (int i = 0; i < mono; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < ch; ++c)
                    sum += frame.pcmF32[static_cast<std::size_t>(i * ch + c)];
                out[static_cast<std::size_t>(i)] = sum / static_cast<float>(ch);
            }
        }
        return out;
    }

    // ---- Linear resampler: src (srcRate, mono Float32) → dst (dstRate, mono) -

    static void resampleToDevice(const std::vector<float>& src, std::vector<float>& dst,
                                  int srcRate, int dstRate) {
        if (src.empty()) { dst.clear(); return; }
        const double ratio = static_cast<double>(dstRate) / static_cast<double>(srcRate);
        const std::size_t outLen = static_cast<std::size_t>(
            static_cast<double>(src.size()) * ratio);
        dst.resize(outLen);
        for (std::size_t i = 0; i < outLen; ++i) {
            const double srcPos = static_cast<double>(i) / ratio;
            const std::size_t i0 = static_cast<std::size_t>(srcPos);
            const std::size_t i1 = std::min(i0 + 1, src.size() - 1);
            const float frac = static_cast<float>(srcPos - static_cast<double>(i0));
            dst[i] = src[i0] * (1.0f - frac) + src[i1] * frac;
        }
    }

    // ---- Config & state ------------------------------------------------------

    WasapiOutputConfig cfg_;
    AudioFrameQueue*   in_ = nullptr;

    std::atomic<bool>  initialized_{false};

    // COM objects — owned by the runLoop thread
    IMMDeviceEnumerator*  enumerator_   = nullptr;
    IMMDevice*            device_       = nullptr;
    IAudioClient*         audioClient_  = nullptr;
    IAudioRenderClient*   renderClient_ = nullptr;
    WAVEFORMATEX*         pwfx_         = nullptr;

    // Event handles
    HANDLE renderEvent_ = nullptr;
    HANDLE wakeEvent_   = nullptr;

    // Device format properties
    int  devSampleRate_ = 48000;
    int  devChannels_   = 2;
    bool devIsFloat_    = true;

    // Working buffers
    std::vector<float> srcBuf_;           // mono Float32 at source rate, unconsumed
    std::vector<float> srcResampledBuf_;  // temp resampled buffer
    std::vector<float> renderBuf_;        // multi-channel Float32 at device rate
};

} // namespace voice_runtime
