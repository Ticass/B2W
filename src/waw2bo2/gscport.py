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
import copy
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from . import gsc, oneway, stockperks, zombieappearance
from .t6api import T6Api

COMPAT_DIR = Path(__file__).parent / "compat"
API = json.loads((COMPAT_DIR / "gsc_api.json").read_text(encoding="utf-8"))
COMPAT = "maps\\mp\\waw\\_waw2bo2_compat"
STUBS = "maps\\mp\\waw\\_waw2bo2_stubs"
ASSETS = "maps\\mp\\waw\\_waw2bo2_assets"
CORE = "maps\\mp\\waw\\_waw2bo2_core"
PRECACHE = "maps\\mp\\waw\\_waw2bo2_precache"
ONEWAY = "maps\\mp\\waw\\_waw2bo2_oneway"
ZOMBIEMODE = "maps\\_zombiemode"
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

    def __init__(self, roots: list[Path], iwds: list[Path], stock_root: Path | None,
                 variant_roots: list[Path] | None = None):
        self.text: dict[str, str] = {}
        # other stock releases of the framework (the WaW / Mod Tools raw
        # scripts maps are authored from), for telling map edits from stock
        self.variant_roots = list(variant_roots or [])
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
            if src is None:
                for root in self.variant_roots:
                    raw = root / (norm(path).replace('\\', '/') + ext)
                    if raw.is_file():
                        src = raw.read_text(encoding='utf-8', errors='replace')
                        self.stock[key] = src
                        break
            self._parsed[key] = gsc.parse(src, norm(path)) if src is not None else None
        return self._parsed[key]

    def stock_variants(self, path: str, ext: str = ".gsc") -> list[str]:
        """Every known stock text of a script: the stock zones' and raw ones."""
        key = norm(path) + ext
        texts = [self.stock[key]] if key in self.stock else []
        for root in self.variant_roots:
            f = root / (norm(path).replace("\\", "/") + ext)
            if f.is_file():
                texts.append(f.read_text(encoding="utf-8", errors="replace"))
        return texts

    def is_stock(self, path: str, ext: str = ".gsc") -> bool:
        return norm(path) + ext in self.stock


def is_core(path: str, sources: Sources) -> bool:
    p = norm(path)
    if p in API.get("preserved_scripts", []):
        return False
    if p in API["core_scripts"] or p in API["template_scripts"]:
        return True
    return sources.is_stock(p) and any(re.search(r, p) for r in API["core_patterns"])


LINE_COMMENT_BACKSLASH = re.compile(r"(//[^\n]*?)\\+[ \t]*(?=\r?\n|$)")


def shadowed_name(name: str) -> str:
    """The ported name of a map function that shares a T6 builtin's name."""
    return "waw_" + name


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


def bridge_contact_kill_attacks(tokens: list[gsc.Token], script: gsc.Script) -> int:
    """Let source proximity/ragdoll kills own combat during their monitor.

    T6's ASD melee can deliver a swipe before a WaW polling kick runs.
    Match the source behavior, not weapon, model, or map names. Keep its
    radius, damage, attribution, ragdoll and timer unchanged.
    """
    fixes = 0
    for fn in script.functions.values():
        body = tokens[fn.body_open + 1:fn.body_close]
        code = "".join(t.low for t in body)
        if not ("getplayers()" in code and "isalive(self)" in code
                and re.search(r"distance\(self\.origin,[a-z_]\w*\[[^]]+\]\.origin\)<=\d", code)
                and "selfstartragdoll()" in code and "selflaunchragdoll(" in code
                and re.search(r"selfdodamage\(self\.health\+\d", code)
                and "wait" in code):
            continue
        # A source function returning a value is not this monitor pattern.
        if any(t.low == "return" and body[i + 1].text != ";"
               for i, t in enumerate(body[:-1])):
            continue
        begin = f"\n    self {COMPAT}::waw_contact_kill_begin();"
        end = f"self {COMPAT}::waw_contact_kill_end();"
        tokens[fn.body_open].text += begin
        for i, token in enumerate(body):
            if token.low == "return":
                token.text = "{ " + end + " return"
                body[i + 1].text = "; }"
        tokens[fn.body_close].text = end + "\n}"
        fixes += 1
    return fixes


CONDITION_KEYWORDS = {"if", "while", "for", "foreach", "switch"}


def fix_syntax(tokens: list[gsc.Token]) -> int:
    """WaW syntax the T6 compiler rejects. ``(a.b).size``: T6 cannot access a
    member of a parenthesised expression; the parentheses are redundant when
    they hold a plain access path. Returns the number of fixes."""
    fixes = 0
    for t in tokens:
        t.pre, repaired = gsc.repair_block_comments(t.pre)
        fixes += repaired
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
        after = tokens[j + 1] if j + 1 < len(tokens) else None
        # ``(self) IsTouching( x )`` / ``(self) thread f()``: WaW accepts a
        # parenthesised method caller, T6 expects ')' (Alcatraz's elevator).
        # Not the condition of ``if (x) foo();`` / ``while (y) bar();``.
        method_call = after is not None and after.kind == gsc.IDENT and j + 2 < len(tokens) and \
            (tokens[j + 2].text == "(" or after.low in ("thread", "childthread")) and \
            not (prev is not None and prev.low in CONDITION_KEYWORDS)
        if simple and j > i + 1 and tokens[j].text == ")" and after is not None and (after.text == "." or method_call):
            t.text = ""
            tokens[j].text = ""
            fixes += 1
    return fixes


# level fields both frameworks own under the same name with different values.
# WaW scripts keep seeing the WaW value (set by _waw2bo2_assets::init); the
# BO2 framework keeps its own. level.script: WaW map name vs the BO2 project.
# level.chests / chest_index: WaW's box triggers vs BO2 _zm_magicbox structs
# (BO2 powerups such as fire sale iterate level.chests as BO2 structs).
WAW_LEVEL_FIELDS = {"script": "waw_script", "chests": "waw_chests", "chest_index": "waw_chest_index", "_effect": "waw_effect"}
WAW_BOX_FLAGS = {'moving_chest_enabled', 'moving_chest_now', 'chest_has_been_used'}


def rename_level_fields(tokens: list[gsc.Token]) -> int:
    renamed = 0
    for i in range(2, len(tokens)):
        t = tokens[i]
        # Both box controllers initialize/use these flags. Source code must
        # never clear BO2's state (or have BO2 clear a source relocation).
        if t.kind == gsc.STRING and t.text.startswith('"') and t.text[1:-1] in WAW_BOX_FLAGS:
            t.text = '"waw_' + t.text[1:-1] + '"'
            renamed += 1
        if t.kind == gsc.IDENT and t.low in WAW_LEVEL_FIELDS and tokens[i - 1].text == "." and \
                tokens[i - 2].low == "level" and (i + 1 >= len(tokens) or tokens[i + 1].text != "("):
            t.text = WAW_LEVEL_FIELDS[t.low]
            renamed += 1
    return renamed


def level_fields_read(tokens: list[gsc.Token], start: int = 2, end: int | None = None) -> set[str]:
    """``level.<field>`` names (lower-case) used in tokens[start:end]."""
    end = len(tokens) if end is None else end
    return {tokens[i].low for i in range(max(start, 2), end)
            if tokens[i].kind == gsc.IDENT and tokens[i - 1].text == "." and tokens[i - 2].low == "level"}


def bridge_revive_reads(tokens: list[gsc.Token]) -> int:
    """WaW's player being_revived field is absent from T6's player schema.

    Resolve reads against the native revive trigger while keeping source
    writes (custom solo-revive handlers) and explicit isdefined probes intact.
    """
    count = 0
    for i, token in enumerate(tokens):
        if token.low != "being_revived" or tokens[i - 1].text != ".":
            continue
        if tokens[i + 1].text in ("=", "+=", "-=", "++", "--"):
            continue
        j, depth = i - 1, 0
        while j > 0:
            prev = tokens[j - 1]
            if depth == 0 and prev.text != "." and not (
                (prev.kind == gsc.IDENT or prev.text in ("]", ")"))
                and tokens[j].text in (".", "[")
            ):
                break
            if prev.text in ("]", ")"):
                depth += 1
            elif prev.text in ("[", "("):
                depth -= 1
            j -= 1
        if j >= 2 and tokens[j - 1].text == "(" and tokens[j - 2].low == "isdefined":
            continue
        receiver = gsc.emit(tokens[j:i - 1]).lstrip()
        tokens[j].text = f"{COMPAT}::waw_being_revived( {receiver} )"
        tokens[j].kind = gsc.PUNCT
        for t in tokens[j + 1:i + 1]:
            t.text = t.pre = ""
        count += 1
    return count


def bridge_player_stat_reads(tokens: list[gsc.Token]) -> int:
    """Read stock WaW session stats from T6's native persistent counters.

    Keep custom keys, assignments/increments and isdefined probes authored.
    """
    count = 0
    keys = {"kills", "score", "downs", "revives", "perks", "headshots", "zombie_gibs"}
    for i in range(2, len(tokens) - 4):
        if (tokens[i].low != "stats" or tokens[i - 1].text != "."
                or tokens[i + 1].text != "[" or tokens[i + 2].kind != gsc.STRING
                or tokens[i + 2].text.strip('"') not in keys or tokens[i + 3].text != "]"
                or tokens[i + 4].text in ("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++", "--")):
            continue
        j, depth = i - 1, 0
        while j > 0:
            prev = tokens[j - 1]
            if depth == 0 and prev.text != "." and not (
                (prev.kind == gsc.IDENT or prev.text in ("]", ")"))
                and tokens[j].text in (".", "[")
            ):
                break
            if prev.text in ("]", ")"):
                depth += 1
            elif prev.text in ("[", "("):
                depth -= 1
            j -= 1
        if j and tokens[j - 1].text in ("++", "--"):
            continue
        if j >= 2 and tokens[j - 1].text == "(" and tokens[j - 2].low == "isdefined":
            continue
        receiver = gsc.emit(tokens[j:i - 1]).lstrip()
        tokens[j].text = f"{COMPAT}::waw_player_stat( {receiver}, {tokens[i + 2].text} )"
        tokens[j].kind = gsc.PUNCT
        for t in tokens[j + 1:i + 4]:
            t.text = t.pre = ""
        count += 1
    return count


def bridge_font_scales(tokens: list[gsc.Token]) -> int:
    """Preserve WaW's effective networked scale, including calculated values."""
    import math
    changed = 0
    for i, token in enumerate(tokens):
        if token.low != 'fontscale' or i < 1 or tokens[i - 1].text != '.' or tokens[i + 1].text != '=':
            continue
        start, end, depth = i + 2, i + 2, 0
        while end < len(tokens):
            if tokens[end].text == ';' and not depth:
                break
            depth += tokens[end].text in ('(', '[')
            depth -= tokens[end].text in (')', ']')
            end += 1
        if end == len(tokens):
            continue
        rhs = gsc.emit(tokens[start:end]).strip()
        if end == start + 1 and tokens[start].kind == gsc.NUMBER:
            scale = float(tokens[start].text)
            decoded = 1 + (math.floor((scale - 1) * 10 + 0.5) & 63) / 10
            if abs(decoded - scale) < 1e-6:
                continue
            replacement = f'{decoded:g}'
        else:
            replacement = f'{COMPAT}::waw_fontscale( {rhs} )'
        tokens[start].text = replacement
        tokens[start].kind = gsc.PUNCT
        for t in tokens[start + 1:end]:
            t.text = t.pre = ''
        changed += 1
    return changed


def bridge_hud_cleanup(tokens: list[gsc.Token], index: int) -> bool:
    """Call the idempotent native HUD cleanup by argument, not receiver.

    Called only after resolving destroyElem to BO2's HUD utility. A source
    implementation with the same name retains its own behavior.
    """
    if index < 1 or tokens[index - 1].kind != gsc.IDENT:
        return False
    start = index - 1
    # Support simple variables and dotted fields; more complex receivers
    # are left intact rather than risking a change to expression ownership.
    while start >= 2 and tokens[start - 1].text == "." and tokens[start - 2].kind == gsc.IDENT:
        start -= 2
    if start and tokens[start - 1].text in (".", "]", ")", "thread"):
        return False
    receiver = gsc.emit(tokens[start:index]).strip()
    tokens[start].text = f"{COMPAT}::waw_destroy_hud_elem( {receiver} )"
    tokens[start].kind = gsc.PUNCT
    for token in tokens[start + 1:index + 3]:
        token.text = token.pre = ""
    return True


def keep_bo2_deathanims(tokens: list[gsc.Token]) -> int:
    """``x.deathanim = <WaW xanim>;`` -> ``x.deathanim = x.deathanim;``. BO2's
    deathanim is an ASD state name (animscripts/zm_death plays it with
    setanimstatefromasd, defaulting to zm_death + legs suffix); a WaW animation
    there is no state. The statement stays (it may be an unbraced if/else
    branch) and the WaW right-hand side, e.g. a read of WaW framework anim
    tables, is not evaluated. Returns the number of rewrites."""
    count = 0
    for i, t in enumerate(tokens):
        if t.low != "deathanim" or t.kind != gsc.IDENT or tokens[i - 1].text != "." or tokens[i + 1].text != "=":
            continue
        # the assigned path: back over a.b[c].d
        j, depth = i - 1, 0
        while j > 0:
            p = tokens[j - 1]
            if depth == 0 and p.text != "." and not \
                    ((p.kind == gsc.IDENT or p.text in ("]", ")")) and tokens[j].text in (".", "[")):
                break   # not part of the postfix path (e.g. an if condition's ')', 'else')
            if p.text in ("]", ")"):
                depth += 1
            elif p.text in ("[", "("):
                depth -= 1
            j -= 1
        target = "".join(x.text for x in tokens[j:i + 1])
        k, depth = i + 2, 0
        while tokens[k].kind != gsc.EOF and (tokens[k].text != ";" or depth):
            depth += tokens[k].text in ("(", "[", "{")
            depth -= tokens[k].text in (")", "]", "}")
            k += 1
        tokens[i + 2].text = target
        tokens[i + 2].pre = " "
        tokens[i + 2].kind = gsc.PUNCT
        for m in range(i + 3, k + 1):
            tokens[m].pre = ""
            if m < k:
                tokens[m].text = ""
                tokens[m].kind = gsc.PUNCT
        count += 1
    return count


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


def bridge_launch_triggers(tokens: list[gsc.Token]) -> int:
    """Include the activation pad in chained trigger velocity launchers.

    Some WaW launchers target a taller push volume above their activation
    pad. T6 players must receive the first impulse while still on that pad.
    Match the trigger relationship and velocity behavior, never map names.
    """
    script = gsc.parse(gsc.emit(tokens), "launch-translation")
    fixes = 0
    for fn in script.functions.values():
        start, end = fn.body_open, fn.body_close
        roots = {}
        for i in range(start, end - 9):
            words = [t.low for t in tokens[i:i + 10]]
            if (words[1:4] == ["=", "getent", "("] and words[5:8] == [".", "target", ","]
                    and tokens[i + 8].text.lower() == '"targetname"' and words[9] == ")"
                    and words[4] in fn.params):
                roots[words[0]] = tokens[i + 4].text
        for i in range(start, end - 8):
            words = [t.low for t in tokens[i:i + 9]]
            if (words[:5] != ["if", "(", "self", "istouching", "("]
                    or words[6:] != [")", ")", "{"] or words[5] not in roots):
                continue
            depth, j = 1, i + 9
            while j < end and depth:
                depth += (tokens[j].text == "{") - (tokens[j].text == "}")
                j += 1
            if not any(t.low == "setvelocity" for t in tokens[i + 9:j]):
                continue
            tokens[i + 2].text = f"self isTouching({roots[words[5]]}) || self"
            fixes += 1
    return fixes


def lower_stationary_fx_carriers(tokens: list[gsc.Token]) -> int:
    """Play effects on inert tag_origin carriers at their authored transform.

    The common WaW stationary emitter idiom attaches a model to a struct
    solely to play FX. Keeping that permanent network entity in T6 can fill
    CL_GetSnapshot's 512 slots. Match the complete carrier usage, excluding
    triggered/moving/deletable emitters; direct PlayFX retains asset looping.
    """
    bodies = []
    i = 0
    while i < len(tokens):
        if tokens[i].text == '{':
            close = gsc._match(tokens, i, '{', '}')
            if i and tokens[i - 1].text == ')':
                bodies.append((i + 1, close))
            i = close + 1
        else:
            i += 1
    changed = 0
    patterns = [
        'self.fx = spawn("script_model", self.origin);',
        'self.fx setmodel("tag_origin");',
        'self.fx.angles = self.angles;',
        'self.fx.origin = self.origin;',
        'self.fx linkto(self, fxTag);',
    ]
    patterns = [[t.low for t in gsc.tokenize(p)[:-1]] for p in patterns]
    for begin, end in bodies:
        words = [t.low for t in tokens]
        body = words[begin:end]
        if any(w in body for w in ('waittill', 'waittill_any', 'moveto', 'rotateyaw', 'delete', 'endon')):
            continue
        if 'script_noteworthy' not in body or not any(
                words[i:i + 4] == ['fxtag', '=', '"tag_origin"', ';'] for i in range(begin, end - 3)):
            continue
        ranges = []
        for pattern in patterns:
            hits = [i for i in range(begin, end - len(pattern) + 1)
                    if words[i:i + len(pattern)] == pattern]
            if len(hits) != 1:
                break
            ranges.append((hits[0], hits[0] + len(pattern)))
        if len(ranges) != len(patterns):
            continue
        calls = [i for i in range(begin, end) if words[i] == 'playfxontag']
        carriers = [i for i in range(begin, end - 2) if words[i:i + 3] == ['self', '.', 'fx']]
        if len(calls) != 1 or len(carriers) != 6:
            continue
        call = calls[0]
        if _argc(tokens, call) != 3:
            continue
        close = gsc._match(tokens, call + 1, '(', ')')
        # All remaining carrier references must be the attachment receiver.
        tail = ['self', '.', 'fx', ',', 'fxtag']
        if words[close - 5:close] != tail or words[close - 6] != ',':
            continue
        tokens[call].text = COMPAT + '::waw_playfx'
        tokens[call].kind = gsc.PUNCT
        tokens[close - 5].text = ('self.origin, anglestoforward(self.angles), '
                                 'anglestoup(self.angles)')
        tokens[close - 5].kind = gsc.PUNCT
        for t in tokens[close - 4:close]:
            t.text = t.pre = ''
        for start, stop in ranges:
            for t in tokens[start:stop]:
                t.text = t.pre = ''
        changed += 1
    return changed


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
        self.bo2_stock_perks = False

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
    def shadows_builtin(self, name: str) -> bool:
        """A map function named like a T6 builtin (back-ported BO2 helpers such
        as getFirstArrayKey): the T6 compiler rejects the definition."""
        return self.api.builtin_range(name.lower()) is not None

    def translate(self, path: str, main_split: bool = False) -> str:
        script = self.sources.get(path)
        tokens = script.tokens
        self.report.rewrites["stationary FX carriers lowered to authored-position FX"] += lower_stationary_fx_carriers(tokens)
        self.report.rewrites["launch volume includes its activation trigger"] += bridge_launch_triggers(tokens)
        where_file = self.sources.origin.get(norm(path) + ".gsc", path)
        included_bo2: set[str] = set()
        new_includes: list[str] = []
        # includes
        for t in tokens:
            if t.kind != gsc.DIRECTIVE or not t.text.lower().startswith("#include"):
                continue
            inc = norm(gsc.INCLUDE_RE.match(t.text).group(1))
            if self.bo2_stock_perks and stockperks.is_perk_framework(inc):
                t.text = r"#include maps\mp\zombies\_zm_perks;"
                included_bo2.add(r"maps\mp\zombies\_zm_perks")
                continue
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
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        for i, token in enumerate(tokens):
            if i and token.low == "playername" and tokens[i - 1].text == ".":
                token.text = "name"
                self.report.rewrites["WaW playername field -> T6 name"] += 1
        defs = {fn.start for fn in script.functions.values()}
        for name, fn in script.functions.items():
            if self.shadows_builtin(name):
                tokens[fn.start].text = shadowed_name(tokens[fn.start].text)
                self.report.rewrites["map functions named like T6 builtins renamed (waw_*)"] += 1
        self.resolve_refs(tokens, script, where_file, included_bo2, defs)
        self.report.rewrites["WaW contact-kill monitors own AI attacks"] += bridge_contact_kill_attacks(tokens, script)
        self.report.rewrites["WaW being_revived reads bridged to T6 revive state"] += bridge_revive_reads(tokens)
        self.report.rewrites["WaW session stat reads bridged to T6 counters"] += bridge_player_stat_reads(tokens)
        self.report.rewrites["WaW HUD network font scales decoded"] += bridge_font_scales(tokens)
        header = (f"// Translated from World at War by waw2bo2 (gscport). Source: {where_file}\n")
        return header + gsc.emit(tokens)

    def resolve_refs(self, tokens: list[gsc.Token], script: gsc.Script, where_file: str,
                     included_bo2: set[str], skip: set[int], extracting: str | None = None) -> None:
        """Rewrite every function reference in ``tokens`` (code of ``script``).
        ``extracting``: the core script whose functions are being extracted;
        its own functions are extracted too instead of kept."""
        # WaW custom scripts may access the hint registry directly. Keep these
        # accesses in the same source registry as the adapted helper calls.
        for i in range(2, len(tokens)):
            if tokens[i].text.lower() == "zombie_hints" and tokens[i - 1].text == "." and tokens[i - 2].text.lower() == "level":
                tokens[i].text = "waw2bo2_hints"
        for ref in gsc.references(tokens):
            if ref.index in skip or not tokens[ref.index].text:
                continue    # a definition, or inside arguments already dropped
            tok = tokens[ref.index]
            name = ref.name.lower()
            where = f"{where_file}:{tok.line}"
            argc = None if ref.pointer else _argc(tokens, ref.index)
            perk_owner = norm(ref.qualifier) if ref.qualifier else self.owner(script, name)
            if self.bo2_stock_perks and stockperks.is_perk_framework(perk_owner):
                native = r"maps\mp\zombies\_zm_perks"
                if name == "init":
                    repl = f"{COMPAT}::stock_perks_noop"
                elif name in ('perk_hud_create', 'perk_think'):
                    repl = f"{COMPAT}::stock_perks_owned_by_bo2"
                elif name == 'regret_purchase':
                    repl = f"{COMPAT}::stock_perks_no_refund"
                elif name == 'play_no_money_perk_dialog':
                    repl = f"{COMPAT}::stock_perks_no_money"
                elif name == 'perk_vo':
                    repl = r"maps\mp\zombies\_zm_audio::perk_vox"
                elif self.api.defines(native, name):
                    repl = f"{native}::{name}"
                else:
                    self.report.errors.append(f"{where}: stock BO2 perks have no equivalent for {perk_owner}::{name}")
                    repl = self.stub(f"{perk_owner}::{name}", name, argc, where)
                if ref.qualifier:
                    tokens[ref.qual_index].text = ""
                    tokens[ref.qual_index + 1].text = ""
                elif ref.pointer:
                    tokens[ref.index - 1].text = ""
                tok.text = repl
                continue
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
                        if self.shadows_builtin(name):
                            tok.text = shadowed_name(tok.text)
                continue
            owner = self.owner(script, name)
            if owner is not None and not is_core(owner, self.sources) and extracting is None \
                    and self.shadows_builtin(name):
                # the map's own function, renamed where it is defined
                tok.text = shadowed_name(tok.text)
                continue
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
            if (name == "destroyelem" and ref.method and argc == 0 and
                    ((owner is not None and API["core_scripts"].get(owner) == r"maps\mp\gametypes_zm\_hud_util") or
                     repl == r"maps\mp\gametypes_zm\_hud_util::destroyelem")):
                if bridge_hud_cleanup(tokens, ref.index):
                    self.report.rewrites["idempotent native HUD cleanup"] += 1
                    continue
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

    def extract_level_state(self, core: str, init: str, read: set[str] | None = None) -> str | None:
        """The statements of ``core::init`` that assign level fields the code
        already extracted from ``core`` reads (or the given ``read`` fields), as
        one extracted function. A WaW framework init (BO2 owns the rest of it)
        also sets up state for the entry points extracted from its script, e.g.
        the box animation table.
        Returns the extracted function's name in CORE (None = nothing to keep)."""
        script = self.sources.get(core)
        if script is None or init not in script.functions:
            return None
        if read is None:
            read = set()
            for (c, n) in self.core_funcs:
                fn = script.functions.get(n) if c == core else None
                if fn is None:
                    continue
                toks = script.tokens
                read |= level_fields_read(toks, fn.body_open + 2, fn.body_close)
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
        tree = (script.animtree or "").lower()
        if tree and tree not in self.core_animtrees and tree not in self.animtrees:
            neutralize_animations(tokens, script.animtree)
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.extracted.append(f"{core}::{init} (level state)")
        self.level_state_parts.append(gsc.emit(tokens))
        return name

    def extract_damage_prelude(self) -> str | None:
        """A map's _zombiemode override may put its own rules in front of WaW's
        player_damage_override (e.g. a shield absorbing zombie hits). BO2's _zm
        owns player damage, so those leading statements become a BO2 player
        damage callback: a bare WaW return (no finishPlayerDamage) drops the
        hit, i.e. 0 damage; falling through keeps the (possibly changed) damage.
        Returns the name of the extracted registration function in CORE."""
        key = ZOMBIEMODE + ".gsc"
        variants = self.sources.stock_variants(ZOMBIEMODE)
        if key not in self.sources.text or not variants or self.sources.text[key] in variants:
            return None
        script = self.sources.get(ZOMBIEMODE)
        fn = script.functions.get(DAMAGE_OVERRIDE)
        if fn is None:
            return None
        toks = script.tokens
        where = self.sources.origin.get(key, ZOMBIEMODE)

        def text(ts: list[gsc.Token], span: tuple[int, int]) -> str:
            return " ".join(t.low for t in ts[span[0]:span[1]])

        stmts = [text(toks, s) for s in top_level_statements(toks, fn.body_open, fn.body_close)]
        def header(ts: list[gsc.Token], span: tuple[int, int]) -> str:
            # An edited stock branch still marks the end of the map prelude.
            # Match its complete control expression, never just the keyword.
            start, end = span
            if ts[start].low != "if" or ts[start + 1].text != "(":
                return text(ts, span)
            depth = 0
            for i in range(start + 1, end):
                depth += ts[i].text == "("
                depth -= ts[i].text == ")"
                if depth == 0:
                    return text(ts, (start, i + 1))
            return text(ts, span)

        map_spans = top_level_statements(toks, fn.body_open, fn.body_close)
        # the map's own rules end where the stock body it was edited from starts
        leads = []
        for variant in variants:
            stock = gsc.parse(variant, ZOMBIEMODE)
            base = stock.functions.get(DAMAGE_OVERRIDE)
            spans = top_level_statements(stock.tokens, base.body_open, base.body_close) if base else []
            if spans and text(stock.tokens, spans[0]) in stmts:
                leads.append(stmts.index(text(stock.tokens, spans[0])))
            elif spans:
                anchor = header(stock.tokens, spans[0])
                leads.extend(i for i, span in enumerate(map_spans) if header(toks, span) == anchor)
        if not leads:
            self.report.errors.append(f"{where}: {DAMAGE_OVERRIDE} does not keep a stock WaW body; the map's "
                                      f"damage rules are not ported")
            return None
        lead = min(leads)
        stmts = top_level_statements(toks, fn.body_open, fn.body_close)
        if lead == 0:
            return None
        body = copy.deepcopy(toks[stmts[0][0]:stmts[lead - 1][1]])
        # Some solo-revive preludes finish a zero-damage hit before returning.
        # A T6 callback must leave damage application to _zm; this exact
        # zero-hit/return idiom is equivalent to the callback cancelling it.
        for ref in gsc.references(body):
            if (ref.pointer or ref.name.lower() != "finishplayerdamagewrapper"
                    or norm(ref.qualifier or "") != r"maps\_callbackglobal"
                    or ref.qual_index is None or ref.qual_index < 1
                    or body[ref.qual_index - 1].low != "self"):
                continue
            opening = ref.index + 1
            closing = gsc._match(body, opening, "(", ")")
            args, begin, depth = [], opening + 1, 0
            for i in range(begin, closing):
                if body[i].text in ("(", "["):
                    depth += 1
                elif body[i].text in (")", "]"):
                    depth -= 1
                elif body[i].text == "," and depth == 0:
                    args.append(body[begin:i])
                    begin = i + 1
            args.append(body[begin:closing])
            if (len(args) != 11 or len(args[2]) != 1 or args[2][0].text != "0"
                    or [t.text.lower() for t in body[closing + 1:closing + 4]] != [";", "return", ";"]):
                continue
            start = ref.qual_index - 1
            body[start].text = f"{fn.params[2]} = 0;"
            body[start].kind = gsc.PUNCT
            for token in body[start + 1:closing + 2]:
                token.text = token.pre = ""
            self.report.rewrites["WaW zero-damage finish/return -> T6 damage cancellation"] += 1
        if len(fn.params) != 11 or any(t.low in ("finishplayerdamage", "finishplayerdamagewrapper") for t in body):
            self.report.errors.append(f"{where}: {DAMAGE_OVERRIDE} prelude applies damage itself; not ported")
            return None
        name = f"{ZOMBIEMODE.split(chr(92))[-1].lstrip('_')}__{DAMAGE_OVERRIDE}_prelude"
        # BO2 callbacks get WaW's parameters without modelIndex (the 10th)
        params = fn.params[:9] + fn.params[10:]
        model_index = fn.params[9].lower()
        reset = f"\t{fn.params[9]} = undefined;\n" if any(t.low == model_index for t in body) else ""
        source = (f"{name}( {', '.join(params)} )\n{{\n{reset}" + gsc.emit(body) +
                  f"\n\treturn {fn.params[2]};\n}}\n")
        tokens = gsc.tokenize(source)
        for i in range(len(tokens) - 1, 0, -1):
            if tokens[i].low == "return" and tokens[i + 1].text == ";":
                tokens.insert(i + 1, gsc.Token(gsc.NUMBER, "0", " "))
        tokens[0].pre = f"\n// {ZOMBIEMODE}::{DAMAGE_OVERRIDE}: the map's leading rules, as a BO2 damage callback\n"
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=ZOMBIEMODE)
        self.report.extracted.append(f"{ZOMBIEMODE}::{DAMAGE_OVERRIDE} (map prelude -> damage callback)")
        self.level_state_parts.append(gsc.emit(tokens) + f"\n{name}_register()\n{{\n"
                                      f"\tmaps\\mp\\zombies\\_zm::register_player_damage_callback( ::{name} );\n}}\n")
        return f"{name}_register"

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
        tree = (script.animtree or "").lower()
        uses_tree = any(t.text == "#" and tokens[i + 1].low == "animtree" for i, t in enumerate(tokens[:-1]))
        if uses_tree and tree not in self.core_animtrees:
            # The extracted functions share one file, so the source script's
            # #using_animtree does not come along (e.g. _spawner's
            # UseAnimTree( #animtree ) with no %anim: "trying to use animtree
            # without specified using animtree").
            if tree in self.animtrees:
                tokens[0].pre += f'#using_animtree( "{script.animtree}" );\n'
            else:
                neutralize_animations(tokens, script.animtree or "")
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.rewrites["WaW HUD network font scales decoded"] += bridge_font_scales(tokens)
        return gsc.emit(tokens)

    def framework_hooks(self) -> list[str]:
        """Calls WaW's _zombiemode::main (usually the map's override) makes
        into scripts that are not framework, e.g. a custom perk pack's init().
        BO2's _zm::init replaces that main, so the port makes them itself."""
        zm = self.sources.get(ZOMBIEMODE)
        fn = zm.functions.get("main") if zm is not None else None
        if fn is None:
            return []
        where_file = self.sources.origin.get(ZOMBIEMODE + ".gsc", ZOMBIEMODE)
        calls = []
        for ref in gsc.references(zm.tokens, fn.body_open, fn.body_close):
            if ref.qualifier is None or ref.pointer or ref.method:
                continue
            target = norm(ref.qualifier)
            if self.bo2_stock_perks and stockperks.is_perk_framework(target):
                continue
            if is_core(target, self.sources):
                continue
            dep = self.sources.get(target)
            hook = dep.functions.get(ref.name.lower()) if dep is not None else None
            call = f"{ported_path(target)}::{ref.name}();"
            if hook is None or call in calls:
                continue
            if hook.params or _argc(zm.tokens, ref.index):
                self.report.errors.append(f"{where_file}:{zm.tokens[ref.index].line}: framework hook "
                                          f"{target}::{ref.name} takes arguments; not called")
                continue
            self.want(target)
            calls.append(call)
            self.report.rewrites[f"framework hook {target}::{ref.name} -> waw_main_post"] += 1
        return calls

    def _split_main(self, script: gsc.Script) -> None:
        fn = script.functions.get("main")
        if fn is None:
            self.report.errors.append(f"{script.path}: map main has no main()")
            return
        tokens = script.tokens
        # before the player wait: hooks register connect/spawn handlers and precache
        hooks = "".join(f"\n\t{call}" for call in self.framework_hooks())
        if hooks:
            hooks = "\n\t// WaW _zombiemode::main inits of the map's own scripts" + hooks
        tokens[fn.start].text = "waw_main_pre"
        tokens[fn.body_open].text = ("{\n\t" + f"{COMPAT}::init();\n\t{CORE}::framework_level_state();"
                                     f"\n\t{PRECACHE}::init();\n\t{ONEWAY}::init();")
        split = None
        for ref in gsc.references(tokens, fn.body_open, fn.body_close):
            if ref.qualifier is not None and norm(ref.qualifier) == "maps\\_zombiemode" and ref.name.lower() == "main":
                split = ref
                break
        if split is None:
            tokens[fn.body_close].text = "}\n\nwaw_main_post()\n{" + hooks + "\n}"
            self.report.errors.append(f"{script.path}: no maps\\_zombiemode::main() call; whole main runs before _zm::init")
            return
        i = split.qual_index
        end = i
        while tokens[end].text != ";":
            end += 1
        # WaW's _zombiemode::main() returns only after flag_wait( "all_players_connected" ),
        # so the rest of a WaW map main runs with every player present (maps loop
        # over getPlayers() there). BO2's counterpart flag is set by _zm.
        tokens[i].text = ("}\n\nwaw_main_post()\n{" + hooks +
                          "\n\tcommon_scripts\\utility::flag_wait( \"initial_players_connected\" );"
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
             core_animtrees: dict[str, set[str]] | None = None, bo2_stock_perks: bool = False,
             actor_types: set[str] = frozenset(), actor_template: str | None = None) -> PortReport:
    """Translate the map main and everything it reaches into out_root/maps/mp/waw.
    ``animtrees``: animtree names BO2 can load (stock + converted).
    ``core_animtrees``: WaW framework-override animtrees staged for BO2 -> their
    animations with WaW xanims (t6bridge.stage_core_animtrees)."""
    if actor_types:
        sources = copy.copy(sources)
        sources.text = dict(sources.text)
        sources._parsed = {}
        for name in sorted(actor_types):
            path = 'aitype\\' + name
            script = sources.get(path)
            if script is None:
                raise ValueError(f'WaW zombie actor {path}.gsc missing: donor appearance is forbidden')
            original = sources.text.get(path + '.gsc', sources.stock.get(path + '.gsc'))
            sources.text[path + '.gsc'] = zombieappearance.appearance_script(original)
            sources._parsed.pop(path + '.gsc', None)
    if bo2_stock_perks:
        sources = copy.copy(sources)
        sources.text = {key: stockperks.translate_literals(text) for key, text in sources.text.items()}
        sources.stock = {key: stockperks.translate_literals(text) for key, text in sources.stock.items()}
        sources._parsed = {}
    report = PortReport(map_main=f"maps\\{map_name}")
    tr = Translator(sources, api, report)
    tr.bo2_stock_perks = bo2_stock_perks
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
    for old in (out_root / 'aitype').rglob('waw_*') if (out_root / 'aitype').exists() else ():
        if old.is_file() and old.read_text(encoding='utf-8').startswith('// waw2bo2:'):
            old.unlink()
    tr.queue.append(main)
    for name in sorted(actor_types):
        tr.want('aitype\\' + name)
    hint_registry = tr.extract_entry(r"maps\_zombiemode", "init_strings")
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
    # WaW framework tables BO2 never builds that the ported scripts read
    # (e.g. level._zombie_tesla_death for a custom Electric Cherry)
    read = set().union(*(level_fields_read(gsc.tokenize(text)) for text in tr.done.values()))
    framework_state = [state for core, init in FRAMEWORK_STATE_INITS
                       if (state := tr.extract_level_state(core, init, read))]
    if hint_registry:
        framework_state.append(hint_registry)
    damage_prelude = tr.extract_damage_prelude()
    if damage_prelude:
        framework_state.append(damage_prelude)
    drain()
    core_parts += tr.level_state_parts
    core_parts.append("\n// level state of WaW framework inits BO2 does not run (called by waw_main_pre)\n"
                      "framework_level_state()\n{\n" + "".join(f"\t{fn}();\n" for fn in framework_state) + "}\n")
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
    (out_dir / "_waw2bo2_precache.gsc").write_text(precache_source([]), encoding="utf-8")
    # one-way clip sheets come from the collision (t6bridge.port_scripts)
    (out_dir / "_waw2bo2_oneway.gsc").write_text(oneway.oneway_source([]), encoding="utf-8")
    if appearance:
        (out_dir / "_waw2bo2_characters.gsc").write_text(characters_source(), encoding="utf-8")
    (out_dir / "_waw2bo2_weapons.gsc").write_text(weapons_source(registration), encoding="utf-8")
    report.weapon_registration = {role: [f for f in fns if f] for role, fns in registration.items()}
    report.characters = appearance
    report.fx = sorted(set(report.fx))
    for name in sorted(actor_types):
        if actor_template is None:
            raise ValueError('T6 actor template missing')
        target = out_root / 'aitype' / (zombieappearance.actor_name(name) + '.gsc')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(zombieappearance.native_actor(actor_template, ported_path('aitype\\' + name)),
                          encoding='utf-8')
        client = out_root / 'aitype/clientscripts' / (zombieappearance.actor_name(name) + '.csc')
        client.parent.mkdir(parents=True, exist_ok=True)
        client.write_text(zombieappearance.native_client_actor(name), encoding='utf-8')
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


# Native tactical gameplay helpers give/check these runtime weapon names even
# when the map's box and weapon definitions retain their original WaW names.
BO2_TACTICAL_RUNTIME_WEAPONS = {"zombie_cymbal_monkey": "cymbal_monkey_zm"}


def assets_source(fx_table: dict[str, str], waw_map: str | None = None,
                  weapons: dict[str, str] | None = None) -> str:
    lines = ["// waw2bo2: WaW asset name -> BO2 asset name for converted assets (generated).", "init()", "{"]
    if waw_map:
        # WaW's level.script is the WaW map name (see gscport.WAW_LEVEL_FIELDS)
        lines.append(f'    level.waw_script = "{waw_map}";')
    for waw, bo2 in sorted(fx_table.items()):
        lines.append(f'    level.waw2bo2_fx["{waw}"] = "{bo2}";')
    reverse_names = set()
    for waw, bo2 in sorted((weapons or {}).items()):
        lines.append(f'    level.waw2bo2_weapons["{waw}"] = "{bo2}";')
        if bo2 and bo2 != waw and bo2 not in reverse_names:
            lines.append(f'    level.waw2bo2_weapon_names["{bo2}"] = "{waw}";')
            reverse_names.add(bo2)
        runtime = BO2_TACTICAL_RUNTIME_WEAPONS.get(waw)
        if bo2 and runtime and runtime != bo2:
            lines.append(f'    level.waw2bo2_runtime_weapons["{waw}"] = "{runtime}";')
            lines.append(f'    level.waw2bo2_weapon_names["{runtime}"] = "{waw}";')
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
# WaW framework inits whose level tables map scripts read (the anim tables of
# _zombiemode::init_anims); only the fields the ported scripts read are kept
FRAMEWORK_STATE_INITS = (("maps\\_zombiemode", "init_standard_zombie_anims"),)
DAMAGE_OVERRIDE = "player_damage_override"


def top_level_statements(tokens: list[gsc.Token], open_: int, close: int) -> list[tuple[int, int]]:
    """[start, end) token spans of the statements directly inside the block
    whose braces are at ``open_`` / ``close`` (an if/else chain is one)."""
    spans, start, depth = [], open_ + 1, 0
    for i in range(open_ + 1, close):
        t = tokens[i].text
        depth += t in ("(", "[", "{")
        depth -= t in (")", "]", "}")
        if depth or t not in (";", "}") or tokens[i + 1].low == "else":
            continue
        spans.append((start, i + 1))
        start = i + 1
    return spans
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
    lines += ["}", "", "// The WaW mystery box, after BO2 _zm_magicbox::init, with independent chest flags.",
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


# The server and the client must register script-mover animtrees in the same
# order ("script mover animtrees registered in different order server <a>
# client <b>"). Both register BO2's zm_ally inside _zm::init (_zm_clone), so
# the WaW trees follow _zm::init on both sides.
ANIMTREE_HOOKS = (("maps", "    maps\\mp\\zombies\\_zm::init();", "gsc"),
                  ("clientscripts", "    clientscripts\\mp\\zombies\\_zm::init();", "csc"))


def animtrees_source(trees: list[str]) -> str:
    """BO2 refuses an animtree on a script_model until the level registers it
    with ScriptModelsUseAnimTree ("Unrecognized animtree '%s'. You may need to
    call ScriptModelsUseAnimTree()"); WaW has no such step. Stock maps register
    theirs from their main (zm_buried_jail, zm_alcatraz_traps); the clients
    register the same trees from their client scripts (_zm_clone.csc)."""
    lines = ["// waw2bo2: register the WaW animtrees the ported scripts play on script models (generated).", ""]
    for i, tree in enumerate(trees):
        lines += [f'#using_animtree( "{tree}" );', "", f"use_tree_{i}()", "{",
                  "    scriptmodelsuseanimtree( #animtree );", "}", ""]
    lines += ["init()", "{"] + [f"    use_tree_{i}();" for i in range(len(trees))] + ["}", ""]
    return "\n".join(lines)


def hook_bo2_animtrees(main_gsc: Path, main_csc: Path, out_root: Path, trees: list[str]) -> bool:
    """Write the animtree registration (server and client script) and call it
    right after _zm::init in the BO2 map's server and client main (idempotent)."""
    if not trees:
        return False
    changed = False
    for (side, anchor, ext), main in zip(ANIMTREE_HOOKS, (main_gsc, main_csc)):
        base = "maps" if side == "maps" else "clientscripts"
        path = out_root / base / "mp" / "waw" / f"_waw2bo2_animtrees.{ext}"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(animtrees_source(trees), encoding="utf-8")
        source = main.read_text(encoding="utf-8", errors="replace")
        if anchor + '\n' not in source:
            # Perk ownership redirects _zm::init to the converter's owned
            # bootstrap. It still performs the same base animtree registration.
            anchor = anchor.replace(base + '\\mp\\zombies\\_zm',
                                    base + '\\mp\\waw\\_waw2bo2_zm')
        call = f"    {base}\\mp\\waw\\_waw2bo2_animtrees::init();\n"
        if anchor + "\n" + call in source:
            continue
        if anchor + "\n" not in source:
            raise ValueError(f"{main}: no '{anchor.strip()}' line to register the WaW animtrees after")
        # a call placed elsewhere by an earlier run moves after the anchor
        source = source.replace(call, "")
        main.write_text(source.replace(anchor + "\n", anchor + "\n" + call, 1), encoding="utf-8")
        changed = True
    return changed


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


def precache_source(models: list[str]) -> str:
    """First-frame precache of the models the map scripts precache by name.
    T6 errors on precacheModel after the level script's first wait ("must be
    called before any wait statements"); WaW scripts call it late (e.g. when
    the power turns a perk machine on). compat waw_precachemodel skips the
    late repeat. ``models``: the ones the zone carries (staged or stock)."""
    lines = ["// waw2bo2: models the map scripts precache, precached in the first frame (generated).", "",
             "init()", "{"]
    lines += [f'    {COMPAT}::waw_precachemodel( "{m}" );' for m in sorted(set(models))]
    return "\n".join(lines + ["}", ""])


def guard_missing_model_calls(source: str, missing: set[str]) -> tuple[str, list[str]]:
    """Keep subsequent source behavior running when a model is unavailable.

    T6 rejects a missing model at the model API call. Report that unsupported
    operation, retaining the entity's existing model and all following script
    statements (including the map's original power-on light effects).
    """
    script = gsc.parse(source, '<model dependencies>')
    guarded = []
    for ref in gsc.references(script.tokens):
        if ref.pointer or ref.name.lower() not in ('setmodel', 'precachemodel', 'waw_precachemodel'):
            continue
        if ref.qualifier is None and ref.name.lower() in script.functions:
            continue
        if ref.qualifier and ref.qualifier.lower() != COMPAT.lower():
            continue
        if script.tokens[ref.index - 1].text == '::' and ref.qualifier is None:
            continue
        args = script.tokens[ref.index + 2:ref.index + 4]
        if len(args) != 2 or args[0].kind != gsc.STRING or args[1].text != ')':
            continue
        name = args[0].text[1:-1]
        if name not in missing:
            continue
        token = script.tokens[ref.index]
        token.text = 'waw_missing_model'
        if ref.qual_index is None:
            token.text = COMPAT + '::' + token.text
        guarded.append(name)
    return gsc.emit(script.tokens), guarded


def bo2_scripts(out_root: Path) -> list[str]:
    """Asset names of the ported scripts (for the zone and the script compile)."""
    base = out_root / "maps" / "mp" / "waw"
    found = [p.relative_to(out_root).as_posix() for p in base.rglob("*.gsc")] if base.exists() else []
    for name in BO2_PERK_OVERRIDES:
        path = out_root / name
        if path.exists() and path.read_text(encoding="utf-8").startswith(PERK_OVERRIDE_HEADER):
            found.append(name)
    return sorted(found)


def ensure_owned_client_bootstrap(out_root: Path, bo2_root: Path) -> Path:
    """Recreate the generated client bootstrap if a prior build was partial."""
    destination = out_root / "clientscripts/mp/waw/_waw2bo2_zm.csc"
    if destination.is_file():
        return destination
    source = bo2_root / "raw/clientscripts/mp/zombies/_zm.csc"
    if not source.is_file():
        raise ValueError(f"BO2 client bootstrap missing: {source}")
    bootstrap = source.read_text(encoding="utf-8")
    native = "clientscripts\\mp\\zombies\\_zm_perks"
    owned = "clientscripts\\mp\\waw\\_waw2bo2_perks"
    if native not in bootstrap:
        raise ValueError("BO2 client bootstrap has no perk initializer")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(PERK_OVERRIDE_HEADER + bootstrap.replace(native, owned), encoding="utf-8")
    return destination


PERK_OVERRIDE_HEADER = "// waw2bo2: WaW owns perk gameplay; BO2 runtime support only.\n"
BO2_PERK_OVERRIDES = (
    "maps/mp/zombies/_zm_perks.gsc",
    "clientscripts/mp/zombies/_zm_perks.csc",
)


def stage_bo2_perk_support(out_root: Path, bo2_root: Path, *, bo2_stock_perks: bool = False) -> None:
    """Suppress BO2 machine/purchase controllers and HUD in converted WaW maps.

    Preserve the stock exports for BO2 framework callers and register its
    client fields in the same order as the stock client. Gameplay entry points
    in WaW scripts resolve to the ported WaW perk framework instead. Client
    registration remains intact, but its LUI code callbacks are never bound:
    native setperk must apply effects without drawing a second perk HUD.
    """
    if not bo2_stock_perks:
        for prefix, ext in (('maps', 'gsc'), ('clientscripts', 'csc')):
            for module in stockperks.MODULES:
                (out_root / f'{prefix}/mp/waw/_waw2bo2_perk_{module}.{ext}').unlink(missing_ok=True)
    for name in BO2_PERK_OVERRIDES:
        source = (bo2_root / "raw" / name).read_text(encoding="utf-8")
        script = gsc.parse(source, name)
        bodies = {
            "init": '''{
    level.additionalprimaryweapon_limit = 3;
    level.perk_purchase_limit = 4;
    level.machine_assets = [];
    initialize_custom_perk_arrays();
    if ( !level.createfx_enabled )
        perks_register_clientfield();
    if ( !isdefined( level.flag["pack_machine_in_use"] ) )
        flag_init( "pack_machine_in_use" );
    // WaW initializes its machines from waw_main_post, before player startup.
}''',
            "perk_pause_all_perks": "{\n    // The WaW framework owns machine availability.\n}",
            "perk_unpause_all_perks": "{\n    // The WaW framework owns machine availability.\n}",
        }
        if name.endswith(".csc"):
            bodies = {
                "init": '''{
    if ( !level.createfx_enabled )
    {
        init_custom_perks();
        perks_register_clientfield();
    }
}''',
                "perk_init_code_callbacks": '''{
    // setupclientfieldcodecallbacks binds native LUI perk-icon events.
    // WaW creates and removes its own HUD; retain fields without these bindings.
}''',
                "init_perk_custom_threads": "{\n    // Source WaW scripts own perk behavior.\n}",
            }
        for name_fn, body in bodies.items():
            fn = script.functions.get(name_fn)
            if fn is None:
                raise ValueError(f"{name}: expected BO2 perk entry point {name_fn} is missing")
            script.tokens[fn.body_open].text = body
            for t in script.tokens[fn.body_open + 1:fn.body_close + 1]:
                t.pre = ""
                t.text = ""
        dest = out_root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        text = gsc.emit(script.tokens)
        if bo2_stock_perks:
            text = stockperks.native_source(source, name, out_root, bo2_root)
        dest.write_text(PERK_OVERRIDE_HEADER + text, encoding="utf-8")

    # Stock CSCs can already be linked by the base zone before a same-name
    # map asset arrives. Use unique script names and an explicit map entry
    # point so the client never enters the cached stock perk initializer.
    client = out_root / "clientscripts/mp/waw/_waw2bo2_perks.csc"
    client.parent.mkdir(parents=True, exist_ok=True)
    client.write_text((out_root / "clientscripts/mp/zombies/_zm_perks.csc").read_text(encoding="utf-8"),
                      encoding="utf-8")
    ensure_owned_client_bootstrap(out_root, bo2_root)

    # The server has the same stock-script caching risk as the client. Route
    # the framework initializer through owned names, rather than assuming a
    # map-zone same-name override replaces an already linked stock script.
    server_dir = out_root / "maps/mp/waw"
    server_dir.mkdir(parents=True, exist_ok=True)
    native = "maps\\mp\\zombies\\_zm_perks"
    owned = "maps\\mp\\waw\\_waw2bo2_perks"
    source = (out_root / "maps/mp/zombies/_zm_perks.gsc").read_text(encoding="utf-8")
    (server_dir / "_waw2bo2_perks.gsc").write_text(source.replace(native, owned), encoding="utf-8")
    bootstrap = (bo2_root / "raw/maps/mp/zombies/_zm.gsc").read_text(encoding="utf-8")
    if native + "::init" not in bootstrap:
        raise ValueError("BO2 server bootstrap has no perk initializer")
    (server_dir / "_waw2bo2_zm.gsc").write_text(
        PERK_OVERRIDE_HEADER + bootstrap.replace(native, owned), encoding="utf-8")


def hook_bo2_perk_server(main_gsc: Path) -> None:
    """Use owned server entry points for perk initialization and availability."""
    source = main_gsc.read_text(encoding="utf-8")
    script = gsc.parse(source, main_gsc.as_posix())
    native = "maps\\mp\\zombies\\_zm"
    owned = "maps\\mp\\waw\\_waw2bo2_zm"
    count = 0
    for ref in gsc.references(script.tokens):
        if ref.name.lower() == "init" and ref.qualifier in (native, owned):
            script.tokens[ref.qual_index].text = owned
            count += 1
        elif ref.qualifier == "maps\\mp\\zombies\\_zm_perks":
            script.tokens[ref.qual_index].text = "maps\\mp\\waw\\_waw2bo2_perks"
    if not count:
        raise ValueError(f"{main_gsc}: no BO2 server bootstrap call")
    main_gsc.write_text(gsc.emit(script.tokens), encoding="utf-8")


def hook_bo2_perk_client(main_csc: Path) -> None:
    """Enter the uniquely named client bootstrap for every converted map."""
    source = main_csc.read_text(encoding="utf-8")
    script = gsc.parse(source, main_csc.as_posix())
    native = "clientscripts\\mp\\zombies\\_zm"
    owned = "clientscripts\\mp\\waw\\_waw2bo2_zm"
    count = 0
    for ref in gsc.references(script.tokens):
        if ref.name.lower() == "init" and ref.qualifier in (native, owned):
            script.tokens[ref.qual_index].text = owned
            count += 1
    if not count:
        raise ValueError(f"{main_csc}: no BO2 client bootstrap call")
    main_csc.write_text(gsc.emit(script.tokens), encoding="utf-8")
