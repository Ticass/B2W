"""Audit original XWMA with the actual installed XAudio2 2.0 xWMA decoder.

Native candidates remain separate; fixed-offset helper is audit-only and hash
guarded. This does not validate original sound alias behavior or live playback.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
from pathlib import Path

from waw2bo2 import audio
from waw2bo2.weapons import output_name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--helper", type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    dll = Path(os.environ["SystemRoot"]) / "SysWOW64/XAudio2_0.dll"
    digest = hashlib.sha256(dll.read_bytes()).hexdigest()
    if digest != "e435b73193bdf651f7ae564eba05266595ac672db45e0e22dce92d0bcb3c6513":
        raise RuntimeError("XAudio2 DLL differs from the IDA-audited reference")
    decoder = audio.find_decoder()
    report = {"dll_sha256": digest, "audio": [], "errors": [], "runtime_validated": False}
    manifest = json.loads((source / "sounds.stage.json").read_text())
    for entry in manifest["audio"]:
        if entry.get("status") != "preserved_requires_decode":
            continue
        relative = entry["output"]
        output_name("audio", relative)
        path = source / relative
        destination = output / Path(relative).with_suffix(".native.pcm")
        destination.parent.mkdir(parents=True, exist_ok=True)
        proc = subprocess.run([str(args.helper.resolve()), str(path), str(destination)],
                              capture_output=True, text=True, timeout=30)
        if proc.returncode:
            report["errors"].append({"name": entry["name"], "exit": proc.returncode, "error": proc.stderr})
            continue
        result = json.loads(proc.stdout)
        native = destination.read_bytes()
        _, parts = audio.chunks(path.read_bytes())
        channels = struct.unpack_from("<H", parts[b"fmt "], 2)[0]
        result.update(name=entry["name"], source=str(path), pcm_sha256=hashlib.sha256(native).hexdigest(),
                      status="native_dpds_exact" if len(native) == result["dpds_bytes"] and not result["input_remaining"] else "native_length_requires_review",
                      native_candidate=destination.relative_to(output).as_posix())
        if decoder:
            proc = subprocess.run([str(decoder), "-nostdin", "-v", "error", "-i", str(path),
                                   "-map", "0:a:0", "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"],
                                  capture_output=True, timeout=30)
            if proc.returncode or proc.stderr.strip():
                result["ffmpeg_error"] = proc.stderr.decode(errors="replace")
            else:
                candidate = proc.stdout
                result.update(ffmpeg_bytes=len(candidate), ffmpeg_pcm_sha256=hashlib.sha256(candidate).hexdigest(),
                              length_delta_bytes=len(native) - len(candidate),
                              samples_agree=native == candidate)
                if len(native) == len(candidate):
                    count = len(native) // 2
                    a, b = struct.unpack(f"<{count}h", native), struct.unpack(f"<{count}h", candidate)
                    result.update(different_samples=sum(x != y for x, y in zip(a, b)),
                                  max_sample_delta=max((abs(x - y) for x, y in zip(a, b)), default=0))
                result["frame_delta"] = (len(native) - len(candidate)) // (2 * channels)
        report["audio"].append(result)
    output.mkdir(parents=True, exist_ok=True)
    (output / "xwma.reference.json").write_text(json.dumps(report, indent=2) + "\n")
    from collections import Counter
    print(json.dumps({"status": dict(Counter(a["status"] for a in report["audio"])),
                      "length_deltas": dict(Counter(a.get("frame_delta") for a in report["audio"])),
                      "samples_agree": sum(a.get("samples_agree", False) for a in report["audio"]),
                      "errors": report["errors"][:5], "report": str(output / "xwma.reference.json")}, indent=2))
    return int(bool(report["errors"]) or any(a["status"] != "native_dpds_exact" for a in report["audio"]))


if __name__ == "__main__":
    raise SystemExit(main())
