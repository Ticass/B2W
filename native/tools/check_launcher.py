# Compare the native launcher rules (waw2bo2n launcher-check) with launcher.py
# for settings files. Usage: check_launcher.py <settings.json> ...
import json
import subprocess
import sys
import warnings
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
from waw2bo2 import launcher  # noqa: E402
from waw2bo2.all2raw import CachePaths  # noqa: E402

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'
warnings.simplefilter('ignore')


def python_view(path: Path) -> dict:
    s = launcher.discover(launcher.Settings.load(path))
    out = {'settings': asdict(s), 'project_name': launcher.project_name(s.fastfile),
           'map_zone': str(launcher.map_zone(s.fastfile))}
    caches = CachePaths.for_settings(s)
    out['cache'] = [str(caches.waw), str(caches.bo2)]
    try:
        p = launcher.BuildPaths.for_settings(s)
        out['paths'] = [str(p.root), str(p.stage), str(p.stock), str(p.waw_dumps), str(p.mod), str(p.output)]
        out['complete'] = p.complete(s.project, bo2_stock_perks=s.bo2_stock_perks)
        command = launcher.build_command(s)
        out['build_command'] = command[command.index('build-map'):]
    except ValueError as error:
        out['paths'] = str(error)
    out['checks'] = [[c.name, c.ready, c.detail, c.required] for c in launcher.preflight(s)]
    return out


def main():
    failed = 0
    for name in sys.argv[1:]:
        native = json.loads(subprocess.run([str(NATIVE), 'launcher-check', name], capture_output=True, text=True, check=True).stdout)
        python = python_view(Path(name))
        if 'build_command' in native:
            # the executable differs by design: the native driver against python -m waw2bo2.cli
            native['build_command'] = native['build_command'][native['build_command'].index('build-map'):]
        native.pop('installed', None)
        native.pop('stamp', None)
        keys = sorted(set(native) | set(python))
        diff = [k for k in keys if native.get(k) != python.get(k)]
        print(name, 'OK' if not diff else 'DIFF ' + ', '.join(diff))
        for k in diff:
            print('  native', json.dumps(native.get(k))[:600])
            print('  python', json.dumps(python.get(k))[:600])
        failed += bool(diff)
    raise SystemExit(failed)


main()
