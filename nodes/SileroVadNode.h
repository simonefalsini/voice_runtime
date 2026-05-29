#pragma once

// ---------------------------------------------------------------------------
// SileroVadNode — Silero VAD v6 via GGML for voice_runtime
//
//   Fully ports the DS4 SileroVAD_GGML inference engine (STFT, Conv1D, LSTM,
//   sigmoid) into the ActiveNodeBase + IVadNode pipeline architecture.
//
//   GGML availability is detected at compile time.  When absent, a simple
//   RMS energy-based fallback keeps the node functional for CI and simulation.
//
//   Frame accumulation:
//     Silero requires 512 samples @ 16 kHz (32 ms).  The pipeline produces
//     160-sample frames (10 ms).  The node accumulates 4 frames (640 samples)
//     and feeds the first 512 to the model.  Pending frames are forwarded or
//     dropped based on the current VAD decision.
//
//     Accumulation only occurs when TTS is **inactive** (pure listening mode).
//     When TTS is active, frames pass through with no gating.
//
//   VAD state machine:
//     SpeechStart  → probability >= thresholdStart AND previous = silence
//     SpeechEnd    → silence lasting hangoverChunks × 10 ms
//     silenceChunks reset on each speech detection.
//
//   Noise level presets (from DS4):
//     NONE   → start 0.20, stop 0.15
//     LOW    → start 0.40, stop 0.25
//     MEDIUM → start 0.60, stop 0.45
//     HIGH   → start 0.80, stop 0.65
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "voice_runtime/Interfaces.h"
#include "voice_runtime/SharedBufferPool.h"

// ---------------------------------------------------------------------------
// GGML detection
// ---------------------------------------------------------------------------

#if __has_include(<ggml.h>)
#  include <ggml.h>
#  include <ggml-alloc.h>
#  include <ggml-backend.h>
#  include <ggml-cpu.h>
#  define VOICE_RUNTIME_HAS_GGML 1
#else
#  define VOICE_RUNTIME_HAS_GGML 0
#endif

namespace voice_runtime {

// ---------------------------------------------------------------------------
// SileroVadConfig
// ---------------------------------------------------------------------------

struct SileroVadConfig {
    std::string modelPath = "models/vad/ggml-silero-v6.2.0.bin";
    AudioFormat format    = {16000, 1, 10, SampleFormat::Int16};
    std::size_t poolSize  = 128;

    float thresholdStart  = 0.6f;
    float thresholdStop   = 0.45f;
    int   hangoverChunks  = 25;   // 25 × 10 ms = 250 ms

    // Noise level presets — convenience matching DS4 SileroVAD_GGML::NoiseLevel
    enum class NoiseLevel { NONE, LOW, MEDIUM, HIGH };

    static void applyNoiseLevel(NoiseLevel level,
                                float& outStart, float& outStop) {
        switch (level) {
            case NoiseLevel::NONE:   outStart = 0.2f; outStop = 0.15f; break;
            case NoiseLevel::LOW:    outStart = 0.4f; outStop = 0.25f; break;
            case NoiseLevel::MEDIUM: outStart = 0.6f; outStop = 0.45f; break;
            case NoiseLevel::HIGH:   outStart = 0.8f; outStop = 0.65f; break;
        }
    }
};

// ===========================================================================
// GGML model internals — only compiled when GGML is available
// ===========================================================================

#if VOICE_RUNTIME_HAS_GGML

namespace detail_silero {

// ---- binary helpers (identical to DS4) ------------------------------------

inline void readSafeString(std::ifstream& fin, std::string& str, std::size_t length) {
    std::vector<char> buffer(length + 1, 0);
    fin.read(buffer.data(), static_cast<std::streamsize>(length));
    str.assign(buffer.data(), length);
}

template <typename T>
inline void readSafeVal(std::ifstream& fin, T& val) {
    fin.read(reinterpret_cast<char*>(&val), sizeof(T));
}

// ---- model structures -----------------------------------------------------

struct VadHparams {
    int32_t   n_encoder_layers  = 0;
    int32_t*  encoder_in_channels  = nullptr;
    int32_t*  encoder_out_channels = nullptr;
    int32_t*  kernel_sizes         = nullptr;
    int32_t   lstm_input_size  = 0;
    int32_t   lstm_hidden_size = 0;
    int32_t   final_conv_in    = 0;
    int32_t   final_conv_out   = 0;
};

struct VadModel {
    std::string type;
    std::string version;
    VadHparams  hparams{};

    ggml_tensor* stft_forward_basis = nullptr;

    ggml_tensor* encoder_0_weight = nullptr;
    ggml_tensor* encoder_0_bias   = nullptr;
    ggml_tensor* encoder_1_weight = nullptr;
    ggml_tensor* encoder_1_bias   = nullptr;
    ggml_tensor* encoder_2_weight = nullptr;
    ggml_tensor* encoder_2_bias   = nullptr;
    ggml_tensor* encoder_3_weight = nullptr;
    ggml_tensor* encoder_3_bias   = nullptr;

    ggml_tensor* lstm_ih_weight = nullptr;
    ggml_tensor* lstm_ih_bias   = nullptr;
    ggml_tensor* lstm_hh_weight = nullptr;
    ggml_tensor* lstm_hh_bias   = nullptr;

    ggml_tensor* final_conv_weight = nullptr;
    ggml_tensor* final_conv_bias   = nullptr;

    std::vector<ggml_context*>          ctxs;
    std::vector<ggml_backend_buffer_t>  buffers;
    std::map<std::string, ggml_tensor*> tensors;
};

struct VadContext {
    int n_window  = 0;
    int n_context = 0;

    ggml_backend_t         backend = nullptr;
    ggml_backend_buffer_t  buffer  = nullptr;
    ggml_gallocr_t         allocr  = nullptr;

    VadModel       model;
    ggml_tensor*   h_state = nullptr;
    ggml_tensor*   c_state = nullptr;
};

// ---- tensor name map (DS4-compatible) -------------------------------------

enum VadTensor {
    VAD_TENSOR_STFT_BASIS,
    VAD_TENSOR_ENC_0_WEIGHT,
    VAD_TENSOR_ENC_0_BIAS,
    VAD_TENSOR_ENC_1_WEIGHT,
    VAD_TENSOR_ENC_1_BIAS,
    VAD_TENSOR_ENC_2_WEIGHT,
    VAD_TENSOR_ENC_2_BIAS,
    VAD_TENSOR_ENC_3_WEIGHT,
    VAD_TENSOR_ENC_3_BIAS,
    VAD_TENSOR_LSTM_WEIGHT_IH,
    VAD_TENSOR_LSTM_WEIGHT_HH,
    VAD_TENSOR_LSTM_BIAS_IH,
    VAD_TENSOR_LSTM_BIAS_HH,
    VAD_TENSOR_FINAL_CONV_WEIGHT,
    VAD_TENSOR_FINAL_CONV_BIAS
};

inline const std::map<VadTensor, const char*>& vadTensorNames() {
    static const std::map<VadTensor, const char*> names = {
        {VAD_TENSOR_STFT_BASIS,        "_model.stft.forward_basis_buffer"},
        {VAD_TENSOR_ENC_0_WEIGHT,      "_model.encoder.0.reparam_conv.weight"},
        {VAD_TENSOR_ENC_0_BIAS,        "_model.encoder.0.reparam_conv.bias"},
        {VAD_TENSOR_ENC_1_WEIGHT,      "_model.encoder.1.reparam_conv.weight"},
        {VAD_TENSOR_ENC_1_BIAS,        "_model.encoder.1.reparam_conv.bias"},
        {VAD_TENSOR_ENC_2_WEIGHT,      "_model.encoder.2.reparam_conv.weight"},
        {VAD_TENSOR_ENC_2_BIAS,        "_model.encoder.2.reparam_conv.bias"},
        {VAD_TENSOR_ENC_3_WEIGHT,      "_model.encoder.3.reparam_conv.weight"},
        {VAD_TENSOR_ENC_3_BIAS,        "_model.encoder.3.reparam_conv.bias"},
        {VAD_TENSOR_LSTM_WEIGHT_IH,    "_model.decoder.rnn.weight_ih"},
        {VAD_TENSOR_LSTM_WEIGHT_HH,    "_model.decoder.rnn.weight_hh"},
        {VAD_TENSOR_LSTM_BIAS_IH,      "_model.decoder.rnn.bias_ih"},
        {VAD_TENSOR_LSTM_BIAS_HH,      "_model.decoder.rnn.bias_hh"},
        {VAD_TENSOR_FINAL_CONV_WEIGHT, "_model.decoder.decoder.2.weight"},
        {VAD_TENSOR_FINAL_CONV_BIAS,   "_model.decoder.decoder.2.bias"}
    };
    return names;
}

// ---- graph building (ported verbatim from DS4) ----------------------------

inline ggml_tensor* buildStftLayer(ggml_context* ctx0, const VadModel& model,
                                   ggml_tensor* cur) {
    ggml_tensor* padded = ggml_pad_reflect_1d(ctx0, cur, 64, 64);
    ggml_tensor* stft   = ggml_conv_1d(ctx0, model.stft_forward_basis, padded,
                                       model.hparams.lstm_input_size, 0, 1);
    const int cutoff       = model.stft_forward_basis->ne[2] / 2;
    ggml_tensor* real_part = ggml_view_2d(ctx0, stft, 4, cutoff,
                                          stft->nb[1], 0);
    ggml_tensor* img_part  = ggml_view_2d(ctx0, stft, 4, cutoff,
                                          stft->nb[1],
                                          cutoff * stft->nb[1]);
    ggml_tensor* real_sq   = ggml_mul(ctx0, real_part, real_part);
    ggml_tensor* img_sq    = ggml_mul(ctx0, img_part,  img_part);
    ggml_tensor* sum_sq    = ggml_add(ctx0, real_sq,   img_sq);
    return ggml_sqrt(ctx0, sum_sq);
}

inline ggml_tensor* buildEncoderLayer(ggml_context* ctx0,
                                      const VadModel& model,
                                      ggml_tensor* cur) {
    cur = ggml_conv_1d(ctx0, model.encoder_0_weight, cur, 1, 1, 1);
    cur = ggml_add(ctx0, cur,
                   ggml_reshape_3d(ctx0, model.encoder_0_bias, 1, 128, 1));
    cur = ggml_relu(ctx0, cur);

    cur = ggml_conv_1d(ctx0, model.encoder_1_weight, cur, 2, 1, 1);
    cur = ggml_add(ctx0, cur,
                   ggml_reshape_3d(ctx0, model.encoder_1_bias, 1, 64, 1));
    cur = ggml_relu(ctx0, cur);

    cur = ggml_conv_1d(ctx0, model.encoder_2_weight, cur, 2, 1, 1);
    cur = ggml_add(ctx0, cur,
                   ggml_reshape_3d(ctx0, model.encoder_2_bias, 1, 64, 1));
    cur = ggml_relu(ctx0, cur);

    cur = ggml_conv_1d(ctx0, model.encoder_3_weight, cur, 1, 1, 1);
    cur = ggml_add(ctx0, cur,
                   ggml_reshape_3d(ctx0, model.encoder_3_bias, 1, 128, 1));
    cur = ggml_relu(ctx0, cur);

    return cur;
}

inline ggml_tensor* buildLstmLayer(ggml_context* ctx0, VadContext& vctx,
                                   ggml_tensor* cur, ggml_cgraph* gf) {
    const VadModel& model = vctx.model;
    const int hdim = model.hparams.lstm_hidden_size;

    ggml_tensor* x_t = ggml_transpose(ctx0, cur);

    ggml_tensor* inp_gate = ggml_mul_mat(ctx0, model.lstm_ih_weight, x_t);
    inp_gate = ggml_add(ctx0, inp_gate, model.lstm_ih_bias);

    ggml_tensor* hid_gate = ggml_mul_mat(ctx0, model.lstm_hh_weight, vctx.h_state);
    hid_gate = ggml_add(ctx0, hid_gate, model.lstm_hh_bias);

    ggml_tensor* out_gate = ggml_add(ctx0, inp_gate, hid_gate);
    const size_t hdim_size = ggml_row_size(out_gate->type, hdim);

    ggml_tensor* i_t = ggml_sigmoid(ctx0, ggml_view_1d(ctx0, out_gate, hdim, 0 * hdim_size));
    ggml_tensor* f_t = ggml_sigmoid(ctx0, ggml_view_1d(ctx0, out_gate, hdim, 1 * hdim_size));
    ggml_tensor* g_t = ggml_tanh(ctx0,    ggml_view_1d(ctx0, out_gate, hdim, 2 * hdim_size));
    ggml_tensor* o_t = ggml_sigmoid(ctx0, ggml_view_1d(ctx0, out_gate, hdim, 3 * hdim_size));

    ggml_tensor* c_out = ggml_add(ctx0,
        ggml_mul(ctx0, f_t, vctx.c_state),
        ggml_mul(ctx0, i_t, g_t));
    ggml_build_forward_expand(gf, ggml_cpy(ctx0, c_out, vctx.c_state));

    ggml_tensor* out = ggml_mul(ctx0, o_t, ggml_tanh(ctx0, c_out));
    ggml_build_forward_expand(gf, ggml_cpy(ctx0, out, vctx.h_state));

    return out;
}

inline ggml_cgraph* buildGraph(VadContext& vctx, ggml_context* ctx0) {
    const auto& model = vctx.model;

    ggml_cgraph* gf = ggml_new_graph(ctx0);

    ggml_tensor* frame = ggml_new_tensor_2d(ctx0, GGML_TYPE_F32,
                                            vctx.n_window, 1);
    ggml_set_name(frame, "frame");
    ggml_set_input(frame);

    ggml_tensor* cur = frame;
    cur = buildStftLayer(ctx0, model, cur);
    cur = buildEncoderLayer(ctx0, model, cur);
    cur = ggml_view_2d(ctx0, cur, 1, 128, cur->nb[1], 0);

    cur = buildLstmLayer(ctx0, vctx, cur, gf);
    cur = ggml_relu(ctx0, cur);
    cur = ggml_conv_1d(ctx0, model.final_conv_weight, cur, 1, 0, 1);
    cur = ggml_add(ctx0, cur, model.final_conv_bias);
    cur = ggml_sigmoid(ctx0, cur);
    ggml_set_name(cur, "prob");
    ggml_set_output(cur);

    ggml_build_forward_expand(gf, cur);
    ggml_gallocr_alloc_graph(vctx.allocr, gf);

    return gf;
}

// ---- model loading (ported from DS4 SileroVAD_GGML::init) -----------------

inline VadContext* loadModel(const std::string& modelPath) {
    std::ifstream fin(modelPath, std::ios::binary);
    if (!fin) {
        std::fprintf(stderr, "[SileroVadNode] Failed to open model: %s\n",
                     modelPath.c_str());
        return nullptr;
    }

    uint32_t magic = 0;
    readSafeVal(fin, magic);
    if (magic != 0x67676d6c) {  // 'ggml'
        std::fprintf(stderr, "[SileroVadNode] Invalid magic in %s\n",
                     modelPath.c_str());
        return nullptr;
    }

    auto* vctx  = new VadContext();
    auto& model  = vctx->model;
    auto& hparams = model.hparams;

    // Model type string
    int32_t str_len = 0;
    readSafeVal(fin, str_len);
    readSafeString(fin, model.type, static_cast<std::size_t>(str_len));

    // Version
    int32_t major = 0, minor = 0, patch = 0;
    readSafeVal(fin, major);
    readSafeVal(fin, minor);
    readSafeVal(fin, patch);
    model.version = std::to_string(major) + "." +
                    std::to_string(minor) + "." +
                    std::to_string(patch);

    readSafeVal(fin, vctx->n_window);
    readSafeVal(fin, vctx->n_context);

    // Encoder hparams
    readSafeVal(fin, hparams.n_encoder_layers);
    hparams.encoder_in_channels  = new int32_t[hparams.n_encoder_layers];
    hparams.encoder_out_channels = new int32_t[hparams.n_encoder_layers];
    hparams.kernel_sizes         = new int32_t[hparams.n_encoder_layers];

    for (int32_t i = 0; i < hparams.n_encoder_layers; ++i) {
        readSafeVal(fin, hparams.encoder_in_channels[i]);
        readSafeVal(fin, hparams.encoder_out_channels[i]);
        readSafeVal(fin, hparams.kernel_sizes[i]);
    }

    readSafeVal(fin, hparams.lstm_input_size);
    readSafeVal(fin, hparams.lstm_hidden_size);
    readSafeVal(fin, hparams.final_conv_in);
    readSafeVal(fin, hparams.final_conv_out);

    // Allocate tensor metadata context
    ggml_init_params params = { 1024 * 1024, nullptr, true };
    ggml_context* ctx = ggml_init(params);
    model.ctxs.push_back(ctx);

    const auto& names = vadTensorNames();

    auto createTensor = [&](VadTensor type, ggml_tensor* meta) -> ggml_tensor* {
        ggml_tensor* tensor = ggml_dup_tensor(ctx, meta);
        model.tensors[names.at(type)] = tensor;
        return tensor;
    };

    // STFT basis
    model.stft_forward_basis = createTensor(
        VAD_TENSOR_STFT_BASIS,
        ggml_new_tensor_3d(ctx, GGML_TYPE_F16, 256, 1, 258));

    // Encoder layers
    for (int i = 0; i < 4; ++i) {
        auto* w = createTensor(
            static_cast<VadTensor>(VAD_TENSOR_ENC_0_WEIGHT + 2 * i),
            ggml_new_tensor_3d(ctx, GGML_TYPE_F16,
                               hparams.kernel_sizes[i],
                               hparams.encoder_in_channels[i],
                               hparams.encoder_out_channels[i]));
        auto* b = createTensor(
            static_cast<VadTensor>(VAD_TENSOR_ENC_0_BIAS + 2 * i),
            ggml_new_tensor_1d(ctx, GGML_TYPE_F32,
                               hparams.encoder_out_channels[i]));

        switch (i) {
            case 0: model.encoder_0_weight = w; model.encoder_0_bias = b; break;
            case 1: model.encoder_1_weight = w; model.encoder_1_bias = b; break;
            case 2: model.encoder_2_weight = w; model.encoder_2_bias = b; break;
            case 3: model.encoder_3_weight = w; model.encoder_3_bias = b; break;
        }
    }

    // LSTM
    const int hstate_dim = hparams.lstm_hidden_size * 4;
    model.lstm_ih_weight = createTensor(VAD_TENSOR_LSTM_WEIGHT_IH,
        ggml_new_tensor_2d(ctx, GGML_TYPE_F32, hparams.lstm_hidden_size, hstate_dim));
    model.lstm_ih_bias   = createTensor(VAD_TENSOR_LSTM_BIAS_IH,
        ggml_new_tensor_1d(ctx, GGML_TYPE_F32, hstate_dim));
    model.lstm_hh_weight = createTensor(VAD_TENSOR_LSTM_WEIGHT_HH,
        ggml_new_tensor_2d(ctx, GGML_TYPE_F32, hparams.lstm_hidden_size, hstate_dim));
    model.lstm_hh_bias   = createTensor(VAD_TENSOR_LSTM_BIAS_HH,
        ggml_new_tensor_1d(ctx, GGML_TYPE_F32, hstate_dim));

    // Final conv
    model.final_conv_weight = createTensor(VAD_TENSOR_FINAL_CONV_WEIGHT,
        ggml_new_tensor_2d(ctx, GGML_TYPE_F16, hparams.final_conv_in, 1));
    model.final_conv_bias   = createTensor(VAD_TENSOR_FINAL_CONV_BIAS,
        ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 1));

    // Backend + buffer allocation
    // Always use CPU backend for VAD.  The Silero model is tiny (885KB)
    // and runs in microseconds on CPU.  Using Metal/GPU here would cause
    // a data-race crash when the STT encoder (also Metal) runs concurrently
    // in another thread.
    vctx->backend = ggml_backend_cpu_init();
    if (!vctx->backend) {
        std::fprintf(stderr, "[SileroVadNode] Failed to init CPU backend\n");
        delete vctx;
        return nullptr;
    }
    ggml_backend_buffer_t buf = ggml_backend_alloc_ctx_tensors(ctx, vctx->backend);
    model.buffers.push_back(buf);

    // Load tensor data from file
    while (!fin.eof()) {
        int32_t n_dims = 0, length = 0, ttype = 0;
        fin.read(reinterpret_cast<char*>(&n_dims), sizeof(int32_t));
        if (fin.eof()) break;
        readSafeVal(fin, length);
        readSafeVal(fin, ttype);

        int32_t ne[4] = {1, 1, 1, 1};
        for (int i = 0; i < n_dims; ++i) {
            readSafeVal(fin, ne[i]);
        }

        std::string tname;
        readSafeString(fin, tname, static_cast<std::size_t>(length));

        if (model.tensors.find(tname) == model.tensors.end()) {
            std::fprintf(stderr, "[SileroVadNode] Unknown tensor: %s\n",
                         tname.c_str());
            delete vctx;
            return nullptr;
        }
        auto* tensor = model.tensors[tname];
        fin.read(reinterpret_cast<char*>(tensor->data), ggml_nbytes(tensor));
    }

    // LSTM hidden / cell state tensors
    ggml_init_params stateParams = {
        ggml_tensor_overhead() * 4, nullptr, true
    };
    ggml_context* ctx_state = ggml_init(stateParams);
    model.ctxs.push_back(ctx_state);

    vctx->h_state = ggml_new_tensor_1d(ctx_state, GGML_TYPE_F32,
                                        hparams.lstm_hidden_size);
    vctx->c_state = ggml_new_tensor_1d(ctx_state, GGML_TYPE_F32,
                                        hparams.lstm_hidden_size);
    ggml_set_name(vctx->h_state, "h_state");
    ggml_set_name(vctx->c_state, "c_state");

    vctx->buffer = ggml_backend_alloc_ctx_tensors(ctx_state, vctx->backend);
    vctx->allocr = ggml_gallocr_new(
        ggml_backend_get_default_buffer_type(vctx->backend));

    // Warm-up allocator with a dummy graph
    ggml_init_params warmupParams = { 512 * 1024, nullptr, true };
    ggml_context* ctx_warmup = ggml_init(warmupParams);
    ggml_cgraph* gf_warmup = buildGraph(*vctx, ctx_warmup);
    ggml_gallocr_reserve(vctx->allocr, gf_warmup);
    ggml_free(ctx_warmup);

    // Zero-initialize LSTM state
    if (vctx->buffer) {
        ggml_backend_buffer_clear(vctx->buffer, 0);
    }

    std::printf("[SileroVadNode] Loaded model: %s v%s  window=%d  context=%d\n",
                model.type.c_str(), model.version.c_str(),
                vctx->n_window, vctx->n_context);
    std::fflush(stdout);

    return vctx;
}

// ---- inference (ported from DS4 SileroVAD_GGML::process) ------------------

inline float runInference(VadContext& vctx, const float* samples, int count) {
    ggml_init_params params = { 512 * 1024, nullptr, true };
    ggml_context* ctx0 = ggml_init(params);

    ggml_cgraph* gf = buildGraph(vctx, ctx0);
    ggml_gallocr_alloc_graph(vctx.allocr, gf);

    ggml_tensor* frame = ggml_graph_get_tensor(gf, "frame");
    ggml_tensor* prob  = ggml_graph_get_tensor(gf, "prob");

    if (count > vctx.n_window) count = vctx.n_window;

    ggml_backend_tensor_set(frame, samples, 0,
                            static_cast<std::size_t>(count) * sizeof(float));
    if (count < vctx.n_window) {
        std::vector<float> zeros(static_cast<std::size_t>(vctx.n_window - count), 0.0f);
        ggml_backend_tensor_set(frame, zeros.data(),
                                static_cast<std::size_t>(count) * sizeof(float),
                                zeros.size() * sizeof(float));
    }

    ggml_backend_graph_compute(vctx.backend, gf);

    float p = 0.0f;
    ggml_backend_tensor_get(prob, &p, 0, sizeof(float));

    ggml_free(ctx0);
    return p;
}

inline void resetState(VadContext& vctx) {
    if (vctx.buffer) {
        ggml_backend_buffer_clear(vctx.buffer, 0);
    }
}

inline void freeContext(VadContext* vctx) {
    if (!vctx) return;
    for (auto* ctx : vctx->model.ctxs)   ggml_free(ctx);
    for (auto  buf : vctx->model.buffers) ggml_backend_buffer_free(buf);

    delete[] vctx->model.hparams.encoder_in_channels;
    delete[] vctx->model.hparams.encoder_out_channels;
    delete[] vctx->model.hparams.kernel_sizes;

    if (vctx->allocr)  ggml_gallocr_free(vctx->allocr);
    if (vctx->buffer)  ggml_backend_buffer_free(vctx->buffer);
    if (vctx->backend) ggml_backend_free(vctx->backend);

    delete vctx;
}

} // namespace detail_silero

#endif // VOICE_RUNTIME_HAS_GGML

// ===========================================================================
// SileroVadNode
// ===========================================================================

class SileroVadNode final
    : public ActiveNodeBase
    , public IVadNode
{
public:
    explicit SileroVadNode(SileroVadConfig config)
        : cfg_(std::move(config))
        , pool_(cfg_.poolSize)
    {}

    ~SileroVadNode() override {
#if VOICE_RUNTIME_HAS_GGML
        if (vctx_) detail_silero::freeContext(vctx_);
#endif
    }

    const char* name() const override { return "SileroVAD"; }

    bool initialize() override {
        // Validate format: require 16 kHz mono 10 ms Int16
        if (cfg_.format.sampleRate != 16000 ||
            cfg_.format.channels   != 1     ||
            cfg_.format.frameMs    != 10    ||
            cfg_.format.sampleFormat != SampleFormat::Int16) {
            std::fprintf(stderr,
                "[SileroVadNode] Unsupported format. "
                "Need 16kHz/mono/10ms/Int16.\n");
            return false;
        }

        samplesPerFrame_ = cfg_.format.totalSamplesPerFrame(); // 160

        // Silero expects 512 samples.  We accumulate 4 frames → 640 samples,
        // then take the first 512.
        static constexpr int kSileroWindow = 512;
        framesToAccumulate_ = (kSileroWindow + samplesPerFrame_ - 1) / samplesPerFrame_;
        // For 160-sample frames: ceil(512/160) = 4 → 640 total, take 512
        accumBuf_.resize(static_cast<std::size_t>(framesToAccumulate_ * samplesPerFrame_), 0.0f);

#if VOICE_RUNTIME_HAS_GGML
        vctx_ = detail_silero::loadModel(cfg_.modelPath);
        if (!vctx_) {
            std::fprintf(stderr,
                "[SileroVadNode] GGML model load failed — using energy fallback\n");
            useEnergyFallback_ = true;
        } else {
            sileroWindow_ = vctx_->n_window;   // should be 512
            useEnergyFallback_ = false;
        }
        pendingFrames_.reserve(static_cast<std::size_t>(framesToAccumulate_));
#else
        std::printf("[SileroVadNode] GGML not available — using energy fallback\n");
        std::fflush(stdout);
        useEnergyFallback_ = true;
        pendingFrames_.reserve(static_cast<std::size_t>(framesToAccumulate_));
#endif
        initialized_ = true;
        return true;
    }

    // IVadNode ---------------------------------------------------------------

    void setInputQueue(AudioFrameQueue* in)        override { in_  = in; }
    void setOutputQueue(AudioFrameQueue* out)       override { out_ = out; }
    void setEventQueue(VadEventQueue* ev)           override { evq_ = ev; }

    void setSpeechThreshold(float threshold) override {
        cfg_.thresholdStart = std::max(0.0f, std::min(1.0f, threshold));
    }

    void setTtsStateSignal(const TtsStateSignal* signal) override {
        ttsState_ = signal;
    }

    // Additional API ---------------------------------------------------------

    void setNoiseLevel(SileroVadConfig::NoiseLevel level) {
        SileroVadConfig::applyNoiseLevel(level,
                                         cfg_.thresholdStart,
                                         cfg_.thresholdStop);
        std::printf("[SileroVadNode] Noise level set: start=%.2f stop=%.2f\n",
                    cfg_.thresholdStart, cfg_.thresholdStop);
        std::fflush(stdout);
    }

    void resetModelState() {
#if VOICE_RUNTIME_HAS_GGML
        if (vctx_) detail_silero::resetState(*vctx_);
#endif
        accumCount_ = 0;
    }

    bool isSpeaking() const {
        return speaking_.load(std::memory_order_acquire);
    }

    float lastProbability() const {
        return lastProbability_.load(std::memory_order_acquire);
    }

protected:
    void wake() override { pool_.stop(); }

    void runLoop() override {
        if (!initialized_) return;

        while (running()) {
            AudioFrameHandle frame;
            if (!in_ || !in_->pop(frame)) break;

            // TTS active → pass-through (no gating, no VAD events)
            if (ttsState_ && ttsState_->isActive()) {
                // Flush any partially-accumulated frames so they don't
                // leak into the next VAD batch after TTS deactivates.
                for (auto& pf : pendingFrames_) {
                    if (out_) out_->push(std::move(pf));
                }
                pendingFrames_.clear();
                accumCount_ = 0;
                if (out_) out_->push(std::move(frame));
                continue;
            }

            // Accumulate frames for Silero inference
            pendingFrames_.push_back(std::move(frame));
            ++accumCount_;

            if (accumCount_ < framesToAccumulate_) {
                continue;   // need more frames
            }

            // We have enough frames — run inference
            const float prob = runVadOnAccumulated();
            lastProbability_.store(prob, std::memory_order_release);
            const bool speech = evaluateVadDecision(prob);
            updateVadState(speech, prob, pendingFrames_.back()->timestampNs);

            // Forward or discard pending frames
            forwardPendingFrames(speech);

            // Reset accumulator
            accumCount_ = 0;
        }

        // On exit, forward any remaining pending frames to avoid losing audio
        for (auto& pf : pendingFrames_) {
            if (out_) out_->push(std::move(pf));
        }
        pendingFrames_.clear();
    }

private:
    // -------------------------------------------------------------------
    // Run Silero (or fallback) on the accumulated samples
    // -------------------------------------------------------------------

    float runVadOnAccumulated() {
        // Convert accumulated Int16 frames to float and collect in accumBuf_
        std::size_t offset = 0;
        for (const auto& pf : pendingFrames_) {
            const std::size_t n = static_cast<std::size_t>(
                pf->format.totalSamplesPerFrame());
            for (std::size_t i = 0; i < n && offset < accumBuf_.size(); ++i) {
                accumBuf_[offset++] =
                    static_cast<float>(pf->pcm16[i]) / 32768.0f;
            }
        }

        if (useEnergyFallback_) {
            return energyVadProbability(accumBuf_.data(),
                                       static_cast<int>(offset));
        }

#if VOICE_RUNTIME_HAS_GGML
        if (vctx_) {
            const int window = sileroWindow_;
            const int count  = std::min(static_cast<int>(offset), window);
            std::unique_lock<std::mutex> lock(g_ggml_mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                // If STT is transcribing (holding the lock), skip VAD inference
                // to prevent data-race crashes and avoid blocking the VAD thread.
                return lastProbability_.load(std::memory_order_acquire);
            }
            return detail_silero::runInference(*vctx_, accumBuf_.data(), count);
        }
#endif
        return 0.0f;
    }

    // -------------------------------------------------------------------
    // Evaluate speech/silence based on probability + hysteresis thresholds
    // -------------------------------------------------------------------

    bool evaluateVadDecision(float probability) const {
        const bool currentlySpeaking = speaking_.load(std::memory_order_acquire);
        if (currentlySpeaking) {
            // Stay in speech until probability drops below stop threshold
            return probability >= cfg_.thresholdStop;
        } else {
            // Enter speech only when probability exceeds start threshold
            return probability >= cfg_.thresholdStart;
        }
    }

    // -------------------------------------------------------------------
    // VAD state machine with hangover
    // -------------------------------------------------------------------

    void updateVadState(bool speechNow, float prob, uint64_t timestampNs) {
        if (speechNow) {
            silenceChunks_ = 0;
            if (!speaking_.exchange(true, std::memory_order_acq_rel)) {
                emitVadEvent(VadEvent::Type::SpeechStart, timestampNs, prob);
            }
            return;
        }

        // Silence detected
        if (!speaking_.load(std::memory_order_acquire)) return; // already silent

        ++silenceChunks_;
        if (silenceChunks_ >= cfg_.hangoverChunks) {
            speaking_.store(false, std::memory_order_release);
            silenceChunks_ = 0;
            emitVadEvent(VadEvent::Type::SpeechEnd, timestampNs, prob);
        }
    }

    // -------------------------------------------------------------------
    // Forward or discard pending frames
    // -------------------------------------------------------------------

    void forwardPendingFrames(bool speech) {
        if (!out_) {
            pendingFrames_.clear();
            return;
        }

        const bool wasSpeaking = speaking_.load(std::memory_order_acquire);

        if (speech || wasSpeaking) {
            // Forward all accumulated frames to output
            for (auto& pf : pendingFrames_) {
                out_->push(std::move(pf));
            }
        }
        // else: frames are discarded (silence gating)

        pendingFrames_.clear();
    }

    // -------------------------------------------------------------------
    // Energy-based fallback VAD
    // -------------------------------------------------------------------

    float energyVadProbability(const float* data, int samples) const {
        if (!data || samples <= 0) return 0.0f;
        double acc = 0.0;
        for (int i = 0; i < samples; ++i) {
            acc += static_cast<double>(data[i]) * static_cast<double>(data[i]);
        }
        const double rms = std::sqrt(acc / static_cast<double>(samples));
        // Map RMS (0..1) to a pseudo-probability.
        // RMS of ~0.03 is a reasonable speech threshold for normalised audio.
        // Scale so that rms=0.03 → p≈0.6 (MEDIUM start threshold).
        const double p = std::min(1.0, rms / 0.05);
        return static_cast<float>(p);
    }

    // -------------------------------------------------------------------
    // Event emission
    // -------------------------------------------------------------------

    void emitVadEvent(VadEvent::Type type, uint64_t timestampNs,
                      float confidence) {
        const char* label = (type == VadEvent::Type::SpeechStart)
                            ? "SpeechStart" : "SpeechEnd";
        std::printf("[SileroVAD] %s  conf=%.2f\n", label, confidence);
        std::fflush(stdout);

        if (!evq_) return;
        VadEvent ev;
        ev.type        = type;
        ev.timestampNs = timestampNs;
        ev.confidence  = confidence;
        evq_->push(ev);
    }

    // -------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------

    SileroVadConfig cfg_;

    AudioFrameQueue*      in_       = nullptr;
    AudioFrameQueue*      out_      = nullptr;
    VadEventQueue*        evq_      = nullptr;
    const TtsStateSignal* ttsState_ = nullptr;

    SharedBufferPool<AudioFrame> pool_;   // for potential future use
    bool initialized_ = false;

    // Frame accumulation
    int samplesPerFrame_      = 160;
    int framesToAccumulate_   = 4;      // 4 × 160 = 640 ≥ 512
    int accumCount_           = 0;
    std::vector<float> accumBuf_;       // float conversion buffer
    std::vector<AudioFrameHandle> pendingFrames_;

    // VAD state
    std::atomic<bool> speaking_{false};
    int silenceChunks_ = 0;

    // GGML engine
    bool useEnergyFallback_ = true;
    std::atomic<float> lastProbability_{0.0f};

#if VOICE_RUNTIME_HAS_GGML
    detail_silero::VadContext* vctx_ = nullptr;
    int sileroWindow_ = 512;
#endif
};

} // namespace voice_runtime
