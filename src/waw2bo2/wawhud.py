"""WaW's engine-owned HUD, rendered through T6 LUI.

The source font glyphs are small sprites: T6's linker cannot import T4 fonts.
This preserves the original atlas and metrics without substituting a BO2 font.
"""
from __future__ import annotations

import csv
import hashlib
import io
import json
from pathlib import Path
from PIL import Image

from . import iwi, menuart, techsets, weapons

RUNTIME = 'ui_mp/t6/hud.lua'
BASE = 'ui_mp/t6/waw_hud_base.lua'
AMMO = 'ui_mp/t6/zombie/ammoareazombie.lua'
AMMO_BASE = 'ui_mp/t6/zombie/waw_ammoarea_base.lua'
SUPPORT = 'ui_mp/t6/waw_hud_runtime.lua'
SCORE = 'ui_mp/t6/zombie/hudcompetitivescoreboardzombie.lua'
SCORE_BASE = 'ui_mp/t6/zombie/waw_scoreboard_base.lua'
OTHER = 'ui_mp/t6/zombie/otherammocounters.lua'
OTHER_BASE = 'ui_mp/t6/zombie/waw_otherammo_base.lua'
WRAPPERS = ((AMMO, AMMO_BASE, 'waw_ammoarea'), (SCORE, SCORE_BASE, 'waw_scoreboard'),
            (OTHER, OTHER_BASE, 'waw_otherammo'))
MAPS = 'zm/waw_hud_maps.csv'
WEAPONS = 'zm/waw_hud_weapons.csv'
GLYPHS = 'zm/waw_hud_glyphs.csv'
MANIFEST = 'content_source/waw_hud.json'
ASSETS = ('ammo_counter_bullet', 'ammo_counter_riflebullet', 'ammo_counter_beltbullet',
          'ammo_counter_rocket', 'ammo_counter_shotgunshell', 'ammo_counter_tesla',
          'scorebar_zom_1', 'scorebar_zom_2', 'scorebar_zom_3', 'scorebar_zom_4')


def write_table(path: Path, rows) -> None:
    data = io.StringIO(newline='')
    csv.writer(data, lineterminator='\n').writerows(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data.getvalue(), encoding='utf-8')


def stage_assets(project: Path, roots: list[Path], stock=None) -> dict:
    """Resolve the map's overrides first, then the original WaW stock zones."""
    from .assetresolve import Resolver
    resolver = Resolver(roots, stock)
    materials = []

    def find(kind: str, name: str) -> Path | None:
        from .assetresolve import asset_path
        relative = asset_path(kind, name)
        for root in resolver.roots:
            path = root / relative
            if path.is_file():
                return path
        return resolver.find(kind, name)

    def sprite(name: str, image: Image.Image) -> str:
        name = 'waw_hud/' + name
        target = project / 'content_source' / techsets.oat_image_path(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        # Lossless RGBA, including the source font's antialiased alpha.
        buffer = io.BytesIO()
        image.convert('RGBA').save(buffer, format='DDS')
        target.write_bytes(iwi.dds_to_iwi(buffer.getvalue(), base_level_only=True))
        material = project / 'content_source' / techsets.oat_material_path(name)
        material.parent.mkdir(parents=True, exist_ok=True)
        material.write_text(json.dumps(menuart.material(name)), encoding='utf-8')
        materials.append(name)
        return name

    missing = []
    for name in ASSETS:
        path = find('material', name)
        # Tesla is DLC-owned and absent on maps which do not ship the weapon.
        if path is None:
            if name != 'ammo_counter_tesla':
                missing.append(name)
            continue
        data = json.loads(path.read_text(encoding='utf-8'))
        textures = data.get('textures', [])
        image = next((t['image'] for t in textures if t.get('name') == 'colorMap'), None)
        source = find('image', image) if image else None
        if source is None:
            missing.append(name)
            continue
        with Image.open(source) as pixels:
            sprite(name, pixels)

    glyph_rows = []
    fonts = []
    for role, font_name in (('objective', 'fonts/objectiveFont'), ('score', 'fonts/normalFont')):
        path = next((r / (font_name + '.json') for r in roots if (r / (font_name + '.json')).is_file()), None)
        if path is None and stock is not None:
            found = stock.root_for('font', font_name)
            if found:
                path = found[1] / (font_name + '.json')
        if path is None or not path.is_file():
            missing.append(font_name)
            continue
        font = json.loads(path.read_text(encoding='utf-8'))
        material = find('material', font['material'])
        if material is None:
            missing.append(font['material'])
            continue
        image_name = json.loads(material.read_text(encoding='utf-8'))['textures'][0]['image']
        atlas_path = find('image', image_name)
        if atlas_path is None:
            missing.append(image_name)
            continue
        with Image.open(atlas_path) as source:
            atlas = source.convert('RGBA')
            for glyph in font['glyphs']:
                char = glyph['letter']
                if isinstance(char, int):
                    char = chr(char)
                if not isinstance(char, str) or len(char) != 1:
                    raise ValueError(f'Invalid source font glyph in {path}')
                w, h = glyph['pixelWidth'], glyph['pixelHeight']
                name = ''
                pw = ph = 0
                if w and h:
                    x, y = round(glyph['s0'] * atlas.width), round(glyph['t0'] * atlas.height)
                    pw, ph = max(4, 1 << (w - 1).bit_length()), max(4, 1 << (h - 1).bit_length())
                    pixels = Image.new('RGBA', (pw, ph))
                    pixels.paste(atlas.crop((x, y, x+w, y+h)), (0, 0))
                    name = sprite(f'{role}_{ord(char):04x}', pixels)
                glyph_rows.append([f'{role}:{ord(char)}', name, glyph['x0'], glyph['y0'],
                                   glyph['dx'], pw, ph, font['pixelHeight']])
        fonts.append(font_name)
    if missing:
        raise ValueError('WaW HUD source assets missing: ' + ', '.join(missing))
    write_table(project / GLYPHS, glyph_rows)
    result = {'materials': materials, 'fonts': fonts, 'glyphs': len(glyph_rows)}
    (project / MANIFEST).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def stage_runtime(bo2: Path, project: Path, name: str) -> list[str]:
    manifest = project / MANIFEST
    if not manifest.is_file():
        return []
    data = json.loads(manifest.read_text(encoding='utf-8'))
    def checked_bytecode(wrapper_name):
        source = Path(__file__).parent / f'compat/{wrapper_name}.lua'
        hashes = json.loads(source.with_suffix('.json').read_text())
        for path, key in ((source, 'source_sha256'), (source.with_suffix('.luac'), 'bytecode_sha256')):
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[key]:
                raise ValueError(f'Recompile {wrapper_name} with tools/compile_menu_layers.py')
        return source.with_suffix('.luac').read_bytes()
    (project / SUPPORT).parent.mkdir(parents=True, exist_ok=True)
    (project / SUPPORT).write_bytes(checked_bytecode('waw_hud'))
    # The stock first-snapshot path selects DPadArea on TranZit and
    # AmmoAreaZombie on later DLCs. Assemble the converted HUD before the
    # native ForceHUDRefresh instead of hooking just one of those constructors.
    (project / BASE).write_bytes((bo2 / 'raw' / RUNTIME).read_bytes())
    (project / RUNTIME).write_bytes(checked_bytecode('waw_hud_entry'))
    for module, base, wrapper_name in WRAPPERS:
        original = (bo2 / 'raw' / module).read_bytes()
        if module == OTHER:
            from .overheathud import repair_bytecode
            original = repair_bytecode(original)
        wrapper = checked_bytecode(wrapper_name)
        if not original.startswith(b'\x1bLua') or original[:16] != wrapper[:16]:
            raise ValueError('Unsupported stock T6 HUD bytecode format')
        (project / base).parent.mkdir(parents=True, exist_ok=True)
        (project / base).write_bytes(original)
        (project / module).write_bytes(wrapper)
    write_table(project / MAPS, [[name, '1']])
    rows = []
    runtime = project / 'content_source/weapons.runtime.json'
    names = json.loads(runtime.read_text()).get('weapons', []) if runtime.is_file() else []
    for weapon in names:
        info = project / 'content_source/weapons' / weapon
        if not info.is_file():
            continue
        d = weapons.read_info_file(info)
        # T4 sub_42B990 tests the source weapon name for "tesla_gun".
        counter_icon = 'ammo_counter_tesla' if 'tesla_gun' in weapon.lower() else ''
        rows.append([weapon, d.get('ammoCounterClip', 'None'), d.get('clipSize', '0'),
                     d.get('overheatWeapon', '0'), d.get('displayName', ''), d.get('altWeapon', ''), counter_icon,
                     # T4 sub_44F540 draws a heat weapon's hudIcon as its gauge.
                     d.get('hudIcon', '') if d.get('overheatWeapon') == '1' else ''])
    write_table(project / WEAPONS, rows)
    modules = [RUNTIME, BASE, SUPPORT, *[p for module, base, _ in WRAPPERS for p in (module, base)]]
    return [f'rawfile,{p}' for p in modules] + [f'stringtable,{p}' for p in (MAPS, WEAPONS, GLYPHS)] + [
        f'material,{m}' for m in data['materials']]
