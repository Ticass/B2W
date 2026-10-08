"""Assemble the Linux release: the Python frontend run by the system Python,
plus the bundled Windows conversion worker for Wine.

No Python, Tk or system libraries are bundled (they were tied to the build
machine's Ubuntu/glibc). Users install the packages listed in docs/LINUX.md
with their own distribution's package manager.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import stat
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]

# argv handling is the same as the Windows executables: desktop_entry.py runs
# the GUI, --self-test, or "-m waw2bo2.cli ..." for the CLI.
LAUNCHER = """#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
PYTHON="${WAWCONVERTER_PYTHON:-python3}"
PYTHONPATH="$DIR/src${PYTHONPATH:+:$PYTHONPATH}" exec "$PYTHON" %s "$@"
"""
ENTRY = '"$DIR/tools/desktop_entry.py"'
ALL2RAW = "-c 'import sys; from waw2bo2.all2raw_cli import main; sys.exit(main(sys.argv[1:]))'"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--windows-bundle', type=Path, required=True)
    args = parser.parse_args()
    portable = args.windows_bundle.resolve()
    if not (portable / 'WawConverter.CLI.exe').is_file():
        raise FileNotFoundError('Missing freshly built Windows runtime')
    output = ROOT / 'work/linux_dist'
    bundle = output / 'WawConverter-Linux'
    if bundle.exists():
        shutil.rmtree(bundle)
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc')
    shutil.copytree(ROOT / 'src/waw2bo2', bundle / 'src/waw2bo2', ignore=ignore)
    (bundle / 'tools').mkdir(parents=True)
    for name in ('desktop_entry.py', 'audit_material_args.py'):
        shutil.copy2(ROOT / 'tools' / name, bundle / 'tools' / name)
    # resource_root() of a source tree is the bundle folder: same layout as a checkout
    shutil.copytree(portable / '_internal/vendor', bundle / 'vendor')
    shutil.copytree(portable / '_internal/tools/bin', bundle / 'tools/bin')
    shutil.copytree(portable, bundle / 'wine_runtime')
    for name, target in (('WawConverter', ENTRY), ('WawConverter.CLI', ENTRY), ('All2Raw', ALL2RAW)):
        script = bundle / name
        script.write_text(LAUNCHER % target, encoding='utf-8', newline='\n')
        script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    for folder in ('licenses', 'source'):
        shutil.copytree(portable / folder, bundle / folder, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'docs/LINUX.md', bundle / 'START HERE.md')
    shutil.copy2(ROOT / 'docs/DESKTOP.md', bundle / 'Desktop guide.md')
    shutil.copy2(ROOT / 'docs/EXTRACT_ALL.md', bundle / 'Extract All.md')
    shutil.copy2(ROOT / 'tools/build_linux.py', bundle / 'source/build_linux.py')
    archive = output / 'WawConverter-Linux-x86_64.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name, filter=_executable_launchers)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(digest + '  ' + archive.name + '\n')
    print(archive)


def _executable_launchers(info: tarfile.TarInfo) -> tarfile.TarInfo:
    # keep the launchers runnable when the archive is assembled off Linux
    if info.isfile() and Path(info.name).name in ('WawConverter', 'WawConverter.CLI', 'All2Raw'):
        info.mode = 0o755
    return info


if __name__ == '__main__':
    sys.exit(main())
