"""Machine-readable build timeline.

When WAW2BO2_TIMELINE names a file, every timed phase (progress.timed and
explicit spans), every native subprocess and every spawned Python worker
records one JSON line. Child processes inherit the variable, so the driver
and all of its CLI steps write one shared timeline (each process appends to
its own part beside the file; merge() joins them); each record names its
parent so nesting is explicit and nothing is summed twice.

Records: {"id", "parent", "kind", "name", "pid", "start", "wall", ...}
  kind "span":    "cpu" is this process's CPU over the span (all threads)
  kind "process": native/CLI child; "cpu", "peak_mb", "read_mb", "write_mb",
                  "returncode" from the OS process accounting
  kind "worker":  a spawned Python pool worker's own lifetime totals
Timing never changes a build: every failure to record is ignored.
"""
from __future__ import annotations

from contextlib import contextmanager
import atexit
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

ENV = 'WAW2BO2_TIMELINE'
PARENT_ENV = 'WAW2BO2_TIMELINE_PARENT'

_ids = itertools.count(1)
_local = threading.local()
_write_lock = threading.Lock()
_started = time.time()


def path() -> Path | None:
    value = os.environ.get(ENV)
    return Path(value) if value else None


def _new_id() -> str:
    return f'{os.getpid()}.{next(_ids)}'


def current() -> str | None:
    stack = getattr(_local, 'stack', None)
    if stack:
        return stack[-1]
    return os.environ.get(PARENT_ENV) or None


def _parts(target: Path) -> Path:
    return target.with_name(target.name + '.parts')


def emit(record: dict) -> None:
    target = path()
    if target is None:
        return
    try:
        line = json.dumps(record, separators=(',', ':'), default=str) + '\n'
        # one file per process: appends from several processes to one file
        # interleave on Windows; load() merges the parts
        part = _parts(target) / f'{os.getpid()}.jsonl'
        with _write_lock:
            part.parent.mkdir(parents=True, exist_ok=True)
            with open(part, 'a', encoding='utf-8') as stream:
                stream.write(line)
    except OSError:
        pass


@contextmanager
def span(name: str, **fields):
    """Time a block. Yields a dict the caller may fill with counts."""
    if path() is None:
        yield {}
        return
    record = {'id': _new_id(), 'parent': current(), 'kind': 'span', 'name': name,
              'pid': os.getpid(), 'thread': threading.current_thread().name, **fields}
    stack = getattr(_local, 'stack', None)
    if stack is None:
        stack = _local.stack = []
    stack.append(record['id'])
    extra: dict = {}
    record['start'] = time.time()
    wall, cpu = time.perf_counter(), time.process_time()
    try:
        yield extra
        record['ok'] = True
    except BaseException as exc:
        record['ok'] = False
        record['error'] = f'{type(exc).__name__}: {exc}'[:300]
        raise
    finally:
        record['wall'] = round(time.perf_counter() - wall, 4)
        record['cpu'] = round(time.process_time() - cpu, 4)
        stack.pop()
        record.update(extra)
        emit(record)


# -- OS process accounting (Windows; elsewhere only wall time is recorded) --

def _accounting(handle) -> dict:
    if os.name != 'nt' or handle is None:
        return {}
    try:
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        h = wintypes.HANDLE(int(handle))
        out: dict = {}
        times = [wintypes.FILETIME() for _ in range(4)]
        if kernel.GetProcessTimes(h, *(ctypes.byref(t) for t in times)):
            ticks = [(t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]]
            out['cpu'] = round(sum(ticks) / 1e7, 4)

        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_ulonglong) for n in (
                'ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]
        io = IO()
        if kernel.GetProcessIoCounters(h, ctypes.byref(io)):
            out['read_mb'] = round(io.ReadTransferCount / 2**20, 2)
            out['write_mb'] = round(io.WriteTransferCount / 2**20, 2)

        class MEM(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
                (n, ctypes.c_size_t) for n in (
                    'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                    'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage',
                    'PagefileUsage', 'PeakPagefileUsage')]
        mem = MEM()
        mem.cb = ctypes.sizeof(MEM)
        if ctypes.WinDLL('psapi').GetProcessMemoryInfo(h, ctypes.byref(mem), mem.cb):
            out['peak_mb'] = round(mem.PeakWorkingSetSize / 2**20, 1)
        return out
    except Exception:  # noqa: BLE001 - accounting is best effort
        return {}


def _own_handle():
    if os.name != 'nt':
        return None
    try:
        import ctypes
        return ctypes.WinDLL('kernel32').GetCurrentProcess()
    except Exception:  # noqa: BLE001
        return None


def self_accounting() -> dict:
    return _accounting(_own_handle())


def _describe(args) -> str:
    if isinstance(args, (str, bytes)):
        text = os.fsdecode(args)
    else:
        parts = [os.fsdecode(a) if isinstance(a, (bytes, os.PathLike)) else str(a) for a in args]
        if parts:
            parts[0] = Path(parts[0]).name
        if '-m' in parts and parts.index('-m') + 2 < len(parts):
            # python -m waw2bo2.cli <command>: name the command
            at = parts.index('-m')
            parts = [parts[at + 1], parts[at + 2]]
        text = ' '.join(parts)
    return text[:160]


class TimedPopen(subprocess.Popen):
    """subprocess.Popen that records one 'process' line when it exits."""

    def __init__(self, args, *a, **kw):
        self._tl = None
        if path() is not None:
            record_id = _new_id()
            env = kw.get('env')
            kw['env'] = dict(os.environ if env is None else env, **{PARENT_ENV: record_id})
            self._tl = {'id': record_id, 'parent': current(), 'kind': 'process',
                        'name': _describe(args), 'pid': None, 'start': time.time()}
            self._tl_clock = time.perf_counter()
        super().__init__(args, *a, **kw)
        if self._tl is not None:
            self._tl['pid'] = self.pid

    def _tl_finish(self):
        record, self._tl = self._tl, None
        if record is None or self.returncode is None:
            self._tl = record
            return
        record['wall'] = round(time.perf_counter() - self._tl_clock, 4)
        record['returncode'] = self.returncode
        record.update(_accounting(getattr(self, '_handle', None)))
        emit(record)

    def _wait(self, *a, **kw):
        result = super()._wait(*a, **kw)
        self._tl_finish()
        return result

    def _internal_poll(self, *a, **kw):
        result = super()._internal_poll(*a, **kw)
        if result is not None:
            self._tl_finish()
        return result


def _worker_exit() -> None:
    try:
        import multiprocessing
        if multiprocessing.parent_process() is None:
            return
    except Exception:  # noqa: BLE001
        return
    record = {'id': _new_id(), 'parent': os.environ.get(PARENT_ENV) or None, 'kind': 'worker',
              'name': 'python worker', 'pid': os.getpid(), 'start': _started,
              'wall': round(time.time() - _started, 4), **self_accounting()}
    emit(record)


_installed = False


def install() -> None:
    """Record subprocesses (and, in pool workers, the worker itself) when a
    timeline is active. Safe to call repeatedly."""
    global _installed
    if _installed or path() is None:
        return
    _installed = True
    subprocess.Popen = TimedPopen
    # spawned workers import this before multiprocessing names their parent
    atexit.register(_worker_exit)


def start(target: Path) -> None:
    """Begin a fresh timeline at target for this process and its children."""
    import shutil
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(_parts(target), ignore_errors=True)
    target.write_text('', encoding='utf-8')
    os.environ[ENV] = str(target)
    os.environ.pop(PARENT_ENV, None)
    install()


def load(target: Path) -> list[dict]:
    records = []
    sources = [target] if target.is_file() else []
    sources += sorted(_parts(target).glob('*.jsonl'))
    for source in sources:
        for line in source.read_text(encoding='utf-8').splitlines():
            try:
                records.append(json.loads(line))
            except ValueError:
                pass
    return sorted(records, key=lambda r: r.get('start', 0))


def merge(target: Path) -> None:
    """Gather the per-process parts into target (one JSON line per record)."""
    import shutil
    records = load(target)
    target.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records), encoding='utf-8')
    shutil.rmtree(_parts(target), ignore_errors=True)


def summarize(records: list[dict]) -> dict:
    """Totals per record and the top-level critical path. Children of a
    record are listed under it; only roots add up to the build's time."""
    by_id = {r['id']: r for r in records}
    children: dict[str | None, list[dict]] = {}
    for r in records:
        parent = r.get('parent') if r.get('parent') in by_id else None
        children.setdefault(parent, []).append(r)

    def tree(r: dict, depth: int) -> dict:
        node = {k: r[k] for k in ('kind', 'name', 'wall', 'cpu', 'peak_mb', 'read_mb', 'write_mb',
                                  'returncode', 'ok') if k in r}
        node['start_offset'] = round(r['start'] - origin, 3)
        kids = sorted(children.get(r['id'], []), key=lambda c: c['start'])
        workers = [k for k in kids if k['kind'] == 'worker']
        if workers:
            node['workers'] = {'count': len(workers), 'cpu': round(sum(w.get('cpu', 0) for w in workers), 3),
                               'peak_mb_max': max((w.get('peak_mb', 0) for w in workers), default=0)}
        nested = [tree(k, depth + 1) for k in kids if k['kind'] != 'worker']
        if nested:
            node['children'] = nested
            node['unattributed'] = round(r['wall'] - _covered(kids), 3)
        return node

    def _covered(kids: list[dict]) -> float:
        # wall time covered by children (overlapping children counted once)
        spans = sorted((k['start'], k['start'] + k['wall']) for k in kids if k['kind'] != 'worker')
        total, end = 0.0, None
        for s, e in spans:
            if end is None or s > end:
                total += e - s
                end = e
            elif e > end:
                total += e - end
                end = e
        return total

    if not records:
        return {}
    origin = min(r['start'] for r in records)
    roots = sorted(children.get(None, []), key=lambda r: r['start'])
    end = max(r['start'] + r['wall'] for r in records)
    processes = [r for r in records if r['kind'] == 'process']
    workers = [r for r in records if r['kind'] == 'worker']
    return {
        'wall': round(end - origin, 3),
        'process_count': len(processes),
        'native_cpu': round(sum(r.get('cpu', 0) for r in processes), 3),
        'worker_count': len(workers),
        'worker_cpu': round(sum(r.get('cpu', 0) for r in workers), 3),
        'peak_mb_max': max((r.get('peak_mb', 0) for r in records), default=0),
        'tree': [tree(r, 0) for r in roots],
    }


def flat(summary: dict) -> list[tuple[int, str, float]]:
    """(depth, label, wall) rows for printing."""
    rows = []

    def walk(nodes, depth):
        for n in nodes:
            rows.append((depth, f"{n['kind'][0]} {n['name']}", n['wall']))
            walk(n.get('children', []), depth + 1)
    walk(summary.get('tree', []), 0)
    return rows


if __name__ == '__main__':
    data = summarize(load(Path(sys.argv[1])))
    minimum = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    print(f"total {data['wall']:.1f}s, {data['process_count']} processes "
          f"({data['native_cpu']:.1f}s CPU), {data['worker_count']} workers ({data['worker_cpu']:.1f}s CPU), "
          f"peak {data['peak_mb_max']:.0f} MB")
    for depth, label, wall in flat(data):
        if wall >= minimum:
            print(f"{'  ' * depth}{wall:8.2f}  {label}")
