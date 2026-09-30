"""Original XWMA -> PCM using the installed XAudio2 2.0 codec.

All 180 fixture payloads are audited against source dpds counts. Never fall back
to FFmpeg: even its length-matching results differ from this native decoder.
No Microsoft DLL or encoded source data is bundled with the converter.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
import struct
import subprocess
import tempfile
from pathlib import Path

from .sounds import SoundError

SUPPORTED_DLL_SHA256 = "e435b73193bdf651f7ae564eba05266595ac672db45e0e22dce92d0bcb3c6513"


def validate(parts: dict[bytes, bytes]) -> tuple[int, int]:
    fmt, sizes, data = parts[b"fmt "], parts.get(b"dpds", b""), parts[b"data"]
    if len(fmt) < 18:
        raise SoundError("truncated XWMA format")
    tag, channels, rate, _, packet, bits, extra = struct.unpack_from("<HHIIHHH", fmt)
    if tag not in (0x161, 0x162) or channels not in (1, 2) or not rate or not packet or extra:
        raise SoundError("XWMA profile requires native implementation/verification")
    if not sizes or len(sizes) % 4 or len(data) % packet or len(sizes) // 4 != len(data) // packet:
        raise SoundError("inconsistent XWMA packet/decoded-size table")
    values = struct.unpack(f"<{len(sizes) // 4}I", sizes)
    if any(a > b for a, b in zip(values, values[1:])) or values[-1] % (channels * 2):
        raise SoundError("invalid XWMA decoded packet sizes")
    if values[-1] > 0x40000000:
        raise SoundError("XWMA decoded payload exceeds native bridge limit")
    return values[-1], channels


def find_helper(explicit: Path | None = None) -> Path:
    path = explicit or Path(__file__).resolve().parents[2] / "tools/bin/xaudio_wma_decoder.exe"
    if not path.is_file():
        raise SoundError("native XWMA bridge not built: build tools/xaudio_wma_decoder.cpp (x86) or use --xwma-decoder")
    return path.resolve()


def system_codec() -> Path:
    if os.name != "nt":
        raise SoundError("verified XWMA codec requires Windows XAudio2 2.0")
    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetWindowsDirectoryW(buffer, len(buffer))
    if not length or length >= len(buffer):
        raise SoundError("cannot locate the Windows audio codec")
    root = Path(buffer.value)
    return root / ("SysWOW64" if (root / "SysWOW64").is_dir() else "System32") / "XAudio2_0.dll"


def decode(source: Path, parts: dict[bytes, bytes], helper: Path | None = None) -> tuple[bytes, dict]:
    expected, channels = validate(parts)
    program = find_helper(helper)
    dll = system_codec()
    if not dll.is_file():
        raise SoundError("original XAudio2 2.0 codec is missing from Windows")
    digest = hashlib.sha256(dll.read_bytes()).hexdigest()
    if digest != SUPPORTED_DLL_SHA256:
        raise SoundError("installed XAudio2 2.0 version is not audited; refusing unverified internal codec ABI")
    with tempfile.TemporaryDirectory(prefix="waw2bo2_xwma_") as directory:
        destination = Path(directory) / "decoded.pcm"
        try:
            proc = subprocess.run([str(program), str(source.resolve()), str(destination)],
                                  capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired as exc:
            raise SoundError("native XWMA decoding timed out") from exc
        if proc.returncode or not destination.is_file():
            raise SoundError(f"native XWMA bridge failed ({proc.returncode}): {proc.stderr[:1000]}")
        try:
            info = json.loads(proc.stdout)
        except ValueError as exc:
            raise SoundError("invalid native XWMA decoder report") from exc
        pcm = destination.read_bytes()
    if len(pcm) != expected or info.get("decoded_bytes") != expected or info.get("dpds_bytes") != expected or info.get("input_remaining") != 0:
        raise SoundError("native XWMA sample count disagrees with original packet metadata")
    return pcm, {"decoder": "installed XAudio2_0 xWMA", "decoder_dll_sha256": digest,
                 "expected_pcm_bytes": expected, "decoded_pcm_bytes": len(pcm),
                 "native_input_callbacks": info.get("callbacks"), "channels": channels}
