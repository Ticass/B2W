# Write the settings work/perf/bench.py builds a map with, as the JSON the
# native driver takes: bench_settings.py der_riese|snn <settings.json>
import json
import runpy
import sys
from dataclasses import asdict

sys.path.insert(0, 'C:/WawConverter/src')
from waw2bo2 import builddriver  # noqa: E402

captured = {}


def capture(settings, **_):
    captured['settings'] = settings
    return 0


builddriver.build = capture
target = sys.argv[2]
sys.argv = ['bench.py', sys.argv[1], 'unused']
try:
    runpy.run_path('C:/WawConverter/work/perf/bench.py', run_name='__main__')
except SystemExit:
    pass
with open(target, 'w', encoding='utf-8') as stream:
    json.dump(asdict(captured['settings']), stream)
print(target)
