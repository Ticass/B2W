"""Align native animated barricades with authored WaW plank planes."""
import json
import math
import re
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


# ---- WaW window rebuild motion on the native boards ----
#
# Windows stay BO2 zbarriers (the entity-budget exception), whose boards fly
# back with the barrier asset's repair animations (o_zombie_board_N_repair: a
# 1.1 s flip with a bounce). WaW has no rebuild animation: the map's own
# _zombiemode_blockers replace_chunk moves the board in script. It shows the
# board where it landed, lifts it to its height (MoveTo only_z) while turning
# it upright (RotateTo), waits, then pushes it into the window (MoveTo og_origin).
# The same motion is written over each native board's repair animation, from
# its stock start pose (where the pull left it) to its stock rest pose, timed
# by the map's literals. The barrier asset and entities stay as they are.

REPLACE_CHUNK_SCRIPTS = ('maps\\_zombiemode_blockers_new', 'maps\\_zombiemode_blockers')
REPAIR_FPS = 30


def replace_chunk_timing(text: str) -> dict | None:
    """The map's replace_chunk timings (seconds) without the fast-reload perk:
    lift (MoveTo only_z), turn (RotateTo), hold (wait after the turn) and
    push (MoveTo og_origin). None when the function does not have that shape."""
    start = re.search(r'^replace_chunk\s*\(', text, re.M | re.I)
    if start is None:
        return None
    rest = text[start.end():]
    nxt = re.search(r'^[A-Za-z_]\w*\s*\([^;{]*\)\s*$\s*\{', rest, re.M)
    body = rest[:nxt.start()] if nxt else rest
    body = re.sub(r'//[^\n]*', '', body)
    num = r'(\d*\.?\d+)'
    lift = re.search(r'MoveTo\s*\(\s*only_z\s*,\s*' + num + r'\s*\)\s*;\s*\w+\s+RotateTo\s*\([^;]*,\s*' + num
                     + r'\s*\)\s*;[^;]*;\s*wait\s*\(\s*' + num + r'\s*\)', body, re.I)
    push = re.findall(r'MoveTo\s*\(\s*\w+\.og_origin\s*,\s*' + num + r'\s*\)', body, re.I)
    if lift is None or not push:
        return None
    # the perk branch comes first in WaW; the last og_origin move is the plain one
    return {'lift': float(lift.group(1)), 'turn': float(lift.group(2)), 'hold': float(lift.group(3)),
            'push': float(push[-1])}


def _decode_repair(raw: bytes) -> dict:
    """Header, root (delta) translation and the single bone's tracks (first
    and last key) of a raw v17/v19 board animation, in the layout
    animnotes.notification_offset walks."""
    version, frames, bones, flags, kind, fps = struct.unpack_from('<3H2BH', raw)
    if bones != 1 or not flags & 2 or flags & 1:
        raise ValueError('board animation: expected one bone, root motion, not looping')
    pos = 10

    def u16():
        nonlocal pos
        pos += 2
        return struct.unpack_from('<H', raw, pos - 2)[0]

    def skip_indices(n):
        # tracks keyed on every frame omit their frame indices
        nonlocal pos
        if n < frames:
            pos += n if frames - 1 < 256 else 2 * n

    def trans():
        nonlocal pos
        n = u16()
        if n == 0:
            return [(0.0, 0.0, 0.0)] * 2
        if n == 1:
            v = struct.unpack_from('<3f', raw, pos)
            pos += 12
            return [v, v]
        skip_indices(n)
        small = raw[pos]
        mins, rng = struct.unpack_from('<3f', raw, pos + 1), struct.unpack_from('<3f', raw, pos + 13)
        pos += 25
        width, top, fmt = (1, 255, 'B') if small else (2, 65535, 'H')
        keys = [struct.unpack_from('<3' + fmt, raw, pos + i * 3 * width) for i in range(n)]
        pos += n * 3 * width
        return [tuple(mins[k] + rng[k] * key[k] / top for k in range(3)) for key in (keys[0], keys[-1])]

    def quat(stored):
        nonlocal pos
        n = u16()
        if n == 0:
            return None
        if n > 1:
            skip_indices(n)
        keys = [struct.unpack_from(f'<{stored // 2}h', raw, pos + i * stored) for i in range(n)]
        pos += n * stored
        return [keys[0], keys[-1]]

    if quat(2) is not None:
        raise ValueError('board animation: root rotation is not supported')
    root = trans()
    flip, half = raw[pos], raw[pos + 1]
    pos += 2
    end = raw.index(b'\0', pos)
    bone = raw[pos:end]
    pos = end + 1
    if half & 1:
        raise ValueError('board animation: simple (z-only) bone rotation is not supported')
    rotation = quat(6)
    offset = trans()
    if rotation is None:
        raise ValueError('board animation: the board bone has no rotation track')
    return {'header': (version, kind, fps), 'flip': flip, 'bone': bone, 'root': root,
            'rotation': [_unpack_quat(q) for q in rotation], 'offset': offset, 'tail': raw[pos:]}


def _unpack_quat(q) -> tuple:
    x, y, z = (v / 32767.0 for v in q)
    return x, y, z, math.sqrt(max(0.0, 1.0 - x * x - y * y - z * z))


def _slerp(a, b, f):
    dot = sum(p * q for p, q in zip(a, b))
    if dot < 0:
        b, dot = tuple(-q for q in b), -dot
    if dot > 0.9995:
        r = tuple(p + (q - p) * f for p, q in zip(a, b))
    else:
        theta = math.acos(dot)
        s0, s1 = math.sin((1 - f) * theta) / math.sin(theta), math.sin(f * theta) / math.sin(theta)
        r = tuple(s0 * p + s1 * q for p, q in zip(a, b))
    length = math.sqrt(sum(v * v for v in r))
    r = tuple(v / length for v in r)
    # stored without w: keep the half with w >= 0
    return r if r[3] >= 0 else tuple(-v for v in r)


def _lerp(a, b, f):
    return tuple(p + (q - p) * f for p, q in zip(a, b))


def _pack_trans(values) -> bytes:
    mins = [min(v[k] for v in values) for k in range(3)]
    rng = [max(v[k] for v in values) - mins[k] for k in range(3)]
    keys = b''.join(struct.pack('<3H', *(round((v[k] - mins[k]) / rng[k] * 65535) if rng[k] else 0
                                          for k in range(3))) for v in values)
    return (struct.pack('<H', len(values)) + bytes([0]) + struct.pack('<3f', *mins)
            + struct.pack('<3f', *rng) + keys)


def waw_repair_animation(raw: bytes, timing: dict) -> bytes:
    """The board's stock repair animation retimed as WaW's replace_chunk:
    lift to the rest height while turning upright, hold, push into place."""
    stock = _decode_repair(raw)
    (start, rest), (q0, q1), (o0, o1) = stock['root'], stock['rotation'], stock['offset']
    settle = max(timing['lift'], timing['turn']) + timing['hold']
    total = settle + timing['push']
    frames = max(2, round(total * REPAIR_FPS) + 1)
    roots, quats, offsets = [], [], []
    for i in range(frames):
        t = i / REPAIR_FPS if i < frames - 1 else total
        turn = min(1.0, t / timing['turn']) if timing['turn'] > 0 else 1.0
        if t <= settle:
            lift = min(1.0, t / timing['lift']) if timing['lift'] > 0 else 1.0
            root = (start[0], start[1], start[2] + (rest[2] - start[2]) * lift)
        else:
            push = min(1.0, (t - settle) / timing['push']) if timing['push'] > 0 else 1.0
            root = _lerp((start[0], start[1], rest[2]), rest, push)
        roots.append(root)
        quats.append(_slerp(q0, q1, turn))
        offsets.append(_lerp(o0, o1, turn))
    version, kind, _ = stock['header']
    out = struct.pack('<3H2BH', version, frames, 1, 2, kind, REPAIR_FPS)
    out += struct.pack('<H', 0) + _pack_trans(roots)
    out += bytes([stock['flip'], 0]) + stock['bone'] + b'\0'
    out += struct.pack('<H', frames) + b''.join(
        struct.pack('<3h', *(round(v * 32767) for v in q[:3])) for q in quats)
    out += _pack_trans(offsets)
    return out + stock['tail']


def board_repair_animations(barrier_asset: str) -> list[str]:
    """boardAnimN of a T6 zbarrier asset (InfoString text)."""
    fields = barrier_asset.split('\\')
    pairs = dict(zip(fields[1::2], fields[2::2]))
    return [pairs[f'boardAnim{i}'] for i in range(1, 64) if pairs.get(f'boardAnim{i}')]


def stage_waw_repair_animations(barrier_asset: str, bo2: Path, timing: dict, destination: Path) -> dict:
    """Write each board's WaW-timed repair animation to destination/xanim
    under its stock name (the mod link puts this layer before the stock raw
    folder, so the stock barrier asset plays it)."""
    written = {}
    for name in board_repair_animations(barrier_asset):
        target = destination / 'xanim' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(waw_repair_animation((bo2 / 'raw/xanim' / name).read_bytes(), timing))
        written[name] = round(max(timing['lift'], timing['turn']) + timing['hold'] + timing['push'], 3)
    return written
