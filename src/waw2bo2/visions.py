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

from . import glow, gsc, iwi

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


def film_terms(fields):
    """Contrast, brightness and desaturation weight as CoDWaW sub_6DC1A0 sets
    them: the film value combined with the user dvars r_contrast (product),
    r_brightness (sum) and r_desaturation (d (d + (1 - d) r_desaturation)),
    which a map script may set (USER_DVARS, merged into ``fields``)."""
    def scalar(k, default):
        v = float(fields.get(k, default))
        if not math.isfinite(v):
            raise ValueError(f'non-finite vision field {k}')
        return v
    d = scalar('r_filmdesaturation', 0)
    return (scalar('r_filmcontrast', 1) * scalar('r_contrast', 1),
            scalar('r_filmbrightness', 0) + scalar('r_brightness', 0),
            max(1 / 4096, d * (d + (1 - d) * scalar('r_desaturation', 0))))


# Renderer dvars a WaW map script may set with setClientDvar that change the
# film (CoDWaW sub_6DC1A0). r_filmUseTweaks replaces the vision's film fields
# by the r_filmTweak* dvars (FILM_TWEAKS: tweak dvar -> vision field).
USER_DVARS = ('r_contrast', 'r_brightness', 'r_desaturation')
FILM_TWEAKS = {'r_filmtweakenable': 'r_filmenable', 'r_filmtweakcontrast': 'r_filmcontrast',
               'r_filmtweakbrightness': 'r_filmbrightness', 'r_filmtweakdesaturation': 'r_filmdesaturation',
               'r_filmtweakinvert': 'r_filminvert', 'r_filmtweakdarktint': 'r_filmdarktint',
               'r_filmtweaklighttint': 'r_filmlighttint'}
FILM_TWEAK_GRADE = '_filmtweak'


def script_dvars(paths) -> dict[str, str]:
    """Last literal value each script sets per renderer dvar through
    setClientDvar / setClientDvars (WaW names, lower-cased)."""
    out = {}
    for path in paths:
        # comments are not tokens: commented-out settings stay out
        tokens = gsc.tokenize(path.read_text(encoding='utf-8', errors='replace'))
        for i, t in enumerate(tokens):
            if 'setclientdvar' not in t.text.lower() or i + 1 >= len(tokens) or tokens[i+1].text != '(':
                continue
            j = i + 2
            while j + 2 < len(tokens) and tokens[j].kind == gsc.STRING and tokens[j+1].text == ',' \
                    and tokens[j+2].kind in (gsc.STRING, 'number'):
                value = tokens[j+2].text
                out[tokens[j].text[1:-1].lower()] = value[1:-1] if tokens[j+2].kind == gsc.STRING else value
                j += 3
                if j < len(tokens) and tokens[j].text == ',':
                    j += 1
                else:
                    break
    return out


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
    contrast, brightness, desat = film_terms(fields)
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


# T6 hdr_bloom_apply: above 0.75 the composite applies
# 0.75 + 0.25 * (1 - exp(4.328085 - 5.77078 x)) before the LUT.
SHOULDER_KNEE = 0.75
SHOULDER = (4.328085, 5.77078)


def unshoulder(c: float) -> float:
    """Display value WaW wrote, from the value T6 hands the LUT (WaW clips at 1)."""
    if c <= SHOULDER_KNEE:
        return c
    rest = 1 - (c - SHOULDER_KNEE) / (1 - SHOULDER_KNEE)
    if rest <= 0:
        return 1.
    return min(1., (SHOULDER[0] - math.log(rest)) / SHOULDER[1])


def atlas(fields: list[dict]) -> bytes:
    """Native atlas layout: x = blue*32 + red, y = grade*32 + green. Each grid
    point first undoes T6's highlight shoulder, then applies the WaW film."""
    width, height = 1024, 32*len(fields)
    data = bytearray()
    for grade in fields:
        for green in range(32):
            for blue in range(32):
                for red in range(32):
                    rgb = film(tuple(unshoulder(c / 31) for c in (red, green, blue)), grade)
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
          donor: Path | None = None, source_root: Path | None = None,
          techset_dump: Path | None = None) -> dict:
    """``techset_dump``: when given, WaW film and glow run in the full-screen
    overlay pass (glow.py) and the LUT only undoes the composite shoulder."""
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
    def lookup(name):
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
        return name in sources
    report['default_fallback'] = []
    for name in sorted(wanted):
        if lookup(name):
            continue
        # WaW cannot open the file and loads vision/default instead
        # (CoDWaW sub_4629F0: "couldn't open '%s'" -> "vision/default.vision").
        if name != 'default' and lookup('default'):
            sources[name] = sources['default']
            report['default_fallback'].append(name)
        else:
            report['missing'].append(name)
    grades = [{'r_filmenable': '0'}]
    init = ['// WaW vision names and their translated rawfiles.', 'init()', '{',
            '    level.waw2bo2_visions = [];', '    level.waw2bo2_vision_indices = [];']
    zone_lines = []
    identity = project/'vision/waw/_identity.vision'
    identity.parent.mkdir(parents=True, exist_ok=True)
    identity.write_text(neutral_vision(0, {}), encoding='utf-8')
    zone_lines.append('rawfile,vision/waw/_identity.vision\n')
    dvars = script_dvars(sorted((project/'maps/mp/waw').rglob('*.gsc')))
    user = {k: dvars[k] for k in USER_DVARS if k in dvars}
    report['script_render_dvars'] = {k: v for k, v in dvars.items() if k.startswith(('r_', 'sm_'))}
    for name in sorted(wanted & sources.keys()):
        if not re.fullmatch(r'[a-z0-9_/.-]+', name) or '..' in name:
            report['errors'].append(f'invalid vision name: {name}')
            continue
        fields = {**parse(sources[name][0]), **user}
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
    if techset_dump is not None:
        grading = {name: fields for name, fields in zip(report['visions'], grades[1:])}
        if dvars.get('r_filmusetweaks', '0') not in ('0', ''):
            # r_filmUseTweaks: WaW films with the r_filmTweak* dvars whatever
            # the vision says (sub_6DC1A0); compat switches to this grade.
            tweak = {'r_filmenable': '0'}  # r_filmTweakEnable defaults off
            tweak.update({FILM_TWEAKS[k]: v for k, v in dvars.items() if k in FILM_TWEAKS})
            grading[FILM_TWEAK_GRADE] = {**tweak, **user}
            report['film_tweak'] = grading[FILM_TWEAK_GRADE]
        overlays = glow.stage(project, grading, techset_dump)
        init.append('    level.waw2bo2_vision_overlays = [];')
        for name, material in overlays['overlays'].items():
            init.append(f'    level.waw2bo2_vision_overlays["{name}"] = "{material}";')
            init.append(f'    precacheshader( "{material}" );')
        report['overlay'] = overlays
        # Film now runs exactly in the overlay; the LUT keeps the shoulder inverse.
        grades = [{'r_filmenable': '0'} for _ in grades]
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
