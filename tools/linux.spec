from pathlib import Path
import os
from PyInstaller.utils.hooks import collect_data_files, collect_submodules

root = Path(SPECPATH).parent
portable = Path(os.environ['WAWCONVERTER_WINDOWS_BUNDLE'])
datas = collect_data_files('waw2bo2')
datas += [(str(root / 'tools/audit_material_args.py'), 'tools'),
          (str(portable / '_internal/vendor'), 'vendor'),
          (str(portable / '_internal/tools/bin'), 'tools/bin'),
          (str(portable), 'wine_runtime')]
a = Analysis([str(root / 'tools/desktop_entry.py')], pathex=[str(root / 'src')],
             binaries=[], datas=datas, hiddenimports=collect_submodules('waw2bo2'),
             hookspath=[], hooksconfig={}, runtime_hooks=[], excludes=[], noarchive=False)
pyz = PYZ(a.pure)
gui = EXE(pyz, a.scripts, [], exclude_binaries=True, name='WawConverter',
          debug=False, strip=False, upx=False, console=True)
cli = EXE(pyz, a.scripts, [], exclude_binaries=True, name='WawConverter.CLI',
          debug=False, strip=False, upx=False, console=True)
coll = COLLECT(gui, cli, a.binaries, a.datas, strip=False, upx=False, name='WawConverter-Linux')
