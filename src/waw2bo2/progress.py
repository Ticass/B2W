"""Flushed phase timings and bounded asset progress for the build console."""
from functools import wraps
import os
from pathlib import Path
import subprocess
import time


def verbose() -> bool:
    return os.environ.get('WAW2BO2_VERBOSE') == '1'


def timed(label, function, *args, **kwargs):
    print(f'[staging] {label}...', flush=True)
    started = time.monotonic()
    result = function(*args, **kwargs)
    print(f'[staging] {label} finished in {time.monotonic() - started:.1f}s', flush=True)
    return result


def phase(label):
    def decorate(function):
        @wraps(function)
        def wrapped(*args, **kwargs):
            return timed(label, function, *args, **kwargs)
        return wrapped
    return decorate


def items(label, values, *, name=str):
    """Report completed counts periodically; verbose mode names each next asset."""
    total = len(values)
    print(f'[staging] {label}: 0/{total}', flush=True)
    last = time.monotonic()
    for index, value in enumerate(values, 1):
        if verbose():
            print(f'[staging] {label} {index}/{total}: {name(value)}', flush=True)
        yield value
        now = time.monotonic()
        if index == total or now - last >= 5:
            print(f'[staging] {label}: {index}/{total} completed', flush=True)
            last = now


def native(command, log):
    """Keep a complete tool log and optionally stream it to the main console."""
    print(f'[tool] {Path(command[0]).name}; log: {log}', flush=True)
    with log.open('w', encoding='utf-8') as stream:
        if not verbose():
            return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding='utf-8', errors='replace')
        try:
            for line in process.stdout:
                stream.write(line)
                stream.flush()
                print(line.rstrip(), flush=True)
            return subprocess.CompletedProcess(command, process.wait())
        finally:
            process.stdout.close()


def captured(command, **kwargs):
    """Retain captured output for validation while streaming verbose tool lines."""
    if not verbose():
        return subprocess.run(command, **kwargs)
    for option in ('capture_output', 'stdout', 'stderr', 'text', 'errors'):
        kwargs.pop(option, None)
    print(f'[tool] Running {Path(command[0]).name}', flush=True)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace', **kwargs)
    lines = []
    try:
        for line in process.stdout:
            lines.append(line)
            print(line.rstrip(), flush=True)
        return subprocess.CompletedProcess(command, process.wait(), ''.join(lines), '')
    finally:
        process.stdout.close()
