import os
import sys
import argparse
import subprocess
import shutil

def run_cmd(cmd, cwd=None):
    print(f"Executing: {' '.join(cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)

def detect_host_platform():
    if sys.platform.startswith('win'):
        return 'windows'
    elif sys.platform.startswith('darwin'):
        return 'osx'
    elif sys.platform.startswith('linux'):
        return 'linux'
    else:
        return 'unknown'

def copy_static_libs(build_dir, dest_dir, target_names=None):
    os.makedirs(dest_dir, exist_ok=True)
    copied = 0
    for root, dirs, files in os.walk(build_dir):
        if "CMakeFiles" in root:
            continue
        for file in files:
            if file.endswith('.lib') or file.endswith('.a'):
                base = os.path.splitext(file)[0]
                if base.startswith('lib') and base != 'lib':
                    base_no_prefix = base[3:]
                else:
                    base_no_prefix = base
                
                if target_names:
                    match = False
                    for tname in target_names:
                        if base == tname or base_no_prefix == tname:
                            match = True
                            break
                    if not match:
                        continue
                        
                src_file = os.path.join(root, file)
                dest_file = os.path.join(dest_dir, file)
                shutil.copy2(src_file, dest_file)
                print(f"Copied static library: {file} -> {dest_dir}")
                copied += 1
    return copied

def get_ggml_cmake_args(dist_dir, platform, config, abi=None):
    if platform == "android":
        ggml_lib_dir = os.path.join(dist_dir, "lib", "ggml", platform, abi, config)
    else:
        ggml_lib_dir = os.path.join(dist_dir, "lib", "ggml", platform, config)
        
    args = []
    if os.path.exists(ggml_lib_dir):
        for file in os.listdir(ggml_lib_dir):
            if file.endswith('.lib') or file.endswith('.a'):
                path = os.path.join(ggml_lib_dir, file).replace('\\', '/')
                if 'ggml-base' in file:
                    args.append(f"-DGGML_BASE_LIB={path}")
                elif 'ggml-cpu' in file:
                    args.append(f"-DGGML_CPU_LIB={path}")
                elif 'ggml-cuda' in file:
                    args.append(f"-DGGML_CUDA_LIB={path}")
                elif 'ggml-metal' in file:
                    args.append(f"-DGGML_METAL_LIB={path}")
                elif 'ggml-vulkan' in file:
                    args.append(f"-DGGML_VULKAN_LIB={path}")
                elif 'ggml-blas' in file:
                    args.append(f"-DGGML_BLAS_LIB={path}")
                elif file.startswith('ggml.') or file.startswith('libggml.'):
                    args.append(f"-DGGML_LIB={path}")
    return args

def copy_espeak_headers(deps_dir, dist_dir):
    print("--- Exporting espeak-ng Headers ---")
    src_dir = os.path.join(deps_dir, "espeak-ng", "src", "include")
    dest_dir = os.path.join(dist_dir, "include", "espeak-ng")
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    os.makedirs(dest_dir, exist_ok=True)
    if os.path.exists(src_dir):
        for item in ["espeak", "espeak-ng", "compat"]:
            item_src = os.path.join(src_dir, item)
            if os.path.exists(item_src):
                item_dest = os.path.join(dest_dir, item)
                os.makedirs(item_dest, exist_ok=True)
                for root, dirs, files in os.walk(item_src):
                    rel_path = os.path.relpath(root, item_src)
                    dest_subdir = os.path.join(item_dest, rel_path) if rel_path != "." else item_dest
                    os.makedirs(dest_subdir, exist_ok=True)
                    for file in files:
                        src_file = os.path.join(root, file)
                        dest_file = os.path.join(dest_subdir, file)
                        
                        # Handle symlinks safely on Windows
                        if os.path.islink(src_file):
                            try:
                                link_target = os.readlink(src_file)
                                resolved_src = os.path.abspath(os.path.join(root, link_target))
                                if os.path.exists(resolved_src):
                                    shutil.copy2(resolved_src, dest_file)
                                    continue
                            except Exception as e:
                                print(f"Warning: Skipping symlink {src_file} -> {dest_file} due to resolution error: {e}")
                                continue
                        
                        try:
                            shutil.copy2(src_file, dest_file)
                        except Exception as e:
                            print(f"Warning: Failed to copy {src_file} -> {dest_file}: {e}")
    print(f"espeak-ng Headers exported to {dest_dir}")

def copy_kokoro_headers(deps_dir, dist_dir):
    print("--- Exporting kokoro.cpp Headers ---")
    src_dir = os.path.join(deps_dir, "kokoro.cpp")
    dest_dir = os.path.join(dist_dir, "include", "kokoro")
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    os.makedirs(dest_dir, exist_ok=True)
    headers = ["Kokoro.h", "Tokenizer.h", "ZHFrontend.h", "ZHG2P.h", "ToneSandhi.h", "PinyinFinder.h", "EnG2P.h", "JiebaProcessor.h", "Utils.h"]
    for header in headers:
        src_file = os.path.join(src_dir, header)
        if os.path.exists(src_file):
            shutil.copy2(src_file, os.path.join(dest_dir, header))
    print(f"kokoro.cpp Headers exported to {dest_dir}")

def copy_qwen3_headers(deps_dir, dist_dir):
    print("--- Exporting qwen3-asr.cpp Headers ---")
    src_dir = os.path.join(deps_dir, "qwen3-asr.cpp", "src")
    dest_dir = os.path.join(dist_dir, "include", "qwen3-asr")
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    os.makedirs(dest_dir, exist_ok=True)
    if os.path.exists(src_dir):
        for file in os.listdir(src_dir):
            if file.endswith('.h') or file.endswith('.hpp'):
                shutil.copy2(os.path.join(src_dir, file), os.path.join(dest_dir, file))
    print(f"qwen3-asr.cpp Headers exported to {dest_dir}")

def copy_cpp_httplib_headers(deps_dir, dist_dir):
    print("--- Exporting cpp-httplib Headers ---")
    src_file = os.path.join(deps_dir, "cpp-httplib", "httplib.h")
    dest_dir = os.path.join(dist_dir, "include")
    os.makedirs(dest_dir, exist_ok=True)
    if os.path.exists(src_file):
        shutil.copy2(src_file, os.path.join(dest_dir, "httplib.h"))
        print(f"cpp-httplib Headers exported to {dest_dir}")
    else:
        print(f"Warning: {src_file} not found.")

def copy_nlohmann_json_headers(deps_dir, dist_dir):
    print("--- Exporting nlohmann/json Headers ---")
    src_dir = os.path.join(deps_dir, "json", "single_include", "nlohmann")
    dest_dir = os.path.join(dist_dir, "include", "nlohmann")
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    os.makedirs(dest_dir, exist_ok=True)
    if os.path.exists(src_dir):
        shutil.copytree(src_dir, dest_dir, dirs_exist_ok=True)
        print(f"nlohmann/json Headers exported to {dest_dir}")
    else:
        print(f"Warning: {src_dir} not found.")

def build_dependency_espeak(deps_dir, dist_dir, platform, config, cmake_args, generator=None, arch=None, abi=None):
    print(f"--- Building espeak-ng for {platform} ({config}" + (f", {abi}" if abi else "") + ") ---")
    src_dir = os.path.join(deps_dir, "espeak-ng")
    suffix = f"_{platform}"
    if abi:
        suffix += f"_{abi}"
    suffix += f"_{config.lower()}"
    build_dir = os.path.join(deps_dir, f"build_espeak{suffix}")
    
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    
    cmd_configure = [
        "cmake",
        "-S", src_dir,
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={config}",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DENABLE_TESTS=OFF",
        "-DCOMPILE_INTONATIONS=OFF"
    ]
    if generator:
        cmd_configure += ["-G", generator]
    if arch:
        cmd_configure += ["-A", arch]
    if cmake_args:
        cmd_configure += cmake_args
        
    run_cmd(cmd_configure)
    run_cmd(["cmake", "--build", build_dir, "--config", config, "--parallel"])
    
    dest_dir = os.path.join(dist_dir, "lib", "espeak-ng", platform, abi if abi else "", config)
    copy_static_libs(build_dir, dest_dir, ["espeak-ng", "ucd"])

def build_dependency_kokoro(deps_dir, dist_dir, platform, config, cmake_args, generator=None, arch=None, abi=None):
    print(f"--- Building kokoro.cpp for {platform} ({config}" + (f", {abi}" if abi else "") + ") ---")
    src_dir = os.path.join(deps_dir, "kokoro.cpp")
    suffix = f"_{platform}"
    if abi:
        suffix += f"_{abi}"
    suffix += f"_{config.lower()}"
    build_dir = os.path.join(deps_dir, f"build_kokoro{suffix}")
    
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    
    if platform == "android":
        onnx_root = os.path.join(dist_dir, "onnx", platform, abi, config).replace('\\', '/')
    else:
        onnx_root = os.path.join(dist_dir, "onnx", platform, config).replace('\\', '/')
        
    cmd_configure = [
        "cmake",
        "-S", src_dir,
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DONNXRUNTIME_ROOT={onnx_root}"
    ]
    if platform == "ios":
        cmd_configure += [
            f"-DONNXRUNTIME_LIB={onnx_root}/lib/onnxruntime.xcframework",
            f"-DONNXRUNTIME_INCLUDE_DIR={onnx_root}/include"
        ]
    if generator:
        cmd_configure += ["-G", generator]
    if arch:
        cmd_configure += ["-A", arch]
    if cmake_args:
        cmd_configure += cmake_args
        
    run_cmd(cmd_configure)
    run_cmd(["cmake", "--build", build_dir, "--config", config, "--parallel"])
    
    dest_dir = os.path.join(dist_dir, "lib", "kokoro", platform, abi if abi else "", config)
    copy_static_libs(build_dir, dest_dir, ["kokoro_core"])

def build_dependency_qwen3(deps_dir, dist_dir, platform, config, cmake_args, generator=None, arch=None, abi=None):
    print(f"--- Building qwen3-asr.cpp for {platform} ({config}" + (f", {abi}" if abi else "") + ") ---")
    src_dir = os.path.join(deps_dir, "qwen3-asr.cpp")
    ggml_src = os.path.join(deps_dir, "ggml").replace('\\', '/')
    suffix = f"_{platform}"
    if abi:
        suffix += f"_{abi}"
    suffix += f"_{config.lower()}"
    build_dir = os.path.join(deps_dir, f"build_qwen3{suffix}")
    
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    
    ggml_args = get_ggml_cmake_args(dist_dir, platform, config, abi)
    cmd_configure = [
        "cmake",
        "-S", src_dir,
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DGGML_DIR={ggml_src}"
    ] + ggml_args
    if generator:
        cmd_configure += ["-G", generator]
    if arch:
        cmd_configure += ["-A", arch]
    if cmake_args:
        cmd_configure += cmake_args
        
    run_cmd(cmd_configure)
    run_cmd(["cmake", "--build", build_dir, "--config", config, "--parallel"])
    
    dest_dir = os.path.join(dist_dir, "lib", "qwen3-asr", platform, abi if abi else "", config)
    copy_static_libs(build_dir, dest_dir, [
        "qwen3_asr", "mel_spectrogram", "audio_encoder", 
        "text_decoder", "audio_injection", "forced_aligner"
    ])

def copy_ggml_libs(build_dir, dest_dir, config=None):
    os.makedirs(dest_dir, exist_ok=True)
    copied = 0
    for root, dirs, files in os.walk(build_dir):
        if "CMakeFiles" in root:
            continue
        if config:
            path_parts = os.path.normpath(root).split(os.sep)
            # Skip folders of other configurations
            has_other_config = False
            for cfg in ["Release", "Debug", "MinSizeRel", "RelWithDebInfo"]:
                if cfg != config and cfg in path_parts:
                    has_other_config = True
                    break
            if has_other_config:
                continue
        for file in files:
            if file.endswith('.lib') or file.endswith('.a'):
                src_file = os.path.join(root, file)
                dest_file = os.path.join(dest_dir, file)
                shutil.copy2(src_file, dest_file)
                print(f"Copied static library: {file} -> {dest_dir}")
                copied += 1
    if copied == 0:
        print(f"Warning: No static libraries (*.lib/*.a) found in {build_dir}")

def copy_ggml_headers(deps_dir, dist_dir):
    print("--- Exporting GGML Headers ---")
    ggml_dir = os.path.join(deps_dir, "ggml")
    if not os.path.exists(ggml_dir):
        print("Warning: GGML directory does not exist, skipping header copy.")
        return
        
    include_dest = os.path.join(dist_dir, "include", "ggml")
    if os.path.exists(include_dest):
        shutil.rmtree(include_dest)
        
    # Copy from include/
    src_include = os.path.join(ggml_dir, "include")
    if os.path.exists(src_include):
        for root, dirs, files in os.walk(src_include):
            for file in files:
                if file.endswith('.h') or file.endswith('.hpp') or file.endswith('.inc'):
                    src_file = os.path.join(root, file)
                    rel_path = os.path.relpath(src_file, src_include)
                    dest_file = os.path.join(include_dest, rel_path)
                    os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                    shutil.copy2(src_file, dest_file)
                    
    # Copy from src/
    src_src = os.path.join(ggml_dir, "src")
    if os.path.exists(src_src):
        for root, dirs, files in os.walk(src_src):
            for file in files:
                if file.endswith('.h') or file.endswith('.hpp') or file.endswith('.inc'):
                    src_file = os.path.join(root, file)
                    rel_path = os.path.relpath(src_file, src_src)
                    dest_file = os.path.join(include_dest, "src", rel_path)
                    os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                    shutil.copy2(src_file, dest_file)
                    
    print(f"GGML Headers exported to {include_dest}")

def build_windows(deps_dir, dist_dir):
    print("--- Building WebRTC APM for Windows ---")
    build_dir = os.path.join(deps_dir, "build_windows")
    # if os.path.exists(build_dir):
    #     print("Cleaning previous build directory...")
    #     shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    
    # Configure (MSVC is multi-config, so we don't specify CMAKE_BUILD_TYPE at configure time)
    cmd_configure = [
        "cmake",
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-S", deps_dir,
        "-B", build_dir
    ]
    run_cmd(cmd_configure)
    
    configs = ["Release", "Debug"]
    for config in configs:
        print(f"Building Windows {config} configuration...")
        cmd_build = [
            "cmake",
            "--build", build_dir,
            "--config", config,
            "--parallel"
        ]
        run_cmd(cmd_build)
        
        lib_src = os.path.join(build_dir, config, "webrtc_audio_processing.lib")
        if not os.path.exists(lib_src):
            lib_src = os.path.join(build_dir, "webrtc_audio_processing.lib")
            
        lib_dest_dir = os.path.join(dist_dir, "lib", "webrtc_audio_processing", "windows", config)
        os.makedirs(lib_dest_dir, exist_ok=True)
        shutil.copy(lib_src, os.path.join(lib_dest_dir, "webrtc_audio_processing.lib"))
        absl_build_dir = os.path.join(build_dir, "abseil-cpp")
        if os.path.exists(absl_build_dir):
            copy_static_libs(absl_build_dir, lib_dest_dir)
        
    print("Windows WebRTC build successful for all configurations!")

    # GGML Build
    ggml_src = os.path.join(deps_dir, "ggml")
    if os.path.exists(ggml_src):
        print("--- Building GGML for Windows (CUDA support) ---")
        ggml_build = os.path.join(deps_dir, "build_ggml_windows")
        # if os.path.exists(ggml_build):
        #     shutil.rmtree(ggml_build, ignore_errors=True)
        os.makedirs(ggml_build, exist_ok=True)
        
        cmd_ggml_configure = [
            "cmake",
            "-G", "Visual Studio 17 2022",
            "-A", "x64",
            "-S", ggml_src,
            "-B", ggml_build,
            "-DBUILD_SHARED_LIBS=OFF",
            "-DGGML_BUILD_TESTS=OFF",
            "-DGGML_BUILD_EXAMPLES=OFF",
            "-DGGML_CUDA=ON"
        ]
        run_cmd(cmd_ggml_configure)
        
        for config in configs:
            print(f"Building GGML Windows {config} configuration...")
            cmd_ggml_build = [
                "cmake",
                "--build", ggml_build,
                "--config", config,
                "--parallel"
            ]
            run_cmd(cmd_ggml_build)
            
            dest_dir = os.path.join(dist_dir, "lib", "ggml", "windows", config)
            copy_ggml_libs(ggml_build, dest_dir, config)
            
        print("Windows GGML build successful for all configurations!")

    # Build new dependencies for Windows
    for config in configs:
        build_dependency_espeak(
            deps_dir, dist_dir, "windows", config,
            cmake_args=[],
            generator="Visual Studio 17 2022",
            arch="x64"
        )
        build_dependency_kokoro(
            deps_dir, dist_dir, "windows", config,
            cmake_args=[],
            generator="Visual Studio 17 2022",
            arch="x64"
        )
        build_dependency_qwen3(
            deps_dir, dist_dir, "windows", config,
            cmake_args=[],
            generator="Visual Studio 17 2022",
            arch="x64"
        )

def build_linux(deps_dir, dist_dir):
    print("--- Building WebRTC APM for Linux ---")
    for config in ["Release", "Debug"]:
        print(f"Building Linux {config} configuration...")
        build_dir = os.path.join(deps_dir, f"build_linux_{config.lower()}")
        if os.path.exists(build_dir):
            shutil.rmtree(build_dir, ignore_errors=True)
        os.makedirs(build_dir, exist_ok=True)
        
        # Configure
        cmd_configure = [
            "cmake",
            "-S", deps_dir,
            "-B", build_dir,
            f"-DCMAKE_BUILD_TYPE={config}"
        ]
        run_cmd(cmd_configure)
        
        # Build
        cmd_build = [
            "cmake",
            "--build", build_dir,
            "--config", config,
            "--parallel"
        ]
        run_cmd(cmd_build)
        
        # Copy library
        lib_src = os.path.join(build_dir, "libwebrtc_audio_processing.a")
        lib_dest_dir = os.path.join(dist_dir, "lib", "webrtc_audio_processing", "linux", config)
        os.makedirs(lib_dest_dir, exist_ok=True)
        shutil.copy(lib_src, os.path.join(lib_dest_dir, "libwebrtc_audio_processing.a"))
        absl_build_dir = os.path.join(build_dir, "abseil-cpp")
        if os.path.exists(absl_build_dir):
            copy_static_libs(absl_build_dir, lib_dest_dir)
        
    print("Linux WebRTC build successful for all configurations!")

    # GGML Build
    ggml_src = os.path.join(deps_dir, "ggml")
    if os.path.exists(ggml_src):
        print("--- Building GGML for Linux (CUDA support) ---")
        for config in ["Release", "Debug"]:
            print(f"Building GGML Linux {config} configuration...")
            ggml_build = os.path.join(deps_dir, f"build_ggml_linux_{config.lower()}")
            if os.path.exists(ggml_build):
                shutil.rmtree(ggml_build, ignore_errors=True)
            os.makedirs(ggml_build, exist_ok=True)
            
            cmd_ggml_configure = [
                "cmake",
                "-S", ggml_src,
                "-B", ggml_build,
                f"-DCMAKE_BUILD_TYPE={config}",
                "-DBUILD_SHARED_LIBS=OFF",
                "-DGGML_BUILD_TESTS=OFF",
                "-DGGML_BUILD_EXAMPLES=OFF",
                "-DGGML_CUDA=ON"
            ]
            run_cmd(cmd_ggml_configure)
            
            cmd_ggml_build = [
                "cmake",
                "--build", ggml_build,
                "--config", config,
                "--parallel"
            ]
            run_cmd(cmd_ggml_build)
            
            dest_dir = os.path.join(dist_dir, "lib", "ggml", "linux", config)
            copy_ggml_libs(ggml_build, dest_dir, config)
            
        print("Linux GGML build successful for all configurations!")

    # Build new dependencies for Linux
    for config in ["Release", "Debug"]:
        build_dependency_espeak(
            deps_dir, dist_dir, "linux", config,
            cmake_args=[]
        )
        build_dependency_kokoro(
            deps_dir, dist_dir, "linux", config,
            cmake_args=[]
        )
        build_dependency_qwen3(
            deps_dir, dist_dir, "linux", config,
            cmake_args=[]
        )

def build_osx(deps_dir, dist_dir):
    print("--- Building WebRTC APM for OSX ---")
    for config in ["Release", "Debug"]:
        print(f"Building OSX {config} configuration...")
        build_dir = os.path.join(deps_dir, f"build_osx_{config.lower()}")
        if os.path.exists(build_dir):
            shutil.rmtree(build_dir, ignore_errors=True)
        os.makedirs(build_dir, exist_ok=True)
        
        # Configure
        cmd_configure = [
            "cmake",
            "-S", deps_dir,
            "-B", build_dir,
            f"-DCMAKE_BUILD_TYPE={config}"
        ]
        run_cmd(cmd_configure)
        
        # Build
        cmd_build = [
            "cmake",
            "--build", build_dir,
            "--config", config,
            "--parallel"
        ]
        run_cmd(cmd_build)
        
        # Copy library
        lib_src = os.path.join(build_dir, "libwebrtc_audio_processing.a")
        lib_dest_dir = os.path.join(dist_dir, "lib", "webrtc_audio_processing", "osx", config)
        os.makedirs(lib_dest_dir, exist_ok=True)
        shutil.copy(lib_src, os.path.join(lib_dest_dir, "libwebrtc_audio_processing.a"))
        absl_build_dir = os.path.join(build_dir, "abseil-cpp")
        if os.path.exists(absl_build_dir):
            copy_static_libs(absl_build_dir, lib_dest_dir)
        
    print("OSX WebRTC build successful for all configurations!")

    # GGML Build
    ggml_src = os.path.join(deps_dir, "ggml")
    if os.path.exists(ggml_src):
        print("--- Building GGML for OSX (Metal support) ---")
        for config in ["Release", "Debug"]:
            print(f"Building GGML OSX {config} configuration...")
            ggml_build = os.path.join(deps_dir, f"build_ggml_osx_{config.lower()}")
            if os.path.exists(ggml_build):
                shutil.rmtree(ggml_build, ignore_errors=True)
            os.makedirs(ggml_build, exist_ok=True)
            
            cmd_ggml_configure = [
                "cmake",
                "-S", ggml_src,
                "-B", ggml_build,
                f"-DCMAKE_BUILD_TYPE={config}",
                "-DBUILD_SHARED_LIBS=OFF",
                "-DGGML_BUILD_TESTS=OFF",
                "-DGGML_BUILD_EXAMPLES=OFF",
                "-DGGML_METAL=ON"
            ]
            run_cmd(cmd_ggml_configure)
            
            cmd_ggml_build = [
                "cmake",
                "--build", ggml_build,
                "--config", config,
                "--parallel"
            ]
            run_cmd(cmd_ggml_build)
            
            dest_dir = os.path.join(dist_dir, "lib", "ggml", "osx", config)
            copy_ggml_libs(ggml_build, dest_dir, config)
            
        print("OSX GGML build successful for all configurations!")

    # Build new dependencies for OSX
    for config in ["Release", "Debug"]:
        build_dependency_espeak(
            deps_dir, dist_dir, "osx", config,
            cmake_args=[]
        )
        build_dependency_kokoro(
            deps_dir, dist_dir, "osx", config,
            cmake_args=[]
        )
        build_dependency_qwen3(
            deps_dir, dist_dir, "osx", config,
            cmake_args=[]
        )

def build_ios(deps_dir, dist_dir):
    print("--- Building WebRTC APM for iOS ---")
    build_dir = os.path.join(deps_dir, "build_ios")
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    
    # Configure for iOS device (arm64) using Xcode generator
    cmd_configure = [
        "cmake",
        "-G", "Xcode",
        "-S", deps_dir,
        "-B", build_dir,
        "-DCMAKE_SYSTEM_NAME=iOS",
        "-DCMAKE_OSX_SYSROOT=iphoneos"
    ]
    run_cmd(cmd_configure)
    
    for config in ["Release", "Debug"]:
        print(f"Building iOS {config} configuration...")
        # Build
        cmd_build = [
            "cmake",
            "--build", build_dir,
            "--config", config,
            "--parallel"
        ]
        run_cmd(cmd_build)
        
        # Copy library (Xcode builds usually put it under config-iphoneos/)
        lib_src = os.path.join(build_dir, f"{config}-iphoneos", "libwebrtc_audio_processing.a")
        if not os.path.exists(lib_src):
            lib_src = os.path.join(build_dir, "libwebrtc_audio_processing.a")
            
        lib_dest_dir = os.path.join(dist_dir, "lib", "webrtc_audio_processing", "ios", config)
        os.makedirs(lib_dest_dir, exist_ok=True)
        shutil.copy(lib_src, os.path.join(lib_dest_dir, "libwebrtc_audio_processing.a"))
        absl_build_dir = os.path.join(build_dir, "abseil-cpp")
        if os.path.exists(absl_build_dir):
            copy_static_libs(absl_build_dir, lib_dest_dir)
        
    print("iOS WebRTC build successful for all configurations!")

    # GGML Build
    ggml_src = os.path.join(deps_dir, "ggml")
    if os.path.exists(ggml_src):
        print("--- Building GGML for iOS (Metal support) ---")
        ggml_build = os.path.join(deps_dir, "build_ggml_ios")
        if os.path.exists(ggml_build):
            shutil.rmtree(ggml_build, ignore_errors=True)
        os.makedirs(ggml_build, exist_ok=True)
        
        cmd_ggml_configure = [
            "cmake",
            "-G", "Xcode",
            "-S", ggml_src,
            "-B", ggml_build,
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_SYSROOT=iphoneos",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DGGML_BUILD_TESTS=OFF",
            "-DGGML_BUILD_EXAMPLES=OFF",
            "-DGGML_METAL=ON"
        ]
        run_cmd(cmd_ggml_configure)
        
        for config in ["Release", "Debug"]:
            print(f"Building GGML iOS {config} configuration...")
            cmd_ggml_build = [
                "cmake",
                "--build", ggml_build,
                "--config", config,
                "--parallel"
            ]
            run_cmd(cmd_ggml_build)
            
            dest_dir = os.path.join(dist_dir, "lib", "ggml", "ios", config)
            copy_ggml_libs(ggml_build, dest_dir, config)
            
        print("iOS GGML build successful for all configurations!")

    # Build new dependencies for iOS
    for config in ["Release", "Debug"]:
        ios_cmake_args = [
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_SYSROOT=iphoneos",
            "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
            "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO"
        ]
        build_dependency_espeak(
            deps_dir, dist_dir, "ios", config,
            cmake_args=ios_cmake_args,
            generator="Xcode"
        )
        build_dependency_kokoro(
            deps_dir, dist_dir, "ios", config,
            cmake_args=ios_cmake_args,
            generator="Xcode"
        )
        build_dependency_qwen3(
            deps_dir, dist_dir, "ios", config,
            cmake_args=ios_cmake_args,
            generator="Xcode"
        )

def build_android(deps_dir, dist_dir, ndk_path=None):
    print("--- Building WebRTC APM for Android ---")
    
    # Try to find NDK if not provided
    if not ndk_path:
        ndk_path = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
    
    if not ndk_path:
        print("Error: Android NDK path not provided and not found in environment (ANDROID_NDK_HOME / ANDROID_NDK).")
        print("Please provide NDK path with --ndk parameter.")
        sys.exit(1)
        
    ndk_path = os.path.abspath(ndk_path)
    toolchain_file = os.path.join(ndk_path, "build", "cmake", "android.toolchain.cmake")
    if not os.path.exists(toolchain_file):
        print(f"Error: NDK toolchain file not found at: {toolchain_file}")
        sys.exit(1)
        
    # Build for major Android ABIs: arm64-v8a, armeabi-v7a, x86_64
    abis = ["arm64-v8a", "armeabi-v7a", "x86_64"]
    configs = ["Release", "Debug"]
    
    for abi in abis:
        for config in configs:
            print(f"Building WebRTC for Android ABI: {abi} ({config})...")
            build_dir = os.path.join(deps_dir, f"build_android_{abi}_{config.lower()}")
            if os.path.exists(build_dir):
                shutil.rmtree(build_dir, ignore_errors=True)
            os.makedirs(build_dir, exist_ok=True)
            
            # Configure
            cmd_configure = [
                "cmake",
                "-S", deps_dir,
                "-B", build_dir,
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
                f"-DANDROID_ABI={abi}",
                "-DANDROID_PLATFORM=android-21",
                f"-DCMAKE_BUILD_TYPE={config}"
            ]
            
            # On Windows, need to specify Make generator for NDK cross compilation if not using Ninja
            if sys.platform.startswith('win'):
                cmd_configure += ["-G", "MinGW Makefiles"]
                
            run_cmd(cmd_configure)
            
            # Build
            cmd_build = [
                "cmake",
                "--build", build_dir,
                "--config", config,
                "--parallel"
            ]
            run_cmd(cmd_build)
            
            # Copy library
            lib_src = os.path.join(build_dir, "libwebrtc_audio_processing.a")
            lib_dest_dir = os.path.join(dist_dir, "lib", "webrtc_audio_processing", "android", abi, config)
            os.makedirs(lib_dest_dir, exist_ok=True)
            shutil.copy(lib_src, os.path.join(lib_dest_dir, "libwebrtc_audio_processing.a"))
            absl_build_dir = os.path.join(build_dir, "abseil-cpp")
            if os.path.exists(absl_build_dir):
                copy_static_libs(absl_build_dir, lib_dest_dir)
            
    print("Android WebRTC build successful for all ABIs and configurations!")

    # GGML Build
    ggml_src = os.path.join(deps_dir, "ggml")
    if os.path.exists(ggml_src):
        print("--- Building GGML for Android (Vulkan GPU support) ---")
        for abi in abis:
            for config in configs:
                print(f"Building GGML for Android ABI: {abi} ({config})...")
                ggml_build = os.path.join(deps_dir, f"build_ggml_android_{abi}_{config.lower()}")
                if os.path.exists(ggml_build):
                    shutil.rmtree(ggml_build, ignore_errors=True)
                os.makedirs(ggml_build, exist_ok=True)
                
                cmd_ggml_configure = [
                    "cmake",
                    "-S", ggml_src,
                    "-B", ggml_build,
                    f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
                    f"-DANDROID_ABI={abi}",
                    "-DANDROID_PLATFORM=android-21",
                    f"-DCMAKE_BUILD_TYPE={config}",
                    "-DBUILD_SHARED_LIBS=OFF",
                    "-DGGML_BUILD_TESTS=OFF",
                    "-DGGML_BUILD_EXAMPLES=OFF",
                    "-DGGML_VULKAN=ON"
                ]
                if sys.platform.startswith('win'):
                    cmd_ggml_configure += ["-G", "MinGW Makefiles"]
                    
                run_cmd(cmd_ggml_configure)
                
                cmd_ggml_build = [
                    "cmake",
                    "--build", ggml_build,
                    "--config", config,
                    "--parallel"
                ]
                run_cmd(cmd_ggml_build)
                
                dest_dir = os.path.join(dist_dir, "lib", "ggml", "android", abi, config)
                copy_ggml_libs(ggml_build, dest_dir, config)
                
        print("Android GGML build successful for all ABIs and configurations!")

    # Build new dependencies for Android
    for abi in abis:
        for config in configs:
            cmake_args = [
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
                f"-DANDROID_ABI={abi}",
                "-DANDROID_PLATFORM=android-21"
            ]
            generator = "MinGW Makefiles" if sys.platform.startswith('win') else None
            
            build_dependency_espeak(
                deps_dir, dist_dir, "android", config,
                cmake_args=cmake_args,
                generator=generator,
                abi=abi
            )
            build_dependency_kokoro(
                deps_dir, dist_dir, "android", config,
                cmake_args=cmake_args,
                generator=generator,
                abi=abi
            )
            build_dependency_qwen3(
                deps_dir, dist_dir, "android", config,
                cmake_args=cmake_args,
                generator=generator,
                abi=abi
            )

def copy_headers(deps_dir, dist_dir):
    print("--- Exporting WebRTC APM Headers ---")
    webrtc_dir = os.path.join(deps_dir, "WebRTC")
    include_dest = os.path.join(dist_dir, "include", "webrtc_audio_processing")
    
    # We will copy all .h files from the requested parts of WebRTC
    subdirs_to_copy = [
        "api",
        "common_audio",
        "modules/audio_processing",
        "rtc_base",
        "system_wrappers"
    ]
    
    # Clear old headers if any
    if os.path.exists(include_dest):
        shutil.rmtree(include_dest)
        
    for sdir in subdirs_to_copy:
        src_path = os.path.join(webrtc_dir, sdir)
        for root, dirs, files in os.walk(src_path):
            # Skip test directories
            if "test" in root or "tests" in root or "mocks" in root:
                continue
            for file in files:
                if file.endswith('.h') or file.endswith('.hpp'):
                    # Source file full path
                    src_file = os.path.join(root, file)
                    # Get relative path from WebRTC root
                    rel_path = os.path.relpath(src_file, webrtc_dir)
                    # Destination file path
                    dest_file = os.path.join(include_dest, rel_path)
                    
                    os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                    shutil.copy2(src_file, dest_file)
                    
    # Also copy Abseil headers
    abseil_dir = os.path.join(deps_dir, "abseil-cpp")
    if os.path.exists(abseil_dir):
        absl_src = os.path.join(abseil_dir, "absl")
        absl_dest = os.path.join(include_dest, "absl")
        if os.path.exists(absl_dest):
            shutil.rmtree(absl_dest)
        
        for root, dirs, files in os.walk(absl_src):
            for file in files:
                if file.endswith('.h') or file.endswith('.inc'):
                    src_file = os.path.join(root, file)
                    rel_path = os.path.relpath(src_file, abseil_dir)
                    dest_file = os.path.join(include_dest, rel_path)
                    os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                    shutil.copy2(src_file, dest_file)

    print(f"Headers exported to {include_dest}")

def generate_cmake_imports(dist_dir):
    print("--- Generating CMake Import Modules ---")
    cmake_dir = os.path.join(dist_dir, "cmake")
    os.makedirs(cmake_dir, exist_ok=True)
    
    # Template contents
    find_webrtc = """# FindWebRtcAudioProcessing.cmake
# Defines target: WebRtcAudioProcessing::WebRtcAudioProcessing

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_DIR "windows")
    set(LIB_SUFFIX ".lib")
    set(LIB_PREFIX "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(PLATFORM_DIR "android/${ANDROID_ABI}")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_DIR "ios")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_DIR "osx")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
else()
    set(PLATFORM_DIR "linux")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
endif()

set(WEBRTC_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include/webrtc_audio_processing")
set(WEBRTC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../deps/WebRTC")

if(NOT TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    add_library(WebRtcAudioProcessing::WebRtcAudioProcessing STATIC IMPORTED GLOBAL)
    set_target_properties(WebRtcAudioProcessing::WebRtcAudioProcessing PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${WEBRTC_INCLUDE_DIR};${WEBRTC_SOURCE_DIR}"
    )

    if(APPLE)
        set_property(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES "-framework CoreFoundation" "-framework Foundation"
        )
    endif()

    # Debug config
    set(LIB_DEBUG "${CMAKE_CURRENT_LIST_DIR}/../lib/webrtc_audio_processing/${PLATFORM_DIR}/Debug/${LIB_PREFIX}webrtc_audio_processing${LIB_SUFFIX}")
    if(EXISTS "${LIB_DEBUG}")
        set_property(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(WebRtcAudioProcessing::WebRtcAudioProcessing PROPERTIES
            IMPORTED_LOCATION_DEBUG "${LIB_DEBUG}"
        )
        get_filename_component(LIB_DIR_DEBUG "${LIB_DEBUG}" DIRECTORY)
        file(GLOB ABSL_LIBS_DEBUG "${LIB_DIR_DEBUG}/${LIB_PREFIX}absl_*${LIB_SUFFIX}")
        foreach(lib ${ABSL_LIBS_DEBUG})
            set_property(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:Debug>:${lib}>"
            )
        endforeach()
    endif()

    # Release config
    set(LIB_RELEASE "${CMAKE_CURRENT_LIST_DIR}/../lib/webrtc_audio_processing/${PLATFORM_DIR}/Release/${LIB_PREFIX}webrtc_audio_processing${LIB_SUFFIX}")
    if(EXISTS "${LIB_RELEASE}")
        set_property(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
        set_target_properties(WebRtcAudioProcessing::WebRtcAudioProcessing PROPERTIES
            IMPORTED_LOCATION_RELEASE "${LIB_RELEASE}"
        )
        get_filename_component(LIB_DIR_RELEASE "${LIB_RELEASE}" DIRECTORY)
        file(GLOB ABSL_LIBS_RELEASE "${LIB_DIR_RELEASE}/${LIB_PREFIX}absl_*${LIB_SUFFIX}")
        foreach(lib ${ABSL_LIBS_RELEASE})
            set_property(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:Release>:${lib}>"
            )
        endforeach()
    endif()
endif()
"""

    find_ggml = """# FindGgml.cmake
# Defines targets: Ggml::Ggml and Ggml::<sublib> (ggml, ggml-base, ggml-cpu, ggml-cuda, ggml-metal, ggml-vulkan)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_DIR "windows")
    set(LIB_SUFFIX ".lib")
    set(LIB_PREFIX "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(PLATFORM_DIR "android/${ANDROID_ABI}")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_DIR "ios")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_DIR "osx")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
else()
    set(PLATFORM_DIR "linux")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
endif()

set(GGML_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include/ggml")

if(NOT TARGET Ggml::Ggml)
    # Main target
    add_library(Ggml::Ggml INTERFACE IMPORTED GLOBAL)
    set_target_properties(Ggml::Ggml PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${GGML_INCLUDE_DIR}"
    )

    # We check for configurations
    foreach(config IN ITEMS DEBUG RELEASE)
        string(TOLOWER ${config} config_lower)
        if(config_lower STREQUAL "debug")
            set(cfg_dir "Debug")
        else()
            set(cfg_dir "Release")
        endif()
        
        set(base_dir "${CMAKE_CURRENT_LIST_DIR}/../lib/ggml/${PLATFORM_DIR}/${cfg_dir}")
        if(EXISTS "${base_dir}")
            set_property(TARGET Ggml::Ggml APPEND PROPERTY IMPORTED_CONFIGURATIONS ${config})
            
            # List of candidate libraries: ggml, ggml-base, ggml-cpu, ggml-cuda, ggml-metal, ggml-vulkan, ggml-blas
            set(cand_libs "ggml" "ggml-base" "ggml-cpu" "ggml-cuda" "ggml-metal" "ggml-vulkan" "ggml-blas")
            set(GGML_LIBS "")
            foreach(lib IN LISTS cand_libs)
                set(lib_path "${base_dir}/${LIB_PREFIX}${lib}${LIB_SUFFIX}")
                if(EXISTS "${lib_path}")
                    # Create imported target for this sub-library if not exists
                    if(NOT TARGET Ggml::${lib})
                        add_library(Ggml::${lib} STATIC IMPORTED GLOBAL)
                    endif()
                    set_target_properties(Ggml::${lib} PROPERTIES
                        IMPORTED_LOCATION_${config} "${lib_path}"
                    )
                    list(APPEND GGML_LIBS Ggml::${lib})
                endif()
            endforeach()
            
            # Link main target to all sub-libraries
            set_property(TARGET Ggml::Ggml APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:${config}>:${GGML_LIBS}>"
            )
        endif()
    endforeach()
endif()
"""

    find_espeak = """# FindEspeakNg.cmake
# Defines target: EspeakNg::EspeakNg and EspeakNg::<sublib> (espeak-ng, ucd)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_DIR "windows")
    set(LIB_SUFFIX ".lib")
    set(LIB_PREFIX "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(PLATFORM_DIR "android/${ANDROID_ABI}")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_DIR "ios")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_DIR "osx")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
else()
    set(PLATFORM_DIR "linux")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
endif()

set(ESPEAK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include/espeak-ng")

if(NOT TARGET EspeakNg::EspeakNg)
    add_library(EspeakNg::EspeakNg INTERFACE IMPORTED GLOBAL)
    set_target_properties(EspeakNg::EspeakNg PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ESPEAK_INCLUDE_DIR}"
    )

    foreach(config IN ITEMS DEBUG RELEASE)
        string(TOLOWER ${config} config_lower)
        if(config_lower STREQUAL "debug")
            set(cfg_dir "Debug")
        else()
            set(cfg_dir "Release")
        endif()
        
        set(base_dir "${CMAKE_CURRENT_LIST_DIR}/../lib/espeak-ng/${PLATFORM_DIR}/${cfg_dir}")
        if(EXISTS "${base_dir}")
            set_property(TARGET EspeakNg::EspeakNg APPEND PROPERTY IMPORTED_CONFIGURATIONS ${config})
            
            set(libs "espeak-ng" "ucd")
            set(ESPEAK_LIBS "")
            foreach(lib IN LISTS libs)
                set(lib_path "${base_dir}/${LIB_PREFIX}${lib}${LIB_SUFFIX}")
                if(EXISTS "${lib_path}")
                    if(NOT TARGET EspeakNg::${lib})
                        add_library(EspeakNg::${lib} STATIC IMPORTED GLOBAL)
                    endif()
                    set_target_properties(EspeakNg::${lib} PROPERTIES
                        IMPORTED_LOCATION_${config} "${lib_path}"
                    )
                    list(APPEND ESPEAK_LIBS EspeakNg::${lib})
                endif()
            endforeach()
            
            set_property(TARGET EspeakNg::EspeakNg APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:${config}>:${ESPEAK_LIBS}>"
            )
        endif()
    endforeach()
endif()
"""

    find_kokoro = """# FindKokoro.cmake
# Defines target: Kokoro::Kokoro (composed of Kokoro::kokoro_core and Kokoro::onnxruntime)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_DIR "windows")
    set(LIB_SUFFIX ".lib")
    set(LIB_PREFIX "")
    set(ONNX_LIB_NAME "onnxruntime")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(PLATFORM_DIR "android/${ANDROID_ABI}")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
    set(ONNX_LIB_NAME "onnxruntime")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_DIR "ios")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
    set(ONNX_LIB_NAME "onnxruntime")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_DIR "osx")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
    set(ONNX_LIB_NAME "onnxruntime")
else()
    set(PLATFORM_DIR "linux")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
    set(ONNX_LIB_NAME "onnxruntime")
endif()

set(KOKORO_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include/kokoro")

if(NOT TARGET Kokoro::Kokoro)
    add_library(Kokoro::Kokoro INTERFACE IMPORTED GLOBAL)
    set_target_properties(Kokoro::Kokoro PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${KOKORO_INCLUDE_DIR}"
    )

    if(NOT TARGET Kokoro::onnxruntime)
        add_library(Kokoro::onnxruntime SHARED IMPORTED GLOBAL)
    endif()

    foreach(config IN ITEMS DEBUG RELEASE)
        string(TOLOWER ${config} config_lower)
        if(config_lower STREQUAL "debug")
            set(cfg_dir "Debug")
        else()
            set(cfg_dir "Release")
        endif()
        
        set(base_dir "${CMAKE_CURRENT_LIST_DIR}/../lib/kokoro/${PLATFORM_DIR}/${cfg_dir}")
        set(onnx_dir "${CMAKE_CURRENT_LIST_DIR}/../onnx/${PLATFORM_DIR}/${cfg_dir}")
        
        if(EXISTS "${base_dir}")
            set_property(TARGET Kokoro::Kokoro APPEND PROPERTY IMPORTED_CONFIGURATIONS ${config})
            
            # Kokoro core
            set(lib_path "${base_dir}/${LIB_PREFIX}kokoro_core${LIB_SUFFIX}")
            if(EXISTS "${lib_path}")
                if(NOT TARGET Kokoro::kokoro_core)
                    add_library(Kokoro::kokoro_core STATIC IMPORTED GLOBAL)
                endif()
                set_target_properties(Kokoro::kokoro_core PROPERTIES
                    IMPORTED_LOCATION_${config} "${lib_path}"
                )
            endif()
            
            # ONNX Runtime
            set(onnx_include "${onnx_dir}/include")
            if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
                set(onnx_lib_file "${onnx_dir}/lib/${ONNX_LIB_NAME}${LIB_SUFFIX}")
                set(onnx_dll_file "${onnx_dir}/lib/${ONNX_LIB_NAME}.dll")
            else()
                set(onnx_lib_file "${onnx_dir}/lib/${LIB_PREFIX}${ONNX_LIB_NAME}${LIB_SUFFIX}")
            endif()
            
            if(EXISTS "${onnx_lib_file}")
                set_target_properties(Kokoro::onnxruntime PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${onnx_include}"
                )
                if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
                    set_target_properties(Kokoro::onnxruntime PROPERTIES
                        IMPORTED_IMPLIB_${config} "${onnx_lib_file}"
                        IMPORTED_LOCATION_${config} "${onnx_dll_file}"
                    )
                else()
                    set_target_properties(Kokoro::onnxruntime PROPERTIES
                        IMPORTED_LOCATION_${config} "${onnx_lib_file}"
                    )
                endif()
            endif()
            
            set_property(TARGET Kokoro::Kokoro APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:${config}>:Kokoro::kokoro_core;Kokoro::onnxruntime>"
            )
        endif()
    endforeach()
endif()
"""

    find_qwen3 = """# FindQwen3Asr.cmake
# Defines target: Qwen3Asr::Qwen3Asr and Qwen3Asr::<component> (qwen3_asr, forced_aligner, text_decoder, audio_encoder, mel_spectrogram, audio_injection)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PLATFORM_DIR "windows")
    set(LIB_SUFFIX ".lib")
    set(LIB_PREFIX "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(PLATFORM_DIR "android/${ANDROID_ABI}")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PLATFORM_DIR "ios")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PLATFORM_DIR "osx")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
else()
    set(PLATFORM_DIR "linux")
    set(LIB_SUFFIX ".a")
    set(LIB_PREFIX "lib")
endif()

set(QWEN3_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include/qwen3-asr")

if(NOT TARGET Qwen3Asr::Qwen3Asr)
    add_library(Qwen3Asr::Qwen3Asr INTERFACE IMPORTED GLOBAL)
    set_target_properties(Qwen3Asr::Qwen3Asr PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${QWEN3_INCLUDE_DIR}"
    )

    # Qwen3Asr requires Ggml
    if(NOT TARGET Ggml::Ggml)
        find_package(Ggml REQUIRED)
    endif()

    foreach(config IN ITEMS DEBUG RELEASE)
        string(TOLOWER ${config} config_lower)
        if(config_lower STREQUAL "debug")
            set(cfg_dir "Debug")
        else()
            set(cfg_dir "Release")
        endif()
        
        set(base_dir "${CMAKE_CURRENT_LIST_DIR}/../lib/qwen3-asr/${PLATFORM_DIR}/${cfg_dir}")
        if(EXISTS "${base_dir}")
            set_property(TARGET Qwen3Asr::Qwen3Asr APPEND PROPERTY IMPORTED_CONFIGURATIONS ${config})
            
            set(libs "qwen3_asr" "forced_aligner" "text_decoder" "audio_encoder" "mel_spectrogram" "audio_injection")
            set(QWEN3_LIBS "")
            foreach(lib IN LISTS libs)
                set(lib_path "${base_dir}/${LIB_PREFIX}${lib}${LIB_SUFFIX}")
                if(EXISTS "${lib_path}")
                    if(NOT TARGET Qwen3Asr::${lib})
                        add_library(Qwen3Asr::${lib} STATIC IMPORTED GLOBAL)
                    endif()
                    set_target_properties(Qwen3Asr::${lib} PROPERTIES
                        IMPORTED_LOCATION_${config} "${lib_path}"
                    )
                    list(APPEND QWEN3_LIBS Qwen3Asr::${lib})
                endif()
            endforeach()
            
            # Link main target to all components and also GGML
            set_property(TARGET Qwen3Asr::Qwen3Asr APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "$<$<CONFIG:${config}>:${QWEN3_LIBS};Ggml::Ggml>"
            )
        endif()
    endforeach()
endif()
"""

    find_openssl = """# FindOpenSSL.cmake
# Custom FindOpenSSL module to handle local precompiled OpenSSL on Windows NuGet, and system OpenSSL on other platforms.

if(WIN32)
    # Local precompiled OpenSSL for Windows
    set(OPENSSL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")
    set(OPENSSL_FOUND TRUE)
    
    if(NOT TARGET OpenSSL::SSL)
        add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
        set_target_properties(OpenSSL::SSL PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        )
        
        # Debug config
        set(SSL_LIB_DEBUG "${CMAKE_CURRENT_LIST_DIR}/../lib/openssl/windows/Debug/libssl.lib")
        if(EXISTS "${SSL_LIB_DEBUG}")
            set_property(TARGET OpenSSL::SSL APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(OpenSSL::SSL PROPERTIES
                IMPORTED_LOCATION_DEBUG "${SSL_LIB_DEBUG}"
            )
        endif()
        
        # Release config
        set(SSL_LIB_RELEASE "${CMAKE_CURRENT_LIST_DIR}/../lib/openssl/windows/Release/libssl.lib")
        if(EXISTS "${SSL_LIB_RELEASE}")
            set_property(TARGET OpenSSL::SSL APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(OpenSSL::SSL PROPERTIES
                IMPORTED_LOCATION_RELEASE "${SSL_LIB_RELEASE}"
            )
        endif()
    endif()

    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        )
        
        # Debug config
        set(CRYPTO_LIB_DEBUG "${CMAKE_CURRENT_LIST_DIR}/../lib/openssl/windows/Debug/libcrypto.lib")
        if(EXISTS "${CRYPTO_LIB_DEBUG}")
            set_property(TARGET OpenSSL::Crypto APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(OpenSSL::Crypto PROPERTIES
                IMPORTED_LOCATION_DEBUG "${CRYPTO_LIB_DEBUG}"
            )
        endif()
        
        # Release config
        set(CRYPTO_LIB_RELEASE "${CMAKE_CURRENT_LIST_DIR}/../lib/openssl/windows/Release/libcrypto.lib")
        if(EXISTS "${CRYPTO_LIB_RELEASE}")
            set_property(TARGET OpenSSL::Crypto APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(OpenSSL::Crypto PROPERTIES
                IMPORTED_LOCATION_RELEASE "${CRYPTO_LIB_RELEASE}"
            )
        endif()
    endif()

    # Link to socket library for Windows
    set_property(TARGET OpenSSL::SSL APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
    
else()
    # For non-Windows platforms, bypass this custom module path and use the built-in system FindOpenSSL.cmake
    # Save current module path
    set(_OLD_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
    # Clear module path so find_package searches system path
    set(CMAKE_MODULE_PATH "")
    
    # Delegate to standard FindOpenSSL
    find_package(OpenSSL ${OpenSSL_FIND_VERSION} REQUIRED)
    
    # Restore module path
    set(CMAKE_MODULE_PATH ${_OLD_CMAKE_MODULE_PATH})
endif()
"""

    find_cpphttplib = """# FindCppHttplib.cmake
# Defines target: CppHttplib::CppHttplib

set(CPPM_HTTPLIB_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")

if(NOT TARGET CppHttplib::CppHttplib)
    add_library(CppHttplib::CppHttplib INTERFACE IMPORTED GLOBAL)
    set_target_properties(CppHttplib::CppHttplib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CPPM_HTTPLIB_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "CPPHTTPLIB_OPENSSL_SUPPORT"
    )
    
    # Depend on OpenSSL
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        find_package(OpenSSL REQUIRED)
    endif()
    
    set_target_properties(CppHttplib::CppHttplib PROPERTIES
        INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto"
    )
endif()
"""

    find_json = """# FindNlohmannJson.cmake
# Defines target: nlohmann_json::nlohmann_json

set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")

if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NLOHMANN_JSON_INCLUDE_DIR}"
    )
endif()
"""

    with open(os.path.join(cmake_dir, "FindWebRtcAudioProcessing.cmake"), "w", encoding="utf-8") as f:
        f.write(find_webrtc)
    with open(os.path.join(cmake_dir, "FindGgml.cmake"), "w", encoding="utf-8") as f:
        f.write(find_ggml)
    with open(os.path.join(cmake_dir, "FindEspeakNg.cmake"), "w", encoding="utf-8") as f:
        f.write(find_espeak)
    with open(os.path.join(cmake_dir, "FindKokoro.cmake"), "w", encoding="utf-8") as f:
        f.write(find_kokoro)
    with open(os.path.join(cmake_dir, "FindQwen3Asr.cmake"), "w", encoding="utf-8") as f:
        f.write(find_qwen3)
    with open(os.path.join(cmake_dir, "FindOpenSSL.cmake"), "w", encoding="utf-8") as f:
        f.write(find_openssl)
    with open(os.path.join(cmake_dir, "FindCppHttplib.cmake"), "w", encoding="utf-8") as f:
        f.write(find_cpphttplib)
    with open(os.path.join(cmake_dir, "FindNlohmannJson.cmake"), "w", encoding="utf-8") as f:
        f.write(find_json)
    print(f"CMake Find modules generated in {cmake_dir}")

def main():
    parser = argparse.ArgumentParser(description="Build WebRTC APM Static Library")
    parser.add_argument("--platform", choices=["windows", "linux", "osx", "ios", "android", "current", "all"], default="current",
                        help="Platform to build for (current: detects host OS, all: builds all possible on this host)")
    parser.add_argument("--ndk", help="Android NDK directory path (required for Android builds)")
    
    args = parser.parse_args()
    
    deps_dir = os.path.dirname(os.path.abspath(__file__))
    dist_dir = os.path.join(os.path.dirname(deps_dir), "libraries")
    os.makedirs(dist_dir, exist_ok=True)
    
    host = detect_host_platform()
    target = args.platform
    
    if target == "current":
        target = host
        
    print(f"Host platform: {host}")
    print(f"Target platform: {target}")
    
    # Run build tasks based on target
    if target == "windows":
        if host != "windows":
            print("Error: Cannot build Windows binaries on non-Windows host.")
            sys.exit(1)
        build_windows(deps_dir, dist_dir)
    elif target == "linux":
        if host != "linux":
            print("Warning: Cross-compiling for Linux from non-Linux host may fail depending on toolchains.")
        build_linux(deps_dir, dist_dir)
    elif target == "osx":
        if host != "osx":
            print("Error: Cannot build OSX binaries on non-macOS host.")
            sys.exit(1)
        build_osx(deps_dir, dist_dir)
    elif target == "ios":
        if host != "osx":
            print("Error: Cannot build iOS binaries on non-macOS host.")
            sys.exit(1)
        build_ios(deps_dir, dist_dir)
    elif target == "android":
        build_android(deps_dir, dist_dir, args.ndk)
    elif target == "all":
        # Build all that are natively compilable on host
        if host == "windows":
            build_windows(deps_dir, dist_dir)
            # Try Android if NDK is available
            ndk = args.ndk or os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
            if ndk:
                build_android(deps_dir, dist_dir, ndk)
        elif host == "linux":
            build_linux(deps_dir, dist_dir)
            # Try Android if NDK is available
            ndk = args.ndk or os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
            if ndk:
                build_android(deps_dir, dist_dir, ndk)
        elif host == "osx":
            build_osx(deps_dir, dist_dir)
            build_ios(deps_dir, dist_dir)
            # Try Android if NDK is available
            ndk = args.ndk or os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
            if ndk:
                build_android(deps_dir, dist_dir, ndk)
                
    # Copy header files
    copy_headers(deps_dir, dist_dir)
    copy_ggml_headers(deps_dir, dist_dir)
    copy_espeak_headers(deps_dir, dist_dir)
    copy_kokoro_headers(deps_dir, dist_dir)
    copy_qwen3_headers(deps_dir, dist_dir)
    copy_cpp_httplib_headers(deps_dir, dist_dir)
    copy_nlohmann_json_headers(deps_dir, dist_dir)
    
    # Generate CMake import configurations
    generate_cmake_imports(dist_dir)
    print("\nAll dependencies built and packaging complete!")

if __name__ == "__main__":
    main()
