"""Local support bundles: failure context without game assets or source archives."""
from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
import platform
import re
import time
import zipfile

from . import __version__
from .launcher import BuildPaths, Settings


def create_bundle(settings: Settings, console: str, error: str = '') -> Path:
    work = Path(settings.work).resolve()
    cache = work / 'asset_cache'
    try:
        build = BuildPaths.for_settings(settings).root
    except ValueError:
        build = None
    files: set[Path] = set()
    if build is not None and build.exists():
        files.update(build.rglob('*.log'))
        files.update(build.rglob('*report.json'))
        files.update(build.rglob('weapons.dependencies.json'))
        files.update(build.rglob('weapons.stage.json'))
    extract_log = cache / 'extract.log'
    if extract_log.is_file():
        files.add(extract_log)
    # Tool logs referenced by this run include deferred stock dependency dumps.
    # Only generated logs within this build/cache may enter the archive.
    logs = console
    if build is not None and (build / 'build.log').is_file():
        logs += '\n' + (build / 'build.log').read_text(encoding='utf-8', errors='replace')
    for value in re.findall(r'\blog: (.*?)(?=\[tool\]|\r|\n|$)', logs):
        path = Path(value.strip()).resolve()
        if path.suffix.lower() == '.log' and (path.is_relative_to(cache) or
                                              (build is not None and path.is_relative_to(build))):
            files.add(path)
    directory = work / 'diagnostics'
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / f'{settings.project or "extraction"}-failure-{time.time_ns()}.zip'
    manifest = []
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('summary.json', json.dumps({'version': __version__, 'platform': platform.platform(),
            'error': error, 'settings': asdict(settings)}, indent=2))
        archive.writestr('console.log', console)
        for path in sorted(files):
            try:
                resolved = path.resolve()
                if build is not None and resolved.is_relative_to(build):
                    name = 'build/' + resolved.relative_to(build).as_posix()
                elif resolved.is_relative_to(cache):
                    name = 'asset_cache/' + resolved.relative_to(cache).as_posix()
                else:
                    continue
                size = path.stat().st_size
                with path.open('rb') as stream:
                    stream.seek(max(0, size - 2 * 1024 * 1024))
                    data = stream.read()
                archive.writestr(name, data)
                manifest.append({'file': name, 'original_bytes': size, 'included_bytes': len(data)})
            except OSError as exc:
                manifest.append({'file': str(path), 'read_error': str(exc)})
        archive.writestr('manifest.json', json.dumps(manifest, indent=2))
    return destination
