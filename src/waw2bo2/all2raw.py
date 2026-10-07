"""Extract every installed game zone once into shared, resumable asset caches."""
from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

SCHEMA = 1
WAW_ASSETS = ('clipmap,gfxworld,gameworldsp,material,image,fx,xmodel,weapon,xanim,sound,'
              'loadedsound,rawfile,comworld,lightdef,physpreset,snddriverglobals,localize')


def parallel_zones(zones, extract, *, label: str, workers: int | None = None):
    """Run independent native processes on every available logical CPU.

    Return results in input order so completion order cannot change which
    duplicate asset wins. Completed zone receipts survive any failed job.
    """
    if not zones:
        return []
    if workers is None:
        workers = int(os.environ.get('WAW2BO2_EXTRACT_WORKERS') or os.cpu_count() or 1)
    if workers < 1:
        raise ValueError('Extraction worker count must be at least 1')
    workers = min(workers, len(zones))
    print(f'Extract All: {label}: {workers} parallel workers, {len(zones)} zones', flush=True)
    started = time.monotonic()
    results = [None] * len(zones)
    with ThreadPoolExecutor(max_workers=workers, thread_name_prefix='extract-zone') as pool:
        pending = {pool.submit(extract, zone): index for index, zone in enumerate(zones)}
        try:
            for completed, future in enumerate(as_completed(pending), 1):
                results[pending[future]] = future.result()
                print(f'Extract All: {label}: {completed}/{len(zones)} zones completed '
                      f'({time.monotonic() - started:.1f}s)', flush=True)
        except BaseException:
            # Finish active jobs so their caches remain reusable, but do not
            # start queued work after an error. No complete cache is published.
            for future in pending:
                future.cancel()
            raise
    return results


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stamp(path: Path) -> list[int]:
    stat = path.stat()
    return [stat.st_size, stat.st_mtime_ns]


def key(data) -> str:
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()[:24]


def search_paths(paths) -> str:
    # Native Unlinker aborts when constructing a search path for a missing
    # directory. WaW normally has zone/<language> and main, but no zone/all
    # or sound; those optional layouts must never be passed unconditionally.
    return ';'.join(str(path) for path in paths if path.is_dir())


def write_json(path: Path, data) -> None:
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(data, sort_keys=True), encoding='utf-8')
    temporary.replace(path)


@contextmanager
def cache_lock(root: Path):
    root.mkdir(parents=True, exist_ok=True)
    with (root / 'extract.lock').open('a+b') as stream:
        try:
            if os.name == 'nt':
                import msvcrt
                stream.write(b'0')
                stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError(f'Game assets are being extracted by another process: {root}') from error
        yield


def run(command: list[str], log: Path) -> None:
    from .progress import native
    log.parent.mkdir(parents=True, exist_ok=True)
    result = native(command, log)
    if result.returncode:
        print(log.read_text(encoding='utf-8', errors='replace')[-8000:], flush=True)
        raise RuntimeError(f'Extraction failed ({result.returncode}); see {log}')


def listing(text: str, *, references: bool = False) -> dict[str, list[str]]:
    assets: dict[str, list[str]] = {}
    for line in text.splitlines():
        kind, separator, name = line.partition(',')
        name = name.strip()
        if separator and name and (references or not name.startswith(',')) and kind.strip().replace('_', '').isalnum():
            assets.setdefault(kind.strip(), []).append(name.lstrip(','))
    return assets


def extract_zone(ff: Path, tool: Path, root: Path, search: str, *, assets: str | None,
                 image_format: str, tool_digest: str | None = None, dependencies=None,
                 refresh: bool = False, list_assets: bool = True) -> tuple[Path, dict]:
    """Publish a zone only after both native extraction and indexing succeed."""
    inputs = {'schema': SCHEMA, 'file': str(ff.resolve()), 'stamp': stamp(ff),
              'tool': tool_digest or digest(tool), 'dependencies': dependencies,
              'assets': assets, 'images': image_format, 'models': 'GLTF', 'list': list_assets}
    folder = root / key(inputs)
    if not refresh:
        try:
            pointer = json.loads((root / (key(inputs) + '.json')).read_text())['folder']
            candidate = root / pointer
            if candidate.parent.resolve() == root.resolve():
                folder = candidate
        except (OSError, ValueError, KeyError):
            pass
    receipt = folder / 'extraction.json'
    try:
        cached = json.loads(receipt.read_text(encoding='utf-8'))
        if not refresh and cached['inputs'] == inputs and all(
                (folder / name).is_file() for name in cached['files']):
            return folder, cached
    except (OSError, ValueError, KeyError):
        pass
    # Unique generations preserve outputs already being read by another build.
    if folder.exists():
        folder = root / (key(inputs) + '-' + uuid.uuid4().hex[:8])
    temporary = root / ('pending-' + uuid.uuid4().hex)
    temporary.mkdir(parents=True)
    log = root / (ff.stem + '.log')
    command = [str(tool), '--no-color', '--search-path', search,
               '--image-format', image_format, '--model-format', 'GLTF',
               '--output-folder', str(temporary)]
    if assets is not None:
        command += ['--include-assets', assets]
    command.append(str(ff))
    run(command, log)
    index: dict[str, list[str]] = {}
    loaded_index: dict[str, list[str]] = {}
    if list_assets:
        list_log = root / (ff.stem + '.assets.log')
        run([str(tool), '--no-color', '--list', '--search-path', search, str(ff)], list_log)
        index = listing(list_log.read_text(encoding='utf-8', errors='replace'))
        loaded_index = listing(list_log.read_text(encoding='utf-8', errors='replace'), references=True)
    files = sorted(p.relative_to(temporary).as_posix() for p in temporary.rglob('*') if p.is_file())
    if not files:
        raise RuntimeError(f'Extractor returned success without writing files for {ff}; see {log}')
    data = {'inputs': inputs, 'files': files, 'index': index, 'loaded_index': loaded_index}
    write_json(temporary / 'extraction.json', data)
    temporary.rename(folder)
    # A pointer lets retries/refreshes find the new generation on subsequent runs.
    write_json(root / (key(inputs) + '.json'), {'folder': folder.name})
    return folder, data


@dataclass(frozen=True)
class CachePaths:
    waw: Path
    bo2: Path

    @classmethod
    def for_settings(cls, settings):
        base = Path(settings.work).resolve() / 'asset_cache'
        def install(value):
            value = str(Path(value).resolve())
            return value.casefold() if os.name == 'nt' else value
        return cls(base / ('waw_' + key(install(settings.waw))),
                   base / ('bo2_' + key(install(settings.bo2))))


def _rank(name: str) -> tuple[int, str]:
    # Keep the Zombies runtime/framework authoritative for duplicate filenames.
    stem = Path(name).stem
    core = ('patch_zm', 'code_post_gfx_zm', 'common_zm', 'code_post_gfx', 'common')
    return (core.index(stem) if stem in core else len(core), name)


def inventory(game: Path, tool: Path, engine: str) -> dict:
    archives = {str(p.relative_to(game)): stamp(p) for directory in ('main', 'zone', 'sound')
                for p in (game / directory).rglob('*') if p.is_file() and p.suffix.lower() in
                ('.iwd', '.ipak', '.sabl', '.sabs')}
    derived = {}
    if engine == 'T6':
        for path in [game / 't6zm.exe', *sorted((game / 'raw').rglob('*.gsc')),
                     *sorted((game / 'raw').rglob('*.csc'))]:
            if path.is_file():
                derived[str(path.relative_to(game))] = stamp(path)
    return {'schema': SCHEMA, 'engine': engine, 'game': str(game.resolve()), 'tool': digest(tool),
            'archives': archives, 'derived': derived,
            'zones': {p.relative_to(game).as_posix(): stamp(p) for p in sorted((game / 'zone').rglob('*.ff'))}}


def ready(game: Path, tool: Path, root: Path, *, engine: str) -> Path:
    try:
        receipt = json.loads((root / 'all2raw.json').read_text(encoding='utf-8'))
        raw = Path(receipt['raw'])
        if receipt['inventory'] == inventory(game, tool, engine) and (raw / 'catalog.json').is_file():
            catalog = json.loads((raw / 'catalog.json').read_text(encoding='utf-8'))
            if all((raw / name).is_file() for name in catalog['owners']) and all(
                    (Path(entry['folder']) / name).is_file()
                    for entry in receipt['zones'].values() for name in entry['files']):
                if engine != 'T6' or (raw / 't6api_cache.json').is_file():
                    return raw
    except (OSError, ValueError, KeyError):
        pass
    raise RuntimeError(f'{engine} game asset cache is missing or stale. Run Extract All in Setup before building.')


def prepare(game: Path, tool: Path, root: Path, *, engine: str, refresh: bool = False,
            workers: int | None = None) -> Path:
    if not refresh:
        try:
            raw = ready(game, tool, root, engine=engine)
            print(f'Extract All: reusing complete {engine} cache: {raw}', flush=True)
            return raw
        except RuntimeError:
            pass
    zones = sorted((game / 'zone').rglob('*.ff'), key=lambda p: _rank(p.relative_to(game).as_posix()))
    if not zones:
        raise FileNotFoundError(f'No installed fastfiles found in {game / "zone"}')
    inputs = inventory(game, tool, engine)
    tool_hash = inputs['tool']
    archives = inputs['archives']
    with cache_lock(root):
        def extract(ff):
            name = ff.relative_to(game).as_posix()
            zone_root = root / 'zones' / key(name)
            # Extraction stamps are checked independently: changing one FF does
            # not invalidate another FF or a different custom map's cache.
            print(f'Extract All: {engine} starting {name}', flush=True)
            folder, data = extract_zone(ff, tool, zone_root,
                search_paths((ff.parent, game / 'zone/all', game / 'main', game / 'sound', game)),
                assets=WAW_ASSETS if engine == 'T4' else None,
                image_format='DDS' if engine == 'T4' else 'IWI', tool_digest=tool_hash,
                dependencies=archives, refresh=refresh)
            return {'folder': str(folder), 'files': data['files'], 'index': data['index'],
                    'loaded_index': data['loaded_index'], 'inputs': data['inputs']}
        outputs = parallel_zones(zones, extract, label=engine, workers=workers)
        entries = {ff.relative_to(game).as_posix(): data for ff, data in zip(zones, outputs)}
        generation = key({'entries': entries, 'inventory': inputs})
        raw = root / 'views' / generation
        if (raw / 'catalog.json').is_file():
            previous = json.loads((raw / 'catalog.json').read_text())
            if not all((raw / name).is_file() for name in previous['owners']):
                raw = root / 'views' / (generation + '-' + uuid.uuid4().hex[:8])
        if not (raw / 'catalog.json').is_file():
            pending = root / 'views' / ('pending-' + uuid.uuid4().hex)
            pending.mkdir(parents=True)
            owners: dict[str, str] = {}
            conflicts = []
            for number, (name, entry) in enumerate(entries.items(), 1):
                print(f'Extract All: assembling {engine} cache {number}/{len(entries)} '
                      f'{name} ({len(entry["files"])} files)', flush=True)
                folder = Path(entry['folder'])
                for filename in entry['files']:
                    if filename.startswith('zone_source/'):
                        continue
                    source = folder / filename
                    target = pending / filename
                    if filename in owners:
                        if target.stat().st_size != source.stat().st_size or digest(target) != digest(source):
                            conflicts.append({'asset': filename, 'selected': owners[filename], 'alternative': name})
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        os.link(source, target)
                    except OSError:
                        shutil.copy2(source, target)
                    owners[filename] = name
            write_json(pending / 'catalog.json', {'schema': SCHEMA, 'zones': entries,
                                                'owners': owners, 'conflicts': conflicts})
            pending.rename(raw)
        if engine == 'T6':
            from . import t6api
            print('Extract All: indexing BO2 script/API metadata', flush=True)
            t6api.build(game, raw / 't6api_cache.json', tool, stock_dump=raw)
        write_json(root / 'all2raw.json', {'schema': SCHEMA, 'engine': engine, 'raw': str(raw),
                                         'zones': entries, 'inventory': inputs})
        print(f'Extract All: {engine} cache ready ({len(zones)} zones): {raw}', flush=True)
        return raw


def extract_all(settings, *, refresh=False, cache_paths: CachePaths | None = None) -> tuple[Path, Path]:
    paths = cache_paths or CachePaths.for_settings(settings)
    waw = prepare(Path(settings.waw), Path(settings.t4) / 'Unlinker.exe', paths.waw, engine='T4', refresh=refresh)
    bo2 = prepare(Path(settings.bo2), Path(settings.t6) / 'Unlinker.exe', paths.bo2, engine='T6', refresh=refresh)
    if settings.fastfile and Path(settings.fastfile).is_file():
        source_dumps(settings, paths, refresh=refresh)
    return waw, bo2


def source_dumps(settings, paths: CachePaths, *, refresh=False) -> dict[str, Path]:
    source = Path(settings.fastfile).resolve()
    folder = source.parent
    tool = Path(settings.t4) / 'Unlinker.exe'
    root = paths.waw / 'custom_maps' / key(str(folder))
    dependencies = {str(p): stamp(p) for directory in (folder, Path(settings.waw) / 'main')
                    for p in directory.glob('*.iwd')}
    result = {}
    with cache_lock(root):
        selected = [('map', source), ('patch', folder / (source.stem + '_patch.ff')),
                    ('mod', folder / 'mod.ff')]
        expected = {ff for _, ff in selected}
        selected += [('extra:' + ff.stem, ff) for ff in sorted(folder.glob('*.ff')) if ff not in expected]
        selected = [(role, ff) for role, ff in selected if ff.is_file()]
        unique = list(dict.fromkeys(ff for _, ff in selected))
        tool_hash = digest(tool)
        def extract(ff):
            print(f'Extract All: custom map starting {ff.name}', flush=True)
            output, _ = extract_zone(ff, tool, root / key(str(ff)),
                search_paths((folder, Path(settings.waw) / 'main')), assets=WAW_ASSETS,
                image_format='DDS', tool_digest=tool_hash, dependencies=dependencies, refresh=refresh)
            return output
        outputs = dict(zip(unique, parallel_zones(unique, extract, label='custom map')))
        result = {role: outputs[ff] for role, ff in selected}
    return result
