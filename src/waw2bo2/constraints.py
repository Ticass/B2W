"""Compiled WaW map joints/ropes, translated to the native T6 clipmap contract."""
import json
import math
from pathlib import Path
import shutil
import re


def stage(source: Path, project: Path, dynents: list[dict] = ()) -> dict:
    target = project / 'BSP/constraints.json'
    if not source.is_file():
        target.unlink(missing_ok=True)
        return {'count': 0, 'ropes': 0, 'materials': [], 'exported': False}
    data = json.loads(source.read_text())
    if data.get('version') != 1:
        raise ValueError('unsupported map physics constraint schema')
    entries = data['constraints']
    if len(entries) > 511:
        raise ValueError('map physics constraint count exceeds T6 limit 511')
    materials = set()
    ropes = 0
    for entry in entries:
        if entry['type'] not in range(9):
            raise ValueError('unknown WaW physics constraint type')
        ropes += entry['type'] == 8
        for number in (1, 2):
            attachment = entry[f'attach_point_type{number}']
            if attachment == 1 and not 0 <= entry[f'target_index{number}'] < len(dynents):
                raise ValueError('map physics dynent attachment outside the map dynents')
            if attachment not in (0, 1, 2, 3):
                raise ValueError('unknown WaW physics attachment type')
            if attachment in (2, 3):
                if not entry[f'target_ent{number}']:
                    raise ValueError('map physics entity attachment has no name to rebind')
                entry[f'target_index{number}'] = -1
        for field in ('offset', 'pos', 'pos2', 'dir', 'scale'):
            vector = entry[field]
            if len(vector) != 3 or not all(math.isfinite(v) for v in vector):
                raise ValueError(f'invalid map physics vector {field}')
        for field in ('distance', 'damp', 'power', 'spin_scale', 'minAngle', 'maxAngle'):
            if not math.isfinite(entry[field]):
                raise ValueError(f'invalid map physics scalar {field}')
        material = entry['material'].lstrip(',')
        entry['material'] = material
        if material:
            materials.add(material)
    if ropes > 300:
        raise ValueError('map rope count exceeds T6 limit 300')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(data, indent=2) + '\n')
    return {'count': len(entries), 'ropes': ropes, 'materials': sorted(materials), 'exported': True}


def quat_axis(q) -> tuple:
    """Rows of the rotation of a WaW/T6 GfxPlacement quaternion (x, y, z, w)."""
    x, y, z, w = q
    return ((1 - 2*(y*y + z*z), 2*(x*y + w*z), 2*(x*z - w*y)),
            (2*(x*y - w*z), 1 - 2*(x*x + z*z), 2*(y*z + w*x)),
            (2*(x*z + w*y), 2*(y*z - w*x), 1 - 2*(x*x + y*y)))


def stage_entities(project: Path) -> dict:
    """T6 MP builds runtime joints from entity text, not the compiled clip table.

    WaW removes info_null rope endpoints when compiling them into constraints.
    Reconstruct those endpoints from the compiled attachment frames so T6's
    native parser can bind the same joints at map startup.
    """
    from . import techsets
    manifest = project / 'BSP/constraints.json'
    if not manifest.is_file():
        return {'ropes': 0, 'endpoints': 0, 'materials': []}
    entries = json.loads(manifest.read_text())['constraints']
    if not entries:
        return {'ropes': 0, 'endpoints': 0, 'materials': []}
    dynent_file = project / 'BSP/dynents.json'
    dynents = json.loads(dynent_file.read_text())['dynents'] if dynent_file.is_file() else []
    path = project / 'BSP/entities.json'
    data = json.loads(path.read_text())
    entities = data['entities']
    generated, materials = [], set()
    names = {e.get('targetname') for e in entities}
    vector = lambda v: ' '.join(format(x, '.9g') for x in v)

    def endpoint(entry, number):
        point = entry['pos' if number == 1 else 'pos2']
        attachment = entry[f'attach_point_type{number}']
        if attachment == 0:
            return point, None
        if attachment == 1:
            # a rope hanging a dynent: its point is in the dynent's frame and
            # the native rope parser finds the dynent by its targetname
            dynent = dynents[entry[f'target_index{number}']]
            if not dynent.get('targetname'):
                raise ValueError(f"native rope dynent attachment {entry[f'target_index{number}']}: no targetname")
            axis = quat_axis(dynent['quat'])
            origin = dynent['origin']
            return [origin[k] + sum(point[j]*axis[j][k] for j in range(3)) for k in range(3)], dynent['targetname']
        if attachment != 2:
            raise ValueError('native rope entity translation requires world or entity attachment')
        name = entry[f'target_ent{number}']
        matches = [e for e in entities if e.get('targetname') == name and
                   e.get('classname') in ('script_model', 'script_brushmodel')]
        if len(matches) != 1:
            raise ValueError(f'native rope attachment {name}: expected one script model')
        entity = matches[0]
        origin = [float(v) for v in entity['origin'].split()]
        pitch, yaw, roll = [math.radians(float(v)) for v in entity.get('angles', '0 0 0').split()]
        sp, cp, sy, cy, sr, cr = math.sin(pitch), math.cos(pitch), math.sin(yaw), math.cos(yaw), math.sin(roll), math.cos(roll)
        axis = ((cp*cy, cp*sy, -sp), (sr*sp*cy-cr*sy, sr*sp*sy+cr*cy, sr*cp),
                (cr*sp*cy+sr*sy, cr*sp*sy-sr*cy, cr*cp))
        return [origin[k] + sum(point[j]*axis[j][k] for j in range(3)) for k in range(3)], name

    for i, entry in enumerate(entries):
        if entry['type'] != 8:
            raise ValueError(f"native map joint type {entry['type']} requires runtime entity translation")
        points = [endpoint(entry, n) for n in (1, 2)]
        # Anchor at a world endpoint when present; both named endpoints are
        # represented by info_nulls with their parent target relationships.
        anchor = next((point for point, parent in points if parent is None), points[0][0])
        rope = {'classname': 'rope', 'origin': vector(anchor), 'length': str(entry['distance']),
                'force_scale': str(entry['scale'][0]), 'length_scale': str(entry['scale'][1]),
                'width': str(entry['minAngle']), 'damp': str(entry['damp']),
                'spawnflags': str(entry['flags']), 'timeout': str(entry['timeout'] / 1000)}
        if entry['targetname']:
            rope['targetname'] = entry['targetname']
        material = entry['material']
        if material:
            # The native rope parser prepends mc/ to this field. Keep the
            # converted source pixels under an isolated, compatible identity.
            alias = 'mc/waw_rope/' + material
            destination = project / techsets.oat_material_path(alias)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(project / techsets.oat_material_path(material), destination)
            rope['material'] = alias.removeprefix('mc/')
            materials.add(alias)
        for number, (point, parent) in enumerate(points, 1):
            if parent is None and point == anchor:
                continue
            name = f'waw_rope_{i:04d}_end{number}'
            if name in names:
                raise ValueError(f'native rope endpoint name collision: {name}')
            names.add(name)
            end = {'classname': 'info_null', 'targetname': name, 'origin': vector(point)}
            if parent:
                end['target'] = parent
            generated.append(end)
            rope['target' if number == 1 else 'target2'] = name
        generated.append(rope)
    data['entities'] = [e for e in entities if e.get('classname') != 'rope'] + generated
    path.write_text(json.dumps(data, indent=1) + '\n')
    return {'ropes': len(entries), 'endpoints': len(generated)-len(entries), 'materials': sorted(materials)}


def stage_script_suspensions(project: Path) -> dict:
    """Register fixed-world decorative suspensions before source PhysicsLaunch.

    T6 MP's centity rope anchors follow ScriptMovers; unlike dynent anchors,
    they do not create a rigid-body joint. Keep these models on a server-owned
    pendulum instead of letting client physics detach them from their ropes.
    Breakable, timed and multiply constrained bodies require a separate solver.
    """
    assets = project / 'maps/mp/waw/_waw2bo2_assets.gsc'
    if not assets.is_file():
        return {'suspensions': 0, 'unsupported': []}
    manifest = project / 'BSP/constraints.json'
    entries = json.loads(manifest.read_text())['constraints'] if manifest.exists() else []
    entities = json.loads((project / 'BSP/entities.json').read_text())['entities'] if entries else []
    owners = {}
    for e in entries:
        for n in (1, 2):
            if e[f'attach_point_type{n}'] == 2:
                owners.setdefault(e[f'target_ent{n}'], []).append(e)
    lines = ['    // BEGIN SCRIPT SUSPENSIONS', '    level.waw_rope_suspensions = [];']
    unsupported = []
    count = 0
    vec = lambda v: '(' + ', '.join(format(float(x), '.9g') for x in v) + ')'
    for name, joints in sorted(owners.items()):
        e = joints[0]
        n = 1 if e['target_ent1'] == name else 2
        other = 3 - n
        model = next((x for x in entities if x.get('targetname') == name and x.get('classname') == 'script_model'), None)
        if (len(joints) != 1 or e['type'] != 8 or e[f'attach_point_type{other}'] != 0
                or any(e.get(k, 0) for k in ('flags', 'timeout', 'min_health', 'max_health'))
                or e['distance'] <= 0 or model is None):
            unsupported.append(name)
            continue
        point = e['pos' if n == 1 else 'pos2']
        anchor = e['pos2' if n == 1 else 'pos']
        key = json.dumps(name)
        lines += ['    suspension = spawnstruct();',
                  f'    suspension.anchor = {vec(anchor)};',
                  f'    suspension.point = {vec(point)};',
                  f'    suspension.length = {format(e["distance"], ".9g")};',
                  f'    suspension.damp = {format(e["damp"], ".9g")};',
                  f'    level.waw_rope_suspensions[{key}] = suspension;']
        count += 1
    lines.append('    // END SCRIPT SUSPENSIONS')
    source = assets.read_text(encoding='utf-8')
    block = '\n'.join(lines)
    pattern = r'    // BEGIN SCRIPT SUSPENSIONS.*?    // END SCRIPT SUSPENSIONS'
    if re.search(pattern, source, re.S):
        source = re.sub(pattern, lambda _: block, source, flags=re.S)
    else:
        if 'init()\n{' not in source:
            raise ValueError('asset initializer missing while registering rope suspensions')
        source = source.replace('init()\n{', 'init()\n{\n' + block, 1)
    assets.write_text(source, encoding='utf-8')
    return {'suspensions': count, 'unsupported': unsupported}
