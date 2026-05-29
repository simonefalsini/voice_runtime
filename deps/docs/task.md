# Task List: Integration of kokoro.cpp, qwen3-asr.cpp, and espeak-ng

- [x] Create patches for local changes in all dependencies using `generate_patches.py`
- [x] Download and install ONNX Runtime 1.24.1 across all platforms in `libraries/onnx/` via NuGet
- [x] Modify `kokoro.cpp/CMakeLists.txt` to support and prioritize custom `ONNXRUNTIME_ROOT`
- [x] Update patch registry for `kokoro.cpp` to include the `CMakeLists.txt` changes
- [x] Update `build_deps.py` to:
    - [x] Compile and package `espeak-ng` as a static library
    - [x] Compile and package `kokoro.cpp` (linking to ONNX Runtime 1.24.1)
    - [x] Compile and package `qwen3-asr.cpp` (linking to main GGML static libraries with GPU backends)
    - [x] Export header files for `espeak-ng`, `kokoro`, and `qwen3-asr`
- [x] Run Windows build for validation and verify output libraries and headers
- [x] Run macOS (OSX) build for validation and verify output libraries and headers
- [x] Run iOS build for validation and verify output libraries and headers
- [x] Create walkthrough documenting changes and results
