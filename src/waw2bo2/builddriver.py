"""Desktop conversion pipeline, with no dependency on PowerShell.

Runs inside the bundled Windows Python runtime, including when launched by Wine.
Native conversion and auditing use the same commands as tools/run_bridge.ps1.
"""
from __future__ import annotations

from pathlib import Path
import shutil
import struct
import subprocess

from .launcher import BuildPaths, Settings, cli_command
from .resources import resource_root


def run(command: list[str], log: Path | None = None) -> None:
    if log is None:
        result = subprocess.run(command, check=False)
    else:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w', encoding='utf-8') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
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


def build(settings: Settings, *, redump: bool = False, root: Path | None = None) -> int:
    paths = BuildPaths.for_settings(settings)
    if root is not None:
        root = root.resolve()
        paths = BuildPaths(root, root / 'stage', root / 'stock_t6', root / 'waw_dumps',
                           root / 'mod_build', root / 'package')
    stage, dumps, stock, mod = paths.stage, paths.waw_dumps, paths.stock, paths.mod
    for folder in (stage, dumps, stock, mod):
        folder.mkdir(parents=True, exist_ok=True)
    waw, bo2 = Path(settings.waw), Path(settings.bo2)
    source = Path(settings.fastfile).resolve()
    mapmod, mapzone, project = source.parent, source.stem, settings.project
    t4 = Path(settings.t4) / 'Unlinker.exe'
    t6 = Path(settings.t6) / 'Unlinker.exe'
    linker = Path(settings.t6) / 'Linker.exe'
    search = str(mapmod) + ';' + str(waw / 'main')

    def announce(text: str) -> None:
        print(text, flush=True)

    def dump(tool: Path, ff: Path, output: Path, assets: str, log: Path,
             search_path: str = search, options: tuple[str, ...] = ()) -> None:
        run([str(tool), '--no-color', '--search-path', search_path, *options,
             '--include-assets', assets, '--output-folder', str(output), str(ff)], log)

    def cached(marker: Path, action) -> None:
        if redump or not marker.exists():
            # Never keep an old success marker after a failed refresh.
            marker.unlink(missing_ok=True)
            action()
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()

    announce('== 0. dumping WaW clipmap and companion zones')
    dump(t4, source, stage,
         'clipmap,gfxworld,gameworldsp,comworld,lightdef,fx,weapon,xanim,sound,loadedsound,rawfile,physpreset,snddriverglobals,xmodel',
         stage / 'clip_dump.log', options=('--model-format', 'GLTF'))
    dump(t4, source, dumps / 'map_rawfiles', 'rawfile', stage / 'rawfile_dump.log')
    modraw = dumps / 'mod_rawfiles'
    if redump or not (modraw / 'maps').exists():
        dump(t4, mapmod / 'mod.ff', modraw, 'rawfile', dumps / 'mod_rawfiles.log')

    scripts = dumps / 'stock_scripts'
    def stock_scripts():
        for name in ('common', 'nazi_zombie_prototype', 'nazi_zombie_asylum',
                     'nazi_zombie_sumpf', 'nazi_zombie_factory'):
            ff = waw / 'zone/english' / (name + '.ff')
            if ff.exists():
                dump(t4, ff, scripts, 'rawfile', dumps / ('stock_scripts_' + name + '.log'),
                     str(waw / 'main'))
    cached(scripts / '.complete', stock_scripts)

    roots: list[str] = []
    lighting = dumps / 'lighting_code'
    cached(lighting / '.lighting_assets_v1', lambda: dump(
        t4, waw / 'zone/english/code_post_gfx.ff', lighting, 'image,lightdef', dumps / 'lighting_code.log'))
    roots += ['--extra-root', str(lighting)]
    materials = dumps / 'map_materials'
    cached(materials / '.dump_v9_lightdefs', lambda: dump(
        t4, source, materials, 'material', dumps / 'map_materials.log'))
    roots += ['--extra-root', str(materials)]
    companions = [(mapzone + '_patch', mapmod / (mapzone + '_patch.ff')),
                  ('mod', mapmod / 'mod.ff'), ('common', waw / 'zone/english/common.ff'),
                  ('code_post_gfx', waw / 'zone/english/code_post_gfx.ff')]
    for name, ff in companions:
        if not ff.exists():
            continue
        output = dumps / name
        cached(output / '.dump_v9_lightdefs', lambda: dump(t4, ff, output,
               'material,image,xmodel,fx,weapon,xanim,sound,loadedsound,rawfile,comworld,lightdef,physpreset,snddriverglobals',
               dumps / (name + '.log'), options=('--image-format', 'DDS', '--model-format', 'GLTF')))
        roots += ['--extra-root', str(output)]

    zone = bo2 / 'zone/all'
    stockff = zone / 'zm_nuked.ff'
    if not stockff.is_file():
        raise FileNotFoundError(f'Stock zone not found: {stockff}')
    def stock_materials():
        for name in ('zm_nuked', 'common_zm', 'code_post_gfx_zm'):
            announce(f'== 1. dumping materials + technique sets from {name}.ff')
            dump(t6, zone / (name + '.ff'), stock, 'material,techniqueset',
                 paths.root / ('stock_t6_dump_' + name + '.log'), str(zone))
    cached(stock / 'techniquesets/.complete', stock_materials)
    announce('== 1b. preparing stock BO2 barrier and material images')
    cached(stock / 'zbarrier/.barrier_complete', lambda: dump(
        t6, zone / 'zm_prison.ff', stock, 'zbarrier,xmodel,material,image,techniqueset',
        paths.root / 'stock_t6_barrier.log', str(zone), ('--image-format', 'IWI')))
    cached(stock / '.material_images_v1', lambda: dump(
        t6, stockff, stock, 'image', paths.root / 'stock_t6_material_images.log',
        str(zone), ('--image-format', 'IWI')))
    if redump or not (stock / 'sounddriverglobals/singleton.w2bsdg').exists():
        announce('== 1c. dumping stock BO2 sound driver')
        dump(t6, zone / 'code_post_gfx_zm.ff', stock, 'snddriverglobals',
             paths.root / 'stock_t6_sound_driver.log', str(zone))

    announce(f'== 2. staging {project}')
    args = ['stage-bridge', str(stage), project,
            '--gfx', str(stage / 'waw2bo2/maps' / (mapzone + '.d3dbsp.gfx.bin')),
            '--clip', str(stage / 'waw2bo2/maps' / (mapzone + '.d3dbsp.clip.bin')), *roots,
            '--iwd-dir', str(mapmod), '--iwd-dir', str(waw / 'main'),
            '--waw-map-script', str(dumps / 'map_rawfiles/maps' / (mapzone + '.gsc')),
            '--waw-script-root', str(modraw), '--waw-stock-scripts', str(scripts),
            '--t6-unlinker', str(t6), '--waw-root', str(waw), '--t4-unlinker', str(t4),
            '--waw-stock-dumps', str(dumps / 'stock'), '--xwma-decoder', settings.decoder,
            '--approximate-sound-curves', '--stock-materials', str(stock / 'materials'),
            '--techset-dump', str(stock), '--bo2', str(bo2),
            '--script-template', str(stage / 'zone_raw/bridge'), '--script-template-name', 'bridge']
    if settings.source_fx:
        args += ['--waw-mod-tools', settings.waw_tools, '--waw-source-fx',
                 '--waw-source-dumps', str(dumps / 'source')]
    run(cli_command(*args))
    ensure_laa(linker)
    announce('== 3a. linking gameplay mod.ff with the BO2 mod tools linker')
    run(cli_command('build-mod', str(stage), project, '--bo2', str(bo2), '--work', str(mod),
                    '--unlinker', str(t6), '--linker', str(linker), '--techset-dump', str(stock)))
    announce('== 3b. compiling map scripts with the BO2 mod tools linker')
    run(cli_command('compile-scripts', str(stage), project, '--bo2', str(bo2), '--unlinker', str(t6)))
    announce(f'== 4. linking {project} with the T6 bridge')
    run(cli_command('bridge-link', str(stage), project, '--linker', str(linker),
                    '--techset-dump', str(stock), '--log', str(stage / (project + '_bridge.log'))))
    announce('== 6. auditing built material arguments')
    for ff in (stage / 'zone_out' / project / (project + '.ff'), mod / 'out/mod.ff'):
        if not ff.is_file():
            raise FileNotFoundError(f'Linker did not create {ff}')
        audit = stage / ('audit_' + ff.stem)
        # Audit roots are fixed children of stage; reject symlink escapes.
        if not audit.resolve().is_relative_to(stage.resolve()):
            raise ValueError(f'Audit cleanup target is outside staging: {audit}')
        if audit.exists():
            shutil.rmtree(audit)
        dump(t6, ff, audit, 'material,techniqueset', stage / (audit.name + '.log'), str(zone))
        from .launcher import python_command
        run(python_command() + [str(resource_root() / 'tools/audit_material_args.py'), str(audit), str(stock)])
    return 0
