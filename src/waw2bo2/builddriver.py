"""Desktop build pipeline using shared Extract All caches."""
from __future__ import annotations

from pathlib import Path
from contextlib import ExitStack
import shutil
import struct
import subprocess

from . import all2raw, progress
from .launcher import BuildPaths, Settings, cli_command
from .resources import resource_root


def run(command: list[str], log: Path | None = None) -> None:
    if log is None:
        result = subprocess.run(command, check=False)
    else:
        log.parent.mkdir(parents=True, exist_ok=True)
        result = progress.native(command, log)
    if result.returncode:
        if log is not None:
            print(log.read_text(encoding='utf-8', errors='replace')[-8000:], flush=True)
        raise RuntimeError(f'{Path(command[0]).name} failed ({result.returncode})' +
                           (f'; see {log}' if log else ''))


def ensure_laa(linker: Path) -> None:
    data = bytearray(linker.read_bytes())
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError(f'Not a PE executable: {linker}')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'Invalid PE header: {linker}')
    flags = struct.unpack_from('<H', data, pe + 22)[0]
    if not flags & 0x20:
        struct.pack_into('<H', data, pe + 22, flags | 0x20)
        linker.write_bytes(data)


def build(settings: Settings, *, redump: bool = False, root: Path | None = None,
          cache_paths: all2raw.CachePaths | None = None) -> int:
    caches = cache_paths or all2raw.CachePaths.for_settings(settings)
    # Extraction may prune old generations. Protect the stock view while a
    # build reads it; dependency extraction locks its own separate folders.
    with ExitStack() as locks:
        for cache in sorted({caches.waw, caches.bo2}, key=str):
            locks.enter_context(all2raw.cache_lock(cache))
        return _build(settings, redump=redump, root=root, cache_paths=caches)


def _build(settings: Settings, *, redump: bool = False, root: Path | None = None,
           cache_paths: all2raw.CachePaths | None = None) -> int:
    import os
    os.environ['WAW2BO2_VERBOSE'] = '1' if settings.verbose else '0'
    paths = BuildPaths.for_settings(settings)
    if root is not None:
        root = root.resolve()
        paths = BuildPaths(root, root / 'stage', paths.stock, root / 'waw_dumps',
                           root / 'mod_build', root / 'package')
    caches = cache_paths or all2raw.CachePaths.for_settings(settings)
    waw, bo2 = Path(settings.waw), Path(settings.bo2)
    t4 = Path(settings.t4) / 'Unlinker.exe'
    t6 = Path(settings.t6) / 'Unlinker.exe'
    # Builds never extract or list a stock FF. Extract All owns every static
    # asset, index, sound driver and API cache.
    wawraw = all2raw.ready(waw, t4, caches.waw, engine='T4')
    stock = all2raw.ready(bo2, t6, caches.bo2, engine='T6')
    stage, mod = paths.stage, paths.mod
    for folder in (stage, mod):
        folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(stock / 't6api_cache.json', stage / 't6api_cache.json')
    print('== 0. reusing shared game assets and preparing cached map sources', flush=True)
    sources = all2raw.source_dumps(settings, caches, refresh=redump)
    source = Path(settings.fastfile).resolve()
    mapzone, project = source.stem, settings.project
    mapraw = sources['map']
    roots = [str(p) for p in sources.values()] + [str(wawraw)]
    extra = [arg for path in roots for arg in ('--extra-root', path)]
    linker = Path(settings.t6) / 'Linker.exe'
    print('== 1. using pre-extracted stock assets (no stock dumping)', flush=True)
    print(f'== 2. staging {project}', flush=True)
    args = ['stage-bridge', str(stage), project,
            '--gfx', str(mapraw / 'waw2bo2/maps' / (mapzone + '.d3dbsp.gfx.bin')),
            '--clip', str(mapraw / 'waw2bo2/maps' / (mapzone + '.d3dbsp.clip.bin')), *extra,
            '--iwd-dir', str(source.parent), '--iwd-dir', str(waw / 'main'),
            '--waw-map-script', str(mapraw / 'maps' / (mapzone + '.gsc')),
            '--waw-script-root', str(sources.get('mod', mapraw)), '--waw-stock-scripts', str(wawraw),
            '--t6-unlinker', str(t6), '--waw-root', str(waw), '--t4-unlinker', str(t4),
            '--waw-stock-dumps', str(caches.waw), '--xwma-decoder', settings.decoder,
            '--approximate-sound-curves', '--stock-materials', str(stock / 'materials'),
            '--techset-dump', str(stock), '--bo2', str(bo2),
            '--script-template', str(stage / 'zone_raw/bridge'), '--script-template-name', 'bridge']
    if settings.source_fx:
        args += ['--waw-mod-tools', settings.waw_tools, '--waw-source-fx',
                 '--waw-source-dumps', str(caches.waw / 'source')]
    run(cli_command(*args))
    ensure_laa(linker)
    print('== 3a. linking gameplay mod.ff with the BO2 mod tools linker', flush=True)
    run(cli_command('build-mod', str(stage), project, '--bo2', str(bo2), '--work', str(mod),
                    '--unlinker', str(t6), '--linker', str(linker), '--techset-dump', str(stock)))
    print('== 3b. compiling map scripts with the BO2 mod tools linker', flush=True)
    run(cli_command('compile-scripts', str(stage), project, '--bo2', str(bo2), '--unlinker', str(t6)))
    print(f'== 4. linking {project} with the T6 bridge', flush=True)
    run(cli_command('bridge-link', str(stage), project, '--linker', str(linker),
                    '--techset-dump', str(stock), '--log', str(stage / (project + '_bridge.log'))))
    print('== 6. auditing built material arguments', flush=True)
    for ff in (stage / 'zone_out' / project / (project + '.ff'), mod / 'out/mod.ff'):
        if not ff.is_file():
            raise FileNotFoundError(f'Linker did not create {ff}')
        audit = stage / ('audit_' + ff.stem)
        if not audit.resolve().is_relative_to(stage.resolve()):
            raise ValueError(f'Audit cleanup target is outside staging: {audit}')
        if audit.exists():
            shutil.rmtree(audit)
        run([str(t6), '--no-color', '--search-path', str(bo2 / 'zone/all'), '--include-assets',
             'material,techniqueset', '--output-folder', str(audit), str(ff)], stage / (audit.name + '.log'))
        from .launcher import python_command
        run(python_command() + [str(resource_root() / 'tools/audit_material_args.py'), str(audit), str(stock)])
    return 0
