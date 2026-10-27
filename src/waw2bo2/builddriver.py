"""Desktop build pipeline using shared Extract All caches."""
from __future__ import annotations

from pathlib import Path
from contextlib import ExitStack
import hashlib
import json
import shutil
import struct
import subprocess
import time

from . import __version__, all2raw, knowledge, modelhitboxes, progress, timeline
from .launcher import BuildPaths, Settings, cli_command
from .resources import resource_root


# the build step running now, for the knowledge ledger's failed_step
_current_step: list[str] = []


def run(command: list[str], log: Path | None = None) -> None:
    if '-m' in command and command.index('-m') + 2 < len(command):
        _current_step[:] = [command[command.index('-m') + 2]]
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


def world_name(dump: Path, stem: str) -> str:
    """The map's world name in its dumped zone. Usually the fastfile's name;
    a map shipped only as mod.ff (or renamed) carries its world under the
    map's real name (maps/<name>.d3dbsp)."""
    maps = dump / 'waw2bo2/maps'
    if (maps / f'{stem}.d3dbsp.gfx.bin').is_file():
        return stem
    worlds = sorted(p.name.removesuffix('.d3dbsp.gfx.bin') for p in maps.glob('*.d3dbsp.gfx.bin'))
    if len(worlds) == 1:
        print(f'== map world "{worlds[0]}" found in {stem}.ff', flush=True)
        return worlds[0]
    if worlds:
        raise RuntimeError(f'{stem}.ff holds several map worlds ({", ".join(worlds)}); choose that map\'s own fastfile')
    return stem     # no world dumped: staging reports the missing world files


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


# Staged text files a user may hand-edit to get past a failing step
# (translated scripts, string tables, animtrees, entities/zone sources).
EDITABLE_SUFFIXES = ('.gsc', '.csc', '.csv', '.atr', '.json', '.str', '.txt', '.cfg', '.menu')
# rewritten by stage_art on every build, before staging
REGENERATED_EACH_BUILD = {'menu.json'}


def converter_digest() -> str:
    """Identity of the converter code: a staged tree from another converter
    version is never resumed."""
    h = hashlib.sha256(__version__.encode())
    package = Path(__file__).resolve().parent
    for path in sorted(package.rglob('*')):
        if path.suffix in ('.py', '.json') and path.is_file() and '__pycache__' not in path.parts:
            h.update(path.relative_to(package).as_posix().encode() + b'\0' + path.read_bytes())
    return h.hexdigest()


def stage_fingerprint(args: list[str]) -> str:
    # the stage-bridge command line names every input: map dumps (their
    # folders are keyed by the fastfile stamps), game roots and flags
    return hashlib.sha256(json.dumps([converter_digest(), args]).encode()).hexdigest()


def editable_files(project_root: Path) -> dict[str, list[int]]:
    if not project_root.is_dir():
        return {}
    return {p.relative_to(project_root).as_posix(): [p.stat().st_size, p.stat().st_mtime_ns]
            for p in sorted(project_root.rglob('*'))
            if p.suffix.lower() in EDITABLE_SUFFIXES and p.name not in REGENERATED_EACH_BUILD and p.is_file()}


def stage_receipt(stage: Path) -> Path:
    return stage / 'stage_receipt.json'


def resumable_stage(stage: Path, project: str, fingerprint: str) -> tuple[bool, str]:
    """Whether the last completed staging can be reused as it is (including
    the user's edits to it). Returns (reusable, reason)."""
    try:
        receipt = json.loads(stage_receipt(stage).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False, 'the last build did not finish staging'
    if receipt.get('project') != project:
        return False, 'the last staging was for another map name'
    if receipt.get('fingerprint') != fingerprint:
        return False, 'the map sources, build options or converter version changed since the last staging'
    if not (stage / 'zone_raw' / project).is_dir():
        return False, 'the staged files are missing'
    return True, ''


def edited_since_staging(stage: Path, project: str) -> tuple[list[str], list[str]]:
    """Staged text files changed or removed since staging finished."""
    try:
        recorded = json.loads(stage_receipt(stage).read_text(encoding='utf-8')).get('files', {})
    except (OSError, ValueError):
        return [], []
    current = editable_files(stage / 'zone_raw' / project)
    changed = sorted(name for name, stat in current.items() if recorded.get(name) not in (None, stat))
    changed += sorted(name for name in current if name not in recorded)
    removed = sorted(name for name in recorded if name not in current)
    return changed, removed


def build(settings: Settings, *, redump: bool = False, root: Path | None = None,
          cache_paths: all2raw.CachePaths | None = None) -> int:
    stage = (root.resolve() / 'stage') if root is not None else BuildPaths.for_settings(settings).stage
    # an outer caller (a benchmark) may already own the timeline
    owned = timeline.path() is None
    if owned:
        timeline.start(stage / 'build_timeline.jsonl')
    try:
        with timeline.span('build', project=settings.project):
            return _locked_build(settings, redump=redump, root=root, cache_paths=cache_paths)
    finally:
        if owned:
            write_timeline_summary(timeline.path())


def write_timeline_summary(source: Path) -> None:
    try:
        timeline.merge(source)
        summary = timeline.summarize(timeline.load(source))
        source.with_suffix('.json').write_text(json.dumps(summary, indent=1), encoding='utf-8')
        print(f'== build timeline: {summary.get("wall", 0):.1f}s total; {source.with_suffix(".json")}', flush=True)
    except Exception as exc:  # noqa: BLE001 - timing never fails a build
        print(f'build timeline: not summarized ({type(exc).__name__}: {exc})', flush=True)


def _locked_build(settings: Settings, *, redump: bool, root: Path | None,
                  cache_paths: all2raw.CachePaths | None) -> int:
    caches = cache_paths or all2raw.CachePaths.for_settings(settings)
    # Prepare only missing/stale caches before acquiring the read locks.
    # prepare() owns the extraction lock itself and resumes cached zones.
    for game, tool, cache, engine in (
            (Path(settings.waw), Path(settings.t4) / 'Unlinker.exe', caches.waw, 'T4'),
            (Path(settings.bo2), Path(settings.t6) / 'Unlinker.exe', caches.bo2, 'T6')):
        try:
            all2raw.ready(game, tool, cache, engine=engine)
        except RuntimeError:
            print(f'== 1. preparing {engine} shared game assets automatically', flush=True)
            all2raw.prepare(game, tool, cache, engine=engine)
    # Extraction may prune old generations. Protect the stock view while a
    # build reads it; dependency extraction locks its own separate folders.
    with ExitStack() as locks:
        for cache in sorted({caches.waw, caches.bo2}, key=str):
            locks.enter_context(all2raw.cache_lock(cache))
        _current_step.clear()
        started = time.time()
        try:
            result = _build(settings, redump=redump, root=root, cache_paths=caches)
        except BaseException as exc:
            remember(settings, root, 'failed', f'{type(exc).__name__}: {exc}', since=started)
            raise
        remember(settings, root, 'built', since=started)
        return result


def remember(settings: Settings, root: Path | None, outcome: str, error: str = '', *, since: float = 0) -> None:
    """Record this map's observation in the knowledge ledger (knowledge.py).
    Evidence only: a ledger problem never changes the build's result."""
    try:
        stage = (root.resolve() / 'stage') if root is not None else BuildPaths.for_settings(settings).stage
        report_file = stage / 'zone_raw' / settings.project / knowledge.REPORT_NAME
        # a report older than this build belongs to an earlier staging run
        # (kept only when the build resumed it)
        fresh = report_file.is_file() and (settings.resume or report_file.stat().st_mtime >= since)
        report = json.loads(report_file.read_text(encoding='utf-8')) if fresh else None
        obs = knowledge.observation(report, source=settings.fastfile, project=settings.project,
                                    version=__version__, outcome=outcome,
                                    step=_current_step[0] if outcome == 'failed' and _current_step else None,
                                    error=error)
        path = knowledge.record(Path(settings.work) / 'knowledge', obs)
        gaps = sum(len(symbols) for symbols in obs['gaps'].values())
        print(f'knowledge: {outcome}, {gaps} untranslated features recorded in {path}', flush=True)
    except Exception as exc:  # noqa: BLE001 - evidence must not fail the build
        print(f'knowledge: not recorded ({type(exc).__name__}: {exc})', flush=True)


def mod_frameworks(fastfile: Path) -> list[str]:
    """Mod frameworks a map ships beside its fastfile (ZCT's zct_*.iwd, UGX's
    ugx*.iwd). Their scripts are the map's scripts and port like any other;
    this only names them in the build log."""
    folder = Path(fastfile).parent
    if not folder.is_dir():
        return []
    found = set()
    for path in folder.iterdir():
        name = path.name.lower()
        if path.suffix.lower() == '.iwd':
            if name.startswith('zct'):
                found.add('ZCT')
            elif name.startswith('ugx'):
                found.add('UGX')
    return sorted(found)


def _build(settings: Settings, *, redump: bool = False, root: Path | None = None,
           cache_paths: all2raw.CachePaths | None = None) -> int:
    import os
    frameworks = mod_frameworks(Path(settings.fastfile)) if settings.fastfile else []
    if frameworks:
        print(f"map framework: {', '.join(frameworks)} (its scripts drive the map, like any map's)", flush=True)
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
    # Stage in the worker as well: standalone CLI and Wine builds do not
    # necessarily share the launcher's filesystem or its pre-staged metadata.
    from .menuart import stage_art
    stage_art(settings, stage / 'zone_raw' / settings.project)
    for folder in (stage, mod):
        folder.mkdir(parents=True, exist_ok=True)
    shutil.copy2(stock / 't6api_cache.json', stage / 't6api_cache.json')
    print('== 0. reusing shared game assets and preparing cached map sources', flush=True)
    with timeline.span('0 map source dumps'):
        sources = all2raw.source_dumps(settings, caches, refresh=redump)
    source = Path(settings.fastfile).resolve()
    project = settings.project
    mapraw = sources['map']
    mapzone = world_name(mapraw, source.stem)
    roots = [str(p) for p in sources.values()] + [str(wawraw)]
    extra = [arg for path in roots for arg in ('--extra-root', path)]
    extra += [arg for path in sources.values() for arg in ('--map-root', str(path))]
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
    if settings.bo2_stock_perks:
        args += ['--bo2-stock-perks']
    if settings.remaster:
        args += ['--remaster-bo2-materials']
    if 'patch' in sources:
        # WaW loads <map>_patch.ff after the map zone: its scripts win
        args += ['--waw-patch-script-root', str(sources['patch'])]
    if source.parent.is_relative_to((waw / 'zone').resolve()):
        # a retail WaW level: stock_map_policies apply (never to custom maps)
        args += ['--waw-stock-map']
    if settings.source_fx:
        args += ['--waw-mod-tools', settings.waw_tools, '--waw-source-fx',
                 '--waw-source-dumps', str(caches.waw / 'source')]
    fingerprint = stage_fingerprint(args)
    reuse, reason = resumable_stage(stage, project, fingerprint) if settings.resume else (False, '')
    if reuse:
        changed, removed = edited_since_staging(stage, project)
        print(f'== 2. resuming: keeping the staged files of the last build ({len(changed)} edited, '
              f'{len(removed)} removed since staging)', flush=True)
        for name in (changed + removed)[:20]:
            print(f'   kept edit: zone_raw/{project}/{name}', flush=True)
    else:
        if settings.resume:
            print(f'== 2. cannot resume: {reason}; staging again from the WaW sources', flush=True)
        # staging rewrites zone_raw: a half-finished run must never be resumed
        stage_receipt(stage).unlink(missing_ok=True)
        with timeline.span('2 stage-bridge'):
            run(cli_command(*args))
        stage_receipt(stage).write_text(json.dumps({
            'project': project, 'fingerprint': fingerprint,
            'files': editable_files(stage / 'zone_raw' / project)}), encoding='utf-8')
    ensure_laa(linker)
    print('== 3a. linking gameplay mod.ff with the BO2 mod tools linker', flush=True)
    with timeline.span('3a build-mod'):
        run(cli_command('build-mod', str(stage), project, '--bo2', str(bo2), '--work', str(mod),
                        '--unlinker', str(t6), '--linker', str(linker), '--techset-dump', str(stock)))
    print('== 3b. compiling map scripts with the BO2 mod tools linker', flush=True)
    with timeline.span('3b compile-scripts'):
        run(cli_command('compile-scripts', str(stage), project, '--bo2', str(bo2), '--unlinker', str(t6)))
    print(f'== 4. linking {project} with the T6 bridge', flush=True)
    with timeline.span('4 bridge-link'):
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
        model_source = stage / 'zone_raw' / project
        audit_hitboxes = ff.stem == project and modelhitboxes.has_authored_hitboxes(model_source)
        with timeline.span(f'6 audit unlink {ff.name}'):
            run([str(t6), '--no-color', '--search-path', str(bo2 / 'zone/all'), '--include-assets',
                 'material,techniqueset' + (',xmodel' if audit_hitboxes else ''),
                 '--output-folder', str(audit), str(ff)], stage / (audit.name + '.log'))
        from .launcher import python_command
        with timeline.span(f'6 audit material args {ff.name}'):
            run(python_command() + [str(resource_root() / 'tools/audit_material_args.py'), str(audit), str(stock)])
        if audit_hitboxes:
            with timeline.span('6 audit bone hitboxes'):
                result = modelhitboxes.audit(model_source, audit)
            (stage / 'bone_hitbox_audit.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
            if result['errors'] or not result['models']:
                raise RuntimeError('Authored bone hitboxes changed during linking; see bone_hitbox_audit.json')
            print(f'{result["bones"]} authored bone hitboxes checked across {result["models"]} models, 0 violations', flush=True)
    return 0
