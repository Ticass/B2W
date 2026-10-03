"""WaW film and glow as one full-screen pass over BO2's resolved frame.

WaW applies its film grade and glow after the 3D view, in 8-bit gamma space
(measured in CoDWaW 1.7):

* film (postfx_color, visions.film): ``sat(((1-d) x + d L) (dark + (light-dark) L) k + b)``
  on the raw scene ``x`` with ``L = luma(x)``.
* glow source, glow_consistent_setup, written to a ``screen >> 2`` target
  (sub_723400): four bilinear taps at the target texel centre +-0.25 target
  texel (vs_glow_consistent_setup), each ``sat(L - cutoff) / (1 - cutoff) *
  film(x)`` with ``L`` the raw tap luma, averaged, then desaturated toward luma
  by r_glowBloomDesaturation (glowSetup = (cutoff, 1/(1-cutoff), 0, desat),
  sub_6DC300).
* blur (sub_74B2E0 -> sub_74B160 -> sub_74AA70 -> sub_74A550): separable
  Gaussian ``exp(-x^2 / 2 s^2)`` over target texels, eight tap pairs per side,
  normalized over all of them, pairs below 0.01 after normalization dropped,
  ``s_y = r_glowRadius0 * (targetW / screenW) * screenH / 480`` and
  ``s_x = s_y * screenH * aspect / screenW`` (sub_6D4F60), clamp addressing.
* apply (glow_apply_bloom): ``frame + r_glowBloomIntensity0 * glow``, bilinear
  from the quarter target (glowApply = (0, 0, 0, intensity)).

BO2 offers no extra render targets to a material, but its resolved frame is
code texture 9 (stock distortion binds it) and is still bound when HUD
elements draw. The converted world leaves film to this pass: the vision LUT is
identity apart from the composite shoulder, so code texture 9 holds WaW's raw
scene. A full-screen HUD material then evaluates both stages exactly. The four
pixels of a 2x2 quad share their quarter-resolution neighbourhood; each one
evaluates a quarter of the glow sources and the partial sums are exchanged
with ddx_fine/ddy_fine.

Not reproduced: god rays (r_glowRayIntensity), multi-pass chains for blur
radii above 6.4977 target texels (one Gaussian of the same variance is used),
the tiny-radius 2D pass (radius < 1.39 target texels), and WaW's timed fades
between visions (a vision switch is immediate). Each is reported.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

from . import shaders, techsets

SCENE_CODE_TEXTURE = 9
# The exact single-pass glow costs ~10 ms per 1080p frame (216 -> 63 FPS in
# game): off until a cheaper exact formulation exists. Film is one texel read.
GLOW_ENABLED = False
LUMA = (0.299, 0.587, 0.114)
MAX_SINGLE_PASS_RADIUS = 6.4977503


def _num(fields, key, default):
    try:
        return float(fields.get(key, default))
    except (TypeError, ValueError):
        return float(default)


def film_constants(fields: dict) -> dict:
    """visions.film as constants: enable, desaturation, tints, scale, bias."""
    if not _num(fields, 'r_filmenable', 1):
        return {'enabled': False}
    def vector(key):
        values = [float(v) for v in fields.get(key, '1 1 1').split()]
        if len(values) != 3:
            raise ValueError(f'invalid vision vector {key}')
        return values
    from .visions import film_terms
    contrast, brightness, desat = film_terms(fields)
    scale, bias = contrast, brightness + .5 - contrast * .5
    if _num(fields, 'r_filminvert', 0):
        scale, bias = -scale, bias + 1
    return {'enabled': True, 'desat': desat,
            'dark': vector('r_filmdarktint'), 'light': vector('r_filmlighttint'), 'scale': scale, 'bias': bias}


def glow_constants(fields: dict) -> tuple[dict | None, list[str]]:
    """WaW glow parameters of a vision (None when WaW draws no glow)."""
    notes = []
    intensity = _num(fields, 'r_glowbloomintensity0', 0)
    radius = _num(fields, 'r_glowradius0', 0)
    if not _num(fields, 'r_glow', 0) or intensity == 0 or radius == 0:
        return None, notes
    if _num(fields, 'r_glowrayintensity', 0) > 0:
        notes.append('god rays (r_glowRayIntensity) not reproduced')
    cutoff = min(_num(fields, 'r_glowbloomcutoff', 0), 0.999)
    return {'cutoff': cutoff, 'cut_scale': 1 / (1 - cutoff), 'desat': _num(fields, 'r_glowbloomdesaturation', 0),
            'intensity': intensity, 'radius': radius}, notes


def _f(v) -> str:
    return repr(float(v))


def _f3(v) -> str:
    return 'float3(' + ', '.join(_f(x) for x in v) + ')'


def pixel_hlsl(film: dict, glow: dict | None) -> str:
    lines = ['Texture2D<float4> scene : register(t0);',
             'SamplerState scene_sampler : register(s0);',
             f'static const float3 LUMA = {_f3(LUMA)};',
             'float3 film(float3 x)', '{']
    if film['enabled']:
        lines += ['    float lum = dot(x, LUMA);',
                  f'    float3 tint = {_f3(film["dark"])} + ({_f3(film["light"])} - {_f3(film["dark"])}) * lum;',
                  f'    return ((1.0 - {_f(film["desat"])}) * x + {_f(film["desat"])} * lum) * tint * {_f(film["scale"])}'
                  f' + {_f(film["bias"])};']
    else:
        lines += ['    return x;']
    lines += ['}', '']
    if glow is None:
        lines += ['float4 main(float4 pos : SV_Position) : SV_Target0',
                  '{',
                  '    return float4(saturate(film(scene.Load(int3(int2(pos.xy), 0)).rgb)), 1.0);',
                  '}']
        return '\n'.join(lines) + '\n'
    lines += [
        '// WaW glow_consistent_setup tap: 2x2 texel average at a texel corner',
        'float3 glow_tap(float2 uv)',
        '{',
        '    float4 r = scene.GatherRed(scene_sampler, uv);',
        '    float4 g = scene.GatherGreen(scene_sampler, uv);',
        '    float4 b = scene.GatherBlue(scene_sampler, uv);',
        '    float3 x = float3(dot(r, 0.25), dot(g, 0.25), dot(b, 0.25));',
        f'    return saturate(dot(x, LUMA) - {_f(glow["cutoff"])}) * film(x);',
        '}',
        '',
        '// one quarter-resolution glow texel q (WaW glow source)',
        'float3 glow_source(int2 q, float2 quarter)',
        '{',
        '    float2 c = (float2(q) + 0.5) / quarter;',
        '    float2 o = 0.25 / quarter;',
        '    float3 s = glow_tap(c + float2(-o.x, -o.y)) + glow_tap(c + float2(-o.x, o.y))',
        '             + glow_tap(c + float2(o.x, -o.y)) + glow_tap(c + float2(o.x, o.y));',
        f'    s *= 0.25 * {_f(glow["cut_scale"])};',
        f'    return lerp(s, dot(s, LUMA), {_f(glow["desat"])});',
        '}',
        '',
        '// sub_74A550: per-texel weights of eight tap pairs, truncated below 0.01',
        'int waw_kernel(float sigma, out float w[16])',
        '{',
        '    float k = -0.5 / (sigma * sigma);',
        '    float pair[8];',
        '    float total = 0;',
        '    [unroll] for (int i = 0; i < 8; i++)',
        '    {',
        '        float e0 = 2 * i, e1 = 2 * i + 1;',
        '        float we = exp(e0 * e0 * k) * (i == 0 ? 0.5 : 1.0);',
        '        float wo = exp(e1 * e1 * k);',
        '        w[2 * i] = we;',
        '        w[2 * i + 1] = wo;',
        '        pair[i] = we + wo;',
        '        total += pair[i];',
        '    }',
        '    float norm = 0.5 / total;',
        '    int count = 8;',
        '    [unroll] for (int j = 7; j >= 0; j--)',
        '        if (pair[j] * norm < 0.01)',
        '            count = j + 1;',
        '    [unroll] for (int t = 0; t < 16; t++)',
        '        w[t] = (t < 2 * count) ? w[t] * norm * (t == 0 ? 2.0 : 1.0) : 0.0;',
        '    return 2 * count - 1;',
        '}',
        '',
        'float quad_sum(float v, int2 lane)',
        '{',
        '    float dx = ddx_fine(v);',
        '    float row = 2 * v + (lane.x == 0 ? dx : -dx);',
        '    float dy = ddy_fine(row);',
        '    return 2 * row + (lane.y == 0 ? dy : -dy);',
        '}',
        '',
        'float4 main(float4 pos : SV_Position) : SV_Target0',
        '{',
        '    uint sw, sh;',
        '    scene.GetDimensions(sw, sh);',
        '    int2 qsize = int2(max(sw >> 2, 1u), max(sh >> 2, 1u));',
        '    float2 quarter = float2(qsize);',
        '    float2 screen = float2(sw, sh);',
        '    // sub_6D4F60: automatic aspect ratio from the display shape',
        '    int shape = (int)(screen.y * 16.0 / screen.x + 9.313225746154785e-10);',
        '    float aspect = shape == 10 ? 1.6 : (shape > 10 ? 1.33333337 : 1.77777779);',
        f'    float sigma_y = {_f(glow["radius"])} * (quarter.x / screen.x) * screen.y / 480.0;',
        '    float sigma_x = sigma_y * screen.y * aspect / screen.x;',
        '    float wx[16], wy[16];',
        '    int rx = waw_kernel(sigma_x, wx);',
        '    int ry = waw_kernel(sigma_y, wy);',
        '    int2 lane = int2(pos.xy) & 1;',
        '    float2 quad_px = floor(pos.xy) - float2(lane);',
        '    // quarter-texel coordinate of a pixel centre (bilinear apply)',
        '    float2 qlo = (quad_px + 0.5) * quarter / screen - 0.5;',
        '    int2 qmin = int2(floor(qlo));',
        '    float3 acc[3][3];',
        '    [unroll] for (int a = 0; a < 3; a++)',
        '        [unroll] for (int b = 0; b < 3; b++)',
        '            acc[a][b] = 0;',
        '    int2 lo = qmin - int2(rx, ry);',
        '    int2 extent = int2(2 * rx + 3, 2 * ry + 3);',
        '    int cells = extent.x * extent.y;',
        '    int index = lane.x + 2 * lane.y;',
        '    for (int k = index; k < cells; k += 4)',
        '    {',
        '        int2 cell = lo + int2(k % extent.x, k / extent.x);',
        '        float3 g = glow_source(clamp(cell, int2(0, 0), qsize - 1), quarter);',
        '        [unroll] for (int a2 = 0; a2 < 3; a2++)',
        '        {',
        '            int cx = clamp(qmin.x + a2, 0, qsize.x - 1);',
        '            int dx2 = abs(cell.x - cx);',
        '            float fx = dx2 < 16 ? wx[dx2] : 0.0;',
        '            [unroll] for (int b2 = 0; b2 < 3; b2++)',
        '            {',
        '                int cy = clamp(qmin.y + b2, 0, qsize.y - 1);',
        '                int dy2 = abs(cell.y - cy);',
        '                float fy = dy2 < 16 ? wy[dy2] : 0.0;',
        '                acc[a2][b2] += fx * fy * g;',
        '            }',
        '        }',
        '    }',
        '    float3 blur[3][3];',
        '    [unroll] for (int a3 = 0; a3 < 3; a3++)',
        '        [unroll] for (int b3 = 0; b3 < 3; b3++)',
        '            blur[a3][b3] = saturate(float3(quad_sum(acc[a3][b3].x, lane), quad_sum(acc[a3][b3].y, lane),',
        '                                            quad_sum(acc[a3][b3].z, lane)));',
        '    float2 q = (floor(pos.xy) + 0.5) * quarter / screen - 0.5;',
        '    int2 q0 = int2(floor(q));',
        '    float2 f = q - float2(q0);',
        '    int2 d = clamp(q0 - qmin, int2(0, 0), int2(1, 1));',
        '    float3 top = 0;',
        '    [unroll] for (int a4 = 0; a4 < 2; a4++)',
        '        [unroll] for (int b4 = 0; b4 < 2; b4++)',
        '        {',
        '            float3 v = blur[d.x + a4][d.y + b4];',
        '            float wgt = (a4 ? f.x : 1.0 - f.x) * (b4 ? f.y : 1.0 - f.y);',
        '            top += wgt * v;',
        '        }',
        '    float3 frame = saturate(film(scene.Load(int3(int2(pos.xy), 0)).rgb));',
        f'    return float4(saturate(frame + {_f(glow["intensity"])} * top), 1.0);',
        '}',
    ]
    return '\n'.join(lines) + '\n'


def overlay_name(vision: str) -> str:
    return 'waw/vision_overlay/' + vision.replace('/', '_')


def _hud_technique(techset_dump: Path) -> dict:
    """Stock T6 single-pass HUD technique set (the T4 2d counterpart)."""
    for path in sorted((techset_dump / 'techniquesets').glob('*.json')):
        parsed = techsets.parse(path.stem)
        if parsed.family == 'other' and parsed.unlit_kind == 'trivial':
            data = json.loads(path.read_text(encoding='utf-8'))
            passes = [t for t in data['techniques'] if t]
            if len(passes) == 1 and len(passes[0]['passArray']) == 1:
                return data
    raise ValueError('no stock T6 trivial HUD technique set in the technique dump')


def _hud_material(techset_dump: Path, techset: str) -> dict:
    for path in sorted((techset_dump / 'materials').rglob('*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))
        if data.get('techniqueSet') == techset:
            return data
    raise ValueError(f'no stock material uses {techset}')


def stage(project_root: Path, visions: dict[str, dict], techset_dump: Path) -> dict:
    """Write one overlay material per vision (plus the identity vision).

    ``visions``: vision name -> parsed WaW vision fields. Returns
    {'overlays': {vision: material}, 'notes': {...}, 'materials': [...]}.
    """
    base = _hud_technique(techset_dump)
    donor = _hud_material(techset_dump, base['name'])
    slot = next(i for i, t in enumerate(base['techniques']) if t)
    native_pass = base['techniques'][slot]['passArray'][0]
    report = {'overlays': {}, 'glow': {}, 'notes': {}, 'materials': [], 'technique_donor': base['name']}
    for vision, fields in sorted({'_identity': {'r_filmenable': '0'}, **visions}.items()):
        film = film_constants(fields)
        glow, notes = glow_constants(fields)
        if glow and not GLOW_ENABLED:
            notes.append('WaW glow not reproduced (exact single-pass glow too slow)')
            glow = None
        code = shaders.compile_hlsl(pixel_hlsl(film, glow), 'ps_5_0')
        shader = 'waw/overlay_' + hashlib.sha256(code).hexdigest()[:20]
        target = project_root / 'shader_bin' / f'ps_{shader}.cso'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(code)
        technique = copy.deepcopy(base)
        technique['name'] = shader
        tech = technique['techniques'][slot]
        tech['name'] = f'{shader}_{slot}'
        p = tech['passArray'][0]
        p['pixelShader'] = {'name': shader}
        # The scene texture replaces the material colour map; it is a stable argument.
        others = [a for a in p['args'] if a['type'] != 2]
        removed = len(p['args']) - len(others)
        p['args'] = others + [{'buffer': 0, 'location': 0, 'size': 1, 'type': 4, 'u': {'value': SCENE_CODE_TEXTURE}}]
        p['stableArgCount'] += 1 - removed
        path = project_root / 'techniquesets' / f'{shader}.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(technique, indent=2), encoding='utf-8')
        material = copy.deepcopy(donor)
        material['techniqueSet'] = shader
        for texture in material.get('textures', []):
            texture['image'] = techsets.as_reference(texture.get('image', '').removeprefix(','))
        for state in material.get('stateBits', []):
            state.update(blendOpRgb='disabled', srcBlendRgb='one', dstBlendRgb='zero', alphaTest='disabled',
                         blendOpAlpha='disabled', srcBlendAlpha='one', dstBlendAlpha='zero')
        name = overlay_name(vision)
        out = project_root / techsets.oat_material_path(name)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(material, indent=2) + '\n', encoding='utf-8')
        report['overlays'][vision] = name
        report['materials'].append(name)
        report['glow'][vision] = glow
        if glow and glow['radius'] * 0.25 * 2160 / 480 > MAX_SINGLE_PASS_RADIUS:
            notes.append('blur above 6.4977 target texels at 4K: chained passes approximated by one Gaussian')
        if notes:
            report['notes'][vision] = notes
    if native_pass.get('stableArgCount') is None:
        raise ValueError('native HUD pass lacks argument counts')
    return report
