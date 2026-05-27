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

def build_windows(deps_dir, dist_dir):
    print("--- Building WebRTC APM for Windows ---")
    build_dir = os.path.join(deps_dir, "build_windows")
    if os.path.exists(build_dir):
        print("Cleaning previous build directory...")
        shutil.rmtree(build_dir, ignore_errors=True)
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
        
    print("Windows build successful for all configurations!")

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
        
    print("Linux build successful for all configurations!")

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
        
    print("OSX build successful for all configurations!")

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
        
    print("iOS build successful for all configurations!")

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
            print(f"Building for Android ABI: {abi} ({config})...")
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
            
    print("Android build successful for all ABIs and configurations!")

def copy_headers(deps_dir, dist_dir):
    print("--- Exporting WebRTC APM Headers ---")
    webrtc_dir = os.path.join(deps_dir, "WebRTC")
    include_dest = os.path.join(dist_dir, "include", "webrtc_audio_processing")
    
    # We will copy all .h files from the requested parts of WebRTC
    subdirs_to_copy = [
        "api/audio",
        "api/environment",
        "api/task_queue",
        "api/units",
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
    print("\nWebRTC APM build and packaging complete!")

if __name__ == "__main__":
    main()
