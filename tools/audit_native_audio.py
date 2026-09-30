"""Compare staged FFmpeg samples with offline native Windows ACM decoding.

Windows codec agreement is useful evidence, not proof of the WaW runtime's
particular decoder. Candidate output remains separate from runtime staging.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from waw2bo2 import acm, audio
from waw2bo2.sounds import SoundError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("pcm", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--xaudio-helper", type=Path,
                        help="x86 audit harness for the installed XAudio2 2.0 DLL")
    args = parser.parse_args()
    source, pcm, output = args.source.resolve(), args.pcm.resolve(), args.output.resolve()
    sources = json.loads((source / "sounds.stage.json").read_text())
    previous = {e["name"]: e for e in json.loads((pcm / "audio.stage.json").read_text())["audio"]}
    report = {"audio": [], "errors": [], "waw_runtime_decoder_verified": False}
    if args.xaudio_helper:
        dll = Path("C:/Windows/SysWOW64/XAudio2_0.dll")
        digest = hashlib.sha256(dll.read_bytes()).hexdigest()
        if digest != "e435b73193bdf651f7ae564eba05266595ac672db45e0e22dce92d0bcb3c6513":
            raise RuntimeError("XAudio reference DLL differs from the IDA-audited binary")
        report["xaudio_reference_dll_sha256"] = digest
    for entry in sources["audio"]:
        if "output" not in entry:
            continue
        path = source / entry["output"]
        try:
            _, parts = audio.chunks(path.read_bytes())
            tag, channels, rate = struct.unpack_from("<HHI", parts[b"fmt "])
            if tag != 2:  # ACM MS ADPCM reference; WMA still needs a different native API.
                continue
            native = acm.decode(parts[b"fmt "], parts[b"data"])
            old = previous.get(entry["name"], {})
            info = {"name": entry["name"], "native_pcm_bytes": len(native),
                    "native_pcm_sha256": hashlib.sha256(native).hexdigest(), "source": str(path)}
            if "output" in old:
                candidate = audio.chunks((pcm / old["output"]).read_bytes())[1][b"data"]
                info["ffmpeg_pcm_sha256"] = hashlib.sha256(candidate).hexdigest()
                if len(candidate) == len(native):
                    count = len(native) // 2
                    a, b = struct.unpack(f"<{count}h", native), struct.unpack(f"<{count}h", candidate)
                    info.update(different_samples=sum(x != y for x, y in zip(a, b)),
                                max_sample_delta=max((abs(x - y) for x, y in zip(a, b)), default=0),
                                status="exact_codec_agreement" if native == candidate else "codec_samples_disagree")
                else:
                    info["status"] = "codec_lengths_disagree"
            else:
                info["status"] = "no_ffmpeg_candidate"
            target_rate = old.get("bank_sample_rate", rate)
            wav = audio.canonical_pcm(native, channels, target_rate)
            destination = output / Path(entry["output"]).with_suffix(".wav")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(wav)
            info["native_candidate"] = destination.relative_to(output).as_posix()
            if args.xaudio_helper:
                temporary = destination.with_suffix(".xaudio.pcm")
                proc = subprocess.run([str(args.xaudio_helper.resolve()), str(path), str(temporary)],
                                      capture_output=True, timeout=30)
                if proc.returncode:
                    raise SoundError(f"XAudio2 native harness failed: {proc.returncode}")
                reference = temporary.read_bytes()
                info["xaudio_pcm_sha256"] = hashlib.sha256(reference).hexdigest()
                info["xaudio_status"] = "exact_xaudio_acm_agreement" if reference == native else "xaudio_acm_disagree"
                temporary.unlink()
            report["audio"].append(info)
        except SoundError as exc:
            report["errors"].append({"name": entry["name"], "reason": str(exc)})
    output.mkdir(parents=True, exist_ok=True)
    (output / "native_decoder_audit.json").write_text(json.dumps(report, indent=2) + "\n")
    from collections import Counter
    print(json.dumps({"status": dict(Counter(e["status"] for e in report["audio"])),
                      "xaudio": dict(Counter(e.get("xaudio_status", "not_audited") for e in report["audio"])),
                      "errors": len(report["errors"]), "report": str(output / "native_decoder_audit.json")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
