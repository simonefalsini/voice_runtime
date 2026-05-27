# Dependency Management & Build System

This directory handles downloading, patching, and compiling the static libraries and headers for the application's dependencies (currently WebRTC Audio Processing Modules and Abseil).

## Contents

- [download_deps.py](file:///d:/befree/voice_runtime/deps/download_deps.py): Python script that clones Abseil, WebRTC, downloads required third-party components (`rnnoise` and `pffft`), and applies the MSVC compatibility patch.
- [build_deps.py](file:///d:/befree/voice_runtime/deps/build_deps.py): Python script that configures and builds the dependencies in both **Debug** and **Release** configurations.
- [CMakeLists.txt](file:///d:/befree/voice_runtime/deps/CMakeLists.txt): CMake configuration containing source files lists, compiler definitions, and linking flags for WebRTC.
- [webrtc_msvc_compat.patch](file:///d:/befree/voice_runtime/deps/webrtc_msvc_compat.patch): Git patch file with fixes required to compile WebRTC's AVX2 code under Visual Studio/MSVC.
- [.gitignore](file:///d:/befree/voice_runtime/deps/.gitignore): Excludes cloned vendor sources and local build/intermediate files from Git.

---

## Workflow Instructions

### 1. Download & Clone Dependencies
To download all required dependencies and apply the compiler patch, run:
```bash
python download_deps.py
```
This script will:
- Clone the WebRTC source repository.
- Clone the Abseil-cpp repository.
- Download `rnnoise` and `pffft` files directly into WebRTC's third-party directory.
- Check and apply `webrtc_msvc_compat.patch` to WebRTC.

### 2. Build and Package
To build the static library and copy the headers into the project's root `libraries/` folder, run:
```bash
python build_deps.py --platform <platform>
```

#### Platform Options:
- `windows`: Compiles using Visual Studio 2022 (requires Windows host).
- `linux`: Compiles on a Linux host (requires Linux host).
- `osx`: Compiles on a macOS host.
- `ios`: Cross-compiles for iOS (requires macOS host and Xcode).
- `android`: Cross-compiles for Android ABIs (`arm64-v8a`, `armeabi-v7a`, `x86_64`). Requires `--ndk <path-to-ndk>`.
- `current` (default): Detects and compiles for the host OS.
- `all`: Builds all possible targets natively compilable on the host.

Example for Android:
```bash
python build_deps.py --platform android --ndk C:\Android\Sdk\ndk\25.1.8937393
```

---

## Output Layout

After a successful compilation, the built assets are packaged and placed in the project root's `libraries/` directory with the following structure:

### Headers
Exported under `libraries/include/webrtc_audio_processing/` (e.g. `api/`, `absl/`, `rtc_base/` directories). Add this include path to your compiler settings:
```cpp
#include "api/audio/audio_processing.h"
#include "absl/strings/string_view.h"
```

### Static Libraries
Compiled static libraries are installed under:
- **Windows**:
  - Debug: `libraries/lib/webrtc_audio_processing/windows/Debug/webrtc_audio_processing.lib`
  - Release: `libraries/lib/webrtc_audio_processing/windows/Release/webrtc_audio_processing.lib`
- **Linux**:
  - Debug: `libraries/lib/webrtc_audio_processing/linux/Debug/libwebrtc_audio_processing.a`
  - Release: `libraries/lib/webrtc_audio_processing/linux/Release/libwebrtc_audio_processing.a`
- **macOS (OSX)**:
  - Debug: `libraries/lib/webrtc_audio_processing/osx/Debug/libwebrtc_audio_processing.a`
  - Release: `libraries/lib/webrtc_audio_processing/osx/Release/libwebrtc_audio_processing.a`
- **iOS**:
  - Debug: `libraries/lib/webrtc_audio_processing/ios/Debug/libwebrtc_audio_processing.a`
  - Release: `libraries/lib/webrtc_audio_processing/ios/Release/libwebrtc_audio_processing.a`
- **Android**:
  - Debug: `libraries/lib/webrtc_audio_processing/android/<abi>/Debug/libwebrtc_audio_processing.a`
  - Release: `libraries/lib/webrtc_audio_processing/android/<abi>/Release/libwebrtc_audio_processing.a`
