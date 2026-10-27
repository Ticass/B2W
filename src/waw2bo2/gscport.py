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

from . import gsc, oneway, stockperks, waterbullets, zombieappearance, zombiebehavior
from .t6api import T6Api, load_params_snapshot

COMPAT_DIR = Path(__file__).parent / "compat"
API = json.loads((COMPAT_DIR / "gsc_api.json").read_text(encoding="utf-8"))
# BO2 parameter names of the scripts core_scripts maps to (signature_differs)
BO2_PARAMS = load_params_snapshot()
COMPAT = "maps\\mp\\waw\\_waw2bo2_compat"
STUBS = "maps\\mp\\waw\\_waw2bo2_stubs"
ASSETS = "maps\\mp\\waw\\_waw2bo2_assets"
CORE = "maps\\mp\\waw\\_waw2bo2_core"
PRECACHE = "maps\\mp\\waw\\_waw2bo2_precache"
ONEWAY = "maps\\mp\\waw\\_waw2bo2_oneway"
WATER_BULLETS = "maps\\mp\\waw\\_waw2bo2_water_bullets"
ZOMBIEMODE = "maps\\_zombiemode"
PERKS = "maps\\_zombiemode_perks"
GAMESKILL = "maps\\_gameskill"
WEAPONS_FRAMEWORK = "maps\\_zombiemode_weapons"
SCORE_FRAMEWORK = "maps\\_zombiemode_score"
# the owned copy of BO2's _zm (stage_bo2_perk_support)
OWNED_ZM = "maps\\mp\\waw\\_waw2bo2_zm"
CORE_INCLUDES = ("common_scripts\\utility", "maps\\mp\\_utility", "maps\\mp\\zombies\\_zm_utility")
# BO2 scripts a unique-name lookup may resolve into (never map/other-mode scripts)
BO2_SHARED = re.compile(r"^(common_scripts\\utility|maps\\mp\\_utility|maps\\mp\\zombies\\_zm[a-z_]*|"
                        r"maps\\mp\\gametypes_zm\\_[a-z_]+|maps\\mp\\_[a-z_]+)$")


def script_branch_taken(tokens: list, body_open: int, body_close: int, index: int, map_name: str | None) -> bool:
    """False when tokens[index] lies in the branch of a top-level
    if( level.script ==/!= "<map>" ) {...} else {...} that the map never takes
    (level.script is the immutable WaW map name). Anything else is taken."""
    if not map_name:
        return True
    for a, b in top_level_statements(tokens, body_open, body_close):
        if not (a <= index < b) or tokens[a].low != "if":
            continue
        guard_end = gsc._match(tokens, a + 1, "(", ")")
        words = [t.low for t in tokens[a + 2:guard_end]]
        if len(words) != 5 or words[:3] != ["level", ".", "script"] or words[3] not in ("==", "!=")                 or not words[4].startswith('"') or tokens[guard_end + 1].text != "{":
            return True
        matched = (map_name.lower() == words[4].strip('"')) == (words[3] == "==")
        yes_end = gsc._match(tokens, guard_end + 1, "{", "}")
        return matched if index <= yes_end else not matched
    return True


def norm(path: str) -> str:
    return path.replace("/", "\\").lower().removesuffix(".gsc").removesuffix(".csc")


def source_drives(core: str, name: str) -> bool:
    """The map's own function runs for a call into this WaW framework script
    (gsc_api source_driven): every function but the script's glue, or only the
    listed ones where the script names them."""
    driven = API.get("source_driven", {}).get(core)
    if driven is None or name in driven.get("glue", {}):
        return False
    return "only" not in driven or name in driven["only"]


def ported_path(path: str) -> str:
    p = norm(path)
    if p.startswith("clientscripts\\"):
        return "clientscripts\\mp\\waw\\" + p.removeprefix("clientscripts\\")
    return "maps\\mp\\waw\\" + p.removeprefix("maps\\")


def animation_tree_name(tree: str) -> str:
    return "waw2bo2_" + norm(tree).replace("\\", "_")


def animation_asset_name(name: str) -> str:
    return "waw_anim_" + name.lower()


def animation_names(tokens: list[gsc.Token]) -> set[str]:
    return {tokens[i + 1].low for i in range(1, len(tokens) - 1)
            if tokens[i].text == "%" and tokens[i + 1].kind == gsc.IDENT
            and tokens[i - 1].text in ("(", ",", "=", "[", "return", "[[")}


# A source feature the port cannot carry, where BO2's behaviour runs instead:
# reported in scripts.unsupported, never a build error (a fallback must not stop
# the map from converting; Verruckt failed to build on one, 2026-10-25).
FALLBACK = "GSC fallback (reported; BO2's behaviour runs)"


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
    # HUD materials a stock policy draws in their WaW form although BO2 ships
    # a material of the same name (converted and isolated, never BO2's)
    waw_hud_materials: list[str] = field(default_factory=list)
    # native T6 animtrees of non-human source actors (dog) -> WaW AI type
    native_actor_trees: dict[str, str] = field(default_factory=dict)
    # string literals of the client listeners run on the server: client-only
    # entities they name must exist on the server (t6bridge)
    client_listener_strings: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {"map_main": self.map_main, "ported": self.ported, "core_overrides": self.core_overrides,
                "characters": self.characters, "weapon_registration": self.weapon_registration,
                "extracted": self.extracted,
                "unsupported": self.unsupported, "rewrites": dict(self.rewrites), "fx": self.fx,
                "animtrees": self.animtrees, "errors": self.errors,
                "waw_hud_materials": self.waw_hud_materials,
                "native_actor_trees": self.native_actor_trees,
                "client_listener_strings": self.client_listener_strings}


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
        # The merged stock view selects one definition per path. Framework
        # comparisons must also see later stock-zone versions (e.g. Der Riese
        # damage handling), rather than treating them as authored map preludes.
        if stock_root is not None and (stock_root / 'catalog.json').is_file():
            catalog = json.loads((stock_root / 'catalog.json').read_text(encoding='utf-8'))
            for entry in catalog.get('zones', {}).values():
                folder = Path(entry['folder'])
                if folder.is_dir() and folder not in self.variant_roots:
                    self.variant_roots.append(folder)
        self.origin: dict[str, str] = {}
        for iwd in iwds:
            with zipfile.ZipFile(iwd) as z:
                for name in sorted(z.namelist()):
                    if name.lower().endswith((".gsc", ".csc")):
                        key = norm(name) + Path(name).suffix.lower()
                        if key not in self.text:
                            # Universal newlines, as read_text gives zone rawfiles:
                            # CRLF kept here is written back as CR CR LF on Windows,
                            # doubling every line the T6 compiler reports.
                            text = z.read(name).decode("utf-8", errors="replace")
                            self.text[key] = text.replace("\r\n", "\n").replace("\r", "\n")
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

    def bind_zombie_framework(self, main_path: str) -> dict[str, str]:
        """Recognize renamed WaW zombie startup through its framework contract."""
        script = self.get(main_path)
        main = script.functions.get('main') if script else None
        if main is None:
            return {}
        candidates = set()
        for ref in gsc.references(script.tokens, main.body_open, main.body_close):
            if ref.qualifier is None or ref.pointer or ref.name.lower() != 'main':
                continue
            target = norm(ref.qualifier)
            if target == ZOMBIEMODE:
                candidates.add(target)
                continue
            dependency = self.get(target)
            markers = {'init_levelvars', 'round_start', 'round_think',
                       'onplayerconnect', 'player_damage_override'}
            if dependency is None or not markers <= dependency.functions.keys():
                continue
            entry = dependency.functions.get('main')
            if entry is None or entry.params:
                continue
            refs = gsc.references(dependency.tokens, entry.body_open, entry.body_close)
            loads_map = any(r.qualifier and norm(r.qualifier) == r'maps\_load' and
                            r.name.lower() == 'main' and not r.pointer for r in refs)
            waits_players = any(r.name.lower() == 'flag_wait' and not r.pointer and
                dependency.tokens[r.index + 2].kind == gsc.STRING and
                dependency.tokens[r.index + 2].text.strip('"').lower() == 'all_players_connected'
                for r in refs)
            if loads_map and waits_players:
                candidates.add(target)
        if len(candidates) > 1:
            raise ValueError(f'{main_path}: multiple zombie startup frameworks: {sorted(candidates)}')
        if not candidates or candidates == {ZOMBIEMODE}:
            return {}
        alias = candidates.pop()
        key = alias + '.gsc'
        source = self.text.get(key, self.stock.get(key))
        self.text = dict(self.text)
        self.origin = dict(self.origin)
        self.text[ZOMBIEMODE + '.gsc'] = source
        self.origin[ZOMBIEMODE + '.gsc'] = self.origin.get(key, alias)
        for path, text in list(self.text.items()):
            tokens = gsc.tokenize(text)
            changed = False
            for ref in gsc.references(tokens):
                if ref.qualifier and norm(ref.qualifier) == alias:
                    tokens[ref.qual_index].text = ZOMBIEMODE
                    changed = True
            for token in tokens:
                match = gsc.INCLUDE_RE.match(token.text) if token.kind == gsc.DIRECTIVE else None
                if match and norm(match[1]) == alias:
                    token.text = f'#include {ZOMBIEMODE};'
                    changed = True
            if changed:
                self.text[path] = gsc.emit(tokens)
        self._parsed = {}
        return {alias: ZOMBIEMODE}

    def is_stock(self, path: str, ext: str = ".gsc") -> bool:
        key = norm(path) + ext
        if key not in self.stock:
            for root in self.variant_roots:
                raw = root / (norm(path).replace('\\', '/') + ext)
                if raw.is_file():
                    self.stock[key] = raw.read_text(encoding='utf-8', errors='replace')
                    break
        return key in self.stock


def is_core(path: str, sources: Sources) -> bool:
    p = norm(path)
    if p in API.get("preserved_scripts", []):
        return False
    if p in API["core_scripts"] or p in API["template_scripts"]:
        return True
    # Check raw stock variants even when the map supplies an override. Porting
    # the entire animscripts file would pull WaW's AI framework into T6.
    return any(re.search(r, p) for r in API["core_patterns"]) and sources.is_stock(p)


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
                and re.search(r"self(?:\S*::waw_)?startragdoll\(\)", code)
                and re.search(r"self(?:\S*::waw_)?launchragdoll\(", code)
                and re.search(r"self(?:\S*::waw_)?dodamage\(self\.health\+\d", code)
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
        # WaW accepts an indented ``#include``; T6 expects an identifier
        # after leading whitespace (Project X's forum-pasted IWD scripts).
        if t.kind == gsc.DIRECTIVE:
            fixed = t.pre.rstrip(" \t")
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
WAW_LEVEL_FIELDS["zombie_move_speed"] = API['zombie_runtime']['source_speed']['owned_result_field']
# The map's own zone manager (preserved _zombiemode_zone_manager) keeps the
# zones, its init pointer and the dog spawn lists the source spawn and dog
# code read; BO2's _zm_zonemgr keeps its own for the BO2 triggers still in use.
# The map's own weapon registry and box (preserved _zombiemode_weapons): BO2's
# _zm_weapons/_zm_magicbox keep theirs for the BO2 code still running.
WAW_LEVEL_FIELDS.update({name: "waw_" + name for name in (
    "zombie_weapons", "zombie_include_weapons", "limited_weapons", "weapon_weighting_funcs",
    "zombie_treasure_chest_cost", "chest_accessed", "chest_min_move_usage", "open_chest_location",
    "verify_chest", "random_pandora_box_start", "pulls_since_last_ray_gun", "pulls_since_last_tesla_gun",
    "player_drops_tesla_gun", "player_seen_tesla_gun")})
WAW_LEVEL_FIELDS.update({"zones": "waw_zones", "zone_manager_init_func": "waw_zone_manager_init_func",
                         "enemy_dog_locations": "waw_enemy_dog_locations",
                         "enemy_dog_spawns": "waw_enemy_dog_spawns"})
WAW_BOX_FLAGS = {'moving_chest_enabled', 'moving_chest_now', 'chest_has_been_used',
                 # the map's wall-buy triggers (entities.WAW_WALL_BUY_TARGETNAME)
                 'weapon_upgrade'}


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
        elif t.kind == gsc.IDENT and t.low == "zombie_move_speed" and tokens[i - 1].text == ".":
            # Source actor speed is separate from the field native AI can write.
            t.text = API['zombie_runtime']['source_speed']['owned_result_field']
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


def bridge_drinking_flag(tokens: list[gsc.Token]) -> int:
    """WaW and BO2 share the player field is_drinking with different meanings.

    WaW: defined (1) while a bottle/flourish plays, undefined otherwise; its
    scripts test isdefined(). BO2: a counter, 0 when idle, set at spawn and
    tested with > 0. Under BO2 a WaW isdefined() test is always true (Der
    Riese's bowie could never be bought), and a WaW reset to undefined breaks
    BO2's comparisons. WaW tests read the count; WaW resets write 0."""
    count = 0
    for i in range(2, len(tokens) - 2):
        if tokens[i].low != "is_drinking" or tokens[i - 1].text != ".":
            continue
        if tokens[i + 1].text == "=" and tokens[i + 2].low == "undefined":
            tokens[i + 2].text = "0"
            count += 1
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
        if not (j >= 2 and tokens[j - 1].text == "(" and tokens[j - 2].low == "isdefined"
                and tokens[i + 1].text == ")"):
            continue
        receiver = gsc.emit(tokens[j:i - 1]).lstrip()
        tokens[j - 2].text = f"{COMPAT}::waw_is_drinking( {receiver} )"
        tokens[j - 2].kind = gsc.PUNCT
        for t in tokens[j - 1:i + 2]:
            t.text = t.pre = ""
        count += 1
    return count


def bridge_score_writes(tokens: list[gsc.Token]) -> int:
    """T6 keeps a player's score in pers["score"] too: the scoreboard reads it
    and a (re)connect restores score from it (BO2's _zm_score writes both).
    After each WaW statement assigning <player>.score, the persistent copy
    follows (an idempotent copy, so a braceless if needs no block)."""
    count = 0
    for i in range(2, len(tokens) - 2):
        if tokens[i].low != "score" or tokens[i - 1].text != "." or \
                tokens[i + 1].text not in ("=", "+=", "-=", "*=", "/=", "++", "--"):
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
        receiver = gsc.emit(tokens[j:i - 1]).strip()
        if not receiver or "(" in receiver:
            continue
        end = i + 1
        depth = 0
        while end < len(tokens) and not (tokens[end].text == ";" and depth == 0):
            depth += tokens[end].text in ("(", "[")
            depth -= tokens[end].text in (")", "]")
            end += 1
        if end >= len(tokens):
            continue
        tokens[end].text = f'; {receiver}.pers["score"] = {receiver}.score;'
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


HUD_MATERIAL_TABLE = r"maps\mp\waw\_waw2bo2_hud_materials"


def bridge_dynamic_hud_materials(tokens: list[gsc.Token]) -> int:
    """SetShader with a material name built at run time ("hud_chalk_" + n):
    converted HUD materials can be renamed out of BO2's identities
    (materialisolation), which rewrites literal names only. The built name
    goes through the generated table instead; unknown names stay unchanged."""
    rewrites = 0
    for i in range(1, len(tokens) - 2):
        if tokens[i].low != "setshader" or tokens[i + 1].text != "(" or tokens[i - 1].text == "::":
            continue
        close = gsc._match(tokens, i + 1, "(", ")")
        if close == i + 2:
            continue
        end, depth = close, 0
        for j in range(i + 2, close):
            depth += tokens[j].text in ("(", "[")
            depth -= tokens[j].text in (")", "]")
            if tokens[j].text == "," and depth == 0:
                end = j
                break
        if end == i + 3 and tokens[i + 2].kind == gsc.STRING:
            continue
        tokens[i + 2].text = f"{COMPAT}::waw_hud_material( {tokens[i + 2].text}"
        tokens[end - 1].text += " )"
        rewrites += 1
    return rewrites


def mirror_team_zombie_vars(tokens: list[gsc.Token]) -> int:
    """WaW keeps every zombie var in level.zombie_vars[name]; BO2 keeps the
    power-up ones per team (gsc_api team_zombie_vars) and its scoring and
    damage read level.zombie_vars[team][name]. After each source statement
    that sets one (an assignment or set_zombie_var), the compat runtime copies
    the value to every team, so WaW's double points and insta-kill reach BO2's
    readers."""
    names = set(API["team_zombie_vars"]["names"])
    rewrites = 0
    for i in range(len(tokens) - 6):
        t = tokens[i]
        name = None
        if t.low == "level" and [x.text for x in tokens[i + 1:i + 4]] == [".", "zombie_vars", "["] \
                and tokens[i + 4].kind == gsc.STRING and tokens[i + 5].text == "]" \
                and tokens[i + 6].text in ("=", "+=", "-=", "*=", "/=", "++", "--") \
                and (i == 0 or tokens[i - 1].text != "."):
            name = tokens[i + 4].text.strip('"')
        elif t.low.endswith("set_zombie_var") and tokens[i + 1].text == "(" and tokens[i + 2].kind == gsc.STRING:
            name = tokens[i + 2].text.strip('"')
        if name not in names:
            continue
        depth = 0
        for j in range(i, len(tokens)):
            depth += tokens[j].text in ("(", "[")
            depth -= tokens[j].text in (")", "]")
            if depth < 0 or tokens[j].text in ("{", "}"):
                break   # inside a for header or an expression: not a statement
            if tokens[j].text == ";" and depth == 0:
                tokens[j].text += f' {COMPAT}::waw_team_zombie_var( "{name}" );'
                rewrites += 1
                break
    return rewrites


def hud_material_table_source(renamed: dict[str, str]) -> str:
    lines = ["// waw2bo2: converted HUD materials renamed out of BO2's names (materialisolation).",
             "table()", "{", "\tmaterials = [];"]
    # keys are split so the literal-name rewrite never touches them
    for old, new in sorted(renamed.items()):
        lines.append(f'\tmaterials["{old[:1]}" + "{old[1:]}"] = "{new}";')
    return "\n".join(lines + ["\treturn materials;", "}", ""])


def bridge_stock_perk_costs(tokens: list[gsc.Token], policies: dict) -> int:
    """Stock WaW maps only: a perk machine's cost assignment under its case
    label (``case "specialty_quickrevive": cost = 1500;``) asks the stock map
    policy, which keeps the source cost unless a policy (BO-style solo Quick
    Revive) prices that perk differently for the current game."""
    perks = {p["perk"] for name, p in policies.items() if not name.startswith("_") and "solo_cost" in p}
    rewrites = 0
    for i in range(len(tokens) - 5):
        if tokens[i].low != "case" or tokens[i + 1].kind != gsc.STRING or tokens[i + 1].text.strip('"') not in perks:
            continue
        # The same perk cases also select bottle weapons and hint strings.
        # Only the machine's cost local is subject to pricing policy.
        if (tokens[i + 2].text != ":" or tokens[i + 3].kind != gsc.IDENT
                or tokens[i + 3].low != "cost" or tokens[i + 4].text != "="):
            continue
        end = next((j for j in range(i + 5, len(tokens)) if tokens[j].text == ";"), None)
        if end is None or end == i + 5:
            continue
        tokens[i + 5].text = f"{COMPAT}::waw_stock_perk_cost( {tokens[i + 1].text}, {tokens[i + 5].text}"
        tokens[end - 1].text += " )"
        rewrites += 1
    return rewrites


def stock_map_policy(policies: dict, level: str) -> str:
    """Level state for gsc_api.json stock_map_policies (stock WaW maps only).
    ``level``: the source level name (WaW level.script)."""
    revive = policies["solo_quick_revive"]
    lines = ["", "// stock WaW map policies (gsc_api.json stock_map_policies); custom maps keep their own logic",
             "waw2bo2_stock_map_policies()", "{",
             "\tlevel.waw2bo2_solo_revive = [];",
             f'\tlevel.waw2bo2_solo_revive["perk"] = "{revive["perk"]}";',
             f'\tlevel.waw2bo2_solo_revive["cost"] = {int(revive["solo_cost"])};',
             f'\tlevel.waw2bo2_solo_revive["limit"] = {int(revive["solo_purchase_limit"])};',
             f'\tlevel.waw2bo2_solo_revive["hint"] = &"{revive["solo_hint"]}";',
             f'\tprecachestring( &"{revive["solo_hint"]}" );',
             f"\tlevel thread {COMPAT}::waw_stock_solo_revive();"]
    return "\n".join(lines + ["}", ""])


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


def bridge_hud_cleanup(tokens: list[gsc.Token], index: int, wrapper: str = "waw_destroy_hud_elem") -> bool:
    """Call the idempotent native HUD cleanup by argument, not receiver.

    Called only after resolving a cleanup to BO2's HUD utility. A source
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
    tokens[start].text = f"{COMPAT}::{wrapper}( {receiver} )"
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


INTERMISSION_CAMERA = (
    "\n\tif ( !isdefined( self.waw_intermission_cam ) )\n\t{{"
    "\n\t\tself.waw_intermission_cam = spawn( \"script_model\", {mover}.origin );"
    "\n\t\tself.waw_intermission_cam setmodel( \"tag_origin\" );"
    "\n\t\tself.waw_intermission_cam.angles = {mover}.angles;"
    "\n\t\tself.waw_intermission_cam linkto( {mover} );"
    "\n\t\tself camerasetposition( self.waw_intermission_cam );"
    "\n\t\tself camerasetlookat();"
    "\n\t\tself cameraactivate( 1 );"
    "\n\t}}")


def bridge_intermission_camera(tokens: list[gsc.Token]) -> int:
    """WaW intermissions link the player to a mover and steer the view with
    SetPlayerAngles; BO2's own intermission views through the script camera
    (CameraSetPosition on a tag_origin script_model + CameraActivate, _zm
    player_intermission). The camera follows a networked tag_origin model
    linked to the WaW mover (often a server-only script_origin), so the view
    keeps the mover's path and rotation. The WaW link is kept."""
    count = 0
    for i in range(1, len(tokens) - 4):
        if tokens[i].low not in ("linkto", f"{COMPAT}::waw_linkto".lower()) or tokens[i - 1].low != "self" \
                or tokens[i + 1].text != "(":
            continue
        close = gsc._match(tokens, i + 1, "(", ")")
        mover = tokens[i + 2]
        if mover.kind != gsc.IDENT or tokens[i + 3].text not in (",", ")") or tokens[close + 1].text != ";":
            continue
        tokens[close + 1].text += INTERMISSION_CAMERA.format(mover=mover.text)
        count += 1
    return count


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


def lower_maxhealth_assignments(tokens: list[gsc.Token]) -> int:
    """`<ent>.maxhealth = <expr>;` -> `<ent> waw_set_maxhealth( <expr> );`.

    WaW scripts raise a player's maximum health by assigning the field (Jugg:
    `player.maxhealth = 160`). T6 keeps the player's cap in its player state,
    which only setmaxhealth changes (BO2's own scripts never assign the field),
    so health stayed clamped to 100 and regenerated only that far. The
    helper uses setmaxhealth for players and keeps the field for AI.
    """
    changed = 0
    for i in range(2, len(tokens) - 2):
        if tokens[i].low != 'maxhealth' or tokens[i - 1].text != '.' or tokens[i + 1].text != '=':
            continue
        start = i - 2  # receiver: identifiers joined by '.' and [index] groups
        while True:
            if tokens[start].text == ']':
                depth = 0
                while True:
                    depth += {']': 1, '[': -1}.get(tokens[start].text, 0)
                    if depth == 0:
                        break
                    start -= 1
                start -= 1
            if tokens[start].kind not in (gsc.IDENT, gsc.PATH) and tokens[start].low not in ('self', 'level'):
                break
            if tokens[start - 1].text != '.':
                break
            start -= 2
        # Statement start (an earlier rewrite may have widened a ';').
        if tokens[start].kind != gsc.IDENT or tokens[start - 1].text.strip()[-1:] not in (';', '{', '}', ')') \
                and tokens[start - 1].low != 'else':
            continue
        end, depth = i + 2, 0
        while depth or tokens[end].text != ';':
            if tokens[end].kind == gsc.EOF:
                break
            depth += tokens[end].text in ('(', '[', '{')
            depth -= tokens[end].text in (')', ']', '}')
            end += 1
        if tokens[end].text != ';':
            continue
        tokens[i - 1].text = ''
        tokens[i].text = ' ' + COMPAT + '::waw_set_maxhealth('
        # no longer a name: an IDENT here made the reference scanner read the
        # first call of the expression as a method (`x int( ... )`) and stub it
        tokens[i].kind = gsc.PUNCT
        tokens[i].pre = ''
        tokens[i + 1].text = tokens[i + 1].pre = ''
        tokens[i + 2].pre = ' '
        tokens[end].text = ' );'
        changed += 1
    return changed


# T6's plain "left"/"right"/"top"/"bottom" anchor to the 4:3 region; BO2's
# own corner HUD uses the safe-area "user_*" anchors, where WaW drew its
# HUD (the round chalk sat inward on 16:9 screens). Centre stays centre.
HUD_ALIGNMENTS = {"horzalign": {"left": "user_left", "right": "user_right"},
                  "vertalign": {"top": "user_top", "bottom": "user_bottom"}}


def lower_hud_fields(tokens: list[gsc.Token]) -> int:
    """WaW HUD element field assignments, translated for T6.

    Edge alignments use the safe-area anchors (HUD_ALIGNMENTS). WaW's opaque
    pause menu covered script HUD whatever hidewheninmenu said; T6's pause
    menu draws under it, so WaW HUD hides in menus (the round chalk showed
    on pause)."""
    changed = 0
    for i in range(1, len(tokens) - 3):
        if tokens[i - 1].text != "." or tokens[i + 1].text != "=" or tokens[i + 2].text == "=":
            continue
        field, value = tokens[i].low, tokens[i + 2]
        if field in HUD_ALIGNMENTS and value.kind == gsc.STRING and tokens[i + 3].text == ";":
            target = HUD_ALIGNMENTS[field].get(value.text[1:-1].lower())
            if target:
                value.text = f'"{target}"'
                changed += 1
        elif field == "hidewheninmenu" and tokens[i + 3].text == ";" and value.low in ("false", "0"):
            value.text = "true"
            changed += 1
    return changed


def lower_zombie_weapon_keys(tokens: list[gsc.Token]) -> int:
    """`GetArrayKeys( level.zombie_weapons )` -> the WaW-registered keys only
    (compat waw_zombie_weapon_keys): the WaW box chooser offers every key, and
    BO2's framework pistol and native helpers share that array."""
    pattern = ['getarraykeys', '(', 'level', '.', 'zombie_weapons', ')']
    changed = 0
    for i in range(len(tokens) - len(pattern)):
        if [t.low for t in tokens[i:i + len(pattern)]] != pattern or tokens[i - 1].text in ('.', '::'):
            continue
        tokens[i].text = COMPAT + '::waw_zombie_weapon_keys'
        # This is a complete target call, not a source builtin for resolve_refs.
        # Leaving it IDENT makes the resolver replace the qualified text with
        # an unsupported stub, whose undefined result stalls the box chooser.
        tokens[i].kind = gsc.PUNCT
        for t in tokens[i + 2:i + 5]:
            t.text = t.pre = ''
        tokens[i + 5].pre = ''
        changed += 1
    return changed


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
        self.animation_discovery = False
        self.animation_requirements: dict[str, set[str]] = defaultdict(set)
        self.bo2_stock_perks = False
        self.stock_map = False
        # stock-perks mode: WaW perk scripts -> functions that stay WaW's
        # (Pack-a-Punch, power relays; waw_pack_a_punch_source)
        self.waw_perk_parts: dict[str, set[str]] = {}
        # calls waw_main_post makes after BO2's _zm::init (framework overrides)
        self.post_calls: list[str] = []
        # calls waw_main_pre makes before BO2's _zm::init (native replacements)
        self.pre_calls: list[str] = []
        self.intermission_funcs: set[tuple[str, str]] = set()
        # (core, function) -> text patch applied after translation (extract_round_loop glue)
        self.post_patches: dict = {}

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

    @staticmethod
    def stub_name(desc: str) -> str:
        return "stub_" + re.sub(r"\W+", "_", desc.replace("::", "__")).strip("_")

    def stub(self, desc: str, name: str, argc: int | None, where: str) -> str:
        stub = self.stub_name(desc)
        old = self.stubs.get(stub, (desc, 0))
        self.stubs[stub] = (desc, max(old[1], argc or 0))
        self.report.unsupported.setdefault(f"GSC {desc}", []).append(where)
        return f"{STUBS}::{stub}"

    SOUND_BUILTINS = frozenset(("playsound", "playloopsound", "playsoundatposition", "playlocalsound",
                                "playsoundasmaster", "playsoundtoplayer", "playsoundwithnotify"))

    def plays_argument_sound(self, core: str, name: str, seen: frozenset = frozenset()) -> bool:
        """A parameter of the WaW framework function decides the alias a sound
        builtin plays: it occurs in the builtin's first argument (the alias,
        or a table lookup such as level.zombie_sounds[ref]), or is passed to a
        function of the same script that plays it (play_sound_2d ->
        really_play_2d_sound)."""
        script = self.sources.get(core)
        fn = script.functions.get(name.lower()) if script else None
        if fn is None or not fn.params or name.lower() in seen:
            return False
        params = [p.lower() for p in fn.params]
        toks = script.tokens

        def arguments(open_):
            args, current, depth = [], [], 0
            for j in range(open_ + 1, len(toks)):
                t = toks[j].text
                if t in ("(", "["):
                    depth += 1
                elif t in (")", "]"):
                    if depth == 0:
                        args.append(current)
                        return args
                    depth -= 1
                elif t == "," and depth == 0:
                    args.append(current)
                    current = []
                    continue
                current.append(toks[j].low)
            return args

        for ref in gsc.references(toks, fn.body_open, fn.body_close):
            i = ref.index
            if i + 1 >= len(toks) or toks[i + 1].text != "(":
                continue
            args = arguments(i + 1)
            callee = ref.name.lower()
            if callee in self.SOUND_BUILTINS:
                if args and any(t in params for t in args[0]):
                    return True
            elif any(arg in ([p] for p in params) for arg in args):
                owner = self.function_script(core, ref)
                if owner is not None and self.plays_argument_sound(owner, callee, seen | {name.lower()}):
                    return True
        return False

    def function_script(self, core: str, ref) -> str | None:
        """The WaW script defining the function a reference in ``core`` calls:
        its qualifier, ``core`` itself, or one of ``core``'s includes."""
        callee = ref.name.lower()
        if ref.qualifier:
            target = ref.qualifier
            script = self.sources.get(target)
            return target if script is not None and callee in script.functions else None
        script = self.sources.get(core)
        if script is None:
            return None
        if callee in script.functions:
            return core
        for include in script.includes:
            other = self.sources.get(include)
            if other is not None and callee in other.functions:
                return include
        return None

    def resolve_core(self, core: str, name: str, argc, where, included: set[str], sibling: bool = False) -> str | None:
        """Returns replacement text for the reference (None = keep as is).
        ``sibling``: called from code extracted from ``core`` itself; such a
        call resolves to BO2's counterpart of ``core`` or to the WaW function,
        never to an unrelated BO2 script that happens to share the name (the
        WaW box's treasure_chest_think is not BO2 _zm_magicbox's)."""
        renamed = API["function_renames"].get(name)
        if renamed:
            return renamed
        # Direct script damage is authored gameplay, including WaW-only modes
        # (e.g. the Betty). Native actor damage dispatch remains on T6.
        if name in API.get('source_script_functions', {}).get(core, []):
            extracted = self.extract(core, name)
            if extracted:
                return extracted
        # Template functions are authored source behavior. In particular,
        # DLC3's spawnFX() bootstraps createFX; it is not the engine spawnfx
        # builtin wrapped by waw_spawnfx(fx, origin, ...).
        if core in API["template_scripts"]:
            extracted = self.extract(core, name)
            if extracted:
                return extracted
        # A compat wrapper replaces every spelling of its call. WaW defines
        # clientNotify in maps\_utility, so the source call reaches it through
        # the include, and BO2's maps\mp\_utility::clientnotify never raises
        # the "waw_client_" notify the server copies of client listeners
        # (Shi No Numa's electric trap arcs) wait for.
        if name in API["compat_wrappers"]:
            compat = self.compat_name(name)
            if compat:
                self.report.rewrites[f"{core}::{name} -> compat wrapper"] += 1
                return compat
        # A WaW helper that plays a sound its caller names (play_sound_2d(
        # "ann_vox_dog_start")) runs from source: its sound builtin goes through
        # the compat wrappers that name WaW's aliases in the bank. BO2's
        # same-named helper received WaW's bare alias and played nothing (Shi
        # No Numa's dog-round "Fetch me their souls").
        if self.plays_argument_sound(core, name):
            extracted = self.extract(core, name)
            if extracted:
                self.report.rewrites[f"{core}::{name} -> source (plays a caller's sound alias)"] += 1
                return extracted
        # WaW drives (docs/CONVERT_THE_WHOLE_MAP.md): a source-driven framework
        # script runs the map's own function, not BO2's of the same name; only
        # its engine glue (gsc_api source_driven glue, each with its reason)
        # stays BO2's.
        if source_drives(core, name):
            extracted = self.extract(core, name)
            if extracted:
                self.report.rewrites[f"{core}::{name} -> source (WaW drives)"] += 1
                return extracted
        # The map ships its own framework script, and its function takes other
        # arguments than BO2's counterpart: older WaW maps call
        # network_safe_spawn( classname, origin ) where BO2's _zm_net (and Der
        # Riese's WaW) takes ( id, max, classname, origin ). BO2's errored every
        # frame on Shi No Numa's fire dogs, whose death explosion never spawned.
        if self.signature_differs(core, name):
            extracted = self.extract(core, name)
            if extracted:
                self.report.rewrites[f"{core}::{name} -> source (signature differs from BO2's)"] += 1
                return extracted
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
            # WaW's VM recovers from builtin parameter errors by cleaning the
            # arguments, returning undefined and continuing at the next opcode.
            # T6 rejects the call at link time, so report it through a returning
            # stub rather than suspending the source thread or inventing defaults.
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
        # T6 actor setup a source spawn function lacks because WaW's engine did
        # it itself (gsc_api zombie_runtime actor_glue: "script::function" ->
        # owned glue), run first in that function
        for key, glue in API['zombie_runtime'].get('actor_glue', {}).items():
            owner, _, name = key.rpartition('::')
            fn = script.functions.get(name.lower()) if norm(owner) == norm(path) else None
            if fn is not None and tokens[fn.body_open].text == "{":
                tokens[fn.body_open].text = "{\n\tself " + glue + "();"
                self.report.rewrites[f"T6 actor glue {glue} -> {key}"] += 1
        for fn in script.functions.values():
            if fn.animtree:
                self.animation_requirements[fn.animtree.lower()].update(
                    animation_names(tokens[fn.start:fn.body_close + 1]))
        self.report.rewrites["stationary FX carriers lowered to authored-position FX"] += lower_stationary_fx_carriers(tokens)
        self.source_lowerings(tokens)
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
        neutralized_ranges = []
        if script.animtree:
            for token in tokens:
                match = gsc.ANIMTREE_RE.match(token.text) if token.kind == gsc.DIRECTIVE else None
                if not match:
                    continue
                tree = match[1].lower()
                self.report.animtrees.setdefault(match[1], []).append(path)
                if tree in self.core_animtrees:
                    token.text = f'#using_animtree( "{animation_tree_name(tree)}" );'
                elif tree not in self.animtrees:
                    token.text = f'// waw2bo2: {token.text} (animtree not converted)'
            for fn in script.functions.values():
                tree = (fn.animtree or '').lower()
                if tree in self.core_animtrees:
                    self.rewrite_animation_refs(tokens, tree, path, fn.start, fn.body_close + 1)
                elif tree and tree not in self.animtrees:
                    neutralize_animations(tokens[fn.start:fn.body_close + 1], tree)
                    neutralized_ranges.append((fn.start, fn.body_close + 1))

        if main_split:
            self._split_main(script)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        defs = {fn.start for fn in script.functions.values()}
        for name, fn in script.functions.items():
            if self.shadows_builtin(name):
                tokens[fn.start].text = shadowed_name(tokens[fn.start].text)
                self.report.rewrites["map functions named like T6 builtins renamed (waw_*)"] += 1
        self.resolve_refs(tokens, script, where_file, included_bo2, defs,
                          neutralized_ranges=neutralized_ranges)
        self.report.rewrites["WaW contact-kill monitors own AI attacks"] += bridge_contact_kill_attacks(tokens, script)
        self.report.rewrites["WaW being_revived reads bridged to T6 revive state"] += bridge_revive_reads(tokens)
        self.report.rewrites["WaW session stat reads bridged to T6 counters"] += bridge_player_stat_reads(tokens)
        self.report.rewrites["WaW is_drinking flag bridged to BO2's drink counter"] += bridge_drinking_flag(tokens)
        self.report.rewrites["WaW score writes keep T6's persistent score"] += bridge_score_writes(tokens)
        self.report.rewrites["WaW HUD network font scales decoded"] += bridge_font_scales(tokens)
        self.report.rewrites["run-time HUD material names -> converted table"] += bridge_dynamic_hud_materials(tokens)
        self.report.rewrites["WaW team-held zombie vars mirrored to BO2's teams"] += mirror_team_zombie_vars(tokens)
        if self.stock_map:
            self.report.rewrites["stock WaW map perk costs -> stock map policy"] +=                 bridge_stock_perk_costs(tokens, API["stock_map_policies"])
        header = (f"// Translated from World at War by waw2bo2 (gscport). Source: {where_file}\n")
        return header + gsc.emit(tokens)

    def adapt_native_arguments(self, tokens: list[gsc.Token], ref, native: str, argc, where: str) -> None:
        """A WaW call resolved to the BO2 function of the same name, whose
        parameters differ (gsc_api native_argument_adapters): BO2's barrier
        helpers take the barrier beside its chunks. Each output argument is a
        WaW argument by index, "object" (what the WaW call runs on),
        "owner:N" (argument N without its last field: the barrier of
        X.barrier_chunks) or a literal."""
        adapter = API["native_argument_adapters"].get(native)
        if adapter is None or argc != adapter["waw_args"]:
            return
        open_ = ref.index + 1
        close = gsc._match(tokens, open_, "(", ")")
        spans, start, depth = [], open_ + 1, 0
        for j in range(open_ + 1, close):
            depth += tokens[j].text in ("(", "[")
            depth -= tokens[j].text in (")", "]")
            if tokens[j].text == "," and depth == 0:
                spans.append((start, j))
                start = j + 1
        spans.append((start, close))
        if any(tokens[j].kind == gsc.IDENT and tokens[j + 1].text == "(" for j in range(open_ + 1, close)):
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {native} arguments call functions; not adapted")
            return
        text = lambda s: " ".join(t.text for t in tokens[s[0]:s[1]]).replace(" . ", ".")
        out = []
        for arg in adapter["args"]:
            if isinstance(arg, int):
                out.append(text(spans[arg]))
            elif arg == "object":
                k = (ref.qual_index if ref.qualifier is not None else ref.index) - 1
                if tokens[k].low == "thread":
                    k -= 1
                if tokens[k].kind != gsc.IDENT or tokens[k - 1].text not in (";", "{", "}"):
                    self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {native} on a compound object; not adapted")
                    return
                out.append(tokens[k].text)
            elif arg.startswith("owner:"):
                s = spans[int(arg.split(":")[1])]
                if s[1] - s[0] < 3 or tokens[s[1] - 2].text != ".":
                    self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {native} chunks without an owner; not adapted")
                    return
                out.append(text((s[0], s[1] - 2)))
            elif arg.startswith("defined_if:"):
                # a WaW flag where BO2 tests only isdefined()
                value = text(spans[int(arg.split(":")[1])]).lower()
                if value not in ("true", "false", "1", "0"):
                    self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {native} flag {value}; not adapted")
                    return
                out.append("1" if value in ("true", "1") else "undefined")
            else:
                out.append(arg)
        for j in range(open_ + 1, close):
            tokens[j].text = ""
        tokens[open_].text = "( " + ", ".join(out) + " "
        self.report.rewrites[f"{native}: WaW arguments adapted"] += 1

    def resolve_refs(self, tokens: list[gsc.Token], script: gsc.Script, where_file: str,
                     included_bo2: set[str], skip: set[int], extracting: str | None = None,
                     neutralized_ranges: list[tuple[int, int]] | None = None) -> None:
        """Rewrite every function reference in ``tokens`` (code of ``script``).
        ``extracting``: the core script whose functions are being extracted;
        its own functions are extracted too instead of kept."""
        # WaW custom scripts may access the hint registry directly. Keep these
        # accesses in the same source registry as the adapted helper calls.
        for i in range(2, len(tokens)):
            if tokens[i].low == 'playername' and tokens[i - 1].text == '.' and \
                    (i + 1 == len(tokens) or tokens[i + 1].text != '('):
                tokens[i].text = 'name'
                self.report.rewrites['WaW playername field -> T6 name'] += 1
            if tokens[i].text.lower() == "zombie_hints" and tokens[i - 1].text == "." and tokens[i - 2].text.lower() == "level":
                tokens[i].text = "waw2bo2_hints"
        self.report.rewrites["zombie_weapons key lists without BO2-only entries"] += lower_zombie_weapon_keys(tokens)
        for ref in gsc.references(tokens):
            if neutralized_ranges is not None:
                self.anim_neutralized = any(start <= ref.index < end for start, end in neutralized_ranges)
            if ref.index in skip or not tokens[ref.index].text:
                continue    # a definition, or inside arguments already dropped
            tok = tokens[ref.index]
            name = ref.name.lower()
            where = f"{where_file}:{tok.line}"
            argc = None if ref.pointer else _argc(tokens, ref.index)
            # T6 retains script_linkname on entities but does not index it
            # for GetEnt. Route literal field queries through a source-order
            # scan; ordinary targetname/classname lookups remain native.
            if (ref.qualifier is None and argc == 2
                    and self.owner(script, name) is None
                    and name in ('getent', 'getentarray')):
                depth = 0
                for end in range(ref.index + 1, len(tokens)):
                    if tokens[end].text == '(':
                        depth += 1
                    elif tokens[end].text == ')':
                        depth -= 1
                        if depth == 0:
                            field = tokens[end - 1].text.strip('"').lower()
                            bridge = API.get('entity_lookup_bridges', {}).get(field, {}).get(name)
                            if (bridge and tokens[end - 1].kind == gsc.STRING
                                    and tokens[end - 1].text.startswith('"')
                                    and tokens[end - 2].text == ','):
                                tok.text = f'{COMPAT}::{bridge}'
                                self.report.rewrites[f'WaW entity lookup field {field}'] += 1
                            break
                if tok.text != ref.name:
                    continue
            perk_owner = norm(ref.qualifier) if ref.qualifier else self.owner(script, name)
            if self.bo2_stock_perks and name in self.waw_perk_parts.get(perk_owner, ()):
                if ref.qualifier is None and perk_owner != script.path:
                    # its include became BO2's _zm_perks: name the WaW script
                    self.want(perk_owner)
                    if ref.pointer:
                        tokens[ref.index - 1].text = ""
                    tok.text = f"{ported_path(perk_owner)}::{ref.name}"
                    continue
            elif self.bo2_stock_perks and stockperks.is_perk_framework(perk_owner):
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
                    if not ref.pointer:
                        self.adapt_native_arguments(tokens, ref, repl, argc, where)
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
            native_cleanup = repl
            if repl is None and owner is not None and API["core_scripts"].get(owner):
                native_cleanup = API["core_scripts"][owner] + "::" + name
            if native_cleanup and not ref.pointer:
                self.adapt_native_arguments(tokens, ref, native_cleanup, argc, where)
            wrapper = API["hud_cleanup_wrappers"].get(native_cleanup)
            if wrapper and ref.method and argc == 0:
                if bridge_hud_cleanup(tokens, ref.index, wrapper):
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

    def signature_differs(self, core: str, name: str) -> bool:
        """The map's own core function takes an argument in another position
        than BO2's counterpart (a parameter of the same name at a different
        index). Extra optional parameters on either side do not count: BO2's
        flag_init( message, val ) still takes WaW's flag_init( message )."""
        mapped = API["core_scripts"].get(core)
        if not mapped or core not in API.get("signature_source_scripts", {}):
            return False
        bo2 = BO2_PARAMS.get(mapped.lower(), {}).get(name)
        script = self.sources.get(core)
        own = script.functions.get(name) if script is not None else None
        if bo2 is None or own is None:
            return False
        return any(p.lower() in bo2 and bo2.index(p.lower()) != i for i, p in enumerate(own.params))

    def extract(self, core: str, name: str) -> str | None:
        """Port one WaW framework function BO2 has no counterpart for."""
        if (core, name) in self.core_funcs:   # deliberately extracted entry (extract_entry)
            return f"{CORE}::{self.core_funcs[(core, name)]}"
        if core in API["no_extract"] and not source_drives(core, name) and \
                not any(name.endswith(s) for s in API["extract_exceptions"].get(core, ())):
            return None
        script = self.sources.get(core)
        if script is None or name not in script.functions:
            return None
        key = (core, name)
        fn = script.functions[name]
        tree = (fn.animtree or "").lower()
        if tree:
            self.animation_requirements[tree].update(animation_names(script.tokens[fn.start:fn.body_close + 1]))
        if key not in self.core_funcs and uses_animations(script.tokens, fn.body_open, fn.body_close) and \
                tree not in self.core_animtrees and not self.animation_discovery:
            # needs its WaW animtree and xanims, which are not converted yet
            self.report.animtrees.setdefault(fn.animtree or "?", []).append(f"{core}::{name}")
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

    def extract_level_state(self, core: str, init: str, read: set[str] | None = None,
                            effect_keys: set[str] | None = None) -> str | None:
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
        # whole top-level statements: an assignment after a block statement
        # (for/if { ... }) is its own statement, not the block's tail
        for start, end in top_level_statements(toks, fn.body_open, fn.body_close):
            stmt = toks[start:end]
            if effect_keys is not None and not (len(stmt) > 5 and stmt[3].text == "[" and stmt[4].kind == gsc.STRING
                                                and stmt[4].text[1:-1].lower() in effect_keys):
                continue  # a keyed effect table: only the entries ported scripts read
            if len(stmt) > 3 and stmt[0].low == "level" and stmt[1].text == "." and stmt[2].low in read and \
                    any(s.text == "=" for s in stmt):
                kept += [gsc.Token(s.kind, s.text, s.pre, s.line) for s in stmt]
        if not kept:
            return None
        name = f"{core.split(chr(92))[-1].lstrip('_')}__{init}_level_state"
        tokens = [gsc.Token(gsc.IDENT, name, f"\n// {core}::{init}: level state its extracted functions read\n"),
                  gsc.Token(gsc.PUNCT, "("), gsc.Token(gsc.PUNCT, ")"), gsc.Token(gsc.PUNCT, "{", "\n"),
                  *kept, gsc.Token(gsc.PUNCT, "}", "\n"), gsc.Token(gsc.EOF, "")]
        if fn.animtree:
            self.animation_requirements[fn.animtree.lower()].update(animation_names(tokens))
        self.keep_core_animations(script, tokens, f"{core}::{init}", fn.animtree)
        tree = (fn.animtree or "").lower()
        if tree and tree not in self.core_animtrees and tree not in self.animtrees:
            neutralize_animations(tokens, fn.animtree)
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.extracted.append(f"{core}::{init} (level state)")
        self.level_state_parts.append(gsc.emit(tokens))
        return name

    # level lists _zombiemode::main fills for the map's spawn loop and zone manager
    SPAWNER_LISTS = {"enemy_spawns", "zombie_rise_spawners"}

    def extract_spawner_lists(self) -> str | None:
        """_zombiemode::main's statements that set up the spawner lists
        round_spawning and the zone manager read (level.enemy_spawns from the
        spawners main is given, level.zombie_rise_spawners), as one function
        taking main's parameters. BO2's _zm::init replaces the rest of main.
        A statement is kept whole when every assignment in it is to one of the
        lists and it calls nothing but isdefined/getentarray."""
        script = self.sources.get(ZOMBIEMODE)
        fn = script.functions.get("main") if script is not None else None
        if fn is None:
            return None
        toks = script.tokens
        kept: list[gsc.Token] = []
        for start, end in top_level_statements(toks, fn.body_open, fn.body_close):
            stmt = toks[start:end]
            targets = {stmt[k + 2].low for k in range(len(stmt) - 3)
                       if stmt[k].low == "level" and stmt[k + 1].text == "." and stmt[k + 3].text == "="}
            calls = {stmt[k].low for k in range(len(stmt) - 1) if stmt[k].kind == gsc.IDENT and stmt[k + 1].text == "("
                     and stmt[k].low not in ("if", "else", "while", "for")}
            if targets and targets <= self.SPAWNER_LISTS and calls <= {"isdefined", "getentarray"}:
                kept += [gsc.Token(s.kind, s.text, s.pre, s.line) for s in stmt]
        if not kept:
            return None
        name = "zombiemode__main_spawner_lists"
        params = [p for p in fn.params]
        head = [gsc.Token(gsc.IDENT, name, f"\n// {ZOMBIEMODE}::main: the spawner lists its spawn loop and zones read\n"),
                gsc.Token(gsc.PUNCT, "(")]
        for k, p in enumerate(params):
            if k:
                head.append(gsc.Token(gsc.PUNCT, ","))
            head.append(gsc.Token(gsc.IDENT, p, " "))
        tokens = [*head, gsc.Token(gsc.PUNCT, ")", " "), gsc.Token(gsc.PUNCT, "{", "\n"),
                  *kept, gsc.Token(gsc.PUNCT, "}", "\n"), gsc.Token(gsc.EOF, "")]
        where = self.sources.origin.get(ZOMBIEMODE + ".gsc", ZOMBIEMODE)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=ZOMBIEMODE)
        self.report.extracted.append(f"{ZOMBIEMODE}::main (spawner lists)")
        self.level_state_parts.append(gsc.emit(tokens))
        return name

    def extract_player_connect_fields(self) -> str | None:
        """The player fields _zombiemode::onPlayerConnect assigns on each
        connecting player (entity_num the map's VO and weapon code index by,
        its starting score, score_total/old_score its scorer reads, ...), as a
        connect loop of their own. BO2's gametype owns the rest of connect.
        Returns the CORE function to thread before players connect."""
        script = self.sources.get(ZOMBIEMODE)
        fn = script.functions.get("onplayerconnect") if script is not None else None
        if fn is None:
            return None
        toks = script.tokens
        loop_var, kept = None, []
        for i in range(fn.body_open, fn.body_close - 6):
            if [t.low for t in toks[i:i + 4]] == ["level", "waittill", "(", '"connecting"'] and \
                    toks[i + 4].text == "," and toks[i + 5].kind == gsc.IDENT:
                loop_var = toks[i + 5].low
                start = gsc._match(toks, i + 2, "(", ")") + 2   # after "...);"
                end = next(k for k in range(start, fn.body_close) if toks[k].text == "}")
                for a, b in top_level_statements(toks, start - 1, end):
                    stmt = toks[a:b]
                    if len(stmt) > 4 and stmt[0].low == loop_var and stmt[1].text == "." and stmt[3].text == "=" \
                            and not any(t.low in ("thread", "waittill", "wait") for t in stmt):
                        kept += [gsc.Token(s.kind, s.text, s.pre, s.line) for s in stmt]
                break
        if not kept:
            return None
        name = "zombiemode__onplayerconnect_fields"
        body = gsc.tokenize(f'{name}()\n{{\n\tfor ( ;; )\n\t{{\n\t\tlevel waittill( "connecting", {loop_var} );\n')[:-1]
        body[0].pre = f"\n// {ZOMBIEMODE}::onPlayerConnect: the player fields its scripts read\n"
        tokens = [*body, *kept, *gsc.tokenize("\n\t}\n}\n")]
        where = self.sources.origin.get(ZOMBIEMODE + ".gsc", ZOMBIEMODE)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW score writes keep T6's persistent score"] += bridge_score_writes(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=ZOMBIEMODE)
        self.report.extracted.append(f"{ZOMBIEMODE}::onplayerconnect (player fields)")
        self.level_state_parts.append(gsc.emit(tokens))
        return name

    def extract_damage_prelude(self, core: str = None, function: str = None) -> str | None:
        """A map's _zombiemode override may put its own rules in front of WaW's
        player_damage_override (e.g. a shield absorbing zombie hits). BO2's _zm
        owns player damage, so those leading statements become a BO2 player
        damage callback: a bare WaW return (no finishPlayerDamage) drops the
        hit, i.e. 0 damage; falling through keeps the (possibly changed) damage.
        The same holds for a map's _callbackglobal Callback_PlayerDamage (e.g. a
        custom PhD Flopper cancelling explosive and fall damage), which BO2
        replaces as well. Returns the name of the extracted registration
        function in CORE."""
        core = core or ZOMBIEMODE
        function = function or DAMAGE_OVERRIDE
        key = core + ".gsc"
        variants = self.sources.stock_variants(core)
        if key not in self.sources.text or not variants or self.sources.text[key] in variants:
            return None
        script = self.sources.get(core)
        fn = script.functions.get(function)
        if fn is None:
            return None
        toks = script.tokens
        where = self.sources.origin.get(key, core)

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
            stock = gsc.parse(variant, core)
            base = stock.functions.get(function)
            spans = top_level_statements(stock.tokens, base.body_open, base.body_close) if base else []
            if spans and text(stock.tokens, spans[0]) in stmts:
                leads.append(stmts.index(text(stock.tokens, spans[0])))
            elif spans:
                anchor = header(stock.tokens, spans[0])
                leads.extend(i for i, span in enumerate(map_spans) if header(toks, span) == anchor)
        if not leads:
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {function} does not keep a stock WaW body; the map's "
                                      f"damage rules are not ported")
            return None
        lead = min(leads)
        stmts = top_level_statements(toks, fn.body_open, fn.body_close)
        if lead == 0:
            return None
        rules = stmts[:lead]
        if self.bo2_stock_perks:
            # BO2 owns the perks: a rule gated on a perk (a WaW PhD Flopper
            # cancelling explosive damage) is the WaW perk itself.
            perk_rules = [span for span in rules if re.search(r"\bhasperk \(", header(toks, span))]
            rules = [span for span in rules if span not in perk_rules]
            if perk_rules:
                self.report.rewrites["WaW perk damage rules dropped (BO2 stock perks)"] += len(perk_rules)
            if not rules:
                return None
        body = copy.deepcopy([t for start, end in rules for t in toks[start:end]])
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
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {function} prelude applies damage itself; not ported")
            return None
        name = f"{core.split(chr(92))[-1].lstrip('_')}__{function}_prelude"
        # BO2 callbacks get WaW's parameters without modelIndex (the 10th)
        params = fn.params[:9] + fn.params[10:]
        model_index = fn.params[9].lower()
        reset = f"\t{fn.params[9]} = undefined;\n" if any(t.low == model_index for t in body) else ""
        # BO2 stops at the first callback returning anything but -1: a hit the
        # rules leave unchanged must return -1, or one prelude hides the next
        # (and every later BO2 callback).
        damage = fn.params[2]
        source = (f"{name}( {', '.join(params)} )\n{{\n{reset}\twaw2bo2_damage_in = {damage};\n" +
                  gsc.emit(body) + f"\n\tif ( {damage} != waw2bo2_damage_in )\n\t\treturn {damage};\n"
                  f"\treturn -1;\n}}\n")
        tokens = gsc.tokenize(source)
        for i in range(len(tokens) - 1, 0, -1):
            if tokens[i].low == "return" and tokens[i + 1].text == ";":
                tokens.insert(i + 1, gsc.Token(gsc.NUMBER, "0", " "))
        tokens[0].pre = f"\n// {core}::{function}: the map's leading rules, as a BO2 damage callback\n"
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.extracted.append(f"{core}::{function} (map prelude -> damage callback)")
        self.level_state_parts.append(gsc.emit(tokens) + f"\n{name}_register()\n{{\n"
                                      f"\tmaps\\mp\\zombies\\_zm::register_player_damage_callback( ::{name} );\n}}\n")
        return f"{name}_register"

    def extract_round_total(self, core: str = None, function: str = None) -> str | None:
        """WaW's round_spawning computes the round's zombie count inline
        (solo stays at zombie_max_ai, ramped by 0.2/0.4/0.6/0.8 on rounds 1-4);
        BO2's _zm adds its own per-player term and ramp, then asks
        level.max_zombie_func. The source statements from the first assignment
        of the variable stored into level.zombie_total up to that store become
        the BO2 max function, so the map's (or stock) formula decides the count.
        Returns the registration function's name in CORE."""
        core = core or ZOMBIEMODE
        function = function or ROUND_SPAWNING
        script = self.sources.get(core)
        fn = script.functions.get(function) if script else None
        if fn is None:
            return None
        toks = script.tokens
        where = self.sources.origin.get(core + ".gsc", core)
        spans = top_level_statements(toks, fn.body_open, fn.body_close)
        store = None
        for i, (start, end) in enumerate(spans):
            words = [t.low for t in toks[start:end]]
            if words[:4] == ["level", ".", "zombie_total", "="] and len(words) == 6 and words[5] == ";" \
                    and toks[start + 4].kind == gsc.IDENT:
                store = i
                break
        if store is None:
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {function} sets no level.zombie_total from a local; the "
                                      f"source round count formula is not ported")
            return None
        total = toks[spans[store][0] + 4].low
        first = next((i for i, (start, end) in enumerate(spans[:store])
                      if toks[start].low == total and toks[start + 1].text == "="), None)
        if first is None:
            return None
        # Locals set before the formula (players, count) would be undefined here.
        earlier = {toks[start].low for start, end in spans[:first]
                   if toks[start].kind == gsc.IDENT and toks[start + 1].text == "="}
        body = [t for start, end in spans[first:store] for t in toks[start:end]]
        used = {t.low for i, t in enumerate(body) if t.kind == gsc.IDENT and (i == 0 or body[i - 1].text != ".")}
        # A local the formula sets itself before any read, with every use inside
        # the block holding that assignment, never sees the earlier value
        # (Verruckt: players = get_players(); in the dog-round branch).
        for name in sorted(earlier & used):
            at = [i for i, t in enumerate(body) if t.low == name and (i == 0 or body[i - 1].text != ".")]
            if body[at[0] + 1].text != "=":
                continue
            depth, open_ = 0, None
            for i in range(at[0] - 1, -1, -1):
                if body[i].text == "}":
                    depth += 1
                elif body[i].text == "{":
                    if depth == 0:
                        open_ = i
                        break
                    depth -= 1
            if open_ is not None and at[-1] < gsc._match(body, open_, "{", "}"):
                used.discard(name)
        if earlier & used:
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: {function} count formula reads earlier locals "
                                      f"{', '.join(sorted(earlier & used))}; not ported")
            return None
        name = f"{core.split(chr(92))[-1].lstrip('_')}__{function}_total"
        source = (f"{name}( bo2_max )\n{{\n" + gsc.emit([gsc.Token(t.kind, t.text, t.pre, t.line) for t in body])
                  + f"\n\treturn {toks[spans[store][0] + 4].text};\n}}\n")
        tokens = gsc.tokenize(source)
        tokens[0].pre = f"\n// {core}::{function}: the source round count formula, as BO2's max_zombie_func\n"
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.extracted.append(f"{core}::{function} (round count formula -> max_zombie_func)")
        self.level_state_parts.append(gsc.emit(tokens) + f"\n{name}_register()\n{{\n"
                                      f"\tlevel.max_zombie_func = ::{name};\n}}\n")
        return f"{name}_register"

    def extract_round_health(self, core: str = None, function: str = None, source_loop: bool = False) -> str | None:
        """WaW's round_spawning calls the framework's health step (stock:
        ai_calculate_health, one increment of level.zombie_health per call).
        Only round_spawning calls it, and a dog round swaps round_spawn_func
        to dog_round_spawning, so dog rounds never raise health. BO2's
        round_spawning instead rebuilds health from the round number. The
        function the source round_spawning calls with no arguments that
        assigns level.zombie_health is extracted and registered as
        level.waw2bo2_round_health_func; the owned _zm round_spawning calls it
        in place of BO2's ai_calculate_health (WAW_ROUND_HEALTH), so the map's
        own code decides (docs/CONVERT_THE_WHOLE_MAP.md).
        Returns the registration function's name in CORE."""
        core = core or ZOMBIEMODE
        function = function or ROUND_SPAWNING
        script = self.sources.get(core)
        fn = script.functions.get(function) if script else None
        if fn is None:
            return None
        toks = script.tokens
        where = self.sources.origin.get(core + ".gsc", core)
        step = None
        for start, end in top_level_statements(toks, fn.body_open, fn.body_close):
            words = [t.text for t in toks[start:end]]
            callee = script.functions.get(toks[start].low) if toks[start].kind == gsc.IDENT else None
            if callee is None or words[1:] != ["(", ")", ";"]:
                continue
            body = [t.low for t in toks[callee.body_open:callee.body_close]]
            if any(body[i:i + 3] == ["level", ".", "zombie_health"] for i in range(len(body) - 2)):
                step = toks[start].low
                break
        loop = script.functions.get("round_think")
        if step is None and source_loop and loop is not None:
            # Older frameworks (Verruckt) step the health in round_think
            # itself; the source loop runs that call, so BO2's round_spawning
            # must leave the health alone.
            toks_loop = toks[loop.body_open + 1:loop.body_close]
            for i in range(1, len(toks_loop) - 3):
                callee = script.functions.get(toks_loop[i].low) if toks_loop[i].kind == gsc.IDENT else None
                if callee is None or [t.text for t in toks_loop[i + 1:i + 4]] != ["(", ")", ";"] \
                        or toks_loop[i - 1].text not in (";", "{", "}"):
                    continue
                body = [t.low for t in toks[callee.body_open:callee.body_close]]
                if any(body[j:j + 3] == ["level", ".", "zombie_health"] for j in range(len(body) - 2)):
                    self.report.rewrites[f"WaW health step {toks_loop[i].low} in the source round loop"] += 1
                    self.level_state_parts.append(
                        f"\n// {core}::round_think calls {toks_loop[i].low} itself (the source loop runs it)\n"
                        "waw2bo2_health_by_round_loop()\n{\n}\n\n"
                        "waw2bo2_health_in_round_loop_register()\n{\n"
                        "\tlevel.waw2bo2_health_in_round_loop = 1;\n"
                        "\tlevel.waw2bo2_round_health_func = ::waw2bo2_health_by_round_loop;\n}\n")
                    return "waw2bo2_health_in_round_loop_register"
        if step is None:
            # reported, never fatal: the map still converts with BO2's health
            self.report.unsupported.setdefault(
                f"GSC {core}::{function} health step (no zombie health function called; BO2's runs)",
                []).append(where)
            return None
        extracted = self.extract_entry(core, step)
        name = f"{core.split(chr(92))[-1].lstrip('_')}__{step}_register"
        self.level_state_parts.append(
            f"\n// {core}::{function} calls {step} once per round it runs (never on a dog round)\n"
            f"{name}()\n{{\n\tlevel.waw2bo2_round_health_func = ::{extracted};\n}}\n")
        return name

    def extract_round_counter(self, core: str = None, source_loop: bool = False) -> str | None:
        """Stock maps: WaW's own round counter (stock_map_policies waw_round_counter).
        BO2 shows rounds through setroundsplayed, an 8-bit display, and clamps
        level.round_number at 255; WaW draws script HUD elements (chalk tallies,
        then SetValue) with no limit. The source display functions run where
        WaW's round_think called them: the round-start one on BO2's
        zombie_round_change_custom hook, the round-end one on end_of_round. The
        HUD objects come from the source round-start assignments that create
        them with the framework's own functions. Returns the registration name.
        ``source_loop``: the map's own round loop runs (extract_round_loop) and
        calls the display itself; only the HUD state and materials are set up."""
        core = core or ZOMBIEMODE
        contract = API["stock_map_policies"]["waw_round_counter"]
        script = self.sources.get(core)
        where = self.sources.origin.get(core + ".gsc", core)
        names = (contract["round_start"], contract["round_end"])
        if script is None or any(n not in script.functions for n in names):
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{where}: no WaW round display ({', '.join(names)}); BO2's counter stays")
            return None
        toks = script.tokens
        # level fields the display reads that the source start-of-game creates
        # with this framework's own functions (the HUD objects; never round state)
        read = set()
        for n in names:
            fn = script.functions[n]
            read |= level_fields_read(toks, fn.body_open + 2, fn.body_close)
        created = set()
        init = script.functions.get(contract["state_init"])
        if init is not None:
            for start, end in top_level_statements(toks, init.body_open, init.body_close):
                words = toks[start:end]
                if len(words) > 5 and words[0].low == "level" and words[1].text == "." and words[3].text == "=" \
                        and words[4].kind == gsc.IDENT and words[4].low in script.functions:
                    created.add(words[2].low)
        # the display's helpers in this framework script (create_chalk_hud), so
        # the HUD objects and display calls resolve to source code, not stubs
        helpers, queue = set(), [script.functions[n] for n in names]
        if init is not None:
            for start, end in top_level_statements(toks, init.body_open, init.body_close):
                if toks[start].low == "level" and toks[start + 2].low in read & created:
                    for r in gsc.references(toks, start, end):
                        if r.qualifier is None and r.name.lower() in script.functions:
                            helpers.add(r.name.lower())
                            queue.append(script.functions[r.name.lower()])
        while queue:
            fn = queue.pop()
            for r in gsc.references(toks, fn.body_open, fn.body_close):
                name = r.name.lower()
                if r.qualifier is None and name in script.functions and name not in helpers and name not in names:
                    helpers.add(name)
                    queue.append(script.functions[name])
        mapped = API["core_scripts"].get(core)
        driven = core in API.get("source_driven", {})
        for name in sorted(helpers):
            if driven or not (mapped and self.api.defines(mapped, name)):   # else BO2's counterpart resolves first
                self.extract_entry(core, name)
        state = self.extract_level_state(core, contract["state_init"], read & created) if read & created else None
        start = self.extract_entry(core, contract["round_start"])
        end = self.extract_entry(core, contract["round_end"])
        # HUD materials the display draws: the source precaches every name it
        # may build at run time ("hud_chalk_" + n) with literal PrecacheShader
        literals = set()
        for n in names:
            fn = script.functions[n]
            literals |= {t.text.strip('"') for t in toks[fn.body_open:fn.body_close] if t.kind == gsc.STRING}
        precache = []
        for fn_name in contract["precache_functions"]:
            fn = script.functions.get(fn_name)
            if fn is None:
                continue
            for ref in gsc.references(toks, fn.body_open, fn.body_close):
                if ref.name.lower() != "precacheshader" or toks[ref.index + 2].kind != gsc.STRING:
                    continue
                material = toks[ref.index + 2].text.strip('"')
                if any(material.startswith(l) for l in literals if l) and material not in precache:
                    precache.append(material)
        self.report.waw_hud_materials += [m for m in precache if m not in self.report.waw_hud_materials]
        if source_loop:
            lines = ["", f"// {core}: the source round display's HUD state (the source loop draws it)",
                     "waw2bo2_waw_round_counter()", "{", "\tlevel.waw2bo2_waw_round_counter = 1;",
                     *[f'\tprecacheshader( "{m}" );' for m in precache],
                     *([f"\t{state}();"] if state else []), "}", ""]
            self.level_state_parts.append("\n".join(lines))
            self.report.rewrites["source round display (source loop)"] += 1
            return "waw2bo2_waw_round_counter"
        lines = ["", f"// {core}: WaW's round counter (stock_map_policies waw_round_counter); BO2's 8-bit",
                 "// setroundsplayed display and its 255 round clamp stay unused",
                 "waw2bo2_waw_round_counter()", "{", "\tlevel.waw2bo2_waw_round_counter = 1;",
                 *[f'\tprecacheshader( "{m}" );' for m in precache],
                 "\tlevel.zombie_round_change_custom = ::waw2bo2_waw_round_start;",
                 # BO2's gametype starts the stock round_think; only the owned
                 # copy skips the 8-bit display and the 255 clamp
                 f"\tlevel.round_think_func = {OWNED_ZM}::round_think;",
                 "\tlevel thread waw2bo2_waw_round_end();", "}", "",
                 "waw2bo2_waw_round_start()", "{",
                 "\tif ( !isdefined( level.waw2bo2_waw_round_hud ) )", "\t{",
                 "\t\tlevel.waw2bo2_waw_round_hud = 1;",
                 *([f"\t\t{state}();"] if state else []), "\t}",
                 # BO2's round_one_up path also started the round music
                 '\tlevel thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( "round_start" );',
                 f"\t{start}();", "}", "",
                 "waw2bo2_waw_round_end()", "{", '\tlevel endon( "end_game" );', "\tfor ( ;; )", "\t{",
                 '\t\tlevel waittill( "end_of_round" );', f"\t\tlevel thread {end}();", "\t}", "}", ""]
        self.level_state_parts.append("\n".join(lines))
        self.report.rewrites["stock WaW map round counter (source display)"] += 1
        return "waw2bo2_waw_round_counter"

    def rewrite_animation_refs(self, tokens: list[gsc.Token], tree: str, where: str,
                               start: int = 0, end: int | None = None) -> None:
        available = self.core_animtrees[tree.lower()]
        for i in range(max(start, 1), (len(tokens) if end is None else end) - 1):
            if tokens[i].text != "%" or tokens[i + 1].kind != gsc.IDENT or \
                    tokens[i - 1].text not in ("(", ",", "=", "[", "return", "[["):
                continue
            name = tokens[i + 1].low
            if name in available:
                tokens[i + 1].text = animation_asset_name(name)
            else:
                self.report.unsupported.setdefault(f"XANIM {name} (animtree {tree}: no WaW xanim)", []).append(where)
                tokens[i].text = ""
                tokens[i + 1].text = "undefined"

    def keep_core_animations(self, script: gsc.Script, tokens: list[gsc.Token], where: str,
                             animtree: str | None = None) -> None:
        """Extracted code with ``%anim`` references from a staged animtree: name
        the tree before the function (the extracted functions share one file),
        and drop references to animations with no WaW xanim (``undefined``)."""
        tree = (animtree or script.animtree or "").lower()
        uses_tree = any(t.text == "#" and tokens[i + 1].low == "animtree" for i, t in enumerate(tokens[:-1]))
        if tree not in self.core_animtrees or not (uses_tree or uses_animations(tokens, 0, len(tokens) - 1)):
            return
        tokens[0].pre += f'#using_animtree( "{animation_tree_name(tree)}" );\n'
        self.rewrite_animation_refs(tokens, tree, where)

    def source_lowerings(self, tokens: list[gsc.Token]) -> None:
        """WaW semantics lowered onto T6 for ported and extracted source code alike."""
        self.report.rewrites["maxhealth assignments through setmaxhealth for players"] += lower_maxhealth_assignments(tokens)
        self.report.rewrites["zombie_weapons key lists without BO2-only entries"] += lower_zombie_weapon_keys(tokens)
        self.report.rewrites["HUD fields (safe-area anchors, hidden in menus)"] += lower_hud_fields(tokens)

    def translate_extracted(self, core: str, name: str) -> str:
        script = self.sources.get(core)
        fn = script.functions[name]
        if fn.animtree:
            self.animation_requirements[fn.animtree.lower()].update(
                animation_names(script.tokens[fn.start:fn.body_close + 1]))
        tokens = [gsc.Token(t.kind, t.text, t.pre, t.line) for t in script.tokens[fn.start:fn.body_close + 1]]
        tokens.append(gsc.Token(gsc.EOF, ""))
        tokens[0].text = self.core_funcs[(core, name)]
        self.anim_neutralized = False
        tokens[0].pre = f"\n// {core}::{name} ({'map override' if core + '.gsc' in self.sources.text else 'stock'})\n"
        self.keep_core_animations(script, tokens, f"{core}::{name}", fn.animtree)
        tree = (fn.animtree or "").lower()
        uses_tree = any(t.text == "#" and tokens[i + 1].low == "animtree" for i, t in enumerate(tokens[:-1]))
        if uses_tree and tree not in self.core_animtrees:
            # The extracted functions share one file, so the source script's
            # #using_animtree does not come along (e.g. _spawner's
            # UseAnimTree( #animtree ) with no %anim: "trying to use animtree
            # without specified using animtree").
            if tree in self.animtrees:
                tokens[0].pre += f'#using_animtree( "{fn.animtree}" );\n'
            else:
                neutralize_animations(tokens, fn.animtree or "")
        where = self.sources.origin.get(core + ".gsc", core)
        self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
        self.source_lowerings(tokens)
        self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
        self.report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += keep_bo2_deathanims(tokens)
        self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=core)
        self.report.rewrites["WaW HUD network font scales decoded"] += bridge_font_scales(tokens)
        self.report.rewrites["run-time HUD material names -> converted table"] += bridge_dynamic_hud_materials(tokens)
        self.report.rewrites["WaW team-held zombie vars mirrored to BO2's teams"] += mirror_team_zombie_vars(tokens)
        if (core, name) in self.intermission_funcs:
            self.report.rewrites["intermission player link -> T6 script camera"] += bridge_intermission_camera(tokens)
        text = gsc.emit(tokens)
        patch = self.post_patches.get((core, name))
        return patch(text) if patch else text

    def extract_round_loop(self, core: str = None, function: str = "round_think") -> str | None:
        """WaW drives the rounds (docs/CONVERT_THE_WHOLE_MAP.md): the map's own
        round loop (_zombiemode::round_think and what it calls: round_wait,
        add_later_round_spawners, the chalk display, award_grenades...) runs as
        BO2's level.round_think_func. BO2's systems still need the signals its
        own loop gives (start_of_round / end_of_round, stats, the round display,
        hot-join Quick Revive): owned _zm glue (WAW_SOURCE_ROUND_GLUE) is called
        after the anchors every WaW loop shares. A loop without them is
        reported and BO2's loop stays. Returns the registration name."""
        core = core or ZOMBIEMODE
        script = self.sources.get(core)
        fn = script.functions.get(function) if script else None
        if fn is None:
            return None
        where = self.sources.origin.get(core + ".gsc", core)
        body = gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
        missing = [label for label, pattern in SOURCE_ROUND_ANCHORS
                   if len(re.findall(pattern.replace("ROUND_WAIT", r"round_wait"), body, re.IGNORECASE)) != 1]
        if missing:
            self.report.unsupported.setdefault(
                f"GSC {core}::{function} round loop (no anchors {missing}; BO2's loop runs)", []).append(where)
            return None
        name = self.extract_entry(core, function)

        def glue(text: str) -> str:
            for label, pattern in SOURCE_ROUND_ANCHORS:
                # translated: the source round_wait() became the extracted name
                pattern = pattern.replace("ROUND_WAIT", r"\w*round_wait")
                before, after = SOURCE_ROUND_GLUE_CALLS[label]
                text, count = re.subn(pattern, lambda m: (f"{OWNED_ZM}::{before}();\n\t\t" if before else "")
                                      + m[0] + (f"\n\t\t{OWNED_ZM}::{after}();" if after else ""),
                                      text, count=1, flags=re.IGNORECASE)
                if count != 1:
                    raise ValueError(f"{where}: {function} translated without anchor {label}")
            # A local spawn function (older loops) is round_spawning, which is
            # glue: BO2's (T6 actors, source hooks), the function BO2 keeps in
            # level.round_spawn_func and dog rounds swap.
            text, local = re.subn(r"(?<![.\w])round_spawn_func\s*=\s*[^;]+;",
                                  "round_spawn_func = level.round_spawn_func;", text)
            if local:
                self.report.rewrites["source loop's local spawn function -> level.round_spawn_func"] += local
            return text

        self.post_patches[(core, function)] = glue
        # the map's own spawn loop (round_start sets level.round_spawn_func to it;
        # dog rounds swap it and restore it, as in WaW)
        spawning = None
        if "round_spawning" in script.functions and source_drives(core, "round_spawning"):
            spawning = self.extract_entry(core, "round_spawning")
        if spawning:
            self.report.rewrites["WaW drives the spawning: source round_spawning -> round_spawn_func"] += 1
        spawn_lines = (f"\tlevel.waw2bo2_source_round_spawning = ::{spawning};\n"
                       f"\tlevel.round_spawn_func = ::{spawning};\n") if spawning else ""
        self.report.rewrites["WaW drives the rounds: source round_think -> round_think_func"] += 1
        self.report.extracted.append(f"{core}::{function} (round loop -> round_think_func)")
        self.level_state_parts.append(
            f"\n// {core}::{function}: the map's round loop drives the rounds; BO2's glue runs at its anchors\n"
            f"waw2bo2_source_round_think( restart )\n{{\n"
            f'\tlevel endon( "end_round_think" );\n'
            f"\t{OWNED_ZM}::waw2bo2_round_think_begin( restart );\n"
            f"\t{name}();\n}}\n\n"
            f"waw2bo2_source_round_loop()\n{{\n"
            f"\tlevel.waw2bo2_source_round_loop = 1;\n"
            f"{spawn_lines}"
            f"\tlevel.round_think_func = ::waw2bo2_source_round_think;\n}}\n")
        return "waw2bo2_source_round_loop"

    def map_framework_function(self, name: str) -> gsc.Function | None:
        """``name`` in the map's _zombiemode override when the map edited it
        (its tokens differ from every stock WaW text of the function)."""
        key = ZOMBIEMODE + ".gsc"
        variants = self.sources.stock_variants(ZOMBIEMODE)
        if key not in self.sources.text or not variants or self.sources.text[key] in variants:
            return None
        script = self.sources.get(ZOMBIEMODE)
        fn = script.functions.get(name.lower()) if script is not None else None
        if fn is None:
            return None
        body = [t.low for t in script.tokens[fn.start:fn.body_close + 1]]
        for variant in variants:
            stock = gsc.parse(variant, ZOMBIEMODE)
            base = stock.functions.get(name.lower())
            if base is not None and [t.low for t in stock.tokens[base.start:base.body_close + 1]] == body:
                return None
        return fn

    def extract_custom_intermission(self) -> str | None:
        """A map's _zombiemode override may set level.custom_intermission to its
        own end-game camera (e.g. Nuketown flying the view down with a nuke).
        BO2's _zm::init sets level.custom_intermission to BO2's camera path, so
        the map's version is extracted and set again after _zm::init.
        Returns the call that sets it (for waw_main_post)."""
        script = self.sources.get(ZOMBIEMODE)
        if script is None:
            return None
        toks = script.tokens
        target = None
        for i in range(len(toks) - 5):
            if [t.low for t in toks[i:i + 5]] == ["level", ".", "custom_intermission", "=", "::"]:
                target = toks[i + 5].low
                break
        if target is None or self.map_framework_function(target) is None:
            return None
        self.intermission_funcs.add((ZOMBIEMODE, target))
        name = self.extract_entry(ZOMBIEMODE, target)
        if name is None:
            return None
        self.post_calls.append(f"level.custom_intermission = {CORE}::{name};")
        self.report.rewrites["map custom_intermission -> set after _zm::init"] += 1
        return name

    def extract_end_game(self) -> list[str]:
        """The parts of a map's edited _zombiemode::end_game that BO2's _zm
        end_game (which replaces it) does not do:
        - the statements before its "end_game" wait run at level start (e.g.
          hiding the end-game camera rocket);
        - the map's own wait between intermission() and the exit (e.g. until
          its cutscene notifies the end) replaces BO2's fixed
          zombie_intermission_time wait. BO2's wait is read when intermission()
          returns, before a cutscene's length is known, so the map's wait runs
          in its own thread followed by the exit BO2 and WaW both make (stop
          the intermission, fade to black, 1.5 s, exit level); BO2's timer is
          raised to a fallback that only ends a cutscene that never finishes.
        Returns the names of the extracted functions in CORE."""
        fn = self.map_framework_function("end_game")
        if fn is None:
            return []
        script = self.sources.get(ZOMBIEMODE)
        toks = script.tokens
        where = self.sources.origin.get(ZOMBIEMODE + ".gsc", ZOMBIEMODE)
        prefix = ZOMBIEMODE.split(chr(92))[-1].lstrip('_')

        def find(ts: list[gsc.Token], spans: list[tuple[int, int]], words: list[str], after: int = 0) -> int | None:
            return next((k for k, (a, b) in enumerate(spans) if k >= after and [t.low for t in ts[a:b]] == words),
                        None)

        def intermission_wait(ts: list[gsc.Token], spans: list[tuple[int, int]]) -> tuple[int, int] | None:
            begin = find(ts, spans, ["intermission", "(", ")", ";"])
            stop = find(ts, spans, ["level", "notify", "(", '"stop_intermission"', ")", ";"],
                        0 if begin is None else begin)
            return None if begin is None or stop is None else (begin + 1, stop)

        def function(name: str, comment: str, body: list[gsc.Token], tail: str = "") -> str:
            tokens = [gsc.Token(gsc.IDENT, name, f"\n// {ZOMBIEMODE}::end_game: {comment}\n"),
                      gsc.Token(gsc.PUNCT, "("), gsc.Token(gsc.PUNCT, ")"), gsc.Token(gsc.PUNCT, "{", "\n"),
                      *copy.deepcopy(body), gsc.Token(gsc.PUNCT, "}", "\n"), gsc.Token(gsc.EOF, "")]
            self.report.rewrites["syntax fixes (T6 compiler)"] += fix_syntax(tokens)
            self.report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += rename_level_fields(tokens)
            self.resolve_refs(tokens, script, where, set(CORE_INCLUDES), {0}, extracting=ZOMBIEMODE)
            tokens[-2].text = tail + tokens[-2].text
            self.level_state_parts.append(gsc.emit(tokens))
            return name

        spans = top_level_statements(toks, fn.body_open, fn.body_close)
        names = []
        wait = find(toks, spans, ["level", "waittill", "(", '"end_game"', ")", ";"])
        if wait:
            names.append(function(f"{prefix}__end_game_prelude", "the map's statements before the end_game wait",
                                  toks[spans[0][0]:spans[wait - 1][1]]))
            self.report.extracted.append(f"{ZOMBIEMODE}::end_game (map prelude)")
        span = intermission_wait(toks, spans)
        if span is not None and span[0] < span[1]:
            body = [t.low for t in toks[spans[span[0]][0]:spans[span[1] - 1][1]]]
            stock = []
            for variant in self.sources.stock_variants(ZOMBIEMODE):
                parsed = gsc.parse(variant, ZOMBIEMODE)
                base = parsed.functions.get("end_game")
                if base is None:
                    continue
                base_spans = top_level_statements(parsed.tokens, base.body_open, base.body_close)
                found = intermission_wait(parsed.tokens, base_spans)
                if found is not None and found[0] < found[1]:
                    stock.append([t.low for t in parsed.tokens[base_spans[found[0]][0]:base_spans[found[1] - 1][1]]])
            if body not in stock:
                head = [gsc.Token(t.kind, t.text, t.pre) for t in gsc.tokenize(
                    "\n\tlevel waittill( \"end_game\" );"
                    "\n\t// BO2's end_game waits zombie_intermission_time after intermission(); this map's own"
                    "\n\t// wait decides instead. BO2's timer only ends a cutscene that never finishes."
                    f"\n\tlevel.zombie_vars[\"zombie_intermission_time\"] = {END_GAME_FALLBACK};"
                    "\n\tlevel waittill( \"intermission\" );")[:-1]]
                tail = ("\tlevel notify( \"stop_intermission\" );"
                        "\n\tarray_thread( get_players(), maps\\mp\\zombies\\_zm::player_exit_level );"
                        "\n\twait 1.5;\n\tplayers = get_players();"
                        "\n\tfor ( i = 0; i < players.size; i++ )\n\t\tplayers[i] cameraactivate( 0 );"
                        "\n\texitlevel( 0 );\n")
                names.append(function(f"{prefix}__end_game_intermission_wait",
                                      "the map's wait after intermission(), then the exit",
                                      head + toks[spans[span[0]][0]:spans[span[1] - 1][1]], tail))
                self.report.extracted.append(f"{ZOMBIEMODE}::end_game (intermission wait)")
                self.report.rewrites["map end_game intermission wait -> replaces BO2's fixed wait"] += 1
        for name in names:
            self.post_calls.append(f"level thread {CORE}::{name}();")
        return names

    def juggernog_hooks(self) -> list[str]:
        """WaW's Juggernog, carried into the BO2 perk runtime.

        WaW _zombiemode_perks sets player.maxhealth/health to 160 on the drink
        and _zombiemode::main threads check_for_jugg_perk: every 0.5 s a
        Juggernog player gains 40 health (3 during dog rounds), capped at 160.
        The regen loop was never started in the port, so three zombie hits (60
        each) downed a Juggernog player. The BO2 perk script, which also runs,
        reapplies max health from zombie_vars["zombie_perk_juggernaut_health"],
        which nothing set. The value and the loop both come from the source."""
        calls = []
        perks = self.sources.text.get(PERKS + ".gsc", "")
        health = [int(n) for n in re.findall(r"\bplayer\.maxhealth\s*=\s*(\d+)\s*;", perks, re.IGNORECASE)
                  if int(n) > 100]
        if health:
            calls.append(f'level.zombie_vars["zombie_perk_juggernaut_health"] = {health[0]};')
            calls.append(f'level.zombie_vars["zombie_perk_juggernaut_health_upgrade"] = {health[0]};')
            self.report.rewrites["WaW Juggernog health -> BO2 zombie_perk_juggernaut_health"] += 1
        script = self.sources.get(ZOMBIEMODE)
        main = script.functions.get("main") if script is not None else None
        if main is not None and "check_for_jugg_perk" in script.functions and any(
                r.name.lower() == "check_for_jugg_perk"
                for r in gsc.references(script.tokens, main.body_open, main.body_close)):
            name = self.extract_entry(ZOMBIEMODE, "check_for_jugg_perk")
            calls.append(f"level thread {CORE}::{name}();")
            self.report.rewrites["WaW check_for_jugg_perk regen -> started after _zm::init"] += 1
        return calls

    def announcer_lines(self, core: str, function: str, depth: int = 5) -> dict[str, list[str]]:
        """{case label: aliases} for the switch in ``function`` of ``core``:
        the literal aliases each case hands to a sound helper of the same
        script (play_devil_dialog( "insta_vox" ), reached from case
        "insta_kill" through insta_kill_powerup), in call order."""
        script = self.sources.get(core)
        fn = script.functions.get(function) if script else None
        if fn is None:
            return {}
        toks = script.tokens
        lines: dict[str, list[str]] = {}

        def literals(open_: int, close: int, seen: set[str], level: int) -> list[str]:
            found = []
            for ref in gsc.references(toks, open_, close):
                i, callee = ref.index, ref.name.lower()
                if i + 3 >= len(toks) or toks[i + 1].text != "(":
                    continue
                owner = self.function_script(core, ref)
                if owner is None:
                    continue
                if toks[i + 2].kind == gsc.STRING and toks[i + 3].text in (",", ")") \
                        and self.plays_argument_sound(owner, callee):
                    found.append(toks[i + 2].text.strip('"').lower())
                elif owner == core and level < depth and callee not in seen:
                    inner = script.functions[callee]
                    found += literals(inner.body_open, inner.body_close, seen | {callee}, level + 1)
            return found

        for i in range(fn.body_open, fn.body_close):
            if toks[i].low != "switch":
                continue
            close = gsc._match(toks, gsc._match(toks, i + 1, "(", ")") + 1, "{", "}")
            cases = [j for j in range(i, close) if toks[j].low in ("case", "default")]
            for k, j in enumerate(cases):
                if toks[j].low != "case" or toks[j + 1].kind != gsc.STRING:
                    continue
                end = cases[k + 1] if k + 1 < len(cases) else close
                label = toks[j + 1].text.strip('"')
                aliases = literals(j, end, {function}, 1)
                if aliases:
                    lines[label] = list(dict.fromkeys(aliases))
        return lines

    def powerup_models_hook(self) -> list[str]:
        """BO2's power-up script registers the drops with BO2's models; the
        map's own power-up script names its models (Shi No Numa:
        add_zombie_powerup( "nuke", "zombie_bomb", ... )). BO2's
        add_zombie_powerup keeps the model in level.zombie_powerups[name], so
        the map's model replaces it there. The literal also makes the model
        part of the conversion and of the first-frame precache."""
        contract = API["map_content"]["powerup_models"]
        script = self.sources.get(contract["script"])
        if script is None:
            return []
        toks, models = script.tokens, {}
        for ref in gsc.references(toks, 0, len(toks)):
            i = ref.index
            if ref.name.lower() != contract["register"] or i + 5 >= len(toks) or toks[i + 1].text != "(":
                continue
            if toks[i + 2].kind == gsc.STRING and toks[i + 3].text == "," and toks[i + 4].kind == gsc.STRING:
                models[toks[i + 2].text.strip('"')] = toks[i + 4].text.strip('"')
        if not models:
            return []
        body = "".join(f'    {COMPAT}::waw_precachemodel( "{model}" );\n'
                       f'    if ( isdefined( level.zombie_powerups ) && isdefined( level.zombie_powerups["{name}"] ) )\n'
                       f'        level.zombie_powerups["{name}"].model_name = "{model}";\n'
                       for name, model in sorted(models.items()))
        self.level_state_parts.append(
            f"\n// {contract['script']}: the map's power-up models on BO2's power-up registry\n"
            "waw2bo2_powerup_models()\n{\n" + body + "}\n")
        self.report.rewrites["WaW power-up models -> BO2 power-up registry"] += len(models)
        return [f"{CORE}::waw2bo2_powerup_models();"]

    def native_replacements(self) -> list[str]:
        """BO2 framework code calls its own functions where WaW called the
        map's (BO2's _zm::init -> _zm_powerups::init, its spawner ->
        powerup_drop). Plutonium's replacefunc puts the map's function there
        (gsc_api native_replacements), through a wrapper with BO2's arity that
        passes the map's own arguments on. A replacement's "keep" lists the BO2
        calls that still run first, each for a stated engine reason."""
        calls, parts = [], []
        for core, entries in API["native_replacements"].items():
            if core == "doc":
                continue
            for entry in entries:
                native_script, native = entry["native"].rsplit("::", 1)
                script = self.sources.get(core)
                fn = script.functions.get(entry["function"]) if script else None
                arity = self.api.scripts.get(native_script.lower(), {}).get(native)
                if fn is None or arity is None:
                    continue
                name = self.extract_entry(core, entry["function"])
                params = [f"arg{i}" for i in range(arity)]
                passed = ", ".join(params[:len(fn.params)])
                wrapper = f"waw2bo2_native_{native}"
                keep = "".join(f"    // {k['reason']}\n    {k['call']};\n" for k in entry.get("keep", []))
                parts.append(f"// {core}::{entry['function']} in place of {entry['native']}\n"
                             f"{wrapper}( {', '.join(params)} )\n{{\n{keep}"
                             f"    return self {CORE}::{name}( {passed} );\n}}\n")
                calls.append(f"replacefunc( {entry['native']}, {CORE}::{wrapper} );")
        if parts:
            self.level_state_parts.append("\n" + "\n".join(parts))
            self.report.rewrites["WaW functions in place of BO2's (replacefunc)"] += len(parts)
        return calls

    def quote_hooks(self) -> list[str]:
        """WaW's player quotes start in framework code BO2 replaces. Where BO2
        calls its own quote function at the same moment with the same
        arguments (a kill's zombie_death_points, a power-up grab), Plutonium's
        replacefunc puts the map's function in its place. The map's
        zombie_death_event (death vocals, the kill count its kill-streak timer
        reads, the close-call line, zom_kill) runs where WaW threads it, on
        each spawned zombie; the owned spawner drops BO2's copies of that work
        (zombiebehavior.waw_quote_rules). _zombiemode::main's ammo watcher
        starts with the map."""
        contract = API["quote_triggers"]
        parts, calls = [], []
        for entry in contract["replace"]:
            native_script, native = entry["native"].rsplit("::", 1)
            name = self.extract_entry(entry["script"], entry["function"]) \
                if self.api.defines(native_script, native) else None
            if name:
                calls.append(f"    // {entry['script']}::{entry['function']} at BO2's call\n"
                             f"    replacefunc( {entry['native']}, {CORE}::{name} );\n")
        death = contract["death_event"]
        name = self.extract_entry(death["script"], death["function"])
        if name:
            parts.append(f"// {death['script']}: zombie_spawn_init threads it on each zombie\n"
                         "waw2bo2_zombie_death_event( zombie )\n{\n"
                         f"    level thread {CORE}::{name}( zombie );\n}}\n")
        ammo = contract["ammo"]
        name = self.extract_entry(ammo["script"], ammo["function"])
        if name:
            calls.append(f"    level thread {CORE}::{name}();\n")
        if not parts and not calls:
            return []
        self.level_state_parts.append(
            "\n" + "\n".join(parts) + "\n// the map's quote triggers on BO2's events\n"
            "waw2bo2_quote_triggers()\n{\n" + "".join(calls) + "}\n")
        self.report.rewrites["WaW quote triggers -> BO2 events"] += len(parts) + len(calls)
        return [f"{CORE}::waw2bo2_quote_triggers();"]

    def zombie_vocals_hook(self) -> list[str]:
        """BO2's zombie actors voice their animation events through
        level.zmb_vox (prefix + category alias); WaW's zombies voiced the same
        events from their own animations with the map's aliases (amb_vocals,
        sprint_vocals, ...). Each category whose WaW alias the bank holds plays
        it; the others keep BO2's full name."""
        table = API["zombie_vocals"]
        body = ("    if ( !isdefined( level.zmb_vox ) || !isdefined( level.zmb_vox[\"prefix\"] ) )\n        return;\n"
                "    prefix = level.zmb_vox[\"prefix\"];\n"
                "    foreach ( type, entries in level.zmb_vox )\n    {\n"
                "        if ( type == \"prefix\" )\n            continue;\n"
                "        foreach ( category, alias in entries )\n"
                "            level.zmb_vox[type][category] = prefix + alias;\n    }\n"
                "    level.zmb_vox[\"prefix\"] = \"\";\n")
        for actor, categories in table.items():
            if actor == "doc":
                continue
            body += f'    if ( !isdefined( level.zmb_vox["{actor}"] ) )\n        level.zmb_vox["{actor}"] = [];\n'
            for category, alias in categories.items():
                body += (f'    if ( soundexists( "waw/{alias}" ) )\n'
                         f'        level.zmb_vox["{actor}"]["{category}"] = "waw/{alias}";\n')
        self.level_state_parts.append(
            "\n// BO2's zombie vocal events play the map's own voices\nwaw2bo2_zombie_vocals()\n{\n" + body + "}\n")
        self.report.rewrites["WaW zombie vocals -> BO2 zmb_vox"] += 1
        return [f"{CORE}::waw2bo2_zombie_vocals();"]

    def sound_table_hook(self) -> list[str]:
        """BO2's init_sounds fills level.zombie_sounds with BO2 aliases
        (music_chest -> zmb_music_box); the map's framework fills it with its
        own (music_chest -> music_box). WaW code and BO2 code both play a ref
        through that table, so each ref WaW names is rebound to WaW's alias as
        the bank holds it (waw/music_box), which the compat sound lookup takes
        as it is. The framework's table first, then any the map adds."""
        contract = API["map_content"]["sound_table"]
        framework = norm(contract["script"]) + ".gsc"
        keys = [framework] + sorted(k for k in self.sources.text if k.endswith(".gsc") and k != framework)
        table = {}
        for key in keys:
            script = self.sources.get(key[:-4])
            if script is None:
                continue
            toks = script.tokens
            for ref in gsc.references(toks, 0, len(toks)):
                i = ref.index
                if ref.name.lower() != contract["register"] or i + 5 >= len(toks) or toks[i + 1].text != "(":
                    continue
                if toks[i + 2].kind == gsc.STRING and toks[i + 3].text == "," and toks[i + 4].kind == gsc.STRING \
                        and toks[i + 5].text == ")":
                    table[toks[i + 2].text.strip('"')] = toks[i + 4].text.strip('"')
        if not table:
            return []
        body = "".join(f'    if ( soundexists( "waw/{alias}" ) )\n'
                       f'        level.zombie_sounds["{name}"] = "waw/{alias}";\n'
                       for name, alias in table.items())
        self.level_state_parts.append(
            f"\n// {contract['script']}::init_sounds: the map's sound for each ref on BO2's sound table\n"
            "waw2bo2_sound_table()\n{\n"
            "    if ( !isdefined( level.zombie_sounds ) )\n        level.zombie_sounds = [];\n" + body + "}\n")
        self.report.rewrites["WaW sound table -> BO2 level.zombie_sounds"] += len(table)
        return [f"{CORE}::waw2bo2_sound_table();"]

    def announcer_hook(self, out_root: Path) -> list[str]:
        """BO2's power-up grab says leaderdialog( powerup_name ) through
        game["zmbdialog"]; the map's own power-up script names WaW's line for
        each (Shi No Numa: insta_kill -> insta_vox). Bound at runtime to the
        announcer copy the sound bank holds for a line WaW authored as an
        announcement (audioremaster), so BO2's announcer plays WaW's voice."""
        contract = API["audio_remaster"]["powerup_announcer"]
        lines = self.announcer_lines(contract["script"], contract["function"])
        events = out_root / "content_source/announcer_events.json"
        events.unlink(missing_ok=True)
        if not lines:
            return []
        events.parent.mkdir(parents=True, exist_ok=True)
        events.write_text(
            json.dumps(lines, indent=2) + "\n", encoding="utf-8")
        prefix = API["audio_remaster"]["announcer_alias_prefix"]
        # The map's line replaces BO2's stem: the first candidate the bank
        # holds an announcer copy of (a sound effect the case also plays,
        # such as nuke_flash, gets none).
        body = ""
        for label, aliases in sorted(lines.items()):
            for alias in aliases:
                name = prefix + re.sub(r"\W", "_", alias)
                body += (f'    if ( !isdefined( bound["{label}"] ) && soundexists( "vox_zmba_{name}" ) )\n    {{\n'
                         f'        game["zmbdialog"]["{label}"] = "{name}";\n'
                         f'        bound["{label}"] = 1;\n    }}\n')
        self.level_state_parts.append(
            f"\n// {contract['script']}::{contract['function']}: WaW's announcer line for each power-up, said "
            "by BO2's announcer\nwaw2bo2_announcer_events()\n{\n"
            '    if ( !isdefined( game["zmbdialog"] ) )\n        return;\n    bound = [];\n' + body + "}\n")
        self.report.rewrites["WaW power-up announcer lines -> BO2 announcer events"] += len(lines)
        return [f"{CORE}::waw2bo2_announcer_events();"]

    def player_damage_scale_hook(self) -> list[str]:
        """WaW's player_damageMultiplier, which the engine applies to every
        non-melee hit on a player (scripted DoDamage and radiusdamage too: stock
        _zombiemode divides a DoDamage by it to land an exact 60). _gameskill
        sets it once all players spawned: 100 / (playerDifficultyHealth at the
        "normal" starting fraction, g_gameskill 1, * coopPlayerDifficultyHealth
        for the player count). Unported, Verruckt's electric trap (50 per touch)
        hit three times as hard and downed a Juggernog player at once."""
        text = self.sources.text.get(GAMESKILL + ".gsc", "")
        if not text:
            return []   # no WaW framework skill script (synthetic sources)
        start = re.search(r'difficulty_starting_frac\s*\[\s*"normal"\s*\]\s*=\s*([\d.]+)\s*;', text)
        points = sorted((float(f), float(v)) for f, v in re.findall(
            r'add_fractional_data_point\(\s*"playerDifficultyHealth"\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*\)', text))
        coop = dict((int(i), float(v)) for i, v in re.findall(
            r'level\.difficultySettings\s*\[\s*"coopPlayerDifficultyHealth"\s*\]\s*\[\s*"normal"\s*\]\s*'
            r'\[\s*(\d)\s*\]\s*=\s*([\d.]+)\s*;', text))
        if not start or len(points) < 2 or sorted(coop) != [0, 1, 2, 3]:
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{GAMESKILL}: playerDifficultyHealth data not found; "
                                      "player damage keeps BO2's scale (1)")
            return []
        frac = float(start[1])
        health = points[-1][1]
        for (f0, v0), (f1, v1) in zip(points, points[1:]):
            if f0 <= frac <= f1:
                health = v0 + (v1 - v0) * ((frac - f0) / (f1 - f0) if f1 > f0 else 0)
                break
        scales = [100 / (health * coop[i]) for i in range(4)]
        body = "".join(f"    scales[{i}] = {s!r};\n" for i, s in enumerate(scales))
        self.level_state_parts.append(
            f"\n// maps\\_gameskill: player_damageMultiplier (normal: playerDifficultyHealth {health:g}, "
            f"coop scalars {[coop[i] for i in range(4)]})\n"
            "waw2bo2_player_damage_scale()\n{\n"
            '    common_scripts\\utility::flag_wait( "initial_players_connected" );\n'
            f"    scales = [];\n{body}"
            "    count = get_players().size;\n"
            "    if ( count < 1 )\n        count = 1;\n"
            "    if ( count > 4 )\n        count = 4;\n"
            "    level.waw2bo2_player_damage_scale = scales[count - 1];\n}\n")
        self.report.rewrites["WaW player_damageMultiplier -> scaled non-melee player damage"] += 1
        return [f"level thread {CORE}::waw2bo2_player_damage_scale();"]

    # WaW _gameskill::setSkill names -> the level variables BO2's
    # _zm_playerhealth::playerhealthregen reads (the same SP algorithm).
    REGEN_SETTINGS = {'playerHealth_RegularRegenDelay': 'playerhealth_regularregendelay',
                      'longRegenTime': 'longregentime', 'healthOverlayCutoff': 'healthoverlaycutoff',
                      'invulTime_preShield': 'invultime_preshield', 'invulTime_onShield': 'invultime_onshield',
                      'invulTime_postShield': 'invultime_postshield', 'worthyDamageRatio': 'worthydamageratio'}

    def player_health_regen_hook(self) -> list[str]:
        """The map's own player regen timing. WaW threads
        _gameskill::playerHealthRegen from _load with the values setSkill
        interpolates at the starting fraction of "normal" (g_gameskill 1).
        BO2's playerhealthregen is the same algorithm, but _zm_playerhealth::init
        sets its timing as constants; the source's values replace them."""
        text = self.sources.text.get(GAMESKILL + ".gsc", "")
        if not text:
            return []
        start = re.search(r'difficulty_starting_frac\s*\[\s*"normal"\s*\]\s*=\s*([\d.]+)\s*;', text)
        if not start:
            self.report.unsupported.setdefault(FALLBACK, []).append(
                f"{GAMESKILL}: no normal starting fraction; player regen keeps BO2's timing")
            return []
        frac = float(start[1])
        lines, missing = [], []
        for name, variable in self.REGEN_SETTINGS.items():
            points = sorted((float(f), float(v)) for f, v in re.findall(
                r'add_fractional_data_point\(\s*"' + name + r'"\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*\)', text))
            if not points:
                missing.append(name)
                continue
            value = points[-1][1] if frac >= points[-1][0] else points[0][1]
            for (f0, v0), (f1, v1) in zip(points, points[1:]):
                if f0 <= frac <= f1:
                    value = v0 + (v1 - v0) * ((frac - f0) / (f1 - f0) if f1 > f0 else 0)
                    break
            lines.append(f"    level.{variable} = {int(value) if value == int(value) else value!r};\n")
        if missing:
            self.report.unsupported.setdefault(FALLBACK, []).append(
                f"{GAMESKILL}: no setSkill data for {', '.join(missing)}; BO2's value kept for those")
        if not lines:
            return []
        self.level_state_parts.append(
            f"\n// maps\\_gameskill: player regen timing at normal (starting fraction {frac:g})\n"
            "waw2bo2_player_regen_timing()\n{\n" + "".join(lines) + "}\n")
        self.report.rewrites["WaW _gameskill regen timing -> BO2 playerhealthregen"] += 1
        return [f"{CORE}::waw2bo2_player_regen_timing();"]

    def dog_rounds_hook(self) -> str | None:
        """WaW _zombiemode::main starts dog rounds through _zombiemode_dogs::init,
        which returns unless the map set level.dogs_enabled. BO2's _zm::init does
        not start its counterpart (_zm_ai_dogs: same round counts and first dog
        round timing); the port does, guarded the same way. The source dog
        script still decides where dogs spawn (its spawn selection over the
        spawners the map opened) and their health table."""
        dogs = API['zombie_runtime']['dog_rounds']
        source, native = dogs['source_script'], dogs['native_script']
        script = self.sources.get(source)
        if script is None:
            return None
        logic = "undefined"
        spawning = script.functions.get(dogs['source_spawning'])
        if spawning is not None:
            called = {r.name.lower() for r in gsc.references(script.tokens, spawning.body_open, spawning.body_close)}
            chosen = [n for n in dogs['spawn_logic'] if n in called and self.api.defines(native, n)]
            if chosen:
                logic = f"{dogs['owned_script']}::{chosen[0]}"
            elif called & set(script.functions) - {dogs['source_spawning']}:
                self.report.unsupported.setdefault(
                    f"GSC {source}::{dogs['source_spawning']} spawn selection", []).append(
                    self.sources.origin.get(source + ".gsc", source))
        health = "undefined"
        if dogs['health'] in script.functions:
            health = f"{CORE}::{self.extract_entry(source, dogs['health'])}"
        fog = self.client_fog_monitors(dogs['client_notifies'])
        # The owned BO2 dog runtime loads its effects through waw_loadfx_native:
        # convert the source's (same paths: spawn lightning, eyes, trails, gib).
        self.report.fx += re.findall(r'\bloadfx\s*\(\s*"([^"]+)"', self.sources.text.get(source + ".gsc", ""),
                                     re.IGNORECASE)
        self.report.rewrites[f"WaW dog rounds -> {native} (source spawn selection/health)"] += 1
        guard = f"level.{dogs['guard']}"
        threads = "".join(f" level thread {CORE}::{name}();" for name in fog)
        owned = dogs['owned_script']   # BO2's runtime with the source's dog sounds
        return (f"if ( isdefined( {guard} ) && {guard} ) {{ {owned}::{dogs['native_init']}(); "
                f"{COMPAT}::waw_dog_rounds( {logic}, {health}, {owned}::enable_dog_rounds );{threads} }}")

    def client_fog_monitors(self, moments: dict) -> list[str]:
        """The source map's client scripts react to a clientnotify with volume fog
        (Shi No Numa: level waittill( "dog_start" ); SetVolFog( ... )). T6 client
        scripts have no volumetric fog, and the server never sees a clientnotify,
        so each monitor runs on the server at the moments the native framework
        sends that notify (``moments``: msg -> [[server notify, delay]])."""
        made = []
        for key in sorted(k for k in self.sources.text if k.endswith(".csc")):
            script = self.sources.get(key[:-4], ".csc")
            if script is None:
                continue
            toks = script.tokens
            for fn in script.functions.values():
                for i in range(fn.body_open, fn.body_close - 4):
                    if not (toks[i].low == "level" and toks[i + 1].low == "waittill" and toks[i + 2].text == "("
                            and toks[i + 3].kind == gsc.STRING):
                        continue
                    msg = toks[i + 3].text.strip('"')
                    if msg not in moments:
                        continue
                    close = gsc._match(toks, i + 2, "(", ")")
                    block_end = next((j for j in range(close, fn.body_close) if toks[j].text == "}"), fn.body_close)
                    fogs = []
                    for ref in gsc.references(toks, close, block_end):
                        if ref.name.lower() == "setvolfog" and not ref.method:
                            end = gsc._match(toks, ref.index + 1, "(", ")")
                            if all(t.kind == gsc.NUMBER or t.text in (",", "-", ".") for t in toks[ref.index + 2:end]):
                                fogs.append(gsc.emit(toks[ref.index + 1:end + 1]).strip())
                        elif ref.qualifier is None and not ref.method:
                            self.report.unsupported.setdefault(
                                f"CSC {ref.name} in a {msg} monitor (client-only)", []).append(key)
                    if not fogs:
                        continue
                    for n, (notify, delay) in enumerate(moments[msg]):
                        name = f"waw2bo2_client_{msg}_{n}"
                        if any(part.startswith(name + "()") for part in self.level_state_parts):
                            continue
                        body = "".join(f"\t\t{COMPAT}::waw_setvolfog{args};\n" for args in fogs)
                        wait = f"\t\twait {delay};\n" if delay else ""
                        self.level_state_parts.append(
                            f"{name}()\n{{\n\t// {key}::{fn.name}: client {msg} fog, on the server\n"
                            f"\tlevel endon( \"end_game\" );\n\tfor ( ;; )\n\t{{\n"
                            f"\t\tlevel waittill( \"{notify}\" );\n{wait}{body}\t}}\n}}\n")
                        made.append(name)
                        self.report.rewrites[f"client {msg} fog monitor -> server"] += 1
        return made

    def framework_zombie_vars(self) -> str | None:
        """WaW framework zombie_vars defaults for BO2's init_levelvars.

        BO2's _zm replaces _zombiemode::init_levelvars (where WaW sets its
        zombie_vars) and starts from its own values (zombie_spawn_delay 2.0
        instead of WaW's 3: zombies spawned 50% faster). The owned _zm asks
        waw2bo2_set_zombie_var for every variable it sets; the map's own WaW
        mp/zombiemode.csv still wins. Only literal values."""
        script = self.sources.get(ZOMBIEMODE)
        functions = script.functions if script is not None else {}
        fn = functions.get("init_levelvars") or functions.get("init_zombie_vars")
        if fn is None:
            return None
        toks, values, divisors = script.tokens, {}, {}
        dev = False
        for i in range(fn.body_open, fn.body_close - 5):
            # /# ... #/ is developer-only code a release build drops
            # (Der Riese: zombie_score_start 100000 under zombie_cheat)
            if toks[i].text in ("/#", "#/"):
                dev = toks[i].text == "/#"
            if dev:
                continue
            if toks[i].low == "set_zombie_var" and toks[i + 1].text == "(" and toks[i + 2].kind == gsc.STRING \
                    and toks[i + 3].text == ",":
                j, sign = i + 4, ""
                if toks[j].text == "-":
                    sign, j = "-", j + 1
                if (toks[j].kind == gsc.NUMBER or toks[j].low in ("true", "false")) and toks[j + 1].text in (",", ")"):
                    values[toks[i + 2].text[1:-1]] = sign + toks[j].text
                    # WaW set_zombie_var( var, value, div, is_float ) divides by div
                    # after any table override, so the divisor is kept apart.
                    if toks[j + 1].text == "," and toks[j + 2].kind == gsc.NUMBER \
                            and toks[j + 3].text in (",", ")"):
                        divisors[toks[i + 2].text[1:-1]] = toks[j + 2].text
        if not values:
            return None
        name = "zombiemode__init_zombie_vars_defaults"
        self.level_state_parts.append(
            f"\n// {ZOMBIEMODE}::init_zombie_vars: WaW defaults for BO2's init_zombie_vars\n{name}()\n{{\n"
            "\tlevel.waw2bo2_zombie_var_defaults = [];\n" +
            "".join(f'\tlevel.waw2bo2_zombie_var_defaults["{k}"] = {v};\n' for k, v in sorted(values.items())) +
            "\tlevel.waw2bo2_zombie_var_divisors = [];\n" +
            "".join(f'\tlevel.waw2bo2_zombie_var_divisors["{k}"] = {v};\n' for k, v in sorted(divisors.items()))
            + "}\n")
        self.report.rewrites["WaW framework zombie_vars defaults"] += len(values)
        return name

    def client_fx_listeners(self) -> list[str]:
        """Client listeners of the map's client scripts, run on the server.

        WaW maps answer a server clientnotify in their client scripts: Shi No
        Numa's trap_fx_monitor spawns electric arcs on its structs, Der Riese
        swaps its lamp models at power-on, lights its teleporter maps, runs
        sparks along the pad wires and floats the fly-trap objects. The port
        does not run the source client scripts, so a listener main() threads
        becomes a server thread when every call it makes is one gsc_api
        client_listener_calls defines (the local client argument dropped,
        client-only builtins routed to the compat runtime). getlocalplayers
        yields one entry, so per-client work happens once for everyone, on
        networked server entities. The client's level messages live in the
        "waw_client_" namespace: compat waw_clientnotify raises
        "waw_client_" + msg, and the copies' own level notify/endon/waittill
        use the same prefix. Client effects are registered on the server."""
        made: list[str] = []
        effects: dict[str, str] = {}
        csc = sorted(k for k in self.sources.text if k.endswith(".csc"))
        for key in csc:
            # level._effect is one client namespace across the client scripts
            script = self.sources.get(key[:-4], ".csc")
            toks = script.tokens if script is not None else []
            for i in range(len(toks) - 9):
                if [t.low for t in toks[i:i + 4]] == ["level", ".", "_effect", "["] and toks[i + 4].kind == gsc.STRING \
                        and toks[i + 5].text == "]" and toks[i + 6].text == "=" and toks[i + 7].low == "loadfx" \
                        and toks[i + 9].kind == gsc.STRING:
                    effects.setdefault(toks[i + 4].text[1:-1].lower(), toks[i + 9].text)
        for key in csc:
            script = self.sources.get(key[:-4], ".csc")
            main = script.functions.get("main") if script is not None else None
            if main is None:
                continue
            toks = script.tokens
            for ref in gsc.references(toks, main.body_open, main.body_close):
                if ref.qualifier is not None or ref.pointer or ref.method or ref.name.lower() not in script.functions:
                    continue
                if not (toks[ref.index - 1].low == "thread" and toks[ref.index - 2].low == "level"):
                    continue
                listener = self.client_fx_listener(key, script, ref.name.lower(), effects)
                if listener is None:
                    continue
                end = gsc._match(toks, ref.index + 1, "(", ")")
                args = gsc.emit(toks[ref.index + 1:end + 1]).strip()
                name = f"waw2bo2_client_fx_{len(made)}"
                self.level_state_parts.append(
                    f"{name}()\n{{\n\t// {key}::main: client listener, on the server\n"
                    f"{listener['effects']}\tlevel thread {listener['entry']}{args};\n}}\n")
                made.append(name)
                self.report.rewrites["client effect listeners -> server"] += 1
                literals = {t.text[1:-1] for t in gsc.tokenize(listener["code"] + args) if t.kind == gsc.STRING}
                self.report.client_listener_strings = sorted(set(self.report.client_listener_strings) | literals)
        return made

    def client_fx_listener(self, key: str, script, entry: str, effects: dict[str, str]) -> dict | None:
        """Server copies of ``entry`` and its local callees, or None if any
        call has no server form (that listener stays unported and reported).
        Copies are emitted once however many threads main() starts."""
        cache = self.__dict__.setdefault("_client_listener_copies", {})
        if (key, entry) not in cache:
            cache[(key, entry)] = self._client_fx_listener_copy(key, script, entry, effects)
        return cache[(key, entry)]

    @staticmethod
    def _drop_argument(toks: list[gsc.Token], name_index: int, index: int) -> None:
        """Blank argument ``index`` (and one adjacent comma) of the call at ``name_index``."""
        close = gsc._match(toks, name_index + 1, "(", ")")
        depth, commas = 0, []
        for k in range(name_index + 2, close):
            if toks[k].text == "," and depth == 0:
                commas.append(k)
            depth += toks[k].text in ("(", "[")
            depth -= toks[k].text in (")", "]")
        bounds = [name_index + 1, *commas, close]
        if index >= len(bounds) - 1:
            return
        if index + 1 < len(bounds) - 1:
            first, last = bounds[index] + 1, bounds[index + 1]          # the argument and its following comma
        else:
            first, last = (bounds[index] if index else bounds[index] + 1), bounds[index + 1] - 1
        for k in range(first, last + 1):
            toks[k].text = toks[k].pre = ""

    def _client_fx_listener_copy(self, key: str, script, entry: str, effects: dict[str, str]) -> dict | None:
        calls = API["client_listener_calls"]
        names, pending = [], [entry]
        while pending:
            name = pending.pop()
            if name in names:
                continue
            names.append(name)
            fn = script.functions[name]
            if any(t.text == "[[" for t in script.tokens[fn.body_open:fn.body_close]):
                # a function pointer's target is not a call the table can vouch for
                self.report.unsupported.setdefault(
                    f"CSC function pointer call in client listener {entry} (client-only)", []).append(key)
                return None
            for ref in gsc.references(script.tokens, fn.body_open, fn.body_close):
                low = ref.name.lower()
                rule = calls.get(low) if ref.qualifier is None else None
                if ref.qualifier is None and low in script.functions:
                    pending.append(low)
                elif rule is not None and rule.get("skip"):
                    return None     # another translation owns this listener (client_fog_monitors)
                elif rule is None or (ref.method and "to" in rule):
                    self.report.unsupported.setdefault(
                        f"CSC {ref.name} in client listener {entry} (client-only)", []).append(key)
                    return None
        waits = any(script.tokens[i].low == "level" and script.tokens[i + 1].low == "waittill"
                    for n in names for i in range(script.functions[n].body_open, script.functions[n].body_close))
        if not waits:
            return None
        prefix = f"waw2bo2_{key.split(chr(92))[-1].removesuffix('.csc')}__"
        parts, used = [], set()
        # listeners share helpers (Der Riese's map lights: get_guide_struct_angles):
        # each source function is emitted once per client script
        emitted = self.__dict__.setdefault("_client_listener_functions", set())
        for name in names:
            fn = script.functions[name]
            if (key, name) in emitted:
                continue
            toks = [gsc.Token(t.kind, t.text, t.pre, t.line) for t in script.tokens[fn.start:fn.body_close + 1]]
            i = 0
            while i < len(toks) - 1:
                t = toks[i]
                low = t.low
                call = t.kind == gsc.IDENT and toks[i + 1].text == "(" and (i == 0 or toks[i - 1].text != "::")
                rule = calls.get(low) if call and i else None
                if call and i and low in names:
                    t.text = prefix + low
                elif rule is not None:
                    if "client_arg" in rule:
                        self._drop_argument(toks, i, rule["client_arg"])
                    if low == "spawnfx":
                        # server spawnfx( fx, origin, forward, up ): no delay argument
                        self._drop_argument(toks, i, 2)
                    if "to" in rule:
                        t.text = f"{COMPAT}::{rule['to']}" if rule["to"].startswith("waw_") else rule["to"]
                elif low == "level" and toks[i + 1].text == "." and toks[i + 2].low == "_effect" \
                        and toks[i + 3].text == "[" and toks[i + 4].kind == gsc.STRING:
                    used.add(toks[i + 4].text[1:-1].lower())
                    toks[i + 2].text = "waw_effect"
                elif low == "level" and toks[i + 1].low in ("waittill", "notify", "endon") and toks[i + 2].text == "(":
                    toks[i + 3].pre = " \"waw_client_\" + " + toks[i + 3].pre.lstrip()
                i += 1
            if toks[0].low in names:
                toks[0].text = prefix + toks[0].low
            parts.append((name, gsc.emit(toks)))
        missing = sorted(used - effects.keys())
        if missing:
            self.report.unsupported.setdefault(
                f"CSC client effects {', '.join(missing)} in listener {entry} (no client registration)", []).append(key)
            return None
        done = {name for name, _ in parts}
        emitted.update((key, name) for name in done)
        parts = [text for _, text in parts]
        code = "\n".join(parts)
        self.level_state_parts.append(f"\n// {key}: client listener {entry} (server copy)\n" + code + "\n")
        registrations = "".join(
            f'\tif ( !isdefined( level.waw_effect["{k}"] ) )\n'
            f'\t\tlevel.waw_effect["{k}"] = {COMPAT}::waw_loadfx( {effects[k]} );\n' for k in sorted(used))
        # helpers an earlier listener emitted belong to this one's code too
        code += "".join(gsc.emit(script.tokens[script.functions[n].start:script.functions[n].body_close + 1])
                        for n in names if n not in done)
        return {"entry": prefix + entry, "effects": registrations, "code": code}

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
            if not script_branch_taken(zm.tokens, fn.body_open, fn.body_close, ref.index,
                                       getattr(self, "map_name", None)):
                continue    # e.g. _zombiemode_weapons_sumpf::init outside Shi No Numa
            target = norm(ref.qualifier)
            tesla = API['tesla_runtime']
            if target == tesla['source_script'] and ref.name.lower() == tesla['initialize'] and \
                    (target, ref.name.lower()) in self.core_funcs:
                # The damage adapter calls the extracted initializer, including
                # a map override. Do not start its connection threads twice.
                continue
            if self.bo2_stock_perks and stockperks.is_perk_framework(target) and \
                    ref.name.lower() not in self.waw_perk_parts.get(target, ()):
                continue
            dogs = API['zombie_runtime']['dog_rounds']
            # WaW drives the dog rounds: the source _zombiemode_dogs::init is
            # ported like any framework hook (the source tracker, spawning,
            # health, aftermath); T6 actor setup comes from actor_glue.
            if target == norm(dogs['source_script']) and ref.name.lower() == dogs['source_init'] \
                    and not dogs.get('source_driven'):
                call = self.dog_rounds_hook()
                if call and call not in calls:
                    calls.append(call)
                continue
            if target == norm(dogs['source_script']) and ref.name.lower() == dogs['source_init']:
                # the source tracker sends the moments the client fog monitors wait for
                for name in self.client_fog_monitors(dogs['client_notifies']):
                    calls.append(f"level thread {CORE}::{name}();")
                self.report.rewrites["WaW drives the dog rounds: source _zombiemode_dogs"] += 1
            # A framework script whose map functions run where BO2's take other
            # arguments (signature_source_scripts) keeps the map's init when
            # BO2's counterpart has none: Shi No Numa's _zombiemode_net::init
            # sets the spawn counters its network_safe_spawn reads.
            mapped = API["core_scripts"].get(target)
            if target in API.get("signature_source_scripts", {}) and mapped \
                    and not self.api.defines(mapped, ref.name.lower()) and not _argc(zm.tokens, ref.index):
                extracted = self.extract(target, ref.name.lower())
                if extracted and f"{extracted}();" not in calls:
                    calls.append(f"{extracted}();")
                    self.report.rewrites[f"framework hook {target}::{ref.name} -> source (BO2 has none)"] += 1
                continue
            if is_core(target, self.sources):
                continue
            dep = self.sources.get(target)
            hook = dep.functions.get(ref.name.lower()) if dep is not None else None
            prefix = ''
            previous = ref.qual_index - 1
            if zm.tokens[previous].low == 'thread':
                prefix = 'thread '
                previous -= 1
            if zm.tokens[previous].low in ('level', 'self'):
                prefix = zm.tokens[previous].text + ' ' + prefix
            call = f"{prefix}{ported_path(target)}::{ref.name}();"
            if hook is None or call in calls:
                continue
            if hook.params or _argc(zm.tokens, ref.index):
                self.report.unsupported.setdefault(FALLBACK, []).append(f"{where_file}:{zm.tokens[ref.index].line}: framework hook "
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
        if self.post_calls:
            hooks += ("\n\t// the map's _zombiemode overrides BO2's _zm::init replaced" +
                      "".join(f"\n\t{call}" for call in self.post_calls))
        tokens[fn.start].text = "waw_main_pre"
        weapons = self.sources.get(WEAPONS_FRAMEWORK)
        if weapons is not None and "include_zombie_weapon" in weapons.functions:
            # before the map's include_weapons(): compat waw_include_weapon
            # fills the map's own registry through it as well as BO2's
            self.want(WEAPONS_FRAMEWORK)
            self.pre_calls.insert(0, f"level.waw_include_zombie_weapon = "
                                     f"{ported_path(WEAPONS_FRAMEWORK)}::include_zombie_weapon;")
        score = self.sources.get(SCORE_FRAMEWORK)
        if score is not None and "player_add_points" in score.functions:
            # compat waw_player_add_points: the native AI's kill and hit awards
            # go to the map's own scorer
            self.want(SCORE_FRAMEWORK)
            self.pre_calls.insert(0, f"level.waw_player_add_points = "
                                     f"{ported_path(SCORE_FRAMEWORK)}::player_add_points;")
        connect = self.extract_player_connect_fields()
        if connect:
            self.pre_calls.insert(0, f"level thread {CORE}::{connect}();")
        pre = "".join(f"\n\t{call}" for call in self.pre_calls)
        if pre:
            pre = "\n\t// the map's functions in place of BO2's, before _zm::init calls them" + pre
        tokens[fn.body_open].text = ("{\n\t" + f"{COMPAT}::init();\n\t{CORE}::framework_level_state();" + pre +
                                     f"\n\t{PRECACHE}::init();\n\t{ONEWAY}::init();\n\t{WATER_BULLETS}::init();")
        split = None
        for ref in gsc.references(tokens, fn.body_open, fn.body_close):
            if ref.qualifier is not None and norm(ref.qualifier) == "maps\\_zombiemode" and ref.name.lower() == "main":
                split = ref
                break
        if split is None:
            tokens[fn.body_close].text = "}\n\nwaw_main_post()\n{" + hooks + "\n}"
            self.report.unsupported.setdefault(FALLBACK, []).append(f"{script.path}: no maps\\_zombiemode::main() call; whole main runs before _zm::init")
            return
        i = split.qual_index
        end = i
        while tokens[end].text != ";":
            end += 1
        # the map's spawner lists first, with the argument it gives _zombiemode::main
        lists = self.extract_spawner_lists()
        if lists:
            close = gsc._match(tokens, split.index + 1, "(", ")")
            args = gsc.emit(tokens[split.index + 2:close]).strip()
            hooks = f"\n\t{CORE}::{lists}( {args} );" + hooks
        # WaW's _zombiemode::main() returns only after flag_wait( "all_players_connected" ),
        # so the rest of a WaW map main runs with every player present (maps loop
        # over getPlayers() there). BO2's counterpart flag is set by _zm.
        tokens[i].text = ("}\n\nwaw_main_post()\n{" + hooks +
                          "\n\tcommon_scripts\\utility::flag_wait( \"initial_players_connected\" );"
                          "\t// WaW _zombiemode::main waits for all players\n")
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
                      f'    {COMPAT}::report( "GSC", "{desc.replace(chr(92), chr(92) * 2)}" );']
            lines += ["}", ""]
        return "\n".join(lines)


def discover_animation_dependencies(sources: Sources, api: T6Api, map_name: str,
                                    actor_types: set[str] = frozenset()) -> dict[str, set[str]]:
    """Run the normal dependency resolver with animation extraction enabled.

    Discovery uses a private source copy because translation edits tokens.
    No files are emitted, and no source function name needs a special list.
    """
    sources = copy.deepcopy(sources)
    sources.bind_zombie_framework(f'maps/{map_name}')
    for name in sorted(actor_types):
        path = 'aitype\\' + name
        original = sources.text.get(path + '.gsc', sources.stock.get(path + '.gsc'))
        if original is not None:
            sources.text[path + '.gsc'] = zombieappearance.appearance_script(original)
            sources._parsed.pop(path + '.gsc', None)
    tr = Translator(sources, api, PortReport())
    tr.map_name = map_name
    tr.animation_discovery = True
    tr.queue.append(norm(f"maps/{map_name}"))
    tr.extract_custom_intermission()
    tr.extract_end_game()
    tr.extract_entry(ZOMBIEMODE, "init_strings")
    speed_definition = API['zombie_runtime']['source_speed']
    tr.extract_entry(speed_definition['script'], speed_definition['function'])
    for name in sorted(actor_types):
        tr.want('aitype\\' + name)
    if has_waw_appearance(sources):
        tr.want(LOADOUT)
    for entries in WEAPON_REGISTRATION.values():
        for core, name in entries:
            tr.extract_entry(core, name)
    parts = []

    def drain():
        while tr.queue or tr.core_queue:
            if tr.core_queue:
                parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
            else:
                path = tr.queue.pop(0)
                if path not in tr.done:
                    tr.done[path] = tr.translate(path, main_split=(path == norm(f"maps/{map_name}")))

    drain()
    read = set().union(*(level_fields_read(gsc.tokenize(text)) for text in [*tr.done.values(), *parts]))
    for core, name in [BOX_STATE_INIT, *FRAMEWORK_STATE_INITS]:
        tr.extract_level_state(core, name, read)
    drain()
    return {tree: names for tree, names in tr.animation_requirements.items() if names}


def port_map(sources: Sources, api: T6Api, map_name: str, out_root: Path,
             fx_table: dict[str, str] | None = None, animtrees: set[str] | None = None,
             core_animtrees: dict[str, set[str]] | None = None, bo2_stock_perks: bool = False,
             actor_types: set[str] = frozenset(), actor_template: str | None = None,
             stock_map: bool = False) -> PortReport:
    """Translate the map main and everything it reaches into out_root/maps/mp/waw.
    ``animtrees``: animtree names BO2 can load (stock + converted).
    ``core_animtrees``: WaW framework-override animtrees staged for BO2 -> their
    animations with WaW xanims (t6bridge.stage_core_animtrees).
    ``stock_map``: the source is a retail WaW level (its fastfile is in the WaW
    install's zone folder); gsc_api.json stock_map_policies apply to it only.
    Custom maps keep their own logic."""
    sources = copy.copy(sources)
    framework_aliases = sources.bind_zombie_framework(f'maps/{map_name}')
    actor_kinds: dict[str, str] = {}
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
            # the source engine AI type (self.type) picks the native T6 actor family
            actor_kinds[name] = zombieappearance.source_actor_kind(original)
            sources.text[path + '.gsc'] = zombieappearance.appearance_script(original)
            sources._parsed.pop(path + '.gsc', None)
    if bo2_stock_perks:
        sources = copy.copy(sources)
        sources.text = {key: stockperks.translate_literals(text) for key, text in sources.text.items()}
        sources.stock = {key: stockperks.translate_literals(text) for key, text in sources.stock.items()}
        sources._parsed = {}
    waw_perk_parts: dict[str, set[str]] = {}
    if bo2_stock_perks:
        for key in sorted({*sources.text, *sources.stock}):
            if not key.endswith(".gsc") or not stockperks.is_perk_framework(norm(key)):
                continue
            kept = waw_pack_a_punch_source(sources.text.get(key, sources.stock.get(key)), norm(key))
            if kept is not None:
                sources.text[key], waw_perk_parts[norm(key)] = kept
    report = PortReport(map_main=f"maps\\{map_name}")
    for alias, canonical in framework_aliases.items():
        report.rewrites[f'framework startup {alias} -> {canonical}'] += 1
    tr = Translator(sources, api, report)
    tr.map_name = map_name
    tr.bo2_stock_perks = bo2_stock_perks
    tr.stock_map = stock_map
    tr.waw_perk_parts = waw_perk_parts
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
    # before the main is translated: waw_main_post makes these calls
    tr.extract_custom_intermission()
    tr.extract_end_game()
    tr.post_calls += tr.juggernog_hooks()
    tr.post_calls += tr.player_damage_scale_hook()
    tr.post_calls += tr.player_health_regen_hook()
    tr.post_calls += tr.announcer_hook(out_root)
    tr.post_calls += tr.powerup_models_hook()
    tr.post_calls += tr.sound_table_hook()
    tr.post_calls += tr.zombie_vocals_hook()
    tr.post_calls += tr.quote_hooks()
    tr.pre_calls += tr.native_replacements()
    for name in sorted(actor_types):
        tr.want('aitype\\' + name)
    hint_registry = tr.extract_entry(r"maps\_zombiemode", "init_strings")
    speed_definition = API['zombie_runtime']['source_speed']
    speed_script = sources.get(speed_definition['script'])
    speed_entry = speed_script.functions.get(speed_definition['function']) if speed_script else None
    speed_function = None
    if speed_entry is not None:
        if len(speed_entry.params) != speed_definition['arguments']:
            report.errors.append(f"{speed_definition['script']}::{speed_definition['function']}: "
                                 "source speed selector must accept zero arguments")
        else:
            speed_function = tr.extract_entry(speed_definition['script'], speed_definition['function'])
            report.rewrites['source WaW zombie speed selector -> native spawn callback'] += 1
    tr.post_calls.insert(0, zombiebehavior.RUNTIME + '::init();')
    speed_state_function = None
    speed_state_source = zombiebehavior.source_speed_state(sources, API['zombie_runtime'])
    if speed_state_source:
        sources.text = dict(sources.text)
        sources._parsed = dict(sources._parsed)
        sources.text[zombiebehavior.SPEED_STATE + '.gsc'] = speed_state_source
        sources.origin[zombiebehavior.SPEED_STATE + '.gsc'] = 'source speed initialization and round inputs'
        sources._parsed.pop(zombiebehavior.SPEED_STATE + '.gsc', None)
        tr.want(zombiebehavior.SPEED_STATE)
        speed_state_function = ported_path(zombiebehavior.SPEED_STATE)
        report.rewrites['source WaW speed round inputs -> shared runtime'] += 1
    last_zombie = zombiebehavior.source_last_zombie(sources, API['zombie_runtime'])
    report.rewrites['source last-zombie speed rule -> native zombie_speed_up' if last_zombie
                    else 'no source last-zombie speed rule: native zombie_speed_up disabled'] += 1
    selection_source = zombiebehavior.source_selection(sources, API['zombie_runtime'])
    chase_source = zombiebehavior.source_chase_policy(sources, API['zombie_runtime'])
    chase_function = None
    if chase_source:
        sources.text = dict(sources.text)
        sources._parsed = dict(sources._parsed)
        sources.origin = dict(sources.origin)
        sources.text[zombiebehavior.CHASE_POLICY + '.gsc'] = chase_source
        sources.origin[zombiebehavior.CHASE_POLICY + '.gsc'] = 'source zombie chase/entrance policy'
        sources._parsed.pop(zombiebehavior.CHASE_POLICY + '.gsc', None)
        tr.want(zombiebehavior.CHASE_POLICY)
        chase_function = ported_path(zombiebehavior.CHASE_POLICY) + '::should_chase'
        report.rewrites['source zombie chase/entrance policy -> native AI'] += 1
    selection_function = None
    if selection_source:
        sources.text = dict(sources.text)
        sources._parsed = dict(sources._parsed)
        sources.origin = dict(sources.origin)
        sources.text[zombiebehavior.SELECTION + '.gsc'] = selection_source
        selection_contract = API['zombie_runtime']['source_selection']
        sources.origin[zombiebehavior.SELECTION + '.gsc'] = (
            sources.origin.get(selection_contract['script'] + '.gsc', selection_contract['script']) +
            '::' + selection_contract['function'] + ' (selection statements)')
        sources._parsed.pop(zombiebehavior.SELECTION + '.gsc', None)
        tr.want(zombiebehavior.SELECTION)
        selection_function = ported_path(zombiebehavior.SELECTION) + '::choose'
        report.rewrites['source WaW spawner selection -> native round spawning'] += 1
    registration = {role: [tr.extract_entry(core, fn) for core, fn in entries if sources.get(core) is not None
                           and fn in sources.get(core).functions]
                    for role, entries in WEAPON_REGISTRATION.items()}
    # The native zombie framework replaces WaW's damage dispatch, not its
    # scripted wonder-weapon behavior. Retain the source chain and bind it
    # through T6's callback shared by hip-fire and ADS damage.
    tesla_definition = API['tesla_runtime']
    tesla = tesla_definition['source_script']
    tesla_functions = [tesla_definition[key] for key in ('initialize', 'damage_predicate', 'damage_entry')]
    tesla_script = sources.get(tesla)
    tesla_entries = {}
    if tesla_script and all(n in tesla_script.functions for n in tesla_functions):
        tesla_entries = {key: tr.extract_entry(tesla, tesla_definition[key]) for key in
                         ('initialize', 'damage_predicate', 'damage_entry')}
        tr.post_calls.append(f'{CORE}::waw_tesla_init();')
        report.rewrites['source Tesla chain -> native zombie damage callback'] += 1
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
    if tesla_entries:
        core_parts.append(tesla_callback_source(tesla_entries))
    # level state the extracted box code reads (set in the framework's init)
    if registration.get("box"):
        state = tr.extract_level_state(*BOX_STATE_INIT)
        if state:
            registration["box_state"] = [state]
            drain()
    # WaW framework tables BO2 never builds that the ported scripts read
    # (e.g. level._zombie_tesla_death for a custom Electric Cherry)
    read = set().union(*(level_fields_read(gsc.tokenize(text)) for text in [*tr.done.values(), *core_parts]))
    framework_state = [state for core, init in FRAMEWORK_STATE_INITS
                       if (state := tr.extract_level_state(core, init, read))]
    # Source zone managers can own the active actor list instead of DLC3's
    # adjacency graph. Retain its initializer only when ported gameplay reads
    # that list and the round selector can be translated exactly.
    selection_contract = API['zombie_runtime']['source_selection']
    membership_field = selection_contract['array_field']
    source_membership = False
    if selection_function and membership_field in read:
        state = tr.extract_level_state(selection_contract['script'],
                                       API['zombie_runtime']['source_membership']['initializer'], {membership_field})
        if state:
            framework_state.append(state)
            source_membership = True
            report.rewrites['source active spawner membership -> shared runtime'] += 1
    respawn_contract = API['zombie_runtime']['source_respawn']
    respawn_script = sources.get(respawn_contract['script'])
    respawn_function = None
    if respawn_script and respawn_contract['function'] in respawn_script.functions:
        respawn_function = tr.extract_entry(respawn_contract['script'], respawn_contract['function'])
    if hint_registry:
        framework_state.append(hint_registry)
    for core, function in DAMAGE_PRELUDES:
        damage_prelude = tr.extract_damage_prelude(core, function)
        if damage_prelude:
            framework_state.append(damage_prelude)
    round_total = tr.extract_round_total()
    if round_total:
        framework_state.append(round_total)
    # WaW drives the rounds: the map's own round loop (its display included)
    round_loop = tr.extract_round_loop()
    if round_loop:
        framework_state.append(round_loop)
    round_health = tr.extract_round_health(source_loop=bool(round_loop))
    if round_health:
        framework_state.append(round_health)
    if round_loop or stock_map:
        # the source display's HUD state and materials (the source loop calls
        # its display itself; BO2's loop calls it through the counter hook)
        counter = tr.extract_round_counter(source_loop=bool(round_loop))
        if counter:
            framework_state.append(counter)
    if stock_map:
        policy = stock_map_policy(API["stock_map_policies"], map_name.lower())
        core_parts.append(policy)
        framework_state.append("waw2bo2_stock_map_policies")
        report.rewrites["stock WaW map policies (solo Quick Revive)"] += 1
    # Effects of WaW's framework FX init (BO2 replaces _zombiemode): the box
    # glow, eye glow, window chunks... registered for the keys ported scripts
    # read and never assign.
    texts = [*tr.done.values(), *core_parts]
    effect_reads = {k.lower() for t in texts for k in re.findall(r'level\.waw_effect\[\s*"([^"]+)"\s*\]', t)}
    effect_sets = {k.lower() for t in texts
                   for k in re.findall(r'level\.waw_effect\[\s*"([^"]+)"\s*\]\s*=[^=]', t)}
    framework_state += tr.client_fx_listeners()
    zombie_vars = tr.framework_zombie_vars()
    if zombie_vars:
        framework_state.append(zombie_vars)
    missing_effects = effect_reads - effect_sets
    if missing_effects:
        for core, init in FRAMEWORK_EFFECT_INITS:
            state = tr.extract_level_state(core, init, {"_effect"}, missing_effects)
            if state:
                framework_state.append(state)
                report.rewrites["framework effects registered for ported reads"] += 1
    drain()
    core_parts += tr.level_state_parts
    # effects loaded by extracted framework/template functions (e.g. the DLC3
    # template's dlc3_fx: the mystery box's sky marker) are converted too
    report.fx += re.findall(r'waw_loadfx\s*\(\s*"([^"]+)"', "\n".join(core_parts), re.IGNORECASE)
    core_parts.append("\n// level state of WaW framework inits BO2 does not run (called by waw_main_pre)\n"
                      "framework_level_state()\n{\n" + "".join(f"\t{fn}();\n" for fn in framework_state) + "}\n")
    (out_dir / "_waw2bo2_compat.gsc").write_text((COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"),
                                                 encoding="utf-8")
    from . import hintdefaults
    (out_dir / '_waw2bo2_hint_defaults.gsc').write_text(hintdefaults.source(out_root), encoding='utf-8')
    core_head = ["// waw2bo2: WaW framework functions BO2 has no counterpart for, extracted from the map's",
                 "// framework scripts (its overrides, else stock WaW) and translated.",
                 *[f"#include {inc};" for inc in CORE_INCLUDES], ""]
    (out_dir / "_waw2bo2_core.gsc").write_text("\n".join(core_head) + "\n".join(core_parts) + "\n",
                                               encoding="utf-8")
    (out_dir / "_waw2bo2_stubs.gsc").write_text(tr.stub_source(), encoding="utf-8")
    # filled by materialisolation once HUD materials are converted
    (out_dir / "_waw2bo2_hud_materials.gsc").write_text(hud_material_table_source({}), encoding="utf-8")
    (out_dir / "_waw2bo2_assets.gsc").write_text(assets_source(fx_table or {}, map_name), encoding="utf-8")
    # filled with the linked mod.ff's weapons by build-mod
    (out_dir / BO2_WEAPONS_SCRIPT).write_text(bo2_weapons_source(), encoding="utf-8")
    (out_dir / "_waw2bo2_visions.gsc").write_text('init()\n{\n    level.waw2bo2_visions = [];\n}\n', encoding="utf-8")
    (out_dir / "_waw2bo2_precache.gsc").write_text(precache_source([]), encoding="utf-8")
    # one-way clip sheets come from the collision (t6bridge.port_scripts)
    (out_dir / "_waw2bo2_oneway.gsc").write_text(oneway.oneway_source([]), encoding="utf-8")
    # filled from the water brushes and weapon runtime by t6bridge.stage_bridge
    (out_dir / "_waw2bo2_water_bullets.gsc").write_text(waterbullets.water_bullets_source({}), encoding="utf-8")
    if appearance:
        (out_dir / "_waw2bo2_characters.gsc").write_text(characters_source(), encoding="utf-8")
    # WaW's bowie is the engine perk specialty_altmelee; T6 melees with a
    # melee weapon, so compat equips BO2's (waw_altmelee_weapon). It must be
    # included (precached) or giveweapon silently fails and the player keeps no knife.
    melee = [weapon for perk, weapon in ALTMELEE_WEAPONS.items()
             if any(perk in text for text in sources.text.values())]
    for weapon in melee:
        tr.report.rewrites[f"WaW altmelee perk -> BO2 {weapon} (included, not in the box)"] += 1
    (out_dir / "_waw2bo2_weapons.gsc").write_text(weapons_source(registration, melee), encoding="utf-8")
    (out_dir / '_waw2bo2_zombie_behavior.gsc').write_text(
        zombiebehavior.runtime_source(API['zombie_runtime'], f'{CORE}::{speed_function}' if speed_function else None,
                                      selection_function, speed_state_function, source_membership,
                                      f'{CORE}::{respawn_function}' if respawn_function else None, chase_function,
                                      last_zombie),
        encoding='utf-8')
    report.weapon_registration = {role: [f for f in fns if f] for role, fns in registration.items()}
    report.characters = appearance
    report.fx = sorted(set(report.fx))
    templates = actor_template if isinstance(actor_template, dict) else {"human": actor_template}
    for name in sorted(actor_types):
        kind = actor_kinds.get(name, "human")
        template = templates.get(kind) or templates.get("human")
        if template is None:
            raise ValueError('T6 actor template missing')
        if kind not in templates:
            report.errors.append(f"aitype\\{name}: no native T6 actor family for WaW AI type '{kind}'; "
                                 f"built on the human zombie actor")
        target = out_root / 'aitype' / (zombieappearance.actor_name(name) + '.gsc')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(zombieappearance.native_actor(template, ported_path('aitype\\' + name)),
                          encoding='utf-8')
        client = out_root / 'aitype/clientscripts' / (zombieappearance.actor_name(name) + '.csc')
        client.parent.mkdir(parents=True, exist_ok=True)
        tree = zombieappearance.template_tree(template)
        client.write_text(zombieappearance.native_client_actor(name, tree), encoding='utf-8')
        if kind in templates and kind != "human":
            report.native_actor_trees[tree] = kind
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
                  weapons: dict[str, str] | None = None, bolted_loops: set[str] = frozenset(),
                  zombie_var_overrides: dict[str, str] | None = None,
                  tables: dict[str, str] | None = None) -> str:
    lines = ["// waw2bo2: WaW asset name -> BO2 asset name for converted assets (generated).", "init()", "{"]
    # the map's own string tables, read by compat waw_tablelookup
    lines.append('    level.waw2bo2_tables = [];')
    for waw, staged in sorted((tables or {}).items()):
        lines.append(f'    level.waw2bo2_tables[{json.dumps(waw)}] = {json.dumps(staged)};')
    if waw_map:
        # WaW's level.script is the WaW map name (see gscport.WAW_LEVEL_FIELDS)
        lines.append(f'    level.waw_script = "{waw_map}";')
    lines.append('    level.waw_zombie_var_overrides = [];')
    for name, value in sorted((zombie_var_overrides or {}).items()):
        lines.append(f'    level.waw_zombie_var_overrides[{json.dumps(name)}] = {json.dumps(value)};')
    for waw, bo2 in sorted(fx_table.items()):
        lines.append(f'    level.waw2bo2_fx["{waw}"] = "{bo2}";')
    # WaW libraries can request effects after their first wait. T6 requires
    # each valid effect to be precached during the initial level frame.
    for bo2 in sorted(set(fx_table.values())):
        lines.append(f'    loadfx( "{bo2}" );')
    if bolted_loops:
        lines.append('    level.waw2bo2_bolted_loops = [];')
        for name in sorted(bolted_loops):
            lines.append(f'    level.waw2bo2_bolted_loops[loadfx( "{name}" )] = true;')
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


BO2_WEAPONS_SCRIPT = "_waw2bo2_bo2weapons.gsc"


def bo2_weapons_source(weapons: set[str] = frozenset()) -> str:
    """Every weapon the game has loaded for the map (linked mod.ff + always
    loaded zones). compat waw_weapon passes a name outside the converted table
    through only when it is listed here; an empty list disables that check."""
    lines = ["// waw2bo2: BO2 weapons loaded with the map (generated by build-mod).", "init()", "{"]
    lines += [f'    level.waw2bo2_bo2_weapons["{name}"] = 1;' for name in sorted({w.lower() for w in weapons})]
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
        "    // BO2's solo swap ranks the player's pistols against BO2's list (a WaW",
        "    // pistol ranks 0), marks the player as having no pistol and hands over",
        "    // level.laststandpistol, taken again on revive. That is the WaW start",
        "    // pistol here, so a solo revive took the player's own gun. WaW gives",
        "    // back exactly the weapons held when downed: disable the swap.",
        "    while ( !isdefined( level.pistol_value_solo_replace_below ) )",
        "        wait 0.05;",
        "    level.pistol_value_solo_replace_below = -1;",
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
    # The mystery box, wall buys, ammo and turrets are the map's own: the
    # preserved _zombiemode_weapons runs its init (gsc_api preserved_scripts).
}
# the framework init whose level assignments the box code reads (animations, flags)
BOX_STATE_INIT = ("maps\\_zombiemode_weapons", "init")
# WaW framework inits whose level tables map scripts read (the anim tables of
# _zombiemode::init_anims); only the fields the ported scripts read are kept
FRAMEWORK_STATE_INITS = (("maps\\_zombiemode", "init_standard_zombie_anims"),)
# WaW framework FX tables (level._effect) BO2 never builds; only the keys the
# ported scripts read without assigning are kept
FRAMEWORK_EFFECT_INITS = (("maps\\_zombiemode", "init_fx"),)
DAMAGE_OVERRIDE = "player_damage_override"
# WaW framework function whose inline formula sets level.zombie_total
ROUND_SPAWNING = "round_spawning"
# WaW player damage entry points a map may override with leading rules of its own
DAMAGE_PRELUDES = [(ZOMBIEMODE, DAMAGE_OVERRIDE), ("maps\\_callbackglobal", "callback_playerdamage")]
# s: BO2 end_game's intermission wait while a map's own end_game wait runs
END_GAME_FALLBACK = 30


# Statements of a WaW perk script's init that start its Pack-a-Punch
PACK_A_PUNCH_MARKERS = ("upgrade", "packapunch", "pack_a_punch", "knuckle")


def _power_relay(script: gsc.Script, fn: gsc.Function) -> bool:
    """A function that only waits and then notifies level events (a perk
    pack's wait_for_power: flag_wait electricity_on; notify staminup_on)."""
    spans = top_level_statements(script.tokens, fn.body_open, fn.body_close)
    texts = ["".join(t.low for t in script.tokens[a:b]) for a, b in spans]
    return any(t.startswith("levelnotify(") for t in texts) and all(
        t.startswith(("flag_wait(", "wait", "levelwaittill(", "levelnotify(")) for t in texts)


def waw_pack_a_punch_source(source: str, path: str) -> tuple[str, set[str]] | None:
    """With BO2 stock perks, a WaW perk script keeps only what BO2 does not
    replace: its Pack-a-Punch and the power relays that turn machines on.
    Returns the script reduced to an init with those statements and the
    functions they reach, and the kept function names; None if nothing."""
    script = gsc.parse(source, path)
    init = script.functions.get("init")
    if init is None:
        return None
    toks = script.tokens
    own = norm(path)

    def callees(start: int, end: int) -> set[str]:
        return {r.name.lower() for r in gsc.references(toks, start, end)
                if (r.qualifier is None or norm(r.qualifier) == own) and r.name.lower() in script.functions}

    kept = []
    for start, end in top_level_statements(toks, init.body_open, init.body_close):
        text = "".join(t.low for t in toks[start:end])
        relays = [script.functions[n] for n in callees(start, end) if n != "init"]
        if any(m in text for m in PACK_A_PUNCH_MARKERS) or \
                (relays and all(_power_relay(script, fn) for fn in relays) and "thread" in text):
            kept.append((start, end))
    if not kept:
        return None
    reached: set[str] = set()
    queue = [n for span in kept for n in callees(*span)]
    while queue:
        name = queue.pop()
        if name in reached or name == "init":
            continue
        reached.add(name)
        fn = script.functions[name]
        queue += callees(fn.body_open, fn.body_close)
    directives = [t.text for t in toks if t.kind == gsc.DIRECTIVE]
    parts = ["\n".join(directives), "init()\n{" + "".join(gsc.emit(toks[a:b]) for a, b in kept) + "\n}"]
    parts += [gsc.emit(toks[fn.start:fn.body_close + 1]).strip()
              for fn in sorted((script.functions[n] for n in reached), key=lambda f: f.start)]
    header = f"// waw2bo2: BO2 stock perks; the WaW Pack-a-Punch and power relays of {path} stay.\n"
    return header + "\n\n".join(parts) + "\n", reached | {"init"}


def top_level_statements(tokens: list[gsc.Token], open_: int, close: int) -> list[tuple[int, int]]:
    """[start, end) token spans of the statements directly inside the block
    whose braces are at ``open_`` / ``close`` (an if/else chain is one).
    A /# ... #/ developer block between statements is skipped, as a release
    WaW build does (else the next statement's span starts at #/: Verruckt's
    round_start lost level.chalk_hud1)."""
    spans, start, depth = [], open_ + 1, 0
    skip_to = 0
    for i in range(open_ + 1, close):
        if i < skip_to:
            continue
        t = tokens[i].text
        if t == "/#" and depth == 0 and start == i:
            skip_to = next((j for j in range(i + 1, close) if tokens[j].text == "#/"), close) + 1
            start = skip_to
            continue
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
# WaW engine melee perks -> the BO2 melee weapon _waw2bo2_compat equips for them
ALTMELEE_WEAPONS = {"specialty_altmelee": "bowie_knife_zm"}


def tesla_callback_source(entries: dict[str, str]) -> str:
    definition = API['tesla_runtime']
    return f'''
// WaW's source chain owns Tesla kills; the projectile only starts the chain.
waw_tesla_init()
{{
    {entries['initialize']}();
    // Wunderwaffe limit requested by the user (WaW's script says 20).
    if ( isdefined( level.zombie_vars["tesla_max_enemies_killed"] ) )
        level.zombie_vars["tesla_max_enemies_killed"] = min( {definition['max_targets']}, level.zombie_vars["tesla_max_enemies_killed"] );
    // A Tesla kill never drops through the ordinary path; WaW's only Waffe
    // drop needs tesla_kills_for_powerup hits in one chain. At the user's
    // request it drops when a shot reaches the cap (WaW's 15 is unreachable).
    if ( isdefined( level.zombie_vars["tesla_kills_for_powerup"] ) && isdefined( level.zombie_vars["tesla_max_enemies_killed"] ) )
        level.zombie_vars["tesla_kills_for_powerup"] = min( level.zombie_vars["tesla_kills_for_powerup"], level.zombie_vars["tesla_max_enemies_killed"] );
    {definition['native_register']}( ::waw_tesla_damage );
}}

waw_tesla_damage( mod, hit_location, hit_origin, player, amount )
{{
    if ( !isdefined( player ) || !isplayer( player ) || !self {entries['damage_predicate']}( mod ) )
        return false;
    // Reserve synchronously: T6 can dispatch direct and splash callbacks in
    // the same frame before a newly threaded source chain starts running.
    if ( isdefined( player.waw_tesla_chain_active ) && player.waw_tesla_chain_active )
        return true;
    if ( isdefined( self.zombie_tesla_hit ) && self.zombie_tesla_hit )
        return true;
    player.waw_tesla_chain_active = true;
    if ( !isdefined( player.tesla_network_death_choke ) )
        player.tesla_network_death_choke = 0;
    if ( !isdefined( player.waw_tesla_choke_bound ) )
    {{
        player.waw_tesla_choke_bound = true;
        player thread waw_tesla_network_choke();
    }}
    // The shooter owns the thread. The target may die during the source
    // traversal; its damage-listener lifetime must not terminate the chain.
    player thread waw_tesla_run_chain( self, hit_location, hit_origin );
    return true;
}}

waw_tesla_run_chain( target, hit_location, hit_origin )
{{
    self endon( "disconnect" );
    if ( isdefined( target ) && isalive( target ) )
        target {entries['damage_entry']}( hit_location, hit_origin, self );
    self.waw_tesla_chain_active = false;
}}

waw_tesla_network_choke()
{{
    self endon( "disconnect" );
    for ( ;; )
    {{
        wait_network_frame();
        wait_network_frame();
        self.tesla_network_death_choke = 0;
    }}
}}
'''


def weapons_source(registration: dict[str, list[str | None]], melee: list[str] = ()) -> str:
    """``melee``: BO2 melee weapons standing in for a WaW engine melee perk."""
    lines = ["// waw2bo2: the map's WaW weapon registration on BO2 _zm_weapons (generated).",
             "// include/add_zombie_weapon calls go through _waw2bo2_compat (WaW -> BO2 arguments).", "",
             "include_weapons()", "{",
             "    // WaW hints are plain text: prompt = display name + cost (see waw_add_zombie_weapon)",
             "    level.monolingustic_prompt_format = 1;"]
    lines += [f'    maps\\mp\\zombies\\_zm_weapons::include_zombie_weapon( "{w}", 0 );'
              for w in (*BO2_FRAMEWORK_WEAPONS, *melee)]
    lines += [f"    {CORE}::{fn}();" for fn in registration.get("include", []) if fn]
    lines += ["}", "", "add_weapons()", "{",
              '    maps\\mp\\zombies\\_zm_weapons::add_zombie_weapon( "m1911_zm", "m1911_upgraded_zm", '
              '&"ZOMBIE_WEAPON_M1911", 50, "", "", undefined );']
    lines += [f'    {COMPAT}::waw_native_only_weapon( "{w}" );' for w in (*BO2_FRAMEWORK_WEAPONS, *melee)]
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
    changed = False
    for (side, anchor, ext), main in zip(ANIMTREE_HOOKS, (main_gsc, main_csc)):
        base = "maps" if side == "maps" else "clientscripts"
        path = out_root / base / "mp" / "waw" / f"_waw2bo2_animtrees.{ext}"
        call = f"    {base}\\mp\\waw\\_waw2bo2_animtrees::init();\n"
        source = main.read_text(encoding="utf-8", errors="replace")
        if not trees:
            # Re-porting clears the generated server scripts, while the map
            # main and client scripts can retain hooks from the previous run.
            # Remove both sides when the new conversion needs no registration.
            updated = source.replace(call, "")
            if updated != source:
                main.write_text(updated, encoding="utf-8")
                changed = True
            path.unlink(missing_ok=True)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(animtrees_source(trees), encoding="utf-8")
        if anchor + '\n' not in source:
            # Perk ownership redirects _zm::init to the converter's owned
            # bootstrap. It still performs the same base animtree registration.
            anchor = anchor.replace(base + '\\mp\\zombies\\_zm',
                                    base + '\\mp\\waw\\_waw2bo2_zm')
        if anchor + "\n" + call in source:
            continue
        if anchor + "\n" not in source:
            raise ValueError(f"{main}: no '{anchor.strip()}' line to register the WaW animtrees after")
        # a call placed elsewhere by an earlier run moves after the anchor
        source = source.replace(call, "")
        main.write_text(source.replace(anchor + "\n", anchor + "\n" + call, 1), encoding="utf-8")
        changed = True
    return changed


def drop_preserved_template_inits(main_gsc: Path, out_root: Path) -> list[str]:
    """Remove the template's BO2 counterpart setup of each WaW framework script
    the map ports whole (gsc_api preserved_template_inits); returns the lines
    removed. A module the map does not port keeps the template's line."""
    source = main_gsc.read_text(encoding="utf-8", errors="replace")
    removed = []
    for module, lines in API.get("preserved_template_inits", {}).items():
        if module == "doc" or not (out_root / (ported_path(module).replace("\\", "/") + ".gsc")).is_file():
            continue
        for entry in lines:
            if entry["line"] + "\n" in source:
                source = source.replace(entry["line"] + "\n", "", 1)
                removed.append(entry["line"].strip())
    if removed:
        main_gsc.write_text(source, encoding="utf-8")
    return removed


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
# Keep the native runtime on Original: its Easy branch changes the shared
# round speed state. Source WaW set_run_speed owns each zombie's speed choice,
# including map overrides such as sprinting from round one.
WAW_DIFFICULTY = ('level.gamedifficulty = getgametypesetting( "zmDifficulty" );',
                  'level.gamedifficulty = 1; // waw2bo2: Original runtime, source WaW speed selection')
# Stock maps draw WaW's round counter (stock_map_policies waw_round_counter):
# BO2's setroundsplayed display is 8 bits wide (300 shows as 43) and round_think
# clamps level.round_number at 255. Both stay as they were for other maps.
WAW_ROUND_COUNTER_FLAG = "isdefined( level.waw2bo2_waw_round_counter ) && level.waw2bo2_waw_round_counter"
# (BO2 text, replacement, occurrences in round_think)
WAW_ROUND_COUNTER = (
    ("setroundsplayed( level.round_number );", "waw2bo2_setroundsplayed( level.round_number );", 2),
    ("if ( 255 < level.round_number )", f"if ( 255 < level.round_number && !( {WAW_ROUND_COUNTER_FLAG} ) )", 1),
)
WAW_ROUND_COUNTER_HELPER = f"""
waw2bo2_setroundsplayed( round )
{{
    // waw2bo2: WaW's script round counter replaces BO2's display on stock maps
    if ( {WAW_ROUND_COUNTER_FLAG} )
        return;

    setroundsplayed( round );
}}
"""
# WaW's Callback_PlayerDamage passes the player's own explosive/projectile
# damage through in full (a point-blank Wunderwaffe or grenade can down you).
# BO2's player_damage_override caps it at 75 above 75 health. Zombie and dog
# hits already match: WaW's weaponless zombies melee with defaultweapon
# (meleeDamage 150) and dogs with dog_MeleeDamage 100, both times
# player_meleeDamageMultiplier 0.4 (60 and 40, BO2's values).
WAW_SELF_DAMAGE_CAP = (
    "        if ( self.health > 75 && !( isdefined( self.is_zombie ) && self.is_zombie ) )\n"
    "            return 75;\n",
    "        // waw2bo2: WaW applies the player's own explosive damage in full.\n")
# WaW scales non-melee damage to players by player_damageMultiplier
# (Translator.player_damage_scale_hook). Zombie and dog hits are WaW's measured
# melee values already (see WAW_SELF_DAMAGE_CAP); falling damage keeps BO2's.
WAW_PLAYER_DAMAGE_SCALE = (
    "\n    finaldamage = idamage;\n",
    "\n    // waw2bo2: WaW's player_damageMultiplier on non-melee hits\n"
    "    if ( isdefined( level.waw2bo2_player_damage_scale ) && idamage > 0 && smeansofdeath != \"MOD_MELEE\"\n"
    "        && smeansofdeath != \"MOD_FALLING\" && !( isdefined( eattacker ) && isdefined( eattacker.is_zombie ) && eattacker.is_zombie ) )\n"
    "        idamage = int( max( 1, idamage * level.waw2bo2_player_damage_scale ) );\n\n"
    "    finaldamage = idamage;\n")
# A map started directly (devmap, no lobby) leaves ui_zm_mapstartlocation
# empty. The scripts read "" as "default", but the lobby the game returns to
# after the match falls back to "transit" and asks Engine.GetGamemodesZombie
# for (map, "transit"): no row, nil, and SelectStartLocZombie.lua's ipairs
# raises an LUI error on "Map". Name the converted map's only location.
WAW_START_LOCATION = (
    '    level.scr_zm_map_start_location = getdvar( #"ui_zm_mapstartlocation" );\n',
    '    if ( getdvar( #"ui_zm_mapstartlocation" ) == "" )\n'
    '        setdvar( "ui_zm_mapstartlocation", "default" );\n\n'
    '    level.scr_zm_map_start_location = getdvar( #"ui_zm_mapstartlocation" );\n')
# WaW's round_spawn_failsafe kills a zombie that is stuck or below the world
# and never returns it to the round (no zombie_total++ anywhere in WaW's
# _zombiemode scripts). BO2 requeues every timed-out zombie, which put extra
# zombies into rounds the source formula had already counted.
WAW_FAILSAFE_NO_REQUEUE = ("level.put_timed_out_zombies_back_in_queue = 1;",
                           "level.put_timed_out_zombies_back_in_queue = 0; // waw2bo2: WaW never requeues")
# WaW's round_spawning calls the source health step (Translator.extract_round_health):
# one increment per round that runs round_spawning, so dog rounds (which run
# dog_round_spawning instead) never raise health, and the source arithmetic
# wraps at 32 bits ("insta-kill rounds") exactly as the map's code does.
WAW_ROUND_HEALTH = (
    "    ai_calculate_health( level.round_number );\n",
    "    if ( isdefined( level.waw2bo2_round_health_func ) )\n"
    "        waw2bo2_round_health();\n"
    "    else\n"
    "        ai_calculate_health( level.round_number );\n")
WAW_ROUND_HEALTH_HELPER = """
waw2bo2_round_health()
{
    // waw2bo2: the source health step, called where WaW's round_spawning calls it
    // (an older framework steps it in its own round loop, which runs instead)
    if ( isdefined( level.waw2bo2_health_in_round_loop ) && level.waw2bo2_health_in_round_loop )
        return;

    if ( isdefined( level.waw2bo2_source_zombie_health ) )
        level.zombie_health = level.waw2bo2_source_zombie_health;

    [[ level.waw2bo2_round_health_func ]]();
    level.waw2bo2_source_zombie_health = level.zombie_health;
}
"""
# Anchors every WaW round_think has (Translator.extract_round_loop); ROUND_WAIT is
# the source round_wait call (translated: the extracted function's name).
SOURCE_ROUND_ANCHORS = (
    # older frameworks (Verruckt) thread a local: round_spawn_func = ::round_spawning
    ("spawn", r"level\s+thread\s+\[\[\s*(?:level\.)?round_spawn_func\s*\]\]\s*\(\s*\)\s*;"),
    ("wait", r"\bROUND_WAIT\s*\(\s*\)\s*;"),
    ("advance", r"level\.round_number\s*\+\+\s*;"),
)
# anchor -> (owned glue before it, owned glue after it)
SOURCE_ROUND_GLUE_CALLS = {
    "spawn": ("waw2bo2_round_spawn_ready", "waw2bo2_round_started"),
    "wait": (None, "waw2bo2_round_waited"),
    "advance": (None, "waw2bo2_round_advanced"),
}
# BO2's round_think minus its game rules: what BO2's other systems, stats and
# the HUD need from a round loop when the map's own loop drives the rounds.
WAW_SOURCE_ROUND_GLUE = """
waw2bo2_round_think_begin( restart )
{
    // waw2bo2: BO2 round_think's start (controls, stats, round display)
    if ( !isdefined( restart ) )
        restart = 0;

    if ( !restart )
    {
        if ( isdefined( level.initial_round_wait_func ) )
            [[ level.initial_round_wait_func ]]();

        if ( !( isdefined( level.host_ended_game ) && level.host_ended_game ) )
        {
            players = get_players();

            foreach ( player in players )
            {
                if ( !( isdefined( player.hostmigrationcontrolsfrozen ) && player.hostmigrationcontrolsfrozen ) )
                    player freezecontrols( 0 );

                player maps\\mp\\zombies\\_zm_stats::set_global_stat( "rounds", level.round_number );
            }
        }
    }

    waw2bo2_setroundsplayed( level.round_number );
}

waw2bo2_round_spawn_ready()
{
    // waw2bo2: BO2's spawner needs an active spawn location
    while ( level.zombie_spawn_locations.size <= 0 )
        wait 0.1;
}

waw2bo2_round_started()
{
    // waw2bo2: BO2's start-of-round signal and stats
    level notify( "start_of_round" );
    recordzombieroundstart();
    players = getplayers();

    for ( index = 0; index < players.size; index++ )
    {
        zonename = players[index] get_current_zone();

        if ( isdefined( zonename ) )
            players[index] recordzombiezone( "startingZone", zonename );
    }

    if ( isdefined( level.round_start_custom_func ) )
        [[ level.round_start_custom_func ]]();
}

waw2bo2_round_waited()
{
    // waw2bo2: BO2's end-of-round signal and stats
    level notify( "end_of_round" );
    uploadstats();

    if ( isdefined( level.round_end_custom_logic ) )
        [[ level.round_end_custom_logic ]]();

    players = get_players();
    array_thread( players, maps\\mp\\zombies\\_zm_pers_upgrades_system::round_end );

    for ( player_index = 0; player_index < players.size; player_index++ )
    {
        if ( players[player_index].pers["team"] != "spectator" )
        {
            zonename = players[player_index] get_current_zone();

            if ( isdefined( zonename ) )
                players[player_index] recordzombiezone( "endingZone", zonename );
        }
    }

    recordzombieroundend();
}

waw2bo2_round_advanced()
{
    // waw2bo2: BO2's per-round stats, round display and hot-join Quick Revive
    waw2bo2_setroundsplayed( level.round_number );
    matchutctime = getutc();
    players = get_players();

    foreach ( player in players )
    {
        if ( level.curr_gametype_affects_rank && level.round_number > 3 + level.start_round )
            player maps\\mp\\zombies\\_zm_stats::add_client_stat( "weighted_rounds_played", level.round_number );

        player maps\\mp\\zombies\\_zm_stats::set_global_stat( "rounds", level.round_number );
        player maps\\mp\\zombies\\_zm_stats::update_playing_utc_time( matchutctime );
    }

    check_quickrevive_for_hotjoin();
}
"""
WAW_ZOMBIE_VAR_HELPER = """
waw2bo2_zombie_var_divisor( var )
{
    if ( isdefined( level.waw2bo2_zombie_var_divisors ) && isdefined( level.waw2bo2_zombie_var_divisors[var] ) )
        return level.waw2bo2_zombie_var_divisors[var];

    return undefined;
}

waw2bo2_apply_zombie_var_defaults()
{
    // waw2bo2: WaW framework zombie_vars BO2's init_levelvars does not set
    // (zombie_health_increase_percent, read by the source health step)
    if ( !isdefined( level.waw2bo2_zombie_var_defaults ) )
        return;

    foreach ( var, value in level.waw2bo2_zombie_var_defaults )
    {
        if ( !isdefined( level.zombie_vars[var] ) )
            maps\\mp\\waw\\_waw2bo2_compat::waw_set_zombie_var( var, value, waw2bo2_zombie_var_divisor( var ) );
    }
}

waw2bo2_set_zombie_var( var, value, is_float, column, is_team_based )
{
    // waw2bo2: a variable WaW's framework sets keeps WaW's default and only the
    // map's own WaW table may change it (Translator.framework_zombie_vars);
    // BO2's stock mp/zombiemode.csv (zombie_spawn_delay 2.0) does not apply.
    if ( isdefined( level.waw2bo2_zombie_var_defaults ) && isdefined( level.waw2bo2_zombie_var_defaults[var] ) )
    {
        maps\\mp\\waw\\_waw2bo2_compat::waw_set_zombie_var( var, level.waw2bo2_zombie_var_defaults[var], waw2bo2_zombie_var_divisor( var ), is_float );
        return level.zombie_vars[var];
    }

    return set_zombie_var( var, value, is_float, column, is_team_based );
}
"""


def waw_pvp_attack_notify(bootstrap: str) -> str:
    """T4 notifies the victim "weapon_pvp_attack" (attacker, weapon, damage,
    mod) when a player's weapon hits a player, including the shooter. WaW's
    Wunderwaffe self-zap (tesla_pvp_thread: 25% of max health, never below 1,
    electrocution) waits for it; T6 never sends it, so the zap only did the
    projectile's 1 damage."""
    script = gsc.parse(bootstrap, "owned zombie bootstrap")
    fn = script.functions.get("callback_playerdamage")
    if fn is None or len(fn.params) < 6:
        raise ValueError("BO2 server bootstrap has no Callback_PlayerDamage")
    attacker, damage, mod, weapon = fn.params[1], fn.params[2], fn.params[4], fn.params[5]
    script.tokens[fn.body_open].text += (
        f"\n    // waw2bo2: T4's player weapon hit notify (WaW scripts wait for it)\n"
        f"    if ( isdefined( {attacker} ) && isplayer( {attacker} ) )\n"
        f"        self notify( \"weapon_pvp_attack\", {attacker}, {COMPAT}::waw_weapon_name( {weapon} ), {damage}, {mod} );\n")
    return gsc.emit(script.tokens)


def waw_zombie_var_defaults(bootstrap: str) -> str:
    """BO2's zombie_vars (_zm::init_levelvars, its init_zombie_vars) through
    waw2bo2_set_zombie_var: WaW's framework value where WaW sets the variable."""
    script = gsc.parse(bootstrap, "owned zombie bootstrap")
    fn = script.functions.get("init_levelvars")
    if fn is None:
        raise ValueError("BO2 server bootstrap has no init_levelvars")
    toks, count = script.tokens, 0
    for i in range(fn.body_open, fn.body_close - 2):
        if toks[i].low == "set_zombie_var" and toks[i + 1].text == "(" and toks[i + 2].kind == gsc.STRING \
                and toks[i - 1].text not in (".", "::"):
            toks[i].text = "waw2bo2_set_zombie_var"
            count += 1
    if not count:
        raise ValueError("BO2 init_levelvars sets no zombie_vars")
    # WaW framework variables BO2 never sets (zombie_health_increase_percent,
    # read by the source health step) get WaW's value too.
    toks[fn.body_close].pre = "\n    waw2bo2_apply_zombie_var_defaults();\n" + toks[fn.body_close].pre
    return gsc.emit(script.tokens) + WAW_ZOMBIE_VAR_HELPER


BO2_PERK_OVERRIDES = (
    "maps/mp/zombies/_zm_perks.gsc",
    "clientscripts/mp/zombies/_zm_perks.csc",
)


def owned_dogs_source(bo2_root: Path) -> str:
    """BO2's _zm_ai_dogs with the WaW sounds the source plays at the same points
    (gsc_api zombie_runtime dog_rounds native_sounds / native_music): the map's
    bank holds WaW's dog aliases as waw/<alias>, never BO2's hellhound ones."""
    dogs = API['zombie_runtime']['dog_rounds']
    native, owned = dogs['native_script'], dogs['owned_script']
    text = (bo2_root / 'raw' / (native.replace('\\', '/') + '.gsc')).read_text(encoding='utf-8')
    for bo2, waw in dogs['native_sounds'].items():
        if f'"{bo2}"' not in text:
            raise ValueError(f'{native}: dog sound "{bo2}" not found')
        text = text.replace(f'"{bo2}"', f'"waw/{waw}"')
    for state, waw in dogs['native_music'].items():
        old = f'level thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( "{state}" );'
        if old not in text:
            raise ValueError(f'{native}: dog music state "{state}" not found')
        text = text.replace(old, f'level thread maps\\mp\\zombies\\_zm_utility::play_sound_2d( "waw/{waw}" );')
    # Effects: WaW's dog script loads the same paths (spawn lightning, eyes,
    # trails, gib); prefer its converted effects (dog_rounds_hook queues them).
    text = re.sub(r'\bloadfx\s*\(', lambda _: f'{COMPAT}::waw_loadfx_native(', text, flags=re.IGNORECASE)
    return ('// waw2bo2: BO2 dog rounds playing the source WaW dog sounds and effects (generated).\n' +
            text.replace(native + '::', owned + '::') + DOG_ACTOR_GLUE)


# T6 actor setup BO2's dog_init gives a dog and WaW's dog_init leaves to its
# engine (gsc_api zombie_runtime actor_glue runs it first in the source
# dog_init when WaW drives the dog rounds). No round rules here.
DOG_ACTOR_GLUE = """

waw2bo2_dog_actor_glue()
{
    self.team = level.zombie_team;
    // BO2's dog animscripts and traversals read these
    self.zombie_move_speed = "sprint";
    self.a.disablepain = 1;
    self disable_react();
    // WaW: dog_MeleeDamage 100 x player_meleeDamageMultiplier 0.4 (_zombiemode_dogs::init)
    self.meleedamage = 40;
    self.thundergun_knockdown_func = ::dog_thundergun_knockdown;
    // per-hit player points through BO2's actor damage plumbing
    self thread maps\\mp\\zombies\\_zm_spawner::enemy_death_detection();
    self thread waw2bo2_dog_keep_enemy();
    self thread waw2bo2_dog_combat_signal();
}

waw2bo2_dog_combat_signal()
{
    // WaW's dog animscripts notify "dog_combat" on entering combat
    // (dog_combat.gsc main, dog_stop.gsc); T6's zm_dog scripts only send
    // "dog_running". Source dog scripts wait for either (Der Riese's
    // dog_run_think drops the spawn health and lights the eyes then).
    self endon( "death" );
    fighting = 0;

    while ( true )
    {
        state = self getanimstatefromasd();
        now = isdefined( state ) && issubstr( state, "zm_combat" );

        if ( now && !fighting )
            self notify( "dog_combat" );

        fighting = now;
        wait 0.1;
    }
}

waw2bo2_dog_keep_enemy()
{
    // T6 dogs drop a favoriteenemy that went down; WaW's engine retargeted
    self endon( "death" );

    while ( true )
    {
        if ( isdefined( self.favoriteenemy ) && !is_player_valid( self.favoriteenemy ) )
            self.favoriteenemy = get_favorite_enemy();

        wait 0.2;
    }
}
"""


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
    if WAW_DIFFICULTY[0] not in bootstrap:
        raise ValueError("BO2 server bootstrap does not read zmDifficulty")
    bootstrap = bootstrap.replace(*WAW_DIFFICULTY)
    for old, new, count in WAW_ROUND_COUNTER:
        if bootstrap.count(old) != count:
            raise ValueError(f"BO2 server bootstrap round counter changed: {old}")
        bootstrap = bootstrap.replace(old, new)
    if bootstrap.count(WAW_SELF_DAMAGE_CAP[0]) != 1:
        raise ValueError("BO2 server bootstrap explosive self-damage cap changed")
    bootstrap = bootstrap.replace(*WAW_SELF_DAMAGE_CAP)
    if bootstrap.count(WAW_PLAYER_DAMAGE_SCALE[0]) != 1:
        raise ValueError("BO2 server bootstrap player damage result changed")
    bootstrap = bootstrap.replace(*WAW_PLAYER_DAMAGE_SCALE)
    if bootstrap.count(WAW_START_LOCATION[0]) != 1:
        raise ValueError("BO2 server bootstrap start location read changed")
    bootstrap = bootstrap.replace(*WAW_START_LOCATION)
    if bootstrap.count(WAW_FAILSAFE_NO_REQUEUE[0]) != 1:
        raise ValueError("BO2 server bootstrap failsafe requeue changed")
    bootstrap = bootstrap.replace(*WAW_FAILSAFE_NO_REQUEUE)
    if bootstrap.count(WAW_ROUND_HEALTH[0]) != 1:
        raise ValueError("BO2 server bootstrap round health call changed")
    bootstrap = bootstrap.replace(*WAW_ROUND_HEALTH)
    bootstrap += WAW_ROUND_HEALTH_HELPER
    bootstrap += WAW_SOURCE_ROUND_GLUE
    bootstrap = waw_zombie_var_defaults(bootstrap)
    bootstrap = waw_pvp_attack_notify(bootstrap)
    bootstrap += WAW_ROUND_COUNTER_HELPER
    (server_dir / "_waw2bo2_zm.gsc").write_text(
        PERK_OVERRIDE_HEADER + bootstrap.replace(native, owned), encoding="utf-8")
    dogs = API['zombie_runtime']['dog_rounds']
    if (bo2_root / 'raw' / (dogs['native_script'].replace('\\', '/') + '.gsc')).is_file():
        (out_root / (dogs['owned_script'].replace('\\', '/') + '.gsc')).write_text(
            owned_dogs_source(bo2_root), encoding='utf-8')


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
