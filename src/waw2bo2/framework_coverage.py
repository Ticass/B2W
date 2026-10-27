"""What runs in place of each WaW zombie-framework function in a converted map.

docs/CONVERT_THE_WHOLE_MAP.md and CLAUDE.md: the converted map runs its own
WaW code; BO2 is only engine glue. For every function of the WaW framework
scripts the map loads, from the build's own report:

  ported    the source function was extracted/translated (it runs)
  bo2       calls to it were redirected to a BO2 function of that name
  compat    calls to it go to a hand-written compat runtime function
  not run   nothing calls it: BO2's framework runs its own logic instead
            (for entry points such as round_think this is the whole system)

Every build writes framework_coverage.json/.md beside its report and warns
for each function BO2 runs in place of the map's without a glue reason
(gsc_api source_driven glue): that is conversion work still to do.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import re

from . import gsc

FRAMEWORK = re.compile(r"^maps[\\/]_(zombiemode\w*|laststand|gameskill)\.gsc$", re.IGNORECASE)
REDIRECT = re.compile(r"^(maps\\_\w+)::(\w+) -> (.+)$", re.IGNORECASE)


def framework_scripts(roots: list[Path]) -> dict[str, Path]:
    """maps\\<script> -> its first copy along roots (map dumps first, then stock)."""
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


def coverage(report: dict, roots: list[Path]) -> dict[str, dict[str, str]]:
    """{script: {function: status}} from a stage report's dict."""
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


def glue_reason(api: dict, script: str, name: str) -> str | None:
    """The documented reason BO2 keeps this function (gsc_api source_driven glue)."""
    for key, driven in (api.get("source_driven") or {}).items():
        if key.lower() == script.lower():
            return (driven.get("glue") or {}).get(name)
    return None


def undocumented_bo2(cov: dict[str, dict[str, str]], api: dict) -> list[str]:
    """Report lines for map functions BO2's run in place of without a glue reason."""
    lines = []
    for key, rows in sorted(cov.items()):
        for name, status in rows.items():
            if status.startswith("bo2") and not glue_reason(api, key, name):
                lines.append(f"FRAMEWORK_BO2 {key}::{name}: {status}; the map's own function does not run")
    return lines


def totals(cov: dict[str, dict[str, str]]) -> dict[str, int]:
    return dict(Counter(v.split(" ")[0] for rows in cov.values() for v in rows.values()))


def markdown(project: str, cov: dict[str, dict[str, str]]) -> str:
    out = [f"# WaW framework coverage: {project}", ""]
    out.append("| status | functions |\n|---|---|")
    out += [f"| {k} | {v} |" for k, v in sorted(totals(cov).items())]
    for key, rows in sorted(cov.items()):
        counts = Counter(v.split(" ")[0] for v in rows.values())
        out += ["", f"## {key} ({', '.join(f'{k} {v}' for k, v in sorted(counts.items()))})", "",
                "| function | in the port |", "|---|---|"]
        out += [f"| {name} | {status} |" for name, status in rows.items()]
    return "\n".join(out) + "\n"
