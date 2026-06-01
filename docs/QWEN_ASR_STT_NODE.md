# Qwen-ASR (Antirez) Speech-to-Text Node

This document describes the implementation of `AntirezSttNode`, a Speech-to-Text (STT) active processing node based on the dependency-free, pure C inference engine [`antirez/qwen-asr`](https://github.com/antirez/qwen-asr.git).

It can run on **macOS**, **iOS**, and **Linux**, utilizing BLAS acceleration via the Accelerate framework (Apple platforms) or OpenBLAS (Linux).

---

## 1. Node Class Interface

The `AntirezSttNode` class is declared in `nodes/AntirezSttNode.h`:

```cpp
class AntirezSttNode final
    : public ActiveNodeBase
    , public ISpeechToTextNode
```

It implements:
*   `ActiveNodeBase`: runs the processing loop on a dedicated thread.
*   `ISpeechToTextNode`: standard STT node interface to set input/output queues.

### Configuration (`AntirezSttConfig`)

```cpp
struct AntirezSttConfig {
    std::string modelPath; // Path to the directory containing model files

    /// Timeout in ms: if no frame arrives for this long, trigger transcription
    int transcriptionTimeoutMs = 500;

    /// Minimum samples before transcription is attempted (1s = 16000 samples)
    int minSpeechSamples = 16000;

    /// Maximum samples before forced transcription (30s @ 16kHz)
    int maxSpeechSamples = 480000;
};
```

---

## 2. Compilation and Build Instructions

The engine is compiled as a static library `libqwen_asr.a` and linked to the main runtime.

### 2.1 Download Dependency
Run the dependency downloader to fetch/update all cloned dependencies:
```bash
python3 deps/download_deps.py
```

### 2.2 Download Models
To download the `Qwen3-ASR-0.6B` safetensors model:
```bash
python3 deps/download_models.py --target qwen-asr-0.6b
```
The model files (`config.json`, `generation_config.json`, `vocab.json`, `merges.txt`, `model.safetensors`) will be saved under `models/stt/qwen-asr-0.6b/`.

### 2.3 Compile Dependencies
To compile dependencies for macOS/OSX (adds `qwen-asr` compile targets automatically):
```bash
python3 deps/build_deps.py --platform osx
```

To compile dependencies for Linux:
```bash
python3 deps/build_deps.py --platform linux
```

### 2.4 Build voice_runtime
Configure and build the project using CMake:
```bash
cmake -S . -B build -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON
cmake --build build --parallel
```

---

## 3. Usage in AEC Transcription Test

Under macOS and Linux, you can select the STT engine via the `--stt` CLI option:

```bash
build/test_aec_transcription \
  tests/data/user_reading.txt \
  tests/data/tts_playback.txt \
  60 \
  --stt qwen-asr
```

*   `--stt qwen-asr`: uses the `AntirezSttNode` engine (loads safetensors weights).
*   `--stt qwen3-asr` (default): uses the `Qwen3SttNode` engine (loads GGUF weights via GGML).

---

## 4. Performance & Synchronization

Unlike `Qwen3SttNode` (which uses GGML and must coordinate with TTS via `g_ggml_mutex`), `AntirezSttNode` runs a lightweight, independent pure C engine. It does not require `g_ggml_mutex` synchronization, permitting concurrent STT transcription and TTS audio synthesis without thread contention or blocking.
