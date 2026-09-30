"""Explicit, opt-in FX fallback (``stage-bridge --fx-fallback``), off by default.

WaW effects are not converted yet (the real path: WaW FxEffectDef -> iwfx text
-> BO2 mod tools Linker). Until then, and only when asked for, WaW effect names
can be played as a stock BO2 effect. Every substitution is listed in the stage
report as ``FX_FALLBACK``; everything else stays ``UNSUPPORTED_FX``.

Tables (data, not code):
* ``compat/fx_fallback.json``            -- stock WaW effect -> stock BO2 effect (any map)
* ``compat/overrides/<waw map>.json``    -- a map's own effects ("fx" object), isolated per map

A BO2 effect already in a zone every zombies map loads is used as is; one
only in the mod tools' ``raw/fx`` is added to mod.ff (``fx,<name>``), which
the official Linker compiles.
"""
from __future__ import annotations

import json
from pathlib import Path

COMPAT_DIR = Path(__file__).parent / "compat"


def _load(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {k.lower(): v for k, v in data.get("fx", {}).items()}


def fallback_table(names: list[str], map_name: str, bo2_root: Path, stock_fx: set[str]):
    """Returns (table waw->bo2, fx for mod.ff, report lines, errors)."""
    rules = _load(COMPAT_DIR / "fx_fallback.json")
    overrides = _load(COMPAT_DIR / "overrides" / f"{map_name}.json")
    table: dict[str, str] = {}
    mod_fx: list[str] = []
    lines: list[str] = []
    errors: list[str] = []
    for name in sorted(set(names)):
        target = overrides.get(name.lower()) or rules.get(name.lower())
        source = "map profile" if name.lower() in overrides else "global table"
        if not target:
            continue
        if target.lower() in stock_fx:
            where = "stock zone"
        elif (bo2_root / "raw" / "fx" / f"{target}.efx").exists():
            where = "mod.ff"
            if target not in mod_fx:
                mod_fx.append(target)
        else:
            errors.append(f"fx fallback {name} -> {target}: {target} is neither in a stock zone nor in raw/fx")
            continue
        table[name] = target
        lines.append(f"FX_FALLBACK {name} -> stock BO2 {target} ({source}, {where})")
    return table, mod_fx, lines, errors
