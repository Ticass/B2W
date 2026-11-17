# Shadow run: stage-bridge of a real map in process; shaders.stage runs the
# Python step, then waw2bo2n shaders-stage on the same roots, and every
# output file is compared byte for byte.
# Usage: shadow_shaders.py der_riese|snn <outdir>
import filecmp
import os
import runpy
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, 'C:/WawConverter/src')
os.environ['WAW2BO2_BUILD_CACHE'] = '0'  # the Python oracle computes everything
from waw2bo2 import builddriver, cli, shaders  # noqa: E402

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'
python_stage = shaders.python_stage


class Done(Exception):
    pass


def shadow_stage(roots, destination):
    start = time.time()
    python_stage(roots, destination)
    python_seconds = time.time() - start
    native_dest = Path(str(destination) + '_native')
    start = time.time()
    proc = subprocess.run([str(NATIVE), 'shaders-stage', '--dest', str(native_dest), *map(str, roots)],
                          capture_output=True, text=True)
    native_seconds = time.time() - start
    print(f'SHADOW python {python_seconds:.1f}s, native {native_seconds:.1f}s, native exit {proc.returncode}: '
          f'{proc.stdout.strip()} {proc.stderr.strip()}', flush=True)
    a = {p.relative_to(destination).as_posix(): p for p in Path(destination).rglob('*') if p.is_file()}
    b = {p.relative_to(native_dest).as_posix(): p for p in native_dest.rglob('*') if p.is_file()}
    different = [n for n in sorted(set(a) & set(b)) if not filecmp.cmp(a[n], b[n], shallow=False)]
    print(f'SHADOW {len(a)} python files, {len(b)} native; only python {len(set(a) - set(b))}, '
          f'only native {len(set(b) - set(a))}, different {len(different)}', flush=True)
    for n in (sorted(set(a) ^ set(b)) + different)[:30]:
        print('SHADOW   ', n, flush=True)
    raise Done


shaders.stage = shadow_stage


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
