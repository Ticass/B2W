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

from . import lightmaps, shaders


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
NEUTRAL_CODE_UNIFORMS = {'destructibleParms': 'float4(0, 0, 0, 0)'}
# Vertex inputs WaW reads as biased UBYTE4 unit vectors (NORMAL, tangent in
# TEXCOORD2); T6 feeds decoded floats. Checked against the decode literals.
PACKED_VECTOR_INPUTS = ('NORMAL0', 'TEXCOORD2')


def resource_names(assembly):
    """T6 reflection: texture slot -> (name, dimension)."""
    return {int(slot): (name, dim) for name, dim, slot in
            re.findall(r'^//\s+(\w+)\s+texture\s+\w+\s+(\w+)\s+t(\d+)\s+\d+', assembly, re.M)}


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
REACHABLE_LIT_SLOTS = (4, 5, 6, 7, 8, 13, 14)
# Samplers both engines bind per surface, outside the pass arguments.
PER_SURFACE_SAMPLERS = {'lightmapSamplerPrimary', 'lightmapSamplerSecondary', 'reflectionProbeSampler'}


def paired_pixel_contract(source_assembly, source_pass, native_pass, native_root, linked, visibility=None,
                          source_material=None):
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
                'expr': f'float4(sqrt(max(t6_cb0[{index}].xyz * t6_cb0[{hdr_row}].x, 0)), t6_cb0[{index}].w)',
                'uses': [[0, index], [0, hdr_row]]}
            contract.setdefault('unit_conversions', []).append(f'{name}: sqrt(hdr * linear)')
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
            if found is None:
                raise shaders.ShaderError(f'native texture binding absent for {reg} ({source_name})')
            texture, sampler = found['location'] & 255, found['location'] >> 8
        else:
            # Code textures: enums differ between the engines (sun shadow map:
            # WaW 7, T6 6), and per-surface ones (lightmap, reflection probe)
            # are bound outside the pass arguments in both. The engine-filled
            # resource is identified by its reflected name in both programs.
            # WaW's primary lightmap (sun visibility) lives in the third of the
            # WaW-encoded page bound as T6's single lightmap (see lightmaps.py).
            lookup = 'lightmapSamplerSecondary' if source_name in lightmaps.WAW_PAGE_UV else source_name
            slots = [t for t, (n, _) in native_textures.items() if n == lookup]
            if len(slots) != 1 or not re.search(rf'^dcl_sampler s{slots[0]},', ps, re.M):
                raise shaders.ShaderError(f'native code texture absent for {reg} ({source_name})')
            texture = sampler = slots[0]
            arg = {'type': 4}
        native_name, native_dim = native_textures.get(texture, (None, None))
        source_sky_texture = arg['type'] == 2 and (source_material or {}).get('techniqueSet', '').startswith('mc_sky')
        if not source_sky_texture and {'2d': '2d', 'volume': '3d', 'cube': 'cube'}[dim] != native_dim:
            raise shaders.ShaderError(f'texture dimension differs for {reg}: {dim} vs {native_dim}')
        entry = {'texture': texture, 'sampler': sampler}
        if arg['type'] == 4:
            if source_name == 'shadowmapSamplerSun':
                # Same lookup matrix/partition scheme in both engines. WaW point-
                # samples depth and compares itself; T6 binds a comparison sampler
                # there, so read texels without it (exact for point sampling).
                if not re.search(rf'^dcl_sampler s{sampler}, mode_comparison', ps, re.M):
                    raise shaders.ShaderError('native sun shadow sampler mode not recognized')
                entry['point_load'] = True
                contract.setdefault('unit_conversions', []).append('shadowmapSamplerSun: texel load at mip 0')
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
                # Measured: T6 lprobe = sqrt(hdr * 32 * L^2 * C^2) = sqrt(32 hdr) L C;
                # WaW = 2 L' C, so L' = sqrt(8 hdr) L keeps the WaW arithmetic.
                if not re.search(r'l\((?:0\.000000, )?32\.000000, 32\.000000, 32\.000000', ps):
                    raise shaders.ShaderError('native model lighting scale not recognized')
                entry.update(rgb_scale=f'sqrt(8.0 * t6_cb0[{hdr_row}].x)', uses=[[0, hdr_row]])
                contract.setdefault('unit_conversions', []).append('modelLighting: sqrt(8 hdr) * L')
                if 'sunDiffuse' in {n for n, _ in names.values()}:
                    # WaW reads sun visibility from the texel alpha; T6 from
                    # gridLightingCoordsAndVis.w, carried by the vertex program.
                    link = next((i for i, s in enumerate(linked) if visibility and s['semantic'].upper() == visibility.upper()
                                 and s['width'] == 4), None)
                    if link is None:
                        raise shaders.ShaderError('sun visibility varying absent')
                    entry['alpha_expr'] = f'input.link{link}.w'
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
                if not re.search(r'add r\d\.w, r\d\.w, l\(0\.000001\)', ps):
                    raise shaders.ShaderError('native reflection probe encoding not recognized')
                entry.update(texel_expr=f'float4(sqrt(max(T.xyz / (T.w + 0.000001) * t6_cb0[{hdr_row}].x, 0)), 1.0)',
                             uses=[[0, hdr_row]])
                contract.setdefault('unit_conversions', []).append('reflectionProbe: sqrt(hdr * rgb / a)')
            else:
                raise shaders.ShaderError(f'code texture {source_name} needs a unit adapter')
        contract['samplers'][reg] = entry
    return contract


def vertex_contract(source_assembly, source_pass, native_pass, native_root, paired=False, source_material=None):
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
        allowed = ('POSITION0', 'COLOR0', 'TEXCOORD0') + (PACKED_VECTOR_INPUTS if paired else ())
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


def bind_material(source_material, output_material, roots, project_root, native_root):
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
        source = original['techniques'][source_slot] if source_slot < len(original['techniques']) else None
        if source_name.startswith('mc_sky') and slot == 3 and not source:
            # T6 routes the sky through both UNLIT and EMISSIVE. WaW's sky
            # exposes the same unlit program in its lit slots, without an
            # emissive slot; both T6 routes need the complete sky pair.
            source = original['techniques'][4]
        if not target or not source:
            continue
        if len(source['passes']) != 1 or len(target['passArray']) != 1:
            result['unsupported'].append(f'slot {slot}: multi-pass adapter required')
            continue
        source_pass, target_pass = source['passes'][0], target['passArray'][0]
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
                vertex_binding = vertex_contract(vertex_assembly, source_pass, target_pass, native_root, paired=True, source_material=source_material)
                vertex_hlsl, vertex_info = shaders.translate(vertex_assembly, vertex_binding)
                vertex_hlsl = native_fog_adapter(vertex_hlsl, vertex_assembly, vertex_binding, target_pass, native_root)
                vertex_code = shaders.compile_hlsl(vertex_hlsl, 'vs_5_0')
                linked = [{'semantic': s, 'width': w} for s, w in vertex_info['outputs'].values()]
                assembly = embed_literals(_assembly(binary), source_pass, 'ps')
                contract = paired_pixel_contract(assembly, source_pass, target_pass, native_root, linked,
                                                 vertex_binding.get('lighting_visibility'), source_material)
                hlsl, info = shaders.translate(assembly, contract)
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
    # One lightmap page per surface: WaW-lightmap passes are only kept when no
    # remaining donor lit pass reads the T6-encoded lightmap.
    waw_lightmap = [a for a in result['active'] if a.get('lightmap') == 'waw']
    if waw_lightmap:
        active_slots = {a['slot'] for a in result['active']}
        readers = [slot for slot in REACHABLE_LIT_SLOTS if slot not in active_slots and pristine['techniques'][slot]
                   and any('lightmapSamplerSecondary' == n for n, _ in resource_names(
                       _native_program(native_root, 'ps', pristine['techniques'][slot]['passArray'][0])).values())]
        if readers:
            for a in waw_lightmap:
                native['techniques'][a['slot']] = copy.deepcopy(pristine['techniques'][a['slot']])
                result['active'].remove(a)
                result['unsupported'].append(f"slot {a['slot']}: WaW lightmap pass reverted; donor slots {readers} read the T6 lightmap")
        else:
            result['lightmap'] = 'waw'
    if result['active']:
        # Named by content: embedded material constants make programs per material.
        bound = json.dumps([(a['slot'], a['shader'], a.get('vertex_shader')) for a in result['active']])
        ts_name = 'waw/runtime_' + hashlib.sha256((native_name + bound).encode()).hexdigest()[:16]
        for a in result['active']:
            native['techniques'][a['slot']]['name'] = f"{ts_name}_{a['slot']}"
        native['name'] = ts_name
        path = project_root / 'techniquesets' / f'{ts_name}.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(native, indent=2))
        output_material['techniqueSet'] = ts_name
    return result


def prune_missing_material_constants(native_pass, material, contracts):
    """Remove unused donor constants; T6's hash lookup has no missing-key bound.

    Keep argument frequency groups intact. A shader still reading a missing
    material row must be rejected rather than receiving an uninitialized row.
    """
    hashes = {c.get('nameHash', t6_hash(c.get('name', ''))) for c in material.get('constants', [])}
    counts = ('perPrimArgCount', 'perObjArgCount', 'stableArgCount')
    args, cursor, new_counts = [], 0, {}
    for count in counts:
        group = native_pass['args'][cursor:cursor + native_pass[count]]
        cursor += native_pass[count]
        kept = []
        for arg in group:
            if arg['type'] in (0, 6) and arg['u']['value'] not in hashes:
                start, end = arg['location'], arg['location'] + arg['size']
                for contract in contracts:
                    for binding in contract['constants'].values():
                        uses = binding.get('uses', [])
                        if 'buffer' in binding:
                            uses = [*uses, [binding['buffer'], binding['index']]]
                        if any(buffer == arg['buffer'] and start <= row*16 < end for buffer, row in uses):
                            raise shaders.ShaderError('translated shader reads an absent donor material constant')
                continue
            kept.append(arg)
        new_counts[count] = len(kept)
        args.extend(kept)
    if cursor != len(native_pass['args']):
        raise shaders.ShaderError('native argument frequency counts do not cover all bindings')
    native_pass.update(new_counts)
    native_pass['args'] = args
