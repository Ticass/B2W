"""Build native Linux executables with a bundled Wine conversion worker."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--windows-bundle', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'linux':
        raise RuntimeError('Native Linux executables must be built on Linux; use the release workflow.')
    portable = args.windows_bundle.resolve()
    if not (portable / 'WawConverter.CLI.exe').is_file():
        raise FileNotFoundError('Missing freshly built Windows runtime')
    output = ROOT / 'work/linux_dist'
    environment = os.environ.copy()
    environment['WAWCONVERTER_WINDOWS_BUNDLE'] = str(portable)
    subprocess.run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--distpath', str(output),
                    '--workpath', str(ROOT / 'work/linux_build'), str(ROOT / 'tools/linux.spec')],
                   env=environment, cwd=ROOT, check=True)
    bundle = output / 'WawConverter-Linux'
    for folder in ('licenses', 'source'):
        shutil.copytree(portable / folder, bundle / folder, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'docs/LINUX.md', bundle / 'START HERE.md')
    shutil.copy2(ROOT / 'docs/DESKTOP.md', bundle / 'Desktop guide.md')
    shutil.copy2(ROOT / 'docs/EXTRACT_ALL.md', bundle / 'Extract All.md')
    shutil.copy2(ROOT / 'tools/build_linux.py', bundle / 'source/build_linux.py')
    shutil.copy2(ROOT / 'tools/linux.spec', bundle / 'source/linux.spec')
    archive = output / 'WawConverter-Linux-x86_64.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(bundle, arcname=bundle.name)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(digest + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__':
    main()
