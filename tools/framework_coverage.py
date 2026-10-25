"""What runs in place of each WaW zombie-framework function in a converted map.

docs/CONVERT_THE_WHOLE_MAP.md: a converted map must run the map's own WaW code.
The port replaces WaW's _zombiemode framework with BO2's _zm; this tool measures,
for every function of the WaW framework scripts the map loads, what the build
does with it, from the build's own report (bridge_stage.report.json):

  ported    the source function was extracted/translated (it runs)
  bo2       calls to it were redirected to a BO2 function of that name
  compat    calls to it go to a hand-written compat runtime function
  not run   nothing calls it: BO2's framework runs its own logic instead
            (for entry points such as round_think this is the whole system)

usage: python tools/framework_coverage.py <stage zone_raw/project> <waw script root>... [--md out.md]
The WaW roots are searched in order (map dump first, then stock raw/).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from waw2bo2 import gsc  # noqa: E402

FRAMEWORK = re.compile(r"^maps[\\/]_(zombiemode\w*|laststand|zombiemode)\.gsc$", re.IGNORECASE)
REDIRECT = re.compile(r"^(maps\\_\w+)::(\w+) -> (.+)$", re.IGNORECASE)


def framework_scripts(roots: list[Path]) -> dict[str, Path]:
    found: dict[str, Path] = {}
    for root in roots:
        folder = root / "maps"
        if not folder.is_dir():
            continue
        for path in sorted(folder.glob("_*.gsc")):
            key = "maps\\" + path.stem.lower()
            if FRAMEWORK.match(f"maps/{path.name}") and key not in found:
                found[key] = path
    return found


def coverage(project: Path, roots: list[Path]) -> dict[str, dict[str, str]]:
    report = json.loads((project / "bridge_stage.report.json").read_text(encoding="utf-8"))
    scripts = report.get("scripts") or {}
    extracted = {e.split(" ")[0].lower() for e in scripts.get("extracted", [])}
    ported = {p.lower().replace("/", "\\").removesuffix(".gsc") for p in scripts.get("ported", [])}
    redirects: dict[str, str] = {}
    for rewrite in (scripts.get("rewrites") or {}):
        m = REDIRECT.match(rewrite)
        if m:
            redirects[f"{m[1].lower()}::{m[2].lower()}"] = m[3]
    result: dict[str, dict[str, str]] = {}
    for key, path in framework_scripts(roots).items():
        script = gsc.parse(path.read_text(encoding="utf-8", errors="replace"), key)
        whole = any(p.endswith(key.split("\\")[-1]) for p in ported)
        rows = {}
        for name in sorted(script.functions):
            qualified = f"{key}::{name}"
            if whole or qualified in extracted:
                rows[name] = "ported"
            elif qualified in redirects:
                target = redirects[qualified]
                rows[name] = "compat" if "compat" in target else f"bo2 ({target})"
            else:
                rows[name] = "not run"
        result[key] = rows
    return result


def markdown(project: Path, cov: dict[str, dict[str, str]]) -> str:
    out = [f"# WaW framework coverage: {project.name}", ""]
    total = Counter()
    for rows in cov.values():
        total.update(v.split(" ")[0] for v in rows.values())
    out.append("| status | functions |\n|---|---|")
    out += [f"| {k} | {v} |" for k, v in sorted(total.items())]
    for key, rows in sorted(cov.items()):
        counts = Counter(v.split(" ")[0] for v in rows.values())
        out += ["", f"## {key} ({', '.join(f'{k} {v}' for k, v in sorted(counts.items()))})", "",
                "| function | in the port |", "|---|---|"]
        out += [f"| {name} | {status} |" for name, status in rows.items()]
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("roots", type=Path, nargs="+")
    parser.add_argument("--md", type=Path)
    args = parser.parse_args()
    cov = coverage(args.project, args.roots)
    text = markdown(args.project, cov)
    if args.md:
        args.md.write_text(text, encoding="utf-8")
    total = Counter(v.split(" ")[0] for rows in cov.values() for v in rows.values())
    print(dict(total))
    for key, rows in sorted(cov.items()):
        print(key, dict(Counter(v.split(" ")[0] for v in rows.values())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
