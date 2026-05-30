#pragma once

// ---------------------------------------------------------------------------
// WasapiMicNode.h
//
// Windows-native microphone capture node using WASAPI (Windows Audio Session API).
// Implements IMicrophoneNode — drop-in replacement for MiniaudioMicNode on Windows.
//
// Features:
//   • Event-driven capture (IAudioCaptureClient + SetEventHandle) — zero busy-wait
//   • SHARED mode (default) with EXCLUSIVE mode opt-in via config
//   • Automatic format conversion: device native → target (16kHz mono Int16)
//     - Float32/Int16 device formats supported
//     - Stereo → mono downmix
//     - Linear resampling if device rate ≠ target rate
//   • DC removal (high-pass via mean subtraction)
//   • Pool-based frame allocation (SharedBufferPool<AudioFrame>)
//   • COM lifecycle owned by the worker thread (CoInitializeEx / CoUninitialize)
//
// Usage:
//   WasapiMicConfig cfg;
//   cfg.format = {16000, 1, 10, SampleFormat::Int16};
//   WasapiMicNode mic(cfg);
//   mic.setOutputQueue(&micQueue);
//   mic.initialize();
//   mic.start();
//   ...
//   mic.stop();
//
// Note: setDropCallback() must be called before start().
// ---------------------------------------------------------------------------

#if !defined(_WIN32)
#  error "WasapiMicNode.h is only available on Windows"
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

// CLSID_MMDeviceEnumerator / IID_IMMDeviceEnumerator are defined in mmdeviceapi.h
// and linked from mmdevapi.lib (included automatically on MSVC via #pragma comment).
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace voice_runtime {

// ---------------------------------------------------------------------------
// WasapiMicConfig
// ---------------------------------------------------------------------------

struct WasapiMicConfig {
    /// Target output format pushed to the AudioFrameQueue.
    AudioFormat format = {16000, 1, 10, SampleFormat::Int16};

    /// SHARED mode (false, default) or EXCLUSIVE mode (true).
    /// EXCLUSIVE requires the device to natively support cfg.format.sampleRate;
    /// if initialization fails in EXCLUSIVE mode the node logs an error and exits.
    bool enableExclusiveMode = false;

    /// Subtract DC offset (running mean) from captured samples.
    bool enableDcRemoval = true;

    /// Number of AudioFrame objects preallocated in the pool.
    std::size_t poolSize = 256;

    /// Device ID (L"" = default capture endpoint).
    std::wstring deviceId = L"";
};

// ---------------------------------------------------------------------------
// WasapiMicNode
// ---------------------------------------------------------------------------

class WasapiMicNode final
    : public ActiveNodeBase
    , public IMicrophoneNode
{
public:
    explicit WasapiMicNode(WasapiMicConfig config)
        : cfg_(std::move(config))
        , pool_(cfg_.poolSize)
    {}

    ~WasapiMicNode() override = default;

    const char* name() const override { return "WasapiMic"; }

    void setOutputQueue(AudioFrameQueue* q) override { out_ = q; }

    bool initialize() override {
        if (cfg_.format.channels <= 0 || cfg_.format.channels > 2) {
            std::fprintf(stderr, "[WasapiMic] Invalid channel count %d\n",
                         cfg_.format.channels);
            return false;
        }
        if (cfg_.format.frameMs != 10 && cfg_.format.frameMs != 20
                && cfg_.format.frameMs != 30) {
            std::fprintf(stderr, "[WasapiMic] frameMs must be 10, 20 or 30\n");
            return false;
        }
        initialized_.store(true, std::memory_order_release);
        return true;
    }

protected:
    void wake() override {
        pool_.stop();
        if (wakeEvent_) SetEvent(wakeEvent_);
    }

    void runLoop() override {
        if (!initialized_.load(std::memory_order_acquire)) return;

        // COM initialization — must be called on the thread that uses COM objects
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
            std::fprintf(stderr, "[WasapiMic] CoInitializeEx failed: 0x%08lX\n", hrCo);
            return;
        }
        const bool coInitialized = SUCCEEDED(hrCo);

        // RAII cleanup guard
        auto cleanup = [&] {
            if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
            if (audioClient_)   { audioClient_->Release();   audioClient_   = nullptr; }
            if (device_)        { device_->Release();         device_        = nullptr; }
            if (enumerator_)    { enumerator_->Release();     enumerator_    = nullptr; }
            if (captureEvent_)  { CloseHandle(captureEvent_); captureEvent_  = nullptr; }
            if (wakeEvent_)     { CloseHandle(wakeEvent_);    wakeEvent_     = nullptr; }
            if (pwfx_)          { CoTaskMemFree(pwfx_);       pwfx_          = nullptr; }
            if (coInitialized)  CoUninitialize();
        };

        // ---- Device enumeration -----------------------------------------------
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        if (cfg_.deviceId.empty()) {
            hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &device_);
        } else {
            hr = enumerator_->GetDevice(cfg_.deviceId.c_str(), &device_);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] GetDefaultAudioEndpoint failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- IAudioClient activation ------------------------------------------
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] Activate(IAudioClient) failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- Format negotiation -----------------------------------------------
        hr = audioClient_->GetMixFormat(&pwfx_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] GetMixFormat failed: 0x%08lX\n", hr);
            cleanup(); return;
        }
        deviceSampleRate_ = static_cast<int>(pwfx_->nSamplesPerSec);
        deviceChannels_   = static_cast<int>(pwfx_->nChannels);
        deviceIsFloat_    = isFloatFormat(pwfx_);

        std::printf("[WasapiMic] Device: %dHz %dch %s\n",
                    deviceSampleRate_, deviceChannels_,
                    deviceIsFloat_ ? "Float32" : "Int16");

        // ---- Stream initialization --------------------------------------------
        const AUDCLNT_SHAREMODE shareMode = cfg_.enableExclusiveMode
            ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

        // Request 100ms buffer in SHARED mode; EXCLUSIVE needs exact period.
        const REFERENCE_TIME bufDuration = 1000000; // 100ms in 100-ns units
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        // In EXCLUSIVE mode, use the target format; in SHARED, use mix format.
        WAVEFORMATEX* pInitFmt = cfg_.enableExclusiveMode
            ? buildTargetFormat() : pwfx_;

        hr = audioClient_->Initialize(shareMode, flags, bufDuration, 0, pInitFmt, nullptr);
        if (cfg_.enableExclusiveMode && pInitFmt != pwfx_) {
            CoTaskMemFree(pInitFmt);
            pInitFmt = nullptr;
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] Initialize failed: 0x%08lX%s\n", hr,
                         cfg_.enableExclusiveMode
                             ? " (try SHARED mode or match device sample rate)" : "");
            cleanup(); return;
        }

        // ---- Event handles ---------------------------------------------------
        captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        wakeEvent_    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!captureEvent_ || !wakeEvent_) {
            std::fprintf(stderr, "[WasapiMic] CreateEvent failed\n");
            cleanup(); return;
        }

        hr = audioClient_->SetEventHandle(captureEvent_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] SetEventHandle failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- IAudioCaptureClient ---------------------------------------------
        hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&captureClient_));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] GetService(IAudioCaptureClient) failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        // ---- Start streaming -------------------------------------------------
        hr = audioClient_->Start();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[WasapiMic] Start failed: 0x%08lX\n", hr);
            cleanup(); return;
        }

        std::printf("[WasapiMic] Streaming — target: %dHz %dch %dms %s\n",
                    cfg_.format.sampleRate, cfg_.format.channels, cfg_.format.frameMs,
                    cfg_.enableExclusiveMode ? "(EXCLUSIVE)" : "(SHARED)");

        const int targetFrameSamples = cfg_.format.samplesPerChannelPerFrame();

        // ---- Capture loop ----------------------------------------------------
        while (running()) {
            const HANDLE events[2] = { captureEvent_, wakeEvent_ };
            const DWORD waitRes = WaitForMultipleObjects(2, events, FALSE, 200 /*ms*/);
            if (waitRes == WAIT_FAILED || waitRes == WAIT_OBJECT_0 + 1) break; // wakeEvent or error
            if (waitRes == WAIT_TIMEOUT) continue;

            // Drain all available packets
            UINT32 packetSize = 0;
            while (SUCCEEDED(captureClient_->GetNextPacketSize(&packetSize))
                   && packetSize > 0 && running()) {

                BYTE*  data   = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                hr = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                // Convert device samples → Int16 mono at device sample rate
                const int devSamples = static_cast<int>(frames) * deviceChannels_;
                convertToInt16Mono(data, devSamples, deviceChannels_, deviceIsFloat_,
                                   silent, accumulator_);

                captureClient_->ReleaseBuffer(frames);

                // Resample if needed
                if (deviceSampleRate_ != cfg_.format.sampleRate) {
                    resampleAccumulator();
                }

                // DC removal
                if (cfg_.enableDcRemoval) {
                    applyDcRemoval();
                }

                // Cut accumulator into fixed-size output frames and push
                while (static_cast<int>(resampled_.size()) >= targetFrameSamples) {
                    auto frame = pool_.acquireWithTimeout(std::chrono::milliseconds(20));
                    if (!frame) {
                        if (!running()) goto capture_done;
                        break;
                    }

                    frame->format      = cfg_.format;
                    frame->resizeForFormat();
                    frame->sequence    = seq_++;
                    frame->timestampNs = nowNs();

                    std::memcpy(frame->pcm16.data(),
                                resampled_.data(),
                                static_cast<std::size_t>(targetFrameSamples) * sizeof(int16_t));

                    resampled_.erase(resampled_.begin(),
                                     resampled_.begin() + targetFrameSamples);

                    if (out_) out_->push(std::move(frame));
                }
            }
        }
        capture_done:

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
        wfx->wFormatTag      = WAVE_FORMAT_PCM;
        wfx->nChannels       = static_cast<WORD>(cfg_.format.channels);
        wfx->nSamplesPerSec  = static_cast<DWORD>(cfg_.format.sampleRate);
        wfx->wBitsPerSample  = 16;
        wfx->nBlockAlign     = wfx->nChannels * 2;
        wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
        wfx->cbSize          = 0;
        return wfx;
    }

    // ---- Sample conversion: device buffer → Int16 mono in accumulator_ ------

    void convertToInt16Mono(const BYTE* src, int devSamplesTotal, int devCh,
                             bool isFloat, bool silent,
                             std::vector<int16_t>& dst) {
        const int monoFrames = devSamplesTotal / devCh;
        const std::size_t dstBase = dst.size();
        dst.resize(dstBase + static_cast<std::size_t>(monoFrames));

        if (silent) {
            std::memset(dst.data() + dstBase, 0,
                        static_cast<std::size_t>(monoFrames) * sizeof(int16_t));
            return;
        }

        if (isFloat) {
            const auto* fSrc = reinterpret_cast<const float*>(src);
            for (int i = 0; i < monoFrames; ++i) {
                // Downmix channels (average)
                float sum = 0.0f;
                for (int ch = 0; ch < devCh; ++ch)
                    sum += fSrc[i * devCh + ch];
                const float mono = sum / static_cast<float>(devCh);
                const float clamped = std::max(-1.0f, std::min(1.0f, mono));
                dst[dstBase + static_cast<std::size_t>(i)] =
                    static_cast<int16_t>(clamped * 32767.0f);
            }
        } else {
            const auto* iSrc = reinterpret_cast<const int16_t*>(src);
            for (int i = 0; i < monoFrames; ++i) {
                int32_t sum = 0;
                for (int ch = 0; ch < devCh; ++ch)
                    sum += iSrc[i * devCh + ch];
                dst[dstBase + static_cast<std::size_t>(i)] =
                    static_cast<int16_t>(sum / devCh);
            }
        }
    }

    // ---- Linear resampler: accumulator_ (deviceSampleRate_) → resampled_ ----

    void resampleAccumulator() {
        if (accumulator_.empty()) return;
        const double ratio = static_cast<double>(cfg_.format.sampleRate)
                           / static_cast<double>(deviceSampleRate_);
        const std::size_t outLen =
            static_cast<std::size_t>(static_cast<double>(accumulator_.size()) * ratio);
        const std::size_t dstBase = resampled_.size();
        resampled_.resize(dstBase + outLen);

        for (std::size_t i = 0; i < outLen; ++i) {
            const double srcPos = static_cast<double>(i) / ratio;
            const std::size_t i0 = static_cast<std::size_t>(srcPos);
            const std::size_t i1 = std::min(i0 + 1, accumulator_.size() - 1);
            const float frac = static_cast<float>(srcPos - static_cast<double>(i0));
            const float s = static_cast<float>(accumulator_[i0]) * (1.0f - frac)
                          + static_cast<float>(accumulator_[i1]) * frac;
            resampled_[dstBase + i] = static_cast<int16_t>(
                std::max(-32768.0f, std::min(32767.0f, s)));
        }
        accumulator_.clear();
    }

    // ---- DC removal (high-pass via running mean subtraction) -----------------

    void applyDcRemoval() {
        std::vector<int16_t>& buf = (deviceSampleRate_ != cfg_.format.sampleRate)
                                    ? resampled_ : accumulator_;
        for (auto& s : buf) {
            dcMean_ = dcMean_ * 0.999f + static_cast<float>(s) * 0.001f;
            const float cleaned = static_cast<float>(s) - dcMean_;
            s = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, cleaned)));
        }
    }

    // ---- Utility -------------------------------------------------------------

    static uint64_t nowNs() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // ---- Config & state ------------------------------------------------------

    WasapiMicConfig    cfg_;
    AudioFrameQueue*   out_ = nullptr;

    SharedBufferPool<AudioFrame> pool_;
    uint64_t           seq_ = 0;

    std::atomic<bool>  initialized_{false};

    // COM objects — owned by the runLoop thread
    IMMDeviceEnumerator*   enumerator_    = nullptr;
    IMMDevice*             device_        = nullptr;
    IAudioClient*          audioClient_   = nullptr;
    IAudioCaptureClient*   captureClient_ = nullptr;
    WAVEFORMATEX*          pwfx_          = nullptr;

    // Event handles
    HANDLE captureEvent_ = nullptr;
    HANDLE wakeEvent_    = nullptr;

    // Device format properties (determined after GetMixFormat)
    int  deviceSampleRate_ = 48000;
    int  deviceChannels_   = 2;
    bool deviceIsFloat_    = true;

    // Sample accumulation buffers
    std::vector<int16_t> accumulator_; // device-rate mono Int16
    std::vector<int16_t> resampled_;   // target-rate mono Int16, sliced into frames

    // DC removal state
    float dcMean_ = 0.0f;
};

} // namespace voice_runtime
