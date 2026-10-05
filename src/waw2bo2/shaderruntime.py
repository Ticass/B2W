"""Bind translated WaW programs to measured native T6 material passes.

Common position/color/UV vertex inputs have explicit packing adapters.
Other interfaces remain unsupported until their adapters are available.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
import re
from functools import lru_cache
from pathlib import Path

from . import lightmaps, shaders, techsets


def _find(roots, relative):
    return next((root / relative for root in roots if (root / relative).is_file()), None)


@lru_cache(maxsize=2048)
def _assembly(path: Path):
    return shaders.disassemble(path.read_bytes())


def registers(assembly):
    """D3DX constant table register names and array/matrix row counts."""
    result = {}
    for name, bank, first, count in re.findall(r'^//\s+(\w+)\s+([cs])(\d+)\s+(\d+)\s*$', assembly, re.M):
        for row in range(int(count)):
            result[f'{bank}{int(first)+row}'] = (name, row)
    return result


def embed_literals(assembly, source_pass, stage):
    kind = 7 if stage == 'ps' else 1
    definitions = [f'def c{a["dest"]}, ' + ', '.join(repr(float(v)) for v in a['literal'])
                   for a in source_pass['args'] if a['type'] == kind]
    return assembly.replace(stage + '_3_0', stage + '_3_0\n' + '\n'.join(definitions), 1)


def signature(assembly, kind):
    match = re.search(r'// ' + kind + r' signature:(.*?)(?:// (?:Input|Output) signature:|\n(?:ps|vs)_\d)', assembly, re.S)
    if not match:
        raise shaders.ShaderError(f'no native {kind} signature')
    result = {}
    for name, index, mask, typ in re.findall(r'^//\s+(\w+)\s+(\d+)\s+([xyzw]+)\s+\d+\s+\w+\s+(float|uint|int)', match[1], re.M):
        result[(name + index).upper()] = (max('xyzw'.index(c)+1 for c in mask), typ)
    return result


def buffers(assembly):
    """Read D3D compiler reflection, including unused global uniform fields."""
    resources = {name: int(slot) for name, slot in re.findall(r'^//\s+(\w+)\s+cbuffer\s+NA\s+NA\s+cb(\d+)\s+\d+', assembly, re.M)}
    result = {}
    for name, body in re.findall(r'// cbuffer (\w+)\s*\n// \{(.*?)// \}', assembly, re.S):
        if name not in resources:
            continue
        for variable, count, offset, size in re.findall(r'//\s+(?:float\d?(?:x\d)?|int\d?|uint\d?)\s+(\w+)(?:\[(\d+)\])?;\s*// Offset:\s*(\d+)\s+Size:\s*(\d+)', body):
            result[variable] = (resources[name], int(offset), int(size))
    return result


def _native_program(root, stage, pass_json):
    name = pass_json['vertexShader' if stage == 'vs' else 'pixelShader'].get('name')
    if not name:
        raise shaders.ShaderError(f'native pass has no {stage} program')
    return _assembly(root / 'shader_bin' / f'{stage}_{name}.cso')


def waw_hash(name: str) -> int:
    """WaW R_HashString: djb2 with xor, case-insensitive, seed 0 (verified on
    colorMap/normalMap/envMapParms against dumped pass arguments)."""
    value = 0
    for byte in name.lower().encode():
        value = ((value * 33) ^ byte) & 0xFFFFFFFF
    return value


def t6_hash(name: str) -> int:
    """T6 R_HashString: ORs 0x20 into each byte, so '_' differs from lower()
    (verified on 175,770 stock zm_nuked material arguments)."""
    value = 0
    for byte in name.encode():
        value = ((value * 33) ^ (byte | 0x20)) & 0xFFFFFFFF
    return value


def material_literal(source_material, hash_value):
    """The source material's own value for a material constant argument."""
    for constant in (source_material or {}).get('constants', []):
        key = constant.get('nameHash')
        if key is None and constant.get('name'):
            key = waw_hash(constant['name'])
        if key == hash_value and len(constant.get('literal', [])) == 4:
            return [float(v) for v in constant['literal']]
    return None


def pixel_contract(source_assembly, source_pass, native_pass, native_root, source_material=None):
    _, ir = shaders.translate(source_assembly)
    vs = _native_program(native_root, 'vs', native_pass)
    ps = _native_program(native_root, 'ps', native_pass)
    available = signature(vs, 'Output')
    native_targets = signature(ps, 'Output')
    if set(native_targets) != {'SV_TARGET0'}:
        raise shaders.ShaderError('native pass uses a different render-target contract')
    contract = {'inputs': {}, 'outputs': {}, 'constants': {}, 'samplers': {}}
    for reg, (semantic, width) in ir['inputs'].items():
        key = semantic.upper()
        if key not in ('COLOR0', 'TEXCOORD0') or (key == 'TEXCOORD0' and width > 3):
            raise shaders.ShaderError(f'vertex adapter required for {semantic}/{width}')
        if key not in available or available[key][0] < width or available[key][1] != 'float':
            raise shaders.ShaderError(f'native vertex output cannot supply {semantic}/{width}')
        if key == 'TEXCOORD0' and width == 3 and 'fogConsts' not in buffers(vs):
            raise shaders.ShaderError('native vertex program has no verified fog varying')
        contract['inputs'][reg] = {'semantic': semantic, 'width': width}
    for reg, (semantic, width) in ir['outputs'].items():
        if semantic.upper() != 'SV_TARGET0' or width != 4:
            raise shaders.ShaderError(f'unsupported source target {semantic}')
        contract['outputs'][reg] = {'semantic': semantic, 'width': width}

    names = registers(source_assembly)
    native_fields = {**buffers(vs), **buffers(ps)}
    source_args = {f'c{a["dest"]+row}': a for a in source_pass['args'] if a['type'] in (5,6,7)
                   for row in range(a.get('rowCount', 1))}
    for reg in ir['constants']:
        arg = source_args.get(reg)
        if arg is None:
            raise shaders.ShaderError(f'no original pass argument for {reg}')
        if arg['type'] == 5:
            if reg not in names or names[reg][0] not in native_fields:
                raise shaders.ShaderError(f'no native code uniform for {reg}: {names.get(reg)}')
            name, row = names[reg]
            row = arg.get('firstRow', 0) + int(reg[1:]) - arg['dest']
            slot, offset, size = native_fields[name]
            # PerScene is globally maintained by T6. Other buffers need the
            # donor pass's explicit argument routing and cannot be assumed live.
            if slot != 0 or offset % 16 or size < 16 * (row+1):
                raise shaders.ShaderError(f'code uniform requires an adapter: {name}')
            contract['constants'][reg] = {'buffer': slot, 'index': offset//16 + row}
        elif arg['type'] == 6:
            found = next((a for a in native_pass['args'] if a['type'] == 6 and a['u'].get('value') == arg['value']), None)
            if found is None or found['location'] % 16 or found['size'] < 16:
                # The donor has no slot: embed this material's own WaW value.
                literal = material_literal(source_material, arg['value'])
                if literal is None:
                    raise shaders.ShaderError(f'native material constant binding absent for {reg}')
                contract['constants'][reg] = {'expr': 'float4(' + ', '.join(repr(v) for v in literal) + ')', 'uses': []}
                contract.setdefault('embedded_material_constants', []).append(arg['value'])
                continue
            contract['constants'][reg] = {'buffer': found['buffer'], 'index': found['location']//16}
        else:
            raise shaders.ShaderError(f'original literal argument needs source embedding: {reg}')

    texture_args = {f's{a["dest"]}': a for a in source_pass['args'] if a['type'] in (2,4)}
    for reg, dim in ir['samplers'].items():
        arg = texture_args.get(reg)
        if arg is None or arg['type'] != 2:
            raise shaders.ShaderError(f'code texture adapter required for {reg}')
        found = next((a for a in native_pass['args'] if a['type'] == 2 and a['u'].get('value') == arg['value']), None)
        if found is None or dim != '2d':
            raise shaders.ShaderError(f'native material texture binding absent for {reg}/{dim}')
        contract['samplers'][reg] = {'texture': found['location'] & 255, 'sampler': found['location'] >> 8}
    return contract


# WaW code uniforms whose T6 counterpart has another name. Each pair is
# measured: same producer role and same consumer arithmetic in both engines.
#  baseLightingCoords (WaW lp_* VS, passed to the PS and added to the
#  modelLightingSampler lookup) == gridLightingCoordsAndVis (T6 lprobe VS
#  cb3[4] -> TEXCOORD6, added to the same 3D lookup in the lprobe PS).
#  sunSpecular (WaW world sun PS: specular = sunSpecular * envMapParms.w)
#  has no T6 field; the T6 world sun PS drives its sun specular with
#  sunDiffuse (cb0[19] in both its diffuse and specular terms).
CODE_UNIFORM_ALIASES = {'baseLightingCoords': 'gridLightingCoordsAndVis', 'sunSpecular': 'sunDiffuse'}
# WaW-only code uniforms with a measured neutral value in T6, where the WaW
# system they drive does not exist. destructibleParms.z fades vertex alpha
# of WaW destructible pieces (o.w = v.w - v.w * z); 0 keeps it unchanged.
NEUTRAL_CODE_UNIFORMS = {'destructibleParms': 'float4(0, 0, 0, 0)',
                         # per-entity burn amount of WaW "charred" programs; static
                         # models carry none (unburnt) and no T6 pass routes one
                         '__characterCharredAmount': 'float4(0, 0, 0, 0)'}
# Vertex inputs WaW reads as biased UBYTE4 unit vectors (NORMAL, tangent in
# TEXCOORD2); T6 feeds decoded floats. Checked against the decode literals.
PACKED_VECTOR_INPUTS = ('NORMAL0', 'TEXCOORD2')


def resource_names(assembly):
    """T6 reflection: texture slot -> (name, dimension)."""
    return {int(slot): (name, dim) for name, dim, slot in
            re.findall(r'^//\s+(\w+)\s+texture\s+\w+\s+(\w+)\s+t(\d+)\s+\d+', assembly, re.M)}


# T6 composites the scene as sqrt(4 * buffer^2 + bloom) before its shoulder and
# LUT (every hdr_bloom_apply variant), so the screen shows twice the buffer.
# WaW programs compute display colour: they write half of it, and quantities
# they take from T6 linear data are converted to display units, sqrt(4 hdr L).
DISPLAY_SCALE = 4.0
WAW_OUTPUT_SCALE = 0.5


def hdr_display(row):
    return f'({DISPLAY_SCALE!r} * t6_cb0[{row}].x)'


def scales_output(state):
    """Halving the source colour is exact unless the blend uses it as a
    multiplier of the destination (multiply / 2x multiply decals)."""
    if not state or state.get('blendOpRgb', 'disabled') == 'disabled':
        return True
    return (state.get('srcBlendRgb') not in ('destcolor', 'invdestcolor')
            and state.get('dstBlendRgb') not in ('srccolor', 'invsrccolor'))


def native_sqrt_hdr_output(native_ps):
    """True when the donor pixel program writes sqrt(hdrControl0.x * linear).

    Measured on T6 lit programs (e.g. lprobe): linear colour, fog lerp in
    linear space, * hdrControl0.x, then ``sqrt o0.xyz``. WaW programs write
    gamma directly, so WaW inputs taken from T6 linear data need converting.
    """
    used = re.search(r'float4 hdrControl0;\s*// Offset:\s*320 Size:\s*16\s*\n', native_ps)
    return bool(used) and bool(re.search(r'^sqrt o0\.xyz', native_ps, re.M))


# Imported primary lights make spot/omni passes reachable as well as sun.
# Every reachable lightmap reader must agree on the surface page encoding.
# Spots are staged as plain T6 spots (lighting.t6_light_fields), so the
# SPOT_SQUARE/SPOT_ROUND techniques 9-12 and 20-23 are never selected.
REACHABLE_LIT_SLOTS = (4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 24, 25)
# Spot/omni shadowed variants: T6 draws them only for a light holding a shadow
# map this frame (sub_7836F0), never for lights staged without canUseShadowMap.
SHADOWED_LIT_SLOTS = (8, 14, 19, 25)
# T6 *_DLIGHT_GLIGHT techniques (a dynamic light also reaches the surface) and
# the technique they extend. They read the surface lightmap too; without a
# dynamic-light adapter they draw the WaW program of their base technique.
DLIGHT_BASE_SLOTS = {15: 4, 16: 5, 17: 6, 18: 7, 19: 8, 24: 13, 25: 14}
# T6 world lit draws bind these per-surface textures at fixed slots, outside
# the pass arguments (stock world lit programs: lightmap t13, probe t15 cube).
WORLD_SURFACE_TEXTURE_SLOTS = {'lightmapSamplerSecondary': (13, '2d'), 'reflectionProbeSampler': (15, 'cube')}
# T6 binds a per-surface texture only for a pass whose customSamplerFlags has
# its bit (measured on all 8003 stock passes: bit 0 exactly when the program
# reads reflectionProbeSampler, bit 1 exactly when it reads
# lightmapSamplerSecondary); without the bit the slot keeps a stale texture.
CUSTOM_SAMPLER_BITS = {'reflectionProbeSampler': 1, 'lightmapSamplerSecondary': 2}
# Samplers both engines bind per surface, outside the pass arguments.
PER_SURFACE_SAMPLERS = {'lightmapSamplerPrimary', 'lightmapSamplerSecondary', 'reflectionProbeSampler'}


# WaW primary-light code uniforms (WaW sub_742400, sub_7425D0) rebuilt from the
# T6 per-light uniforms (T6 sub_782FA0). T6 keeps the light position in the
# same camera-relative space; the rest are T6-derived values whose ComPrimaryLight
# inputs are staged so they carry WaW's quantities (lighting.t6_light_fields):
#   falloff = (near, radius, near, 0)  -> lightFallOffA.w = -1/radius
#   aAbB = (cosInner, cosOuter, 1/exponent, 0) -> lightFallOffA.xy, lightFallOffB.x
#     (spot lights; WaW's spot factors are 1/(cosIn-cosOut), -cosOut/(cosIn-cosOut))
# r_diffuseColorScale and r_specularColorScale both default to 1 in WaW, so the
# diffuse and specular colours are the light colour.
LIGHT_ADAPTERS = {
    'lightPosition': 'lightPosition: T6 xyz, w = 1/radius from lightFallOffA.w',
    'lightDiffuse': 'lightDiffuse: sqrt(4 hdr * linear)',
    'lightSpecular': 'lightSpecular: light colour, sqrt(4 hdr * linear)',
    'lightSpotDir': 'lightSpotDir: T6 xyz',
    'lightSpotFactors': 'lightSpotFactors: from lightFallOffA/B (staged aAbB)',
    'lightFalloffPlacement': 'lightFalloffPlacement: light def lightmap lookup row',
    'spotShadowmapPixelAdjust': 'spotShadowmapPixelAdjust: WaW tap pattern in T6 tile units (derived)',
}


# T6 code constants of the per-light uniforms (sub_782FA0 writes them at
# source + 2048 + 16 * index). A pass copies each into its constant buffer
# through a type-5 argument (value 0x01000000 + index, location = byte offset):
# they are not global, so a pass must carry every row its program reads.
LIGHT_CODE_CONSTANTS = {'lightPosition': 0, 'lightDiffuse': 1, 'lightSpotDir': 2, 'lightSpotFactors': 3,
                        'lightFallOffA': 5, 'lightFallOffB': 6, 'spotShadowmapPixelAdjust': 60}
# WaW spot shadow taps (sub_737E60, 1024 tiles): offsets (1/4096, 1/4096) and
# (1/2048, -1/8192) = (0.25, 0.25) and (0.5, -0.125) per 1/1024 tile unit. T6
# (sub_76A7A0) stores (1/S, 1/S, 0, 0) for its tile size S: the WaW pattern is
# expressed in that unit (derived tap spacing, reported as such).
WAW_SPOT_SHADOW_TAPS = (0.25, 0.25, 0.5, -0.125)


def ensure_code_constant_arg(native_pass, field, location):
    value = 0x01000000 + LIGHT_CODE_CONSTANTS[field]
    if any(a['type'] == 5 and a['u']['value'] == value for a in native_pass['args']):
        return
    if any(a['type'] == 5 and a['location'] == location for a in native_pass['args']):
        raise shaders.ShaderError(f'native constant row of {field} carries another code constant')
    start = native_pass['perPrimArgCount'] + native_pass['perObjArgCount']
    stable = native_pass['args'][start:]
    position = start + next((i for i, a in enumerate(stable) if a['type'] == 5 and a['location'] > location), len(stable))
    native_pass['args'].insert(position, {'buffer': 0, 'location': location, 'size': 16, 'type': 5, 'u': {'value': value}})
    native_pass['stableArgCount'] += 1


def light_constant(name, native_fields, hdr_row, falloff_placement, native_pass=None):
    def row(field):
        found = native_fields.get(field)
        if found is None or found[0] != 0 or found[1] % 16:
            raise shaders.ShaderError(f'native light uniform absent: {field}')
        if native_pass is not None:
            ensure_code_constant_arg(native_pass, field, found[1])
        return found[1] // 16
    if name == 'lightFalloffPlacement':
        if falloff_placement is None:
            raise shaders.ShaderError('lightFalloffPlacement needs one light def shared by all local lights')
        return {'expr': 'float4(' + ', '.join(repr(float(v)) for v in falloff_placement) + ')', 'uses': []}
    if name in ('lightDiffuse', 'lightSpecular'):
        if hdr_row is None:
            raise shaders.ShaderError('native light colour units not recognized')
        diffuse = row('lightDiffuse')
        return {'expr': f'float4(sqrt(max(t6_cb0[{diffuse}].xyz * {hdr_display(hdr_row)}, 0)), 1.0)',
                'uses': [[0, diffuse], [0, hdr_row]]}
    if name == 'spotShadowmapPixelAdjust':
        adjust = row('spotShadowmapPixelAdjust')
        taps = WAW_SPOT_SHADOW_TAPS
        return {'expr': f'(t6_cb0[{adjust}].xyxy * float4({taps[0]!r}, {taps[1]!r}, {taps[2]!r}, {taps[3]!r}))',
                'uses': [[0, adjust]]}
    if name == 'lightSpotDir':
        spot = row('lightSpotDir')
        return {'expr': f'float4(t6_cb0[{spot}].xyz, 0.0)', 'uses': [[0, spot]]}
    if name == 'lightPosition':
        position, a = row('lightPosition'), row('lightFallOffA')
        return {'expr': f'float4(t6_cb0[{position}].xyz, -t6_cb0[{a}].w)', 'uses': [[0, position], [0, a]]}
    # .w: the shadow fade both engines pass to the setter (WaW sub_742400 a5,
    # T6 sub_782FA0 a5), read by the shadowed spot programs
    a, b, factors = row('lightFallOffA'), row('lightFallOffB'), row('lightSpotFactors')
    return {'expr': f'float4(t6_cb0[{a}].x, t6_cb0[{b}].x, t6_cb0[{a}].y, t6_cb0[{factors}].w)',
            'uses': [[0, a], [0, b], [0, factors]]}


def light_falloff_placement(primary_lights: Path, roots) -> tuple[float, float, float, float] | None:
    """WaW lightFalloffPlacement (sub_7425D0: attenuation width / 512, 0,
    lmapLookupStart / 512, 0) for the light def every local light shares,
    from the source primary-light table, light def and images.

    WaW bakes each def's attenuation ramp into row 0 of every secondary
    lightmap page, starting at ``lmapLookupStart``, and samples it at v = 0;
    the centre of that row is used (equal to WaW's clamped lookup).
    None when the local lights do not share one def or a source is missing."""
    from . import iwi
    try:
        lights = json.loads(Path(primary_lights).read_text())['lights']
        defs = {l['defName'] for l in lights if l.get('type', 0) > 1 and l.get('defName')}
        if len(defs) != 1:
            return None
        definition = json.loads(_find(roots, Path(f'lightdef/{next(iter(defs))}.json')).read_text())
        attenuation = _find(roots, Path(f'images/{definition["attenuation"].removeprefix(",")}.dds'))
        page = _find(roots, Path('images/_lightmap0_secondary.dds'))
        width = iwi.read_dds(attenuation.read_bytes()).width
        page_width = iwi.read_dds(page.read_bytes()).width
    except (OSError, KeyError, ValueError, AttributeError, TypeError, iwi.IwiError):
        return None
    return (width / 512.0, 0.0, definition['lmapLookupStart'] / 512.0, 0.5 / (2 * page_width))


# T6 code textures bound through type-4 pass arguments (value = T6 code
# texture index, measured on stock spot passes: attenuation 15 at slot 12).
CODE_TEXTURE_ARGS = {'attenuationSampler': 15}


def _free_texture_slot(native_pass, native_ps):
    used = set(WORLD_SURFACE_TEXTURE_SLOTS[k][0] for k in WORLD_SURFACE_TEXTURE_SLOTS) | {14}
    for a in native_pass['args']:
        if a['type'] in (2, 4):
            used |= {a['location'] & 255, a['location'] >> 8}
    used |= set(resource_names(native_ps)) | {int(i) for i in re.findall(r'^dcl_sampler s(\d+),', native_ps, re.M)}
    slot = next((i for i in range(16) if i not in used), None)
    if slot is None:
        raise shaders.ShaderError('no free texture slot')
    return slot


def add_code_texture(native_pass, code, native_ps):
    """Bind T6 code texture ``code`` for a translated program: a type-4
    argument in the stable group after the material textures (stock order)."""
    found = next((a for a in native_pass['args'] if a['type'] == 4 and a['u']['value'] == code), None)
    if found is not None:
        return found['location'] & 255
    slot = _free_texture_slot(native_pass, native_ps)
    start = native_pass['perPrimArgCount'] + native_pass['perObjArgCount']
    stable = native_pass['args'][start:]
    position = start + next((i for i, a in enumerate(stable) if a['type'] not in (2, 4)), len(stable))
    native_pass['args'].insert(position, {'buffer': 0, 'location': slot | (slot << 8), 'size': 1, 'type': 4,
                                          'u': {'value': code}})
    native_pass['stableArgCount'] += 1
    return slot


def add_material_texture(native_pass, output_material, source_material, hash_value, native_ps):
    """Bind a source material texture the donor pass has no slot for.

    The donor matched with that map dropped (e.g. a layer specular map) while
    the original program samples it. The texture joins the output material and
    a type-2 argument joins the pass's stable group at a free texture/sampler
    slot. T6 sub_77C150 scans the texture table without an end bound from the
    previous match, so both stay ordered by name hash (as in stock data).
    Returns the new argument, or None when the source has no such texture."""
    texture = next((t for t in (source_material or {}).get('textures', [])
                    if t.get('name') and waw_hash(t['name']) == hash_value and t.get('image')), None)
    if texture is None or t6_hash(texture['name']) != hash_value:
        return None
    textures = output_material.setdefault('textures', [])
    if not any(t.get('name') == texture['name'] for t in textures):
        entry = {k: copy.deepcopy(v) for k, v in texture.items() if k in ('image', 'isMatureContent', 'name', 'samplerState', 'semantic')}
        textures.append(entry)
        textures.sort(key=lambda t: t6_hash(t.get('name', '')))
    slot = _free_texture_slot(native_pass, native_ps)
    arg = {'buffer': 0, 'location': slot | (slot << 8), 'size': 1, 'type': 2, 'u': {'value': hash_value}}
    start = native_pass['perPrimArgCount'] + native_pass['perObjArgCount']
    stable = native_pass['args'][start:]
    position = start + next((i for i, a in enumerate(stable) if a['type'] == 2 and a['u']['value'] > hash_value), len(stable))
    native_pass['args'].insert(position, arg)
    native_pass['stableArgCount'] += 1
    return arg


def paired_pixel_contract(source_assembly, source_pass, native_pass, native_root, linked, visibility=None,
                          source_material=None, falloff_placement=None, output_material=None):
    """Contract for a WaW pixel program fed by its own translated vertex program.

    ``linked`` is the translated vertex output signature, in register order;
    ``visibility`` names the linked varying whose .w is T6 sun visibility.
    """
    _, ir = shaders.translate(source_assembly)
    vs = _native_program(native_root, 'vs', native_pass)
    ps = _native_program(native_root, 'ps', native_pass)
    if set(signature(ps, 'Output')) != {'SV_TARGET0'}:
        raise shaders.ShaderError('native pass uses a different render-target contract')
    provided = {s['semantic'].upper(): s['width'] for s in linked}
    contract = {'inputs': {}, 'outputs': {}, 'constants': {}, 'samplers': {}, 'input_signature': linked}
    for reg, (semantic, width) in ir['inputs'].items():
        if provided.get(semantic.upper(), 0) < width:
            raise shaders.ShaderError(f'translated vertex program does not supply {semantic}/{width}')
        contract['inputs'][reg] = {'semantic': semantic, 'width': width}
    for reg, (semantic, width) in ir['outputs'].items():
        if semantic.upper() != 'SV_TARGET0' or width != 4:
            raise shaders.ShaderError(f'unsupported source target {semantic}')
        contract['outputs'][reg] = {'semantic': semantic, 'width': width}

    names = registers(source_assembly)
    native_fields = {**buffers(vs), **buffers(ps)}
    sqrt_hdr = native_sqrt_hdr_output(ps)
    hdr = native_fields.get('hdrControl0')
    if sqrt_hdr and (hdr is None or hdr[0] != 0 or hdr[1] % 16):
        raise shaders.ShaderError('native hdr scale not addressable')
    hdr_row = hdr[1] // 16 if sqrt_hdr else None
    source_args = {f'c{a["dest"]+row}': a for a in source_pass['args'] if a['type'] in (5, 6, 7)
                   for row in range(a.get('rowCount', 1))}
    for reg in ir['constants']:
        arg = source_args.get(reg)
        if arg is None:
            raise shaders.ShaderError(f'no original pass argument for {reg}')
        if arg['type'] == 6:
            found = next((a for a in native_pass['args'] if a['type'] == 6 and a['u'].get('value') == arg['value']), None)
            if found is None or found['location'] % 16 or found['size'] < 16:
                # The donor has no slot: embed this material's own WaW value.
                literal = material_literal(source_material, arg['value'])
                if literal is None:
                    raise shaders.ShaderError(f'native material constant binding absent for {reg}')
                contract['constants'][reg] = {'expr': 'float4(' + ', '.join(repr(v) for v in literal) + ')', 'uses': []}
                contract.setdefault('embedded_material_constants', []).append(arg['value'])
                continue
            contract['constants'][reg] = {'buffer': found['buffer'], 'index': found['location']//16}
            continue
        if arg['type'] != 5:
            raise shaders.ShaderError(f'original literal argument needs source embedding: {reg}')
        name = names.get(reg, (None, 0))[0]
        if name == 'gameTime' and name not in native_fields:
            # T6 CONST_SRC_CODE_GAMETIME (0x19): stock pixel passes route it to
            # buffer 2 row 4 (type 5, location 64; 382 stock passes)
            binding = {'type': 5, 'buffer': 2, 'location': 64, 'size': 16, 'u': {'value': 0x01000019}}
            if binding not in native_pass['args']:
                if any(a['type'] == 5 and a['buffer'] == 2 and a['location'] == 64 for a in native_pass['args']):
                    raise shaders.ShaderError('pixel game time row is taken')
                native_pass['args'].append(binding)
                native_pass['stableArgCount'] += 1
            contract['constants'][reg] = {'buffer': 2, 'index': 4}
            contract.setdefault('unit_conversions', []).append('gameTime: T6 code constant 0x19')
            continue
        if name in LIGHT_ADAPTERS:
            contract['constants'][reg] = light_constant(name, native_fields, hdr_row if sqrt_hdr else None,
                                                        falloff_placement, native_pass)
            contract.setdefault('unit_conversions', []).append(LIGHT_ADAPTERS[name])
            continue
        name = CODE_UNIFORM_ALIASES.get(name, name) if name not in native_fields else name
        if name not in native_fields:
            raise shaders.ShaderError(f'no native code uniform for {reg}: {names.get(reg)}')
        row = arg.get('firstRow', 0) + int(reg[1:]) - arg['dest']
        slot, offset, size = native_fields[name]
        if slot != 0 or offset % 16 or size < 16 * (row+1):
            raise shaders.ShaderError(f'code uniform requires an adapter: {name}')
        index = offset // 16 + row
        if name in ('fogColor', 'sunDiffuse') and sqrt_hdr:  # includes aliased sunSpecular
            # T6 keeps these linear (summed/lerped before * hdr, sqrt); WaW's
            # programs sum them in gamma space. Convert the quantity itself.
            contract['constants'][reg] = {
                'expr': f'float4(sqrt(max(t6_cb0[{index}].xyz * {hdr_display(hdr_row)}, 0)), t6_cb0[{index}].w)',
                'uses': [[0, index], [0, hdr_row]]}
            contract.setdefault('unit_conversions', []).append(f'{name}: sqrt(4 hdr * linear)')
        else:
            contract['constants'][reg] = {'buffer': slot, 'index': index}

    native_textures = resource_names(ps)
    source_samplers = {f's{a["dest"]}': a for a in source_pass['args'] if a['type'] in (2, 4)}
    for reg, dim in ir['samplers'].items():
        arg = source_samplers.get(reg)
        source_name = names.get(reg, (None, 0))[0]
        if arg is None and source_name not in PER_SURFACE_SAMPLERS:
            raise shaders.ShaderError(f'no original sampler argument for {reg}')
        if arg is not None and arg['type'] == 2:
            found = next((a for a in native_pass['args'] if a['type'] == 2 and a['u'].get('value') == arg['value']), None)
            added = None
            if found is None and output_material is not None:
                found = added = add_material_texture(native_pass, output_material, source_material, arg['value'], ps)
            if found is None:
                raise shaders.ShaderError(f'native texture binding absent for {reg} ({source_name})')
            texture, sampler = found['location'] & 255, found['location'] >> 8
            if added is not None:
                native_textures = {**native_textures, texture: (source_name, '2d')}  # material maps are 2D
        else:
            # Code textures: enums differ between the engines (sun shadow map:
            # WaW 7, T6 6), and per-surface ones (lightmap, reflection probe)
            # are bound outside the pass arguments in both. The engine-filled
            # resource is identified by its reflected name in both programs.
            # WaW's primary lightmap (sun visibility) lives in the third of the
            # WaW-encoded page bound as T6's single lightmap (see lightmaps.py).
            if (source_name or '').startswith('terrainScorchTextureSampler') and is_scorch_program(source_material):
                contract['samplers'][reg] = {'texture': 0, 'sampler': 0, 'constant': list(SCORCH_NEUTRAL)}
                contract.setdefault('unit_conversions', []).append(f'{source_name}: unscorched (weight 0)')
                continue
            lookup = 'lightmapSamplerSecondary' if source_name in lightmaps.WAW_PAGE_UV else source_name
            slots = [t for t, (n, _) in native_textures.items() if n == lookup]
            if not slots and lookup in CODE_TEXTURE_ARGS:
                # the engine fills this code texture for every draw (T6 sub_782FA0
                # sets the light's attenuation); the donor program just never read it
                slots = [add_code_texture(native_pass, CODE_TEXTURE_ARGS[lookup], ps)]
                native_textures = {**native_textures, slots[0]: (lookup, '2d')}
            elif not slots and lookup in WORLD_SURFACE_TEXTURE_SLOTS and sqrt_hdr and native_fields.get('hdrControl0'):
                # per-surface textures sit at fixed slots in every world lit
                # draw; the engine fills them when the pass flags ask for them
                # (CUSTOM_SAMPLER_BITS, set below for every per-surface read)
                slots = [WORLD_SURFACE_TEXTURE_SLOTS[lookup][0]]
                native_textures = {**native_textures, slots[0]: (lookup, WORLD_SURFACE_TEXTURE_SLOTS[lookup][1])}
            elif len(slots) != 1 or not re.search(rf'^dcl_sampler s{slots[0]},', ps, re.M):
                raise shaders.ShaderError(f'native code texture absent for {reg} ({source_name})')
            texture = sampler = slots[0]
            arg = {'type': 4}
        native_name, native_dim = native_textures.get(texture, (None, None))
        source_sky_texture = arg['type'] == 2 and (source_material or {}).get('techniqueSet', '').startswith('mc_sky')
        if not source_sky_texture and {'2d': '2d', 'volume': '3d', 'cube': 'cube'}[dim] != native_dim:
            raise shaders.ShaderError(f'texture dimension differs for {reg}: {dim} vs {native_dim}')
        entry = {'texture': texture, 'sampler': sampler}
        if arg['type'] == 4:
            if source_name in ('shadowmapSamplerSun', 'shadowmapSamplerSpot'):
                # Same lookup matrix/partition scheme in both engines. WaW point-
                # samples depth and compares itself; T6 binds a comparison sampler
                # there, so read texels without it (exact for point sampling).
                if not re.search(rf'^dcl_sampler s{sampler}, mode_comparison', ps, re.M):
                    raise shaders.ShaderError(f'native {source_name} sampler mode not recognized')
                entry['point_load'] = True
                contract.setdefault('unit_conversions', []).append(f'{source_name}: texel load at mip 0')
                if os.environ.get('WAW2BO2_DIAG_SHADOW_LIT'):
                    entry['constant'] = [1.0, 1.0, 1.0, 1.0]  # diagnostic: shadow test always lit
            elif source_name == 'floatZSampler':
                # Native soft-particle PS decodes reciprocal depth as zNear.x/abs(z).
                # WaW soft-particle PS consumes camera-space depth directly.
                near = native_fields.get('zNear')
                if near is None or near[0] != 0 or near[1] % 16:
                    raise shaders.ShaderError('native reciprocal-depth scale absent')
                near_row = near[1] // 16
                if not re.search(rf'div r\d+\.x, cb0\[{near_row}\]\.x, r\d+\.x', ps):
                    raise shaders.ShaderError('native soft-particle depth encoding unrecognized')
                entry.update(texel_expr=f'float4(t6_cb0[{near_row}].x / max(abs(T.x), 1e-20), T.yzw)', uses=[[0, near_row]])
                contract.setdefault('unit_conversions', []).append('floatZ: reciprocal to camera depth')
            elif source_name == 'modelLightingSampler' and sqrt_hdr:
                # Measured: T6 lprobe = sqrt(hdr * 32 * L^2 * C^2) = sqrt(32 hdr) L C,
                # displayed at twice that; WaW = 2 L' C, so L' = sqrt(8 * 4 hdr) L.
                if not re.search(r'(?:mul|mad) r\d+\.\w+, r\d+\.\w+, l\((?:[^)]*32\.000000){3}[^)]*\)', ps):
                    raise shaders.ShaderError('native model lighting scale not recognized')
                entry.update(rgb_scale=f'sqrt(8.0 * {hdr_display(hdr_row)})', uses=[[0, hdr_row]])
                contract.setdefault('unit_conversions', []).append('modelLighting: sqrt(8 * 4 hdr) * L')
                if 'sunDiffuse' in {n for n, _ in names.values()}:
                    # WaW reads sun visibility from the texel alpha; T6 from
                    # gridLightingCoordsAndVis.w, carried by the vertex program.
                    link = next((i for i, s in enumerate(linked) if visibility and s['semantic'].upper() == visibility.upper()
                                 and s['width'] == 4), None)
                    if link is None:
                        raise shaders.ShaderError('sun visibility varying absent')
                    entry['alpha_expr'] = f'input.link{link}.w'
            elif source_name == 'attenuationSampler':
                # T6 code texture 15 is the light def's attenuation image, staged
                # from the WaW light def with its sampler state: WaW reads the
                # same texel (T6 programs square it; WaW uses it directly).
                pass
            elif source_name in lightmaps.WAW_PAGE_UV:
                # WaW and T6 lightmap pages are encoded differently (WaW: two
                # halves, colour + tangent-space direction in alpha, plus a
                # primary sun visibility texture; T6: three thirds, rgb/a + world
                # direction). WaW programs read the WaW-encoded page, selected
                # through the surface's lightmapIndex, at their exact sub-page.
                entry['v_scale_offset'] = list(lightmaps.WAW_PAGE_UV[source_name])
                contract['lightmap'] = 'waw'
            elif source_name == 'reflectionProbeSampler' and sqrt_hdr:
                # Measured T6 world lit PS: probe = rgb / (a + 1e-6), linear.
                # WaW multiplies rgb by alpha in gamma space: supply a = 1.
                # (A donor that never samples the probe has nothing to check.)
                donor_samples_probe = 'reflectionProbeSampler' in {n for n, _ in resource_names(ps).values()}
                if donor_samples_probe and not re.search(r'add r\d+\.[xyzw], r\d+\.w, l\(0\.000001\)', ps):
                    raise shaders.ShaderError('native reflection probe encoding not recognized')
                entry.update(texel_expr=f'float4(sqrt(max(T.xyz / (T.w + 0.000001) * {hdr_display(hdr_row)}, 0)), 1.0)',
                             uses=[[0, hdr_row]])
                contract.setdefault('unit_conversions', []).append('reflectionProbe: sqrt(4 hdr * rgb / a)')
            else:
                raise shaders.ShaderError(f'code texture {source_name} needs a unit adapter')
        contract['samplers'][reg] = entry
        if arg is None or arg['type'] == 4:
            bit = CUSTOM_SAMPLER_BITS.get('lightmapSamplerSecondary' if source_name in lightmaps.WAW_PAGE_UV else source_name)
            if bit:
                native_pass['customSamplerFlags'] = native_pass.get('customSamplerFlags', 0) | bit
    return contract


# Extra-layer streams of layered world techniques (MTL_WORLDVERT_TEX_*):
# layer texcoords and 2x2 layer normal transforms. The bridge writes T6 vd1 so
# each carries WaW's value (half2 texcoords, transform bytes swapped for T6's
# B8G8R8A8 read); WaW and T6 route them to the same TEXCOORD destinations.
LAYER_STREAM_INPUTS = ('TEXCOORD3', 'TEXCOORD4', 'TEXCOORD5', 'TEXCOORD6')
# WaW "_sco" world programs add dynamic terrain scorch (burn marks after
# explosions): the vertex program weights terrainScorchTextureSampler* by
# (v.x > 0) * sat((gameTime.w - v.x) / 3) * v.y from a BLENDWEIGHT stream the
# engine fills at runtime and keeps zero on an unscorched world. T6 has no such
# system: that zero state is supplied, so the scorch term vanishes exactly as
# on a fresh WaW map (and gameTime cannot affect it).
SCORCH_NEUTRAL = (0.0, 0.0, 0.0, 0.0)


def is_scorch_program(source_material):
    return '_sco' in (source_material or {}).get('techniqueSet', '')


def vertex_contract(source_assembly, source_pass, native_pass, native_root, paired=False, source_material=None,
                    layered=False):
    _, ir = shaders.translate(source_assembly)
    native_vs = _native_program(native_root, 'vs', native_pass)
    incoming = signature(native_vs, 'Input')
    # The native sky program does not consume the model's stored vertex color.
    # Original sky layers do; GfxPackedVertex still supplies that COLOR0 field.
    if paired and source_material and source_material.get('techniqueSet', '').startswith('mc_sky'):
        incoming.setdefault('COLOR0', (4, 'float'))
        incoming.setdefault('TEXCOORD0', (2, 'float'))
    fields = buffers(native_vs)
    names = registers(source_assembly)
    contract = {'inputs': {}, 'outputs': {}, 'constants': {}, 'samplers': {}}
    if paired and source_material and source_material.get('techniqueSet', '').startswith('mc_sky'):
        # Source sky fog uses a normalized direction at a shader-defined
        # distance, independent of the dome's physical radius.
        match = re.search(r'nrm (r\d+)\.xyz, v\d+\s*\n\s*mul \1\.xyz, \1, (c\d+)\.([xyzw])', source_assembly)
        if match:
            literal = re.search(rf'def {match[2]}, ([^\n]+)', source_assembly)
            if literal:
                contract['sky_fog_distance'] = float(literal[1].split(',')['xyzw'.index(match[3])])
    for reg, (semantic, width) in ir['inputs'].items():
        key = semantic.upper()
        allowed = (('POSITION0', 'COLOR0', 'TEXCOORD0') + (PACKED_VECTOR_INPUTS if paired else ())
                   + (LAYER_STREAM_INPUTS if paired and layered else ()))
        if key == 'BLENDWEIGHT0' and paired and is_scorch_program(source_material):
            contract['inputs'][reg] = {'semantic': semantic, 'width': width, 'adapter': 'waw_neutral',
                                       'constant': list(SCORCH_NEUTRAL)}
            contract.setdefault('neutral', []).append('terrain scorch weights: unscorched (0)')
            continue
        if key not in allowed or key not in incoming:
            raise shaders.ShaderError(f'packed vertex adapter required for {semantic}')
        native_width, typ = incoming[key]
        if typ != 'float':
            raise shaders.ShaderError(f'unsupported native attribute type {semantic}/{typ}')
        entry = {'semantic': semantic, 'width': width}
        if key in PACKED_VECTOR_INPUTS:
            if width != 4 or native_width != 3 or not all(v in source_assembly for v in ('0.00787401572', '0.752941191')):
                raise shaders.ShaderError(f'unrecognized packed vector encoding for {semantic}')
            entry.update(width=3, adapter='waw_ubyte4_vector')
        elif key == 'TEXCOORD0' and width == 4 and native_width >= 2:
            # Measured original dtex decoder consumes four bytes of VU half
            # words. Reconstruct those bytes from T6's decoded half-float UV.
            if all(v in source_assembly for v in ('0.0009765625', '0.0078125', '3.05175781e-005')):
                entry.update(width=2, adapter='waw_half_uv')
            elif (reads := [line.strip() for line in source_assembly.splitlines()
                            if not line.strip().startswith(('//', 'dcl_'))
                            and re.search(r'\b' + re.escape(reg) + r'\b', line)]) and all(
                    re.fullmatch(r'(?:mov\s+o\d+\.xy,\s*|add\s+r\d+\.xy,\s*r\d+(?:\.xy)?,\s*-?)'
                                 + re.escape(reg) + r'(?:\.xy)?', line)
                    for line in reads):
                # World unlit passes can also subtract UV.xy from the camera
                # position for distance falloff. Both forms consume only xy.
                # No half-byte decoder and no lightmap stream are involved.
                entry.update(width=2, adapter='waw_uv')
            elif paired and incoming.get('TEXCOORD1', (0, ''))[0] >= 2 and incoming['TEXCOORD1'][1] == 'float':
                # WaW world vertex: float4 texcoord = texture UV + lightmap UV
                # (lm_* pixel programs sample the lightmap at .zw). T6 world
                # passes stream the lightmap UV as TEXCOORD1.
                entry.update(width=2, adapter='waw_uv_lightmap')
            else:
                raise shaders.ShaderError('unrecognized legacy UV packing')
        elif key == 'POSITION0' and width == 4 and native_width == 3:
            entry.update(width=3, adapter='waw_position')
        elif native_width < width:
            raise shaders.ShaderError(f'native input cannot supply {semantic}/{width}')
        contract['inputs'][reg] = entry
    for reg, (semantic, width) in ir['outputs'].items():
        # Paired with its own translated pixel program, the varyings are WaW's.
        if not paired and semantic.upper() not in ('SV_POSITION', 'COLOR0', 'TEXCOORD0'):
            raise shaders.ShaderError(f'vertex varying adapter required for {semantic}')
        contract['outputs'][reg] = {'semantic': semantic, 'width': width}
    for reg in ir['constants']:
        material_arg = next((a for a in source_pass['args'] if a['type'] == 0 and a['dest'] == int(reg[1:])), None)
        if material_arg is not None:
            literal = material_literal(source_material, material_arg['value'])
            if literal is None:
                raise shaders.ShaderError(f'original vertex material constant absent for {reg}')
            contract['constants'][reg] = {'expr': 'float4(' + ', '.join(repr(v) for v in literal) + ')', 'uses': []}
            continue
        arg = next((a for a in source_pass['args'] if a['type'] == 3 and a['dest'] <= int(reg[1:]) < a['dest'] + a['rowCount']), None)
        source_name = names.get(reg, (None, 0))[0]
        if arg is not None and source_name == 'gameTime' and source_material and source_material.get('techniqueSet', '').startswith('mc_sky'):
            # T6 CONST_SRC_CODE_GAMETIME (0x19), routed to a free material row.
            # This sky donor omits time because its original variant is static.
            binding = {'type': 3, 'buffer': 2, 'location': 240, 'size': 16, 'u': {'value': 0x01000019}}
            if binding not in native_pass['args']:
                native_pass['args'].append(binding)
                native_pass['stableArgCount'] = native_pass.get('stableArgCount', 0) + 1
            contract['constants'][reg] = {'buffer': 2, 'index': 15}
            continue
        if arg is not None and source_name == 'gameTime' and is_scorch_program(source_material):
            # only read by the scorch fade, which the zero scorch weight removes
            contract['constants'][reg] = {'expr': 'float4(0.0, 0.0, 0.0, 0.0)', 'uses': []}
            continue
        if arg is not None and source_name == 'worldViewProjectionMatrix':
            wm = fields.get('worldMatrix')
            vp = fields.get('viewProjectionMatrix')
            if not wm or not vp or wm[2] != 64 or vp[2] != 64:
                raise shaders.ShaderError('matrix composition inputs absent')
            row = arg.get('firstRow', 0) + int(reg[1:]) - arg['dest']
            v = f't6_cb{vp[0]}[{vp[1]//16+row}]'
            sky = source_material and source_material.get('techniqueSet', '').startswith('mc_sky')
            w = [f't6_cb{wm[0]}[{wm[1]//16+i}]' for i in range(4)]
            if sky:
                w = [f'float4({r}.xyz, 0)' for r in w]
            contract['constants'][reg] = {'expr': ' + '.join(f'{v}.{c} * {r}' for c, r in zip('xyzw', w)),
                'uses': [[vp[0], vp[1]//16+row]] + [[wm[0], wm[1]//16+i] for i in range(4)]}
            continue
        if arg is not None and source_name == 'inverseWorldViewMatrix':
            # Distance falloff reads the camera's object-space position from
            # the inverse matrix's translation column. Recover it from the
            # native world transform and inverse view matrix, including scale.
            row = arg.get('firstRow', 0) + int(reg[1:]) - arg['dest']
            reads = [line.strip() for line in source_assembly.splitlines()
                     if not line.strip().startswith(('//', 'def'))
                     and re.search(r'\b' + re.escape(reg) + r'\b', line)]
            wm, iv = fields.get('worldMatrix'), fields.get('inverseViewMatrix')
            if row >= 3 or not reads or not all(re.fullmatch(
                    r'mov\s+r\d+\.[xyzw],\s*' + re.escape(reg) + r'\.w', line) for line in reads):
                raise shaders.ShaderError('inverse world-view matrix needs a full matrix adapter')
            if not wm or not iv or wm[2] != 64 or iv[2] != 64:
                raise shaders.ShaderError('inverse world-view camera inputs absent')
            w = [f't6_cb{wm[0]}[{wm[1]//16+i}]' for i in range(3)]
            v = [f't6_cb{iv[0]}[{iv[1]//16+i}]' for i in range(3)]
            columns = [f'float3({w[0]}.{c}, {w[1]}.{c}, {w[2]}.{c})' for c in 'xyz']
            delta = 'float3(' + ', '.join(f'{v[i]}.w - {w[i]}.w' for i in range(3)) + ')'
            cofactor = f'cross({columns[(row+1)%3]}, {columns[(row+2)%3]})'
            determinant = f'dot({columns[0]}, cross({columns[1]}, {columns[2]}))'
            contract['constants'][reg] = {
                'expr': f'float4(0, 0, 0, dot({cofactor}, {delta}) / {determinant})',
                'uses': [[wm[0], wm[1]//16+i] for i in range(3)] +
                        [[iv[0], iv[1]//16+i] for i in range(3)]}
            continue
        if arg is not None and source_name in ('clipSpaceLookupScale', 'clipSpaceLookupOffset'):
            # D3D11 pixel centres need no D3D9 half-texel bias. The source
            # computes projected texture coordinates scale*clip + offset*clip.w.
            value = 'float4(0.5, -0.5, 1, 1)' if source_name == 'clipSpaceLookupScale' else 'float4(0.5, 0.5, 0, 0)'
            contract['constants'][reg] = {'expr': value, 'uses': []}
            continue
        if arg is not None and source_name in NEUTRAL_CODE_UNIFORMS and source_name not in fields:
            contract['constants'][reg] = {'expr': NEUTRAL_CODE_UNIFORMS[source_name], 'uses': []}
            contract.setdefault('neutral', []).append(source_name)
            continue
        name = CODE_UNIFORM_ALIASES.get(source_name, source_name) if source_name not in fields else source_name
        if arg is None or reg not in names or name not in fields:
            raise shaders.ShaderError(f'native vertex uniform absent for {reg}: {names.get(reg)}')
        row = arg.get('firstRow', 0) + int(reg[1:]) - arg['dest']
        slot, offset, size = fields[name]
        at = offset + row * 16
        if at % 16 or size < (row+1)*16:
            raise shaders.ShaderError(f'incompatible vertex uniform {name}')
        if slot != 0 and not any(a['type'] == 3 and a['buffer'] == slot and a['location'] <= at and at+16 <= a['location']+a['size'] for a in native_pass['args']):
            raise shaders.ShaderError(f'native pass does not maintain vertex uniform {name}')
        contract['constants'][reg] = {'buffer': slot, 'index': at//16}
        if paired and source_name == 'baseLightingCoords' and name == 'gridLightingCoordsAndVis' and size >= 16:
            # T6 keeps sun visibility in gridLightingCoordsAndVis.w (lprobe sun
            # PS: sun term * v.w); WaW keeps it in the model lighting texel
            # alpha. Carry .w on the varying WaW already uses for these coords.
            hits = tainted_outputs(source_assembly, {reg})
            carriers = {r for r, _ in hits}
            if len(carriers) == 1 and {c for _, c in hits} <= set('xyz'):
                carrier = carriers.pop()
                contract['outputs'][carrier]['width'] = 4
                contract['epilogue'] = {'lines': [f'{carrier}.w = t6_cb{slot}[{at//16}].w;'], 'uses': [[slot, at//16]]}
                contract['lighting_visibility'] = contract['outputs'][carrier]['semantic']
    if ir['samplers']:
        raise shaders.ShaderError('vertex texture binding adapter required')
    return contract


_ELEMENTWISE = {'mov', 'add', 'sub', 'mul', 'mad', 'min', 'max', 'slt', 'sge', 'frc', 'abs', 'cmp', 'lrp', 'dsx', 'dsy'}


def tainted_outputs(assembly, source_regs):
    """Output components whose value depends on any of ``source_regs``.

    Component-exact data flow over SM3 assembly: element-wise ops read the
    swizzled component per destination channel, scalar ops read the first
    selected component, dot products/nrm read every channel. A write replaces
    the destination channel's taint (so overwritten temporaries clear).
    """
    taint: dict[str, set[str]] = {}
    for line in assembly.splitlines():
        line = line.split('//', 1)[0].strip()
        op, _, tail = line.partition(' ')
        op = op.split('_')[0]
        if not tail or op in ('def', 'defi') or op.startswith('dcl') or op in ('vs', 'ps'):
            continue
        args = [a.strip() for a in tail.split(',')]
        dst = shaders.REG.fullmatch(args[0])
        if not dst:
            continue
        mask = (dst[2] or 'xyzw').translate(shaders.SWIZZLE)

        def reads(token, channel):
            token = token.lstrip('-')
            token = re.sub(r'^abs\((.*)\)$', r'\1', token).replace('_abs', '')
            m = shaders.REG.fullmatch(token)
            if not m:
                return False
            reg, sw = m[1], (m[2] or 'xyzw').translate(shaders.SWIZZLE)
            sw += sw[-1] * (4 - len(sw))
            if reg in source_regs:
                return True
            if channel is None:
                # reductions read a fixed channel count of the swizzled source
                comps = sw[:{'dp3': 3, 'nrm': 3, 'dp2add': 2}.get(op, 4)]
            else:
                comps = sw['xyzw'.index(channel)]
            return any(c in taint.get(reg, set()) for c in comps)

        new = set()
        for ch in mask:
            if op in _ELEMENTWISE:
                hit = any(reads(a, ch) for a in args[1:])
            elif op in ('rcp', 'rsq', 'exp', 'log', 'pow'):
                hit = any(reads(a, 'x') for a in args[1:])
            else:
                hit = any(reads(a, None) for a in args[1:])
            if hit:
                new.add(ch)
        reg = dst[1]
        taint[reg] = (taint.get(reg, set()) - set(mask)) | new
    return {(reg, c) for reg, comps in taint.items() if reg.startswith('o') for c in comps}


def fog_output(source_assembly, contract):
    """The single vertex output component carrying WaW fog visibility."""
    fog_regs = {reg for reg, (name, _) in registers(source_assembly).items() if name == 'fogConsts'}
    if not fog_regs:
        return None
    hits = tainted_outputs(source_assembly, fog_regs)
    if len(hits) != 1:
        raise shaders.ShaderError(f'unrecognized vertex fog output {sorted(hits)}')
    reg, comp = next(iter(hits))
    if reg not in contract['outputs']:
        raise shaders.ShaderError('vertex fog output is not declared')
    return reg, comp


def native_fog_adapter(hlsl, source_assembly, contract, native_pass, native_root):
    found = fog_output(source_assembly, contract)
    if found is None:
        return hlsl
    fog_reg, fog_comp = found
    fields = buffers(_native_program(native_root, 'vs', native_pass))
    def field(name, row=0, channels=4):
        slot, offset, size = fields[name]
        if offset % 16 or size < row*16+channels*4:
            raise shaders.ShaderError(f'incompatible native fog uniform {name}')
        # The common vertex contract includes viewProjectionMatrix in cb0 and
        # worldMatrix in cb3, so these declarations cover all referenced rows.
        return f't6_cb{slot}[{offset//16+row}]'
    position = next(reg for reg,e in contract['inputs'].items() if e['semantic'].upper() == 'POSITION0')
    wm = [field('worldMatrix', i) for i in range(3)]
    fc, f, f2, sc = [field(n) for n in ('fogColor','fogConsts','fogConsts2','sunFogColor')]
    sd, sf = field('sunFogDir', channels=3), field('sunFog', channels=2)
    # Derived directly from the native T6 vertex program. WaW's fog parameter
    # packing differs, while its pixel program consumes the same visibility.
    code = f'''
    float3 fog_p = float3(dot(float4({position}.xyz,1), {wm[0]}), dot(float4({position}.xyz,1), {wm[1]}), dot(float4({position}.xyz,1), {wm[2]}));
    float fog_dist = length(fog_p);
    float3 fog_dir = fog_p * rsqrt(dot(fog_p,fog_p));
    float fog_sun = saturate(dot({sd}.xyz, fog_dir)*{sf}.y+{sf}.x);
    float fog_alpha = lerp({fc}.w, {sc}.w, fog_sun);
    float fog_height = {f}.w*fog_p.z+{f}.x;
    float fog_integral = (fog_height<0 ? exp2(min(fog_height,64)*1.442695) : fog_height+1)-{f2}.x;
    float fog_denom = fog_p.z*{f}.w;
    float fog_ratio = abs(fog_denom)<0.0001 ? saturate({f2}.x) : fog_integral/fog_denom;
    float fog_density = fog_ratio*{f}.y*fog_dist+{f}.z;
    {fog_reg}.{fog_comp} = 1-(1-min(exp2(fog_density),1))*fog_alpha;
'''
    if 'sky_fog_distance' in contract:
        sky_position = 'float3(' + ', '.join(f'dot({position}.xyz, {row}.xyz)' for row in wm) + ')'
        start = code.index('    float fog_dist')
        code = f'\n    float3 fog_p = normalize({sky_position}) * {contract["sky_fog_distance"]};\n' + code[start:]
    return hlsl.replace('Output output;', code + '\nOutput output;')


# Matching engine technique meanings. Extended T6 dynamic-light combinations
# cannot be routed to a differently shaped WaW pass by their ordinal alone.
SLOTS = {0: 0, 1: 1, 2: 4, 3: 5, 4: 8, 5: 10, 6: 12, 7: 14, 8: 16, 13: 18, 14: 20}


# Self-illuminated WaW techniques T6 draws through UNLIT/EMISSIVE.
SELF_LIT_KINDS = ('unlit', 'unlit_blend', 'unlit_add', 'unlit_distfalloff', 'objective')


def source_slot(present, source_name, slot):
    """WaW technique index feeding T6 ``slot``; ``present(i)`` tells whether
    WaW technique ``i`` exists."""
    index = SLOTS[slot]
    if present(index):
        return index
    parsed = techsets.parse(source_name)
    self_lit = source_name.startswith('mc_sky') or (not parsed.lit and parsed.unlit_kind in SELF_LIT_KINDS)
    if slot in (2, 3) and self_lit:
        # T6 draws emissive surfaces through UNLIT/EMISSIVE even when WaW
        # draws them through UNLIT or only through lit aliases (objective).
        for alias in (4, 8):
            if present(alias):
                return alias
    return None


def _output_scale(source_material, source_name, slot):
    """Contract entry halving a scene pass's colour (see DISPLAY_SCALE). Depth
    passes and HUD materials (drawn after the composite) keep their colour."""
    parsed = techsets.parse(source_name)
    if slot < 2 or (parsed.family == 'other' and parsed.unlit_kind == '2d'):
        return {}
    entries = source_material.get('stateBitsEntry', [])
    states = source_material.get('stateBits', [])
    present = lambda i: i < len(entries) and 0 <= entries[i] < len(states)
    index = source_slot(present, source_name, slot)
    state = states[entries[index]] if index is not None else None
    return {'output_rgb_scale': WAW_OUTPUT_SCALE} if scales_output(state) else {}


def diag_terms(hlsl, contract, spot_like=False):
    """Diagnostic build: output (colour-map luminance, secondary lightmap
    luminance x2, final colour luminance x10). The first sample of each
    texture is captured from the generated statements."""
    lightmap = next((r for r, e in contract.get('samplers', {}).items()
                     if e.get('v_scale_offset') == list(lightmaps.WAW_PAGE_UV['lightmapSamplerSecondary'])), None)
    lines = hlsl.split(chr(10))
    captured = set()
    for i, line in enumerate(lines):
        for name, tag in (('tex_s0.Sample(', 'diag_c'), (f'tex_{lightmap}.Sample(' if lightmap else None, 'diag_l')):
            if name and name in line and line.rstrip().endswith('}'):
                if tag == 'diag_l':
                    # every lightmap-page read (spot programs read the falloff ramp first)
                    lines[i] = line.rstrip()[:-1] + 'diag_l = max(diag_l, value); }'
                elif tag not in captured:
                    lines[i] = line.rstrip()[:-1] + f'{tag} = value; }}'
                captured.add(tag)
    start = next(i for i, l in enumerate(lines) if l.startswith('Output main(Input input) {'))
    lines.insert(start + 1, 'float4 diag_c = 0; float4 diag_l = 0;')
    end = next(i for i, l in enumerate(lines) if l.startswith('Output output;'))
    target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
    # on/off channels: analog values are crushed by the map's film grade
    lines.insert(end, f'{target} = float4({"2.0" if spot_like else "0.0"}, '
                      f'dot(diag_l.rgb, 0.333) > 0.01 ? 2.0 : 0.0, dot({target}.rgb, 0.333) > 0.002 ? 2.0 : 0.0, 1.0);')
    return chr(10).join(lines)


# WaW alpha test (D3D9 render state) as the D3D11 discard T6 programs do
# themselves (stock alpha-tested PS: lt + discard_nz in every technique).
ALPHA_TEST_DISCARD = {'gt0': '{a} <= 0.0', 'ge128': '{a} < 0.5', 'lt128': '{a} >= 0.5'}


def _source_state(source_material, source_name, slot):
    entries = source_material.get('stateBitsEntry', [])
    states = source_material.get('stateBits', [])
    present = lambda i: i < len(entries) and 0 <= entries[i] < len(states)
    index = source_slot(present, source_name, slot)
    return states[entries[index]] if index is not None else None


def alpha_test_lines(source_material, source_name, slot, target):
    """Discard lines reproducing the WaW pass's alpha test, or []."""
    state = _source_state(source_material, source_name, slot) or {}
    rule = ALPHA_TEST_DISCARD.get(state.get('alphaTest', 'disabled'))
    return [f'if ({rule.format(a=target + ".w")}) discard;'] if rule else []


def alpha_tested(source_material):
    """An opaque cut-out: the WaW state of its main lit pass (T6 slot 4)
    alpha-tests without blending."""
    state = _source_state(source_material, source_material.get('techniqueSet', ''), 4) or {}
    return (state.get('alphaTest', 'disabled') in ALPHA_TEST_DISCARD
            and state.get('blendOpRgb', 'disabled') == 'disabled')


def _blended_state(source_material, source_name, slot):
    """True when the WaW state of this slot blends over the destination."""
    entries = source_material.get('stateBitsEntry', [])
    states = source_material.get('stateBits', [])
    present = lambda i: i < len(entries) and 0 <= entries[i] < len(states)
    index = source_slot(present, source_name, slot)
    state = states[entries[index]] if index is not None else None
    return bool(state) and state.get('blendOpRgb', 'disabled') != 'disabled' and state.get('dstBlendRgb') == 'invsrcalpha'


def source_technique(original, source_name, slot):
    techniques = original['techniques']
    index = source_slot(lambda i: i < len(techniques) and bool(techniques[i]), source_name, slot)
    return techniques[index] if index is not None else None


# Blend semantics belong to the program writing the colour: WaW programs
# output straight or premultiplied colour exactly as their WaW state expects.
PASS_STATE_FIELDS = ('srcBlendRgb', 'dstBlendRgb', 'blendOpRgb', 'srcBlendAlpha',
                     'dstBlendAlpha', 'blendOpAlpha', 'alphaTest')


def _additive(state):
    return state.get('blendOpRgb', 'disabled') != 'disabled' and state.get('dstBlendRgb') == 'one'


def apply_source_pass_states(source_material, output_material, runtime):
    """Give each T6 pass the WaW state of the technique feeding it.

    Applied when the pass runs the original WaW program, and when WaW blends
    additively (black is transparent) while the native donor does not: an
    additive layer drawn with alpha blending or opaque shows its black
    background. Returns the T6 slots changed.
    """
    entries = output_material.get('stateBitsEntry', [])
    states = output_material.get('stateBits', [])
    src_entries = source_material.get('stateBitsEntry', [])
    src_states = source_material.get('stateBits', [])
    name = source_material.get('techniqueSet', '')
    present = lambda i: i < len(src_entries) and 0 <= src_entries[i] < len(src_states)
    waw_slots = {a['slot'] for a in runtime.get('active', [])}
    changed = []
    for slot in SLOTS:
        if slot >= len(entries) or not 0 <= entries[slot] < len(states):
            continue
        index = source_slot(present, name, slot)
        if index is None:
            continue
        wanted, native = src_states[src_entries[index]], states[entries[slot]]
        if slot not in waw_slots and not (_additive(wanted) and not _additive(native)):
            continue
        update = {k: wanted[k] for k in PASS_STATE_FIELDS if k in wanted}
        if all(native.get(k) == v for k, v in update.items()):
            continue
        merged = {**native, **update}
        if merged in states:
            entries[slot] = states.index(merged)
        else:
            states.append(merged)
            entries[slot] = len(states) - 1
        changed.append(slot)
    return changed


def _compiled_signature(code, kind):
    text = shaders.disassemble(code)
    match = re.search(r'// ' + kind + r' signature:\s*\n//\s*\n// Name.*?\n// -.*?\n(.*?)//\s*\n', text, re.S)
    rows = re.findall(r'^//\s+(\w+)\s+(\d+)\s+([xyzw]+)\s+(\d+)', match[1], re.M) if match else []
    return {(name.upper(), int(index)): (int(register), mask) for name, index, mask, register in rows}


def check_linkage(vertex_code, pixel_code):
    """D3D11 links stages by register: every pixel input must be the vertex
    output with the same semantic in the same register, with enough channels."""
    outputs = _compiled_signature(vertex_code, 'Output')
    for key, (register, mask) in _compiled_signature(pixel_code, 'Input').items():
        found = outputs.get(key)
        if found is None or found[0] != register or not set(mask) <= set(found[1]):
            raise shaders.ShaderError(f'compiled stages do not link at {key}: vs {found} ps {(register, mask)}')


def _write_program(project_root, stage, code, source, contract, info):
    # Content-derived name avoids overriding any stock shader or sharing
    # one program across incompatible resource layouts.
    name = 'waw/runtime_' + hashlib.sha256(code).hexdigest()[:20]
    path = project_root / 'shader_bin' / f'{stage}_{name}.cso'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(code)
    proof = project_root / 'content_source/shaders/runtime' / f'{name.rsplit("/", 1)[1]}.json'
    proof.parent.mkdir(parents=True, exist_ok=True)
    proof.write_text(json.dumps({'source': str(source), 'bindings': contract, 'shader': name,
                                 'sha256': hashlib.sha256(code).hexdigest(), **info}, indent=2))
    return name


# WAW2BO2_DIAG_SLOT_COLORS=1: lit passes draw a flat colour per technique
# (no local light green, sun yellow, spot red, omni magenta, shadowed cyan;
# dynamic-light variants copy their base). Never shipped.
DIAGNOSTIC_SLOT_COLORS = {4: '0.0, 0.5, 0.0', 5: '0.5, 0.5, 0.0', 6: '0.5, 0.25, 0.0', 7: '0.5, 0.0, 0.0',
                          8: '0.0, 0.5, 0.5', 13: '0.5, 0.0, 0.5', 14: '0.0, 0.0, 0.5'}


def bind_material(source_material, output_material, roots, project_root, native_root, falloff_placement=None,
                  light_shadows=True):
    """``falloff_placement``: the map's WaW lightFalloffPlacement; ``light_shadows``
    False when every primary light is staged unshadowed (shadowed slots unreachable)."""
    result = {'active': [], 'unsupported': []}
    # Diagnostic A/B switch: WAW2BO2_RUNTIME_SHADERS=none keeps every T6 donor pass.
    if os.environ.get('WAW2BO2_RUNTIME_SHADERS', 'all') == 'none':
        result['native_techset'] = output_material.get('techniqueSet', '')
        result['unsupported'].append('runtime shaders disabled (WAW2BO2_RUNTIME_SHADERS=none)')
        return result
    source_name = source_material.get('techniqueSet', '')
    source_file = _find(roots, Path('waw_techniquesets') / f'{source_name}.json')
    native_name = output_material.get('techniqueSet', '')
    result['native_techset'] = native_name
    native_file = native_root / 'techniquesets' / f'{native_name}.json'
    if source_file is None or not native_file.is_file():
        result['unsupported'].append('original/native technique metadata absent')
        return result
    original = json.loads(source_file.read_text())
    pristine = json.loads(native_file.read_text())
    native = copy.deepcopy(pristine)
    for slot, source_slot in SLOTS.items():
        target = native['techniques'][slot]
        source = source_technique(original, source_name, slot)
        if not target or not source:
            continue
        if len(source['passes']) != 1 or len(target['passArray']) != 1:
            result['unsupported'].append(f'slot {slot}: multi-pass adapter required')
            continue
        source_pass, target_pass = source['passes'][0], target['passArray'][0]
        output_scale = _output_scale(source_material, source_name, slot)
        binary = _find(roots, Path('shader_bin') / f'ps_{source_pass["pixelShader"]}.cso')
        if binary is None:
            result['unsupported'].append(f'slot {slot}: original program absent')
            continue
        vertex_binary = _find(roots, Path('shader_bin') / f'vs_{source_pass["vertexShader"]}.cso')
        # Preferred: the original vertex+pixel pair, so the varyings stay WaW's.
        paired_error = None
        if vertex_binary:
            try:
                vertex_assembly = embed_literals(_assembly(vertex_binary), source_pass, 'vs')
                vertex_binding = vertex_contract(vertex_assembly, source_pass, target_pass, native_root, paired=True,
                                                 source_material=source_material,
                                                 layered=bool(pristine.get('worldVertFormat')))
                vertex_hlsl, vertex_info = shaders.translate(vertex_assembly, vertex_binding)
                vertex_hlsl = native_fog_adapter(vertex_hlsl, vertex_assembly, vertex_binding, target_pass, native_root)
                vertex_code = shaders.compile_hlsl(vertex_hlsl, 'vs_5_0')
                linked = [{'semantic': s, 'width': w} for s, w in vertex_info['outputs'].values()]
                assembly = embed_literals(_assembly(binary), source_pass, 'ps')
                contract = paired_pixel_contract(assembly, source_pass, target_pass, native_root, linked,
                                                 vertex_binding.get('lighting_visibility'), source_material,
                                                 falloff_placement, output_material)
                contract.update(output_scale)
                target_reg = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                discard = alpha_test_lines(source_material, source_name, slot, target_reg)
                if discard:
                    contract['epilogue'] = {'lines': discard, 'uses': []}
                    contract.setdefault('unit_conversions', []).append('alpha test: discard in the program (D3D11)')
                diagnostic = DIAGNOSTIC_SLOT_COLORS.get(slot) if os.environ.get('WAW2BO2_DIAG_SLOT_COLORS') else None
                if diagnostic is not None:
                    # Diagnostic build: every lit pass draws its technique's flat
                    # colour, showing in game which T6 technique a surface uses.
                    target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                    contract['epilogue'] = {'lines': [f'{target} = float4({diagnostic}, 1.0);'], 'uses': []}
                if os.environ.get('WAW2BO2_DIAG_NAN'):
                    # Diagnostic build: NaN/Inf output in magenta, negative in cyan.
                    target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                    contract['epilogue'] = {'lines': [
                        f'if (any((asuint({target}) & 0x7fffffff) >= 0x7f800000)) {target} = float4(1.0, 0.0, 1.0, 1.0);',
                        f'else if (any({target}.xyz < 0.0)) {target} = float4(0.0, 1.0, 1.0, 1.0);'], 'uses': []}
                if os.environ.get('WAW2BO2_DIAG_DECAL') and _blended_state(source_material, source_name, slot):
                    # Diagnostic build: blended (decal) passes show their output
                    # alpha in red and their output brightness x4 in green, opaque.
                    target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                    contract['epilogue'] = {'lines': [
                        f'{target} = float4(saturate({target}.w), saturate(dot({target}.xyz, float3(0.333, 0.333, 0.333)) * 4.0), 0.0, 1.0);'],
                        'uses': []}
                secondary = contract.get('samplers', {}).get('s3', {})
                if (os.environ.get('WAW2BO2_DIAG_LIGHTMAP') and secondary.get('v_scale_offset')
                        and 'v1' in contract.get('inputs', {})):
                    # Diagnostic build: draw only WaW's decoded secondary lightmap
                    # colour (both halves, flat weight) at the lm_* lightmap UV.
                    scale, offset = secondary['v_scale_offset']
                    target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                    top = f'tex_s3.Sample(samp_s3, float2(v1.z, v1.w * 0.5 * {scale!r} + {offset!r}))'
                    bottom = f'tex_s3.Sample(samp_s3, float2(v1.z, (v1.w * 0.5 + 0.5) * {scale!r} + {offset!r}))'
                    contract['epilogue'] = {'lines': [f'{target} = float4({top}.rgb + {bottom}.rgb * 0.6, 1.0);'], 'uses': []}
                hlsl, info = shaders.translate(assembly, contract)
                if os.environ.get('WAW2BO2_DIAG_TERMS'):
                    hlsl = diag_terms(hlsl, contract, slot in (7, 8, 13, 14))
                if os.environ.get('WAW2BO2_DIAG_LMUV'):
                    # Diagnostic build: stripes of the lightmap UV the PS receives
                    # (red: u, green: v, 16 bands each); solid blue: UV (0, 0).
                    uvreg = next((r for r, e in contract['inputs'].items() if e['semantic'].upper() == 'TEXCOORD0'), None)
                    if uvreg and contract.get('lightmap') == 'waw':
                        target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                        hlsl = hlsl.replace('Output output;', f'{target} = float4(frac({uvreg}.z * 16.0) > 0.5 ? 2.0 : 0.0, '
                                            f'frac({uvreg}.w * 16.0) > 0.5 ? 2.0 : 0.0, all({uvreg}.zw == 0.0) ? 2.0 : 0.0, 1.0);'
                                            + chr(10) + 'Output output;', 1)
                if os.environ.get('WAW2BO2_DIAG_PAGEID'):
                    lm = next((r for r, e in contract.get('samplers', {}).items()
                               if e.get('texture') == WORLD_SURFACE_TEXTURE_SLOTS['lightmapSamplerSecondary'][0]), None)
                    if lm:
                        target = next(r for r, e in contract['outputs'].items() if e['semantic'].upper() == 'SV_TARGET0')
                        hlsl = hlsl.replace('Output output;', f'{target} = float4(tex_{lm}.Load(int3(1022, 3070, 0)).rgb * 8.0, 1.0);' + chr(10) + 'Output output;', 1)
                dxbc = shaders.compile_hlsl(hlsl, 'ps_5_0')
                check_linkage(vertex_code, dxbc)
                if source_name.startswith('mc_sky'):
                    # Donor skies stream only POSITION. The translated dome
                    # also consumes stored color/UV; changing the HLSL input
                    # signature alone does not add these streams in T6.
                    streams = {'POSITION0': (0, 0), 'COLOR0': (1, 2), 'TEXCOORD0': (2, 5)}
                    routing = [dict(zip(('source', 'dest'), streams[e['semantic'].upper()]))
                               for e in vertex_binding['inputs'].values()]
                    target_pass['vertexDecl'] = {'streamCount': len(routing), 'hasOptionalSource': False,
                        'isLoaded': False, 'routing': routing + [{'source': 0, 'dest': 0}] * (16-len(routing))}
                    prune_missing_material_constants(target_pass, output_material, (vertex_binding, contract))
                prune_missing_material_constants(target_pass, output_material, (vertex_binding, contract), unread=True)
                shader_name = _write_program(project_root, 'ps', dxbc, binary, contract, info)
                vertex_name = _write_program(project_root, 'vs', vertex_code, vertex_binary, vertex_binding, vertex_info)
                target_pass['pixelShader'] = {'name': shader_name}
                target_pass['vertexShader'] = {'name': vertex_name}
                result['active'].append({'slot': slot, 'source': source_pass['pixelShader'], 'shader': shader_name,
                                         'vertex_shader': vertex_name, 'paired': True,
                                         'neutral': vertex_binding.get('neutral', []),
                                         'unit_conversions': list(dict.fromkeys(contract.get('unit_conversions', []))),
                                         'lightmap': contract.get('lightmap')})
                continue
            except shaders.ShaderError as exc:
                paired_error = str(exc)
        try:
            assembly = embed_literals(_assembly(binary), source_pass, 'ps')
            contract = pixel_contract(assembly, source_pass, target_pass, native_root, source_material)
            contract.update(output_scale)
            hlsl, info = shaders.translate(assembly, contract)
            dxbc = shaders.compile_hlsl(hlsl, 'ps_5_0')
            shader_name = _write_program(project_root, 'ps', dxbc, binary, contract, info)
            vertex_name = None
            if vertex_binary:
                try:
                    vertex_assembly = embed_literals(_assembly(vertex_binary), source_pass, 'vs')
                    vertex_binding = vertex_contract(vertex_assembly, source_pass, target_pass, native_root, source_material=source_material)
                    vertex_hlsl, vertex_info = shaders.translate(vertex_assembly, vertex_binding)
                    vertex_hlsl = native_fog_adapter(vertex_hlsl, vertex_assembly, vertex_binding, target_pass, native_root)
                    vertex_code = shaders.compile_hlsl(vertex_hlsl, 'vs_5_0')
                    vertex_name = _write_program(project_root, 'vs', vertex_code, vertex_binary, vertex_binding, vertex_info)
                except shaders.ShaderError as exc:
                    result['unsupported'].append(f'slot {slot} vertex: {exc}')
            target_pass['pixelShader'] = {'name': shader_name}
            if vertex_name:
                target_pass['vertexShader'] = {'name': vertex_name}
            result['active'].append({'slot': slot, 'source': source_pass['pixelShader'], 'shader': shader_name, 'vertex_shader': vertex_name})
        except shaders.ShaderError as exc:
            result['unsupported'].append(f'slot {slot}: {exc}' + (f' (paired: {paired_error})' if paired_error else ''))
    bound_slots = {a['slot']: a for a in result['active']}
    for slot, base in DLIGHT_BASE_SLOTS.items():
        if base in bound_slots and pristine['techniques'][slot] and slot not in bound_slots:
            native['techniques'][slot] = copy.deepcopy(native['techniques'][base])
            result['active'].append({**bound_slots[base], 'slot': slot, 'dlight_base': base})
            result['unsupported'].append(f'slot {slot}: dynamic lights not added (draws the WaW pass of slot {base})')
    # One lightmap page per surface: WaW-lightmap passes are only kept when no
    # remaining donor lit pass reads the T6-encoded lightmap.
    waw_lightmap = [a for a in result['active'] if a.get('lightmap') == 'waw']
    if waw_lightmap:
        active_slots = {a['slot'] for a in result['active']}
        reachable = [slot for slot in REACHABLE_LIT_SLOTS if light_shadows or slot not in SHADOWED_LIT_SLOTS]
        readers = [slot for slot in reachable if slot not in active_slots and pristine['techniques'][slot]
                   and any('lightmapSamplerSecondary' == n for n, _ in resource_names(
                       _native_program(native_root, 'ps', pristine['techniques'][slot]['passArray'][0])).values())]
        if readers:
            for a in waw_lightmap:
                native['techniques'][a['slot']] = copy.deepcopy(pristine['techniques'][a['slot']])
                result['active'].remove(a)
                result['unsupported'].append(f"slot {a['slot']}: WaW lightmap pass reverted; donor slots {readers} read the T6 lightmap")
        else:
            result['lightmap'] = 'waw'
    if result['active'] and alpha_tested(source_material):
        # The donor depth prepass / shadow caster draw the whole quad (null PS);
        # with the cut-out discarded in the lit pass those holes would stay black.
        entries = output_material.get('stateBitsEntry', [])
        translated = {a['slot'] for a in result['active']}
        for slot in (0, 1):
            # translated WaW depth/shadow programs already discard (alpha_test_lines)
            if slot in translated:
                continue
            if slot < len(native['techniques']) and native['techniques'][slot]:
                native['techniques'][slot] = None
                if slot < len(entries):
                    entries[slot] = -1
                result['unsupported'].append(f'slot {slot}: dropped for the WaW alpha test (cut-out casts no shadow-map shadow)')
    if result['active']:
        # Named by content: embedded material constants make programs per material.
        # The whole technique set is hashed, arguments included: two materials
        # can bind identical programs with different arguments (an added
        # source texture), and a shared name would hand one the other's
        # arguments (T6 then scans past its texture table: crash 0x77C173).
        bound = json.dumps([(a['slot'], a['shader'], a.get('vertex_shader')) for a in result['active']])
        content = json.dumps(native, sort_keys=True)
        ts_name = 'waw/runtime_' + hashlib.sha256((native_name + bound + content).encode()).hexdigest()[:16]
        for a in result['active']:
            native['techniques'][a['slot']]['name'] = f"{ts_name}_{a['slot']}"
        native['name'] = ts_name
        path = project_root / 'techniquesets' / f'{ts_name}.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(native, indent=2))
        output_material['techniqueSet'] = ts_name
    return result


def prune_missing_material_constants(native_pass, material, contracts, unread=False):
    """Remove unused donor constants; T6's hash lookup has no missing-key bound.

    Keep argument frequency groups intact. A shader still reading a missing
    material row must be rejected rather than receiving an uninitialized row.
    ``unread`` also removes constants neither program of a paired pass reads:
    the final material may lose donor constants (the mod-tools baseline
    rebuilds them), and T6 sub_777790 scans for type 6 hashes unbounded.
    """
    hashes = {c.get('nameHash', t6_hash(c.get('name', ''))) for c in material.get('constants', [])}
    counts = ('perPrimArgCount', 'perObjArgCount', 'stableArgCount')
    args, cursor, new_counts = [], 0, {}
    for count in counts:
        group = native_pass['args'][cursor:cursor + native_pass[count]]
        cursor += native_pass[count]
        kept = []
        for arg in group:
            if arg['type'] in (0, 6):
                start, end = arg['location'], arg['location'] + arg['size']
                read = False
                for contract in contracts:
                    for binding in contract['constants'].values():
                        uses = binding.get('uses', [])
                        if 'buffer' in binding:
                            uses = [*uses, [binding['buffer'], binding['index']]]
                        read |= any(buffer == arg['buffer'] and start <= row*16 < end for buffer, row in uses)
                if arg['u']['value'] not in hashes:
                    if read:
                        raise shaders.ShaderError('translated shader reads an absent donor material constant')
                    continue
                if unread and not read:
                    continue
            kept.append(arg)
        new_counts[count] = len(kept)
        args.extend(kept)
    if cursor != len(native_pass['args']):
        raise shaders.ShaderError('native argument frequency counts do not cover all bindings')
    native_pass.update(new_counts)
    native_pass['args'] = args
