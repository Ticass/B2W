"""CodRepo map lookup and safe encoding for public community reports."""
from __future__ import annotations

import base64
import asyncio
from datetime import datetime, timezone
import html
import json
import logging
from pathlib import Path
import re
from typing import Any

import aiohttp

log = logging.getLogger('discord_support.map_compatibility')
API = 'https://callofdutyrepo.com/wp-json/wp/v2'
SOURCE = 'https://callofdutyrepo.com/wawmaps/'
REPORT_LABEL = 'map-compatibility-report'
REPORT_MARKER_RE = re.compile(r'^<!-- b2w-map-compatibility:v1:([A-Za-z0-9_=-]+) -->$')
ALLOWED_UPLOAD_SUFFIXES = {
    '.png', '.jpg', '.jpeg', '.webp', '.gif',
    '.txt', '.log', '.json', '.zip', '.dmp', '.mdmp', '.crash',
}
MAX_UPLOAD_BYTES = 8 * 1024 * 1024
MAX_UPLOAD_TOTAL_BYTES = 24 * 1024 * 1024


def normalize_map(row: dict[str, Any]) -> dict[str, Any] | None:
    title = html.unescape(re.sub(r'<[^>]*>', '', str(row.get('title', {}).get('rendered', '')))).strip()
    url = str(row.get('link', ''))
    try:
        map_id = int(row['id'])
    except (KeyError, TypeError, ValueError):
        return None
    if not title or not url.startswith('https://callofdutyrepo.com/'):
        return None
    return {'id': map_id, 'title': title, 'url': url, 'slug': str(row.get('slug', ''))}


def search_maps(maps: list[dict[str, Any]], query: str, limit: int = 25) -> list[dict[str, Any]]:
    needle = query.strip().casefold()
    if not needle:
        return maps[:limit]
    prefix, contains = [], []
    for item in maps:
        title = item['title'].casefold()
        if title.startswith(needle):
            prefix.append(item)
        elif needle in title:
            contains.append(item)
    return (prefix + contains)[:limit]


def encode_report_marker(report: dict[str, Any]) -> str:
    encoded = base64.urlsafe_b64encode(
        json.dumps(report, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    ).decode('ascii').rstrip('=')
    return f'<!-- b2w-map-compatibility:v1:{encoded} -->'


def decode_report_marker(body: str) -> dict[str, Any] | None:
    if not body:
        return None
    first_line = body.splitlines()[0].strip()
    match = REPORT_MARKER_RE.fullmatch(first_line)
    if not match:
        return None
    encoded = match.group(1)
    try:
        report = json.loads(base64.urlsafe_b64decode(encoded + '=' * (-len(encoded) % 4)))
    except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(report, dict):
        return None
    try:
        report['map_id'] = int(report['map_id'])
    except (KeyError, ValueError, TypeError):
        return None
    if report.get('outcome') not in {'playable', 'broken'}:
        return None
    if report.get('platform') not in {'windows', 'linux'}:
        return None
    if not report.get('map_title') or not report.get('discord_url'):
        return None
    return report


def valid_video_url(value: str) -> str:
    value = value.strip()
    if not value:
        return ''
    from urllib.parse import urlsplit
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('The gameplay link must be a valid https:// URL.')
    return value[:1000]


def validate_upload(filename: str, size: int) -> str | None:
    suffix = Path(filename).suffix.casefold()
    if suffix not in ALLOWED_UPLOAD_SUFFIXES:
        return 'Use screenshots, text/log/JSON/ZIP files, or crash dumps.'
    if size < 0 or size > MAX_UPLOAD_BYTES:
        return 'Each attachment must be 8 MiB or smaller.'
    return None


class CodRepoCatalog:
    def __init__(self, session: aiohttp.ClientSession, cache_path: Path, bootstrap_path: Path | None = None):
        self.session = session
        self.cache_path = cache_path
        self.maps: list[dict[str, Any]] = []
        for path in (cache_path, bootstrap_path):
            if path is None:
                continue
            try:
                cached = json.loads(path.read_text(encoding='utf-8'))
                self.maps = [item for row in cached.get('maps', []) if (item := normalize_cached_map(row))]
                if self.maps:
                    break
            except (OSError, ValueError, TypeError):
                continue
        self.maps.sort(key=lambda item: item['title'].casefold())

    async def _json(self, path: str, params: dict[str, Any] | None = None):
        async with self.session.get(f'{API}{path}', params=params) as response:
            response.raise_for_status()
            return await response.json(), response.headers

    async def refresh(self) -> bool:
        try:
            categories, _ = await self._json('/categories', {'slug': 'wawmaps', 'per_page': 100})
            if not isinstance(categories, list) or len(categories) != 1:
                raise ValueError('CodRepo did not return one WAW maps category')
            category_id = categories[0]['id']
            _, headers = await self._json('/posts', {
                'categories': category_id, 'per_page': 100, 'page': 1,
                '_fields': 'id,link,title,slug',
            })
            expected = int(headers.get('X-WP-Total', '0'))
            pages = int(headers.get('X-WP-TotalPages', '0'))
            if not expected or not pages:
                raise ValueError('CodRepo returned an empty WAW maps category')
            refreshed = []
            for page in range(1, pages + 1):
                rows, _ = await self._json('/posts', {
                    'categories': category_id, 'per_page': 100, 'page': page,
                    '_fields': 'id,link,title,slug',
                })
                refreshed.extend(item for row in rows if (item := normalize_map(row)))
            ids = {item['id'] for item in refreshed}
            if len(ids) != len(refreshed) or len(refreshed) < expected:
                raise ValueError(f'CodRepo map list was incomplete ({len(refreshed)} of {expected})')
            refreshed.sort(key=lambda item: item['title'].casefold())
            self.maps = refreshed
            self.cache_path.parent.mkdir(parents=True, exist_ok=True)
            self.cache_path.write_text(json.dumps({
                'source': SOURCE,
                'fetched_at': datetime.now(timezone.utc).isoformat(),
                'maps': refreshed,
            }, ensure_ascii=False), encoding='utf-8')
            log.info('Refreshed %s map records from CodRepo', len(refreshed))
            return True
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError, KeyError, TypeError) as error:
            log.warning('CodRepo map refresh failed (%s); keeping %s cached maps', type(error).__name__, len(self.maps))
            return False


def normalize_cached_map(row: dict[str, Any]) -> dict[str, Any] | None:
    if not isinstance(row, dict):
        return None
    try:
        map_id = int(row['id'])
    except (KeyError, TypeError, ValueError):
        return None
    title, url = str(row.get('title', '')).strip(), str(row.get('url', ''))
    if not title or not url.startswith('https://callofdutyrepo.com/'):
        return None
    return {'id': map_id, 'title': title, 'url': url, 'slug': str(row.get('slug', ''))}
