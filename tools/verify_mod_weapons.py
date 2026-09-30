"""Round-trip the converted weapons of a built mod.ff (GUIDELINES 30).

Dumps the weapons from the built zone with the T6 Unlinker and compares each
field with the infostring staged in content_source/weapons. Also checks every
staged xanim and image line of weapons.runtime.json reached the zone.

    python tools/verify_mod_weapons.py <project content_source> <mod.ff> --unlinker <T6 Unlinker.exe> --work <dir>
"""
import argparse
import json
import subprocess
from pathlib import Path

from waw2bo2.weapons import read_info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("content", type=Path)
    parser.add_argument("mod_ff", type=Path)
    parser.add_argument("--unlinker", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    runtime = json.loads((args.content / "weapons.runtime.json").read_text(encoding="utf-8"))
    listing = subprocess.run([str(args.unlinker), "--no-color", "--list", "--search-path", str(args.mod_ff.parent),
                              str(args.mod_ff)], capture_output=True, text=True, errors="replace").stdout
    listed = {tuple(p.strip() for p in line.split(",", 1)) for line in listing.splitlines() if "," in line}
    subprocess.run([str(args.unlinker), "--no-color", "--include-assets", "weapon", "--search-path",
                    str(args.mod_ff.parent), "--output-folder", str(args.work), str(args.mod_ff)],
                   capture_output=True, text=True, errors="replace")
    results, mismatched = {}, 0
    for waw, bo2 in sorted(runtime["table"].items()):
        if not bo2 or waw not in runtime["weapons"]:
            continue
        dumped = args.work / "weapons" / bo2
        if not dumped.is_file():
            results[bo2] = "missing from mod.ff dump"
            mismatched += 1
            continue
        staged = read_info((args.content / "weapons" / bo2).read_text(encoding="utf-8"))
        after = read_info(dumped.read_text(encoding="utf-8"))
        diff = {k: [v, after.get(k)] for k, v in staged.items() if after.get(k, "") != v}
        results[bo2] = diff or "exact"
        mismatched += bool(diff)
    missing_lines = [line for line in runtime["zone_lines"]
                     if not line.startswith(">") and not line.startswith("fx,,")
                     and tuple(p.strip() for p in line.split(",", 1)) not in listed]
    report = {"weapons": results, "field_mismatches": mismatched, "zone_lines_missing": missing_lines}
    (args.work / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"weapons": len(results), "exact": sum(v == "exact" for v in results.values()),
                      "mismatched": mismatched, "zone_lines_missing": len(missing_lines),
                      "report": str(args.work / "report.json")}, indent=2))
    return 0 if not mismatched and not missing_lines else 1


if __name__ == "__main__":
    raise SystemExit(main())
