"""Extract every installed game zone once into shared, resumable asset caches."""
from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
import uuid

SCHEMA = 3
WAW_ASSETS = ('clipmap,gfxworld,gameworldsp,material,image,fx,xmodel,weapon,xanim,sound,'
              'loadedsound,rawfile,comworld,lightdef,physpreset,snddriverglobals,localize')


def stock_assets(engine: str, ff: Path) -> str:
    """Cache conversion metadata across maps without exporting entire games."""
    if engine == 'T4':
        if ff.stem == 'code_post_gfx':
            return 'rawfile,image,lightdef'
        # Keep scripts and a complete definition index. Other stock payloads
        # are fetched by the WaW dependency resolver only when referenced.
        return 'rawfile'
    assets = 'material,techniqueset,snddriverglobals,zbarrier'
    if ff.stem == 'zm_prison':
        assets += ',image'
    if ff.stem == 'zm_prison':
        assets += ',xmodel'
    return assets


def trim_barrier_payload(folder: Path, data: dict) -> dict:
    """Keep only the basic wooden barrier's render closure from prison."""
    from .entities import ZBARRIER_ASSET
    from . import techsets
    from .t6bridge import _techset_shaders
    barrier = folder / 'zbarrier' / ZBARRIER_ASSET
    if not barrier.is_file():
        raise RuntimeError(f'Prison donor is missing required barrier {ZBARRIER_ASSET}')
    keep = {barrier.relative_to(folder).as_posix()}
    parts = barrier.read_text(encoding='utf-8').split('\\')[1:]
    models = {value.lstrip(',') for field, value in zip(parts[::2], parts[1::2])
              if value and re.search(r'model\d+$', field, re.I)}
    materials = set()
    for name in models:
        relative = f'xmodel/{name}.json'
        path = folder / relative
        if not path.is_file():
            raise RuntimeError(f'Barrier dependency model {name} is missing from prison donor')
        keep.add(relative)
        model = json.loads(path.read_text())
        for lod in model.get('lods', []):
            relative = lod['file']
            keep.add(relative)
            gltf = json.loads((folder / relative).read_text())
            materials.update(m['name'].lstrip(',') for m in gltf.get('materials', []) if m.get('name'))
            for asset in [*gltf.get('buffers', []), *gltf.get('images', [])]:
                uri = asset.get('uri', '')
                if uri and not uri.startswith('data:'):
                    dependency = (folder / relative).parent / uri
                    dependency = dependency.resolve()
                    if not dependency.is_relative_to(folder.resolve()):
                        raise RuntimeError('Barrier GLTF dependency escapes the cache')
                    keep.add(dependency.relative_to(folder.resolve()).as_posix())
    for name in materials:
        path = folder / techsets.oat_material_path(name)
        if not path.is_file():
            raise RuntimeError(f'Barrier material {name} is missing from prison donor')
        material = json.loads(path.read_text())
        keep.add(path.relative_to(folder).as_posix())
        technique = material.get('techniqueSet')
        if technique:
            technique_path = folder / 'techniquesets' / (technique + '.json')
            if technique_path.is_file():
                keep.add(technique_path.relative_to(folder).as_posix())
                keep.update(_techset_shaders(json.loads(technique_path.read_text())))
        for texture in material.get('textures', []):
            image = texture.get('image', '').lstrip(',')
            if image and not image.startswith('$'):
                keep.add(techsets.oat_image_path(image).as_posix())
    payloads = {'xmodel', 'model_export', 'images', 'zbarrier', 'materials', 'techniquesets', 'shader_bin'}
    files = []
    for name in data['files']:
        if name.split('/')[0] in payloads and name not in keep:
            (folder / name).unlink()
        else:
            files.append(name)
    result = {**data, 'files': files}
    write_json(folder / 'extraction.json', result)
    print(f'Extract All: prison barrier closure retained ({len(models)} models, '
          f'{len(files)}/{len(data["files"])} files)', flush=True)
    return result


def required_bo2_image(stock: Path, name: str, tool: Path | None = None) -> Path | None:
    """Persist one requested stock image, discarding the rest of its zone dump."""
    from . import techsets
    from .resources import resource_root
    catalog_file = stock / 'catalog.json'
    if not catalog_file.is_file():
        return None
    catalog = json.loads(catalog_file.read_text())
    for entry in catalog['zones'].values():
        if name not in entry['index'].get('image', []):
            continue
        ff = Path(entry['inputs']['file'])
        tool = tool or resource_root() / 'vendor/OpenAssetToolsT6/build/bin/Release_x86/Unlinker.exe'
        root = stock.parent.parent / 'dependencies/images' / key([str(ff), name])
        relative = techsets.oat_image_path(name)
        with cache_lock(root):
            output, data = extract_zone(ff, tool, root, search_paths((ff.parent, ff.parents[2])),
                assets='image', image_format='IWI', list_assets=False)
            wanted = output / relative
            if not wanted.is_file():
                return None
            for filename in data['files']:
                if filename != relative.as_posix():
                    (output / filename).unlink()
            write_json(output / 'extraction.json', {**data, 'files': [relative.as_posix()]})
            return wanted
    return None


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


def prune_stock_cache(root: Path, entries: dict, raw: Path) -> None:
    """Remove unpublished/obsolete stock generations; retain custom sources."""
    print('Extract All: removing obsolete stock exports and partial views', flush=True)
    keep = {Path(entry['folder']).resolve() for entry in entries.values()}
    for zone_root in (root / 'zones').iterdir():
        if not zone_root.is_dir():
            continue
        for generation in zone_root.iterdir():
            if generation.is_dir() and generation.resolve() not in keep:
                shutil.rmtree(generation)
            elif generation.suffix == '.json':
                try:
                    target = zone_root / json.loads(generation.read_text())['folder']
                    if target.resolve() not in keep:
                        generation.unlink()
                except (OSError, ValueError, KeyError):
                    continue
    for view in (root / 'views').iterdir():
        if view.is_dir() and view.resolve() != raw.resolve():
            shutil.rmtree(view)


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


def reuse_compact_subset(root: Path, temporary: Path, inputs: dict) -> dict | None:
    """Migrate prior full exports using hard links, without another native dump."""
    assets = inputs['assets']
    if assets is None:
        return None
    wanted = set(assets.split(','))
    supported = {'rawfile', 'material', 'techniqueset', 'image', 'snddriverglobals', 'zbarrier', 'xmodel'}
    if not wanted <= supported:
        return None
    comparable = {k: v for k, v in inputs.items() if k not in ('schema', 'assets')}
    prefixes = {'zone_source'}
    for kind, paths in {'material': ('materials', 'shader_bin'),
                        'techniqueset': ('techniquesets', 'shader_bin'), 'image': ('images',),
                        'snddriverglobals': ('sounddriverglobals',), 'zbarrier': ('zbarrier',),
                        'xmodel': ('xmodel', 'model_export')}.items():
        if kind in wanted:
            prefixes.update(paths)
    for receipt in sorted(root.glob('*/extraction.json')):
        try:
            cached = json.loads(receipt.read_text(encoding='utf-8'))
            old = cached['inputs']
            if {k: v for k, v in old.items() if k not in ('schema', 'assets')} != comparable:
                continue
            if old['assets'] is not None and not wanted <= set(old['assets'].split(',')):
                continue
            rawfiles = {n.replace('\\', '/').lower() for n in cached['index'].get('rawfile', [])}
            files = [n for n in cached['files'] if n.split('/')[0] in prefixes or
                     ('rawfile' in wanted and n.lower() in rawfiles)]
            if not files or not all((receipt.parent / n).is_file() for n in files):
                continue
            for name in files:
                target = temporary / name
                target.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(receipt.parent / name, target)
                except OSError:
                    shutil.copy2(receipt.parent / name, target)
            print(f'Extract All: retaining compact subset of {Path(inputs["file"]).name} '
                  f'({len(files)}/{len(cached["files"])} files)', flush=True)
            return {**cached, 'inputs': inputs, 'files': files}
        except (OSError, ValueError, KeyError):
            continue
    return None


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
    if not refresh:
        migrated = reuse_compact_subset(root, temporary, inputs)
        if migrated is not None:
            write_json(temporary / 'extraction.json', migrated)
            temporary.rename(folder)
            write_json(root / (key(inputs) + '.json'), {'folder': folder.name})
            return folder, migrated
    log = root / (ff.stem + '.log')
    command = [str(tool), '--no-color', '--search-path', search,
               '--image-format', image_format, '--model-format', 'GLTF',
               '--output-folder', str(temporary)]
    if assets is not None:
        command += ['--include-assets', assets]
    command.append(str(ff))
    try:
        run(command, log)
    except BaseException:
        shutil.rmtree(temporary)
        raise
    index: dict[str, list[str]] = {}
    loaded_index: dict[str, list[str]] = {}
    if list_assets:
        list_log = root / (ff.stem + '.assets.log')
        try:
            run([str(tool), '--no-color', '--list', '--search-path', search, str(ff)], list_log)
        except BaseException:
            shutil.rmtree(temporary)
            raise
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


def stock_zones(game: Path, engine: str) -> list[Path]:
    zones = (game / 'zone').rglob('*.ff')
    if engine == 'T6':
        # Nuketown donors and its always-loaded Zombies framework. The basic
        # wooden barrier is defined in prison, not in Nuketown's magic box.
        selected = {'zm_nuked', 'common_zm', 'code_pre_gfx_zm', 'code_post_gfx_zm',
                    'patch_zm', 'zm_prison'}
        zones = (p for p in zones if p.stem in selected and p.parent.name == 'all')
    return sorted(zones, key=lambda p: _rank(p.relative_to(game).as_posix()))


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
            'zones': {p.relative_to(game).as_posix(): stamp(p) for p in stock_zones(game, engine)}}


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
    zones = stock_zones(game, engine)
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
                assets=stock_assets(engine, ff),
                image_format='DDS' if engine == 'T4' else 'IWI', tool_digest=tool_hash,
                dependencies=archives, refresh=refresh)
            if engine == 'T6' and ff.stem == 'zm_prison':
                data = trim_barrier_payload(folder, data)
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
        prune_stock_cache(root, entries, raw)
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
