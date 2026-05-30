import os
import sys
import subprocess
import urllib.request
import base64
import json
import zipfile

def run_cmd(cmd, cwd=None):
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)

def download_gerrit_file(url, output_path):
    print(f"Downloading {url} -> {output_path}")
    try:
        req = urllib.request.urlopen(url)
        content_b64 = req.read()
        content = base64.b64decode(content_b64)
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, 'wb') as f:
            f.write(content)
        print(f"Successfully downloaded {output_path}")
    except Exception as e:
        print(f"Error downloading {url}: {e}")
        raise

def download_and_extract_onnx(deps_dir, dist_dir):
    print("--- Downloading and Installing ONNX Runtime 1.24.1 ---")
    onnx_root = os.path.join(dist_dir, "onnx") # libraries/onnx/
    
    # Check if already installed
    check_file = None
    if sys.platform.startswith('win'):
        check_file = os.path.join(onnx_root, "windows", "Release", "lib", "onnxruntime.lib")
    elif sys.platform.startswith('darwin'):
        check_file = os.path.join(onnx_root, "osx", "Release", "lib", "libonnxruntime.dylib")
    else:
        check_file = os.path.join(onnx_root, "linux", "Release", "lib", "libonnxruntime.so")
        
    if check_file and os.path.exists(check_file):
        print("ONNX Runtime 1.24.1 is already installed.")
        return

    tmp_dir = os.path.join(deps_dir, "build_onnx_tmp")
    if os.path.exists(tmp_dir):
        shutil_rmtree_safe(tmp_dir)
    os.makedirs(tmp_dir, exist_ok=True)
    
    nupkg_url = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/1.24.1"
    nupkg_path = os.path.join(tmp_dir, "onnxruntime.zip")
    
    print(f"Downloading ONNX Runtime NuGet from {nupkg_url}...")
    try:
        urllib.request.urlretrieve(nupkg_url, nupkg_path)
    except Exception as e:
        print(f"Failed to download ONNX Runtime from NuGet: {e}")
        return
        
    print("Extracting NuGet package...")
    with zipfile.ZipFile(nupkg_path, 'r') as zip_ref:
        zip_ref.extractall(tmp_dir)
        
    def copy_headers_to(dest_inc):
        src_headers = os.path.join(tmp_dir, "build", "native", "include")
        if os.path.exists(src_headers):
            os.makedirs(dest_inc, exist_ok=True)
            for root, dirs, files in os.walk(src_headers):
                for file in files:
                    src_file = os.path.join(root, file)
                    rel = os.path.relpath(src_file, src_headers)
                    dest_file = os.path.join(dest_inc, rel)
                    os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                    import shutil
                    shutil.copy2(src_file, dest_file)

    import shutil
    # 1. Windows x64
    win_src = os.path.join(tmp_dir, "runtimes", "win-x64", "native")
    if os.path.exists(win_src):
        for config in ["Release", "Debug"]:
            platform_dir = os.path.join(onnx_root, "windows", config)
            copy_headers_to(os.path.join(platform_dir, "include"))
            dest_lib = os.path.join(platform_dir, "lib")
            os.makedirs(dest_lib, exist_ok=True)
            for file in os.listdir(win_src):
                if file.endswith('.lib') or file.endswith('.dll') or file.endswith('.pdb'):
                    shutil.copy2(os.path.join(win_src, file), os.path.join(dest_lib, file))
                    
    # 2. Linux x64
    linux_src = os.path.join(tmp_dir, "runtimes", "linux-x64", "native")
    if os.path.exists(linux_src):
        for config in ["Release", "Debug"]:
            platform_dir = os.path.join(onnx_root, "linux", config)
            copy_headers_to(os.path.join(platform_dir, "include"))
            dest_lib = os.path.join(platform_dir, "lib")
            os.makedirs(dest_lib, exist_ok=True)
            for file in os.listdir(linux_src):
                if file.endswith('.so'):
                    shutil.copy2(os.path.join(linux_src, file), os.path.join(dest_lib, file))
                    
    # 3. macOS (OSX)
    osx_src = os.path.join(tmp_dir, "runtimes", "osx", "native")
    if not os.path.exists(osx_src):
        osx_src = os.path.join(tmp_dir, "runtimes", "osx-arm64", "native")
    if os.path.exists(osx_src):
        for config in ["Release", "Debug"]:
            platform_dir = os.path.join(onnx_root, "osx", config)
            copy_headers_to(os.path.join(platform_dir, "include"))
            dest_lib = os.path.join(platform_dir, "lib")
            os.makedirs(dest_lib, exist_ok=True)
            for file in os.listdir(osx_src):
                if file.endswith('.dylib'):
                    shutil.copy2(os.path.join(osx_src, file), os.path.join(dest_lib, file))
                    
    # 4. iOS
    ios_zip_path = os.path.join(tmp_dir, "runtimes", "ios", "native", "onnxruntime.xcframework.zip")
    if os.path.exists(ios_zip_path):
        ios_extract_dir = os.path.join(tmp_dir, "runtimes", "ios", "native", "extracted")
        with zipfile.ZipFile(ios_zip_path, 'r') as zip_ref:
            zip_ref.extractall(ios_extract_dir)
        for config in ["Release", "Debug"]:
            platform_dir = os.path.join(onnx_root, "ios", config)
            copy_headers_to(os.path.join(platform_dir, "include"))
            dest_lib = os.path.join(platform_dir, "lib")
            os.makedirs(dest_lib, exist_ok=True)
            shutil.copytree(os.path.join(ios_extract_dir, "onnxruntime.xcframework"), 
                            os.path.join(dest_lib, "onnxruntime.xcframework"), dirs_exist_ok=True)
                            
    # 5. Android
    aar_path = os.path.join(tmp_dir, "runtimes", "android", "native", "onnxruntime.aar")
    if os.path.exists(aar_path):
        android_extract_dir = os.path.join(tmp_dir, "runtimes", "android", "native", "extracted")
        with zipfile.ZipFile(aar_path, 'r') as zip_ref:
            zip_ref.extractall(android_extract_dir)
        jni_dir = os.path.join(android_extract_dir, "jni")
        if os.path.exists(jni_dir):
            for abi in os.listdir(jni_dir):
                abi_src = os.path.join(jni_dir, abi)
                if os.path.isdir(abi_src):
                    for config in ["Release", "Debug"]:
                        platform_dir = os.path.join(onnx_root, "android", abi, config)
                        copy_headers_to(os.path.join(platform_dir, "include"))
                        dest_lib = os.path.join(platform_dir, "lib")
                        os.makedirs(dest_lib, exist_ok=True)
                        for file in os.listdir(abi_src):
                            if file.endswith('.so'):
                                shutil.copy2(os.path.join(abi_src, file), os.path.join(dest_lib, file))
                                
    # Clean up
    shutil_rmtree_safe(tmp_dir)
    print("ONNX Runtime 1.24.1 installation complete!")

def shutil_rmtree_safe(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)

def download_and_extract_openssl(deps_dir, dist_dir):
    print("--- Downloading and Installing OpenSSL 3.0.17 for Windows ---")
    openssl_root = os.path.join(dist_dir, "lib", "openssl", "windows")
    
    # Check if already installed
    check_file = os.path.join(openssl_root, "Release", "libssl.lib")
    if os.path.exists(check_file):
        print("OpenSSL 3.0.17 is already installed.")
        return

    tmp_dir = os.path.join(deps_dir, "build_openssl_tmp")
    if os.path.exists(tmp_dir):
        shutil_rmtree_safe(tmp_dir)
    os.makedirs(tmp_dir, exist_ok=True)
    
    nupkg_url = "https://www.nuget.org/api/v2/package/openssl-native/3.0.17"
    nupkg_path = os.path.join(tmp_dir, "openssl.zip")
    
    print(f"Downloading OpenSSL NuGet from {nupkg_url}...")
    try:
        urllib.request.urlretrieve(nupkg_url, nupkg_path)
    except Exception as e:
        print(f"Failed to download OpenSSL from NuGet: {e}")
        return
        
    print("Extracting OpenSSL NuGet package...")
    with zipfile.ZipFile(nupkg_path, 'r') as zip_ref:
        zip_ref.extractall(tmp_dir)
        
    # Copy headers to libraries/include/openssl
    src_headers = os.path.join(tmp_dir, "include", "openssl")
    dest_headers = os.path.join(dist_dir, "include", "openssl")
    if os.path.exists(src_headers):
        os.makedirs(dest_headers, exist_ok=True)
        import shutil
        shutil.copytree(src_headers, dest_headers, dirs_exist_ok=True)
        
    # Copy libraries to libraries/lib/openssl/windows/Release and Debug
    src_lib_dir = os.path.join(tmp_dir, "lib", "win-x64", "native")
    if os.path.exists(src_lib_dir):
        import glob
        for config in ["Release", "Debug"]:
            platform_dir = os.path.join(dist_dir, "lib", "openssl", "windows", config)
            os.makedirs(platform_dir, exist_ok=True)
            # Find and copy static libs robustly
            ssl_libs = glob.glob(os.path.join(src_lib_dir, "libssl_static_*.lib"))
            crypto_libs = glob.glob(os.path.join(src_lib_dir, "libcrypto_static_*.lib"))
            if ssl_libs and crypto_libs:
                ssl_lib = next((x for x in ssl_libs if "v143" in x), ssl_libs[0])
                crypto_lib = next((x for x in crypto_libs if "v143" in x), crypto_libs[0])
                shutil.copy2(ssl_lib, os.path.join(platform_dir, "libssl.lib"))
                shutil.copy2(crypto_lib, os.path.join(platform_dir, "libcrypto.lib"))
            
            # Copy DLLs
            src_dll_dir = os.path.join(tmp_dir, "runtimes", "win-x64", "native")
            if os.path.exists(src_dll_dir):
                shutil.copy2(os.path.join(src_dll_dir, "libssl-3-x64.dll"), os.path.join(platform_dir, "libssl-3-x64.dll"))
                shutil.copy2(os.path.join(src_dll_dir, "libcrypto-3-x64.dll"), os.path.join(platform_dir, "libcrypto-3-x64.dll"))

    # Clean up
    shutil_rmtree_safe(tmp_dir)
    print("OpenSSL 3.0.17 installation complete!")

def main():
    deps_dir = os.path.dirname(os.path.abspath(__file__))
    dist_dir = os.path.join(os.path.dirname(deps_dir), "libraries")
    
    # 1. List of dependency repositories
    dependencies = [
        {
            "name": "WebRTC",
            "url": "https://webrtc.googlesource.com/src"
        },
        {
            "name": "abseil-cpp",
            "url": "https://github.com/abseil/abseil-cpp.git"
        },
        {
            "name": "ggml",
            "url": "https://github.com/ggml-org/ggml.git"
        },
        {
            "name": "espeak-ng",
            "url": "https://github.com/espeak-ng/espeak-ng.git"
        },
        {
            "name": "kokoro.cpp",
            "url": "https://github.com/koth/kokoro.cpp"
        },
        {
            "name": "qwen3-asr.cpp",
            "url": "https://github.com/predict-woo/qwen3-asr.cpp"
        },
        {
            "name": "cpp-httplib",
            "url": "https://github.com/yhirose/cpp-httplib.git"
        },
        {
            "name": "json",
            "url": "https://github.com/nlohmann/json.git"
        }
    ]
    
    for dep in dependencies:
        name = dep["name"]
        default_url = dep["url"]
        
        # Check if patch metadata exists to get exact repo URL & commit hash
        metadata_file = os.path.join(deps_dir, "patches", name, "metadata.json")
        commit_hash = None
        repo_url = default_url
        
        if os.path.exists(metadata_file):
            try:
                with open(metadata_file, "r", encoding="utf-8") as f:
                    meta = json.load(f)
                    repo_url = meta.get("repository") or default_url
                    commit_hash = meta.get("commit")
                    print(f"Found patch metadata for {name}: {repo_url} @ {commit_hash}")
            except Exception as e:
                print(f"Error reading patch metadata for {name}: {e}")
                
        dep_path = os.path.join(deps_dir, name)
        if not os.path.exists(dep_path):
            print(f"Cloning {name} from {repo_url}...")
            run_cmd(["git", "clone", repo_url, name], cwd=deps_dir)
        else:
            print(f"Directory {name} already exists.")
            
        # Ensure we checkout the exact commit hash if specified in metadata
        if commit_hash:
            current_hash = subprocess.run(["git", "rev-parse", "HEAD"], cwd=dep_path, capture_output=True, text=True).stdout.strip()
            if current_hash != commit_hash:
                print(f"Checking out specific commit {commit_hash} for {name}...")
                # Fetch first in case the commit is not present locally
                subprocess.run(["git", "fetch", "origin"], cwd=dep_path)
                run_cmd(["git", "checkout", commit_hash], cwd=dep_path)
                
        # Check and apply combined patch if exists
        patch_file = os.path.join(deps_dir, "patches", name, "combined.patch")
        if os.path.exists(patch_file):
            print(f"Checking/Applying patch for {name}...")
            try:
                res = subprocess.run(["git", "apply", "--ignore-whitespace", "--check", patch_file], cwd=dep_path, capture_output=True)
                if res.returncode == 0:
                    print("Applying patch...")
                    run_cmd(["git", "apply", "--ignore-whitespace", patch_file], cwd=dep_path)
                else:
                    # Check if already applied
                    res_rev = subprocess.run(["git", "apply", "--ignore-whitespace", "--reverse", "--check", patch_file], cwd=dep_path, capture_output=True)
                    if res_rev.returncode == 0:
                        print("Patch is already applied.")
                    else:
                        print(f"Warning: Patch for {name} cannot be applied cleanly and does not seem already applied.")
            except Exception as e:
                print(f"Error applying patch for {name}: {e}")

    # 2. Download WebRTC third party files
    webrtc_dir = os.path.join(deps_dir, "WebRTC")
    rnnoise_src_dir = os.path.join(webrtc_dir, "third_party", "rnnoise", "src")
    rnnoise_files = [
        ("rnn_activations.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_activations.h?format=TEXT"),
        ("rnn_vad_weights.cc", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_vad_weights.cc?format=TEXT"),
        ("rnn_vad_weights.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_vad_weights.h?format=TEXT")
    ]
    for filename, url in rnnoise_files:
        download_gerrit_file(url, os.path.join(rnnoise_src_dir, filename))

    pffft_src_dir = os.path.join(webrtc_dir, "third_party", "pffft", "src")
    pffft_files = [
        ("fftpack.c", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/fftpack.c?format=TEXT"),
        ("fftpack.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/fftpack.h?format=TEXT"),
        ("pffft.c", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/pffft.c?format=TEXT"),
        ("pffft.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/pffft.h?format=TEXT")
    ]
    for filename, url in pffft_files:
        download_gerrit_file(url, os.path.join(pffft_src_dir, filename))

    # 2.5 Generate experiments/registered_field_trials.h for WebRTC
    if os.path.exists(webrtc_dir):
        print("Generating experiments/registered_field_trials.h...")
        try:
            import sys
            run_cmd([sys.executable, os.path.join("experiments", "field_trials.py"), "header", "--output", os.path.join("experiments", "registered_field_trials.h")], cwd=webrtc_dir)
        except Exception as e:
            print(f"Warning: Failed to generate registered_field_trials.h: {e}")

    # 3. Download ONNX Runtime
    download_and_extract_onnx(deps_dir, dist_dir)

    # 4. Download OpenSSL for Windows
    if sys.platform.startswith('win'):
        download_and_extract_openssl(deps_dir, dist_dir)

    print("\nAll dependencies downloaded, cloned, and patched successfully!")

if __name__ == "__main__":
    main()
