# Dependency Management & Build System

This directory handles downloading, patching, and compiling the static libraries and headers for the application's dependencies: WebRTC APM, Abseil, GGML, espeak-ng, kokoro.cpp, qwen3-asr.cpp, cpp-httplib, nlohmann/json, and OpenSSL.

## Contents

- [download_deps.py](file:///d:/befree/voice_runtime/deps/download_deps.py): Python script that downloads/clones all dependency repositories (including cpp-httplib and nlohmann/json), aligns checkout commits, downloads required third-party files (`rnnoise`/`pffft`), auto-applies patches, and downloads/installs ONNX Runtime 1.24.1 and OpenSSL 3.0.17 (for Windows NuGet target) from NuGet.
- [generate_patches.py](file:///d:/befree/voice_runtime/deps/generate_patches.py): Script to scan local changes in the dependencies, create version-indexed git diffs and metadata (`metadata.json`, `combined.patch`), and organize them under the `patches/` folder.
- [build_deps.py](file:///d:/befree/voice_runtime/deps/build_deps.py): Configures and compiles all dependencies for all target platforms (Windows, Linux, macOS, iOS, Android) in both **Debug** and **Release** configurations. It also copies header files and generates CMake Find modules for all packages.
- [CMakeLists.txt](file:///d:/befree/voice_runtime/deps/CMakeLists.txt): CMake build script for WebRTC APM.
- [patches/](file:///d:/befree/voice_runtime/deps/patches/): Directory containing patch files and version metadata.
- [.gitignore](file:///d:/befree/voice_runtime/deps/.gitignore): Excludes cloned vendor sources and local build/intermediate files from Git.

---

## Workflow Instructions

### 1. Download & Clone Dependencies
To download all required dependencies, install ONNX Runtime 1.24.1, and apply patches:
```bash
python download_deps.py
```
This script will:
- Clone/update all dependency repositories (WebRTC, abseil-cpp, ggml, espeak-ng, kokoro.cpp, qwen3-asr.cpp, cpp-httplib, json).
- Checkout exact commit versions based on metadata.json files in `patches/`.
- Download third-party `rnnoise` and `pffft` files into WebRTC.
- Apply saved patch files (like `combined.patch` or platform compatibility fixes).
- Download, extract, and lay out ONNX Runtime 1.24.1 from NuGet into `libraries/onnx/`.
- Download, extract, and lay out OpenSSL 3.0.17 from NuGet into `libraries/` for Windows platforms.

### 2. Capture Local Changes (Patches)
If you make changes to a dependency (e.g. `kokoro.cpp` or `qwen3-asr.cpp`) and want to generate or update its patch files:
```bash
python generate_patches.py
```
This script will detect dirty files and create new diff patches under `patches/<dep_name>/` alongside `metadata.json` containing the exact commit reference, allowing other developers to apply your changes cleanly.

### 3. Build and Package
To build all static libraries and copy the headers into the project's root `libraries/` folder:
```bash
python build_deps.py --platform <platform>
```

#### Platform Options:
- `windows`: Compiles all dependencies. GGML and Qwen3 link with CUDA (GPU support, expected CUDA 12.x). Kokoro links with ONNX Runtime. Uses MSVC.
- `linux`: Compiles all dependencies with CUDA GPU support on Linux.
- `osx`: Compiles all dependencies with Metal GPU support (requires macOS host).
- `ios`: Cross-compiles for iOS with Metal GPU support. Disables code signing for tests.
- `android`: Cross-compiles for Android ABIs (`arm64-v8a`, `armeabi-v7a`, `x86_64`) using the Vulkan backend for GGML/Qwen3. Requires `--ndk <path-to-ndk>`.
- `current` (default): Detects and compiles for the host OS.
- `all`: Builds all possible targets natively compilable on the host.

Example for Android:
```bash
python build_deps.py --platform android --ndk C:\Android\Sdk\ndk\25.1.8937393
```

---

## Output Layout

Built libraries and include headers are installed under the root `libraries/` directory:

### Headers
Exported under:
- **WebRTC**: `libraries/include/webrtc_audio_processing/`
- **GGML**: `libraries/include/ggml/`
- **espeak-ng**: `libraries/include/espeak-ng/`
- **kokoro.cpp**: `libraries/include/kokoro/`
- **qwen3-asr.cpp**: `libraries/include/qwen3-asr/`
- **cpp-httplib**: `libraries/include/httplib.h`
- **nlohmann/json**: `libraries/include/nlohmann/`
- **ONNX Runtime**: Headers are placed under `libraries/onnx/<platform>/<config>/include/`
- **OpenSSL**: Headers are placed under `libraries/include/openssl/` (when precompiled for Windows)

### Static Libraries
Static libraries (`.lib` or `.a`) are placed in:
- `libraries/lib/<lib_name>/<platform>/[abi/]<config>/`

For example:
- `libraries/lib/kokoro/windows/Release/kokoro_core.lib`
- `libraries/lib/qwen3-asr/windows/Release/qwen3_asr.lib`
- `libraries/lib/espeak-ng/windows/Release/espeak-ng.lib`
- `libraries/lib/ggml/windows/Release/ggml-cuda.lib`
- `libraries/lib/webrtc_audio_processing/windows/Release/webrtc_audio_processing.lib`
- `libraries/lib/openssl/windows/Release/libssl.lib` and `libcrypto.lib` (on Windows NuGet download)

### CMake Find Modules
CMake imports are generated in:
- `libraries/cmake/` (e.g. `FindWebRtcAudioProcessing.cmake`, `FindGgml.cmake`, `FindEspeakNg.cmake`, `FindKokoro.cmake`, `FindQwen3Asr.cmake`, `FindOpenSSL.cmake`, `FindCppHttplib.cmake`, `FindNlohmannJson.cmake`)
