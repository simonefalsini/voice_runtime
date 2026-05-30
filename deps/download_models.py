#!/usr/bin/env python3
"""
download_models.py – Download voice-runtime model files and dependencies.

Pure Python (stdlib only), cross-platform (macOS / Linux / Windows).

Usage examples
  python deps/download_models.py --target all
  python deps/download_models.py --target stt-small --hf-token hf_…
  python deps/download_models.py --target tts --output-dir /tmp/models
  python deps/download_models.py --target deps
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Model catalogue
# ---------------------------------------------------------------------------

# Each entry: (relative_path_under_output, url, expected_sha256_or_None, expected_size_or_None)
# SHA-256 values are set to None for files whose checksums are not pinned yet;
# once known they can be filled in and the script will verify automatically.

_MODELS: Dict[str, List[Tuple[str, str, Optional[str], Optional[int]]]] = {
    "stt-small": [
        (
            "stt/Qwen3-ASR-0.6B-Q8_0.gguf",
            "https://huggingface.co/OpenVoiceOS/qwen3-asr-0.6b-q8-0/resolve/main/qwen3-asr-0.6b-q8_0.gguf",
            None,
            None,
        ),
    ],
    "stt-large": [
        (
            "stt/Qwen3-ASR-1.7B-Q8_0.gguf",
            "https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main/Qwen3-ASR-1.7B-Q8_0.gguf",
            None,
            None,
        ),
        (
            "stt/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf",
            "https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf",
            None,
            None,
        ),
    ],
    "tts": [
        (
            "tts/kokoro-v1.1-zh.onnx",
            "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.1/kokoro-v1.1-zh.onnx",
            None,
            None,
        ),
    ],
    "tts-voices": [
        (
            "tts/voices-v1.0.bin",
            "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin",
            None,
            None,
        ),
    ],
    "vad": [
        (
            "vad/ggml-silero-v6.2.0.bin",
            "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin",
            None,
            None,
        ),
    ],
}

# Header-only C++ dependencies – downloaded into libraries/include/
# Each entry: (relative_path_under_libraries/include, url, expected_sha256_or_None, expected_size_or_None)
_DEPS: Dict[str, List[Tuple[str, str, Optional[str], Optional[int]]]] = {
    "deps": [
        (
            "httplib.h",
            "https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h",
            None,
            None,
        ),
        (
            "nlohmann/json.hpp",
            "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp",
            None,
            None,
        ),
    ],
}

# Pseudo-targets that don't have downloads but create directories / print info.
_PSEUDO_TARGETS = {"voices", "espeak-data"}

# All real download targets (models + deps).
_ALL_REAL_TARGETS = list(_MODELS.keys()) + list(_DEPS.keys())

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _project_root() -> Path:
    """Return the voice_runtime project root (parent of deps/)."""
    return Path(__file__).resolve().parent.parent


def _default_output_dir() -> Path:
    return _project_root() / "models"


def _human_size(nbytes: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if nbytes < 1024:
            return f"{nbytes:.1f} {unit}"
        nbytes /= 1024  # type: ignore[assignment]
    return f"{nbytes:.1f} TiB"


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _progress_hook(block_num: int, block_size: int, total_size: int) -> None:
    downloaded = block_num * block_size
    if total_size > 0:
        pct = min(downloaded / total_size * 100, 100.0)
        bar_len = 40
        filled = int(bar_len * pct / 100)
        bar = "█" * filled + "░" * (bar_len - filled)
        sys.stdout.write(
            f"\r  [{bar}] {pct:5.1f}%  {_human_size(downloaded)} / {_human_size(total_size)}  "
        )
    else:
        sys.stdout.write(f"\r  downloaded {_human_size(downloaded)}  ")
    sys.stdout.flush()


def _download_file(
    url: str,
    dest: Path,
    *,
    expected_sha256: str | None = None,
    expected_size: int | None = None,
    hf_token: str | None = None,
) -> bool:
    """Download *url* to *dest*. Returns True on success."""

    # Skip if already present with correct size.
    if dest.exists():
        if expected_size is not None and dest.stat().st_size == expected_size:
            print(f"  ✓ Already exists (size matches): {dest.name}")
            return True
        elif expected_size is None and dest.stat().st_size > 0:
            print(f"  ✓ Already exists: {dest.name}")
            return True

    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")

    # Build a request so we can add auth headers for HuggingFace.
    req = urllib.request.Request(url)
    if hf_token and "huggingface.co" in url:
        req.add_header("Authorization", f"Bearer {hf_token}")

    print(f"  ↓ Downloading {dest.name}")
    print(f"    {url}")

    try:
        if hf_token and "huggingface.co" in url:
            _download_with_headers(req, part)
        else:
            urllib.request.urlretrieve(url, str(part), reporthook=_progress_hook)
    except urllib.error.HTTPError as exc:
        print(f"\n  ✗ HTTP {exc.code}: {exc.reason}")
        if exc.code == 401 and "huggingface.co" in url:
            print("    → This model may require a HuggingFace token (--hf-token).")
        part.unlink(missing_ok=True)
        return False
    except Exception as exc:
        print(f"\n  ✗ Download failed: {exc}")
        part.unlink(missing_ok=True)
        return False

    print()  # newline after progress bar

    # Verify SHA-256 if provided.
    if expected_sha256:
        print(f"  Verifying SHA-256 …", end=" ")
        actual = _sha256_file(part)
        if actual != expected_sha256:
            print(f"MISMATCH")
            print(f"    expected: {expected_sha256}")
            print(f"    got:      {actual}")
            part.unlink(missing_ok=True)
            return False
        print("OK")

    # Verify size if provided.
    if expected_size is not None:
        actual_size = part.stat().st_size
        if actual_size != expected_size:
            print(f"  ✗ Size mismatch: expected {expected_size}, got {actual_size}")
            part.unlink(missing_ok=True)
            return False

    part.rename(dest)
    print(f"  ✓ Saved: {dest}")
    return True


def _download_with_headers(req: urllib.request.Request, dest: Path) -> None:
    """Download using urllib.request.urlopen (supports custom headers)."""
    with urllib.request.urlopen(req) as resp:
        total = int(resp.headers.get("Content-Length", 0))
        downloaded = 0
        block_size = 1 << 16  # 64 KiB
        with open(dest, "wb") as f:
            while True:
                chunk = resp.read(block_size)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                _progress_hook(downloaded // block_size, block_size, total)


# ---------------------------------------------------------------------------
# Target handlers
# ---------------------------------------------------------------------------

def _handle_downloads(
    targets: list[str],
    output_dir: Path,
    hf_token: str | None,
) -> bool:
    ok = True
    for target in targets:
        entries = _MODELS.get(target, [])
        if not entries:
            continue
        print(f"\n{'=' * 60}")
        print(f"  Target: {target}")
        print(f"{'=' * 60}")
        for rel_path, url, sha256, size in entries:
            dest = output_dir / rel_path
            if not _download_file(
                url, dest, expected_sha256=sha256, expected_size=size, hf_token=hf_token
            ):
                ok = False
    return ok


def _handle_deps(
    targets: list[str],
    hf_token: str | None,
) -> bool:
    """Download header-only C++ dependencies into libraries/include/."""
    ok = True
    deps_dir = _project_root() / "libraries" / "include"
    for target in targets:
        entries = _DEPS.get(target, [])
        if not entries:
            continue
        print(f"\n{'=' * 60}")
        print(f"  Target: {target} (header-only deps → libraries/include/)")
        print(f"{'=' * 60}")
        for rel_path, url, sha256, size in entries:
            dest = deps_dir / rel_path
            if not _download_file(
                url, dest, expected_sha256=sha256, expected_size=size, hf_token=hf_token
            ):
                ok = False
        # Also create a convenience copy of json.hpp at top level
        nlohmann_src = deps_dir / "nlohmann" / "json.hpp"
        flat_copy = deps_dir / "json.hpp"
        if nlohmann_src.exists() and not flat_copy.exists():
            import shutil
            shutil.copy2(str(nlohmann_src), str(flat_copy))
            print(f"  ✓ Copied json.hpp → {flat_copy}")
    return ok


def _handle_voices(output_dir: Path) -> None:
    print(f"\n{'=' * 60}")
    print(f"  Target: voices")
    print(f"{'=' * 60}")
    tts_dir = output_dir / "tts"
    tts_dir.mkdir(parents=True, exist_ok=True)
    print(
        "  Voice vectors are packaged by the companion script:\n"
        "\n"
        "    python deps/package_voices.py --output-dir "
        + str(output_dir)
        + "\n"
        "\n"
        "  Run it after downloading the TTS model to create models/tts/voices.bin."
    )


def _handle_espeak_data(output_dir: Path) -> None:
    print(f"\n{'=' * 60}")
    print(f"  Target: espeak-data")
    print(f"{'=' * 60}")
    espeak_dir = output_dir / "espeak-ng-data"
    espeak_dir.mkdir(parents=True, exist_ok=True)
    
    # Try to find compiled espeak-ng-data in deps build dirs first
    deps_dir = _project_root() / "deps"
    compiled_src: Optional[Path] = None
    for path in deps_dir.glob("build_espeak_*"):
        if path.is_dir():
            candidate = path / "espeak-ng-data"
            if candidate.is_dir() and (candidate / "phondata").exists():
                compiled_src = candidate
                break
                
    import shutil
    if compiled_src:
        print(f"  Found compiled espeak-ng-data at: {compiled_src}")
        print(f"  Copying to: {espeak_dir} …")
        shutil.copytree(str(compiled_src), str(espeak_dir), dirs_exist_ok=True)
        print("  ✓ espeak-ng-data populated successfully!")
    else:
        # Fallback to copy the repository's source espeak-ng-data folder (partial data)
        src_repo_data = deps_dir / "espeak-ng" / "espeak-ng-data"
        if src_repo_data.is_dir():
            print(f"  Compiled espeak-ng-data not found (run build_deps.py first).")
            print(f"  Copying source repository data (partial) from: {src_repo_data} …")
            shutil.copytree(str(src_repo_data), str(espeak_dir), dirs_exist_ok=True)
            print("  ⚠ Copied source repository data folder. Note that compiled phoneme and dict files are missing.")
        else:
            print("  ✗ Could not find espeak-ng data source under deps/espeak-ng/.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _resolve_hf_token(explicit: str | None) -> str | None:
    """Return a HuggingFace token from the CLI arg, env var, or local cache."""
    if explicit:
        return explicit
    env = os.environ.get("HF_TOKEN")
    if env:
        return env
    cache = Path.home() / ".cache" / "huggingface" / "token"
    if cache.is_file():
        token = cache.read_text().strip()
        if token:
            return token
    return None


def main() -> int:
    all_targets = _ALL_REAL_TARGETS + sorted(_PSEUDO_TARGETS)

    parser = argparse.ArgumentParser(
        description="Download voice-runtime model files.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python deps/download_models.py --target all\n"
            "  python deps/download_models.py --target stt-small tts vad\n"
            "  python deps/download_models.py --target stt-large --hf-token hf_…\n"
        ),
    )
    parser.add_argument(
        "--target",
        nargs="+",
        choices=all_targets + ["all"],
        default=["all"],
        help="Model targets to download (default: all)",
    )
    parser.add_argument(
        "--hf-token",
        metavar="TOKEN",
        default=None,
        help="HuggingFace API token for gated models (also reads HF_TOKEN env var)",
    )
    parser.add_argument(
        "--output-dir",
        metavar="DIR",
        type=Path,
        default=None,
        help="Output directory (default: models/ relative to project root)",
    )

    args = parser.parse_args()

    output_dir: Path = args.output_dir if args.output_dir else _default_output_dir()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    hf_token = _resolve_hf_token(args.hf_token)

    # Expand "all" target.
    targets: list[str] = []
    if "all" in args.target:
        targets = all_targets
    else:
        targets = args.target

    print(f"Output directory: {output_dir}")
    print(f"Targets: {', '.join(targets)}")
    if hf_token:
        print(f"HuggingFace token: {'*' * 8}…{hf_token[-4:]}")
    print()

    # Ensure base directories exist.
    for subdir in ("stt", "tts", "vad", "espeak-ng-data"):
        (output_dir / subdir).mkdir(parents=True, exist_ok=True)

    # Process download targets.
    download_targets = [t for t in targets if t in _MODELS]
    ok = _handle_downloads(download_targets, output_dir, hf_token)

    # Process deps targets.
    dep_targets = [t for t in targets if t in _DEPS]
    if dep_targets:
        if not _handle_deps(dep_targets, hf_token):
            ok = False

    # Process pseudo-targets.
    if "voices" in targets:
        _handle_voices(output_dir)
    if "espeak-data" in targets:
        _handle_espeak_data(output_dir)

    print(f"\n{'=' * 60}")
    if ok:
        print("  All downloads completed successfully!")
    else:
        print("  Some downloads failed. Check the output above.")
    print(f"{'=' * 60}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
