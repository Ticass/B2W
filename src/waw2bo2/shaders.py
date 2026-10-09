"""Strict SM3 instruction translation to SM5, with explicit renderer bindings.

An unbound shader uses the original register ABI for offline validation only.
It must not replace a T6 shader until its input/resource/constant contract is
mapped to the target material pass. Unsupported instructions fail closed.
"""
from __future__ import annotations

import ctypes as ct
import hashlib
import json
import math
import re
from functools import lru_cache
from pathlib import Path


class ShaderError(ValueError):
    pass


def _blob_bytes(blob):
    table = ct.cast(blob, ct.POINTER(ct.POINTER(ct.c_void_p))).contents
    ptr = ct.WINFUNCTYPE(ct.c_void_p, ct.c_void_p)(table[3])(blob)
    size = ct.WINFUNCTYPE(ct.c_size_t, ct.c_void_p)(table[4])(blob)
    try:
        return ct.string_at(ptr, size)
    finally:
        ct.WINFUNCTYPE(ct.c_ulong, ct.c_void_p)(table[2])(blob)


def disassemble(bytecode: bytes) -> str:
    dll = ct.WinDLL('d3dcompiler_47.dll')
    fn = dll.D3DDisassemble
    fn.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint, ct.c_char_p, ct.POINTER(ct.c_void_p)]
    fn.restype = ct.c_long
    blob = ct.c_void_p()
    hr = fn(bytecode, len(bytecode), 0, None, ct.byref(blob))
    if hr < 0:
        raise ShaderError(f'D3DDisassemble failed: {hr & 0xffffffff:#x}')
    return _blob_bytes(blob).decode('utf-8').rstrip('\0')


@lru_cache(maxsize=128)
def compile_hlsl(source: str, profile: str) -> bytes:
    if profile not in ('ps_5_0', 'vs_5_0', 'ps_3_0', 'vs_3_0'):
        raise ShaderError(f'unsupported target profile {profile}')
    data = source.encode('utf-8')
    dll = ct.WinDLL('d3dcompiler_47.dll')
    fn = dll.D3DCompile
    fn.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_char_p, ct.c_void_p,
                   ct.c_void_p, ct.c_char_p, ct.c_char_p, ct.c_uint,
                   ct.c_uint, ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_void_p)]
    fn.restype = ct.c_long
    code, errors = ct.c_void_p(), ct.c_void_p()
    # Strictness + IEEE strictness; do not silently fuse/reassociate instructions.
    hr = fn(data, len(data), b'waw_translated.hlsl', None, None, b'main',
            profile.encode(), (1 << 11) | (1 << 13), 0, ct.byref(code), ct.byref(errors))
    diagnostics = _blob_bytes(errors).decode('utf-8', errors='replace') if errors else ''
    if hr < 0:
        if code:
            _blob_bytes(code)
        raise ShaderError(f'D3DCompile failed: {diagnostics}')
    return _blob_bytes(code)


REG = re.compile(r'^(r\d+|v\d+|c\d+|oC\d+|o\d+|oDepth)(?:\.([xyzwrgba]{1,4}))?$')
SEM = re.compile(r'^[A-Z][A-Z0-9_]*$')
SWIZZLE = str.maketrans('rgba', 'xyzw')


def translate(assembly: str, bindings: dict | None = None) -> tuple[str, dict]:
    """Lower supported SM3 assembly. All missing explicit bindings are errors.

    Contract keys: inputs/outputs map registers to {semantic, width}; constants
    map cN to {buffer, index}; samplers map sN to {texture, sampler}. Pixel
    output oC0 defaults to SV_Target0 only in the unbound validation ABI.
    """
    bound = bindings is not None
    bindings = bindings or {}
    lines = [s.split('//', 1)[0].strip().rstrip('\0') for s in assembly.splitlines()]
    lines = [s for s in lines if s]
    if not lines or lines[0] not in ('ps_3_0', 'vs_3_0'):
        raise ShaderError('requires ps_3_0 or vs_3_0 bytecode')
    stage = lines.pop(0)[:2]
    defs, integers, inputs, outputs, samplers, operations = {}, {}, {}, {}, {}, []
    used, writes, centroid = set(), set(), set()
    for line in lines:
        op, _, tail = line.partition(' ')
        args = [s.strip() for s in tail.split(',')]
        if op == 'def':
            if len(args) != 5 or not re.fullmatch(r'c\d+', args[0]):
                raise ShaderError(f'invalid constant declaration: {line}')
            try:
                values = [float(v) for v in args[1:]]
            except ValueError as e:
                raise ShaderError(f'invalid literal: {line}') from e
            if not all(math.isfinite(v) for v in values):
                raise ShaderError(f'nonfinite literal: {line}')
            defs[args[0]] = values
        elif op == 'defi':
            if len(args) != 5 or not re.fullmatch(r'i\d+', args[0]):
                raise ShaderError(f'invalid integer declaration: {line}')
            try:
                integers[args[0]] = [int(v) for v in args[1:]]
            except ValueError as e:
                raise ShaderError(f'invalid integer literal: {line}') from e
        elif op.startswith('dcl_'):
            match = REG.fullmatch(args[0])
            if re.fullmatch(r's\d+', args[0]):
                dim = op[4:]
                if dim not in ('2d', 'cube', 'volume'):
                    raise ShaderError(f'unsupported texture declaration: {line}')
                samplers[args[0]] = dim
            elif match:
                reg, mask = match.groups()
                declaration = op[4:].split('_')
                if any(m not in ('pp', 'centroid') for m in declaration[1:]):
                    raise ShaderError(f'unsupported declaration modifier: {line}')
                semantic = declaration[0].upper()
                if 'centroid' in declaration[1:]:
                    centroid.add(reg)
                if semantic.startswith('POSITION') and reg.startswith('o'):
                    semantic = 'SV_Position'
                elif not re.search(r'\d$', semantic):
                    semantic += '0'
                width = max('xyzw'.index(x) + 1 for x in (mask or 'xyzw').translate(SWIZZLE))
                (outputs if reg.startswith('o') else inputs)[reg] = (semantic, width)
            else:
                raise ShaderError(f'unsupported declaration: {line}')
        else:
            operations.append((op, args, line))
            used.update(re.findall(r'\b(?:r\d+|v\d+|c\d+|oC\d+|o\d+|oDepth)(?=[._,\s)]|$)', tail))
            if args and REG.fullmatch(args[0]):
                writes.add(REG.fullmatch(args[0])[1])

    for reg in writes:
        if reg.startswith('o') and reg not in outputs:
            if stage == 'ps' and re.fullmatch(r'oC\d+', reg):
                outputs[reg] = ('SV_Target' + reg[2:], 4)
            elif reg == 'oDepth':
                outputs[reg] = ('SV_Depth', 1)
            else:
                raise ShaderError(f'missing output declaration for {reg}')
    if not outputs:
        raise ShaderError('shader has no output')
    constants = sorted((used - defs.keys()) & {r for r in used if r.startswith('c')})
    resources, cb_sizes, reg_expr = [], {}, {}

    def use_buffers(uses):
        # uses: [[slot, index], ...] read by an adapter expression
        for slot, index in uses:
            if type(slot) is not int or type(index) is not int or not 0 <= slot < 14 or not 0 <= index < 4096:
                raise ShaderError('invalid adapter buffer use')
            cb_sizes[slot] = max(cb_sizes.get(slot, 0), index + 1)

    for reg in constants:
        entry = bindings.get('constants', {}).get(reg)
        if bound and entry is None:
            raise ShaderError(f'missing target constant binding {reg}')
        if entry and 'expr' in entry:
            # measured adapter: an HLSL float4 expression over t6_cbN rows
            use_buffers(entry.get('uses', []))
            reg_expr[reg] = f'({entry["expr"]})'
            continue
        slot, index = (entry['buffer'], entry['index']) if entry else (0, int(reg[1:]))
        if type(slot) is not int or type(index) is not int or not 0 <= slot < 14 or not 0 <= index < 4096:
            raise ShaderError(f'invalid constant binding {reg}')
        cb_sizes[slot] = max(cb_sizes.get(slot, 0), index + 1)
        reg_expr[reg] = f't6_cb{slot}[{index}]'
    sample_scale, sample_alpha, sample_texel = {}, {}, {}
    for reg, entry in bindings.get('samplers', {}).items():
        if entry.get('rgb_scale') or entry.get('alpha_expr'):
            use_buffers(entry.get('uses', []))
        if entry.get('rgb_scale'):
            sample_scale[reg] = entry['rgb_scale']
        if entry.get('alpha_expr'):
            sample_alpha[reg] = entry['alpha_expr']
        if entry.get('texel_expr'):
            use_buffers(entry.get('uses', []))
            sample_texel[reg] = entry['texel_expr']
    epilogue = bindings.get('epilogue', {})
    if epilogue:
        use_buffers(epilogue.get('uses', []))
    for slot, count in sorted(cb_sizes.items()):
        resources.append(f'cbuffer WawBuffer{slot} : register(b{slot}) {{ float4 t6_cb{slot}[{count}]; }};')
    for reg, values in defs.items():
        resources.append(f'static const float4 {reg} = float4({", ".join(repr(v) for v in values)});')
    declared = {}
    for reg, dim in samplers.items():
        entry = bindings.get('samplers', {}).get(reg)
        if bound and entry is None:
            raise ShaderError(f'missing target sampler binding {reg}')
        tex, samp = (entry['texture'], entry['sampler']) if entry else (int(reg[1:]), int(reg[1:]))
        if type(tex) is not int or type(samp) is not int or not 0 <= tex < 128 or not 0 <= samp < 16:
            raise ShaderError(f'invalid resource binding {reg}')
        typ = {'2d': 'Texture2D', 'cube': 'TextureCube', 'volume': 'Texture3D'}[dim]
        if entry and entry.get('constant') is not None:
            continue  # neutral texel (shaderruntime: source system absent in T6)
        if entry and entry.get('point_load'):
            # Point sampling without the target's sampler state (the target
            # binds a comparison sampler here): read the texel directly, with
            # clamp addressing, exactly as an SM3 point sampler returns it.
            if dim != '2d':
                raise ShaderError(f'point load adapter needs a 2D texture: {reg}')
            resources += [f'{typ}<float4> tex_{reg} : register(t{tex});',
                          f'float4 load_{reg}(float2 uv) {{ uint w, h; tex_{reg}.GetDimensions(w, h); '
                          f'int2 p = clamp(int2(floor(uv * float2(w, h))), int2(0, 0), int2(w, h) - 1); '
                          f'return tex_{reg}.Load(int3(p, 0)); }}']
            continue
        # Several source samplers may alias one target slot (e.g. WaW primary +
        # secondary lightmap sub-pages of one T6 lightmap): declare it once.
        key = (typ, tex, samp)
        if key in declared:
            resources += [f'#define tex_{reg} tex_{declared[key]}', f'#define samp_{reg} samp_{declared[key]}']
            continue
        declared[key] = reg
        resources += [f'{typ}<float4> tex_{reg} : register(t{tex});', f'SamplerState samp_{reg} : register(s{samp});']

    for reg, code in sample_texel.items():
        resources.append(f'float4 waw_texel_{reg}(float4 T) {{ return {code}; }}')

    def interface(kind, declarations):
        result = {}
        for reg, (semantic, width) in declarations.items():
            entry = bindings.get(kind, {}).get(reg)
            if bound and entry is None:
                raise ShaderError(f'missing target {kind} binding {reg}')
            if entry:
                semantic, width = entry['semantic'], entry.get('width', width)
            if not SEM.fullmatch(semantic.upper()) or type(width) is not int or not 1 <= width <= 4:
                raise ShaderError(f'invalid interface for {reg}')
            adapter = entry.get('adapter') if entry else None
            if adapter == 'waw_neutral' and kind == 'inputs' and len(entry.get('constant', ())) == 4:
                result[reg] = (semantic, width)
                continue
            if adapter is not None and not (kind == 'inputs' and declarations[reg][1] == 4 and
                ((adapter == 'waw_half_uv' and width == 2) or (adapter == 'waw_position' and width == 3) or
                 (adapter == 'waw_ubyte4_vector' and width == 3) or (adapter in ('waw_uv_lightmap', 'waw_uv') and width == 2))):
                raise ShaderError(f'unsupported vertex input adapter for {reg}')
            if width < declarations[reg][1] and adapter is None:
                raise ShaderError(f'target interface truncates source channels for {reg}')
            result[reg] = (semantic, width)
        return result
    inputs, outputs = interface('inputs', inputs), interface('outputs', outputs)
    if stage == 'vs' and not any(s.lower() == 'sv_position' for s, _ in outputs.values()):
        raise ShaderError('vertex shader needs an explicitly mapped SV_Position output')

    def source(token):
        if token.startswith('-'):
            return f'(-{source(token[1:])})'
        if token.startswith('abs(') and token.endswith(')'):
            return f'abs({source(token[4:-1])})'
        modifier = re.fullmatch(r'(r\d+|v\d+|c\d+)_abs(\.[xyzwrgba]{1,4})?', token)
        if modifier:
            return f'abs({source(modifier[1] + (modifier[2] or ""))})'
        match = REG.fullmatch(token)
        if not match:
            raise ShaderError(f'unsupported operand {token}')
        reg, swizzle = match.groups()
        if reg.startswith('v') and reg not in inputs:
            raise ShaderError(f'undeclared input {reg}')
        expr = reg_expr.get(reg, reg)
        if swizzle:
            swizzle = swizzle.translate(SWIZZLE)
            swizzle += swizzle[-1] * (4 - len(swizzle))
            expr = f'({expr}).{swizzle}'
        return expr

    body, flow = [], []
    repeat_index = 0
    comparison = {'gt': '>', 'lt': '<', 'ge': '>=', 'le': '<=', 'eq': '==', 'ne': '!='}
    for decorated, args, original in operations:
        flags = decorated.split('_')
        op = flags.pop(0)
        if op == 'nop':
            continue
        if op == 'rep' and not flags and len(args) == 1:
            match = re.fullmatch(r'(i\d+)(?:\.x)?', args[0])
            if not match or match[1] not in integers:
                raise ShaderError(f'only literal integer repeat counts are supported: {original}')
            count = integers[match[1]][0]
            if not 0 <= count <= 65535:
                raise ShaderError(f'invalid repeat count: {original}')
            repeat_index += 1
            counter = f'waw_repeat{repeat_index}'
            body.append(f'for (int {counter}=0; {counter}<{count}; ++{counter}) {{')
            flow.append('rep')
            continue
        if op == 'endrep' and not flags and flow and flow[-1] == 'rep':
            flow.pop()
            body.append('}')
            continue
        if op == 'if' and len(flags) == 1 and flags[0] in comparison and len(args) == 2:
            body.append(f'if (({source(args[0])}).x {comparison[flags[0]]} ({source(args[1])}).x) {{')
            flow.append('if')
            continue
        if op == 'else' and not flags and flow and flow[-1] == 'if':
            flow[-1] = 'else'
            body.append('} else {')
            continue
        if op == 'endif' and not flags and flow and flow[-1] in ('if', 'else'):
            flow.pop()
            body.append('}')
            continue
        if any(f not in ('sat', 'pp') for f in flags):
            raise ShaderError(f'unsupported instruction modifier: {original}')
        if op == 'texkill' and len(args) == 1 and stage == 'ps':
            body.append(f'clip(({source(args[0])}).xyz);')
            continue
        arities = {'mov': 1, 'add': 2, 'sub': 2, 'mul': 2, 'mad': 3, 'dp3': 2,
                   'dp4': 2, 'dp2add': 3, 'min': 2, 'max': 2, 'rcp': 1, 'rsq': 1,
                   'abs': 1, 'frc': 1, 'exp': 1, 'log': 1, 'pow': 2,
                   'lrp': 3, 'cmp': 3, 'slt': 2, 'sge': 2, 'nrm': 1,
                   'dsx': 1, 'dsy': 1, 'sincos': 1, 'texld': 2, 'texldp': 2,
                   'texldb': 2, 'texldl': 2, 'texldd': 4}
        if op not in arities or len(args) != arities[op] + 1:
            raise ShaderError(f'unsupported instruction: {original}')
        dst = REG.fullmatch(args[0])
        if not dst or not (dst[1].startswith('r') or dst[1] in outputs):
            raise ShaderError(f'invalid destination: {original}')
        reg, mask = dst.groups()
        mask = (mask or 'xyzw').translate(SWIZZLE)
        if len(set(mask)) != len(mask):
            raise ShaderError(f'invalid write mask: {original}')
        if op == 'sincos' and not set(mask) <= set('xy'):
            # SM3 defines only .x (cosine) and .y (sine)
            raise ShaderError(f'sincos writes outside .xy: {original}')
        if op.startswith('texld'):
            coord, sampler = source(args[1]), args[2]
            if sampler not in samplers:
                raise ShaderError(f'undeclared sampler: {original}')
            dims = 'xy' if samplers[sampler] == '2d' else 'xyz'
            uv = f'({coord}).{dims}'
            v_remap = bindings.get('samplers', {}).get(sampler, {}).get('v_scale_offset')
            if v_remap is not None:
                # the target packs this texture into a sub-rectangle of a page
                if samplers[sampler] != '2d' or op not in ('texld', 'texldl'):
                    raise ShaderError(f'v remap needs a 2D texld/texldl: {original}')
                uv = f'float2(({uv}).x, ({uv}).y * {float(v_remap[0])!r} + {float(v_remap[1])!r})'
            point_load = bindings.get('samplers', {}).get(sampler, {}).get('point_load')
            if point_load and op not in ('texld', 'texldl', 'texldp'):
                raise ShaderError(f'point load adapter cannot express {op}: {original}')
            if op == 'texldp':
                uv = f'({uv} / ({coord}).w)'
            constant = bindings.get('samplers', {}).get(sampler, {}).get('constant')
            if op == 'texldl':
                expr = f'tex_{sampler}.SampleLevel(samp_{sampler}, {uv}, ({coord}).w)'
            elif op == 'texldb':
                expr = f'tex_{sampler}.SampleBias(samp_{sampler}, {uv}, ({coord}).w)'
            elif op == 'texldd':
                expr = f'tex_{sampler}.SampleGrad(samp_{sampler}, {uv}, ({source(args[3])}).{dims}, ({source(args[4])}).{dims})'
            else:
                if stage != 'ps':
                    raise ShaderError('vertex texture reads must use explicit LOD')
                expr = f'tex_{sampler}.Sample(samp_{sampler}, {uv})'
            if point_load:
                # reads mip 0 (WaW shadow lookups pass LOD 0; reported by the contract)
                expr = f'load_{sampler}({uv})'
                if op == 'texldp':
                    # T6 shadow texels hold projected depth z/w (its PS compares
                    # them with coord.z/coord.w); WaW compares the texel with the
                    # undivided coord.z. Scaling by w (> 0) gives the same test.
                    expr = f'({expr} * ({coord}).w)'
            if constant is not None:
                expr = 'float4(' + ', '.join(repr(float(c)) for c in constant) + ')'
            if sampler in sample_scale:
                # target texel encoding differs; convert to the source's units
                expr = f'({expr} * float4(({sample_scale[sampler]}).xxx, 1.0))'
            if sampler in sample_texel:
                # target texel encoding differs; ``T`` is the raw target texel
                expr = f'(waw_texel_{sampler}({expr}))'
            if sampler in sample_alpha:
                # the target supplies this channel's meaning elsewhere
                expr = f'float4(({expr}).xyz, ({sample_alpha[sampler]}))'
        else:
            v = [source(x) for x in args[1:]]
            a = v[0]
            b = v[1] if len(v) > 1 else ''
            c = v[2] if len(v) > 2 else ''
            expr = {
                'mov': a, 'add': f'({a}+{b})', 'sub': f'({a}-{b})',
                'mul': f'({a}*{b})', 'mad': f'({a}*{b}+{c})',
                'dp3': f'dot(({a}).xyz, ({b}).xyz)', 'dp4': f'dot({a}, {b})',
                'dp2add': f'(dot(({a}).xy, ({b}).xy) + ({c}).x)',
                'min': f'min({a},{b})', 'max': f'max({a},{b})',
                'rcp': f'(1.0 / ({a}).x)', 'rsq': f'rsqrt(abs(({a}).x))',
                'abs': f'abs({a})', 'frc': f'frac({a})',
                'exp': f'exp2(({a}).x)', 'log': f'log2(abs(({a}).x))',
                'pow': f'pow(abs(({a}).x), ({b}).x)',
                'lrp': f'({a}*{b}+(1.0-{a})*{c})',
                'cmp': f'(({a}>=0.0)?{b}:{c})',
                'slt': f'(({a}<{b})?1.0:0.0)', 'sge': f'(({a}>={b})?1.0:0.0)',
                'nrm': f'float4(normalize(({a}).xyz), ({a}).w)',
                'dsx': f'ddx({a})', 'dsy': f'ddy({a})',
                'sincos': f'float4(cos(({a}).x), sin(({a}).x), 0.0, 0.0)',
            }[op]
        # Snapshot the full result before masked write, preserving register aliasing.
        if 'sat' in flags:
            expr = f'saturate({expr})'
        body += [f'{{ float4 value = {expr}; {reg}.{mask} = value.{mask}; }}']
    if flow:
        raise ShaderError('unclosed flow control')
    decls = [f'float4 {reg} = 0;' for reg in sorted(used | inputs.keys() | outputs.keys())
             if reg.startswith(('r', 'v', 'o'))]
    fields = lambda items: ' '.join(f'{"centroid " if reg in centroid else ""}float{width if width > 1 else ""} {reg} : {sem};' for reg, (sem, width) in items.items())
    # A pixel shader paired with a translated vertex shader must declare the
    # vertex output signature exactly (D3D11 links by register order).
    signature = bindings.get('input_signature')
    field_of = {reg: reg for reg in inputs}
    if signature is not None:
        by_semantic = {s['semantic'].upper(): (i, s) for i, s in enumerate(signature)}
        for reg, (semantic, width) in inputs.items():
            found = by_semantic.get(semantic.upper())
            if found is None or found[1]['width'] < width:
                raise ShaderError(f'paired signature cannot supply {reg} {semantic}/{width}')
            field_of[reg] = f'link{found[0]}'
        interp = {field_of[reg]: 'centroid ' for reg in centroid if reg in field_of}
        input_fields = ' '.join(f'{interp.get(f"link{i}", "")}float{s["width"] if s["width"] > 1 else ""} link{i} : {s["semantic"]};'
                                for i, s in enumerate(signature))
    else:
        input_fields = fields({reg: decl for reg, decl in inputs.items()
                               if bindings.get('inputs', {}).get(reg, {}).get('adapter') != 'waw_neutral'})
        for reg in inputs:
            if bindings.get('inputs', {}).get(reg, {}).get('adapter') == 'waw_uv_lightmap':
                input_fields += f' float2 {reg}_lmap : TEXCOORD1;'
    init = []
    for reg, (_, width) in inputs.items():
        adapter = bindings.get('inputs', {}).get(reg, {}).get('adapter')
        name = f'input.{field_of[reg]}'
        if adapter == 'waw_neutral':
            # the source system that fills this stream does not exist in T6
            init.append(f'{reg} = float4(' + ', '.join(repr(float(c)) for c in bindings['inputs'][reg]['constant']) + ');')
        elif signature is not None:
            full = next(s['width'] for i, s in enumerate(signature) if f'link{i}' == field_of[reg])
            init.append(f'{reg} = float4({name}{", 0" * (4-full)});')
        elif adapter == 'waw_half_uv':
            init += [f'uint2 half_{reg} = f32tof16({name});',
                     f'{reg} = float4(half_{reg}.y & 255, half_{reg}.y >> 8, half_{reg}.x & 255, half_{reg}.x >> 8);']
        elif adapter == 'waw_uv':
            init.append(f'{reg} = float4({name}, 0, 0);')
        elif adapter == 'waw_uv_lightmap':
            # WaW world vertices carry texture and lightmap UVs in one float4;
            # T6 streams them as TEXCOORD0 and TEXCOORD1
            init.append(f'{reg} = float4({name}, input.{reg}_lmap);')
        elif adapter == 'waw_position':
            init.append(f'{reg} = float4({name}, 1);')
        elif adapter == 'waw_ubyte4_vector':
            # T6 stores signed bytes in unorm channels (v >= .5 wraps negative);
            # WaW reads biased bytes n = b/127 - 1 scaled by w/255 + 0.752941191.
            # Re-encode the unit vector with w = 63 (scale exactly 1).
            init += [f'float3 wrap_{reg} = {name} >= 0.5 ? {name} * 2.0 - 2.0 : {name} * 2.0;',
                     f'{reg} = float4((normalize(wrap_{reg}) + 1.0) * 127.0, 63.0);']
        else:
            init.append(f'{reg} = float4({name}{", 0" * (4-width)});')
    finish = [f'output.{reg} = {reg}.{"xyzw"[:width]};' for reg, (_, width) in outputs.items()]
    rgb_scale = bindings.get('output_rgb_scale')
    if rgb_scale is not None:
        # the target displays this render target at another scale (see shaderruntime)
        if stage != 'ps':
            raise ShaderError('output colour scale applies to pixel programs only')
        finish = [f'output.{reg} = float4({reg}.xyz * {float(rgb_scale)!r}, {reg}.w);'
                  if sem.upper().startswith('SV_TARGET') and width == 4 else line
                  for line, (reg, (sem, width)) in zip(finish, outputs.items())]
    tail = list(epilogue.get('lines', [])) if epilogue else []
    text = '\n'.join(resources + [f'struct Input {{ {input_fields} }};',
        f'struct Output {{ {fields(outputs)} }};', 'Output main(Input input) {',
        *decls, *init, *body, *tail, 'Output output;', *finish, 'return output;', '}']) + '\n'
    return text, {'source_profile': stage + '_3_0', 'target_profile': stage + '_5_0',
        'instruction_count': len(operations), 'inputs': inputs, 'outputs': outputs,
        'constants': constants, 'samplers': samplers,
        'binding_status': 'explicit_contract' if bound else 'requires_t6_bindings',
        'partial_precision': 'promoted_to_float32'}


def translate_file(source: Path, output: Path, bindings: dict | None = None, *, compile=True) -> dict:
    data = source.read_bytes()
    assembly = disassemble(data)
    hlsl, report = translate(assembly, bindings)
    compiled = compile_hlsl(hlsl, report['target_profile']) if compile else None
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix('.hlsl').write_text(hlsl, encoding='utf-8')
    if compiled is not None:
        output.with_suffix('.cso').write_bytes(compiled)
        report['output_sha256'] = hashlib.sha256(compiled).hexdigest()
    report.update(source=str(source), source_sha256=hashlib.sha256(data).hexdigest(),
                  compiled=compiled is not None)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def _stage_shader(task):
    name, source, destination = task
    # T6 donor DXBC is not WaW input. Preserve it through the existing path.
    if source.read_bytes()[:4] == b'DXBC':
        return None
    try:
        report = translate_file(source, destination / name)
        return dict(report, name=name, status='translated_unbound')
    except (ShaderError, OSError, AttributeError) as exc:
        return {'name': name, 'source': str(source), 'status': 'unsupported', 'error': str(exc)}


def stage(roots: list[Path], destination: Path) -> dict:
    """Translate extracted WaW shaders, retaining first-source precedence.

    Staging artifacts are intentionally outside shader_bin: without a verified
    T6 pass contract they cannot override a live shader with incompatible ABI.
    """
    from .parallel import ordered_map
    sources = {}
    for root in roots:
        for folder in (root / 'shader_bin', root / 'content_source/shader_bin'):
            for path in sorted(folder.rglob('*.cso')):
                sources.setdefault(path.relative_to(folder).as_posix(), path)
    rows = [row for row in ordered_map(_stage_shader,
            [(name, source, destination) for name, source in sources.items()], label='Shaders')
            if row is not None]
    report = {'status': 'requires_t6_pass_bindings' if rows else 'no_source_bytecode',
              'translated': sum(r['status'] == 'translated_unbound' for r in rows),
              'unsupported': sum(r['status'] == 'unsupported' for r in rows), 'shaders': rows}
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'stage.json').write_text(json.dumps(report, indent=2) + '\n')
    return report
