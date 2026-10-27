"""Compiled WaW map dynents (physics clutter, destructibles), for the T6 clipmap.

The T4 dumper writes <map>.dynents.json beside the clip dump: per draw list
(0 xmodel, 1 brush model) every DynEntityDef, followed by the 256 empty
runtime slots the game allocates. T6 keeps the same layout; the bridge linker
(ClipMapLinker::LoadDynEnts) reads BSP/dynents.json in source order, so
constraints keep indexing dynents by their source list-0 index.
"""
import json
import math
from pathlib import Path

CLUTTER, DESTRUCT = 1, 2
RUNTIME_SLOTS = 256


def read(source: Path) -> list[dict]:
    """The map's real dynents (list 0, source order). Anything the T6 bridge
    cannot carry yet is an error, never a silent drop."""
    if not source.is_file():
        return []
    data = json.loads(source.read_text())
    if data.get('version') != 1:
        raise ValueError('unsupported map dynent schema')
    count = data['originalDynEntCount']
    models, brushes = data['lists']
    real_brushes = [e for e in brushes if e['type']]
    if real_brushes:
        raise ValueError(f'map has {len(real_brushes)} brush-model dynents; only xmodel dynents are converted')
    entries = models[:count]
    if any(e['type'] for e in models[count:]):
        raise ValueError('map dynents beyond originalDynEntCount')
    for i, e in enumerate(entries):
        if e['type'] not in (CLUTTER, DESTRUCT):
            raise ValueError(f'dynent {i}: unknown type {e["type"]}')
        if not e['xModel']:
            raise ValueError(f'dynent {i}: xmodel dynent without a model')
        if e['destroyPieces']:
            raise ValueError(f'dynent {i} ({e["xModel"]}): destroy pieces {e["destroyPieces"]["name"]} '
                             f'are not converted yet')
        for field, size in (('quat', 4), ('origin', 3)):
            if len(e[field]) != size or not all(math.isfinite(v) for v in e[field]):
                raise ValueError(f'dynent {i}: invalid {field}')
    return entries


def preset_name(entry: dict) -> str:
    """The WaW physics preset; a leading comma marks a zone reference."""
    preset = entry['physPreset']
    return preset['name'].lstrip(',') if preset and preset.get('name') else ''


def write(project: Path, entries: list[dict], presets: dict[str, str], fx_prefix: str) -> dict:
    """BSP/dynents.json with converted asset names; presets maps WaW -> T6."""
    target = project / 'BSP/dynents.json'
    if not entries:
        target.unlink(missing_ok=True)
        return {'count': 0}
    out = []
    for e in entries:
        out.append({'type': e['type'], 'quat': e['quat'], 'origin': e['origin'], 'xModel': e['xModel'],
                    'physPreset': presets.get(preset_name(e), ''),
                    'destroyFx': fx_prefix + e['destroyFx'] if e['destroyFx'] else '',
                    'physConstraints': [c if c != 255 else -1 for c in e['physConstraints']],
                    'health': e['health'], 'flags': e['flags'], 'contents': e['contents'],
                    'targetname': e['targetname']})
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps({'version': 1, 'dynents': out}, indent=1) + '\n')
    return {'count': len(out), 'models': sorted({e['xModel'] for e in entries}),
            'clutter': sum(e['type'] == CLUTTER for e in entries),
            'destructible': sum(e['type'] == DESTRUCT for e in entries)}
