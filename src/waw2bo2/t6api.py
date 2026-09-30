"""What the BO2 (T6) script runtime provides: builtins and stock script functions.

* Builtins come from the game executable itself. T6 keeps them in tables of
  28-byte records {int type; fn; int 0; const char *name; int id; int minArgs;
  int maxArgs}; they are found by scanning the data sections for records
  whose function pointer lands in code and whose name is an identifier.
* Stock script functions come from the mod tools' ``raw`` scripts.

The BO2 GSC compiler does not check calls: an unresolved function or a wrong
builtin arity only fails when the map loads. The WaW script translator
validates every call against this index instead.
"""
from __future__ import annotations

import json
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import gsc

_IDENT = re.compile(rb"[a-z_][a-z0-9_]{1,63}\0")


def _sections(pe: bytes):
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    opt = struct.unpack_from("<H", pe, e_lfanew + 20)[0]
    base = struct.unpack_from("<I", pe, e_lfanew + 24 + 28)[0]
    off = e_lfanew + 24 + opt
    out = []
    for i in range(nsec):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", pe, off + i * 40)
        chars = struct.unpack_from("<I", pe, off + i * 40 + 36)[0]
        out.append((name.rstrip(b"\0").decode(errors="replace"), base + va, vsize, raw, rsize, chars))
    return out


def extract_tables(exe: Path) -> list[list[tuple[str, int, int]]]:
    """The executable's builtin tables: runs of 24-byte records
    {fn; int 0; const char *name; int id; int minArgs; int maxArgs}.
    Server/client and function/method tables are separate runs."""
    pe = exe.read_bytes()
    secs = _sections(pe)
    code = [(sva, sva + vsize) for _, sva, vsize, _, _, chars in secs if chars & 0x20000000]

    def va_to_off(va):
        for _, sva, vsize, raw, rsize, _ in secs:
            if sva <= va < sva + min(vsize, rsize):
                return raw + va - sva
        return None

    def record(j):
        p_fn, zero, p_name, _ident, lo, hi = struct.unpack_from("<IIIIII", pe, j)
        if zero or lo > 32 or hi > 32 or lo > hi or not any(a <= p_fn < b for a, b in code):
            return None
        off = va_to_off(p_name)
        m = _IDENT.match(pe, off) if off is not None else None
        return (m.group()[:-1].decode(), lo, hi) if m else None

    tables = []
    for _, _, _, raw, rsize, chars in secs:
        if chars & 0x20000000:
            continue
        j, end = raw, raw + rsize - 24
        while j < end:
            run = []
            while j < end:
                r = record(j)
                if r is None:
                    break
                run.append(r)
                j += 24
            if run:
                tables.append(run)
            else:
                j += 4
    return tables


def classify_tables(tables, usage: dict[str, list[int]]):
    """Which tables are server functions / server methods, judged by how the
    stock BO2 scripts call their names (usage: name -> [gsc fn, gsc method,
    csc fn, csc method] counts). Returns (functions, methods)."""
    functions: dict[str, tuple[int, int]] = {}
    methods: dict[str, tuple[int, int]] = {}
    for run in tables:
        # names both VMs / both call styles share (playfx, spawn...) say nothing;
        # vote with the names only one side or one style uses
        server = client = fn = meth = 0
        for name, _, _ in run:
            gf, gm, cf, cm = usage.get(name, (0, 0, 0, 0))
            server += bool(gf or gm) and not (cf or cm)
            client += bool(cf or cm) and not (gf or gm)
            fn += bool(gf or cf) and not (gm or cm)
            meth += bool(gm or cm) and not (gf or cf)
        if server <= client:
            continue    # client (CSC) table, or no evidence
        target = functions if fn >= meth else methods
        for name, lo, hi in run:
            old = target.get(name)
            target[name] = (min(lo, old[0]), max(hi, old[1])) if old else (lo, hi)
    return functions, methods


@dataclass
class T6Api:
    builtins: dict[str, tuple[int, int]]           # server builtin functions
    scripts: dict[str, dict[str, int]]           # script path (lower, backslashes) -> function -> param count
    methods: dict[str, tuple[int, int]] = field(default_factory=dict)   # server builtin methods
    by_name: dict[str, list[str]] = field(default_factory=dict)
    # assets of the zones every BO2 zombies map loads (type -> lower-case names)
    stock_assets: dict[str, set[str]] = field(default_factory=dict)

    def __post_init__(self):
        for path, names in self.scripts.items():
            for n in names:
                self.by_name.setdefault(n, []).append(path)

    def defines(self, script: str, name: str) -> bool:
        return name in self.scripts.get(script.lower(), ())

    def builtin_range(self, name: str, method: bool | None = None) -> tuple[int, int] | None:
        if method is None:
            a, b = self.builtins.get(name), self.methods.get(name)
            if a and b:
                return (min(a[0], b[0]), max(a[1], b[1]))
            return a or b
        return (self.methods if method else self.builtins).get(name)

    def builtin_accepts(self, name: str, argc: int, method: bool | None = None) -> bool | None:
        """None when ``name`` is not a builtin (of that kind)."""
        rng = self.builtin_range(name, method)
        if rng is None:
            return None
        return rng[0] <= argc <= rng[1]


SCRIPT_DIRS = ("maps", "common_scripts", "codescripts", "animscripts", "character", "clientscripts")


CACHE_VERSION = 3
# zones loaded before every zombies map (their assets need not be in the map zone)
STOCK_ZONES = ("code_pre_gfx_zm", "code_post_gfx_zm", "common_zm", "patch_zm")


def list_zone_assets(bo2_root: Path, unlinker: Path) -> dict[str, set[str]]:
    import subprocess

    out: dict[str, set[str]] = {}
    for zone in STOCK_ZONES:
        ff = bo2_root / "zone" / "all" / f"{zone}.ff"
        if not ff.exists():
            continue
        proc = subprocess.run([str(unlinker), "--no-color", "--list", "--search-path", str(ff.parent), str(ff)],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
        for line in proc.stdout.splitlines():
            kind, sep, name = line.partition(",")
            if sep and kind.strip().isidentifier():
                out.setdefault(kind.strip(), set()).add(name.strip().lstrip(",").lower())
    return out


def build(bo2_root: Path, cache: Path, unlinker: Path | None = None) -> T6Api:
    if cache.exists():
        data = json.loads(cache.read_text(encoding="utf-8"))
        if data.get("version") == CACHE_VERSION and (unlinker is None or data.get("stock_assets")):
            return T6Api({k: tuple(v) for k, v in data["builtins"].items()},
                         data["scripts"],
                         methods={k: tuple(v) for k, v in data["methods"].items()},
                         stock_assets={k: set(v) for k, v in data.get("stock_assets", {}).items()})
    raw = bo2_root / "raw"
    scripts: dict[str, dict[str, int]] = {}
    usage: dict[str, list[int]] = {}
    parsed = []
    for sub in SCRIPT_DIRS:
        if not (raw / sub).exists():
            continue
        exts = (".csc",) if sub == "clientscripts" else (".gsc",)
        for rel, script in gsc.load_scripts(raw / sub, exts).items():
            scripts[f"{sub}\\{rel}"] = {n: len(fn.params) for n, fn in script.functions.items()}
            parsed.append((sub == "clientscripts", script))
    defined = set().union(*scripts.values())
    for client, script in parsed:
        for r in gsc.references(script.tokens):
            if r.qualifier is None and not r.pointer and r.name.lower() not in defined:
                usage.setdefault(r.name.lower(), [0, 0, 0, 0])[2 * client + r.method] += 1
    functions, methods = classify_tables(extract_tables(bo2_root / "t6zm.exe"), usage)
    # Names stock server scripts call without defining them are builtins of
    # that kind even when the table scan did not find their table (arity unknown).
    for name, (fn, meth, _, _) in usage.items():
        if fn and name not in functions:
            functions[name] = (0, 32)
        if meth and name not in methods:
            methods[name] = (0, 32)
    assets = list_zone_assets(bo2_root, unlinker) if unlinker is not None else {}
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps({"version": CACHE_VERSION, "builtins": functions, "methods": methods,
                                 "scripts": scripts,
                                 "stock_assets": {k: sorted(v) for k, v in assets.items()}}), encoding="utf-8")
    return T6Api(functions, scripts, methods=methods, stock_assets=assets)
