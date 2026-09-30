"""WaW film grading baked into BO2's native 32-cube LUT atlas.

The film equation is measured in WaW 1.7 sub_6DC1A0 and its
postfx_color pixel shader. T6 create_lut2dv squares atlas values before
grading, then takes their square root; neutral VC settings preserve them.
"""
from __future__ import annotations

import json
import math
import re
import struct
import zipfile
from pathlib import Path

from . import gsc, iwi

PAIR = re.compile(r'([\w]+)\s+"([^"\r\n]*)"')
LUMA = (.299, .587, .114)

def lut_donor(root: Path | None) -> Path | None:
    """Find the native LUT material by renderer role, independent of map name."""
    if root is not None:
        for path in sorted((root/'materials').rglob('*.json')):
            material = json.loads(path.read_text(encoding='utf-8'))
            if material.get('techniqueSet', '').startswith('hdr_create_lut2dv_') and material.get('textures'):
                return path
    return None


def parse(text: str) -> dict[str, str]:
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return {k.lower(): v for k, v in PAIR.findall(text)}


def film(rgb, fields):
    """Evaluate the native WaW film equation (default user renderer dvars)."""
    def scalar(k, default):
        v = float(fields.get(k, default))
        if not math.isfinite(v):
            raise ValueError(f'non-finite vision field {k}')
        return v
    def vector(k):
        v = tuple(float(x) for x in fields.get(k, '1 1 1').split())
        if len(v) != 3 or not all(math.isfinite(x) for x in v):
            raise ValueError(f'invalid vision vector {k}')
        return v
    if not scalar('r_filmenable', 1):
        return tuple(rgb)
    contrast = scalar('r_filmcontrast', 1)
    brightness = scalar('r_filmbrightness', 0)
    desat = max(1 / 4096, scalar('r_filmdesaturation', 0))
    dark, light = vector('r_filmdarktint'), vector('r_filmlighttint')
    lum = sum(a*b for a, b in zip(rgb, LUMA))
    scale = contrast
    bias = brightness + .5 - contrast*.5
    if scalar('r_filminvert', 0):
        scale = -scale
        bias += 1
    return tuple(max(0., min(1., ((1-desat)*x + desat*lum) *
                      (lo + (hi-lo)*lum) * scale + bias))
                 for x, lo, hi in zip(rgb, dark, light))


def atlas(fields: list[dict]) -> bytes:
    """Native atlas layout: x = blue*32 + red, y = grade*32 + green."""
    width, height = 1024, 32*len(fields)
    data = bytearray()
    for grade in fields:
        for green in range(32):
            for blue in range(32):
                for red in range(32):
                    rgb = film((red/31, green/31, blue/31), grade)
                    data.extend(round(x*255) for x in rgb)
                    data.append(255)
    return (b'IWi\x1b' + struct.pack('<bbHHHf', iwi.FMT_RGBA, iwi.FLAG_NOMIPMAPS,
                                   width, height, 1, 0.) + bytes(16) +
            struct.pack('<8I', 64+len(data), *([0]*7)) + data)


def neutral_vision(index: int, original: dict) -> str:
    out = {'r_filmEnable': '1', 'r_filmLut': '-1',
           'vc_RS': '0 .5 0 .75', 'vc_RE': '.5 1 .25 1',
           'vc_FGM': '.5 .5 .5 1', 'vc_FSM': '.212585 .715195 .072220 1',
           'vc_FBM': '0 0 0 0', 'vc_LIB': '0 0 0 0', 'vc_LIW': '32 32 32 32',
           'vc_LIG': '1 1 1 1', 'vc_LOB': '0 0 0 0', 'vc_LOW': '32 32 32 32',
           'vc_RGBH': '0 0 0 0', 'vc_RGBL': '0 0 0 0',
           'vc_YH': '0 0 0 0', 'vc_YL': '0 0 0 0'}
    for band in ('S', 'M', 'H'):
        for channel, vec in zip('RGB', ('1 0 0 0', '0 1 0 0', '0 0 1 0')):
            out[f'vc_{band}M{channel}'] = vec
    # Shared fields verified in T6's vision parser table at 0xD3E4F8.
    for key in ('r_primarylightusetweaks', 'r_primarylighttweakdiffusestrength',
                'r_primarylighttweakspecularstrength', 'snd_masterringmod',
                'snd_reverbringmod', 'snd_hifilter', 'snd_lowfilter',
                'r_sunflaretint', 'r_postemissivebrightening'):
        if key in original:
            out[key] = original[key]
    out['r_reviveFX_enable'] = original.get('r_revivefx_enable', '0')
    return '// WaW film baked into the map LUT atlas.\n' + ''.join(f'{k} "{v}"\n' for k,v in out.items())


def stage(project: Path, roots: list[Path], iwds: list[Path], stock=None,
          donor: Path | None = None, source_root: Path | None = None) -> dict:
    sources = {}
    for archive in iwds:
        with zipfile.ZipFile(archive) as z:
            for name in sorted(z.namelist()):
                low = name.lower().replace('\\', '/')
                if low.startswith('vision/') and low.endswith('.vision'):
                    sources.setdefault(low[7:-7], (z.read(name).decode('utf-8', errors='replace'), f'{archive}:{name}'))
    for root in roots:
        for path in sorted((root/'vision').rglob('*.vision')):
            sources.setdefault(path.relative_to(root/'vision').as_posix()[:-7].lower(),
                               (path.read_text(encoding='utf-8', errors='replace'), str(path)))
    wanted = set()
    for path in (project/'maps/mp/waw').rglob('*.gsc'):
        tokens = gsc.tokenize(path.read_text(encoding='utf-8'))
        for i, t in enumerate(tokens):
            if t.kind == gsc.STRING:
                value = t.text[1:-1].lower()
                if value in sources:
                    wanted.add(value)
                # Literal first arguments to all vision APIs, including missing ones.
                if i >= 2 and tokens[i-1].text == '(' and 'vision' in tokens[i-2].text.lower():
                    if value:
                        wanted.add(value)
    report = {'status': 'native_lut', 'visions': {}, 'missing': [], 'not_translated': {},
              'sampling': '32^3 RGB8, native T6 spatial LUT interpolation',
              'transition': 'LUT grade switches immediately; shared vision fields retain native fade time',
              'errors': []}
    for name in sorted(wanted):
        if name not in sources and stock is not None:
            hit = stock.root_for('rawfile', f'vision/{name}.vision')
            if hit:
                zone, root = hit
                path = root/'vision'/f'{name}.vision'
                if path.exists():
                    sources[name] = (path.read_text(encoding='utf-8'), str(path))
        if name not in sources and source_root is not None:
            path = source_root/'vision'/f'{name}.vision'
            if path.is_file():
                sources[name] = (path.read_text(encoding='utf-8'), str(path))
        if name not in sources:
            report['missing'].append(name)
    grades = [{'r_filmenable': '0'}]
    init = ['// WaW vision names and their translated rawfiles.', 'init()', '{',
            '    level.waw2bo2_visions = [];', '    level.waw2bo2_vision_indices = [];']
    zone_lines = []
    identity = project/'vision/waw/_identity.vision'
    identity.parent.mkdir(parents=True, exist_ok=True)
    identity.write_text(neutral_vision(0, {}), encoding='utf-8')
    zone_lines.append('rawfile,vision/waw/_identity.vision\n')
    for name in sorted(wanted & sources.keys()):
        if not re.fullmatch(r'[a-z0-9_/.-]+', name) or '..' in name:
            report['errors'].append(f'invalid vision name: {name}')
            continue
        fields = parse(sources[name][0])
        index = len(grades)
        # vc_LUT's native range is -32..32; it selects abs(value)-1.
        if index >= 32:
            report['errors'].append(f'native T6 LUT capacity exceeded by vision: {name}')
            continue
        grades.append(fields)
        target = project/'vision/waw'/f'{name}.vision'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(neutral_vision(index, fields), encoding='utf-8')
        init.append(f'    level.waw2bo2_visions["{name}"] = "waw/{name}";')
        init.append(f'    level.waw2bo2_vision_indices["{name}"] = {index};')
        zone_lines.append(f'rawfile,vision/waw/{name}.vision\n')
        report['visions'][name] = {'source': sources[name][1], 'index': index, 'output': f'vision/waw/{name}.vision'}
        report['not_translated'][name] = {k:v for k,v in fields.items()
                                         if k.startswith('r_glow') or (k.startswith('r_revivefx_') and k != 'r_revivefx_enable')}
    init += ['}', '']
    (project/'maps/mp/waw/_waw2bo2_visions.gsc').write_text('\n'.join(init), encoding='utf-8')
    if len(grades) > 1:
        if donor is None or not donor.exists():
            report['errors'].append('native T6 LUT material donor missing')
        else:
            material = json.loads(donor.read_text(encoding='utf-8'))
            material['textures'][0]['image'] = 'waw_vision_lut'
            (project/'materials/waw').mkdir(parents=True, exist_ok=True)
            (project/'materials/waw/vision_lut.json').write_text(json.dumps(material, indent=2)+'\n', encoding='utf-8')
            (project/'images').mkdir(parents=True, exist_ok=True)
            (project/'BSP').mkdir(parents=True, exist_ok=True)
            (project/'images/waw_vision_lut.iwi').write_bytes(atlas(grades))
            (project/'BSP/visions.json').write_text(json.dumps({'material': 'waw/vision_lut', 'grades': len(grades)})+'\n', encoding='utf-8')
    with (project/'mod_extra.zone').open('a', encoding='utf-8') as f:
        f.writelines(zone_lines)
    (project/'content_source').mkdir(parents=True, exist_ok=True)
    (project/'content_source/visions.stage.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    return report
