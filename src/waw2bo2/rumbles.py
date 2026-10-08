"""Carry original controller-rumble profiles and their graph dependencies."""
import json
from pathlib import Path
import shutil

from . import gsc, weapons


def stage(project: Path, roots: list[Path], native_raw: Path | None = None) -> dict:
    names = set()
    runtime = project / 'content_source/weapons.runtime.json'
    if runtime.exists():
        for name in json.loads(runtime.read_text())['weapons']:
            path = project / 'content_source/weapons' / name
            if not path.is_file():
                # Native tactical/melee helper weapons are registered alongside
                # converted weapons; their assets belong to the BO2 runtime.
                continue
            fields = weapons.read_info_file(path)
            names.update(v for k, v in fields.items() if k.lower().endswith('rumble') and v)
    calls = {'precacherumble', 'playrumbleonentity', 'playrumblelooponentity', 'playrumbleonposition'}
    for path in (project / 'maps/mp/waw').rglob('*.gsc'):
        tokens = gsc.tokenize(path.read_text())
        for i, token in enumerate(tokens[:-2]):
            if token.low in calls and tokens[i + 1].text == '(' and tokens[i + 2].kind == gsc.STRING:
                names.add(tokens[i + 2].text[1:-1])
    files, missing, native = {}, [], []
    for name in sorted(names):
        weapons.output_name('rumble', name)
        relative = Path('rumble') / name
        source = next((r / relative for r in roots if (r / relative).is_file()), None)
        if source is None and native_raw is not None and (native_raw / relative).is_file():
            source = native_raw / relative
            native.append(name)  # BO2 framework's own precaches, not donor replacements
        if source is None:
            missing.append(relative.as_posix())
            continue
        text = source.read_text(encoding='utf-8', errors='replace')
        fields = text.strip().split('\\')
        if fields[0] != 'RUMBLE' or len(fields) % 2 != 1:
            raise ValueError(f'invalid rumble profile: {source}')
        files[relative.as_posix()] = source
        for key, value in zip(fields[1::2], fields[2::2]):
            if key not in ('lowRumbleFile', 'highRumbleFile') or not value:
                continue
            weapons.output_name('rumble graph', value)
            graph = Path('rumble') / value
            # Graphs belong to this profile's source; another zone's same-name
            # graph must not override it. Search remaining original roots next.
            candidates = [source.parent.parent, *roots]
            original = next((r / graph for r in candidates if (r / graph).is_file()), None)
            if original is None:
                missing.append(graph.as_posix())
            else:
                files[graph.as_posix()] = original
    for name, source in files.items():
        target = project / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.resolve() != target.resolve():
            shutil.copy2(source, target)
    return {'files': sorted(files), 'missing': sorted(set(missing)), 'native_profiles': native}
