"""Content-addressed cache of decoded audio, shared by every build.

Decoding a sound (XAudio2 xWMA, Windows ACM ADPCM, FFmpeg resampling to 48 kHz)
is a pure function of its source bytes, the decoding code and the native tools.
The key covers all three, so a changed source, converter or tool never reuses
a stale result: a cache hit returns the bytes and report the decode returns.
Only successful decodes are stored. WAW2BO2_AUDIO_CACHE=0 turns the cache off.
"""
from __future__ import annotations

import functools
import hashlib
import json
import os
from pathlib import Path

VERSION = 1
# the code a decode runs (sounds.py only lends SoundError)
_MODULES = ('audio.py', 'audiocache.py', 'xwma.py', 'acm.py')


def enabled() -> bool:
    from . import buildcache
    return buildcache.enabled() and os.environ.get('WAW2BO2_AUDIO_CACHE', '1') != '0'


def root() -> Path:
    override = os.environ.get('WAW2BO2_AUDIO_CACHE_DIR')
    if override:
        return Path(override)
    from .launcher import user_directory
    return user_directory() / 'audio_cache'


def _file_identity(path: Path | None) -> str:
    if path is None:
        return '-'
    try:
        stat = Path(path).stat()
    except OSError:
        return f'{path}:missing'
    return f'{Path(path).resolve()}:{stat.st_size}:{stat.st_mtime_ns}'


@functools.lru_cache(maxsize=None)
def _environment(decoder: str, xwma_decoder: str) -> str:
    """Everything a decode depends on besides the source bytes."""
    from . import audio, xwma
    digest = hashlib.sha256(f'v{VERSION}'.encode())
    here = Path(__file__).parent
    for name in _MODULES:
        try:
            digest.update(name.encode() + b'\0' + (here / name).read_bytes())
        except OSError:
            digest.update(name.encode() + b'\0missing')
    try:
        helper = xwma.find_helper(Path(xwma_decoder) if xwma_decoder != '-' else None)
    except Exception:  # noqa: BLE001 - a missing helper only matters for xWMA sources
        helper = None
    try:
        codec = xwma.system_codec()
    except Exception:  # noqa: BLE001 - non-Windows
        codec = None
    resampler = audio.find_decoder(None)
    for part in (decoder, _file_identity(helper), _file_identity(codec), _file_identity(resampler)):
        digest.update(part.encode() + b'\0')
    return digest.hexdigest()


def key(source: bytes, decoder: Path | None, xwma_decoder: Path | None) -> str | None:
    if not enabled():
        return None
    environment = _environment(str(decoder) if decoder else '-', str(xwma_decoder) if xwma_decoder else '-')
    return hashlib.sha256(environment.encode() + hashlib.sha256(source).digest()).hexdigest()


def _paths(name: str) -> tuple[Path, Path]:
    folder = root() / name[:2]
    return folder / f'{name}.pcm', folder / f'{name}.json'


def load(name: str | None) -> tuple[bytes | None, dict, list] | None:
    if name is None:
        return None
    pcm_path, report_path = _paths(name)
    try:
        record = json.loads(report_path.read_text(encoding='utf-8'))
        pcm = pcm_path.read_bytes() if record['has_pcm'] else None
    except (OSError, ValueError, KeyError):
        return None
    if pcm is not None and hashlib.sha256(pcm).hexdigest() != record.get('file_sha256'):
        return None  # torn or corrupted entry: decode again
    return pcm, record['info'], record['added']


def store(name: str | None, pcm: bytes | None, info: dict, added: list) -> None:
    if name is None:
        return
    pcm_path, report_path = _paths(name)
    try:
        pcm_path.parent.mkdir(parents=True, exist_ok=True)
        suffix = f'.{os.getpid()}.tmp'
        if pcm is not None:
            tmp = pcm_path.with_name(pcm_path.name + suffix)
            tmp.write_bytes(pcm)
            os.replace(tmp, pcm_path)
        record = {'has_pcm': pcm is not None, 'info': info, 'added': added,
                  'file_sha256': hashlib.sha256(pcm).hexdigest() if pcm is not None else None}
        tmp = report_path.with_name(report_path.name + suffix)
        tmp.write_text(json.dumps(record), encoding='utf-8')
        os.replace(tmp, report_path)  # the report is written last: it marks a complete entry
    except OSError:
        pass  # caching never fails a build
