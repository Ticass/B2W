"""
The map's vision file, and why a port must ship one.

A Black Ops II map's own script applies its colour grading by name::

    VisionSetNaked( "mp_area51", 0 );

That call is in the `createart` script, which every Call of Duty map has and
which a port carries across with the rest of its scripts. What it names is a
`rawfile` asset, `vision/<map>.vision`, holding the constants the film pass
runs on -- exposure, the three colour-mix matrices, and the saturation term::

    r_filmEnable "1"
    vc_FSM "0.212585 0.715195 0.072220 0.880859"   <- Rec.709 luma weights
    vc_SMR "1.164402 0.000000 0.000000 0.002557"   <- shadow mix, red row
    vc_HMR / vc_MMR / ...                          <- highlight and midtone

**Every stock Black Ops II map ships one. A port that does not is not merely
ungraded -- it is worse than that in two ways.**

First, `VisionSetNaked` on a name the game cannot resolve leaves the film pass
running on whatever those constants last held. With the colour-mix matrices
unset the pass reduces to the luma row, so the world renders black-and-white
with crushed shadows and blown highlights, while the HUD -- composited after
the film pass -- keeps its colour. That combination is the signature of this
bug and it reads convincingly as "the textures did not load".

Second, and worse, **vision state is global and outlives the map**. Load a port
with no vision file and every subsequent map in that session is graded by the
same broken state, including the game's own. A stock map rendering wrong after
playing a custom one is this, not a damaged installation.

Black Ops's vision files cannot be translated: it grades with `r_film*` and
`r_bloom*` keys, Black Ops II with `vc_*`, and the two sets do not overlap at
all. So the source's *look* genuinely cannot be ported. What can be done is
ship a valid Black Ops II vision file so the map is graded like a Black Ops II
map instead of ungraded -- and say plainly in the report that the grading is a
substitution rather than the original.

The file comes from the user's own installation, like every other piece of
Black Ops II knowledge this tool uses. Nothing is downloaded and nothing ships
with the tool.
"""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

#: Where a vision file lives inside a zone, and what the script asks for.
VISION_DIR = "vision"

#: Stock Black Ops II maps to take a grading from, best first. All are outdoor
#: daylight multiplayer maps, so the grading suits a ported map better than a
#: night or interior one would.
DONOR_MAPS: tuple[str, ...] = (
    "mp_nuketown_2020",
    "mp_dockside",
    "mp_hijacked",
)

#: Keys that must be present for a file to be a usable Black Ops II grading.
#: `vc_FSM` carries the saturation term, so a file without it is the very
#: failure this module exists to prevent.
REQUIRED_KEYS: frozenset[str] = frozenset({"vc_FSM", "vc_SMR", "vc_HMR", "vc_MMR"})


def cache_path() -> Path:
    """Where a recovered grading is kept between runs."""
    import os

    return Path(os.path.expandvars(r"%LOCALAPPDATA%\codport")) / "bo2_vision.txt"


def is_usable(text: str) -> bool:
    """Does this look like a Black Ops II grading rather than a Black Ops one?"""
    return all(key in text for key in REQUIRED_KEYS)


def recover(context: Any, findings: Any = None) -> tuple[str, str]:
    """
    A Black Ops II vision file's text, and the map it came from.

    Returns ("", "") when no Black Ops II installation is available to read
    one from -- the caller reports that rather than inventing constants, since
    a wrong grading is as visible as no grading.
    """
    from codport.core.errors import Category, Severity

    cache = cache_path()
    try:
        if cache.is_file():
            cached = cache.read_text(encoding="utf-8")
            if is_usable(cached):
                return cached, "cache"
    except OSError:
        pass

    toolchain = getattr(context, "toolchain", None)
    game = getattr(toolchain, "games", {}).get("t6") if toolchain else None
    unlinker = None
    try:
        unlinker = toolchain.unlinker.require() if toolchain else None
    except Exception:
        unlinker = None
    if not game or not unlinker:
        return "", ""

    workspace = getattr(context, "workspace", None)
    scratch = Path(getattr(workspace, "root", ".")) / "_vision"

    for donor in DONOR_MAPS:
        zone = Path(game.root) / "zone" / "all" / f"{donor}.ff"
        if not zone.is_file():
            continue
        out = scratch / donor
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        try:
            # Only rawfiles: a whole-zone dump of a stock map takes minutes and
            # writes gigabytes for the sake of one kilobyte of text.
            toolchain.runner.run(
                [unlinker, "-o", out, "--include-assets", "rawfile", zone],
                timeout=900,
                label=f"vision_{donor}",
            )
        except Exception:
            continue

        found = sorted(out.rglob(f"{donor}.vision"))
        for path in found:
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if not is_usable(text):
                continue
            try:
                cache.parent.mkdir(parents=True, exist_ok=True)
                cache.write_text(text, encoding="utf-8")
            except OSError:
                pass  # a read-only cache must not break the run
            shutil.rmtree(scratch, ignore_errors=True)
            return text, donor

    shutil.rmtree(scratch, ignore_errors=True)
    return "", ""


def write(zone_raw: Path, map_name: str, text: str) -> Path:
    """Write `vision/<map>.vision` into the zone's raw tree."""
    path = Path(zone_raw) / VISION_DIR / f"{map_name}.vision"
    path.parent.mkdir(parents=True, exist_ok=True)
    # The engine reads this as plain text; a BOM makes the first key unparseable.
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def asset_name(map_name: str) -> str:
    """What the zone lists and what `VisionSetNaked` resolves."""
    return f"{VISION_DIR}/{map_name}.vision"
