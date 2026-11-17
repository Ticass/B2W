"""The converter's native (C++) steps: native/ builds waw2bo2n.exe.

Steps move to C++ one at a time (docs/CPP_PORT_PLAN.md). A ported step runs
natively whenever the executable exists; WAW2BO2_NATIVE=0 selects the Python
implementation (the equality oracle), WAW2BO2_NATIVE_EXE names another build.
A native failure fails the build: it never silently falls back to Python.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

from .resources import resource_root


class NativeError(RuntimeError):
    pass


def tool() -> Path | None:
    if os.environ.get('WAW2BO2_NATIVE') == '0':
        return None
    explicit = os.environ.get('WAW2BO2_NATIVE_EXE')
    if explicit:
        return Path(explicit)
    for candidate in (resource_root() / 'native/build/waw2bo2n.exe', resource_root() / 'tools/bin/waw2bo2n.exe'):
        if candidate.is_file():
            return candidate
    return None


def run(command: str, *args: str) -> str:
    exe = tool()
    if exe is None:
        raise NativeError('native converter is not available')
    proc = subprocess.run([str(exe), command, *args], capture_output=True, text=True, errors='replace')
    if proc.returncode:
        raise NativeError(f'{exe.name} {command} failed ({proc.returncode}): {proc.stderr.strip()[-2000:]}')
    return proc.stdout
