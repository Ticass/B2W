"""Adapt native T6 probe readers to the original WaW RGBA cube images.

WaW filters RGB and alpha and multiplies them. T6 divides RGB by alpha
and treats the result as linear light. Convert the sampled value, rather
than baking a different product into texels and changing interpolation.
"""
from __future__ import annotations

import hashlib
import json
import math
import struct
from functools import lru_cache
from pathlib import Path


def checksum(data: bytes) -> bytes:
    """DXBC's MD5 compression with its container-specific final block."""
    size = len(data)
    end = size // 64 * 64
    tail = data[end:] + b'\x80'
    if len(tail) > 56:
        blocks = data[:end] + tail.ljust(64, b'\0')
        tail = b''
    else:
        blocks = data[:end]
    bits = (size * 8) & 0xffffffff
    blocks += struct.pack('<I', bits) + tail.ljust(56, b'\0') + struct.pack('<I', (bits >> 2) | 1)
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    shifts = [7, 12, 17, 22]*4 + [5, 9, 14, 20]*4 + [4, 11, 16, 23]*4 + [6, 10, 15, 21]*4
    constants = [int(abs(math.sin(i+1)) * 2**32) for i in range(64)]
    for offset in range(0, len(blocks), 64):
        words = struct.unpack_from('<16I', blocks, offset)
        a, b, c, d = state
        for i in range(64):
            if i < 16: f, g = (b & c) | (~b & d), i
            elif i < 32: f, g = (d & b) | (~d & c), (5*i+1) % 16
            elif i < 48: f, g = b ^ c ^ d, (3*i+5) % 16
            else: f, g = c ^ (b | ~d), (7*i) % 16
            value = (a + f + constants[i] + words[g]) & 0xffffffff
            shift = shifts[i]
            value = ((value << shift) | (value >> (32-shift))) & 0xffffffff
            a, b, c, d = d, (b+value) & 0xffffffff, b, c
        state = [(x+y) & 0xffffffff for x, y in zip(state, (a,b,c,d))]
    return struct.pack('<4I', *state)


def _chunks(code):
    if code[:4] != b'DXBC' or len(code) < 32:
        raise ValueError('native probe adapter requires DXBC')
    count = struct.unpack_from('<I', code, 28)[0]
    result = []
    for offset in struct.unpack_from(f'<{count}I', code, 32):
        size = struct.unpack_from('<I', code, offset+4)[0]
        result.append((code[offset:offset+4], code[offset+8:offset+8+size]))
    return result


@lru_cache(maxsize=512)
def adapt(code: bytes, texture: int) -> bytes:
    """Insert a unit conversion after native, full-register cube samples.

    Three elementwise instructions consume no new temporary registers,
    constants, textures or varyings. Reject unfamiliar operand layouts.
    """
    chunks = _chunks(code)
    changed = 0
    for ci, (tag, body) in enumerate(chunks):
        if tag not in (b'SHDR', b'SHEX'): continue
        words = list(struct.unpack(f'<{len(body)//4}I', body))
        output = words[:2]
        i = 2
        while i < len(words):
            token = words[i]
            length = (token >> 24) & 127
            if token & 2047 == 53: length = words[i+1]  # custom data
            if not length or i+length > len(words): raise ValueError('invalid DXBC instruction')
            inst = words[i:i+length]
            output.extend(inst)
            # SAMPLE, SAMPLE_L, SAMPLE_D and SAMPLE_B; skip opcode extensions.
            if token & 2047 in (69, 72, 73, 74):
                at = 1
                extended = token
                while extended & 0x80000000:
                    extended = inst[at]; at += 1
                # dest and coordinates have one immediate register index.
                if len(inst) > at+5 and (inst[at+4] >> 12) & 255 == 7 and inst[at+5] == texture:
                    if inst[at] != 0x001000f2 or ((inst[at+2] >> 20) & 3) != 1:
                        raise ValueError('unsupported native cube sample operands')
                    reg = inst[at+1]
                    # sample.rgb = (sample.rgb * sample.a)^2; sample.a = 1.
                    output += [0x07000038, 0x00100072, reg, 0x00100e46, reg, 0x00100ff6, reg]
                    output += [0x07000038, 0x00100072, reg, 0x00100e46, reg, 0x00100e46, reg]
                    output += [0x05000036, 0x00100082, reg, 0x00004001, 0x3f800000]
                    changed += 1
            i += length
        output[1] = len(output)
        chunks[ci] = tag, struct.pack(f'<{len(output)}I', *output)
    if not changed: raise ValueError('native cube sample not found')
    offset = 32 + 4*len(chunks)
    offsets, payload = [], bytearray()
    for tag, body in chunks:
        offsets.append(offset+len(payload)); payload += tag + struct.pack('<I',len(body)) + body
    result = bytearray(b'DXBC' + bytes(16) + struct.pack('<3I',1,offset+len(payload),len(chunks))
                       + struct.pack(f'<{len(chunks)}I',*offsets) + payload)
    result[4:20] = checksum(bytes(result[20:]))
    return bytes(result)


def bind(material: dict, project: Path, stock: Path) -> int:
    from . import shaderruntime
    name = material.get('techniqueSet', '')
    file = next((p for p in (project/'techniquesets'/f'{name}.json', stock/'techniquesets'/f'{name}.json') if p.is_file()), None)
    if file is None: return 0
    data = json.loads(file.read_text())
    count = 0
    for technique in data['techniques']:
        for p in (technique or {}).get('passArray', []):
            shader = p.get('pixelShader', {}).get('name', '')
            if shader.startswith(('waw/runtime_', 'waw/native_probe_')): continue
            path = next((x for x in (project/'shader_bin'/f'ps_{shader}.cso', stock/'shader_bin'/f'ps_{shader}.cso') if x.is_file()), None)
            if path is None: continue
            assembly = shaderruntime._assembly(path)
            slots = [i for i, (n, _) in shaderruntime.resource_names(assembly).items() if n == 'reflectionProbeSampler']
            if not slots: continue
            if len(slots) != 1: raise ValueError('ambiguous native reflection probe resource')
            code = adapt(path.read_bytes(), slots[0])
            shader = 'waw/native_probe_' + hashlib.sha256(code).hexdigest()[:20]
            out = project/'shader_bin'/f'ps_{shader}.cso'; out.parent.mkdir(parents=True,exist_ok=True); out.write_bytes(code)
            p['pixelShader'] = {'name':shader}; count += 1
    if count:
        identity = 'waw/runtime_' + hashlib.sha256(json.dumps(data,sort_keys=True).encode()).hexdigest()[:16]
        data['name'] = identity
        for i, t in enumerate(data['techniques']):
            if t: t['name'] = f'{identity}_{i}'
        out = project/'techniquesets'/f'{identity}.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(data))
        material['techniqueSet'] = identity
    return count


def bind_native_materials(dump: Path, overlay: Path, stock: Path) -> dict[str, int]:
    """Adapt stock gameplay readers sharing the converted world's probe cubes.

    Native weapons and impact decals retain their own textures and pass state.
    Materials already supplied by the translated overlay remain authoritative.
    """
    changed = {}
    for source in sorted((dump/'materials').rglob('*.json')):
        relative = source.relative_to(dump)
        target = overlay/relative
        if target.exists():
            continue
        material = json.loads(source.read_text())
        count = bind(material, overlay, stock)
        if count:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(material), encoding='utf-8')
            changed[relative.as_posix()] = count
    return changed
