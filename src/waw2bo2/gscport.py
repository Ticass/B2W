"""World at War GSC -> Black Ops II GSC translator.

Input: the map's scripts (map zone rawfiles, mod.ff rawfiles, IWD archives)
and the stock WaW scripts (dumped from the stock zones). Output: BO2 GSC under
``maps/mp/waw/`` in the project, plus a generated stub script and a report.

Every WaW script is classified:

* core      -- WaW framework that BO2's own _zm framework replaces
               (compat/gsc_api.json: core_scripts / core_patterns /
               template_scripts). Never ported; references into it resolve to
               the BO2 counterpart function of the same name, a compat shim,
               or a reported stub.
* portable  -- everything else the map reaches (its own scripts and stock
               non-core scripts such as maps\\_weather). Ported recursively to
               ``maps\\mp\\waw\\<path>``.

Every function reference is resolved, because the BO2 compiler does not
check them and an unresolved one only fails when the map loads:
local / included ported script -> BO2 script function -> compat shim
(``maps\\mp\\waw\\_waw2bo2_compat::waw_<name>``) -> T6 builtin (arity
checked against the game executable) -> stub that reports
``WAW2BO2 UNSUPPORTED GSC <name>`` at runtime and in the stage report.

The map main (``maps\\<map>``) is split at its ``maps\\_zombiemode::main()``
call into ``waw_main_pre()`` / ``waw_main_post()``, which the BO2 map main
calls around ``maps\\mp\\zombies\\_zm::init()``.
"""
from __future__ import annotations

import json
import re
import zipfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from . import gsc
from .t6api import T6Api

COMPAT_DIR = Path(__file__).parent / "compat"
API = json.loads((COMPAT_DIR / "gsc_api.json").read_text(encoding="utf-8"))
COMPAT = "maps\\mp\\waw\\_waw2bo2_compat"
STUBS = "maps\\mp\\waw\\_waw2bo2_stubs"
ASSETS = "maps\\mp\\waw\\_waw2bo2_assets"
CORE = "maps\\mp\\waw\\_waw2bo2_core"
CORE_INCLUDES = ("common_scripts\\utility", "maps\\mp\\_utility", "maps\\mp\\zombies\\_zm_utility")
# BO2 scripts a unique-name lookup may resolve into (never map/other-mode scripts)
BO2_SHARED = re.compile(r"^(common_scripts\\utility|maps\\mp\\_utility|maps\\mp\\zombies\\_zm[a-z_]*|"
                        r"maps\\mp\\gametypes_zm\\_[a-z_]+|maps\\mp\\_[a-z_]+)$")


def norm(path: str) -> str:
    return path.replace("/", "\\").lower().removesuffix(".gsc").removesuffix(".csc")


def ported_path(path: str) -> str:
    p = norm(path)
    if p.startswith("clientscripts\\"):
        return "clientscripts\\mp\\waw\\" + p.removeprefix("clientscripts\\")
    return "maps\\mp\\waw\\" + p.removeprefix("maps\\")


@dataclass
class PortReport:
    map_main: str = ""
    ported: list[str] = field(default_factory=list)
    core_overrides: list[str] = field(default_factory=list)
    extracted: list[str] = field(default_factory=list)
    unsupported: dict[str, list[str]] = field(default_factory=dict)
    rewrites: Counter = field(default_factory=Counter)
    fx: list[str] = field(default_factory=list)
    animtrees: dict[str, list[str]] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)
    characters: bool = False
    weapon_registration: dict = field(default_factory=dict)

    def to_json(self) -> dict:
        return {"map_main": self.map_main, "ported": self.ported, "core_overrides": self.core_overrides,
                "characters": self.characters, "weapon_registration": self.weapon_registration,
                "extracted": self.extracted,
                "unsupported": self.unsupported, "rewrites": dict(self.rewrites), "fx": self.fx,
                "animtrees": self.animtrees, "errors": self.errors}


class Sources:
    """Map scripts in WaW load priority, plus the stock scripts.

    With a mod loaded (fs_game), WaW (IW3 Scr_ReadFile) reads a script from the
    file system search path -- the mod's IWDs -- before the fastfile rawfile of
    the same name, so IWD copies win over zone rawfiles. Measured on a real
    map: its mod.ff compiled exactly the arms/body models its IWD _loadout and
    character scripts use, and none of the models the mod.ff rawfile copies of
    those scripts name, which would otherwise fail setModel at spawn."""

    def __init__(self, roots: list[Path], iwds: list[Path], stock_root: Path | None):
        self.text: dict[str, str] = {}
        self.origin: dict[str, str] = {}
        for iwd in iwds:
            with zipfile.ZipFile(iwd) as z:
                for name in sorted(z.namelist()):
                    if name.lower().endswith((".gsc", ".csc")):
                        key = norm(name) + Path(name).suffix.lower()
                        if key not in self.text:
                            self.text[key] = z.read(name).decode("utf-8", errors="replace")
                            self.origin[key] = f"{iwd.name}:{name}"
        for root in roots:
            if not root.exists():
                continue
            for f in sorted(list(root.rglob("*.gsc")) + list(root.rglob("*.csc"))):
                key = norm(f.relative_to(root).as_posix()) + f.suffix.lower()
                if key not in self.text:
                    self.text[key] = f.read_text(encoding="utf-8", errors="replace")
                    self.origin[key] = str(f)
        self.stock: dict[str, str] = {}
        if stock_root is not None and stock_root.exists():
            for f in sorted(list(stock_root.rglob("*.gsc")) + list(stock_root.rglob("*.csc"))):
                self.stock[norm(f.relative_to(stock_root).as_posix()) + f.suffix.lower()] = \
                    f.read_text(encoding="utf-8", errors="replace")
        self._parsed: dict[str, gsc.Script | None] = {}

    def get(self, path: str, ext: str = ".gsc") -> gsc.Script | None:
        key = norm(path) + ext
        if key not in self._parsed:
            src = self.text.get(key, self.stock.get(key))
            self._parsed[key] = gsc.parse(src, norm(path)) if src is not None else None
        return self._parsed[key]

    def is_stock(self, path: str, ext: str = ".gsc") -> bool:
        return norm(path) + ext in self.stock


def is_core(path: str, sources: Sources) -> bool:
    p = norm(path)
    if p in API["core_scripts"] or p in API["template_scripts"]:
        return True
    return sources.is_stock(p) and any(re.search(r, p) for r in API["core_patterns"])


LINE_COMMENT_BACKSLASH = re.compile(r"(//[^\n]*?)\\+[ \t]*(?=\r?\n|$)")


def neutralize_animations(tokens: list[gsc.Token], tree: str) -> None:
    """Drop a WaW animtree BO2 does not have: #using_animtree is commented out,
    #animtree and %anim references become strings (the animation builtins are
    routed to compat by the resolver)."""
    for i, t in enumerate(tokens):
        if t.kind == gsc.DIRECTIVE and t.text.lower().startswith("#using_animtree"):
            t.text = f"// waw2bo2: {t.text} (animtree not converted)"
        elif t.text == "#" and tokens[i + 1].low == "animtree":
            t.text = ""
            tokens[i + 1].text = f'"{tree}"'
            tokens[i + 1].kind = gsc.STRING
        elif t.text == "%" and tokens[i + 1].kind == gsc.IDENT and \
                tokens[i - 1].text in ("(", ",", "=", "[", "return", "[["):
            t.text = ""
            tokens[i + 1].text = f'"{tokens[i + 1].text}"'
            tokens[i + 1].kind = gsc.STRING


def fix_syntax(tokens: list[gsc.Token]) -> int:
    """WaW syntax the T6 compiler rejects. ``(a.b).size``: T6 cannot access a
    member of a parenthesised expression; the parentheses are redundant when
    they hold a plain access path. Returns the number of fixes."""
    fixes = 0
    for t in tokens:
        # T6 splices a line comment ending in '\' with the next line (WaW does not)
        if "\\" in t.pre:
            fixed = LINE_COMMENT_BACKSLASH.sub(r"\1", t.pre)
            fixes += fixed != t.pre
            t.pre = fixed
    for i, t in enumerate(tokens):
        if t.text != "(" or t.kind != gsc.PUNCT:
            continue
        prev = tokens[i - 1] if i else None
        if prev is not None and (prev.kind == gsc.IDENT and prev.low not in gsc.CALL_LIKE_KEYWORDS
                                 or prev.text in (")", "]", "]]", "::")):
            continue    # a call's argument list
        depth = 0
        simple = True
        for j in range(i, len(tokens)):
            u = tokens[j]
            if u.text in ("(", "["):
                depth += 1
                if u.text == "(" and j > i:
                    simple = False
            elif u.text in (")", "]"):
                depth -= 1
                if depth == 0:
                    break
            elif depth == 1 and not (u.kind == gsc.IDENT or u.text == "."):
                simple = False
        else:
            continue
        if simple and j > i + 1 and tokens[j + 1].text == "." and tokens[j].text == ")":
            t.text = ""
            tokens[j].text = ""
            fixes += 1
    return fixes


# level fields both frameworks own under the same name with different values.
# WaW scripts keep seeing the WaW value (set by _waw2bo2_assets::init); the
# BO2 framework keeps its own. level.script: WaW map name vs the BO2 project.
# level.chests / chest_index: WaW's box triggers vs BO2 _zm_magicbox structs
# (BO2 powerups such as fire sale iterate level.chests as BO2 structs).
WAW_LEVEL_FIELDS = {"script": "waw_script", "chests": "waw_chests", "chest_index": "waw_chest_index"}


def rename_level_fields(tokens: list[gsc.Token]) -> int:
    renamed = 0
    for i in range(2, len(tokens)):
        t = tokens[i]
        if t.kind == gsc.IDENT and t.low in WAW_LEVEL_FIELDS and tokens[i - 1].text == "." and \
                tokens[i - 2].low == "level" and (i + 1 >= len(tokens) or tokens[i + 1].text != "("):
            t.text = WAW_LEVEL_FIELDS[t.low]
            renamed += 1
    return renamed


def uses_animations(tokens: list[gsc.Token], start: int, end: int) -> bool:
    """``%anim`` references (as opposed to the modulo operator)."""
    for i in range(start + 1, end):
        if tokens[i].text == "%" and tokens[i + 1].kind == gsc.IDENT and \
                tokens[i - 1].text in ("(", ",", "=", "[", "return", "[["):
            return True
    return False


def _truncate_args(tokens: list[gsc.Token], name_index: int, keep: int) -> None:
    """Drop call arguments after the first ``keep`` (the T6 builtin takes fewer)."""
    depth = 0
    count = 0
    for j in range(name_index + 1, len(tokens)):
        t = tokens[j]
        if t.kind == gsc.PUNCT and t.text in ("(", "[", "{", "[["):
            depth += 1
            if depth == 1:
                continue
        elif t.kind == gsc.PUNCT and t.text in (")", "]", "}", "]]"):
            depth -= 1
            if depth == 0:
                return
        elif depth == 1 and t.text == ",":
            count += 1
        # blank argument `keep` onwards (and the comma before it)
        if depth >= 1 and (count >= keep if keep else True) and not (keep and count == keep - 1):
            if count >= keep:
                t.text, t.pre, t.kind = "", "", gsc.PUNCT


def _argc(tokens: list[gsc.Token], name_index: int) -> int | None:
    i = name_index + 1
    if tokens[i].text != "(":
        return None
    depth = 0
    count = 0
    empty = True
    for j in range(i, len(tokens)):
        t = tokens[j]
        if t.kind == gsc.PUNCT and t.text in ("(", "[", "{", "[["):
            depth += 1
        elif t.kind == gsc.PUNCT and t.text in (")", "]", "}", "]]"):
            depth -= 1
            if depth == 0:
                return 0 if empty else count + 1
        elif depth == 1 and t.text == ",":
            count += 1
        elif depth >= 1:
            empty = False
    return None


class Translator:
    def __init__(self, sources: Sources, api: T6Api, report: PortReport):
        self.sources = sources
        self.api = api
        self.report = report
        self.compat = gsc.parse((COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"), COMPAT)
        self.queue: list[str] = []
        self.done: dict[str, str] = {}        # waw path -> BO2 source text
        self.stubs: dict[str, tuple[str, int]] = {}   # stub name -> (description, max argc)
        self.core_funcs: dict[tuple[str, str], str] = {}  # (core script, function) -> extracted name
        self.core_queue: list[tuple[str, str]] = []
        self.animtrees: set[str] = set()      # animtrees available in BO2 (lower-case)
        # WaW framework-override animtrees staged for BO2: tree (lower) -> its
        # animations that have WaW xanims (lower). Extracted functions using
        # one keep their animations.
        self.core_animtrees: dict[str, set[str]] = {}
        self.level_state_parts: list[str] = []
        self.anim_neutralized = False

    # ---- classification -------------------------------------------------
    def owner(self, script: gsc.Script, name: str) -> str | None:
        """Where an unqualified ``name`` used in ``script`` is defined (WaW
        rules: the file itself, then its includes; includes are not transitive)."""
        if name in script.functions:
            return script.path
        for inc in script.includes:
            dep = self.sources.get(inc)
            if dep is not None and name in dep.functions:
                return norm(inc)
        return None

    def want(self, path: str) -> None:
        p = norm(path)
        if p not in self.done and p not in self.queue:
            self.queue.append(p)

    # ---- resolution -----------------------------------------------------
    def library_function(self, script: str, name: str) -> str | None:
        """Compat replacement for a third-party WaW library function (gsc_api library_functions)."""
        repl = API["library_functions"].get(norm(script), {}).get(name)
        if repl is None:
            return None
        self.report.rewrites[f"{norm(script)}::{name} -> compat waw_{repl}"] += 1
        return f"{COMPAT}::waw_{repl}"

    def compat_name(self, name: str) -> str | None:
        return f"{COMPAT}::waw_{name}" if f"waw_{name}" in self.compat.functions else None

    def bo2_unique(self, name: str) -> str | None:
        hits = [p for p in self.api.by_name.get(name, []) if BO2_SHARED.match(p)]
        return f"{hits[0]}::{name}" if len(hits) == 1 else None

    def stub(self, desc: str, name: str, argc: int | None, where: str) -> str:
        stub = "stub_" + re.sub(r"\W+", "_", desc.replace("::", "__")).strip("_")
        old = self.stubs.get(stub, (desc, 0))
        self.stubs[stub] = (desc, max(old[1], argc or 0))
        self.report.unsupported.setdefault(f"GSC {desc}", []).append(where)
        return f"{STUBS}::{stub}"

    def resolve_core(self, core: str, name: str, argc, where, included: set[str], sibling: bool = False) -> str | None:
        """Returns replacement text for the reference (None = keep as is).
        ``sibling``: called from code extracted from ``core`` itself; such a
        call resolves to BO2's counterpart of ``core`` or to the WaW function,
        never to an unrelated BO2 script that happens to share the name (the
        WaW box's treasure_chest_think is not BO2 _zm_magicbox's)."""
        renamed = API["function_renames"].get(name)
        if renamed:
            return renamed
        if any(self.api.defines(inc, name) for inc in included):
            self.report.rewrites[f"{core}::{name} -> included BO2 script"] += 1
            return None
        mapped = API["core_scripts"].get(core)
        if mapped and self.api.defines(mapped, name):
            self.report.rewrites[f"{core}::{name} -> {mapped}"] += 1
            return None if mapped in included else f"{mapped}::{name}"
        compat = self.compat_name(name)
        if compat:
            self.report.rewrites[f"{core}::{name} -> compat"] += 1
            return compat
        if sibling:
            extracted = self.extract(core, name)
            if extracted:
                return extracted
        unique = self.bo2_unique(name)
        if unique:
            self.report.rewrites[f"{core}::{name} -> {unique.split('::')[0]}"] += 1
            return unique
        extracted = self.extract(core, name)
        if extracted:
            return extracted
        return self.stub(f"{core}::{name}", name, argc, where)

    def resolve_builtin(self, name: str, argc, where, script: gsc.Script, included: set[str],
                        method: bool | None = None) -> str | None:
        if self.anim_neutralized and name in API["anim_builtins"]:
            self.report.rewrites[f"builtin {name} -> compat (animtree not converted)"] += 1
            return f"{COMPAT}::waw_anim_{API['anim_builtins'][name]}"
        compat = self.compat_name(name)
        if compat and (name in API["compat_wrappers"] or not self.api.builtin_accepts(name, argc or 0, method)):
            self.report.rewrites[f"builtin {name} -> compat"] += 1
            return compat
        accepts = self.api.builtin_accepts(name, argc if argc is not None else 0, method)
        if accepts:
            return None
        # a BO2 script function (WaW builtin became script code, e.g. getstruct)
        for inc in included:
            if self.api.defines(inc, name):
                return None
        unique = self.bo2_unique(name)
        if unique:
            self.report.rewrites[f"builtin {name} -> {unique.split('::')[0]}"] += 1
            return unique
        if accepts is False:
            rng = self.api.builtin_range(name, method)
            return self.stub(f"{name}/{argc}args(T6 takes {rng[0]}-{rng[1]})", name, argc, where)
        return self.stub(name, name, argc, where)

    # ---- translation ------------------------------------------------------
    def translate(self, path: str, main_split: bool = False) -> str:
        script = self.sources.get(path)
        tokens = script.tokens
        where_file = self.sources.origin.get(norm(path) + ".gsc", path)
        included_bo2: set[str] = set()
        new_includes: list[str] = []
        # includes
        for t in tokens:
            if t.kind != gsc.DIRECTIVE or not t.text.lower().startswith("#include"):
                continue
            inc = norm(gsc.INCLUDE_RE.match(t.text).group(1))
            if is_core(inc, self.sources):
                mapped = API["core_scripts"].get(inc)
                if mapped and mapped not in included_bo2 and mapped not in new_includes:
                    new_includes.append(mapped)
                    included_bo2.add(mapped)
                    t.text = f"#include {mapped};"
                else:
                    t.text = f"// waw2bo2: #include {inc}; (WaW framework, no BO2 include)"
            elif self.sources.get(inc) is None:
                self.report.errors.append(f"{path}: include {inc} not found in the map or stock scripts")
                t.text = f"// waw2bo2: #include {inc}; (script not found)"
            else:
                self.want(inc)
                t.text = f"#include {ported_path(inc)};"
        self.anim_neutralized = False
        if script.animtree:
            self.report.animtrees.setdefault(script.animtree, []).append(path)
            if script.animtree.lower() not in self.animtrees:
                self.anim_neutralized = True
                neutralize_animations(tokens, script.animtree)

        if main_split:
            self._split_main(script)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        defs = {fn.start for fn in script.functions.values()}
        self.resolve_refs(tokens, script, where_file, included_bo2, defs)
        header = (f"// Translated from World at War by waw2bo2 (gscport). Source: {where_file}\n")
        return header + gsc.emit(tokens)

    def resolve_refs(self, tokens: list[gsc.Token], script: gsc.Script, where_file: str,
                     included_bo2: set[str], skip: set[int], extracting: str | None = None) -> None:
        """Rewrite every function reference in ``tokens`` (code of ``script``).
        ``extracting``: the core script whose functions are being extracted;
        its own functions are extracted too instead of kept."""
        for ref in gsc.references(tokens):
            if ref.index in skip or not tokens[ref.index].text:
                continue    # a definition, or inside arguments already dropped
            tok = tokens[ref.index]
            name = ref.name.lower()
            where = f"{where_file}:{tok.line}"
            argc = None if ref.pointer else _argc(tokens, ref.index)
            if ref.qualifier is not None:
                target = norm(ref.qualifier)
                qual_tok = tokens[ref.qual_index]
                library = self.library_function(target, name)
                if library:
                    qual_tok.text = ""
                    tokens[ref.qual_index + 1].text = ""
                    tok.text = library
                elif is_core(target, self.sources):
                    repl = self.resolve_core(target, name, argc, where, set(), sibling=target == extracting)
                    if repl is None:
                        repl = f"{API['core_scripts'][target]}::{ref.name}"
                    qual_tok.text = ""
                    tokens[ref.qual_index + 1].text = ""
                    tok.text = repl
                else:
                    dep = self.sources.get(target)
                    if dep is None or name not in dep.functions:
                        qual_tok.text = ""
                        tokens[ref.qual_index + 1].text = ""
                        tok.text = self.stub(f"{target}::{name}", name, argc, where)
                    else:
                        self.want(target)
                        qual_tok.text = ported_path(target)
                continue
            owner = self.owner(script, name)
            library = self.library_function(owner, name) if owner else None
            if library and owner != script.path:
                if ref.pointer:
                    tokens[ref.index - 1].text = ""
                tok.text = library
                continue
            if owner == script.path and extracting is None:
                continue
            if owner is not None and not is_core(owner, self.sources):
                if extracting is not None:
                    # a framework override calling one of the map's scripts
                    self.want(owner)
                    repl = f"{ported_path(owner)}::{ref.name}"
                else:
                    continue    # included ported script; the include was rewritten
            elif owner is not None:
                repl = self.resolve_core(owner, name, argc, where, included_bo2, sibling=owner == extracting)
            else:
                method = None if ref.pointer else ref.method
                rng = self.api.builtin_range(name, method)
                if rng is not None and argc is not None and argc > rng[1] and not self.compat_name(name):
                    _truncate_args(tokens, ref.index, rng[1])
                    self.report.rewrites[f"builtin {name}: {argc - rng[1]} WaW-only argument(s) dropped"] += 1
                    argc = rng[1]
                repl = self.resolve_builtin(name, argc, where, script, included_bo2, method)
            if repl is not None:
                if extracting is not None and repl.startswith(f"{CORE}::"):
                    repl = repl.removeprefix(f"{CORE}::")     # same synthetic file
                if ref.pointer:
                    # ::name -> path::name
                    tokens[ref.index - 1].text = ""
                    tok.text = repl if "::" in repl else f"::{repl}"
                else:
                    tok.text = repl

    def extract(self, core: str, name: str) -> str | None:
        """Port one WaW framework function BO2 has no counterpart for."""
        if core in API["no_extract"] and \
                not any(name.endswith(s) for s in API["extract_exceptions"].get(core, ())):
            return None
        script = self.sources.get(core)
        if script is None or name not in script.functions:
            return None
        key = (core, name)
        fn = script.functions[name]
        if key not in self.core_funcs and uses_animations(script.tokens, fn.body_open, fn.body_close) and \
                (script.animtree or "").lower() not in self.core_animtrees:
            # needs its WaW animtree and xanims, which are not converted yet
            self.report.animtrees.setdefault(script.animtree or "?", []).append(f"{core}::{name}")
            return None
        if key not in self.core_funcs:
            self.core_funcs[key] = f"{core.split(chr(92))[-1].lstrip('_')}__{name}"
            self.core_queue.append(key)
            self.report.extracted.append(f"{core}::{name}")
        return f"{CORE}::{self.core_funcs[key]}"

    def extract_entry(self, core: str, name: str) -> str | None:
        """Extract a WaW framework entry point the converter itself calls (weapon
        registration), even from scripts whose functions are otherwise never
        extracted. Returns the name of the extracted function in CORE."""
        script = self.sources.get(core)
        if script is None or name not in script.functions:
            return None
        key = (core, name)
        if key not in self.core_funcs:
            self.core_funcs[key] = f"{core.split(chr(92))[-1].lstrip('_')}__{name}"
            self.core_queue.append(key)
            self.report.extracted.append(f"{core}::{name}")
        return self.core_funcs[key]

    def extract_level_state(self, core: str, init: str) -> str | None:
        """The statements of ``core::init`` that assign level fields the code
        already extracted from ``core`` reads, as one extracted function. A WaW
        framework init (BO2 owns the rest of it) also sets up state for the
        entry points extracted from its script, e.g. the box animation table.
        Returns the extracted function's name in CORE (None = nothing to keep)."""
        script = self.sources.get(core)
        if script is None or init not in script.functions:
            return None
        read: set[str] = set()
        for (c, n) in self.core_funcs:
            fn = script.functions.get(n) if c == core else None
            if fn is None:
                continue
            toks = script.tokens
            read |= {toks[i].low for i in range(fn.body_open + 2, fn.body_close)
                     if toks[i].kind == gsc.IDENT and toks[i - 1].text == "." and toks[i - 2].low == "level"}
        fn = script.functions[init]
        toks = script.tokens
        kept: list[gsc.Token] = []
        start, depth = fn.body_open + 1, 0
        for i in range(fn.body_open + 1, fn.body_close):
            t = toks[i]
            depth += t.text in ("(", "[", "{")
            depth -= t.text in (")", "]", "}")
            if t.text != ";" or depth:
                continue
            stmt = toks[start:i + 1]
            start = i + 1
            if len(stmt) > 3 and stmt[0].low == "level" and stmt[1].text == "." and stmt[2].low in read and \
                    any(s.text == "=" for s in stmt):
                kept += [gsc.Token(s.kind, s.text, s.pre, s.line) for s in stmt]
        if not kept:
            return None
        name = f"{core.split(chr(92))[-1].lstrip('_')}__{init}_level_state"
        tokens = [gsc.Token(gsc.IDENT, name, f"\n// {core}::{init}: level state its extracted functions read\n"),
                  gsc.Token(gsc.PUNCT, "("), gsc.Token(gsc.PUNCT, ")"), gsc.Token(gsc.PUNCT, "{", "\n"),
                  *kept, gsc.Token(gsc.PUNCT, "}", "\n"), gsc.Token(gsc.EOF, "")]
        self.keep_core_animations(script, tokens, f"{core}::{init}")
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.extracted.append(f"{core}::{init} (level state)")
        self.level_state_parts.append(gsc.emit(tokens))
        return name

    def keep_core_animations(self, script: gsc.Script, tokens: list[gsc.Token], where: str) -> None:
        """Extracted code with ``%anim`` references from a staged animtree: name
        the tree before the function (the extracted functions share one file),
        and drop references to animations with no WaW xanim (``undefined``)."""
        tree = (script.animtree or "").lower()
        uses_tree = any(t.text == "#" and tokens[i + 1].low == "animtree" for i, t in enumerate(tokens[:-1]))
        if tree not in self.core_animtrees or not (uses_tree or uses_animations(tokens, 0, len(tokens) - 1)):
            return
        available = self.core_animtrees[tree]
        tokens[0].pre += f'#using_animtree( "{script.animtree}" );\n'
        for i in range(1, len(tokens) - 1):
            t = tokens[i]
            if t.text == "%" and tokens[i + 1].kind == gsc.IDENT and \
                    tokens[i - 1].text in ("(", ",", "=", "[", "return", "[[") and \
                    tokens[i + 1].low not in available:
                self.report.unsupported.setdefault(f"XANIM {tokens[i + 1].text} (animtree {script.animtree}: "
                                                   f"no WaW xanim)", []).append(where)
                t.text = ""
                tokens[i + 1].text = "undefined"

    def translate_extracted(self, core: str, name: str) -> str:
        script = self.sources.get(core)
        fn = script.functions[name]
        tokens = [gsc.Token(t.kind, t.text, t.pre, t.line) for t in script.tokens[fn.start:fn.body_close + 1]]
        tokens.append(gsc.Token(gsc.EOF, ""))
        tokens[0].text = self.core_funcs[(core, name)]
        self.anim_neutralized = False
        tokens[0].pre = f"\n// {core}::{name} ({'map override' if core + '.gsc' in self.sources.text else 'stock'})\n"
        self.keep_core_animations(script, tokens, f"{core}::{name}")
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        return gsc.emit(tokens)

    def _split_main(self, script: gsc.Script) -> None:
        fn = script.functions.get("main")
        if fn is None:
            self.report.errors.append(f"{script.path}: map main has no main()")
            return
        tokens = script.tokens
        tokens[fn.start].text = "waw_main_pre"
        tokens[fn.body_open].text = "{\n\t" + f"{COMPAT}::init();"
        split = None
        for ref in gsc.references(tokens, fn.body_open, fn.body_close):
            if ref.qualifier is not None and norm(ref.qualifier) == "maps\\_zombiemode" and ref.name.lower() == "main":
                split = ref
                break
        if split is None:
            tokens[fn.body_close].text = "}\n\nwaw_main_post()\n{\n}"
            self.report.errors.append(f"{script.path}: no maps\\_zombiemode::main() call; whole main runs before _zm::init")
            return
        i = split.qual_index
        end = i
        while tokens[end].text != ";":
            end += 1
        # WaW's _zombiemode::main() returns only after flag_wait( "all_players_connected" ),
        # so the rest of a WaW map main runs with every player present (maps loop
        # over getPlayers() there). BO2's counterpart flag is set by _zm.
        tokens[i].text = ("}\n\nwaw_main_post()\n{\n\tcommon_scripts\\utility::flag_wait( \"initial_players_connected\" );"
                          "\t// WaW _zombiemode::main waits for all players")
        for k in range(i + 1, end + 1):
            tokens[k].text = ""
            tokens[k].kind = gsc.PUNCT
        tokens[i].kind = gsc.PUNCT

    def stub_source(self) -> str:
        lines = ["// waw2bo2: WaW functions/builtins with no BO2 translation yet. Each reports itself once.",
                 "// Listed in the stage report under 'unsupported'.", ""]
        for stub, (desc, argc) in sorted(self.stubs.items()):
            params = ", ".join(f"p{i}" for i in range(argc))
            lines += [f"{stub}({params})", "{",
                      f'    {COMPAT}::report( "GSC", "{desc.replace(chr(92), chr(92) * 2)}" );', "}", ""]
        return "\n".join(lines)


def port_map(sources: Sources, api: T6Api, map_name: str, out_root: Path,
             fx_table: dict[str, str] | None = None, animtrees: set[str] | None = None,
             core_animtrees: dict[str, set[str]] | None = None) -> PortReport:
    """Translate the map main and everything it reaches into out_root/maps/mp/waw.
    ``animtrees``: animtree names BO2 can load (stock + converted).
    ``core_animtrees``: WaW framework-override animtrees staged for BO2 -> their
    animations with WaW xanims (t6bridge.stage_core_animtrees)."""
    report = PortReport(map_main=f"maps\\{map_name}")
    tr = Translator(sources, api, report)
    tr.animtrees = {a.lower() for a in animtrees or ()}
    tr.core_animtrees = {t.lower(): {a.lower() for a in names} for t, names in (core_animtrees or {}).items()}
    main = f"maps\\{map_name}"
    if sources.get(main) is None:
        report.errors.append(f"map main {main}.gsc not found in the map scripts")
        return report
    for key in sorted(sources.text):
        p = norm(key)
        if key.endswith(".gsc") and is_core(p, sources) and key in sources.stock and \
                sources.text[key] != sources.stock[key]:
            report.core_overrides.append(p)
    out_dir = out_root / "maps" / "mp" / "waw"
    if out_dir.exists():
        for old in out_dir.rglob("*.gsc"):
            old.unlink()
    tr.queue.append(main)
    registration = {role: [tr.extract_entry(core, fn) for core, fn in entries if sources.get(core) is not None
                           and fn in sources.get(core).functions]
                    for role, entries in WEAPON_REGISTRATION.items()}
    appearance = has_waw_appearance(sources)
    if appearance:
        tr.want(LOADOUT)
    else:
        report.errors.append(f"{LOADOUT}.gsc with init_loadout/give_model not found (map or stock): players keep "
                             f"the BO2 template's characters")
    core_parts: list[str] = []

    def drain() -> None:
        while tr.queue or tr.core_queue:
            if tr.core_queue:
                core_parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
                continue
            path = tr.queue.pop(0)
            if path in tr.done:
                continue
            text = tr.translate(path, main_split=(path == main))
            tr.done[path] = text
            dst = out_root / (ported_path(path).replace("\\", "/") + ".gsc")
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(text, encoding="utf-8")
            report.ported.append(path)
            report.fx += sorted(set(re.findall(r'loadfx\s*\(\s*"([^"]+)"', sources.text.get(path + ".gsc", ""),
                                               re.IGNORECASE)))

    drain()
    # level state the extracted box code reads (set in the framework's init)
    if registration.get("box"):
        state = tr.extract_level_state(*BOX_STATE_INIT)
        if state:
            registration["box_state"] = [state]
            drain()
    core_parts += tr.level_state_parts
    (out_dir / "_waw2bo2_compat.gsc").write_text((COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"),
                                                 encoding="utf-8")
    core_head = ["// waw2bo2: WaW framework functions BO2 has no counterpart for, extracted from the map's",
                 "// framework scripts (its overrides, else stock WaW) and translated.",
                 *[f"#include {inc};" for inc in CORE_INCLUDES], ""]
    (out_dir / "_waw2bo2_core.gsc").write_text("\n".join(core_head) + "\n".join(core_parts) + "\n",
                                               encoding="utf-8")
    (out_dir / "_waw2bo2_stubs.gsc").write_text(tr.stub_source(), encoding="utf-8")
    (out_dir / "_waw2bo2_assets.gsc").write_text(assets_source(fx_table or {}, map_name), encoding="utf-8")
    (out_dir / "_waw2bo2_visions.gsc").write_text('init()\n{\n    level.waw2bo2_visions = [];\n}\n', encoding="utf-8")
    if appearance:
        (out_dir / "_waw2bo2_characters.gsc").write_text(characters_source(), encoding="utf-8")
    (out_dir / "_waw2bo2_weapons.gsc").write_text(weapons_source(registration), encoding="utf-8")
    report.weapon_registration = {role: [f for f in fns if f] for role, fns in registration.items()}
    report.characters = appearance
    report.fx = sorted(set(report.fx))
    fix_arity(out_root, api, report)
    return report


class _Linker:
    """Resolves references of the generated scripts like the game's script linker."""

    def __init__(self, out_root: Path, api: T6Api):
        self.api = api
        self.base = out_root / "maps" / "mp" / "waw"
        self.files = {("maps\\mp\\waw\\" + p.relative_to(self.base).with_suffix("").as_posix()
                       .replace("/", "\\")).lower(): p for p in self.base.rglob("*.gsc")}
        self.generated = {k: gsc.parse(p.read_text(encoding="utf-8"), k) for k, p in self.files.items()}

    def params(self, script: str, name: str) -> int | None:
        """Parameter count of script function ``script::name`` (None = undefined)."""
        s = script.lower()
        if s in self.generated:
            fn = self.generated[s].functions.get(name)
            return len(fn.params) if fn else None
        return self.api.scripts.get(s, {}).get(name)

    def target(self, script: gsc.Script, ref: gsc.Ref) -> tuple[str, int | None]:
        """("script", params) / ("builtin", None) / ("missing", None)."""
        name = ref.name.lower()
        if ref.qualifier is not None:
            n = self.params(ref.qualifier, name)
            return ("script", n) if n is not None else ("missing", None)
        if name in script.functions:
            return "script", len(script.functions[name].params)
        for inc in script.includes:
            n = self.params(inc, name)
            if n is not None:
                return "script", n
        return "builtin", None


def fix_arity(out_root: Path, api: T6Api, report: PortReport | None = None) -> int:
    """WaW ignores extra call arguments; T6 refuses to link a script call with
    more arguments than the function has parameters. Drop the extras."""
    linker = _Linker(out_root, api)
    fixed = 0
    for path, script in linker.generated.items():
        changed = False
        defs = {fn.start for fn in script.functions.values()}
        for ref in gsc.references(script.tokens):
            if ref.index in defs or ref.pointer or not script.tokens[ref.index].text:
                continue
            kind, n = linker.target(script, ref)
            argc = _argc(script.tokens, ref.index)
            if kind == "script" and argc is not None and n is not None and argc > n:
                _truncate_args(script.tokens, ref.index, n)
                changed = True
                fixed += 1
                if report is not None:
                    report.rewrites[f"call {ref.name}: {argc - n} argument(s) beyond its parameters dropped"] += 1
        if changed:
            linker.files[path].write_text(gsc.emit(script.tokens), encoding="utf-8")
    return fixed


def link_check(out_root: Path, api: T6Api) -> list[str]:
    """Resolve every reference of the generated scripts the way the game's
    script linker will: local, included, qualified, builtin (with arity)."""
    linker = _Linker(out_root, api)
    problems = []
    for path, script in sorted(linker.generated.items()):
        for inc in script.includes:
            if inc.lower() not in linker.generated and inc.lower() not in api.scripts:
                problems.append(f"{path}: #include {inc} does not exist")
        defs = {fn.start for fn in script.functions.values()}
        for ref in gsc.references(script.tokens):
            if ref.index in defs:
                continue
            name = ref.name.lower()
            where = f"{path}:{script.tokens[ref.index].line}"
            argc = None if ref.pointer else _argc(script.tokens, ref.index)
            kind, n = linker.target(script, ref)
            if kind == "missing":
                problems.append(f"{where}: {ref.qualifier}::{ref.name} not found")
                continue
            if kind == "script":
                if argc is not None and n is not None and argc > n:
                    problems.append(f"{where}: {ref.name} called with {argc} arguments, it takes {n}")
                continue
            method = None if ref.pointer else ref.method
            ok = api.builtin_accepts(name, argc or 0, method)
            if ok is None:
                what = "builtin" if method is None else ("builtin method" if method else "builtin function")
                problems.append(f"{where}: {ref.name} is neither defined, included nor a T6 {what}")
            elif not ok and argc is not None:
                lo, hi = api.builtin_range(name, method)
                problems.append(f"{where}: builtin {ref.name} called with {argc} arguments, T6 takes {lo}-{hi}")
    return problems


def assets_source(fx_table: dict[str, str], waw_map: str | None = None,
                  weapons: dict[str, str] | None = None) -> str:
    lines = ["// waw2bo2: WaW asset name -> BO2 asset name for converted assets (generated).", "init()", "{"]
    if waw_map:
        # WaW's level.script is the WaW map name (see gscport.WAW_LEVEL_FIELDS)
        lines.append(f'    level.waw_script = "{waw_map}";')
    for waw, bo2 in sorted(fx_table.items()):
        lines.append(f'    level.waw2bo2_fx["{waw}"] = "{bo2}";')
    for waw, bo2 in sorted((weapons or {}).items()):
        lines.append(f'    level.waw2bo2_weapons["{waw}"] = "{bo2}";')
        if bo2 and bo2 != waw:
            lines.append(f'    level.waw2bo2_weapon_names["{bo2}"] = "{waw}";')
    lines += ["}", ""]
    return "\n".join(lines)


LOADOUT = "maps\\_loadout"
CHARACTERS = "maps\\mp\\waw\\_waw2bo2_characters"


def has_waw_appearance(sources: Sources) -> bool:
    """WaW's player appearance contract: _loadout::init_loadout (level setup,
    precaches, level.player_viewmodel...) and give_model (body + viewmodel)."""
    script = sources.get(LOADOUT)
    return script is not None and {"init_loadout", "give_model"} <= set(script.functions)


def characters_source() -> str:
    """BO2 character hooks driving the ported WaW _loadout.

    WaW: _zombiemode::main sets level.is_zombie_level, _load::main calls
    init_loadout, _zombiemode's connect handler sets player.entity_num and the
    spawn logic calls give_model(self.pers["class"]). BO2 calls
    level.precachecustomcharacters from _zm::init and level.givecustomcharacters
    on spawn; the BO2 fields its framework reads (characterindex, favourite
    wall weapons, danger talk, tombstone index) are filled in around it."""
    ported = ported_path(LOADOUT)
    return "\n".join([
        "// waw2bo2: player bodies and viewmodel arms come from the map's WaW _loadout (generated).",
        "#include common_scripts\\utility;",
        "#include maps\\mp\\_utility;",
        "#include maps\\mp\\zombies\\_zm_utility;",
        "",
        "precache()",
        "{",
        f"    {COMPAT}::init();",
        "    level.is_zombie_level = 1;",
        f"    {ported}::init_loadout();",
        "    // WaW's _loadout names the start and last-stand weapons; BO2's",
        "    // _zm::init_levelvars sets its own defaults right after this hook",
        "    level thread waw_loadout_weapons( level.player_switchweapon, level.laststandpistol );",
        "}",
        "",
        "waw_loadout_weapons( start, pistol )",
        "{",
        "    waittillframeend;",
        f"    start = {COMPAT}::waw_weapon( start );",
        "    if ( isdefined( start ) )",
        "        level.start_weapon = start;",
        f"    pistol = {COMPAT}::waw_weapon( pistol );",
        "    if ( !isdefined( pistol ) )",
        "        return;",
        "    level.laststandpistol = pistol;",
        "    level.default_laststandpistol = pistol;",
        "    // WaW has no solo last stand with an upgraded pistol; keep the same one",
        "    level.default_solo_laststandpistol = pistol;",
        "}",
        "",
        "give()",
        "{",
        "    self detachall();",
        "    self.entity_num = self getentitynumber();",
        "    if ( !isdefined( self.characterindex ) )",
        "        self.characterindex = self.entity_num;",
        "    self.favorite_wall_weapons_list = [];",
        "    self.talks_in_danger = 0;",
        "    if ( !isdefined( self.pers[\"class\"] ) )",
        "        self.pers[\"class\"] = \"closequarters\";",
        f"    self {ported}::give_model( self.pers[\"class\"] );",
        "    self set_player_is_female( 0 );",
        "    self setmovespeedscale( 1 );",
        "    // BO2 zombies players get no sprint unless the character hook sets it",
        "    // (stock maps: 4 s, no cooldown); WaW's player_sprintTime default is 4 s",
        "    self setsprintduration( 4 );",
        "    self setsprintcooldown( 0 );",
        "    self set_player_tombstone_index();",
        "}",
        "",
    ])


WEAPONS = "maps\\mp\\waw\\_waw2bo2_weapons"
# WaW framework entry points that register a map's weapons. BO2 replaces the
# frameworks around them, so the converter calls them itself: "include" from
# the BO2 map's include step, "add" from level._zombie_custom_add_weapons.
# A map whose own main defines/calls its include list needs none of these:
# that code is ported with the map and runs in waw_main_pre.
WEAPON_REGISTRATION = {
    "include": [("maps\\dlc3_code", "include_weapons")],    # WaW DLC3 mod-tools template
    "add": [("maps\\_zombiemode_weapons", "init_weapons")],  # WaW zombiemode framework (map override or stock)
    # WaW's mystery box (map override or stock): BO2 _zm_magicbox only knows
    # its own zbarrier boxes, so the WaW box runs as WaW code
    "box": [("maps\\_zombiemode_weapons", "treasure_chest_init")],
}
# the framework init whose level assignments the box code reads (animations, flags)
BOX_STATE_INIT = ("maps\\_zombiemode_weapons", "init")
# Weapons BO2's own _zm framework hands out regardless of the map (melee
# knife, default lethal grenade, default last-stand pistol and its solo
# upgrade). They are included but never put in the box.
BO2_FRAMEWORK_WEAPONS = ("knife_zm", "frag_grenade_zm", "m1911_zm", "m1911_upgraded_zm")


def weapons_source(registration: dict[str, list[str | None]]) -> str:
    lines = ["// waw2bo2: the map's WaW weapon registration on BO2 _zm_weapons (generated).",
             "// include/add_zombie_weapon calls go through _waw2bo2_compat (WaW -> BO2 arguments).", "",
             "include_weapons()", "{",
             "    // WaW hints are plain text: prompt = display name + cost (see waw_add_zombie_weapon)",
             "    level.monolingustic_prompt_format = 1;"]
    lines += [f'    maps\\mp\\zombies\\_zm_weapons::include_zombie_weapon( "{w}", 0 );' for w in BO2_FRAMEWORK_WEAPONS]
    lines += [f"    {CORE}::{fn}();" for fn in registration.get("include", []) if fn]
    lines += ["}", "", "add_weapons()", "{",
              '    maps\\mp\\zombies\\_zm_weapons::add_zombie_weapon( "m1911_zm", "m1911_upgraded_zm", '
              '&"ZOMBIE_WEAPON_M1911", 50, "", "", undefined );']
    lines += [f"    {CORE}::{fn}();" for fn in registration.get("add", []) if fn]
    lines += ["}", "", "// the WaW mystery box, after BO2 _zm_magicbox::init (it re-inits the shared chest flags)",
              "start_box()", "{"]
    lines += [f"    {CORE}::{fn}();" for fn in registration.get("box_state", []) if fn]
    lines += [f"    {CORE}::{fn}();" for fn in registration.get("box", []) if fn]
    lines += ["}", ""]
    return "\n".join(lines)


def hook_bo2_box(main_gsc: Path) -> bool:
    """Start the WaW box right after the ported WaW main's post part (idempotent)."""
    source = main_gsc.read_text(encoding="utf-8", errors="replace")
    call = f"    level thread {WEAPONS}::start_box();\n"
    if call in source:
        return False
    post = re.search(r"^    level thread maps\\mp\\waw\\\w+::waw_main_post\(\);\n", source, re.MULTILINE)
    if post is None:
        raise ValueError(f"{main_gsc}: no waw_main_post() call to start the WaW box after")
    main_gsc.write_text(source[:post.end()] + call + source[post.end():], encoding="utf-8")
    return True


def hook_bo2_weapons(main_gsc: Path) -> bool:
    """Replace the BO2 template's weapon lists with the WaW registration
    (idempotent). The template calls include_weapons() in main() and sets
    level._zombie_custom_add_weapons."""
    source = main_gsc.read_text(encoding="utf-8", errors="replace")
    changed = re.sub(r"^(\s*)include_weapons\(\s*\);", rf"\g<1>{WEAPONS.replace(chr(92), chr(92) * 2)}::include_weapons();",
                     source, flags=re.MULTILINE)
    changed = re.sub(r"(level\._zombie_custom_add_weapons\s*=\s*)[^;]+;",
                     rf"\g<1>{WEAPONS.replace(chr(92), chr(92) * 2)}::add_weapons;", changed)
    if f"{WEAPONS}::include_weapons();" not in changed or f"{WEAPONS}::add_weapons;" not in changed:
        raise ValueError(f"{main_gsc}: no include_weapons() call / level._zombie_custom_add_weapons to hook")
    if changed != source:
        main_gsc.write_text(changed, encoding="utf-8")
    return changed != source


CHARACTER_HOOKS = {"precachecustomcharacters": "precache", "givecustomcharacters": "give"}


def hook_bo2_characters(main_gsc: Path) -> bool:
    """Point the BO2 map's character hooks at the WaW appearance (idempotent).
    The BO2 map template assigns both in its classic-mode init."""
    source = main_gsc.read_text(encoding="utf-8", errors="replace")
    changed = source
    for hook, fn in CHARACTER_HOOKS.items():
        changed = re.sub(rf"(level\.{hook}\s*=\s*)[^;]+;", rf"\g<1>{CHARACTERS.replace(chr(92), chr(92) * 2)}::{fn};",
                         changed)
    if CHARACTERS not in changed:
        raise ValueError(f"{main_gsc}: no level.precachecustomcharacters / level.givecustomcharacters to hook")
    if changed != source:
        main_gsc.write_text(changed, encoding="utf-8")
    return changed != source


PRE_ANCHOR = "    maps\\mp\\zombies\\_zm::init();"


def hook_bo2_main(main_gsc: Path, map_name: str) -> bool:
    """Call the ported WaW main around _zm::init in the BO2 map main (idempotent)."""
    source = main_gsc.read_text(encoding="utf-8", errors="replace")
    ported = ported_path(f"maps\\{map_name}")
    pre = f"    {ported}::waw_main_pre();\n"
    post = f"    level thread {ported}::waw_main_post();\n"
    if pre in source:
        return False
    if PRE_ANCHOR not in source:
        raise ValueError(f"{main_gsc}: no '{PRE_ANCHOR.strip()}' line to hook the WaW main around")
    source = source.replace(PRE_ANCHOR, pre + PRE_ANCHOR, 1)
    # post: last statement of main()
    m = re.search(r"^main\(\)\s*\{", source, re.MULTILINE)
    depth = 0
    for i in range(m.end() - 1, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                source = source[:i] + post + source[i:]
                break
    main_gsc.write_text(source, encoding="utf-8")
    return True


def bo2_scripts(out_root: Path) -> list[str]:
    """Asset names of the ported scripts (for the zone and the script compile)."""
    base = out_root / "maps" / "mp" / "waw"
    return sorted(p.relative_to(out_root).as_posix() for p in base.rglob("*.gsc")) if base.exists() else []
