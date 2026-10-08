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
from .progress import captured

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
    # Extraction only writes assets present in this zone. Old files must not
    # become dependencies or verification evidence for a subsequent build.
    for relative in ('shader_baseline/dump', 'shader_overlay', 'shader_check', 'shader_out'):
        directory = work / relative
        directory.resolve().relative_to(work)
        if directory.exists():
            shutil.rmtree(directory)
    baseline = work/'shader_baseline/mod.ff'
    baseline.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ff, baseline)
    dump = work/'shader_baseline/dump'
    proc = captured([str(unlinker.resolve()), '--no-color', '--include-assets', 'material',
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
        for folder in ('techniquesets', 'shader_bin', 'english/localizedstrings'):
            if (root/folder).is_dir():
                shutil.copytree(root/folder, overlay/folder, dirs_exist_ok=True)
    # Stringtables must also override loaded/stock assets. The linker's
    # independent search paths are sorted, so adding a project path beside
    # BO2's raw path is insufficient to guarantee source priority.
    for name in ('mapstable.csv', 'gametypestable.csv'):
        source = project / 'zm' / name
        if source.is_file():
            target = overlay / 'zm' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
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
    proc = captured(command, capture_output=True, text=True, errors='replace')
    (work/'shader_linker.log').write_text(proc.stdout+proc.stderr)
    linked = output/'mod.ff'
    if proc.returncode or not linked.is_file() or re.search(r'(?m)^ERROR:', proc.stdout):
        raise RuntimeError(f'bound shader mod relink failed ({proc.returncode}); see {work / "shader_linker.log"}')
    check = work/'shader_check'
    proc = captured([str(unlinker.resolve()), '--no-color', '--include-assets', 'material,techniqueset',
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


def stock_scripts(bo2: Path, unlinker: Path, stock_dump: Path | None = None) -> set[str]:
    names: set[str] = set()
    if stock_dump is not None and (stock_dump / 'catalog.json').is_file():
        catalog = json.loads((stock_dump / 'catalog.json').read_text(encoding='utf-8'))
        for filename, entry in catalog['zones'].items():
            if Path(filename).stem in STOCK_SCRIPT_ZONES:
                names.update(n.lower() for n in entry.get('loaded_index', entry['index']).get('script', []))
        return names
    for zone in STOCK_SCRIPT_ZONES:
        ff = bo2 / "zone" / "all" / f"{zone}.ff"
        proc = captured([str(unlinker), "--no-color", "--list", "--search-path", str(ff.parent), str(ff)],
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


def menu_metadata(project_root: Path) -> dict:
    """Optional authored frontend assets; independent of source map names."""
    path = project_root / 'menu.json'
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding='utf-8'))
    for field in ('title', 'description'):
        if not isinstance(data.get(field), str) or not data[field].strip():
            raise ValueError(f'menu.json requires {field}')
    if 'icon' in data or 'blit' in data:
        for field in ('icon', 'blit'):
            if not isinstance(data.get(field), str) or not data[field].strip():
                raise ValueError(f'menu.json requires {field}')
    return data


def stage_menu_assets(project_root: Path, project: str) -> list[str]:
    """Localize authored branding and include its materials in both zones."""
    data = menu_metadata(project_root)
    if not data:
        return []
    from .localization import quote
    prefix = 'WAW_MENU_' + project.upper()
    asset = 'menu_' + project
    path = project_root / 'english/localizedstrings' / (asset + '.str')
    path.parent.mkdir(parents=True, exist_ok=True)
    values = {'TITLE': data['title'], 'CAPS': data['title'].upper(), 'DESC': data['description']}
    path.write_text('VERSION "1"\nCONFIG ""\nFILENOTES "Authored map menu"\n\n' +
                    ''.join(f'REFERENCE {prefix}_{key}\nLANG_ENGLISH {quote(value)}\n\n'
                            for key, value in values.items()) + 'ENDMARKER\n', encoding='utf-8')
    if not data.get('icon'):
        return [f'localize,{asset}']
    names = list(dict.fromkeys([data['icon'], data['blit'],
        f'menu_{project}_map', f'menu_{project}_map_blur', f'menu_{project}_zclassic_default',
        f'loadscreen_{project}_zclassic_default', f'loadscreen_{project}_zclassic_', *data.get('materials', [])]))
    streaming_file = project_root / 'images/streaming.json'
    streaming = json.loads(streaming_file.read_text()) if streaming_file.is_file() else {'streamingMode': {}}
    for name in names:
        material = project_root / 'materials' / (name + '.json')
        if not material.is_file():
            raise FileNotFoundError(f'authored menu material missing: {material}')
        for texture in json.loads(material.read_text())['textures']:
            if not (project_root / 'images' / (texture['image'] + '.iwi')).is_file():
                raise FileNotFoundError(f'authored menu image missing: {texture["image"]}')
            streaming['streamingMode'][texture['image']] = 2
    streaming_file.write_text(json.dumps(streaming, indent=2) + '\n', encoding='utf-8')
    pack = data.get('image_pack', project + '_menu')
    return [f'>level.ipak_read,{pack}', f'>ipak,{pack}', f'localize,{asset}'] + [f'material,{name}' for name in names]


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
    # The frontend Lua reads every column of the current map's row; an empty
    # one comes back nil (returning to the main menu failed in MainMenuOG.lua:8
    # "attempt to index a nil value" on the converted map, not on stock maps).
    # Empty columns take the base-game map's values: content index 0 (column
    # 11), so the custom map needs no DLC, with valid image/size/faction fields.
    base = next((row for row in rows[1:] if row and row[0] not in ("maxnum_map", "default", project)
                 and len(row) > 11 and row[11].strip() == "0"), None)
    if base is None:
        raise ValueError("zombies map table lacks a base-game map row")
    for column in range(1, 20):
        if column not in (3, 5, 16, 17, 18) and not existing[column].strip():
            existing[column] = base[column]
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
    metadata = menu_metadata(project_root)
    if metadata:
        existing[3] = 'WAW_MENU_' + project.upper() + '_TITLE'
        existing[4] = metadata.get('icon', existing[4])
        existing[6] = 'WAW_MENU_' + project.upper() + '_DESC'
    result = project_root / "zm/mapstable.csv"
    result.parent.mkdir(parents=True, exist_ok=True)
    with result.open("w", encoding="utf-8", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)
    return result


def stage_lobby_gametype_table(stock: Path, project_root: Path, project: str) -> Path:
    """Register one complete WaW play area with the stock four-player co-op mode."""
    rows = list(csv.reader(io.StringIO(stock.read_text(encoding="utf-8-sig"))))
    if not rows or len(rows[0]) < 23:
        raise ValueError("unsupported zombies gametype table schema")
    # Category 5 describes selectable locations; category 6 connects those
    # locations to modes. The YES entry also supplies the lobby defaults.
    for category, count_key in (("5", "maxnum_startloc"), ("6", "startloc_gamemode_map")):
        entries = [r for r in rows if r and r[0] == category]
        count = next((r for r in rows if r and r[0] == count_key), None)
        if count is None:
            raise ValueError(f"zombies gametype table lacks {count_key}")
        if not any(len(r) > 2 and r[2] == project for r in entries):
            index = str(max((int(r[1]) for r in entries), default=-1) + 1)
            if category == "5":
                entry = ["5", index, project, "default", project, project,
                         "menu_zm_map_zombie_dot", "1", "0", "0", "180", "-50",
                         "", "", "", "", project, "100", "-90", "", "", "", ""]
            else:
                entry = ["6", index, project, "default", "zclassic", "0", "left", "YES"] + [""] * 15
            rows.insert(rows.index(entries[-1]) + 1 if entries else rows.index(count) + 1, entry)
        entries = [r for r in rows if r and r[0] == category]
        # Engine iteration uses these bounds, not the physical CSV row count.
        count[1] = str(max((int(r[1]) for r in entries), default=-1) + 1)
    metadata = menu_metadata(project_root)
    if metadata:
        entry = next(r for r in rows if len(r) > 2 and r[0] == '5' and r[2] == project)
        prefix = 'WAW_MENU_' + project.upper()
        entry[4], entry[5], entry[6], entry[16] = prefix + '_CAPS', prefix + '_DESC', metadata.get('blit', entry[6]), prefix + '_TITLE'
    result = project_root / "zm/gametypestable.csv"
    result.parent.mkdir(parents=True, exist_ok=True)
    with result.open("w", encoding="utf-8", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)
    return result


def link_lobby(bo2: Path, project_root: Path, project: str, work: Path,
               linker: Path, stock: Path | None = None) -> Path:
    """Build frontend-only mod_load.ff so selection works before loading a map.

    Authored menu.json metadata uses the same materials and localized strings
    in the frontend and gameplay zones. Unbranded projects retain the empty
    stock frame and their technical name.
    """
    stage_lobby_map_table(bo2 / "raw/zm/mapstable.csv", project_root, project)
    stage_lobby_gametype_table(bo2 / "raw/zm/gametypestable.csv", project_root, project)
    root = work.resolve() / "lobby"
    materials = root / "assets/materials"
    materials.mkdir(parents=True, exist_ok=True)
    frame = bo2 / "raw/materials/menu_zm_map_frame.json"
    names = [f"menu_{project}_map", f"menu_{project}_map_blur",
             f"menu_{project}_zclassic_default", f"loadscreen_{project}_zclassic_default",
             f"loadscreen_{project}_zclassic_"]
    authored = stage_menu_assets(project_root, project)
    for name in names:
        if menu_metadata(project_root).get('icon'):
            if not (project_root / 'materials' / f'{name}.json').is_file():
                raise FileNotFoundError(f'authored menu backdrop missing: {name}')
        else:
            shutil.copy2(frame, materials / f"{name}.json")
    source = root / "zone_source"
    source.mkdir(parents=True, exist_ok=True)
    (source / "mod_load.zone").write_text(
        ">game,T6\nstringtable,zm/mapstable.csv\nstringtable,zm/gametypestable.csv\n"
        "techniqueset,,trivial_9z33feqw\nimage,,menu_zm_map_frame\n" +
        "".join(f"{entry}\n" for entry in dict.fromkeys([*authored, *(f'material,{name}' for name in names)])), encoding="utf-8")
    out = work.resolve() / "out"
    command = [str(linker.resolve()), "--no-color", "--base-folder", str(project_root.resolve()),
               "--source-search-path", str(source), "--asset-search-path", "?base?",
               "--add-asset-search-path", str(root / "assets"),
               "--add-asset-search-path", str(bo2.resolve() / "raw")]
    if stock:
        command += ["--add-asset-search-path", str(stock.resolve())]
    command += ["--output-folder", str(out), "mod_load"]
    proc = captured(command, capture_output=True, text=True, errors="replace")
    (root / "linker.log").write_text(proc.stdout + proc.stderr, encoding="utf-8")
    result = out / "mod_load.ff"
    if proc.returncode or not result.is_file() or re.search(r"(?m)^(?:ERROR:|Missing asset|Failed to load|Could not load)", proc.stdout):
        raise RuntimeError(f"lobby zone link failed; see {root / 'linker.log'}")
    metadata = menu_metadata(project_root)
    if metadata:
        (out / 'menu_metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    else:
        (out / 'menu_metadata.json').unlink(missing_ok=True)
    return result


def verify_lobby_tables(ff: Path, unlinker: Path, work: Path, project: str) -> None:
    """Reject builds where stock search-path precedence lost our registrations."""
    dump = work.resolve() / f"verify_{ff.stem}_lobby"
    if dump.exists():
        dump.relative_to(work.resolve())
        shutil.rmtree(dump)
    proc = captured([str(unlinker.resolve()), '--no-color', '--include-assets', 'stringtable',
                           '--output-folder', str(dump), str(ff.resolve())],
                          capture_output=True, text=True, errors='replace')
    (work.resolve() / f"verify_{ff.stem}_lobby.log").write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError(f"cannot verify lobby tables in {ff}")
    for name in ('mapstable.csv', 'gametypestable.csv'):
        path = dump / 'zm' / name
        if not path.is_file():
            raise RuntimeError(f"missing lobby table {name} in {ff}")
        with path.open(encoding='utf-8-sig', newline='') as stream:
            rows = list(csv.reader(stream))
        if name == 'mapstable.csv':
            valid = any(row and row[0] == project for row in rows)
        else:
            valid = all(any(len(row) > 7 and row[0] == category and row[2] == project
                            and row[3] == 'default' and
                            (category == '5' or (row[4] == 'zclassic' and row[7] == 'YES'))
                            for row in rows) for category in ('5', '6'))
        if not valid:
            raise RuntimeError(f"custom lobby registration missing from {name} in {ff}")


def recover_ipak_images(log: str, roots: list[Path], output: Path,
                        stock: Path | None, unlinker: Path) -> list[dict]:
    """Stage the exact images requested by the native ipak writer as IWI27."""
    from . import all2raw, iwi, techsets
    from .weapons import output_name
    recovered = []
    for name in sorted(set(re.findall(r'Failed to open file for ipak: images/(.+)\.iwi', log))):
        output_name('image', name)
        relative = techsets.oat_image_path(name)
        destination = output / relative
        if destination.is_file():
            continue
        source = next((root / relative for root in roots if (root / relative).is_file()), None)
        if source is None:
            source = next((root / relative.with_suffix('.dds') for root in roots
                           if (root / relative.with_suffix('.dds')).is_file()), None)
        if source is None and stock is not None:
            source = all2raw.required_bo2_image(stock, name, unlinker)
        if source is None:
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix.lower() == '.dds':
            iwi.convert_file(source, destination)
        else:
            if source.read_bytes()[:4] != b'IWi\x1b':
                raise ValueError(f'ipak image {name}: expected BO2 IWI27: {source}')
            shutil.copy2(source, destination)
        recovered.append({'image': name, 'source': str(source), 'output': str(destination)})
        print(f'Prepared required ipak image {name} from {source}', flush=True)
    return recovered


def link_mod(bo2: Path, work: Path, unlinker: Path, extra_lines: list[str] = (),
             asset_root: Path | None = None, extra_asset_roots: list[Path] = (),
             linker: Path | None = None, stock_dump: Path | None = None) -> tuple[Path, list[str]]:
    """Link mod.ff. Template entries whose raw assets are not on disk (DLC
    weapons etc. that All2Raw did not produce) are dropped one at a time and
    returned, so the caller can report exactly what the build lacks."""
    work = work.resolve()  # the linker runs from bo2\bin
    template = bo2 / "mods" / TEMPLATE_MAP / f"{TEMPLATE_MAP}.zone"
    dropped: list[str] = []
    zone_file = work / "zone_source" / "mod.zone"
    zone_file.parent.mkdir(parents=True, exist_ok=True)
    zone_text = build_mod_zone(template, dropped)
    closure = script_closure(zone_text, bo2 / "raw", stock_scripts(bo2, unlinker, stock_dump))
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
    image_root = work / 'ipak_images'
    recovered_images: list[dict] = []
    protected = {line.strip() for line in extra_lines if line.strip() and not line.lstrip().startswith("//")}
    table_override = work / 'lobby_input'
    if asset_root is not None:
        for name in ('mapstable.csv', 'gametypestable.csv'):
            source = asset_root / 'zm' / name
            if source.is_file():
                target = table_override / 'zm' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
    for _ in range(MAX_DROPS):
        zone_file.write_text(zone_text, encoding="utf-8")
        command = [str(linker), "--no-color", "--base-folder", str(bo2.resolve()), "--source-search-path", str(zone_file.parent),
                   "--add-asset-search-path", str(bo2 / "mods" / TEMPLATE_MAP)]
        if asset_root is not None:
            command += ["--add-asset-search-path", str(asset_root.resolve())]
            if table_override.is_dir():
                # ?base? sorts before absolute stock paths. Limit this override
                # layer to frontend tables so gameplay search order is retained.
                command[command.index("--base-folder") + 1] = str(table_override)
                command += ["--asset-search-path", "?base?;?bin?/../raw",
                            "--add-asset-search-path", str(bo2.resolve() / "raw")]
        for root in extra_asset_roots:
            command += ["--add-asset-search-path", str(Path(root).resolve())]
        command += ["--add-asset-search-path", str(image_root)]
        command += ["--output-folder", str(work / "out"), "mod"]
        proc = captured(command,
                              cwd=str(linker.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
        (work / "linker.log").write_text(proc.stdout, encoding="utf-8")
        # mod.ff carries no world; the map-BSP probe error is expected
        errors = [l for l in proc.stdout.splitlines() if "ERROR" in l and "Could not open BSP" not in l]
        ff = work / "out" / "mod.ff"
        if not proc.returncode and not errors and ff.exists():
            break
        recovered = recover_ipak_images(proc.stdout,
            [*([asset_root] if asset_root is not None else []), *extra_asset_roots,
             bo2 / 'mods' / TEMPLATE_MAP, bo2 / 'raw'], image_root, stock_dump, unlinker)
        if recovered:
            recovered_images.extend(recovered)
            (work / 'ipak_images.report.json').write_text(json.dumps(recovered_images, indent=2) + '\n')
            continue
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
