# Shadow run: stage-bridge of a real map in process (Python steps); at
# sounds.stage the Python step runs, then waw2bo2n sounds-stage on the same
# inputs into a separate project folder, and their outputs are compared.
# Usage: shadow_sounds.py der_riese|snn <outdir>
import filecmp
import json
import os
import runpy
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
os.environ['WAW2BO2_NATIVE'] = '0'
os.environ['WAW2BO2_BUILD_CACHE'] = '0'
from waw2bo2 import builddriver, cli, sounds  # noqa: E402

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'


class Done(Exception):
    pass


def outputs(project: Path) -> dict:
    found = {}
    for sub in ('sound_ir', 'sound/waw'):
        for p in (project / sub).rglob('*'):
            if p.is_file():
                found[p.relative_to(project).as_posix()] = p
    if (project / 'sounds.stage.json').is_file():
        found['sounds.stage.json'] = project / 'sounds.stage.json'
    return found


def shadow(roots, project, iwd_dirs, stock=None, names=None):
    start = time.time()
    sounds.python_stage(roots, project, iwd_dirs, stock, names)
    python_seconds = time.time() - start
    native_project = Path(str(project) + '_native')
    native_project.mkdir(parents=True, exist_ok=True)
    globals_file = sounds._globals_file(roots, stock)
    args = {'project': str(native_project), 'roots': [str(r) for r in roots], 'iwd_dirs': [str(d) for d in iwd_dirs],
            'names': sorted(names) if names is not None else None,
            'globals': str(globals_file) if globals_file is not None else None}
    args_file = native_project / 'args.json'
    args_file.write_text(json.dumps(args), encoding='utf-8')
    start = time.time()
    proc = subprocess.run([str(NATIVE), 'sounds-stage', str(args_file)], capture_output=True, text=True)
    native_seconds = time.time() - start
    print(f'SHADOW python {python_seconds:.1f}s, native {native_seconds:.1f}s, exit {proc.returncode}: '
          f'{proc.stdout.strip()} {proc.stderr.strip()[-400:]}', flush=True)
    a, b = outputs(Path(project)), outputs(native_project)
    different = [n for n in sorted(set(a) & set(b)) if not filecmp.cmp(a[n], b[n], shallow=False)]
    print(f'SHADOW {len(a)} python files, {len(b)} native; only python {len(set(a) - set(b))}, '
          f'only native {len(set(b) - set(a))}, different {len(different)}', flush=True)
    for n in (sorted(set(a) ^ set(b)) + different)[:20]:
        print('SHADOW   ', n, flush=True)
    raise Done


sounds.stage = shadow


def run(command, log=None):
    if 'stage-bridge' not in command:
        raise Done
    cli.main(command[command.index('stage-bridge'):])
    raise Done


builddriver.run = run
builddriver.remember = lambda *a, **k: None
sys.argv = ['bench.py', sys.argv[1], sys.argv[2]]
try:
    runpy.run_path('C:/WawConverter/work/perf/bench.py', run_name='__main__')
except Done:
    pass
