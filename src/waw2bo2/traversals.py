"""WaW traversal scripts -> T6, from the source scripts the map's path nodes name.

A WaW negotiation node names ``animscripts/traverse/<name>.gsc``; its main()
branches on the actor: zombies (and crawlers) play a zombie animation, dogs
call a dog traversal of ``animscripts/traverse/shared`` (dog_jump_down,
dog_jump_up, dog_wall_and_window_hop) that plays an entry of the dog
animation table (``anim.dogAnims[self.animSet].traverse[...]``, filled by
``animscripts/dog_init``). paths.py maps the zombie branch onto BO2's
zm_traverse substates; this module carries the dog branch from the source:

* the dog calls of each source main() become the dog branch of the generated
  script;
* the dog traversal functions of the source shared.gsc are transpiled for
  T6's animation state machine (``transpile_dog_library``): a raw animation
  played from the dog table becomes the ``zm_traverse`` substate
  ``waw_dog_<table key>`` of the dog's animstate, WaW's notetrack wait
  becomes T6's for that state; the helpers they call from the same file
  (teleportThreadEx...) come along;
* the dog table of the source dog_init gives those substates their WaW
  animations (``dog_traverse_anims``), for the animation set dog_init picks
  for zombie actors.

Nothing here lists traversal names, heights or animations: they all come from
the source scripts (docs/CONVERT_THE_WHOLE_MAP.md).
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from . import gsc

TRAVERSE_DIR = "animscripts/traverse"
SHARED = "shared"
DOG_INIT = "animscripts/dog_init.gsc"
OWNED_SHARED = "waw2bo2_dog_shared"
OWNED_SHARED_PATH = f"maps\\mp\\animscripts\\traverse\\{OWNED_SHARED}"
DOG_STATE_PREFIX = "waw_dog_"
T6_TRAVERSE_STATE = "zm_traverse"
T6_TRAVERSE_NOTIFY = "traverse_anim"

_DOG_TABLE_READ = r'anim\.dogAnims\[\s*self\.animSet\s*\]\.traverse\[\s*'
_DOG_TABLE_ENTRY = re.compile(r'anim\.dogAnims\[\s*"(\w+)"\s*\]\.traverse\[\s*"(\w+)"\s*\]\s*=\s*%\s*(\w+)\s*;')
_ZOMBIE_ANIM_SET = re.compile(r'if\s*\(\s*self\s+animscripts\\utility::is_zombie\s*\(\s*\)\s*\)\s*\{?\s*'
                              r'self\.animSet\s*=\s*"(\w+)"', re.IGNORECASE)
_ANY_ANIM_SET = re.compile(r'self\.animSet\s*=\s*"(\w+)"', re.IGNORECASE)

# Mechanical rewrites of WaW raw-animation calls onto T6's animation states.
_REWRITES = (
    # the state machine blends; T4's explicit clears have no T6 counterpart
    (re.compile(r'self\s+clearanim\s*\([^;]*\)\s*;', re.IGNORECASE), ''),
    (re.compile(r'self\s+setflaggedanimrestart\s*\(\s*"[^"]*"\s*,\s*' + _DOG_TABLE_READ +
                r'(.+?)\s*\]\s*,[^;]*\)\s*;', re.IGNORECASE),
     lambda m: f'self setanimstatefromasd( "{T6_TRAVERSE_STATE}", "{DOG_STATE_PREFIX}" + {m[1]} );'),
    (re.compile(r'self\s+animscripts\\shared::donotetracks\s*\(\s*"[^"]*"\s*\)\s*;', re.IGNORECASE),
     f'self maps\\\\mp\\\\animscripts\\\\zm_shared::donotetracks( "{T6_TRAVERSE_NOTIFY}" );'),
)
_UNPORTED = re.compile(r'anim\.dogAnims|setflaggedanim|setanim\s*\(|clearanim|(?<!maps\\mp\\)animscripts\\',
                       re.IGNORECASE)


class TraversalError(RuntimeError):
    pass


@dataclass
class DogTraversals:
    """What the source gives the dog: per WaW traversal script, the dog calls
    of its main(); the transpiled library; the substates for the dog animstate."""
    calls: dict[str, list[str]] = field(default_factory=dict)
    library: str = ""
    states: dict[str, str] = field(default_factory=dict)      # substate -> WaW xanim
    errors: list[str] = field(default_factory=list)


def find_source(roots: list[Path], relative: str) -> Path | None:
    """The first root holding ``relative`` (map zones before stock WaW)."""
    for root in roots:
        path = Path(root) / relative
        if path.is_file():
            return path
    return None


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def dog_functions(shared: gsc.Script) -> set[str]:
    """Functions of the source traverse/shared that play the dog table."""
    found = set()
    for name, fn in shared.functions.items():
        body = gsc.emit(shared.tokens[fn.body_open:fn.body_close + 1])
        if re.search(_DOG_TABLE_READ, body, re.IGNORECASE):
            found.add(name)
    return found


def dog_calls(script: gsc.Script, names: set[str]) -> list[str]:
    """Calls of ``names`` inside main() of a source traversal, as statements."""
    fn = script.functions.get("main")
    if fn is None:
        return []
    toks, calls, i = script.tokens, [], fn.body_open + 1
    while i < fn.body_close:
        if toks[i].kind == gsc.IDENT and toks[i].low in names and toks[i + 1].text == "(" \
                and toks[i - 1].text not in (".", "::"):
            close = gsc._match(toks, i + 1, "(", ")")
            args = gsc.emit(toks[i + 2:close]).strip()
            calls.append(f"{toks[i].low}( {args} );" if args else f"{toks[i].low}();")
            i = close
        i += 1
    return calls


def local_callees(script: gsc.Script, names: set[str]) -> set[str]:
    """``names`` plus every function of the same script they call (transitively)."""
    found, todo = set(), list(names)
    while todo:
        name = todo.pop()
        if name in found or name not in script.functions:
            continue
        found.add(name)
        fn = script.functions[name]
        toks = script.tokens
        for i in range(fn.body_open + 1, fn.body_close):
            if toks[i].kind == gsc.IDENT and toks[i + 1].text == "(" and toks[i - 1].text not in (".", "::"):
                todo.append(toks[i].low)
    return found


def transpile_dog_library(shared: gsc.Script, names: set[str]) -> str:
    """The dog traversal functions of the source shared.gsc, with the helpers
    they call from it (teleportThreadEx...), for T6."""
    parts = []
    for name in sorted(local_callees(shared, names)):
        fn = shared.functions[name]
        text = gsc.emit(shared.tokens[fn.start:fn.body_close + 1]).strip()
        for pattern, replacement in _REWRITES:
            text = pattern.sub(replacement, text)
        left = _UNPORTED.search(text)
        if left:
            raise TraversalError(f"{SHARED}.gsc {name}: no T6 form for {left[0]!r}")
        parts.append(text)
    return ("// generated by waw2bo2: WaW animscripts\\traverse\\shared dog traversals for T6\n"
            "// (the dog table's animations are the zm_traverse substates " + DOG_STATE_PREFIX + "<key>)\n"
            "\n" + "\n\n".join(parts) + "\n")


def dog_traverse_anims(dog_init: str) -> tuple[str | None, dict[str, str]]:
    """(animation set dog_init gives zombie actors, {table key: xanim})."""
    chosen = _ZOMBIE_ANIM_SET.search(dog_init) or _ANY_ANIM_SET.search(dog_init)
    if chosen is None:
        return None, {}
    anim_set = chosen[1]
    table = {key: anim for set_, key, anim in _DOG_TABLE_ENTRY.findall(dog_init) if set_ == anim_set}
    return anim_set, table


def source_dog_traversals(roots: list[Path], animscripts: set[str]) -> DogTraversals:
    """Dog branches of the WaW traversal scripts the map's nodes name."""
    result = DogTraversals()
    shared_path = find_source(roots, f"{TRAVERSE_DIR}/{SHARED}.gsc")
    if shared_path is None:
        result.errors.append(f"{TRAVERSE_DIR}/{SHARED}.gsc: no WaW source; dog traversals not converted")
        return result
    shared = gsc.parse(_read(shared_path), f"animscripts\\traverse\\{SHARED}")
    names = dog_functions(shared)
    if not names:
        return result
    for script in sorted(animscripts):
        path = find_source(roots, f"{TRAVERSE_DIR}/{script}.gsc")
        if path is None:
            result.errors.append(f"{TRAVERSE_DIR}/{script}.gsc: no WaW source; its dog branch is not converted")
            continue
        calls = dog_calls(gsc.parse(_read(path), f"animscripts\\traverse\\{script}"), names)
        if calls:
            result.calls[script.lower()] = calls
    if not result.calls:
        return result
    used = {c.split("(")[0] for calls in result.calls.values() for c in calls}
    try:
        result.library = transpile_dog_library(shared, used)
    except TraversalError as exc:
        result.errors.append(str(exc))
        result.calls.clear()
        return result
    init = find_source(roots, DOG_INIT)
    if init is None:
        result.errors.append(f"{DOG_INIT}: no WaW source; dog traversal animations unknown")
        result.calls.clear()
        return result
    anim_set, table = dog_traverse_anims(_read(init))
    if not table:
        result.errors.append(f"{DOG_INIT}: no dog traversal table for animation set {anim_set}")
        result.calls.clear()
        return result
    result.states = {DOG_STATE_PREFIX + key: anim for key, anim in table.items()}
    return result


def dog_branch(calls: list[str]) -> str:
    """The generated script's dog branch (the source calls, on the owned library)."""
    body = "".join(f"        {OWNED_SHARED_PATH}::{call}\n" for call in calls)
    return ("    if ( isdefined( self.isdog ) && self.isdog )\n    {\n" + body + "        return;\n    }\n\n")


def add_dog_states(asd: str, atr: str, states: dict[str, str], asset_name) -> tuple[str, str, list[str]]:
    """Add the source dog traversals to a T6 dog animstate (its zm_traverse
    state) and their animations to its animtree. ``asset_name`` maps a WaW
    xanim to its staged asset name. Returns (asd, atr, added xanim assets)."""
    block = re.search(rf'^{T6_TRAVERSE_STATE}\s*:[^\n]*\n\{{(.*?)^\}}', asd, re.M | re.S)
    lines = "".join(f"\t{state}\t\t\t{asset_name(anim)}\n" for state, anim in sorted(states.items()))
    if block is None:
        asd = asd.rstrip("\n") + f"\n\n{T6_TRAVERSE_STATE} : aliased restart notify {T6_TRAVERSE_NOTIFY}\n{{\n{lines}}}\n"
    else:
        asd = asd[:block.end(1)] + lines + asd[block.end(1):]
    added = sorted({asset_name(anim) for anim in states.values()})
    atr, count = re.subn(r'(^body\s*\n\{)', lambda m: m[1] + '\n' + ''.join(f'\t{a}\n' for a in added),
                         atr, count=1, flags=re.M)
    if count != 1:
        raise TraversalError("dog animtree has no body group")
    return asd, atr, added
