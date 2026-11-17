# Run a real map's stage-bridge in process (pure Python steps) and stop when
# the named function is called. Usage: stage_until.py <map> <outdir> <module.function>
import importlib
import os
import runpy
import sys

sys.path.insert(0, 'C:/WawConverter/src')
os.environ['WAW2BO2_NATIVE'] = '0'
from waw2bo2 import builddriver, cli  # noqa: E402


class Done(Exception):
    pass


module_name, function_name = sys.argv[3].rsplit('.', 1)
module = importlib.import_module('waw2bo2.' + module_name)


def stop(*args, **kwargs):
    raise Done


setattr(module, function_name, stop)


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
print('stopped at', module_name + '.' + function_name)
