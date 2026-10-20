"""Align native animated barricades with authored WaW plank planes."""
import json
import math
import struct
from pathlib import Path

# Tear animation whose root height fixes the barrier origin above the floor.
TEAR_ANIM = 'ai_zombie_boardtear_aligned_m_1_grab'
# Search window for the floor below the WaW goal (goals sit 8-27 above it).
FLOOR_ABOVE_GOAL, FLOOR_BELOW_GOAL = 8.0, 64.0


def repaired_translation(path: Path) -> tuple[float, float, float]:
    """Final root translation of the measured T6 basic-board repair format.

    Raw v19: header, empty delta quaternion, indexed compressed delta
    translation. Reject other contracts instead of guessing a fixed offset.
    See OAT's CompiledXAnimLoader::LoadDeltaTransTrack.
    """
    data = path.read_bytes()
    version, frames, bones, flags, kind, fps = struct.unpack_from('<3H2BH', data)
    if version != 19 or flags != 2 or not bones or not fps:
        raise ValueError(f'{path}: unsupported barricade repair animation header')
    if struct.unpack_from('<H', data, 10)[0] != 0:
        raise ValueError(f'{path}: barricade repair has an unsupported delta rotation')
    count = struct.unpack_from('<H', data, 12)[0]
    offset = 14
    if count == 1:
        return struct.unpack_from('<3f', data, offset)
    if count < 2:
        raise ValueError(f'{path}: barricade repair has no root translation')
    # Non-looping animation has frames keys, numbered 0 through frames-1.
    if count < frames:
        size = 1 if frames <= 256 else 2
        indices = struct.unpack_from('<' + ('B' if size == 1 else 'H') * count, data, offset)
        offset += count * size
        if indices[-1] != frames - 1:
            raise ValueError(f'{path}: barricade repair omits its final frame')
    small = data[offset]
    if small not in (0, 1):
        raise ValueError(f'{path}: invalid compressed translation flag')
    mins = struct.unpack_from('<3f', data, offset + 1)
    ranges = struct.unpack_from('<3f', data, offset + 13)
    width, maximum = (1, 255) if small else (2, 65535)
    values = struct.unpack_from('<3' + ('B' if small else 'H'), data,
                                offset + 25 + (count - 1) * 3 * width)
    result = tuple(mins[k] + ranges[k] * values[k] / maximum for k in range(3))
    if not all(math.isfinite(x) for x in result):
        raise ValueError(f'{path}: non-finite repaired translation')
    return result


class Floor:
    """Highest walkable collision at or below a height: world triangles and
    solid world brushes of the WaW clip map."""

    # Plain Python: the packaged converter ships no numpy (the nightly's
    # Windows package tests failed on its import), and a map asks only a few
    # dozen floor heights.
    def __init__(self, clip):
        v, ix = clip.vertices, clip.indices
        self.tris = []
        for k in range(0, len(ix) - 2, 3):
            a, b, c = (tuple(float(n) for n in v[i]) for i in ix[k:k + 3])
            self.tris.append((min(a[0], b[0], c[0]), max(a[0], b[0], c[0]),
                              min(a[1], b[1], c[1]), max(a[1], b[1], c[1]), a, b, c))
        owned = {b for model in clip.submodels[1:] for b in model.brushes}
        self.brushes = [b for i, b in enumerate(clip.brushes) if i not in owned and b.contents & 1]

    def below(self, x: float, y: float, ceiling: float) -> float | None:
        best = None
        for x0, x1, y0, y1, a, b, c in self.tris:
            if not (x0 <= x <= x1 and y0 <= y <= y1):
                continue
            det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(det) <= 1e-9:
                continue
            l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det
            l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det
            l3 = 1 - l1 - l2
            z = l1 * a[2] + l2 * b[2] + l3 * c[2]
            if min(l1, l2, l3) >= -1e-6 and z <= ceiling and (best is None or z > best):
                best = z
        heights = [best] if best is not None else []
        for brush in self.brushes:
            if not (brush.mins[0] <= x <= brush.maxs[0] and brush.mins[1] <= y <= brush.maxs[1]):
                continue
            top, bottom = math.inf, -math.inf
            for (nx, ny, nz), dist, _ in brush.planes:
                rest = dist - nx * x - ny * y
                if nz > 1e-6:
                    top = min(top, rest / nz)
                elif nz < -1e-6:
                    bottom = max(bottom, rest / nz)
                elif rest < -1e-3:
                    break
            else:
                if bottom <= top <= ceiling:
                    heights.append(top)
        return max(heights) if heights else None


def tear_root_height(bo2: Path) -> float:
    """Height of zbarrier.origin above the attacking zombie's feet.

    _zm_spawner plays every tear and entry animation with animscripted at
    zbarrier.origin, and their root stays a constant ~31 units below it
    (stock T6 xanims), so native barriers stand that far above the floor."""
    return -repaired_translation(bo2 / 'raw/xanim' / TEAR_ANIM)[2]


def align_native_boards(bsp: Path, bo2: Path, clip=None) -> dict:
    """Keep attack slots outside while matching the source boards' depth.

    T6 places its attack slots at the zbarrier origin, but its repair animation
    advances the boards about thirty units. The source entry struct is the
    repair trigger, not the animated barrier's root. Boards are the goal
    group's brush models other than its "clip" brush: stock prefabs number
    them, custom maps (Project X) leave them unnamed or name them freely.
    With the WaW clip map, the origin is also lifted to the tear animations'
    root height above the floor; WaW goals sit 8-27 units above it, which sank
    zombies into the floor while tearing and after climbing in.
    """
    path = bsp / 'entities.json'
    data = json.loads(path.read_text())
    es = data['entities']
    poses, aligned, warnings = {}, [], []
    floor = Floor(clip) if clip is not None else None
    height = tear_root_height(bo2) if floor is not None else None
    for goal in es:
        if goal.get('targetname') != 'exterior_goal' or not goal.get('target'):
            continue
        group = [e for e in es if e.get('targetname') == goal['target']]
        native = [e for e in group if e.get('classname') == 'zbarrier_zmcore_BasicWoodBarrier']
        boards = [e for e in group if e.get('classname') == 'script_brushmodel'
                  and e.get('script_noteworthy') != 'clip']
        if len(native) != 1 or not boards:
            continue
        origin = tuple(float(x) for x in goal.get('origin', '0 0 0').split())
        pitch, yaw, _ = [math.radians(float(x)) for x in goal.get('angles', '0 0 0').split()]
        forward = (math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), -math.sin(pitch))
        depths = [sum((float(x) - origin[k]) * forward[k]
                      for k, x in enumerate(board['origin'].split())) for board in boards]
        names = [native[0][f'zbarrierboardanim{i}'] for i in range(1, int(native[0]['zbarriernumboards']) + 1)]
        for name in names:
            if name not in poses:
                poses[name] = repaired_translation(bo2 / 'raw/xanim' / name)
        source_depth = sum(depths) / len(depths)
        native_depth = sum(poses[name][0] for name in names) / len(names)
        shift = source_depth - native_depth
        root = [origin[k] + forward[k] * shift for k in range(3)]
        entry = {'target': goal['target'], 'source_depth': source_depth,
                 'native_repaired_depth': native_depth, 'root_advance': shift}
        if floor is not None:
            ground = floor.below(root[0], root[1], origin[2] + FLOOR_ABOVE_GOAL)
            if ground is None or ground < origin[2] - FLOOR_BELOW_GOAL:
                warnings.append(f"{goal['target']} at {goal.get('origin')}: no floor under the barrier; "
                                f"height left at the WaW goal")
            else:
                entry['floor'], entry['lift'] = ground, ground + height - root[2]
                root[2] = ground + height
        native[0]['origin'] = ' '.join(f'{v:.9g}' for v in root)
        native[0]['angles'] = goal.get('angles', '0 0 0')
        aligned.append(entry)
    path.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')
    return {'aligned': aligned, 'repair_poses': poses, 'warnings': warnings}


def drop_replaced_chunks(bsp: Path) -> dict:
    """Leave out source plank and clip brushes a native zbarrier replaces.

    BO2 _zm_blockers deletes a window group's brush chunks in favour of its
    zbarrier, but only after level init. T6 zombies leaves 912 of its 1022
    entities to the map (the first 110 slots are reserved; G_Spawn sub_54FCC0
    fails at 1022), so ~7 brushes per window can exhaust the budget before
    BO2 ever deletes them (Project X: "G_Spawn: no free entities" on the first
    frame). Stock BO2 windows (zm_prison) carry no such brushes. Run after
    align_native_boards, which measures the source planks.
    """
    path = bsp / 'entities.json'
    data = json.loads(path.read_text())
    es = data['entities']
    replaced = set()
    for goal in es:
        if goal.get('targetname') != 'exterior_goal' or not goal.get('target'):
            continue
        group = [e for e in es if e.get('targetname') == goal['target']]
        if sum(e.get('classname') == 'zbarrier_zmcore_BasicWoodBarrier' for e in group) == 1:
            replaced.add(goal['target'])
    kept = [e for e in es if not (e.get('classname') == 'script_brushmodel' and e.get('targetname') in replaced)]
    dropped = len(es) - len(kept)
    if dropped:
        data['entities'] = kept
        path.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')
    return {'windows': len(replaced), 'dropped_brushes': dropped}
