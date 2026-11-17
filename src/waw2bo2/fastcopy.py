"""Parallel shutil.copytree for trees of many small files.

Copying thousands of shader binaries and technique files one at a time is
bound by per-file open/close cost (and the virus scanner), not by bytes. This
copies the same files with the same metadata (shutil.copy2) on a thread pool:
the result is the tree shutil.copytree(src, dst, dirs_exist_ok=True) writes.
"""
from __future__ import annotations

import os
import shutil
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

WORKERS = min(32, (os.cpu_count() or 4) * 2)


def copytree(source: Path, target: Path) -> None:
    """shutil.copytree(source, target, dirs_exist_ok=True), files in parallel."""
    source, target = Path(source), Path(target)
    folders: list[tuple[Path, Path]] = []
    files: list[tuple[str, str]] = []
    for directory, names, filenames in os.walk(source):
        relative = os.path.relpath(directory, source)
        destination = target if relative == '.' else target / relative
        destination.mkdir(parents=True, exist_ok=True)
        folders.append((Path(directory), destination))
        files.extend((os.path.join(directory, f), str(destination / f)) for f in filenames)
    if len(files) < 64:
        for src, dst in files:
            shutil.copy2(src, dst)
    else:
        with ThreadPoolExecutor(WORKERS) as pool:
            for _ in pool.map(lambda pair: shutil.copy2(*pair), files, chunksize=64):
                pass
    # copytree copies each folder's stat after its contents, deepest first
    for directory, destination in reversed(folders):
        shutil.copystat(directory, destination)
