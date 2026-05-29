#!/usr/bin/env python3
"""
package_voices.py – Download Kokoro voice vectors and package them into voices.bin.

Adapted from DS4's scripts/package_voices.py for voice_runtime.
Pure Python (stdlib only), cross-platform (macOS / Linux / Windows).

Binary format (VOIC v1):
  Magic:      4 bytes  b'VOIC'
  Version:    uint32   LE  (1)
  NumVoices:  uint32   LE
  Per voice:
    NameLen:  uint32   LE
    Name:     NameLen bytes  UTF-8
    Dim:      uint32   LE   (number of float32 elements)
    Data:     Dim * 4 bytes  raw float32 LE

Usage:
  python deps/package_voices.py
  python deps/package_voices.py --output-dir /path/to/models
  python deps/package_voices.py --voices af_heart am_adam zf_001
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Dict, Optional, Set, Tuple

# ---------------------------------------------------------------------------
# Voice catalogue
# ---------------------------------------------------------------------------

# HuggingFace Kokoro-82M ONNX repository (pinned commit for reproducibility).
HF_VOICES_BASE = (
    "https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX"
    "/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231/voices"
)

# German voice from a separate repo (NPZ archive containing martin.npy).
GERMAN_NPZ_URL = (
    "https://huggingface.co/huggingFresse/Kokoro-82M-ONNX-German-Martin"
    "/resolve/main/voices-martin.npz"
)

# Base voices binary – contains Chinese and additional English voices.
BASE_VOICES_URL = (
    "https://github.com/koth/kokoro.cpp/releases/download"
    "/voices_model_files/voices-v1.1-zh.bin"
)

# Voices extracted from the base binary (voices-v1.1-zh.bin).
BASE_VOICE_IDS = {"af_maple", "zf_001", "zm_009"}

# Voices downloaded individually as .bin from HuggingFace.
INDIVIDUAL_VOICES: dict[str, str] = {
    # English
    "af_heart": "af_heart.bin",
    "af_bella": "af_bella.bin",
    "am_adam": "am_adam.bin",
    # Italian
    "if_sara": "if_sara.bin",
    "im_nicola": "im_nicola.bin",
    # Spanish
    "ef_dora": "ef_dora.bin",
    "em_alex": "em_alex.bin",
    # French
    "ff_siwis": "ff_siwis.bin",
    # Portuguese
    "pf_dora": "pf_dora.bin",
    "pm_alex": "pm_alex.bin",
    # Japanese
    "jf_alpha": "jf_alpha.bin",
    "jm_kumo": "jm_kumo.bin",
    # Hindi
    "hf_alpha": "hf_alpha.bin",
    "hm_omega": "hm_omega.bin",
}

# German (extracted from NPZ, not a plain .bin).
GERMAN_VOICE_ID = "de_martin"

# Full list of the 18 supported voices.
ALL_VOICE_IDS = sorted(
    BASE_VOICE_IDS | set(INDIVIDUAL_VOICES.keys()) | {GERMAN_VOICE_ID}
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _default_output_dir() -> Path:
    return _project_root() / "models"


def _human_size(nbytes: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if nbytes < 1024:
            return f"{nbytes:.1f} {unit}"
        nbytes /= 1024  # type: ignore[assignment]
    return f"{nbytes:.1f} TiB"


def _progress_hook(block_num: int, block_size: int, total_size: int) -> None:
    downloaded = block_num * block_size
    if total_size > 0:
        pct = min(downloaded / total_size * 100, 100.0)
        bar_len = 30
        filled = int(bar_len * pct / 100)
        bar = "█" * filled + "░" * (bar_len - filled)
        sys.stdout.write(
            f"\r    [{bar}] {pct:5.1f}%  {_human_size(downloaded)} / {_human_size(total_size)}  "
        )
    else:
        sys.stdout.write(f"\r    downloaded {_human_size(downloaded)}  ")
    sys.stdout.flush()


def _download(url: str, dest: Path) -> bool:
    """Download *url* to *dest*, skipping if already present and non-empty."""
    if dest.exists() and dest.stat().st_size > 0:
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")
    try:
        urllib.request.urlretrieve(url, str(part), reporthook=_progress_hook)
        print()  # newline after progress bar
        part.rename(dest)
        return True
    except Exception as exc:
        print(f"\n    ✗ Download failed: {exc}")
        part.unlink(missing_ok=True)
        return False


# ---------------------------------------------------------------------------
# VOIC binary format I/O
# ---------------------------------------------------------------------------

def parse_voices_bin(path: Path) -> dict[str, tuple[int, bytes]]:
    """Parse a VOIC-format binary file and return {name: (dim, raw_data)}."""
    voices: dict[str, tuple[int, bytes]] = {}
    if not path.exists():
        return voices

    print(f"  Parsing base voices from {path.name} …")
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"VOIC":
            print("  ⚠ Invalid magic header — skipping base file.")
            return voices
        _version = struct.unpack("<I", f.read(4))[0]
        num_voices = struct.unpack("<I", f.read(4))[0]

        for _ in range(num_voices):
            name_len = struct.unpack("<I", f.read(4))[0]
            name = f.read(name_len).decode("utf-8")
            dim = struct.unpack("<I", f.read(4))[0]
            style_data = f.read(dim * 4)
            voices[name] = (dim, style_data)

    print(f"  Loaded {len(voices)} voices from base file.")
    return voices


def write_voices_bin(
    path: Path, voices: dict[str, tuple[int, bytes]], *, version: int = 1
) -> None:
    """Write voices to a VOIC-format binary file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VOIC")
        f.write(struct.pack("<I", version))
        f.write(struct.pack("<I", len(voices)))
        for name in sorted(voices.keys()):
            dim, style_data = voices[name]
            name_bytes = name.encode("utf-8")
            f.write(struct.pack("<I", len(name_bytes)))
            f.write(name_bytes)
            f.write(struct.pack("<I", dim))
            f.write(style_data)


# ---------------------------------------------------------------------------
# Voice loaders
# ---------------------------------------------------------------------------

def _load_raw_voice_bin(path: Path) -> tuple[int, bytes] | None:
    """Load a raw float32 voice vector from a plain .bin file."""
    if not path.exists() or path.stat().st_size == 0:
        return None
    data = path.read_bytes()
    dim = len(data) // 4
    return (dim, data)


def _extract_german_voice_from_npz(npz_path: Path) -> tuple[int, bytes] | None:
    """Extract the German 'martin' voice from the NPZ archive."""
    if not npz_path.exists():
        return None
    try:
        with zipfile.ZipFile(npz_path, "r") as z:
            data = z.read("martin.npy")
            # Minimal NPY v1.0 parser: magic(6) + minor(1) + major(1) + header_len(2) + header
            if not data.startswith(b"\x93NUMPY"):
                print("    ⚠ martin.npy is not a valid NPY file.")
                return None
            header_len = struct.unpack("<H", data[8:10])[0]
            raw_data = data[10 + header_len :]
            dim = len(raw_data) // 4
            return (dim, raw_data)
    except Exception as exc:
        print(f"    ✗ Failed to extract German voice: {exc}")
        return None


# ---------------------------------------------------------------------------
# Main packaging logic
# ---------------------------------------------------------------------------

def package_voices(
    output_dir: Path,
    voice_filter: set[str] | None = None,
) -> bool:
    """Download voice vectors and package them into voices.bin.

    Args:
        output_dir: Root models directory (contains tts/ subdirectory).
        voice_filter: If set, only include these voice IDs. None means all.

    Returns:
        True on success.
    """
    tts_dir = output_dir / "tts"
    tts_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = tts_dir / ".voice_tmp"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    final_bin = tts_dir / "voices.bin"

    wanted = voice_filter if voice_filter else set(ALL_VOICE_IDS)
    voices: dict[str, tuple[int, bytes]] = {}

    # --- 1. Base voices from voices-v1.1-zh.bin (af_maple, zf_001, zm_009) ---
    needed_base = wanted & BASE_VOICE_IDS
    if needed_base:
        print("\n  Step 1: Base voices (Chinese + English)")
        base_bin = tmp_dir / "voices-v1.1-zh.bin"
        if not base_bin.exists() or base_bin.stat().st_size == 0:
            print(f"    Downloading voices-v1.1-zh.bin …")
            if not _download(BASE_VOICES_URL, base_bin):
                print("    ✗ Could not download base voices file.")
            else:
                print(f"    ✓ Downloaded ({_human_size(base_bin.stat().st_size)})")

        all_base = parse_voices_bin(base_bin)
        for vid in needed_base:
            if vid in all_base:
                voices[vid] = all_base[vid]
                print(f"    ✓ {vid} (from base)")
            else:
                print(f"    ⚠ {vid} not found in base file")

    # --- 2. Individual HuggingFace voices ---
    needed_individual = wanted & set(INDIVIDUAL_VOICES.keys())
    if needed_individual:
        print("\n  Step 2: Individual voices from HuggingFace")
        for vid in sorted(needed_individual):
            if vid in voices:
                continue
            filename = INDIVIDUAL_VOICES[vid]
            local = tmp_dir / filename
            url = f"{HF_VOICES_BASE}/{filename}"
            print(f"    Downloading {vid} …")
            if _download(url, local):
                result = _load_raw_voice_bin(local)
                if result:
                    voices[vid] = result
                    print(f"    ✓ {vid} (dim={result[0]})")
                else:
                    print(f"    ⚠ {vid}: empty or unreadable")
            else:
                print(f"    ✗ {vid}: download failed")

    # --- 3. German voice ---
    if GERMAN_VOICE_ID in wanted:
        print("\n  Step 3: German voice (de_martin)")
        npz_local = tmp_dir / "voices-martin.npz"
        if not npz_local.exists() or npz_local.stat().st_size == 0:
            print("    Downloading voices-martin.npz …")
            _download(GERMAN_NPZ_URL, npz_local)

        result = _extract_german_voice_from_npz(npz_local)
        if result:
            voices[GERMAN_VOICE_ID] = result
            print(f"    ✓ {GERMAN_VOICE_ID} (dim={result[0]})")
        else:
            print(f"    ⚠ {GERMAN_VOICE_ID}: extraction failed")

    # --- 4. Write combined voices.bin ---
    if not voices:
        print("\n  ✗ No voices were loaded — voices.bin not written.")
        return False

    print(f"\n  Writing {final_bin.name} with {len(voices)} voices …")
    write_voices_bin(final_bin, voices)
    size = final_bin.stat().st_size
    print(f"  ✓ Saved: {final_bin}  ({_human_size(size)})")
    print(f"\n  Voices included ({len(voices)}):")
    for name in sorted(voices.keys()):
        dim, _ = voices[name]
        print(f"    • {name:15s}  dim={dim}")

    return True


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download Kokoro voice vectors and package into voices.bin.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python deps/package_voices.py\n"
            "  python deps/package_voices.py --output-dir /tmp/models\n"
            "  python deps/package_voices.py --voices af_heart am_adam de_martin\n"
            "\n"
            f"All {len(ALL_VOICE_IDS)} supported voices:\n"
            f"  {', '.join(ALL_VOICE_IDS)}\n"
        ),
    )
    parser.add_argument(
        "--output-dir",
        metavar="DIR",
        type=Path,
        default=None,
        help="Output directory (default: models/ relative to project root)",
    )
    parser.add_argument(
        "--voices",
        nargs="+",
        choices=ALL_VOICE_IDS,
        default=None,
        help="Subset of voices to include (default: all 18)",
    )

    args = parser.parse_args()
    output_dir: Path = args.output_dir if args.output_dir else _default_output_dir()
    output_dir = output_dir.resolve()

    voice_filter: set[str] | None = set(args.voices) if args.voices else None

    print("=" * 60)
    print("  Kokoro Voice Packager")
    print("=" * 60)
    print(f"  Output directory: {output_dir}")
    if voice_filter:
        print(f"  Voices: {', '.join(sorted(voice_filter))}")
    else:
        print(f"  Voices: all {len(ALL_VOICE_IDS)}")

    ok = package_voices(output_dir, voice_filter)

    print()
    print("=" * 60)
    if ok:
        print("  Voice packaging complete!")
    else:
        print("  Voice packaging failed. Check the output above.")
    print("=" * 60)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
