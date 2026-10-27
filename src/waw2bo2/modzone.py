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
import hashlib
import math
import io
import re
import subprocess
import json
import shutil
from contextlib import contextmanager
from pathlib import Path
from . import timeline
from .progress import captured

TEMPLATE_MAP = "zm_test"
# its sound bank; not shipped (every sound comes from WaW)
TEMPLATE_SOUNDBANK = "zmb_test.all"
# world assets live in the map fastfile (the bridge links skinnedverts too;
# the engine allows exactly one loaded)
BSP_TYPES = {"clipmap", "clipmap_pvs", "comworld", "gameworldsp", "gameworldmp", "mapents", "gfxworld",
             "skinnedverts"}
# referenced by the template but not produced by All2Raw from any shipped zone
KNOWN_ABSENT = {("xmodel", "viewmodel_base_viewhands")}

# Native runtime equipment remains available when the map owns the gun lists.
# These are used by BO2's player, buildable and melee controllers rather than
# the template's mystery box. Converted WaW weapons arrive as protected extras.
RUNTIME_WEAPONS = {"defaultweapon_mp", "death_self_zm", "death_throe_zm",
                   "zombie_fists_zm", "zombie_melee", "zombie_builder_zm",
                   "buildable_piece_zm", "syrette_zm", "zombie_bowie_flourish",
                   "zombie_tazer_flourish", "knife_zm", "frag_grenade_zm",
                   "m1911_zm", "m1911_upgraded_zm", "cymbal_monkey_zm",
                   "claymore_zm", "bowie_knife_zm", "tazer_knuckles_zm",
                   "knife_ballistic_zm", "knife_ballistic_upgraded_zm",
                   "knife_ballistic_bowie_zm", "knife_ballistic_bowie_upgraded_zm",
                   "knife_ballistic_no_melee_zm", "knife_ballistic_no_melee_upgraded_zm"}


def prune_template_weapons(zone: str, project: Path, bo2: Path, dropped: list[str]) -> str:
    """Keep native runtime weapons and map references, not the unused BO2 box.

    T6 uses 8-bit runtime weapon indices. Loading the entire template arsenal
    alongside a WaW arsenal can overflow its registry before players spawn.
    Only template entries are removed; converted weapons remain protected.
    """
    from . import gsc, weapons
    if not (project / 'content_source/weapons.runtime.json').is_file():
        return zone
    needed = set(RUNTIME_WEAPONS)
    for side, ext in (("maps", "gsc"), ("clientscripts", "csc")):
        for source in (project / side).rglob(f'*.{ext}'):
            script = gsc.parse(source.read_text(encoding='utf-8'), str(source))
            refs = gsc.references(script.tokens)
            definitions = {fn.start for fn in script.functions.values()}
            local_refs = {ref.name.lower() for ref in refs if ref.qualifier is None and ref.index not in definitions}
            ignored = set()
            # hook_bo2_weapons replaces these template entry points. Their
            # unused bodies still name every BO2 gun and are not dependencies.
            if source.parent == project / side / 'mp':
                for name in ('include_weapons', 'custom_add_weapons'):
                    if name in script.functions and (name not in local_refs or side == 'clientscripts' and name == 'include_weapons'):
                        fn = script.functions[name]
                        ignored.update(range(fn.start, fn.body_close + 1))
            needed.update(t.text.lstrip('&#').strip('"') for i, t in enumerate(script.tokens)
                          if t.kind == gsc.STRING and i not in ignored)
    # A retained stock weapon can bring an alternate or a left-hand weapon.
    queue, seen = list(needed), set()
    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)
        source = next((root / 'weapons' / prefix / name for root in
                       (project / 'content_source', bo2 / 'raw') for prefix in ('', 'mp', 'sp')
                       if (root / 'weapons' / prefix / name).is_file()), None)
        if source is None:
            continue
        fields = weapons.read_info_file(source)
        for key in ('altWeapon', 'DualWieldWeapon', 'grenadeWeapon', 'parentWeaponName'):
            dependency = fields.get(key)
            if dependency:
                needed.add(dependency)
                queue.append(dependency)
    output, removed = [], []
    for line in zone.splitlines():
        if line.strip().startswith('weapon,') and line.split(',')[-1].strip().split('/')[-1] not in needed:
            dropped.append(line.strip())
            removed.append(line.strip())
        else:
            output.append(line)
    (project / 'content_source/template_weapons.json').write_text(json.dumps(
        {'removed': removed, 'required': sorted(needed)}, indent=2), encoding='utf-8')
    # Client template registration previously retained its entire BO2 arsenal
    # even after the server hooks switched to WaW. Use the same retained native
    # set and converted weapon names on both sides.
    runtime = json.loads((project / 'content_source/weapons.runtime.json').read_text())
    carried = {runtime.get('table', {}).get(name, name) for name in runtime.get('weapons', [])}
    native = {line.split(',')[-1].strip().split('/')[-1] for line in output if line.strip().startswith('weapon,')}
    # The engine also automatically registers weapons from this stats table,
    # including guns available through BO2's parent zones/raw lookup. Pruning
    # zone entries alone cannot remove those registrations. Keep row indices
    # and names stable, but leave unused weapon rows inactive like stock gaps.
    stats_source = bo2 / 'raw/zm/zm_statstable.csv'
    if stats_source.is_file():
        with stats_source.open(newline='', encoding='utf-8') as stream:
            rows = list(csv.reader(stream))
        inactive = []
        for row in rows[1:]:
            if len(row) > 4 and row[2].startswith('weapon_') and row[4] + '_zm' not in native | carried:
                inactive.append(row[0])
                row[2] = ''
        destination = project / 'zm/zm_statstable.csv'
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('w', newline='', encoding='utf-8') as stream:
            csv.writer(stream).writerows(rows)
        (project / 'content_source/weapon_stats.json').write_text(json.dumps(
            {'inactive_weapon_rows': inactive, 'rows': len(rows)}, indent=2), encoding='utf-8')
        output.append('stringtable,zm/zm_statstable.csv')
    for source in (project / 'clientscripts/mp').glob('*.csc'):
        script = gsc.parse(source.read_text(encoding='utf-8'), str(source))
        fn = script.functions.get('include_weapons')
        if fn is None:
            continue
        script.tokens[fn.body_open].text = '{\n' + ''.join(
            f'    include_weapon( "{name}", 0 );\n' for name in sorted(native | carried) if name) + '}'
        for token in script.tokens[fn.body_open + 1:fn.body_close + 1]:
            token.pre = token.text = ''
        source.write_text(gsc.emit(script.tokens), encoding='utf-8')
    return '\n'.join(output) + '\n'


def staged_materials(project: Path) -> list[tuple[Path, dict]]:
    """(root, report entry) for every material THIS build staged. The stage
    folder is reused across builds, so files on disk alone may be stale."""
    report_file = project / 'bridge_stage.report.json'
    entries = [(project, e) for e in json.loads(report_file.read_text())['materials']] if report_file.exists() else []
    weapon_report = project/'content_source/weapons.visuals.json'
    if weapon_report.exists():
        entries += [(project/'content_source', e) for e in json.loads(weapon_report.read_text())['materials']]
    return entries


@contextmanager
def baseline_shader_materials(project: Path, stock: Path):
    """Give the raw mod-tools linker its stock techniques, restoring staging afterward."""
    entries = staged_materials(project)
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
    global asset pools reuse the complete baseline instead. Bound materials,
    shader dependencies and weapons requiring nested flame tables are exposed
    as fresh inputs; the other gameplay and image
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
    (work/'shader_baseline/unlinker.log').write_text(proc.stdout+proc.stderr, encoding='utf-8')
    if proc.returncode:
        raise RuntimeError('shader baseline material extraction failed')
    overlay = work/'shader_overlay'
    # The mod-tools linker preserves flame-table names but leaves the nested
    # pointers empty. Recreate those weapons with our measured native loader
    # during the final relink, retaining all baseline assets as dependencies.
    flame_stage = project / 'content_source/weapons.stage.json'
    if flame_stage.is_file():
        data = json.loads(flame_stage.read_text())
        content = project / 'content_source'
        for entry in data.get('flame_tables', []):
            source = content / entry['file']
            target = overlay / entry['file']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        for entry in data.get('weapons', []):
            if not entry.get('dependencies', {}).get('flametable'):
                continue
            source = content / 'weapons' / entry['name']
            target = overlay / 'weapons' / entry['name']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    bound = set()
    current_files = {(root/entry['file']).resolve() for root, entry in staged_materials(project)}
    for root in (project, project/'content_source'):
        for source in (root/'materials').rglob('*.json'):
            if source.resolve() not in current_files:
                # left by an earlier build; its runtime technique may name
                # shaders the current stock dump does not have
                continue
            current = json.loads(source.read_text())
            if not current.get('techniqueSet', '').startswith('waw/runtime_'):
                continue
            relative = source.relative_to(root)
            extracted = dump/relative
            if not extracted.is_file():
                # This material belongs to the world zone, not gameplay mod.ff.
                continue
            material = json.loads(extracted.read_text())
            slots = lambda m: {t.get('name') for t in m.get('textures', [])}
            if not slots(current) <= slots(material):
                # Same name, different asset: mod.ff holds BO2's own material
                # (e.g. the box's mtl_prop_bear), which lacks the WaW slots
                # the runtime technique reads. Leave the stock material alone.
                continue
            material['techniqueSet'] = current['techniqueSet']
            target = overlay/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(material), encoding='utf-8')
            bound.add(relative.as_posix())
    with timeline.span('copy shader overlay'):
        for root in (stock, project, project/'content_source'):
            for folder in ('techniquesets', 'shader_bin', 'english/localizedstrings'):
                if (root/folder).is_dir():
                    shutil.copytree(root/folder, overlay/folder, dirs_exist_ok=True)
    # Stringtables must also override loaded/stock assets. The linker's
    # independent search paths are sorted, so adding a project path beside
    # BO2's raw path is insufficient to guarantee source priority.
    for name in ('mapstable.csv', 'gametypestable.csv', 'zm_statstable.csv', 'waw_menu_layers.csv', 'waw_menu_preview.csv'):
        source = project / 'zm' / name
        if source.is_file():
            target = overlay / 'zm' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    # Explicit project rawfiles must override both stock disk inputs and the
    # loaded baseline (notably the corrected stock overheat HUD bytecode).
    for source in [*(project / 'ui_mp').rglob('*.lua'), *(project / 'ui').rglob('*.lua')]:
        target = overlay / source.relative_to(project)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    zone = (work/'zone_source/mod.zone').read_text(encoding='utf-8')
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
    (source_root/'mod.zone').write_text(zone, encoding='utf-8')
    output = work/'shader_out'
    command = [str(linker.resolve()), '--no-color', '--base-folder', str(work),
               '--load', str(baseline), '--source-search-path', str(source_root),
               '--asset-search-path', str(overlay), '--output-folder', str(output), 'mod']
    proc = captured(command, capture_output=True, text=True, errors='replace')
    (work/'shader_linker.log').write_text(proc.stdout+proc.stderr, encoding='utf-8')
    linked = output/'mod.ff'
    if proc.returncode or not linked.is_file() or re.search(r'(?m)^ERROR:', proc.stdout):
        raise RuntimeError(f'bound shader mod relink failed ({proc.returncode}); see {work / "shader_linker.log"}')
    check = work/'shader_check'
    proc = captured([str(unlinker.resolve()), '--no-color', '--include-assets', 'material,techniqueset',
        '--output-folder', str(check), str(linked)], capture_output=True, text=True, errors='replace')
    (work/'shader_check.log').write_text(proc.stdout+proc.stderr, encoding='utf-8')
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
            # the template map's BO2 sounds: the map bank carries WaW's under
            # the names T6 plays (sounds.engine_name_copies)
            if kind == "soundbank" and name == TEMPLATE_SOUNDBANK:
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


def open_mod_ipak_first(zone_text: str, extra_lines: list[str]) -> str:
    """Start the mod ipak range right after the template header.

    The ipak writer only stores images linked while inside an ``>ipak`` range,
    yet template entries (stock weapons) pull images in as dependencies before
    the converter's own range. Those images are linked as streamed parts keyed
    by their re-dumped data hash, which no stock ipak holds (stock
    ``side_small`` sits in base.ipak under another hash), so the game draws an
    empty texture: the missing crosshair on every converted map."""
    pack = next((l.strip().split(",", 1)[1] for l in extra_lines if l.strip().startswith(">ipak,")), None)
    if pack is None:
        return zone_text
    lines = zone_text.splitlines()
    header = 0
    while header < len(lines) and (not lines[header].strip() or lines[header].lstrip().startswith(("//", ">"))):
        header += 1
    lines[header:header] = [f">level.ipak_read,{pack}", f">ipak,{pack}"]
    return "\n".join(lines) + "\n"


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


LOADED_WEAPON_RE = re.compile(r'^Loaded weapon "([^"]+)"', re.MULTILINE)


def loaded_weapons(work: Path) -> set[str]:
    """Weapons the successful mod.ff link loaded, its dependencies included
    (alt-mode and dual-wield weapons the zone does not list)."""
    log = work.resolve() / "linker.log"
    return set(LOADED_WEAPON_RE.findall(log.read_text(encoding="utf-8", errors="replace"))) if log.is_file() else set()


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
    from .mapmenu import coordinate
    for field, limit in (('longitude', 180), ('latitude', 90)):
        if field in data:
            coordinate(data[field], field.title(), limit)
    return data


DEFAULT_MENU_MODE = 'Zombies'
DEFAULT_MENU_LOCATION = 'World at War'


def menu_strings(project_root: Path, project: str) -> dict[str, str]:
    """Title, description, mode name and location; unbranded maps get defaults."""
    data = menu_metadata(project_root)
    return {'title': data.get('title') or project,
            'description': data.get('description') or f'{project}: World at War custom map',
            'mode': (data.get('mode') or '').strip() or DEFAULT_MENU_MODE,
            'location': (data.get('location') or '').strip() or DEFAULT_MENU_LOCATION}


def map_name_key(project: str) -> str:
    """mapstable column 3: a key prefix, never display text.

    T6 localizes "<col3>_CAPS" as the map name (ls_mapname) and "<col3>_LOC" as
    its location (ls_maplocation), both formatted into 32-byte buffers
    (sub_5D0EF0, sub_5E6900). A title there showed as "Title_CAPS"/"Title_LOC".
    """
    key = 'WAW_' + re.sub(r'\W', '_', project.upper())
    if len(key) + len('_CAPS') > 31:
        key = 'WAW_' + hashlib.sha1(project.encode()).hexdigest()[:16].upper()
    return key


def gametype_name_keys(project_root: Path, project: str) -> list[str]:
    """Mode-name keys T6 tries before the gametype's own name ("TRANZIT").

    sub_5581A0 (lobby) localizes "<gametype col7>_<map>_CAPS" and sub_694EB0
    (loading screen) "<gametype col7>_<col3>_LOC_CAPS"; stock pairs are
    ZMUI_CLASSIC_ZM_TOMB_CAPS and ZMUI_CLASSIC_ZMUI_BURIED_LOC_CAPS.
    """
    table = project_root / 'zm/gametypestable.csv'
    if not table.is_file():
        return []
    rows = list(csv.reader(io.StringIO(table.read_text(encoding='utf-8-sig'))))
    modes = {r[4] for r in rows if len(r) > 4 and r[0] == '6' and r[2] == project}
    prefixes = sorted({r[7] for r in rows if len(r) > 7 and r[0] == '0' and r[1] in modes and r[7]})
    return [key for prefix in prefixes
            for key in (f'{prefix}_{project.upper()}_CAPS', f'{prefix}_{map_name_key(project)}_LOC_CAPS')]


def start_location_option_strings(project_root: Path, project: str, strings: dict[str, str]) -> dict[str, str]:
    """The mode list SelectStartLocZombie.lua builds for a start location.

    Each option is "ZMUI_<mode ref>_<map>_CAPS" over "<gametype col7>_DESC_<location
    ref>" (stock: ZMUI_ZCLASSIC_ZM_BURIED_CAPS, ZMUI_CLASSIC_DESC_PROCESSING; the
    uncapitalized ZMUI_ZCLASSIC_ZM_BURIED too). Without them the raw keys showed.
    """
    table = project_root / 'zm/gametypestable.csv'
    if not table.is_file():
        return {}
    rows = list(csv.reader(io.StringIO(table.read_text(encoding='utf-8-sig'))))
    prefix = {r[1]: r[7] for r in rows if len(r) > 7 and r[0] == '0' and r[7]}
    values = {}
    for r in rows:
        if len(r) > 4 and r[0] == '6' and r[2] == project:
            location, mode = r[3], r[4]
            values[f'ZMUI_{mode}_{project}_CAPS'.upper()] = strings['mode'].upper()
            values[f'ZMUI_{mode}_{project}'.upper()] = strings['mode']
            if mode in prefix:
                values[f'{prefix[mode]}_DESC_{location}'.upper()] = strings['description']
    return values


def stage_menu_assets(project_root: Path, project: str) -> list[str]:
    """Localize the map's frontend strings and include authored materials in both zones."""
    data = menu_metadata(project_root)
    strings = menu_strings(project_root, project)
    from .localization import quote
    prefix = 'WAW_MENU_' + project.upper()
    name = map_name_key(project)
    asset = 'menu_' + project
    path = project_root / 'english/localizedstrings' / (asset + '.str')
    path.parent.mkdir(parents=True, exist_ok=True)
    values = {f'{prefix}_TITLE': strings['title'], f'{prefix}_CAPS': strings['title'].upper(),
              f'{prefix}_DESC': strings['description'], name: strings['title'],
              f'{name}_CAPS': strings['title'].upper(), f'{name}_LOC': strings['location']}
    values.update((key, strings['mode'].upper()) for key in gametype_name_keys(project_root, project))
    values.update(start_location_option_strings(project_root, project, strings))
    path.write_text('VERSION "1"\nCONFIG ""\nFILENOTES "Converted map menu"\n\n' +
                    ''.join(f'REFERENCE {key}\nLANG_ENGLISH {quote(value)}\n\n'
                            for key, value in values.items()) + 'ENDMARKER\n', encoding='utf-8')
    assets = [f'localize,{asset}']
    if data.get('loading_song'):
        bank = data['loading_song']['bank']
        from .weapons import output_name
        output_name('loading sound bank', bank)
        if not (project_root / 'soundbank' / (bank + '.aliases.csv')).is_file():
            raise FileNotFoundError(f'loading song bank missing: {bank}')
        assets.append(f'soundbank,{bank}')
    if not data.get('icon'):
        return assets
    names = list(dict.fromkeys([data['icon'], data['blit'],
        f'menu_{project}_map', f'menu_{project}_map_blur', f'menu_{project}_zclassic_default',
        f'loadscreen_{project}_zclassic_default', f'loadscreen_{project}_zclassic_', *data.get('materials', []), *data.get('layer_materials', [])]))
    if data.get('layer_layout') == 'aligned':
        from .menulayers import RUNTIME, BASE, TABLE
        for name in (BASE, RUNTIME, TABLE):
            if not (project_root / name).is_file():
                raise FileNotFoundError(f'aligned menu runtime missing: {name}')
        assets += [f'rawfile,{BASE}', f'rawfile,{RUNTIME}', f'stringtable,{TABLE}']
    if data.get('preview_layout') == 'full':
        from . import menupreview
        for name in (menupreview.BASE, menupreview.RUNTIME, menupreview.TABLE):
            if not (project_root / name).is_file():
                raise FileNotFoundError(f'full-frame lobby preview runtime missing: {name}')
        assets += [f'rawfile,{menupreview.BASE}', f'rawfile,{menupreview.RUNTIME}', f'stringtable,{menupreview.TABLE}']
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
    return [f'>level.ipak_read,{pack}', f'>ipak,{pack}', *assets] + [f'material,{name}' for name in names]


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
    # Strings come from stage_menu_assets' localize asset, for every map.
    existing[3] = map_name_key(project)
    metadata = menu_metadata(project_root)
    existing[4] = metadata.get('icon', existing[4])
    for column, field in ((16, 'longitude'), (17, 'latitude')):
        if field in metadata:
            existing[column] = str(metadata[field])
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
    entry = next(r for r in rows if len(r) > 2 and r[0] == '5' and r[2] == project)
    # SelectStartLocZombie.lua reads a location's columns with TableLookup(0, 5,
    # 3, <location>, n): category and location name only, first row wins. The
    # stock test maps zm_bus_test and zm_meat come first with location
    # "default", so the converted map showed their image (playlist_ffa, the
    # checkerboard) at their corner position. Put the map's row ahead of them.
    rows.remove(entry)
    rows.insert(next(i for i, r in enumerate(rows) if r and r[0] == '5'), entry)
    # File order alone did not win in game (the box stayed at zm_bus_test's
    # -320,-240): give those unlisted test maps their own location name so the
    # converted map holds the only category 5 row with its location.
    clashing = {r[2] for r in rows if len(r) > 3 and r[0] == '5' and r[3] == entry[3] and r[2] != project}
    for r in rows:
        if len(r) > 3 and r[0] in ('5', '6') and r[2] in clashing and r[3] == entry[3]:
            r[3] = r[2]
    center = menu_metadata(project_root).get('start_location')
    if center:
        # Aligned artwork: selection on the Blit's object, Nuketown's offsets
        # for the option list (+180, -50) and the "new" badge (+100, -90).
        x, y = (int(v) for v in center)
        entry[8], entry[9], entry[10], entry[11] = str(x), str(y), str(x + 180), str(y - 50)
        entry[17], entry[18] = str(x + 100), str(y - 90)
    prefix = 'WAW_MENU_' + project.upper()
    entry[4], entry[5], entry[6], entry[16] = (prefix + '_CAPS', prefix + '_DESC',
                                               menu_metadata(project_root).get('blit', entry[6]), prefix + '_TITLE')
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
    (work.resolve() / f"verify_{ff.stem}_lobby.log").write_text(proc.stdout + proc.stderr, encoding='utf-8')
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


def rope_asset_lines(asset_root: Path | None) -> list[str]:
    """Rope_Init requests its default material even for named rope materials."""
    manifest = asset_root / 'BSP/constraints.json' if asset_root is not None else None
    if manifest is None or not manifest.is_file():
        return []
    data = json.loads(manifest.read_text(encoding='utf-8'))
    return ['material,mc/rope'] if any(c['type'] == 8 for c in data['constraints']) else []


def link_mod(bo2: Path, work: Path, unlinker: Path, extra_lines: list[str] = (),
             asset_root: Path | None = None, extra_asset_roots: list[Path] = (),
             linker: Path | None = None, stock_dump: Path | None = None) -> tuple[Path, list[str]]:
    """Link mod.ff. Template entries whose raw assets are not on disk (DLC
    weapons etc. that All2Raw did not produce) are dropped one at a time and
    returned, so the caller can report exactly what the build lacks."""
    extra_lines = list(dict.fromkeys([*extra_lines, *rope_asset_lines(asset_root)]))
    work = work.resolve()  # the linker runs from bo2\bin
    template = bo2 / "mods" / TEMPLATE_MAP / f"{TEMPLATE_MAP}.zone"
    dropped: list[str] = []
    zone_file = work / "zone_source" / "mod.zone"
    zone_file.parent.mkdir(parents=True, exist_ok=True)
    zone_text = open_mod_ipak_first(build_mod_zone(template, dropped), extra_lines)
    if asset_root is not None:
        zone_text = prune_template_weapons(zone_text, asset_root, bo2, dropped)
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
        for name in ('mapstable.csv', 'gametypestable.csv', 'zm_statstable.csv'):
            source = asset_root / 'zm' / name
            if source.is_file():
                target = table_override / 'zm' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        for line in extra_lines:
            if not line.startswith('rawfile,'):
                continue
            relative = line.split(',', 1)[1].strip()
            source = (asset_root / relative).resolve()
            source.relative_to(asset_root.resolve())
            if source.is_file():
                target = table_override / relative
                target.resolve().relative_to(table_override.resolve())
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
