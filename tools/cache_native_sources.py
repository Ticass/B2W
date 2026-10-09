"""Retain exact native sources alongside cached release binaries."""
from pathlib import Path
import subprocess

from build_desktop import ROOT, source_archive


def main():
    destination = ROOT / 'work/native_source'
    destination.mkdir(parents=True, exist_ok=True)
    source_archive(ROOT / 'vendor/OpenAssetTools', destination / 'OpenAssetTools-T4.zip')
    prefix = 'vendor/OpenAssetToolsT6/'
    tracked = subprocess.run(['git', 'ls-files', prefix], check=True, capture_output=True,
                             text=True, cwd=ROOT).stdout.splitlines()
    source_archive(ROOT / 'vendor/OpenAssetToolsT6', destination / 'OpenAssetTools-T6.zip',
                   tracked=[name.removeprefix(prefix) for name in tracked])


if __name__ == '__main__':
    main()
