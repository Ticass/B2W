"""Compare staged FBX brush triangles with the linked T6 diagnostic dump."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def audit(fbx: Path, diagnostic: Path) -> dict:
    expected = Counter()
    for owner, corners in re.findall(
            r'Geometry::waw_mesh_\d+_bm(\d+)_pl[^\n]*\n.*?PolygonVertexIndex: \*(\d+)',
            fbx.read_text(), re.S):
        if int(owner):
            if int(corners) % 3:
                raise ValueError('FBX triangle index count is not divisible by three')
            expected[int(owner)] += int(corners) // 3
    linked = {}
    ranges = []
    text = diagnostic.read_text()
    static_count = int(re.search(r'staticSurfaceCount (\d+)', text)[1])
    surface_count = int(re.search(r' surfaces (\d+) lightmaps ', text)[1])
    for owner, start, count, triangles in re.findall(
            r'brushModel (\d+) startSurfIndex (\d+) surfaceCount (\d+) triangles (\d+)', text):
        owner, start, count, triangles = map(int, (owner, start, count, triangles))
        if owner and count:
            linked[owner] = triangles
            ranges.append((start, start + count, owner))
    errors = []
    for owner in sorted(expected.keys() | linked.keys()):
        if expected[owner] != linked.get(owner, 0):
            errors.append(f'brush {owner}: staged {expected[owner]} triangles, linked {linked.get(owner, 0)}')
    cursor = static_count
    for start, end, owner in sorted(ranges):
        if start != cursor or end > surface_count:
            errors.append(f'brush {owner}: invalid or overlapping range {start}:{end}, expected start {cursor}')
        cursor = end
    if cursor != surface_count:
        errors.append(f'unowned linked surfaces: covered {cursor}, total {surface_count}')
    return {'models': len(expected), 'triangles': sum(expected.values()),
            'static_surfaces': static_count, 'total_surfaces': surface_count, 'errors': errors}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fbx', type=Path)
    parser.add_argument('diagnostic', type=Path)
    args = parser.parse_args()
    result = audit(args.fbx, args.diagnostic)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result['errors']))
