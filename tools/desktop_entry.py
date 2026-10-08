"""Common entry point for the windowed launcher and bundled console worker."""
from pathlib import Path
import runpy
import sys


MISSING_HELP = """WawConverter needs {missing}.
Install the dependencies with your distribution's package manager (see START HERE.md):
  Ubuntu/Debian/Mint/Pop!_OS: sudo apt install python3 python3-tk python3-pil python3-pil.imagetk wine
  Fedora:                     sudo dnf install python3 python3-tkinter python3-pillow python3-pillow-tk wine
  Arch/Manjaro/EndeavourOS:   sudo pacman -S python tk python-pillow wine
"""


def check_linux_dependencies(gui: bool):
    """The Linux release runs on the system Python (nothing is bundled)."""
    if getattr(sys, 'frozen', False) or sys.platform == 'win32':
        return
    missing = []
    if sys.version_info < (3, 11):
        missing.append(f'Python 3.11 or newer (this is {sys.version.split()[0]})')
    for module, package in (('tkinter', 'Tk for Python'), ('PIL.ImageTk', 'Pillow with Tk support')) if gui else ():
        try:
            __import__(module)
        except ImportError:
            missing.append(package)
    if missing:
        sys.stderr.write(MISSING_HELP.format(missing=', '.join(missing)))
        raise SystemExit(1)


def main():
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, 'reconfigure'):
            stream.reconfigure(line_buffering=True, write_through=True)
    check_linux_dependencies(gui=sys.argv[1:3] != ['-m', 'waw2bo2.cli'])
    if Path(sys.executable).stem.lower() == 'all2raw':
        from waw2bo2.all2raw_cli import main as extract_main
        return extract_main(sys.argv[1:])
    if sys.argv[1:] == ['--self-test']:
        import json
        import tempfile
        import tkinter as tk
        from waw2bo2.gui import Launcher
        from waw2bo2.launcher import Settings, discover, preflight
        from waw2bo2.resources import resource_root
        from waw2bo2 import weapons, gscport
        with tempfile.TemporaryDirectory() as temp:
            root = tk.Tk()
            root.withdraw()
            settings = discover(Settings(work=temp))
            app = Launcher(root, settings=settings, settings_path=Path(temp) / 'settings.json')
            root.update()
            app._toggle_advanced()
            app._show_reports()
            root.update()
            root.destroy()
            plan, report = weapons.plan([], Path(temp) / 'schema_test')
            native = {c.name: c.ready for c in preflight(settings, include_map=False)
                      if c.name in ('WaW extractor', 'BO2 bridge tools', 'Audio decoder')}
            if not all(native.values()):
                raise RuntimeError('Bundled native tools are missing: ' + str(native))
            print(json.dumps({'gui': 'ok', 'schemas': 'ok', 'compatibility_api': len(gscport.API),
                              'native_tools': native, 'resources': str(resource_root())}))
        return 0
    if len(sys.argv) >= 3 and sys.argv[1:3] == ['-m', 'waw2bo2.cli']:
        from waw2bo2.cli import main as cli_main
        sys.argv = [sys.argv[0], *sys.argv[3:]]
        return cli_main()
    if len(sys.argv) >= 2 and Path(sys.argv[1]).name == 'audit_material_args.py':
        from waw2bo2.resources import resource_root
        script = resource_root() / 'tools/audit_material_args.py'
        sys.argv = [str(script), *sys.argv[2:]]
        runpy.run_path(str(script), run_name='__main__')
        return 0
    from waw2bo2.gui import main as gui_main
    gui_main()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
