# Agent Instructions: Dependencies Management, Compilation & Patching

This document instructs AI coding agents on how to manage, download, build, and extend the portable dependencies located under `deps/` for the voice runtime.

---

## 1. Directory Structure & Layout

The project isolates dependency logic in the `deps/` directory:
- `deps/download_deps.py`: Script to download/clone sources (e.g., WebRTC, GGML, espeak-ng, kokoro.cpp, qwen3-asr.cpp) and fetch ONNX Runtime NuGet packages.
- `deps/build_deps.py`: Cross-platform build automation script compiling static libraries for Debug and Release configurations across 5 platforms.
- `deps/generate_patches.py`: Scans cloned repos for changes and exports patches and version metadata.
- `deps/patches/`: Registry holding diffs and metadata (`metadata.json`, `combined.patch`, and individual `.patch` files) for each repository.
- `libraries/`: Output folder containing packaged public headers (`libraries/include/`) and static libraries (`libraries/lib/`).

---

## 2. Standard Workflows

### A. Downloading Dependencies
To download or reset all dependencies:
```bash
python download_deps.py
```
This clones all required repositories, fetches external resources (such as `rnnoise` and `pffft` for WebRTC), and downloads ONNX Runtime 1.24.1.

### B. Building Dependencies
To build the static libraries for a specific platform, run `build_deps.py`:
```bash
# Windows
python build_deps.py --platform windows

# Linux (with CUDA support)
python build_deps.py --platform linux

# macOS (with Metal support)
python build_deps.py --platform osx

# iOS (with Metal support)
python build_deps.py --platform ios

# Android (with Vulkan GPU support; NDK required)
python build_deps.py --platform android --ndk /path/to/android-ndk
```

### C. Managing and Storing Patches
Local modifications (e.g., Windows compatibility workarounds, CMake customizations) must be preserved using the patch system:
1. Make code modifications directly within the cloned repository subfolder (e.g., `deps/qwen3-asr.cpp/`).
2. Run the patch generator:
   ```bash
   python generate_patches.py
   ```
3. The script will automatically scan for modified or staged files, fetch the exact Git remote URL, commit hash, and branch, and write the diffs and version indexes into the `patches/` folder.
4. Commit the generated patch files to the main project repository.

---

## 3. How to Add a New Library

To integrate a new dependency, follow these steps:

### Step 1: Update `download_deps.py`
Add a new entry to the `dependencies` list in `download_deps.py`:
```python
    dependencies = [
        ...
        {
            "name": "new-library",
            "url": "https://github.com/example/new-library.git"
        }
    ]
```

### Step 2: Configure the Static Build in `build_deps.py`
1. Define a builder function:
   ```python
   def build_dependency_newlib(deps_dir, dist_dir, platform, config, cmake_args, generator=None, arch=None, abi=None):
       # Configure with -DBUILD_SHARED_LIBS=OFF
       # Compile both Debug & Release configurations
       # Call copy_static_libs to copy the compiled static libraries (.lib or .a) to:
       #   dist_dir/lib/new-library/platform/[abi]/config/
   ```
2. Integrate the function inside all target platform loops (e.g. `build_windows`, `build_linux`, etc.).
3. Define a header copy function `copy_newlib_headers` to export public headers to:
   `dist_dir/include/new-library/`

### Step 3: Generate CMake Find Module in `generate_cmake_imports`
1. Define a CMake find module template in `generate_cmake_imports` inside `build_deps.py`:
   - File name format: `Find<LibraryName>.cmake` (placed in `dist_dir/cmake/`).
   - The file should dynamically resolve `PLATFORM_DIR`, `LIB_SUFFIX`, and `LIB_PREFIX` based on `CMAKE_SYSTEM_NAME` (mapping "Darwin" to "osx", "Windows" to "windows", "Android" to "android/${ANDROID_ABI}", etc.).
   - Define a global static imported target (e.g. `NewLibrary::NewLibrary`) and configure its `IMPORTED_LOCATION_DEBUG` and `IMPORTED_LOCATION_RELEASE` targets pointing to the precompiled binaries in `libraries/lib/`.
2. Write the generated find module string to `dist_dir/cmake/Find<LibraryName>.cmake`.

### Step 4: Capture Local Changes
If you had to edit any file in the new dependency to compile it successfully, run `python generate_patches.py` to create its patch files.

---

## 4. Key Portability & Compatibility Guidelines

When building or writing code patches, respect the following constraints:

### A. Windows (MSVC) Portability
- **Virtual Memory Mapping**: MSVC does not support POSIX `sys/mman.h` or `mmap`/`munmap`. Use conditional Win32 File Mapping instead:
  ```cpp
  #if defined(_WIN32)
      HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
      void* mmap_addr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
      // Clean up:
      UnmapViewOfFile(mmap_addr);
  #else
      void* mmap_addr = mmap(nullptr, st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      munmap(mmap_addr, st_size);
  #endif
  ```
- **Variable Length Arrays (VLAs)**: VLAs (e.g., `int arr[n_tokens];` where `n_tokens` is not constant) are unsupported in MSVC. Use dynamically allocated structures (`std::vector`) or constant arrays.

### B. Transitive GPU Backend Linking (GGML static libraries)
- If a dependency links against `ggml`, the final executable targets must link to the appropriate host GPU runtime libraries.
- For CUDA support, link against `CUDA::cuda_driver` (which contains `cuMemCreate` VMM functions) in addition to `CUDA::cudart`, `CUDA::cublas`, and `CUDA::cublasLt`.

### C. Multi-Config Build Isolation
- On Windows, Visual Studio uses a multi-config generator. Ensure that the scripts copy `.lib` files from the correct build folder matching the configuration (e.g., skip `Release` folders when copying `Debug` libraries) to prevent Iterator Debug Level or Runtime Library (`/MD` vs `/MDd`) mismatches.

### D. Symlink Resolution on Windows
- Avoid `shutil.copytree` when exporting header directories that contain Unix relative symlinks. Instead, write manual walking functions that resolve the symlinks to their absolute targets and safely handle any Windows-specific file path issues.

---

## 5. Updating a Dependency to a New Release

To update an existing dependency (e.g. `ggml`, `kokoro.cpp`, `qwen3-asr.cpp`) to a new upstream release:
1. Navigate to the dependency's local repository folder inside `deps/` (e.g. `deps/ggml/`).
2. Fetch and checkout the target branch, tag, or commit hash:
   ```bash
   git fetch origin
   git checkout <new-release-commit-or-tag>
   ```
3. Test compile the dependency locally using the CMake files. If portability adjustments are needed (e.g. Windows compatibility or include path updates), make them directly inside the repository.
4. Run the patch generator script in the `deps/` root:
   ```bash
   python generate_patches.py
   ```
   This will:
   - Calculate git diffs for modified/added files, ignoring submodules/gitlinks (`--ignore-submodules=all`).
   - Clean up any obsolete patch files from the registry.
   - Update `patches/<dependency>/metadata.json` with the new commit hash, branch name, and repository URL.
5. If the new version added or removed source files (especially for WebRTC), run the dependency build to update the file lists in `deps/CMakeLists.txt` automatically:
   ```bash
   python build_deps.py
   ```
6. Commit the updated patch registry and metadata files to the main project repository.

---

## 6. Platform Build Agents: Instructions for Building & Patch Syncing

When setting up builds on other target platforms (e.g. macOS, Linux, iOS, or Android):
1. **Synchronize Dependencies**:
   Run the download script to automatically clone all repositories, check out the exact commit hashes recorded in `metadata.json`, and apply the centralized patches:
   ```bash
   python download_deps.py
   ```
2. **Platform Compilation & Fixing Issues**:
   - If compile issues arise on the new platform, modify the source files directly in the cloned dependency folders (e.g. `deps/espeak-ng/`).
   - Run `python generate_patches.py` to regenerate the patch registry. Submodule pointers are automatically ignored.
3. **Build Execution**:
   - Run the compiler:
     ```bash
     python build_deps.py --platform [linux/osx/ios/android]
     ```
   - Running a successful build automatically triggers `generate_patches.py` at the end to ensure any local updates are captured.
4. **Validation and Staging**:
   - Verify the static library packaging (`libraries/lib/` and `libraries/include/`) contains the necessary targets.
   - Commit the updated patches, `metadata.json` updates, and CMake imports back to Git.
