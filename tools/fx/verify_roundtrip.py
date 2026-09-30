"""Compare staged effects and recovered wavelet images with a T6 zone dump."""
import argparse
import json
from pathlib import Path

from waw2bo2 import iwi


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("staged", type=Path)
    p.add_argument("dump", type=Path)
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    results = {"effects": [], "wavelet_images": []}
    for path in sorted((args.staged / "fx").rglob("*.w2bfx.json")):
        out = args.dump / path.relative_to(args.staged)
        source = json.loads(path.read_text())
        # Provenance is converter metadata, not a runtime FxEffectDef field.
        source.pop("_source", None)
        exact = out.exists() and source == json.loads(out.read_text())
        results["effects"].append({"name": source["name"], "exact": exact})
    stage_report = json.loads((args.staged / "bridge_stage.report.json").read_text())
    for image in stage_report["images"]:
        if "wavelet" not in image:
            continue
        name = image["name"]
        original = args.staged / "images" / f"{name}.iwi"
        out = args.dump / "images" / f"{name}.dds"
        exact = out.exists() and iwi.dds_to_iwi(out.read_bytes()) == original.read_bytes()
        results["wavelet_images"].append({"name": name, "exact": exact})
    results["exact"] = all(x["exact"] for group in ("effects", "wavelet_images") for x in results[group])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(results, indent=2) + "\n")
    print(f"effects: {sum(x['exact'] for x in results['effects'])}/{len(results['effects'])} exact")
    print(f"wavelet images: {sum(x['exact'] for x in results['wavelet_images'])}/{len(results['wavelet_images'])} exact")
    return 0 if results["exact"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
