"""Gameplay zone (mod.ff) for the converted map, built by the BO2 mod tools.

A stock BO2 zombies map carries its gameplay assets (AI types, weapons, perk
machines, zbarriers, player bodies, animscripts...) in its own fastfile. The
mod tools ship that set as the ``zm_test`` template zone. The world of the
converted map comes from the bridge fastfile, so the template's own BSP,
map scripts and map ipak are removed and the rest is linked into ``mod.ff``,
which Plutonium loads with the mod.
"""
from __future__ import annotations

import csv
import math
import io
import re
import subprocess
import json
import shutil
from contextlib import contextmanager
from pathlib import Path

TEMPLATE_MAP = "zm_test"
# world assets live in the map fastfile (the bridge links skinnedverts too;
# the engine allows exactly one loaded)
BSP_TYPES = {"clipmap", "clipmap_pvs", "comworld", "gameworldsp", "gameworldmp", "mapents", "gfxworld",
             "skinnedverts"}
# referenced by the template but not produced by All2Raw from any shipped zone
KNOWN_ABSENT = {("xmodel", "viewmodel_base_viewhands")}


@contextmanager
def baseline_shader_materials(project: Path, stock: Path):
    """Give the raw mod-tools linker its stock techniques, restoring staging afterward."""
    report_file = project / 'bridge_stage.report.json'
    entries = [(project, e) for e in json.loads(report_file.read_text())['materials']] if report_file.exists() else []
    weapon_report = project/'content_source/weapons.visuals.json'
    if weapon_report.exists():
        entries += [(project/'content_source', e) for e in json.loads(weapon_report.read_text())['materials']]
    originals = {}
    try:
        for root, entry in entries:
            runtime = entry.get('shader_runtime', {})
            if not runtime.get('active'):
                continue
            path = root / entry['file']
            if not path.is_file() or path in originals:
                continue
            native = runtime.get('native_techset')
            if not native:
                native = json.loads((stock/'materials'/entry['donor']).read_text())['techniqueSet']
            originals[path] = path.read_bytes()
            material = json.loads(originals[path])
            material['techniqueSet'] = native
            path.write_text(json.dumps(material), encoding='utf-8')
        yield bool(originals)
    finally:
        for path, content in originals.items():
            path.write_bytes(content)


def activate_mod_shaders(ff: Path, project: Path, work: Path, linker: Path, unlinker: Path, stock: Path):
    """Relink the fresh gameplay zone with bound shaders, preserving its texture ABI.

    The native bridge linker cannot read every mod-tools raw asset format. Its
    global asset pools reuse the complete baseline instead. Only bound materials
    and shader dependencies are exposed as fresh inputs; stock gameplay and image
    assets retain their baseline data. The original ipak stays beside the result.
    """
    work = work.resolve()
    baseline = work/'shader_baseline/mod.ff'
    baseline.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ff, baseline)
    dump = work/'shader_baseline/dump'
    proc = subprocess.run([str(unlinker.resolve()), '--no-color', '--include-assets', 'material',
                           '--output-folder', str(dump), str(baseline)], capture_output=True, text=True)
    (work/'shader_baseline/unlinker.log').write_text(proc.stdout+proc.stderr)
    if proc.returncode:
        raise RuntimeError('shader baseline material extraction failed')
    overlay = work/'shader_overlay'
    bound = set()
    for root in (project, project/'content_source'):
        for source in (root/'materials').rglob('*.json'):
            current = json.loads(source.read_text())
            if not current.get('techniqueSet', '').startswith('waw/runtime_'):
                continue
            relative = source.relative_to(root)
            extracted = dump/relative
            if not extracted.is_file():
                # This material belongs to the world zone, not gameplay mod.ff.
                continue
            material = json.loads(extracted.read_text())
            material['techniqueSet'] = current['techniqueSet']
            target = overlay/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(material), encoding='utf-8')
            bound.add(relative.as_posix())
    for root in (stock, project, project/'content_source'):
        for folder in ('techniquesets', 'shader_bin'):
            if (root/folder).is_dir():
                shutil.copytree(root/folder, overlay/folder, dirs_exist_ok=True)
    zone = (work/'zone_source/mod.zone').read_text()
    # Raw weapon files live under mp/sp; loaded assets use engine-internal names.
    zone = re.sub(r'(?m)^weapon,(?:mp|sp)/', 'weapon,', zone)
    # The template's "fximpacttable,,ImpactFx" names the stock singleton the
    # way the mod-tools linker's singleton accessor does; that linker writes no
    # entry for it. The bridge linker reads the real name field and would emit
    # a reference to "ImpactFx", but the only stock table (common_zm) has an
    # EMPTY name: the game then fails with "Could not load default asset
    # 'default' for asset type 'impactfx'". Keep the baseline's asset list.
    zone = re.sub(r'(?m)^fximpacttable,.*(?:\r?\n|$)', '', zone)
    source_root = work/'shader_zone_source'
    source_root.mkdir(exist_ok=True)
    (source_root/'mod.zone').write_text(zone)
    output = work/'shader_out'
    command = [str(linker.resolve()), '--no-color', '--base-folder', str(work),
               '--load', str(baseline), '--source-search-path', str(source_root),
               '--asset-search-path', str(overlay), '--output-folder', str(output), 'mod']
    proc = subprocess.run(command, capture_output=True, text=True, errors='replace')
    (work/'shader_linker.log').write_text(proc.stdout+proc.stderr)
    linked = output/'mod.ff'
    if proc.returncode or not linked.is_file() or re.search(r'(?m)^ERROR:', proc.stdout):
        raise RuntimeError(f'bound shader mod relink failed ({proc.returncode}); see {work / "shader_linker.log"}')
    check = work/'shader_check'
    proc = subprocess.run([str(unlinker.resolve()), '--no-color', '--include-assets', 'material,techniqueset',
        '--output-folder', str(check), str(linked)], capture_output=True, text=True, errors='replace')
    (work/'shader_check.log').write_text(proc.stdout+proc.stderr)
    if proc.returncode:
        raise RuntimeError('bound shader mod roundtrip failed')
    for relative in bound:
        expected = json.loads((overlay/relative).read_text())
        actual = json.loads((check/relative).read_text())
        if actual['techniqueSet'] != expected['techniqueSet'] or actual['textures'] != expected['textures']:
            raise RuntimeError(f'bound material or texture routing changed: {relative}')
        technique = json.loads((check/'techniquesets'/f'{actual["techniqueSet"]}.json').read_text())
        for item in technique['techniques']:
            for shader_pass in item.get('passArray', []) if item else []:
                for field, stage in (('pixelShader', 'ps'), ('vertexShader', 'vs')):
                    name = shader_pass[field].get('name', '')
                    if name.startswith('waw/runtime_') and (check/'shader_bin'/f'{stage}_{name}.cso').read_bytes()[:4] != b'DXBC':
                        raise RuntimeError(f'linked translated shader is not DXBC: {name}')
    shutil.copy2(linked, ff)
    (work/'shader_activation.json').write_text(json.dumps({'materials': sorted(bound),
        'baseline': str(baseline), 'output': str(ff), 'preserved_ipak': True}, indent=2))


def build_mod_zone(template: Path, dropped: list[str]) -> str:
    out = []
    for raw in template.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith(">level.ipak_read,") or line.startswith(">level.ipak_write,"):
            if line.endswith("," + TEMPLATE_MAP):
                dropped.append(line)
                continue
        if line and not line.startswith(("//", ">")):
            parts = [p.strip() for p in line.split(",")]
            kind, name = parts[0], parts[-1]
            if kind in BSP_TYPES:
                dropped.append(line)
                continue
            if kind == "script" and re.search(rf"(^|/){TEMPLATE_MAP}[_.]", name):
                dropped.append(line)
                continue
            if (kind, name) in KNOWN_ABSENT:
                dropped.append(line)
                continue
        out.append(raw)
    return "\n".join(out) + "\n"


def unavailable_weapons(unavailable: list[str]) -> set[str]:
    names = set()
    for entry in unavailable:
        kind, _, name = entry.split("  <-")[0].strip().partition(",")
        if kind == "weapon":
            name = name.split("/")[-1]
            names.add(name)
            # a weapon without its pack-a-punch variant (or vice versa) is unusable
            names.add(name.replace("_upgraded_zm", "_zm") if "_upgraded_zm" in name else name.replace("_zm", "_upgraded_zm"))
    return names


def prune_weapon_references(script: Path, weapons: set[str]) -> list[str]:
    """Comment out map-script lines that name a weapon mod.ff does not carry."""
    lines = script.read_text(encoding="utf-8", errors="replace").splitlines()
    pruned = []
    for i, line in enumerate(lines):
        if any(f'"{w}"' in line for w in weapons) and not line.lstrip().startswith("//"):
            pruned.append(line.strip())
            lines[i] = "//waw2bo2: weapon not in mod.ff// " + line
    if pruned:
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return pruned


INCLUDE_RE = re.compile(r"^\s*#include\s+([\w\\/]+)\s*;", re.MULTILINE)
CALL_RE = re.compile(r"\b([a-z_][\w]*(?:\\[\w]+)+)::", re.IGNORECASE)
# zones every zombies map loads before the map; their scripts must not be duplicated
STOCK_SCRIPT_ZONES = ("common_zm", "patch_zm")


def stock_scripts(bo2: Path, unlinker: Path) -> set[str]:
    names: set[str] = set()
    for zone in STOCK_SCRIPT_ZONES:
        ff = bo2 / "zone" / "all" / f"{zone}.ff"
        proc = subprocess.run([str(unlinker), "--no-color", "--list", "--search-path", str(ff.parent), str(ff)],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
        names.update(l.split(",", 1)[1].strip().lower() for l in proc.stdout.splitlines() if l.startswith("script,"))
    return names


def script_closure(zone_text: str, raw: Path, stock: set[str]) -> list[str]:
    """Scripts reachable (via #include or path::function) from the zone's
    scripts that exist in raw/ but are neither in the zone nor in a stock
    always-loaded zone. The template omits e.g. the xmodelalias scripts its
    character scripts call, which fails script linking at map load."""
    listed = {l.split(",", 1)[1].strip().lower() for l in zone_text.splitlines() if l.startswith("script,")}
    pending = list(listed)
    added: list[str] = []
    seen = set(listed)
    while pending:
        name = pending.pop()
        src = raw / name
        if not src.exists():
            continue
        text = src.read_text(encoding="utf-8", errors="replace")
        ext = ".csc" if name.endswith(".csc") else ".gsc"
        for ref in set(INCLUDE_RE.findall(text)) | set(CALL_RE.findall(text)):
            dep = ref.replace("\\", "/").lower() + ext
            if dep in seen:
                continue
            seen.add(dep)
            if dep in stock or not (raw / dep).exists():
                continue
            added.append(dep)
            pending.append(dep)
    return sorted(added)


MODEL_CALL_RE = re.compile(r'\b(?:precachemodel|setmodel|attach)\s*\(\s*"([^"]+)"', re.IGNORECASE)
ALIAS_ENTRY_RE = re.compile(r'^\s*\w+\s*\[\s*\d+\s*\]\s*=\s*"([^"]+)"\s*;', re.MULTILINE)


def character_models(zone_text: str, raw: Path) -> list[str]:
    """Models the zone's character/xmodelalias scripts put on AI at runtime
    but the zone does not list. The linker only follows static references, so
    a head picked from an xmodelalias array (e.g. c_zom_dlc0_zombie_hazmat_1 ->
    c_zom_dlc0_zom_head_als -> c_zom_dlc0_zom_head1..4) is otherwise missing
    and the zombie spawns headless (measured in game)."""
    listed = {l.split(",")[-1].strip().lower() for l in zone_text.splitlines() if l.startswith("xmodel,")}
    scripts = [l.split(",", 1)[1].strip() for l in zone_text.splitlines() if l.startswith("script,")]
    found: set[str] = set()
    for name in scripts:
        if not name.lower().startswith(("character/", "xmodelalias/")) or not name.endswith(".gsc"):
            continue
        src = raw / name
        if not src.exists():
            continue
        text = src.read_text(encoding="utf-8", errors="replace")
        found.update(MODEL_CALL_RE.findall(text))
        if name.lower().startswith("xmodelalias/"):
            found.update(ALIAS_ENTRY_RE.findall(text))
    return sorted(m for m in found if m.lower() not in listed and (raw / "xmodel" / f"{m}.json").exists())


FAILED_ENTRY_RE = re.compile(r'ERROR: Could not load asset "([^"]+)" of type "([^"]+)"')
MAX_DROPS = 200


def _drop_entry(zone_text: str, kind: str, name: str) -> tuple[str, str | None]:
    """Remove the zone line that pulled in the failing asset."""
    lines = zone_text.splitlines()
    for i, line in enumerate(lines):
        parts = [p.strip() for p in line.split(",")]
        if len(parts) >= 2 and parts[0] == kind and parts[-1].split("/")[-1] == name.split("/")[-1]:
            del lines[i]
            return "\n".join(lines) + "\n", line
    return zone_text, None


def stage_lobby_map_table(stock: Path, project_root: Path, project: str) -> Path:
    """Keep custom-map lobby metadata in mod.ff after the map zone unloads.

    GameGlobeZombie reads numeric longitude/latitude from columns 16/17.
    An unknown map returns empty strings, becomes nil in Lua, and crashes
    MoveToUpDirectly during the return to the lobby. A neutral globe position
    is frontend metadata, not a replacement for source map content.
    """
    rows = list(csv.reader(io.StringIO(stock.read_text(encoding="utf-8-sig"))))
    if not rows or len(rows[0]) < 20:
        raise ValueError("unsupported zombies map table schema")
    existing = next((row for row in rows if row and row[0] == project), None)
    if existing is None:
        defaults = next((row for row in rows if row and row[0] == "default"), None)
        if defaults is None:
            raise ValueError("zombies map table lacks default metadata")
        existing = defaults[:] + [""] * max(0, 20 - len(defaults))
        existing[0] = project
        existing[3] = project
        existing[19] = "top"
        rows.insert(next(i for i, row in enumerate(rows) if row and row[0] == "default"), existing)
    existing += [""] * max(0, 20 - len(existing))
    for column in (16, 17, 18):
        if not existing[column].strip():
            existing[column] = "0"
        try:
            value = float(existing[column])
        except ValueError as error:
            raise ValueError(f"invalid globe coordinate for {project}") from error
        if not math.isfinite(value):
            raise ValueError(f"nonfinite globe coordinate for {project}")
    maps = [row for row in rows[1:] if row and row[0] not in ("maxnum_map", "default")]
    count = next((row for row in rows if row and row[0] == "maxnum_map"), None)
    if count is None:
        raise ValueError("zombies map table lacks map count")
    count[1] = str(len(maps))
    if not existing[5].strip():
        existing[5] = str(maps.index(existing))
    result = project_root / "zm/mapstable.csv"
    result.parent.mkdir(parents=True, exist_ok=True)
    with result.open("w", encoding="utf-8", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)
    return result


def link_mod(bo2: Path, work: Path, unlinker: Path, extra_lines: list[str] = (),
             asset_root: Path | None = None, extra_asset_roots: list[Path] = (),
             linker: Path | None = None) -> tuple[Path, list[str]]:
    """Link mod.ff. Template entries whose raw assets are not on disk (DLC
    weapons etc. that All2Raw did not produce) are dropped one at a time and
    returned, so the caller can report exactly what the build lacks."""
    work = work.resolve()  # the linker runs from bo2\bin
    template = bo2 / "mods" / TEMPLATE_MAP / f"{TEMPLATE_MAP}.zone"
    dropped: list[str] = []
    zone_file = work / "zone_source" / "mod.zone"
    zone_file.parent.mkdir(parents=True, exist_ok=True)
    zone_text = build_mod_zone(template, dropped)
    closure = script_closure(zone_text, bo2 / "raw", stock_scripts(bo2, unlinker))
    if closure:
        zone_text += "// waw2bo2: scripts the template's scripts depend on\n" + \
                     "".join(f"script,{s}\n" for s in closure)
    models = character_models(zone_text, bo2 / "raw")
    if models:
        zone_text += "// waw2bo2: models the template's character scripts attach at runtime\n" + \
                     "".join(f"xmodel,{m}\n" for m in models)
    if extra_lines:
        zone_text += "// waw2bo2: assets the converted map needs from the mod tools' raw folder\n" + \
                     "".join(f"{l}\n" for l in extra_lines)
    (work / "added_dependencies.txt").write_text("\n".join(closure) + "\n", encoding="utf-8")
    linker = linker.resolve() if linker is not None else bo2 / "bin" / "Linker.exe"
    unavailable: list[str] = []
    protected = {line.strip() for line in extra_lines if line.strip() and not line.lstrip().startswith("//")}
    for _ in range(MAX_DROPS):
        zone_file.write_text(zone_text, encoding="utf-8")
        command = [str(linker), "--no-color", "--base-folder", str(bo2.resolve()), "--source-search-path", str(zone_file.parent),
                   "--add-asset-search-path", str(bo2 / "mods" / TEMPLATE_MAP)]
        if asset_root is not None:
            command += ["--add-asset-search-path", str(asset_root.resolve())]
        for root in extra_asset_roots:
            command += ["--add-asset-search-path", str(Path(root).resolve())]
        command += ["--output-folder", str(work / "out"), "mod"]
        proc = subprocess.run(command,
                              cwd=str(linker.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
        (work / "linker.log").write_text(proc.stdout, encoding="utf-8")
        # mod.ff carries no world; the map-BSP probe error is expected
        errors = [l for l in proc.stdout.splitlines() if "ERROR" in l and "Could not open BSP" not in l]
        ff = work / "out" / "mod.ff"
        if not proc.returncode and not errors and ff.exists():
            break
        failed = FAILED_ENTRY_RE.findall(proc.stdout)
        removed = None
        # the last failure is the top-level zone entry; earlier ones are its dependencies
        for name, kind in reversed(failed):
            candidate, removed = _drop_entry(zone_text, kind, name)
            if removed:
                if removed.strip() in protected:
                    raise RuntimeError(f"required converted asset cannot link: {removed}; "
                                       f"it will NOT be dropped; see {work / 'linker.log'}")
                zone_text = candidate
                unavailable.append(f"{removed}  <- {errors[0].removeprefix('ERROR: ')}")
                break
        if not removed:
            raise RuntimeError(f"mod.ff link failed ({proc.returncode}): {errors[:8]}; see {work / 'linker.log'}")
    else:
        raise RuntimeError(f"mod.ff link still failing after dropping {MAX_DROPS} template entries")
    (work / "dropped_from_template.txt").write_text(
        "# removed: belongs to the zm_test map itself\n" + "\n".join(dropped) +
        "\n\n# removed: raw assets not available on disk\n" + "\n".join(unavailable) + "\n", encoding="utf-8")
    return ff, unavailable
