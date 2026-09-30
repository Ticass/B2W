"""Audit every wavelet IWI in ordered IWD directories against the native oracle.

python tools/audit_wavelet.py --binary wawModTools/bin/AssetViewer.exe
  --oracle work/wavelet/wavelet_reference.exe --output work/wavelet/audit
  --iwd-dir <mod> --iwd-dir <WaW/main>
Artifacts contain original pixels, hashes, and source archive names. Not game files.
"""
import argparse
import collections
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path

from waw2bo2 import iwi, wavelet


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--iwd-dir", type=Path, action="append", required=True)
    p.add_argument("--all-copies", action="store_true", help="also audit images shadowed by earlier IWDs")
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tables = wavelet.load_tables(args.binary)
    results = []
    seen = set()
    for directory in args.iwd_dir:
        for archive in sorted(directory.glob("*.iwd")):
            with zipfile.ZipFile(archive) as z:
                for name in z.namelist():
                    if not name.lower().endswith(".iwi") or (name.lower() in seen and not args.all_copies):
                        continue
                    seen.add(name.lower())
                    blob = z.read(name)
                    if blob[:4] != b"IWi\x06" or blob[4] not in range(6, 11):
                        continue
                    dst = args.output / f"{len(results):03d}_{Path(name).stem}"
                    dst.with_suffix(".iwi").write_bytes(blob)
                    image = wavelet.decode(blob, tables)
                    subprocess.run([str(args.oracle.resolve()), str(args.binary.resolve()),
                                    str(dst.with_suffix('.iwi').resolve()), str(dst.with_suffix('.raw').resolve())],
                                   check=True, timeout=30)
                    actual = b"".join(image.levels)
                    reference = dst.with_suffix(".raw").read_bytes()
                    if actual != reference:
                        raise AssertionError(f"pixel mismatch {archive}:{name}")
                    dds = image.dds()
                    # Validate channel masks and DDS -> IWI27 for every decoded mip.
                    parsed = iwi.read_dds(dds)
                    result = iwi.dds_to_iwi(dds)
                    canonical = [iwi._convert_pixels(parsed, chunk) for chunk in image.levels]
                    assert result[64:] == b"".join(reversed(canonical))
                    dst.with_suffix(".dds").write_bytes(dds)
                    record = {"archive": str(archive), "entry": name, "format": image.format,
                              "width": image.width, "height": image.height, "mips": len(image.levels),
                              "faces": image.faces, "bytes": len(actual), "oracle_match": True,
                              "sha256": hashlib.sha256(actual).hexdigest()}
                    results.append(record)
                    print(f"exact {name}: {image.width}x{image.height}, {len(image.levels)} mips", flush=True)
    report = {"binary": str(args.binary), "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "count": len(results), "formats": dict(collections.Counter(r['format'] for r in results)),
              "images": results}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(results)} images exact against native WaW decoder")


if __name__ == "__main__":
    main()
