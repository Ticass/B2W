"""Compile staged original xanims with installed BO2 tools and dump them back.

Compare raw payloads independently of the raw-file version word. A mismatch
is reported, not assumed harmless; this audit does not test gameplay binding.
"""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--bo2", required=True, type=Path)
    parser.add_argument("--unlinker", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    project, work, bo2 = args.project.resolve(), args.work.resolve(), args.bo2.resolve()
    stage = json.loads((project / "weapons.stage.json").read_text())
    names = [a["name"] for a in stage["animations"]]
    source = work / "zone_source"
    source.mkdir(parents=True, exist_ok=True)
    zone = "waw_anim_audit"
    (source / f"{zone}.zone").write_text(
        ">game,T6\n" + "".join(f"xanim,{n}\n" for n in names), encoding="utf-8")
    linker = bo2 / "bin/Linker.exe"
    proc = subprocess.run([str(linker), "--no-color", "--source-search-path", str(source),
                           "--add-asset-search-path", str(project), "--output-folder", str(work / "out"), zone],
                          cwd=linker.parent, capture_output=True, text=True, errors="replace")
    log = proc.stdout + proc.stderr
    (work / "official_linker.log").write_text(log)
    errors = [line for line in log.splitlines() if "ERROR" in line and "Could not open BSP" not in line]
    ff = work / "out" / f"{zone}.ff"
    if proc.returncode or errors or not ff.is_file():
        raise RuntimeError(f"official animation build failed: {errors[:8]}; see {work / 'official_linker.log'}")
    dump = work / "roundtrip"
    proc = subprocess.run([str(args.unlinker.resolve()), "--no-color", "--include-assets", "xanim",
                           "--output-folder", str(dump), str(ff)], capture_output=True, text=True, errors="replace")
    (work / "unlinker.log").write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError(f"roundtrip dump failed: {work / 'unlinker.log'}")
    results = []
    for name in names:
        src, dst = project / "xanim" / name, dump / "xanim" / name
        if not dst.is_file():
            results.append({"name": name, "status": "missing_roundtrip"})
            continue
        before, after = src.read_bytes(), dst.read_bytes()
        results.append({"name": name, "source_bytes": len(before), "target_bytes": len(after),
                        "source_version": int.from_bytes(before[:2], "little"),
                        "target_version": int.from_bytes(after[:2], "little"),
                        "status": "exact_payload" if before[2:] == after[2:] else "payload_changed_requires_review"})
    report = {"compiled": len(names), "exact_payloads": sum(r["status"] == "exact_payload" for r in results),
              "results": results, "gameplay_validated": False}
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "results"}, indent=2))
    return 0 if all(r["status"] == "exact_payload" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
