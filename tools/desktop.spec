# Build a GUI executable and console worker sharing one portable runtime.
from pathlib import Path
import sys
from PyInstaller.utils.hooks import collect_data_files, collect_submodules

root = Path(SPECPATH).parent
sys.path.insert(0, str(root / 'src'))
datas = collect_data_files('waw2bo2')
datas += collect_data_files('imageio_ffmpeg')
datas += [(str(root / 'tools/run_bridge.ps1'), 'tools'),
          (str(root / 'tools/audit_material_args.py'), 'tools'),
          (str(root / 'tools/bin/xaudio_wma_decoder.exe'), 'tools/bin'),
          (str(root / 'vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h'),
           'vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon'),
          (str(root / 'vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponFields.h'),
           'vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon'),
          (str(root / 'vendor/OpenAssetToolsT6/raw/t6'), 'vendor/OpenAssetToolsT6/raw/t6')]
for folder in ['OpenAssetTools', 'OpenAssetToolsT6']:
    for name in (['Unlinker.exe'] if folder == 'OpenAssetTools' else ['Linker.exe', 'Unlinker.exe']):
        datas.append((str(root / 'vendor' / folder / 'build/bin/Release_x86' / name),
                      'vendor/' + folder + '/build/bin/Release_x86'))

a = Analysis([str(root / 'tools/desktop_entry.py')], pathex=[str(root / 'src')],
             binaries=[], datas=datas, hiddenimports=collect_submodules('waw2bo2'),
             hookspath=[], hooksconfig={}, runtime_hooks=[], excludes=[], noarchive=False)
pyz = PYZ(a.pure)
gui = EXE(pyz, a.scripts, [], exclude_binaries=True, name='WawConverter',
          debug=False, bootloader_ignore_signals=False, strip=False, upx=False, console=False)
cli = EXE(pyz, a.scripts, [], exclude_binaries=True, name='WawConverter.CLI',
          debug=False, bootloader_ignore_signals=False, strip=False, upx=False, console=True)
extract = EXE(pyz, a.scripts, [], exclude_binaries=True, name='All2Raw',
              debug=False, bootloader_ignore_signals=False, strip=False, upx=False, console=True)
coll = COLLECT(gui, cli, extract, a.binaries, a.datas, strip=False, upx=False, name='WawConverter')
