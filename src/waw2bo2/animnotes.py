"""Adapt raw v17 weapon sound notifications without changing motion or timing."""
import hashlib
import json
import struct
from pathlib import Path
from . import weapons


def add_sound_events(raw: bytes, sound_map: dict[str, str]) -> tuple[bytes, list[dict]]:
    keys = {key: alias for key, alias in sound_map.items() if not key.startswith('sndnt#')}
    if not any(key.encode() + b'\0' in raw for key in keys):
        return raw, []
    if len(raw) < 4 or struct.unpack_from('<H', raw)[0] != 17:
        raise weapons.WeaponError('weapon sound notifications require a raw v17 animation')
    # Raw v17 ends with count:u8 followed by (name:cstring, frame:u16). The
    # section starts where the motion tracks end, so walk their layout rather
    # than guess: a map's own notes may hold any bytes (School's PM63 reload
    # ships empty and binary names past its last frame) and stay as WaW has them.
    try:
        start = notification_offset(raw)
        pos, notes = start + 1, []
        for _ in range(raw[start]):
            end = raw.index(b'\0', pos)
            notes.append((raw[pos:end].decode('latin-1'), struct.unpack_from('<H', raw, end + 1)[0]))
            pos = end + 3
    except (IndexError, ValueError, struct.error):
        pos = -1
    if pos != len(raw):
        raise weapons.WeaponError('raw animation has no valid notification suffix')
    existing = set(notes)
    result, added = [], []
    for name, frame in notes:
        result.append((name, frame))
        if name in keys:
            native = 'sndnt#' + keys[name]
            if (native, frame) not in existing:
                result.append((native, frame))
                added.append({'source': name, 'sound': keys[name], 'frame': frame})
    if not added:
        return raw, []
    # The linker appends an end event; T6's final notifyCount is also u8.
    if len(result) > 254:
        raise weapons.WeaponError('converted animation exceeds notification count limit including end event')
    return raw[:start] + bytes([len(result)]) + b''.join(
        name.encode('latin-1') + b'\0' + struct.pack('<H', frame) for name, frame in result), added


def notification_offset(raw: bytes) -> int:
    """Offset of the note count in a raw v17 xanim, after its motion tracks
    (the layout OpenAssetTools' CompiledXAnimWriter writes)."""
    frames, bones, flags = struct.unpack_from('<HHB', raw, 2)
    numframes = frames if flags & 1 else frames - 1
    loop_frames, byte_indices = numframes + 1, numframes < 256
    pos = 10

    def count():
        nonlocal pos
        pos += 2
        return struct.unpack_from('<H', raw, pos - 2)[0]

    def keyed(n, size):
        # tracks covering every loop frame omit their indices
        nonlocal pos
        if n < loop_frames:
            pos += n if byte_indices else 2 * n
        pos += n * size

    def trans():
        nonlocal pos
        n = count()
        if n == 1:
            pos += 12
        elif n > 1:
            keyed(n, 0)
            small = raw[pos]
            pos += 1 + 24 + n * (3 if small else 6)

    if flags & 2:
        n = count()
        pos += 2 if n == 1 else 0
        if n > 1:
            keyed(n, 2)
        trans()
    if bones:
        mask = (bones + 7) // 8
        half = raw[pos + mask:pos + 2 * mask]
        pos += 2 * mask
        for _ in range(bones):
            pos = raw.index(b'\0', pos) + 1
        for bone in range(bones):
            stored = 2 if half[bone // 8] >> (bone % 8) & 1 else 6
            n = count()
            if n == 1:
                pos += stored
            elif n > 1:
                keyed(n, stored)
            trans()
    return pos


def stage_weapon_events(content: Path) -> dict:
    path = content / 'weapons.stage.json'
    stage = json.loads(path.read_text())
    report = {}
    for entry in stage['weapons']:
        weapon = content / 'weapons' / entry['name']
        if not weapon.is_file():
            continue
        fields = weapons.read_info_file(weapon)
        sound_map = dict(line.split() for line in fields.get('notetrackSoundMap', '').splitlines() if line.strip())
        if not sound_map:
            continue
        for key, name in list(fields.items()):
            if not name.startswith(weapons.PREFIXES['xanim']):
                continue
            source = content / 'xanim' / name
            if not source.is_file():
                continue
            raw, added = add_sound_events(source.read_bytes(), sound_map)
            if not added:
                continue
            # Shared animations may map the same note to different weapon sounds.
            digest = hashlib.sha256(raw).hexdigest()[:16]
            target_name = weapons.output_name('xanim', 'sound_events/' + digest + '/' + name.removeprefix(weapons.PREFIXES['xanim']))
            target = content / 'xanim' / target_name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
            fields[key] = target_name
            report[target_name] = {'source': name, 'added': added}
        weapon.write_text(weapons.write_info(fields), encoding='utf-8')
    stage['animation_sound_events'] = report
    path.write_text(json.dumps(stage, indent=2) + '\n')
    return report
