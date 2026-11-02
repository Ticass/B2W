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
MENU = 'ui_mp/t6/waw_menu.lua'
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
          'scorebar_zom_1', 'scorebar_zom_2', 'scorebar_zom_3', 'scorebar_zom_4',
          'scorebar_zom_long_1', 'scorebar_zom_long_2', 'scorebar_zom_long_3', 'scorebar_zom_long_4')
# Engine-owned T4 scoreboard labels (code_post_gfx); staged with the map's strings.
ENGINE_STRINGS = ('CGAME_SB_POINTS', 'CGAME_SB_KILLS', 'CGAME_SB_DOWNS', 'CGAME_SB_REVIVES',
                  'CGAME_SB_HEADSHOTS', 'CGAME_SB_PING')


def write_table(path: Path, rows) -> None:
    data = io.StringIO(newline='')
    csv.writer(data, lineterminator='\n').writerows(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data.getvalue(), encoding='utf-8')


class SourceFinder:
    """WaW assets by kind: the map's overrides first, then the stock zones."""

    def __init__(self, roots: list[Path], stock=None, recovery=None):
        from .assetresolve import Resolver
        self.roots = roots
        self.stock = stock
        self.resolver = Resolver(roots, stock)
        # wavelet.IwdRecovery: the IWD copy WaW loads (wavelets, patched copies).
        self.recovery = recovery

    def find(self, kind: str, name: str) -> Path | None:
        from .assetresolve import asset_path
        relative = asset_path(kind, name)
        for root in self.resolver.roots:
            path = root / relative
            if path.is_file():
                return path
        return self.resolver.find(kind, name)

    def material_image(self, name: str) -> Path | None:
        """The colorMap image of a 2D source material."""
        path = self.find('material', name)
        if path is None:
            return None
        textures = json.loads(path.read_text(encoding='utf-8')).get('textures', [])
        image = next((t['image'] for t in textures if t.get('name') == 'colorMap'), None)
        if not image:
            return None
        found = self.find('image', image)
        return self.recovery.resolve(image, found) if self.recovery is not None else found

    def font(self, font_name: str) -> Path | None:
        path = next((r / (font_name + '.json') for r in self.roots if (r / (font_name + '.json')).is_file()), None)
        if path is None and self.stock is not None:
            found = self.stock.root_for('font', font_name)
            if found:
                path = found[1] / (font_name + '.json')
        return path if path is not None and path.is_file() else None


def write_sprite(project: Path, name: str, image: Image.Image) -> str:
    """A lossless RGBA 2D material, including the source font's antialiased alpha."""
    target = project / 'content_source' / techsets.oat_image_path(name)
    target.parent.mkdir(parents=True, exist_ok=True)
    buffer = io.BytesIO()
    image.convert('RGBA').save(buffer, format='DDS')
    target.write_bytes(iwi.dds_to_iwi(buffer.getvalue(), base_level_only=True))
    material = project / 'content_source' / techsets.oat_material_path(name)
    material.parent.mkdir(parents=True, exist_ok=True)
    material.write_text(json.dumps(menuart.material(name)), encoding='utf-8')
    return name


def font_glyphs(source: SourceFinder, font_name: str, role: str, sprite, chars=None) -> list[list]:
    """Glyph table rows of a source font; ``chars`` limits which glyphs are staged.

    Raises ValueError naming the missing font asset."""
    path = source.font(font_name)
    if path is None:
        raise ValueError(font_name)
    font = json.loads(path.read_text(encoding='utf-8'))
    material = source.find('material', font['material'])
    if material is None:
        raise ValueError(font['material'])
    image_name = json.loads(material.read_text(encoding='utf-8'))['textures'][0]['image']
    atlas_path = source.find('image', image_name)
    if atlas_path is None:
        raise ValueError(image_name)
    rows = []
    with Image.open(atlas_path) as pixels:
        atlas = pixels.convert('RGBA')
        for glyph in font['glyphs']:
            char = glyph['letter']
            if isinstance(char, int):
                char = chr(char)
            if not isinstance(char, str) or len(char) != 1:
                raise ValueError(f'Invalid source font glyph in {path}')
            if chars is not None and char not in chars:
                continue
            w, h = glyph['pixelWidth'], glyph['pixelHeight']
            name = ''
            pw = ph = 0
            if w and h:
                x, y = round(glyph['s0'] * atlas.width), round(glyph['t0'] * atlas.height)
                pw, ph = max(4, 1 << (w - 1).bit_length()), max(4, 1 << (h - 1).bit_length())
                cell = Image.new('RGBA', (pw, ph))
                cell.paste(atlas.crop((x, y, x+w, y+h)), (0, 0))
                name = sprite(f'{role}_{ord(char):04x}', cell)
            rows.append([f'{role}:{ord(char)}', name, glyph['x0'], glyph['y0'],
                         glyph['dx'], pw, ph, font['pixelHeight']])
    return rows


def stage_assets(project: Path, roots: list[Path], stock=None) -> dict:
    """Resolve the map's overrides first, then the original WaW stock zones."""
    source = SourceFinder(roots, stock)
    materials = []

    def sprite(name: str, image: Image.Image) -> str:
        materials.append(write_sprite(project, 'waw_hud/' + name, image))
        return materials[-1]

    missing = []
    for name in ASSETS:
        path = source.material_image(name)
        # Tesla is DLC-owned and absent on maps which do not ship the weapon.
        if path is None:
            if name != 'ammo_counter_tesla':
                missing.append(name)
            continue
        with Image.open(path) as pixels:
            sprite(name, pixels)

    glyph_rows = []
    fonts = []
    for role, font_name in (('objective', 'fonts/objectiveFont'), ('score', 'fonts/normalFont')):
        try:
            glyph_rows += font_glyphs(source, font_name, role, sprite)
        except ValueError as error:
            if str(error).startswith('Invalid'):
                raise
            missing.append(str(error))
            continue
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
    # WaW's own pause menu (wawmenu.py); without it the entry keeps BO2's class menu.
    from . import wawmenu
    (project / MENU).write_bytes(checked_bytecode('waw_menu'))
    tables = [MAPS, WEAPONS, GLYPHS]
    materials = list(data['materials'])
    menu = project / wawmenu.MANIFEST
    if menu.is_file():
        staged = json.loads(menu.read_text(encoding='utf-8'))
        if 'staged_materials' in staged:
            tables += [wawmenu.TABLE, wawmenu.GLYPHS]
            materials += [m for m in staged['staged_materials'] if m not in materials]
    if wawmenu.TABLE not in tables:
        wawmenu.write_table(project / wawmenu.TABLE, [])
        tables.append(wawmenu.TABLE)
    modules = [RUNTIME, BASE, SUPPORT, MENU, *[p for module, base, _ in WRAPPERS for p in (module, base)]]
    return [f'rawfile,{p}' for p in modules] + [f'stringtable,{p}' for p in tables] + [
        f'material,{m}' for m in materials]
