"""Build staged custom weapons with official BO2 tools and dump their assets.

FX are reference-only: the eventual map zone must supply their converted data.
Sound strings are NOT evidence of a completed sound-bank or gameplay port.
"""
import argparse
import json
import subprocess
from pathlib import Path

from waw2bo2.weapons import read_info
from waw2bo2 import iwi, techsets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("--bo2", type=Path, required=True)
    parser.add_argument("--unlinker", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    project, work, bo2 = args.project.resolve(), args.work.resolve(), args.bo2.resolve()
    report = json.loads((project / "weapons.stage.json").read_text())
    incomplete = {"missing_models": report["models"]["missing"],
                  "unsupported_models": report["models"]["unsupported"],
                  "missing_animations": report["missing_animations"],
                  "missing_accuracy_graphs": report["missing_accuracy_graphs"]}
    if any(incomplete.values()):
        raise RuntimeError(f"required original dependencies are incomplete; refusing stale staged assets: {incomplete}")
    source = work / "zone_source"
    source.mkdir(parents=True, exist_ok=True)
    zone = "waw_weapon_audit"
    references = "".join(f"fx,,waw/{n}\n" for n in report["dependencies"].get("fx", []))
    visuals = json.loads((project / "weapons.visuals.json").read_text())
    if visuals["errors"]:
        raise RuntimeError(f"weapon visual dependencies incomplete: {visuals['errors']}")
    image_names = [i["name"] for i in visuals["images"]]
    image_section = f">level.ipak_read,{zone}\n>ipak,{zone}\n" + "".join(f"image,{n}\n" for n in image_names)
    (source / f"{zone}.zone").write_text(
        ">game,T6\n" + image_section + references + (project / "weapons.zone.fragment").read_text(), encoding="utf-8")
    linker = bo2 / "bin/Linker.exe"
    proc = subprocess.run([str(linker), "--no-color", "--source-search-path", str(source),
                           "--add-asset-search-path", str(project), "--output-folder", str(work / "out"), zone],
                          cwd=linker.parent, capture_output=True, text=True, errors="replace")
    log = proc.stdout + proc.stderr
    (work / "official_linker.log").write_text(log)
    errors = [l for l in log.splitlines() if "ERROR" in l and "Could not open BSP" not in l]
    ff = work / "out" / f"{zone}.ff"
    if proc.returncode or errors or not ff.is_file():
        raise RuntimeError(f"weapon native build failed: {errors[:8]}; see {work / 'official_linker.log'}")
    dump = work / "roundtrip"
    proc = subprocess.run([str(args.unlinker.resolve()), "--no-color", "--include-assets",
                           "weapon,xmodel,material,image,xanim", "--image-format", "DDS", "--model-format", "GLTF",
                           "--search-path", str(ff.parent), "--output-folder", str(dump), str(ff)],
                          capture_output=True, text=True, errors="replace")
    (work / "unlinker.log").write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError(f"weapon native dump failed: see {work / 'unlinker.log'}")
    results = []
    for weapon in report["weapons"]:
        name = weapon["name"]
        before = read_info((project / "weapons" / name).read_text())
        path = dump / "weapons" / name
        if not path.is_file():
            results.append({"name": name, "status": "missing_roundtrip"})
            continue
        after = read_info(path.read_text())
        changed = {k: {"source": v, "target": after.get(k)} for k, v in before.items() if after.get(k) != v}
        results.append({"name": name, "status": "exact_fields" if not changed else "fields_require_review",
                        "changed_fields": changed})
    images = []
    for name in image_names:
        before = project / techsets.oat_image_path(name)
        after = dump / techsets.oat_image_path(name, ".dds")
        images.append({"name": name, "status": "missing_roundtrip" if not after.is_file() else
                       "exact_pixels" if before.read_bytes() == iwi.dds_to_iwi(after.read_bytes()) else
                       "pixels_changed_requires_review"})
    result = {"weapons": results, "images": images, "gameplay_validated": False, "sounds_converted": False,
              "fx_reference_only": True,
              "unsupported_source_fields": {w["name"]: w["unsupported_fields"] for w in report["weapons"]
                                            if w["unsupported_fields"]}}
    (work / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"weapons": len(results), "field_mismatches": sum(len(w.get("changed_fields", {})) for w in results),
                      "images_exact": sum(i["status"] == "exact_pixels" for i in images),
                      "report": str(work / "report.json")}, indent=2))
    return 0 if all(w["status"] == "exact_fields" for w in results) and all(i["status"] == "exact_pixels" for i in images) else 1


if __name__ == "__main__":
    raise SystemExit(main())
