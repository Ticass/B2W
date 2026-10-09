"""Authored globe placement and native BO2 loading-music aliases."""
from __future__ import annotations

import csv
import hashlib
import math
import struct
import subprocess
import tempfile
from pathlib import Path

from . import audio, sounds


def coordinate(value, label: str, limit: float) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f'{label} must be a number between {-limit:g} and {limit:g}.') from error
    if not math.isfinite(number) or abs(number) > limit:
        raise ValueError(f'{label} must be a number between {-limit:g} and {limit:g}.')
    return number


def read_song(path: Path, *, decode: bool = True) -> bytes:
    """Accept PCM WAV without changing channels, rate, duration or samples."""
    original = path.read_bytes()
    try:
        form, parts = audio.chunks(original)
    except ValueError:
        if decode:
            return decode_song(path)
        raise
    fmt = parts[b'fmt ']
    if len(fmt) < 16:
        raise ValueError('Loading song has a truncated WAV format.')
    tag, channels, rate, byte_rate, align, bits = struct.unpack_from('<HHIIHH', fmt)
    if (form != b'WAVE' or tag != 1 or bits != 16 or channels not in (1, 2)
            or rate not in audio.T6_RATES or align != channels * 2
            or byte_rate != rate * align):
        if decode:
            return decode_song(path)
        raise ValueError('Decoded loading song must be a mono/stereo 16-bit PCM WAV at a supported BO2 sample rate.')
    if not parts[b'data']:
        raise ValueError('Loading song contains no audio samples.')
    return audio.canonical_pcm(parts[b'data'], channels, rate)


def decode_song(path: Path) -> bytes:
    decoder = audio.find_decoder()
    if decoder is None:
        raise ValueError('Loading-song conversion needs FFmpeg. Use the desktop package or install imageio-ffmpeg.')
    with tempfile.TemporaryDirectory(prefix='waw_loading_song_') as folder:
        output = Path(folder) / 'song.wav'
        result = subprocess.run([str(decoder), '-v', 'error', '-nostdin', '-y', '-i', str(path.resolve()),
                                 '-map', '0:a:0', '-vn', '-ac', '2', '-ar', '48000', '-c:a', 'pcm_s16le', str(output)],
                                capture_output=True, text=True, errors='replace')
        if result.returncode or not output.is_file():
            raise ValueError('Cannot decode loading song: ' + result.stderr.strip()[-1000:])
        return read_song(output, decode=False)


def validate_settings(settings) -> tuple[float, float]:
    result = (coordinate(settings.menu_longitude, 'Longitude', 180),
              coordinate(settings.menu_latitude, 'Latitude', 90))
    if settings.loading_song:
        # Native CL loading code formats this into a 64-byte buffer, including
        # its optional _patch suffix. Reject truncation rather than silence.
        if len('mus_load_' + settings.project + '_patch') >= 64:
            raise ValueError('Loading music requires a BO2 map name of at most 48 characters.')
        source = Path(settings.loading_song)
        if not source.is_file() or source.stat().st_size == 0:
            raise ValueError('Choose an existing, nonempty loading-song audio file.')
    return result


def stage_song(settings, root: Path) -> dict:
    bank = f'waw_{settings.project}_menu.all'
    aliases = root / 'soundbank' / (bank + '.aliases.csv')
    pcm = root / 'sound' / 'waw_menu' / (settings.project + '.wav')
    if not settings.loading_song:
        # These two paths belong solely to this generator. Never delete user
        # source tracks or unrelated gameplay banks when clearing the field.
        aliases.unlink(missing_ok=True)
        pcm.unlink(missing_ok=True)
        return {}
    data = read_song(Path(settings.loading_song))
    pcm.parent.mkdir(parents=True, exist_ok=True)
    pcm.write_bytes(data)
    row = dict.fromkeys(sounds.T6_ALIAS_COLUMNS, '')
    row.update(FileSource=pcm.relative_to(root).as_posix(), Storage='streamed',
               Bus='bus_music', VolumeGroup='grp_menu', DuckGroup='snp_music',
               ReverbSend='0', CenterSend='0', VolMin='79', VolMax='79',
               DistMin='0', DistMaxDry='5000', DistMaxWet='5000',
               DryMinCurve='allon', DryMaxCurve='default', WetMinCurve='allon', WetMaxCurve='default',
               LimitCount='1', EntityLimitCount='8', LimitType='oldest', EntityLimitType='oldest',
               PitchMin='0', PitchMax='0', PriorityMin='100', PriorityMax='100',
               PriorityThresholdMin='0', PriorityThresholdMax='1', PanType='2d', Pan='music_all',
               Looping='looping', Probability='1', StartDelay='0', EnvelopMin='0', EnvelopMax='0',
               EnvelopPercent='0', OcclusionLevel='0.25', IsBig='no', DistanceLpf='yes',
               FluxType='none', FluxTime='0', Doppler='no', Timescale='no', IsMusic='yes',
               IsCinematic='no', FadeIn='500', FadeOut='5000', Pauseable='yes',
               StopOnEntDeath='no', DopplerScale='0', VoiceLimit='no', IgnoreMaxDist='no', NeverPlayTwice='no')
    names = ['mus_load_' + settings.project, 'mus_load_' + settings.project + '_patch']
    aliases.parent.mkdir(parents=True, exist_ok=True)
    with aliases.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=sounds.T6_ALIAS_COLUMNS)
        writer.writeheader()
        for name in names:
            writer.writerow({**row, 'Name': name})
    return {'bank': bank, 'aliases': names, 'pcm': pcm.relative_to(root).as_posix(),
            'sha256': hashlib.sha256(data).hexdigest()}
