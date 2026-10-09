"""Native Linux frontend bridge to the bundled Windows conversion worker."""
from __future__ import annotations

from dataclasses import asdict
import json
import os
from pathlib import Path
import shutil
import subprocess

from .resources import resource_root


def wine() -> str:
    runner = os.environ.get('WAWCONVERTER_WINE', 'wine')
    found = shutil.which(runner)
    if not found:
        raise RuntimeError('Wine is required for conversion. Install Wine or set WAWCONVERTER_WINE to its executable.')
    return found


def wine_environment() -> dict[str, str]:
    environment = os.environ.copy()
    # The native frontend keeps a Unix LOCALAPPDATA path. The Windows worker
    # must receive Wine's own Windows value, not that frontend convenience.
    environment.pop('LOCALAPPDATA', None)
    return environment


def winepath(option: str, path: str) -> str:
    runner = Path(wine())
    program = runner.with_name('winepath')
    if not program.is_file():
        found = shutil.which('winepath')
        if not found:
            raise RuntimeError('winepath is required beside the Wine runner or on PATH.')
        program = Path(found)
    result = subprocess.run([str(program), option, path], env=wine_environment(),
                            capture_output=True, text=True, check=True, timeout=60)
    value = result.stdout.strip()
    if not value:
        raise RuntimeError('winepath returned an empty path')
    return value


def windows_path(path: str | Path) -> str:
    return winepath('-w', str(Path(path).resolve()))


def configure_localappdata() -> None:
    """Resolve the actual Windows user in the selected prefix for install/launch."""
    if os.environ.get('LOCALAPPDATA'):
        return
    result = subprocess.run([wine(), 'cmd', '/c', 'echo %LOCALAPPDATA%'],
                            env=wine_environment(), capture_output=True, text=True, check=True, timeout=60)
    lines = result.stdout.strip().splitlines()
    value = lines[-1] if lines else ''
    if '%LOCALAPPDATA%' in value or not value:
        raise RuntimeError('Wine could not resolve LOCALAPPDATA in the selected prefix.')
    os.environ['LOCALAPPDATA'] = winepath('-u', value)


def build_command(settings, paths, *, redump: bool) -> list[str]:
    worker = resource_root() / 'wine_runtime/WawConverter.CLI.exe'
    if not worker.is_file():
        raise RuntimeError('The bundled Windows conversion worker is missing. Extract the entire Linux archive.')
    configure_localappdata()
    payload = asdict(settings)
    for field in ('waw', 'bo2', 'waw_tools', 't4', 't6', 'decoder', 'work', 'fastfile',
                  'menu_blit', 'menu_large', 'menu_blur', 'loading_song'):
        if payload.get(field):
            payload[field] = windows_path(payload[field])
    command = [wine(), str(worker), '-m', 'waw2bo2.cli', 'build-map',
               '--settings-json', json.dumps(payload), '--build-root', windows_path(paths.root)]
    if redump:
        command.append('--redump')
    from .all2raw import CachePaths
    caches = CachePaths.for_settings(settings)
    command += ['--waw-cache-root', windows_path(caches.waw), '--bo2-cache-root', windows_path(caches.bo2)]
    return command


def extract_command(settings) -> list[str]:
    # Use the same path conversion and cache roots as the build worker.
    from .launcher import BuildPaths
    # Extraction works before a map/project has been selected.
    from dataclasses import replace
    temporary_settings = replace(settings, project=settings.project or 'zm_extract',
                                 fastfile=settings.fastfile or str(Path(settings.work) / 'unused.ff'))
    paths = BuildPaths.for_settings(temporary_settings)
    command = build_command(temporary_settings, paths, redump=False)
    command[command.index('build-map')] = 'all2raw'
    index = command.index('--build-root')
    del command[index:index + 2]
    payload_index = command.index('--settings-json') + 1
    payload = json.loads(command[payload_index])
    if not settings.fastfile:
        payload['fastfile'] = ''
    payload['project'] = settings.project
    command[payload_index] = json.dumps(payload)
    return command


def launch_command(command: list[str]) -> list[str]:
    return [wine(), str(command[0]), command[1], windows_path(command[2]), *command[3:]]
