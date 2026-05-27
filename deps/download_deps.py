import os
import subprocess
import urllib.request
import base64

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

def main():
    deps_dir = os.path.dirname(os.path.abspath(__file__))
    
    # 1. Clone WebRTC if not exists (although it should exist in the workspace)
    webrtc_dir = os.path.join(deps_dir, "WebRTC")
    if not os.path.exists(webrtc_dir):
        print("Cloning WebRTC...")
        run_cmd(["git", "clone", "https://webrtc.googlesource.com/src", "WebRTC"], cwd=deps_dir)
    else:
        print("WebRTC directory already exists.")

    # Apply MSVC compatibility patch if available and not yet applied
    patch_file = os.path.join(deps_dir, "webrtc_msvc_compat.patch")
    if os.path.exists(patch_file):
        print("Checking/Applying MSVC compatibility patch...")
        try:
            # Check if patch can be applied cleanly (means it is not yet applied)
            res = subprocess.run(["git", "apply", "--check", patch_file], cwd=webrtc_dir, capture_output=True)
            if res.returncode == 0:
                print("Applying patch...")
                run_cmd(["git", "apply", patch_file], cwd=webrtc_dir)
            else:
                # Check if it is already applied
                res_rev = subprocess.run(["git", "apply", "--reverse", "--check", patch_file], cwd=webrtc_dir, capture_output=True)
                if res_rev.returncode == 0:
                    print("Patch is already applied.")
                else:
                    print("Warning: Patch cannot be applied and does not seem to be already applied (conflict?).")
        except Exception as e:
            print(f"Error checking/applying patch: {e}")

    # 2. Clone Abseil-cpp
    abseil_dir = os.path.join(deps_dir, "abseil-cpp")
    if not os.path.exists(abseil_dir):
        print("Cloning Abseil-cpp...")
        run_cmd(["git", "clone", "https://github.com/abseil/abseil-cpp.git", "abseil-cpp"], cwd=deps_dir)
    else:
        print("Abseil-cpp directory already exists.")

    # 3. Download rnnoise files
    rnnoise_src_dir = os.path.join(webrtc_dir, "third_party", "rnnoise", "src")
    rnnoise_files = [
        ("rnn_activations.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_activations.h?format=TEXT"),
        ("rnn_vad_weights.cc", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_vad_weights.cc?format=TEXT"),
        ("rnn_vad_weights.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rnnoise/src/rnn_vad_weights.h?format=TEXT")
    ]
    for filename, url in rnnoise_files:
        download_gerrit_file(url, os.path.join(rnnoise_src_dir, filename))

    # 4. Download pffft files
    pffft_src_dir = os.path.join(webrtc_dir, "third_party", "pffft", "src")
    pffft_files = [
        ("fftpack.c", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/fftpack.c?format=TEXT"),
        ("fftpack.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/fftpack.h?format=TEXT"),
        ("pffft.c", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/pffft.c?format=TEXT"),
        ("pffft.h", "https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/pffft/src/pffft.h?format=TEXT")
    ]
    for filename, url in pffft_files:
        download_gerrit_file(url, os.path.join(pffft_src_dir, filename))

    print("\nAll dependencies downloaded and cloned successfully!")

if __name__ == "__main__":
    main()
