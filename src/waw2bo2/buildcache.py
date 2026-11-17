"""Content-addressed cache for pure conversion steps, shared by every build.

A step whose result depends only on its input bytes, its own code and the
native tools it runs (shader translation, audio decoding) stores that result
here, keyed by all of them. A changed input, converter or tool is a different
key, never a stale hit. One file per entry, published atomically and checked
against its digest on read. WAW2BO2_BUILD_CACHE=0 turns every cache off.
"""
from __future__ import annotations

import functools
import hashlib
import os
import pickle
from pathlib import Path

FORMAT = 1


def enabled() -> bool:
    return os.environ.get('WAW2BO2_BUILD_CACHE', '1') != '0'


def root() -> Path:
    override = os.environ.get('WAW2BO2_BUILD_CACHE_DIR')
    if override:
        return Path(override)
    from .launcher import user_directory
    return user_directory() / 'build_cache'


def file_identity(path: Path | str | None) -> str:
    """Path, size and modification time: a replaced tool is a new identity."""
    if path is None:
        return '-'
    try:
        stat = Path(path).stat()
    except OSError:
        return f'{path}:missing'
    return f'{Path(path).resolve()}:{stat.st_size}:{stat.st_mtime_ns}'


@functools.lru_cache(maxsize=None)
def code_digest(*modules: str) -> str:
    """Digest of this package's source files (names relative to the package)."""
    digest = hashlib.sha256()
    here = Path(__file__).parent
    for name in modules:
        try:
            digest.update(name.encode() + b'\0' + (here / name).read_bytes() + b'\0')
        except OSError:
            digest.update(name.encode() + b'\0missing\0')
    return digest.hexdigest()


def key(*parts: str | bytes) -> str:
    digest = hashlib.sha256(f'format{FORMAT}'.encode())
    for part in parts:
        data = part if isinstance(part, bytes) else part.encode('utf-8')
        digest.update(hashlib.sha256(data).digest())
    return digest.hexdigest()


def _path(namespace: str, name: str) -> Path:
    return root() / namespace / name[:2] / f'{name}.bin'


def load(namespace: str, name: str):
    """The stored value, or None (missing, disabled or damaged)."""
    if not enabled():
        return None
    try:
        data = _path(namespace, name).read_bytes()
    except OSError:
        return None
    if len(data) < 32 or hashlib.sha256(data[32:]).digest() != data[:32]:
        return None
    try:
        return pickle.loads(data[32:])
    except Exception:  # noqa: BLE001 - a damaged entry is a miss
        return None


def store(namespace: str, name: str, value) -> None:
    """Publish value; caching never fails a build."""
    if not enabled():
        return
    target = _path(namespace, name)
    try:
        payload = pickle.dumps(value, protocol=pickle.HIGHEST_PROTOCOL)
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(f'{target.name}.{os.getpid()}.tmp')
        partial.write_bytes(hashlib.sha256(payload).digest() + payload)
        os.replace(partial, target)
    except OSError:
        pass
