"""The WaW zombie zone graph, and BO2's zone manager kept out.

WaW drives zones: the map's own _zombiemode_zone_manager (preserved, started
by the map's own code) keeps level.zones; the template main's start of BO2's
_zm_zonemgr is removed (drop_bo2_zone_manager). Zones are ``info_volume``
brush entities named by targetname, linked with ``add_adjacent_zone( a, b,
flag )``, plus a list of zones active at start; the graph is read from the
map script's tokens (commented-out and ``/# #/`` calls are not part of it)
for the entity conversion and the build report.

A flag argument may be a local array built just before the call (Der Riese:
``flag_array[0] = ...; add_adjacent_zone( a, b, flag_array, true )``); it is
resolved from the local assignments in source order as ``[ "f0", "f1" ]``.
"""
from __future__ import annotations

import re
from pathlib import Path

from . import gsc

ADJACENT_RE = re.compile(r"^\s*(?:[\w\\/]+::)?add_adjacent_zone\s*\(([^;]*)\)\s*;", re.MULTILINE)
INIT_ZONE_RE = re.compile(
    r'^\s*(?:zones|init_zones|level\s*\.\s*dlc3\s*\.\s*initialzones)'
    r'\s*\[[^\]]*\]\s*=\s*"([^"]+)"\s*;', re.MULTILINE | re.IGNORECASE)


class ZoneError(ValueError):
    pass


def live_tokens(waw_gsc: str) -> list[gsc.Token]:
    """Tokens of retail code: comments dropped, ``/# ... #/`` developer blocks skipped."""
    out, dev = [], False
    for token in gsc.tokenize(waw_gsc):
        if token.text == '/#':
            dev = True
        elif token.text == '#/':
            dev = False
        elif not dev and token.kind != gsc.EOF:
            out.append(token)
    return out


def _arguments(tokens: list[gsc.Token], open_: int) -> tuple[list[list[gsc.Token]], int]:
    args, current, depth, i = [], [], 0, open_ + 1
    while i < len(tokens):
        text = tokens[i].text
        if text in ('(', '[', '[['):
            depth += 1
        elif text in (')', ']', ']]'):
            if depth == 0:
                break
            depth -= 1
        if text == ',' and depth == 0:
            args.append(current)
            current = []
        else:
            current.append(tokens[i])
        i += 1
    if current:
        args.append(current)
    return args, i


def _render(arg: list[gsc.Token], arrays: dict[str, dict[int, str]], scalars: dict[str, str]) -> str:
    if len(arg) == 1 and arg[0].kind == gsc.IDENT:
        name = arg[0].low
        if name in arrays:
            return '[ ' + ', '.join(value for _, value in sorted(arrays[name].items())) + ' ]'
        if name in scalars:
            return scalars[name]
    return ' '.join(t.text for t in arg).replace(' ( ', '( ').replace(' ,', ',')


def read_waw_zones(waw_gsc: str) -> tuple[list[str], list[str]]:
    """(initial zones, add_adjacent_zone argument lists) from a WaW map script.

    Both are empty for a map without a zone graph (prototype/asylum/sumpf
    scripts); entities.synthesize_zones builds one from its spawners."""
    tokens = live_tokens(waw_gsc)
    lines: dict[int, list[str]] = {}
    for t in tokens:
        lines.setdefault(t.line, []).append(t.text)
    live = '\n'.join(' '.join(words) for words in lines.values())
    adjacency: list[str] = []
    arrays: dict[str, dict[int, str]] = {}
    scalars: dict[str, str] = {}
    depth = 0
    for i, token in enumerate(tokens):
        if token.text == '{':
            depth += 1
        elif token.text == '}':
            depth -= 1
            if depth == 0:
                arrays, scalars = {}, {}   # locals belong to one function
        nxt = tokens[i + 1:i + 7]
        texts = [t.text for t in nxt]
        if token.kind == gsc.IDENT and (i == 0 or tokens[i - 1].text in (';', '{', '}')):
            # local array element / local string: name[ n ] = "x";  name = "x";
            # (the script's last statements leave fewer than six tokens ahead)
            if len(nxt) > 5 and texts[0] == '[' and texts[2:4] == [']', '='] and texts[5] == ';' \
                    and nxt[1].kind == gsc.NUMBER and nxt[4].kind == gsc.STRING:
                arrays.setdefault(token.low, {})[int(float(nxt[1].text))] = nxt[4].text
            elif len(nxt) > 2 and texts[0] == '=' and texts[2] == ';' and nxt[1].kind == gsc.STRING:
                scalars[token.low] = nxt[1].text
                arrays.pop(token.low, None)
            elif texts[:4] == ['=', '[', ']', ';']:
                arrays[token.low] = {}
        if token.low == 'add_adjacent_zone' and texts[:1] == ['('] and                 (i == 0 or tokens[i - 1].text != '.'):
            args, _ = _arguments(tokens, i + 1)
            adjacency.append(', '.join(_render(arg, arrays, scalars) for arg in args))
    initial = list(dict.fromkeys(INIT_ZONE_RE.findall(live)))
    if not adjacency:
        return [], []
    if not initial:
        initial = [re.findall(r'"([^"]+)"', adjacency[0])[0]]
    return initial, adjacency


def zone_names(initial: list[str], adjacency: list[str]) -> set[str]:
    """Every zone a WaW zone graph names (start zones and both ends of each link)."""
    names = set(initial)
    for args in adjacency:
        names.update(re.findall(r'"([^"]+)"', args)[:2])
    return names


# The template main starts BO2's zone manager on its own graph.
BO2_ZONE_MANAGER = re.compile(
    r'^\s*level\.zone_manager_init_func = ::\w+_zone_init;\n'
    r'\s*init_zones = \[\];\n(?:\s*init_zones\[\d+\] = "[^"]*";\n)+'
    r'\s*level thread maps\\mp\\zombies\\_zm_zonemgr::manage_zones\( init_zones \);\n', re.MULTILINE)


def drop_bo2_zone_manager(bo2_gsc: str) -> str:
    """WaW drives zones: the map's own zone manager (the preserved
    _zombiemode_zone_manager, started by the map's own code) keeps level.zones;
    BO2's is never started beside it. A map without one has none in WaW either."""
    out, n = BO2_ZONE_MANAGER.subn(
        "    // waw2bo2: WaW drives zones: the map's own zone manager runs (none here: BO2's never starts)\n",
        bo2_gsc, count=1)
    if n != 1:
        raise ZoneError("template zone manager start not found in the map script")
    return out


def apply(waw_gsc_path: Path, bo2_gsc_path: Path, project: str,
          synthesized: dict | None = None) -> tuple[list[str], int]:
    """The map's zone graph (reported), and BO2's zone manager removed."""
    initial, adjacency = read_waw_zones(waw_gsc_path.read_text(encoding="utf-8", errors="replace"))
    text = bo2_gsc_path.read_text(encoding="utf-8", errors="replace")
    bo2_gsc_path.write_text(drop_bo2_zone_manager(text), encoding="utf-8")
    return initial, len(adjacency)
