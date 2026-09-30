"""Original RIFF audio -> canonical PCM for the fixed-header T6 bank writer.

No channel remixing, padding or invented samples. The only resampling is of
loaded sounds to the 48 kHz BO2 loaded banks require. Compressed decode
length disagreement is retained as a failure requiring a native decoder audit.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
from pathlib import Path

from .sounds import SoundError
from .weapons import output_name

T6_RATES = (8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000)


def chunks(data: bytes) -> tuple[bytes, dict[bytes, bytes]]:
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] not in (b"WAVE", b"XWMA"):
        raise SoundError("unsupported audio container; expected original RIFF WAVE/XWMA")
    riff_end = struct.unpack_from("<I", data, 4)[0] + 8
    if riff_end < 12 or riff_end > len(data):
        raise SoundError("RIFF byte count exceeds or invalidates the original container")
    offset, result = 12, {}
    while offset < riff_end:
        if offset + 8 > riff_end:
            raise SoundError("truncated RIFF chunk header")
        tag, size = struct.unpack_from("<4sI", data, offset)
        end = offset + 8 + size
        if end > riff_end or tag in result:
            raise SoundError("truncated or duplicate RIFF chunk")
        result[tag] = data[offset + 8:end]
        offset = end + (size & 1)
    if b"fmt " not in result or b"data" not in result:
        raise SoundError("missing RIFF format/audio data")
    return data[8:12], result


def canonical_pcm(pcm: bytes, channels: int, rate: int) -> bytes:
    if channels not in (1, 2) or rate not in T6_RATES:
        raise SoundError(f"T6 bank cannot preserve channels={channels}, sample rate={rate}")
    if len(pcm) % (channels * 2):
        raise SoundError("partial PCM frame")
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    return b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm


def find_decoder(explicit: Path | None = None) -> Path | None:
    if explicit:
        if not explicit.is_file():
            raise SoundError(f"missing requested audio decoder: {explicit}")
        return explicit.resolve()
    found = shutil.which("ffmpeg")
    if found:
        return Path(found)
    try:
        import imageio_ffmpeg
        return Path(imageio_ffmpeg.get_ffmpeg_exe())
    except (ImportError, RuntimeError):
        return None


def convert(source: Path, decoder: Path | None = None,
            xwma_decoder: Path | None = None) -> tuple[bytes | None, dict]:
    original = source.read_bytes()
    form, parts = chunks(original)
    fmt = parts[b"fmt "]
    if len(fmt) < 16:
        raise SoundError("truncated audio format")
    tag, channels, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt)
    report = {"source_sha256": hashlib.sha256(original).hexdigest(), "format": tag,
              "channels": channels, "sample_rate": rate,
              "source_chunks": {t.decode("ascii", errors="replace"): len(v) for t, v in parts.items()}}
    riff_end = struct.unpack_from("<I", original, 4)[0] + 8
    if riff_end < len(original):
        # Bytes outside the complete declared RIFF object are not a data chunk.
        # Preserve original file and record them, never turn them into samples.
        report.update(riff_trailing_bytes=len(original) - riff_end,
                      riff_trailing_sha256=hashlib.sha256(original[riff_end:]).hexdigest())
    if rate <= 0:
        raise SoundError("invalid source sample rate")
    # T6 bank stores a rate enum, not a Hz integer. Preserve every PCM sample
    # and express the rate difference as an alias playback-pitch correction.
    # Downstream bank generation MUST apply this factor to min/max pitch.
    target_rate = min(T6_RATES, key=lambda r: abs(math.log(r / rate)))
    canonical_pcm(b"", channels, target_rate)
    report.update(bank_sample_rate=target_rate, alias_pitch_scale=rate / target_rate)
    if form == b"WAVE" and tag == 1 and bits == 16 and align == channels * 2:
        pcm = parts[b"data"]
        report["status"] = "exact_original_pcm"
    elif form == b"WAVE" and tag == 2 and bits == 4:
        if len(fmt) < 50 or align < 7 * channels or len(parts[b"data"]) % align:
            raise SoundError("incomplete Microsoft ADPCM blocks")
        samples_per_block = struct.unpack_from("<H", fmt, 18)[0]
        if samples_per_block != 2 + (align - 7 * channels) * 2 // channels:
            raise SoundError("inconsistent Microsoft ADPCM samples per block")
        coefficients = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
        if struct.unpack_from("<H", fmt, 20)[0] != 7 or fmt[22:50] != b"".join(struct.pack("<hh", *p) for p in coefficients):
            raise SoundError("custom ADPCM coefficient table requires XAudio2 semantics verification")
        if os.name != "nt":
            raise SoundError("verified ADPCM decoder requires Windows ACM; FFmpeg samples differ from XAudio2")
        from . import acm
        pcm = acm.decode(fmt, parts[b"data"])
        expected = len(parts[b"data"]) // align * samples_per_block * channels * 2
        if len(pcm) != expected:
            raise SoundError("native ADPCM output length disagrees with source blocks")
        report.update(status="native_ms_adpcm", decoder="Windows ACM msadp32",
                      sample_reference="XAudio2_0 native decoder agreement (audited standard 7-coefficient format)",
                      expected_pcm_bytes=expected, decoded_pcm_bytes=len(pcm), adpcm_samples_per_block=samples_per_block)
    elif form == b"XWMA" and tag in (0x161, 0x162):
        from . import xwma
        pcm, native = xwma.decode(source, parts, xwma_decoder)
        report.update(native, status="native_xwma")
    else:
        raise SoundError(f"audio codec {tag:#x}/{bits} bits requires implementation")
    wav = canonical_pcm(pcm, channels, target_rate)
    if rate != target_rate:
        report["sample_rate_translation"] = "original PCM unchanged; multiply both alias pitches by alias_pitch_scale"
    report.update(frames=len(pcm) // (channels * 2), pcm_sha256=hashlib.sha256(pcm).hexdigest())
    if b"PRIV" in parts:
        report["original_private_metadata_hex"] = parts[b"PRIV"].hex()
    # Loop cue/sampler metadata is kept for a later loop-region translation.
    if any(t in parts for t in (b"smpl", b"cue ")):
        report.update(status="loop_region_requires_translation",
                      loop_metadata={t.decode("ascii"): parts[t].hex() for t in (b"smpl", b"cue ") if t in parts})
        return None, report
    return wav, report


LOADED_RATE = 48000  # every stock BO2 .sabl/.sabs entry (measured); the linker warns otherwise


def resample(pcm: bytes, channels: int, rate: int, target: int, tool: Path) -> bytes:
    """Band-limited rate conversion of decoded PCM (FFmpeg swr, long filter).
    Used only where BO2 cannot play the original rate; never for decoding."""
    command = [str(tool), "-v", "error", "-f", "s16le", "-ar", str(rate), "-ac", str(channels), "-i", "pipe:0",
               "-af", f"aresample={target}:filter_size=128:phase_shift=14:cutoff=0.97",
               "-f", "s16le", "-ar", str(target), "-ac", str(channels), "pipe:1"]
    proc = subprocess.run(command, input=pcm, capture_output=True, timeout=300)
    if proc.returncode or len(proc.stdout) % (channels * 2):
        raise SoundError(f"resampling failed: {proc.stderr.decode(errors='replace')[:200]}")
    expected = round(len(pcm) // (channels * 2) * target / rate)
    if abs(len(proc.stdout) // (channels * 2) - expected) > 2:
        raise SoundError("resampled length disagrees with the source duration")
    return proc.stdout


def stage(project: Path, output: Path, decoder: Path | None = None,
          xwma_decoder: Path | None = None) -> dict:
    source_report = json.loads((project / "sounds.stage.json").read_text())
    decoder = find_decoder(decoder) if decoder else None
    report = {"status": "partial_pcm_stage", "decoder": str(decoder) if decoder else None,
              "audio": [], "errors": [], "runtime_validated": False}
    written = {}
    resampler = None
    for entry in source_report["audio"]:
        if "output" not in entry:
            report["errors"].append({"name": entry["name"], "reason": entry["status"]})
            continue
        relative = entry["output"]
        output_name("audio", relative)
        try:
            pcm, info = convert(project / relative, decoder, xwma_decoder)
            info["name"] = entry["name"]
            info["loadType"] = entry["loadType"]
            if pcm is not None and entry["loadType"] == 1 and info["sample_rate"] != LOADED_RATE:
                if resampler is None:
                    resampler = find_decoder(None) or False
                if not resampler:
                    raise SoundError(f"loaded sound at {info['sample_rate']} Hz needs resampling to {LOADED_RATE}; "
                                     "no resampler (FFmpeg) found")
                data = resample(chunks(pcm)[1][b"data"], info["channels"], info["sample_rate"], LOADED_RATE,
                                resampler)
                info.update(original_pcm_sha256=info["pcm_sha256"], original_frames=info["frames"],
                            pcm_sha256=hashlib.sha256(data).hexdigest(), frames=len(data) // (info["channels"] * 2),
                            bank_sample_rate=LOADED_RATE, alias_pitch_scale=1.0,
                            sample_rate_translation=f"resampled {info['sample_rate']} -> {LOADED_RATE} Hz "
                                                    "(BO2 loaded banks are 48 kHz only)")
                pcm = canonical_pcm(data, info["channels"], LOADED_RATE)
            info["aliases"] = entry.get("aliases", [])
            report["audio"].append(info)
            if pcm is not None:
                destination = output / Path(relative).with_suffix(".wav")
                destination.parent.mkdir(parents=True, exist_ok=True)
                # A normal rebuild may improve a decoder or change source data.
                # Detect conflicting assets in THIS run, not the previous output.
                if destination in written and written[destination] != info["pcm_sha256"]:
                    raise SoundError(f"conflicting PCM output: {destination}")
                destination.write_bytes(pcm)
                written[destination] = info["pcm_sha256"]
                info["output"] = destination.relative_to(output).as_posix()
        except (SoundError, subprocess.TimeoutExpired) as exc:
            report["errors"].append({"name": entry["name"], "reason": str(exc)})
    output.mkdir(parents=True, exist_ok=True)
    (output / "audio.stage.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report
