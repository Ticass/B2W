# Run the native build driver with a settings file (no shell quoting of the
# JSON argument): run_driver.py <settings.json> <build root> [extra args...]
import subprocess
import sys
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[1] / 'build/waw2bo2n.exe'
settings = Path(sys.argv[1]).read_text(encoding='utf-8')
raise SystemExit(subprocess.run([str(NATIVE), 'build-map', '--settings-json', settings, '--build-root', sys.argv[2], *sys.argv[3:]]).returncode)
