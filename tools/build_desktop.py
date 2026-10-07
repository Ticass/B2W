"""Build the portable Windows launcher using existing native converter tools."""
from pathlib import Path
import importlib.metadata
import shutil
import struct
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def source_archive(folder: Path, destination: Path, *, tracked: list[str] | None = None):
    """Include corresponding native sources and dependency source checkouts."""
    paths = [folder / name for name in tracked] if tracked is not None else []
    if tracked is None:
        result = subprocess.run(['git', '-C', str(folder), 'ls-files', '--cached', '--others', '--exclude-standard'],
                                check=True, capture_output=True, text=True)
        paths = [folder / name for name in result.stdout.splitlines()]
    thirdparty = folder / 'thirdparty'
    if thirdparty.exists():
        paths.extend(p for p in thirdparty.rglob('*') if p.is_file())
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        seen = set()
        for path in paths:
            if not path.is_file():
                continue
            relative = path.relative_to(folder)
            if '.git' in relative.parts or relative.name.startswith('.git'):
                continue
            if 'build' in relative.parts or path.suffix.lower() in {'.exe', '.dll', '.obj', '.pdb', '.lib', '.a', '.so', '.pyc'}:
                continue
            if relative.as_posix() not in seen:
                archive.write(path, relative.as_posix())
                seen.add(relative.as_posix())


def main():
    output = ROOT / 'work/desktop_dist'
    work = ROOT / 'work/desktop_build'
    subprocess.run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--distpath', str(output),
                    '--workpath', str(work), str(ROOT / 'tools/desktop.spec')], check=True, cwd=ROOT)
    bundle = output / 'WawConverter'
    # Set the bundled copy LAA without requiring Visual Studio on the user's PC.
    linker = bundle / '_internal/vendor/OpenAssetToolsT6/build/bin/Release_x86/Linker.exe'
    data = bytearray(linker.read_bytes())
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('The bundled linker is not a valid PE executable')
    flags = struct.unpack_from('<H', data, pe + 22)[0]
    struct.pack_into('<H', data, pe + 22, flags | 0x20)
    linker.write_bytes(data)
    licenses = bundle / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'vendor/OpenAssetToolsT6/LICENSE', licenses / 'OpenAssetTools-GPL-3.0.txt')
    imageio_distribution = importlib.metadata.distribution('imageio-ffmpeg')
    imageio_license = next(p for p in imageio_distribution.files if str(p).endswith('.dist-info/LICENSE'))
    shutil.copy2(imageio_distribution.locate_file(imageio_license), licenses / 'imageio-ffmpeg-LICENSE.txt')
    (licenses / 'THIRD_PARTY.md').write_text(
        '# Bundled third-party components\n\n'
        'OpenAssetTools: GPL-3.0. Corresponding T4/T6 source archives, including '
        'dependency checkouts, are in ../source/.\n\n'
        'imageio-ffmpeg: BSD-2-Clause; see imageio-ffmpeg-LICENSE.txt. '
        'Sources: https://github.com/imageio/imageio-ffmpeg/tree/v0.6.0\n\n'
        'FFmpeg: 7.1 essentials build by gyan.dev, GPL-3.0. '
        'License text: OpenAssetTools-GPL-3.0.txt. '
        'FFmpeg source: https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz\n'
        'Build scripts and library source links: https://github.com/GyanD/codexffmpeg '
        'and https://www.gyan.dev/ffmpeg/builds/\n\n'
        'Python, Tcl/Tk, and runtime dependency notices are retained under _internal/.\n', encoding='utf-8')
    source = bundle / 'source'
    source.mkdir(exist_ok=True)
    source_archive(ROOT / 'vendor/OpenAssetTools', source / 'OpenAssetTools-T4.zip')
    tracked = subprocess.run(['git', 'ls-files', 'vendor/OpenAssetToolsT6'], check=True,
        capture_output=True, text=True, cwd=ROOT).stdout.splitlines()
    prefix = 'vendor/OpenAssetToolsT6/'
    source_archive(ROOT / 'vendor/OpenAssetToolsT6', source / 'OpenAssetTools-T6.zip',
                   tracked=[p.removeprefix(prefix) for p in tracked])
    python_source = bundle / 'source/waw2bo2'
    shutil.copytree(ROOT / 'src/waw2bo2', python_source, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copy2(ROOT / 'tools/run_bridge.ps1', source / 'run_bridge.ps1')
    shutil.copy2(ROOT / 'tools/xaudio_wma_decoder.cpp', source / 'xaudio_wma_decoder.cpp')
    shutil.copy2(ROOT / 'docs/DESKTOP.md', bundle / 'START HERE.md')
    shutil.copy2(ROOT / 'docs/USAGE.md', bundle / 'Advanced usage.md')
    for name in ['desktop.spec', 'build_desktop.py', 'desktop_entry.py', 'build_audio_decoder.ps1']:
        shutil.copy2(ROOT / 'tools' / name, source / name)
    archive = Path(shutil.make_archive(str(output / 'WawConverter-Windows'), 'zip', output, 'WawConverter'))
    print('Portable executable:', bundle / 'WawConverter.exe')
    print('Portable download:', archive)


if __name__ == '__main__':
    main()
