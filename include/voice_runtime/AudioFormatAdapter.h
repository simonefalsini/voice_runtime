#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Operazioni di conversione — tutte stateless, nessuna allocazione esterna
// ---------------------------------------------------------------------------

namespace detail {

// Resampling lineare — sufficiente per test, da sostituire con r8brain in produzione
inline void resampleLinear(const int16_t* src, int srcLen,
                           int16_t* dst, int dstLen) {
    if (srcLen == dstLen) {
        std::memcpy(dst, src, static_cast<std::size_t>(srcLen) * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < dstLen; ++i) {
        const float pos = static_cast<float>(i) * (srcLen - 1) / (dstLen - 1);
        const int   lo  = static_cast<int>(pos);
        const int   hi  = std::min(lo + 1, srcLen - 1);
        const float frac = pos - lo;
        dst[i] = static_cast<int16_t>(
            src[lo] * (1.f - frac) + src[hi] * frac);
    }
}

// Stereo→mono downmix Int16
inline void stereoToMonoI16(const int16_t* src, int16_t* dst, int frames) {
    for (int i = 0; i < frames; ++i)
        dst[i] = static_cast<int16_t>((src[i * 2] + src[i * 2 + 1]) / 2);
}

// Float32→Int16
inline void f32ToI16(const float* src, int16_t* dst, int n) {
    for (int i = 0; i < n; ++i) {
        const float clamped = std::max(-1.f, std::min(1.f, src[i]));
        dst[i] = static_cast<int16_t>(clamped * 32767.f);
    }
}

// Int16→Float32
inline void i16ToF32(const int16_t* src, float* dst, int n) {
    for (int i = 0; i < n; ++i)
        dst[i] = src[i] / 32768.f;
}

} // namespace detail

// ---------------------------------------------------------------------------
// AudioFormatAdapterNode
//   Converte formato, sample rate e layout dei canali.
//   Se source == target opera in pass-through zero-copy.
// ---------------------------------------------------------------------------

class AudioFormatAdapterNode final
    : public ActiveNodeBase
    , public IAudioFormatAdapterNode
{
public:
    AudioFormatAdapterNode(const char*   nodeName,
                           AudioFormat   sourceFormat,
                           AudioFormat   targetFormat,
                           std::size_t   poolSize)
        : nodeName_(nodeName)
        , sourceFormat_(sourceFormat)
        , targetFormat_(targetFormat)
        , passThrough_(sourceFormat == targetFormat)
        , pool_(poolSize)
    {}

    const char* name()        const override { return nodeName_; }
    bool        initialize()        override { return true; }
    bool        isPassThrough() const override { return passThrough_; }

    void setInputQueue(AudioFrameQueue* in)   override { in_  = in; }
    void setOutputQueue(AudioFrameQueue* out) override { out_ = out; }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;

            if (passThrough_) {
                // Zero-copy: passa lo stesso handle
                if (out_) out_->push(std::move(frame));
                continue;
            }

            auto adapted = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
            if (!adapted) continue; // pool esaurito temporaneamente

            adapted->format     = targetFormat_;
            adapted->sequence   = frame->sequence;
            adapted->timestampNs= frame->timestampNs;
            adapted->resizeForFormat();

            convertFrame(*frame, *adapted);

            if (out_) out_->push(std::move(adapted));
        }
    }

private:
    void convertFrame(const AudioFrame& src, AudioFrame& dst) {
        // 1. Sorgente → Int16 mono (buffer intermedio sul thread stack)
        const int srcTotalSamples = src.format.totalSamplesPerFrame();
        const int srcFrames       = src.format.samplesPerChannelPerFrame();

        // Temporaneo monoI16 sorgente
        std::vector<int16_t> monoSrc(static_cast<std::size_t>(srcFrames));

        if (src.format.sampleFormat == SampleFormat::Float32) {
            // Float32 → Int16
            std::vector<int16_t> tempI16(static_cast<std::size_t>(srcTotalSamples));
            detail::f32ToI16(src.pcmF32.data(), tempI16.data(), srcTotalSamples);

            if (src.format.channels == 2)
                detail::stereoToMonoI16(tempI16.data(), monoSrc.data(), srcFrames);
            else
                monoSrc = tempI16;
        } else {
            if (src.format.channels == 2)
                detail::stereoToMonoI16(src.pcm16.data(), monoSrc.data(), srcFrames);
            else
                monoSrc = src.pcm16;
        }

        // 2. Resample
        const int dstFrames = dst.format.samplesPerChannelPerFrame();
        std::vector<int16_t> resampled(static_cast<std::size_t>(dstFrames));
        detail::resampleLinear(monoSrc.data(), srcFrames, resampled.data(), dstFrames);

        // 3. Destinazione formato finale
        if (dst.format.sampleFormat == SampleFormat::Int16) {
            dst.pcm16 = std::move(resampled);
        } else {
            dst.pcmF32.resize(static_cast<std::size_t>(dstFrames));
            detail::i16ToF32(resampled.data(), dst.pcmF32.data(), dstFrames);
        }
    }

    const char*      nodeName_;
    AudioFormat      sourceFormat_;
    AudioFormat      targetFormat_;
    bool             passThrough_;
    AudioFrameQueue* in_  = nullptr;
    AudioFrameQueue* out_ = nullptr;
    SharedBufferPool<AudioFrame> pool_;
};

} // namespace voice_runtime
