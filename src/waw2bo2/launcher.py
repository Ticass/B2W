"""Desktop launcher services; independent of Tk so build behavior is testable."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
from dataclasses import asdict, dataclass, field

from .resources import resource_root


def user_directory() -> Path:
    return Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'WawConverter'


def python_command() -> list[str]:
    if getattr(sys, 'frozen', False):
        return [str(Path(sys.executable).with_name('WawConverter.CLI.exe'))]
    executable = Path(sys.executable)
    if executable.name.lower() == 'pythonw.exe':
        executable = executable.with_name('python.exe')
    return [str(executable)]


@dataclass
class Settings:
    waw: str = ''
    bo2: str = ''
    waw_tools: str = ''
    t4: str = ''
    t6: str = ''
    decoder: str = ''
    work: str = field(default_factory=lambda: str(user_directory() / 'builds'))
    fastfile: str = ''
    project: str = ''
    menu_title: str = ''
    menu_description: str = ''
    menu_blit: str = ''
    menu_large: str = ''
    menu_blur: str = ''
    source_fx: bool = False
    redump: bool = False

    @classmethod
    def load(cls, path: Path | None = None) -> Settings:
        try:
            data = json.loads((path or user_directory() / 'settings.json').read_text(encoding='utf-8'))
            if not isinstance(data, dict):
                return cls()
            allowed = cls.__dataclass_fields__
            return cls(**{k: v for k, v in data.items() if k in allowed and
                          (isinstance(v, bool) if k in ('source_fx', 'redump') else isinstance(v, str))})
        except (OSError, ValueError, TypeError):
            return cls()

    def save(self, path: Path | None = None) -> None:
        path = path or user_directory() / 'settings.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_suffix('.tmp')
        data = asdict(self)
        # Re-detect bundled tools after the portable folder is moved or upgraded.
        for key, relative in [('t4', 'vendor/OpenAssetTools/build/bin/Release_x86'),
                              ('t6', 'vendor/OpenAssetToolsT6/build/bin/Release_x86'),
                              ('decoder', 'tools/bin/xaudio_wma_decoder.exe')]:
            if data[key] and Path(data[key]).resolve() == (resource_root() / relative).resolve():
                data[key] = ''
        temp.write_text(json.dumps(data, indent=2), encoding='utf-8')
        temp.replace(path)


def discover(settings: Settings) -> Settings:
    """Fill only unset paths, using Steam libraries and bundled native tools."""
    root = resource_root()
    steam_paths = [Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Steam']
    if os.name == 'nt':
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
                steam_paths.insert(0, Path(winreg.QueryValueEx(key, 'SteamPath')[0]))
        except OSError:
            pass
    libraries = list(steam_paths)
    for steam in steam_paths:
        try:
            text = (steam / 'steamapps/libraryfolders.vdf').read_text(encoding='utf-8')
            libraries.extend(Path(value.replace('\\\\', '\\')) for value in
                             re.findall(r'"path"\s+"([^"]+)"', text))
        except OSError:
            pass
    for field_name, folder in [('waw', 'Call of Duty World at War'), ('bo2', 'Call of Duty Black Ops II')]:
        if not getattr(settings, field_name):
            found = next((p / 'steamapps/common' / folder for p in libraries
                          if (p / 'steamapps/common' / folder).is_dir()), None)
            if found:
                setattr(settings, field_name, str(found))
    for field_name, relative in [('t4', 'vendor/OpenAssetTools/build/bin/Release_x86'),
                                  ('t6', 'vendor/OpenAssetToolsT6/build/bin/Release_x86'),
                                  ('decoder', 'tools/bin/xaudio_wma_decoder.exe')]:
        if not getattr(settings, field_name):
            setattr(settings, field_name, str(root / relative))
    if not settings.waw_tools:
        candidates = [root / 'wawModTools'] + ([Path(settings.waw)] if settings.waw else [])
        found = next((p for p in candidates if (p / 'bin/linker_pc.exe').is_file()), None)
        if found:
            settings.waw_tools = str(found)
    return settings


def map_fastfiles(folder: Path) -> list[Path]:
    excluded = {'mod', 'mod_load', 'common', 'code_post_gfx'}
    return sorted((p for p in folder.glob('*.ff') if p.stem.lower() not in excluded
                   and not p.stem.lower().endswith('_patch')), key=lambda p: p.name.lower())


def project_name(source: str) -> str:
    name = re.sub(r'^(?:nazi_zombie_|zm_)', '', Path(source).stem, flags=re.I)
    name = re.sub(r'[^a-z0-9_]+', '_', name.lower()).strip('_') or 'map'
    return 'zm_' + name[:45] + '_waw'


def validate_project(name: str) -> None:
    if not re.fullmatch(r'zm_[a-z0-9_]{1,58}', name):
        raise ValueError('Map name must start with zm_ and use lowercase letters, numbers, and underscores (up to 61 characters).')


@dataclass(frozen=True)
class Check:
    name: str
    ready: bool
    detail: str
    required: bool = True


def preflight(settings: Settings, *, include_map: bool = True) -> list[Check]:
    checks: list[Check] = []

    def files(name: str, folder: str, paths: list[str], *, required: bool = True) -> None:
        missing = paths if not folder else [p for p in paths if not (Path(folder) / p).exists()]
        checks.append(Check(name, not missing, 'Ready' if not missing else 'Missing: ' + ', '.join(missing), required))

    files('World at War', settings.waw, ['main', 'zone/english/common.ff', 'zone/english/code_post_gfx.ff'])
    files('Black Ops II + Mod Tools', settings.bo2, ['bin/Linker.exe', 'raw/animtrees/fxanim_props.atr',
          'raw/zm/mapstable.csv', 'raw/zm/gametypestable.csv', 'raw/maps/mp/zombies/_zm.gsc',
          'mods/zm_test/maps/mp/zm_test.gsc', 'mods/zm_test/clientscripts/mp/zm_test.csc',
          'zone/all/zm_nuked.ff', 'zone/all/zm_prison.ff', 'zone/all/common_zm.ff', 'zone/all/code_post_gfx_zm.ff'])
    files('WaW extractor', settings.t4, ['Unlinker.exe'])
    files('BO2 bridge tools', settings.t6, ['Unlinker.exe', 'Linker.exe'])
    decoder_ok = bool(settings.decoder) and Path(settings.decoder).is_file()
    checks.append(Check('Audio decoder', decoder_ok, 'Ready' if decoder_ok else 'Select xaudio_wma_decoder.exe in Advanced paths.'))
    files('WaW source tools', settings.waw_tools, ['bin/linker_pc.exe'], required=settings.source_fx)
    if include_map:
        source = Path(settings.fastfile)
        valid_source = bool(settings.fastfile) and source.is_file() and source.suffix.lower() == '.ff'
        checks.append(Check('Source map', valid_source, source.name if valid_source else 'Choose the source map fastfile (.ff).'))
        if valid_source:
            files('Companion mod.ff', str(source.parent), ['mod.ff'])
            valid_stem = bool(re.fullmatch(r'[A-Za-z0-9_\-]+', source.stem))
            checks.append(Check('Source zone name', valid_stem, 'Ready' if valid_stem else 'Source fastfile name must use letters, numbers, underscores, or hyphens.'))
        try:
            validate_project(settings.project)
            checks.append(Check('BO2 map name', True, settings.project))
        except ValueError as error:
            checks.append(Check('BO2 map name', False, str(error)))
        from .menuart import validate_art
        try:
            validate_art(settings)
            checks.append(Check('Map artwork', True, 'Ready'))
        except (OSError, ValueError) as error:
            checks.append(Check('Map artwork', False, str(error)))
    checks.append(Check('Build folder', bool(settings.work.strip()), settings.work or 'Choose a writable build folder.'))
    return checks


@dataclass(frozen=True)
class BuildPaths:
    root: Path
    stage: Path
    stock: Path
    waw_dumps: Path
    mod: Path
    output: Path

    @classmethod
    def for_settings(cls, settings: Settings) -> BuildPaths:
        validate_project(settings.project)
        identity = str(Path(settings.fastfile).resolve()).casefold()
        digest = hashlib.sha256(identity.encode()).hexdigest()[:8]
        # Cache native dumps separately for each map and game installation pair.
        game_key = hashlib.sha256((settings.waw.casefold() + '\0' + settings.bo2.casefold()).encode()).hexdigest()[:8]
        root = Path(settings.work).resolve() / f'{settings.project}_{digest}_{game_key}'
        return cls(root, root / 'stage', root / 'stock_t6', root / 'waw_dumps', root / 'mod_build', root / 'package')

    def complete(self, project: str) -> bool:
        expected = [self.output / name for name in (project + '.ff', project + '.ipak', 'mod.ff', 'mod_load.ff', 'mod.json')]
        try:
            receipt = json.loads((self.root / 'build.json').read_text(encoding='utf-8'))
            return receipt.get('project') == project and all(p.is_file() for p in expected)
        except (OSError, ValueError):
            return False


def build_command(settings: Settings) -> list[str]:
    paths = BuildPaths.for_settings(settings)
    root = resource_root()
    source = Path(settings.fastfile).resolve()
    command = ['powershell.exe', '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
               '-File', str(root / 'tools/run_bridge.ps1'), '-Bo2', settings.bo2, '-Waw', settings.waw,
               '-MapMod', str(source.parent), '-MapZone', source.stem, '-Project', settings.project,
               '-Stage', str(paths.stage), '-Dump', str(paths.stock), '-WawDumps', str(paths.waw_dumps),
               '-ModBuild', str(paths.mod), '-OatT4', settings.t4, '-OatT6', settings.t6,
               '-XwmaDecoder', settings.decoder, '-PythonExe', python_command()[0], '-NoInstall']
    if settings.waw_tools:
        command += ['-WawModTools', settings.waw_tools]
    if not settings.source_fx:
        command.append('-NoWawSourceFx')
    try:
        cache = json.loads((paths.root / 'cache.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        cache = None
    if settings.redump or cache != input_stamp(settings):
        command.append('-Redump')
    return command


def input_stamp(settings: Settings) -> dict:
    """Invalidate native caches when source archives, game data, or tools change."""
    paths = list(Path(settings.fastfile).parent.glob('*.ff')) + list(Path(settings.fastfile).parent.glob('*.iwd'))
    paths += list((Path(settings.waw) / 'main').glob('*.iwd'))
    paths += list((Path(settings.waw) / 'zone/english').glob('*.ff'))
    paths += list((Path(settings.bo2) / 'zone/all').glob('*.ff'))
    paths += [Path(settings.t4) / 'Unlinker.exe', Path(settings.t6) / 'Unlinker.exe']
    return {str(p.resolve()): [p.stat().st_size, p.stat().st_mtime_ns] for p in sorted(set(paths)) if p.is_file()}


def cli_command(*args: str) -> list[str]:
    return python_command() + ['-m', 'waw2bo2.cli', *args]


class ProcessRunner:
    """Stream a subprocess without blocking the UI; cancel only its process tree."""
    def __init__(self, emit):
        self.emit = emit
        self.process: subprocess.Popen | None = None
        self.cancelled = threading.Event()

    def run(self, command: list[str], log: Path, *, cwd: Path | None = None) -> int:
        if self.cancelled.is_set():
            return -1
        log.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env['PYTHONPATH'] = str(resource_root() / 'src')
        env['PYTHONUNBUFFERED'] = '1'
        env['PYTHONIOENCODING'] = 'utf-8'
        with log.open('w', encoding='utf-8') as output:
            self.process = subprocess.Popen(command, cwd=cwd or resource_root(), env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if self.cancelled.is_set():
                self._terminate()
            assert self.process.stdout is not None
            for line in self.process.stdout:
                output.write(line)
                output.flush()
                self.emit('line', line.rstrip())
            code = self.process.wait()
            self.process.stdout.close()
            self.process = None
            return code

    def _terminate(self) -> None:
        process = self.process
        if process is not None and process.poll() is None:
            if os.name == 'nt':
                subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                process.terminate()

    def cancel(self) -> None:
        self.cancelled.set()
        self._terminate()


def perform_build(settings: Settings, runner: ProcessRunner) -> int:
    failures = [c for c in preflight(settings) if c.required and not c.ready]
    if failures:
        raise ValueError('\n'.join(c.name + ': ' + c.detail for c in failures))
    paths = BuildPaths.for_settings(settings)
    paths.stage.mkdir(parents=True, exist_ok=True)
    # Hold a OS lock through native build/package to keep other launcher instances out.
    with (paths.root / 'build.lock').open('a+b') as lock:
        if os.name == 'nt':
            import msvcrt
            lock.write(b'0')
            lock.flush()
            lock.seek(0)
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise ValueError('This map is already building in another launcher.') from error
        receipt = paths.root / 'build.json'
        receipt.unlink(missing_ok=True)
        from .menuart import stage_art
        stage_art(settings, paths.stage / 'zone_raw' / settings.project)
        code = runner.run(build_command(settings), paths.root / 'build.log')
        if code or runner.cancelled.is_set():
            return code or -1
        runner.emit('line', '== 7. assembling the finished map package')
        code = runner.run(cli_command('package', str(paths.stage), settings.project, '--bo2', settings.bo2,
                                     '--work', str(paths.mod), '--dest', str(paths.output)), paths.root / 'package.log')
        if code or runner.cancelled.is_set():
            return code or -1
        required = [paths.output / name for name in (settings.project + '.ff', settings.project + '.ipak',
                                                    'mod.ff', 'mod_load.ff', 'mod.json')]
        if not all(p.is_file() for p in required):
            raise ValueError('Packaging did not create all required map files. Review package.log.')
        receipt.write_text(json.dumps({'project': settings.project, 'source': settings.fastfile,
                                      'output': str(paths.output)}, indent=2), encoding='utf-8')
        (paths.root / 'cache.json').write_text(json.dumps(input_stamp(settings)), encoding='utf-8')
        (paths.root / 'installed.json').unlink(missing_ok=True)
        return 0
