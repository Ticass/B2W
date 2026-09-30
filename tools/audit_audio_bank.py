"""Official BO2 bank payload probe, NOT a WaW alias-behavior translation.

Builds neutral test aliases to audit staged PCM/rate metadata through native
SABL/SABS packing. Never install this diagnostic zone as the converted mod.
"""
import argparse
import csv
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path

from waw2bo2.audio import T6_RATES, chunks


def sound_hash(name):
    result = 0x1505
    for char in name.lower():
        result = (ord(char) + 0x1003f * result) & 0xffffffff
    return result or 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcm", type=Path)
    parser.add_argument("--bo2", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--unlinker", type=Path, required=True)
    args = parser.parse_args()
    pcm, bo2, work = args.pcm.resolve(), args.bo2.resolve(), args.work.resolve()
    manifest = json.loads((pcm / "audio.stage.json").read_text())
    entries = [e for e in manifest["audio"] if "output" in e]
    if not entries:
        raise RuntimeError("no staged PCM in the current manifest")
    # This is an explicitly neutral audit definition, not a replacement for
    # source curves/speaker maps/ducking, which remain unresolved in sound IR.
    with (bo2 / "raw/soundbank/zmb_tomb_load.all.aliases.csv").open(newline="") as f:
        reader = csv.DictReader(f)
        headers, neutral = reader.fieldnames, next(reader)
    neutral.update(Secondary="", Bus="bus_fx", VolumeGroup="grp_reference", DuckGroup="snp_never",
                   Duck="", ReverbSend="0", CenterSend="0", VolMin="100", VolMax="100",
                   PanType="2d", Pan="default", Looping="nonlooping", FadeIn="0", FadeOut="0",
                   Subtitle="", IsMusic="no", IsCinematic="no", StopOnPlay="", FutzPatch="")
    # Determine valid duck neutral name from the native constants, do not guess.
    constants = Path(__file__).resolve().parents[1] / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/SoundConstantsT6.h"
    import re
    enum = constants.read_text().split("SOUND_DUCK_GROUPS", 1)[1].split("};", 1)[0]
    neutral["DuckGroup"] = re.findall(r'"([^"]+)"', enum)[0]
    zone, bank = "waw_audio_audit", "waw_audio_audit.all"
    assets, sources = work / "assets", work / "zone_source"
    (assets / "soundbank").mkdir(parents=True, exist_ok=True)
    sources.mkdir(parents=True, exist_ok=True)
    expected = {}
    with (assets / f"soundbank/{bank}.aliases.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        for index, entry in enumerate(entries):
            name = entry["output"]
            data = (pcm / name).read_bytes()
            if hashlib.sha256(data[44:]).hexdigest() != entry["pcm_sha256"]:
                raise RuntimeError(f"stale/different staged PCM: {name}")
            storage = "loaded" if index % 2 == 0 else "streamed"
            row = dict(neutral, Name=f"waw_audio_probe_{index}", FileSource=name, Storage=storage)
            row["PitchMin"] = row["PitchMax"] = str(1200 * math.log2(entry["alias_pitch_scale"]))
            writer.writerow(row)
            # Official BO2 loader canonicalizes FileSource to Windows slashes
            # before hashing (verified against dumped alias strings/bank IDs).
            key = sound_hash(name.replace("/", "\\"))
            if key in expected:
                raise RuntimeError(f"sound hash collision: {name}")
            expected[key] = dict(entry, storage=storage)
    (sources / f"{zone}.zone").write_text(f">game,T6\nsoundbank,{bank}\n", encoding="utf-8")
    out = work / "out"
    proc = subprocess.run([str(bo2 / "bin/Linker.exe"), "--no-color", "--source-search-path", str(sources),
                           "--add-asset-search-path", str(assets), "--add-asset-search-path", str(pcm),
                           "--output-folder", str(out), zone], cwd=work, capture_output=True, text=True, errors="replace")
    log = proc.stdout + proc.stderr
    (work / "official_linker.log").write_text(log)
    errors = [l for l in log.splitlines() if "ERROR" in l and "Could not open BSP" not in l]
    if proc.returncode or errors or not (out / f"{zone}.ff").is_file():
        raise RuntimeError(f"native bank build failed: {errors[:5]}; see {work / 'official_linker.log'}")
    results = []
    for suffix, storage in (("sabl", "loaded"), ("sabs", "streamed")):
        hits = list(work.rglob(f"{bank}.{suffix}"))
        if len(hits) != 1:
            raise RuntimeError(f"expected one current {suffix} bank, found {hits}")
        data = hits[0].read_bytes()
        magic, version, entry_size, _, _, count = struct.unpack_from("<6I", data)
        size, offset = struct.unpack_from("<qq", data, 32)
        if magic != 0x23585532 or version != 14 or entry_size != 20 or size != len(data):
            raise RuntimeError("unexpected native bank header")
        for index in range(count):
            key, length, start, frames, rate_index, channels, looping, fmt = struct.unpack_from("<4I4B", data, offset + 20 * index)
            entry = expected.pop(key)
            exact = (entry["storage"] == storage and fmt == 0 and not looping and
                     frames == entry["frames"] and channels == entry["channels"] and
                     T6_RATES[rate_index] == entry["bank_sample_rate"] and
                     hashlib.sha256(data[start:start + length]).hexdigest() == entry["pcm_sha256"])
            results.append({"name": entry["name"], "storage": storage,
                            "status": "exact_native_bank_pcm" if exact else "bank_mismatch"})
    dump = work / "roundtrip"
    proc = subprocess.run([str(args.unlinker.resolve()), "--no-color", "--include-assets", "soundbank",
                           "--search-path", str(out / "sound"), "--output-folder", str(dump),
                           str(out / f"{zone}.ff")], capture_output=True, text=True, errors="replace")
    (work / "unlinker.log").write_text(proc.stdout + proc.stderr)
    if proc.returncode or "Could not find data" in proc.stdout or "Failed to load sound bank" in proc.stdout:
        raise RuntimeError("native sound-bank dump failed; see unlinker.log")
    roundtrip = []
    for entry in entries:
        # T6 sound dumper appends a codec extension to the full asset filename.
        path = dump / (entry["output"] + ".wav")
        exact = path.is_file() and hashlib.sha256(chunks(path.read_bytes())[1][b"data"]).hexdigest() == entry["pcm_sha256"]
        roundtrip.append({"name": entry["name"], "status": "exact_unlinked_pcm" if exact else "dump_mismatch"})
    report = {"audio": results, "roundtrip": roundtrip, "missing": [e["name"] for e in expected.values()],
              "source_alias_semantics_translated": False, "runtime_validated": False}
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"native_pcm_exact": sum(e["status"] == "exact_native_bank_pcm" for e in results),
                      "roundtrip_pcm_exact": sum(e["status"] == "exact_unlinked_pcm" for e in roundtrip),
                      "missing": len(expected), "report": str(work / "report.json")}, indent=2))
    return int(bool(expected) or any(e["status"] != "exact_native_bank_pcm" for e in results)
               or any(e["status"] != "exact_unlinked_pcm" for e in roundtrip))


if __name__ == "__main__":
    raise SystemExit(main())
