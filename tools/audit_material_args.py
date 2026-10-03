"""Audit extracted T6 materials against the engine's argument lookups.

T6 sub_77C210 resolves MTL_ARG_MATERIAL_CONST (type 0) by scanning the
material's constant table (32-byte entries) for the argument hash, and
sub_77C150 resolves MTL_ARG_MATERIAL_PIXEL_SAMPLER (type 2) by scanning the
texture table (16-byte entries). sub_777790 resolves the stable pixel material
constants (type 6) through the same constant table. No scan has an end bound, and each
continues from the previous match within one argument group. A hash that is
absent, or present only before the previous match, reads past the table and
crashes (observed at 0x77C253 and 0x7777F9).

usage: audit_material_args.py <extracted zone folder> [<extra techset folder> ...]
"""
import json
import sys
from pathlib import Path


def name_hash(name):
    # T6 R_HashString: ORs 0x20 into every byte (differs from lower() on '_').
    value = 0
    for byte in name.encode():
        value = ((value * 33) ^ (byte | 0x20)) & 0xFFFFFFFF
    return value


def entry_hash(entry):
    if 'nameHash' in entry:
        return entry['nameHash']
    return name_hash(entry['name'])


def load_techsets(folders):
    techsets = {}
    for folder in folders:
        root = Path(folder) / 'techniquesets'
        for path in root.rglob('*.json'):
            name = path.relative_to(root).with_suffix('').as_posix()
            techsets.setdefault(name, path)
    return techsets


def scan(table, wanted, start):
    for index in range(start, len(table)):
        if table[index] == wanted:
            return index
    return None


def audit(zone, extra):
    techsets = load_techsets([zone, *extra])
    problems, checked, missing_techsets = [], 0, set()
    for path in sorted((Path(zone) / 'materials').rglob('*.json')):
        material = json.loads(path.read_text())
        name = path.relative_to(Path(zone) / 'materials').with_suffix('').as_posix()
        ts_name = material.get('techniqueSet')
        if not ts_name:
            continue
        if ts_name not in techsets:
            missing_techsets.add(ts_name)
            continue
        constants = [entry_hash(c) for c in material.get('constants', [])]
        textures = [entry_hash(t) for t in material.get('textures', [])]
        techset = json.loads(techsets[ts_name].read_text())
        for slot, technique in enumerate(techset['techniques']):
            for pass_index, pass_ in enumerate(technique.get('passArray', [])):
                args, cursor = pass_['args'], 0
                for count in ('perPrimArgCount', 'perObjArgCount', 'stableArgCount'):
                    group = args[cursor:cursor + pass_.get(count, 0)]
                    cursor += pass_.get(count, 0)
                    positions = {0: 0, 2: 0, 6: 0}
                    for arg in group:
                        if arg['type'] not in (0, 2, 6):
                            continue
                        checked += 1
                        table = textures if arg['type'] == 2 else constants
                        found = scan(table, arg['u']['value'], positions[arg['type']])
                        if found is None:
                            kind = 'texture' if arg['type'] == 2 else 'constant'
                            state = 'out of order' if arg['u']['value'] in table else 'absent'
                            problems.append(f'{name} [{ts_name} slot {slot} pass {pass_index} {count}] '
                                            f'{kind} hash 0x{arg["u"]["value"]:08x} {state}')
                        else:
                            positions[arg['type']] = found
                if cursor != len(args):
                    problems.append(f'{name} [{ts_name} slot {slot}] argument counts cover {cursor} of {len(args)}')
    return problems, checked, missing_techsets


def main():
    zone, *extra = sys.argv[1:]
    problems, checked, missing = audit(zone, extra)
    for line in problems:
        print(line)
    for name in sorted(missing):
        print(f'techset not found: {name}')
    print(f'{checked} material arguments checked, {len(problems)} violations, {len(missing)} techsets not found')
    return 1 if problems or missing else 0


if __name__ == '__main__':
    sys.exit(main())
