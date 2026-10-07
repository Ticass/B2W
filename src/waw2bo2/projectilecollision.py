"""Recover physical prop surfaces without making movement clip volumes solid.

This is an explicit compatibility repair: a render mesh is not an authored
collision mesh. Source movement brushes qualify physical placements, then the
complete hard mesh blocks MISSILECLIP only, from either side of each surface.
Authored world, entity and model contents are never promoted.
"""
from __future__ import annotations

import base64
import copy
import hashlib
import json
import math
import struct
from collections import defaultdict
from pathlib import Path

from . import hulls, techsets

MISSILECLIP = 0x80
MOVEMENT_CLIP = 0x30000
# WaW SurfaceTypeBits stores enum value - 1 (default has no bit).
# Bark, brick, concrete, metal, plaster, rock, wood, ceramic, plastic,
# rubber and painted metal. Foliage, grass, water, glass and FX are excluded.
HARD_SURFACES = sum(1 << (i - 1) for i in (1, 2, 5, 13, 16, 17, 21, 23, 24, 25, 28))


def accessor(data: dict, buffers: list[bytes], index: int) -> list[tuple]:
    a = data['accessors'][index]
    if a.get('sparse'):
        raise ValueError('sparse GLTF accessor unsupported for prop collision')
    view = data['bufferViews'][a['bufferView']]
    code = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}[a['componentType']]
    width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    fmt = '<' + code * width
    stride = view.get('byteStride', struct.calcsize(fmt))
    offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
    return [struct.unpack_from(fmt, buffers[view['buffer']], offset + i * stride) for i in range(a['count'])]


def hard_triangles(path: Path, roots: list[Path]) -> list[tuple]:
    data = json.loads(path.read_text(encoding='utf-8'))
    buffers = []
    for buffer in data['buffers']:
        uri = buffer['uri']
        buffers.append(base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:')
                       else (path.parent / uri).read_bytes())
    # Exported mesh vertices are in bind pose; bone nodes are not mesh transforms.
    # Reject additional mesh-node transforms rather than silently misplacing them.
    for node in data.get('nodes', []):
        if 'mesh' in node and any(k in node for k in ('matrix', 'translation', 'rotation', 'scale')):
            raise ValueError('transformed GLTF mesh node unsupported for prop collision')
    triangles = []
    for mesh in data['meshes']:
        for primitive in mesh['primitives']:
            if primitive.get('mode', 4) != 4 or 'material' not in primitive:
                continue
            name = data['materials'][primitive['material']]['name']
            material_path = next((r / techsets.oat_material_path(name) for r in roots
                                  if (r / techsets.oat_material_path(name)).is_file()), None)
            if material_path is None:
                continue
            material = json.loads(material_path.read_text(encoding='utf-8'))
            surface = material.get('surfaceTypeBits', 0)
            shader = techsets.parse(material.get('techniqueSet', ''))
            # Imported props often retain WaW's default surface type. Accept
            # these only with an opaque lit model replace pass, never cutouts,
            # blend passes, glass, foliage, unlit cards or missing metadata.
            opaque_default = (surface == 0 and shader.family in {'model', 'model_vertex'}
                              and shader.lit and shader.layers and shader.layers[0][0] == 'r'
                              and material.get('sortKey') == 4)
            if not (surface & HARD_SURFACES or opaque_default):
                continue
            vertices = [(v[0], -v[2], v[1]) for v in accessor(data, buffers, primitive['attributes']['POSITION'])]
            indices = ([v[0] for v in accessor(data, buffers, primitive['indices'])]
                       if 'indices' in primitive else list(range(len(vertices))))
            for i in range(0, len(indices), 3):
                # GLTF indices are opposite the game's collision winding.
                a, b, c = (vertices[j] for j in indices[i:i + 3])
                if hulls._dot(hulls._cross(hulls._sub(b, a), hulls._sub(c, a)),
                              hulls._cross(hulls._sub(b, a), hulls._sub(c, a))) > 1e-10:
                    triangles.append((a, c, b))
    return triangles


def clip_polygon(polygon: list[tuple], planes) -> list[tuple]:
    """Intersect the visible surface with the original convex brush halfspaces."""
    for normal, distance, _ in planes:
        result = []
        if not polygon:
            break
        previous = polygon[-1]
        pd = hulls._dot(normal, previous) - distance
        for current in polygon:
            cd = hulls._dot(normal, current) - distance
            if (pd <= 0) != (cd <= 0):
                t = pd / (pd - cd)
                result.append(tuple(previous[k] + t * (current[k] - previous[k]) for k in range(3)))
            if cd <= 0:
                result.append(current)
            previous, pd = current, cd
        polygon = result
    return polygon


def collision_lod(path: Path, triangle: tuple, material: str) -> None:
    """A real recovered face carries collLod, without duplicating render meshes.

    These assets are referenced only by the clipmap, never the GfxWorld. All
    recovered faces live in collSurfs; one face is enough for the required LOD.
    """
    normal = hulls._unit(hulls._cross(hulls._sub(triangle[2], triangle[0]),
                                    hulls._sub(triangle[1], triangle[0])))
    rotate = lambda p: (p[0], p[2], -p[1])
    positions = [rotate(p) for p in triangle]
    blob = b''.join(struct.pack('<3f', *p) for p in positions)
    blob += struct.pack('<9f', *(rotate(normal) * 3))
    blob += struct.pack('<6f', 0, 0, 0, 1, 1, 0)
    blob += struct.pack('<3H', 0, 2, 1)
    data = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
            'nodes': [{'mesh': 0}], 'materials': [{'name': material}],
            'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(blob).decode()}],
            'bufferViews': [{'buffer': 0, 'byteOffset': o, 'byteLength': n} for o, n in ((0, 36), (36, 36), (72, 24), (96, 6))],
            'accessors': [
                {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                 'min': [min(p[k] for p in positions) for k in range(3)],
                 'max': [max(p[k] for p in positions) for k in range(3)]},
                {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
                {'bufferView': 2, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                {'bufferView': 3, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
            'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}]}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data) + '\n', encoding='utf-8')


def batch_proxies(records: list[dict], entries: list[dict], project_root: Path) -> tuple[list[dict], set[str]]:
    """Pack nearby collision surfaces to avoid exhausting the xmodel asset pool.

    Each original placement keeps its own surface bounds and narrow-phase
    triangles. Grouping changes only the container and coordinate origin.
    """
    groups = defaultdict(list)
    for record, entry in zip(records, entries):
        cell = tuple(math.floor((record['absmin'][k] + record['absmax'][k]) / 512) for k in range(3))
        groups[cell].append((record, entry))
    output, names = [], set()
    for cell, group in sorted(groups.items()):
        for start in range(0, len(group), 64):
            chunk = group[start:start+64]
            for record, entry in chunk:
                entry['source_collision_asset'] = record['name']
            if len(chunk) == 1:
                record, entry = chunk[0]
                output.append(record)
                names.add(record['name'])
                entry['surface'] = 0
                continue
            origin = tuple((v + 0.5) * 256 for v in cell)
            surfaces = []
            for record, entry in chunk:
                model = json.loads((project_root / 'xmodel' / (record['name'] + '.json')).read_text(encoding='utf-8'))
                placement = hulls.Placement(tuple(record['origin']), tuple(record['invScaledAxis']))
                transform = lambda p: tuple(placement.apply(p)[k] - origin[k] for k in range(3))
                for surf in model['collSurfs']:
                    tris = [tuple(transform(p) for p in hulls.collision_triangle(tuple(t['plane'] + t['svec'] + t['tvec'])))
                            for t in surf['tris']]
                    pts = [p for tri in tris for p in tri]
                    surfaces.append({'bone': 'tag_origin', 'contents': MISSILECLIP, 'surfFlags': surf['surfFlags'],
                                     'mins': [min(p[k] for p in pts) for k in range(3)],
                                     'maxs': [max(p[k] for p in pts) for k in range(3)],
                                     'tris': [hulls.collision_tri_record(*tri) for tri in tris]})
                entry['surface'] = len(surfaces) - 1
            # Equal model names do not imply equal world-space batches. Large
            # repeated prop groups can create several chunks in the same cell.
            # Include placement transforms/bounds or a later chunk overwrites
            # an earlier collision asset with geometry from different spots.
            digest = hashlib.sha256(json.dumps([r for r, _ in chunk], sort_keys=True).encode()).hexdigest()[:16]
            name = 'waw_projectile/cell_' + '_'.join(map(str, cell)) + '_' + digest
            lod = 'model_export/' + name + '.gltf'
            carrier = json.loads((project_root / model['lods'][0]['file']).read_text(encoding='utf-8'))
            first = surfaces[0]['tris'][0]
            collision_lod(project_root / lod, hulls.collision_triangle(tuple(first['plane'] + first['svec'] + first['tvec'])),
                          carrier['materials'][0]['name'])
            model.update(lods=[{'file': lod, 'distance': 100000}], collSurfs=surfaces)
            (project_root / 'xmodel' / (name + '.json')).write_text(json.dumps(model, indent=1) + '\n', encoding='utf-8')
            output.append({'name': name, 'contents': MISSILECLIP, 'origin': list(origin),
                           'invScaledAxis': [1, 0, 0, 0, 1, 0, 0, 0, 1],
                           'absmin': [min(r['absmin'][k] for r, _ in chunk) for k in range(3)],
                           'absmax': [max(r['absmax'][k] for r, _ in chunk) for k in range(3)]})
            names.add(name)
            for _, entry in chunk:
                entry['name'] = name
    return output, names


def recover(report, world, clip, project_root: Path, roots: list[Path]) -> set[str]:
    """Append collision-only placements; keep every existing BSP record exact."""
    if not getattr(clip, 'brush_ownership_complete', False) or not clip.submodels:
        report.warnings.append('PROJECTILE_PROP_COLLISION_UNSUPPORTED: complete source brush ownership required')
        return set()
    world_ids = set(clip.submodels[0].brushes)
    entity_ids = {i for sub in clip.submodels[1:] for i in sub.brushes}
    brushes = [clip.brushes[i] for i in sorted(world_ids - entity_ids)
               if clip.brushes[i].contents & MOVEMENT_CLIP
               and not clip.brushes[i].contents & (1 | MISSILECLIP)]
    records_path = project_root / 'BSP/staticmodels.json'
    records = json.loads(records_path.read_text(encoding='utf-8'))
    # Repeated selective staging is idempotent.
    records['staticModels'] = [m for m in records['staticModels'] if not m['name'].startswith('waw_projectile/')]
    original_records = len(records['staticModels'])
    cache, names, entries = {}, set(), []
    for index, placement in enumerate(world.static_models):
        name = placement.name
        model_path = project_root / 'xmodel' / (name + '.json')
        if not model_path.is_file():
            continue
        model = json.loads(model_path.read_text(encoding='utf-8'))
        # Existing collision (including map-placed entity boxes) owns this model.
        if model.get('collSurfs') or model.get('contents', 0) & (1 | MISSILECLIP):
            continue
        if name not in cache:
            try:
                cache[name] = hard_triangles(project_root / model['lods'][0]['file'], roots)
            except (KeyError, ValueError, OSError) as exc:
                report.warnings.append(f'PROJECTILE_PROP_COLLISION_UNSUPPORTED {name}: {exc}')
                cache[name] = []
        triangles = cache[name]
        if not triangles or placement.scale <= 0:
            continue
        axis, origin, scale = placement.axis, placement.origin, placement.scale
        transform = lambda p: tuple(origin[k] + scale * sum(axis[3*j+k] * p[j] for j in range(3)) for k in range(3))
        vertices = [transform(p) for tri in triangles for p in tri]
        mins = [min(v[k] for v in vertices) for k in range(3)]
        maxs = [max(v[k] for v in vertices) for k in range(3)]
        candidates = [b for b in brushes if all(b.mins[k] <= maxs[k] and b.maxs[k] >= mins[k] for k in range(3))]
        if not candidates:
            continue
        overlaps = False
        for triangle in triangles:
            points = [transform(p) for p in triangle]
            lo = [min(p[k] for p in points) for k in range(3)]
            hi = [max(p[k] for p in points) for k in range(3)]
            for brush in candidates:
                if not all(brush.mins[k] <= hi[k] and brush.maxs[k] >= lo[k] for k in range(3)):
                    continue
                polygon = clip_polygon(points, brush.planes)
                for j in range(1, len(polygon)-1):
                    cross = hulls._cross(hulls._sub(polygon[j], polygon[0]), hulls._sub(polygon[j+1], polygon[0]))
                    if hulls._dot(cross, cross) < 1e-10:
                        continue
                    overlaps = True
                    break
                if overlaps:
                    break
            if overlaps:
                break
        if not overlaps:
            continue
        # A player clip often covers only a prop's base/centre. It is evidence
        # of physical intent, not the extent of the visible solid. Clipping the
        # recovered faces to it made the rest of a fence/rock pass through.
        # T6 XModelTraceLine is one-sided. Render meshes can contain open sheets
        # and inconsistent back faces, so missile-only recovery is two-sided.
        recovered = {}
        for a, b, c in triangles:
            for local in ((a, b, c), (a, c, b)):
                key = tuple(round(v, 5) for p in local for v in p)
                recovered[key] = local
        digest = hashlib.sha256(json.dumps(sorted(recovered)).encode()).hexdigest()[:16]
        output = 'waw_projectile/' + name + '_' + digest
        points = [p for tri in recovered.values() for p in tri]
        lo = [min(p[k] for p in points) for k in range(3)]
        hi = [max(p[k] for p in points) for k in range(3)]
        proxy = copy.deepcopy(model)
        source_mesh = json.loads((project_root / model['lods'][0]['file']).read_text(encoding='utf-8'))
        lod_file = 'model_export/' + output + '.gltf'
        collision_lod(project_root / lod_file, next(iter(recovered.values())), source_mesh['materials'][0]['name'])
        proxy.update(type='rigid', rootBoneName='tag_origin', lods=[{'file': lod_file, 'distance': 100000}])
        proxy.update(contents=MISSILECLIP, collLod=0, collSurfs=[{
            'bone': 'tag_origin', 'contents': MISSILECLIP,
            'surfFlags': 0, 'mins': lo, 'maxs': hi,
            'tris': [hulls.collision_tri_record(*tri) for tri in recovered.values()]}])
        target = project_root / 'xmodel' / (output + '.json')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(proxy, indent=1) + '\n', encoding='utf-8')
        bounds = [transform(p) for p in points]
        records['staticModels'].append({'name': output, 'contents': MISSILECLIP, 'origin': list(origin),
            'invScaledAxis': [v / scale for v in axis],
            'absmin': [min(p[k] for p in bounds) for k in range(3)],
            'absmax': [max(p[k] for p in bounds) for k in range(3)]})
        names.add(output)
        entries.append({'source_model': name, 'placement': index, 'name': output, 'triangles': len(recovered),
                        'source_movement_brushes': len(candidates)})
    batched, names = batch_proxies(records['staticModels'][original_records:], entries, project_root)
    records['staticModels'] = records['staticModels'][:original_records] + batched
    records_path.write_text(json.dumps(records, indent=1) + '\n', encoding='utf-8')
    report.content['projectile_prop_collision'] = entries
    report.warnings.append(f'PROJECTILE_PROP_COLLISION_RECOVERED {len(entries)} placements: hard render surfaces '
                           'complete two-sided hard meshes qualified by source movement brushes; '
                           'missile only, source masks unchanged')
    return names
