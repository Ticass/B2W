"""Durable intake and bounded diagnostics parsing; no Discord connection here."""
from __future__ import annotations

import base64
import io
import json
from pathlib import Path, PurePosixPath
import re
import sqlite3
import time
import zipfile

LABEL = 'discord-report'
REPLY_MARKER = '<!-- discord-agent-reply -->'
MAX_ATTACHMENT = 8 * 1024 * 1024
MAX_REPORT = 42000


def redact(text: str) -> str:
    text = re.sub(r'(?i)(?:[A-Z]:[\\/]Users[\\/]|/home/)[^\\/\s"\']+', '<user-home>', text)
    text = re.sub(r'(?i)((?:token|api[_-]?key|password|authorization)\s*[=:]\s*)[^\s,;]+', r'\1[REDACTED]', text)
    return text


def attachment_text(name: str, data: bytes) -> str:
    if len(data) > MAX_ATTACHMENT:
        return '[Attachment exceeds the 8 MiB intake limit.]'
    suffix = Path(name).suffix.lower()
    if suffix in {'.txt', '.log', '.json', '.csv'}:
        return redact(data[:MAX_REPORT].decode('utf-8', errors='replace'))
    if suffix != '.zip':
        return '[Image or unsupported attachment; see attachment metadata.]'
    blocks, remaining = [], MAX_REPORT
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for entry in archive.infolist()[:100]:
                # Never extract paths, execute files or recurse into nested archives.
                path = PurePosixPath(entry.filename.replace('\\', '/'))
                if (entry.is_dir() or path.is_absolute() or '..' in path.parts or
                    ':' in entry.filename or path.suffix.lower() not in {'.txt', '.log', '.json', '.csv'}):
                    continue
                if entry.file_size > 2 * 1024 * 1024 or entry.flag_bits & 1:
                    blocks.append(f'{path}: skipped oversized or encrypted entry')
                    continue
                if remaining <= 0:
                    break
                with archive.open(entry) as stream:
                    text = redact(stream.read(min(remaining, 32000)).decode('utf-8', errors='replace'))
                blocks.append(f'File: {path}\n{text}')
                remaining -= len(text)
    except (zipfile.BadZipFile, RuntimeError, OSError, NotImplementedError):
        return '[Could not read diagnostics ZIP; ask for a fresh Save Diagnostics ZIP.]'
    return '\n\n'.join(blocks)[:MAX_REPORT] or '[ZIP contains no supported diagnostic text.]'


def message_marker(message_id: int) -> str:
    return f'<!-- discord-message:{message_id} -->'


def reply_text(body: str) -> str:
    """The Discord text of an investigation reply comment, without its hidden markers."""
    return re.sub(r'<!--.*?-->', '', body.removeprefix(REPLY_MARKER), flags=re.DOTALL).strip()


def thread_marker(thread_id: int) -> str:
    return f'<!-- discord-thread:{thread_id} -->'


# The Discord messages behind an issue live in one hidden block of the issue
# body, so the issue reads as a bug report rather than a pasted conversation.
# The investigation reads them from there; base64 keeps '-->' out of the comment.
DATA_RE = re.compile(r'<!-- discord-report-data:([A-Za-z0-9+/=]*) -->')
# The bot's own first draft; the investigation replaces it with its analysis.
DRAFT_MARKER = '<!-- discord-report-draft -->'
MAX_MESSAGE = 4000
MAX_DATA = 40000
ERROR_RE = re.compile(r'\berror: \S|\bTraceback \(most recent call last\)', re.IGNORECASE)
IMAGE_TYPES = {'image/png', 'image/jpeg', 'image/webp', 'image/gif'}


def _zip_text(archive: zipfile.ZipFile, name: str, limit: int) -> str | None:
    try:
        entry = archive.getinfo(name)
    except KeyError:
        return None
    if entry.file_size > 2 * 1024 * 1024 or entry.flag_bits & 1:
        return None
    with archive.open(entry) as stream:
        return stream.read(limit).decode('utf-8', errors='replace')


def diagnostics_facts(data: bytes) -> dict:
    """Map, version, platform, options and the final error lines of a
    Save Diagnostics ZIP (summary.json and console.log; nothing is extracted)."""
    if len(data) > MAX_ATTACHMENT:
        return {}
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            summary = _zip_text(archive, 'summary.json', 256 * 1024)
            console = _zip_text(archive, 'console.log', 2 * 1024 * 1024) or ''
    except (zipfile.BadZipFile, RuntimeError, OSError, NotImplementedError):
        return {}
    try:
        info = json.loads(summary) if summary else {}
    except ValueError:
        info = {}
    info = info if isinstance(info, dict) else {}
    settings = info.get('settings') if isinstance(info.get('settings'), dict) else {}
    facts = {}
    for key, value in (('version', info.get('version')), ('platform', info.get('platform')),
                       ('project', settings.get('project')), ('map', settings.get('menu_title'))):
        if isinstance(value, str) and value.strip():
            facts[key] = redact(value.strip())[:200]
    options = [label for key, label in (('bo2_stock_perks', 'BO2 stock perks'), ('source_fx', 'WaW source FX'),
                                        ('remaster', 'BO2 remaster materials')) if settings.get(key) is True]
    if options:
        facts['options'] = options
    errors = []
    if isinstance(info.get('error'), str) and info['error'].strip():
        errors.append(info['error'].strip())
    for line in console.splitlines():
        if ERROR_RE.search(line) and line.strip() not in errors:
            errors.append(line.strip())
    if errors:
        facts['errors'] = [redact(line)[:500] for line in errors[-5:]]
    return facts


def encode_messages(messages: list[dict]) -> str:
    kept = [dict(message, content=str(message.get('content', ''))[:MAX_MESSAGE]) for message in messages]
    while True:
        data = base64.b64encode(json.dumps({'messages': kept}, ensure_ascii=True).encode()).decode()
        # Keep the original report and the newest replies when a thread grows long.
        if len(data) <= MAX_DATA or len(kept) <= 2:
            return f'<!-- discord-report-data:{data} -->'
        del kept[1]


def decode_messages(body: str) -> list[dict]:
    match = DATA_RE.search(body or '')
    if not match:
        return []
    try:
        data = json.loads(base64.b64decode(match.group(1)))
    except ValueError:
        return []
    messages = data.get('messages') if isinstance(data, dict) else None
    return [m for m in messages if isinstance(m, dict)] if isinstance(messages, list) else []


def with_messages(body: str, messages: list[dict]) -> str:
    """``body`` with its hidden data block replaced (or appended)."""
    block = encode_messages(messages)
    if DATA_RE.search(body or ''):
        return DATA_RE.sub(lambda _: block, body, count=1)
    return f'{(body or "").rstrip()}\n\n{block}'


def merged_facts(messages: list[dict]) -> dict:
    facts = {}
    for message in messages:
        facts.update(message.get('facts') or {})
    return facts


def report_title(thread_title: str, messages: list[dict]) -> str:
    facts = merged_facts(messages)
    project, name = facts.get('project'), facts.get('map')
    if project:
        label = f'{name} ({project})' if name and name != project else project
        return f'Error when building {label}'[:240]
    return f'Error report: {thread_title}'[:240]


def attachment_lines(messages: list[dict]) -> list[str]:
    lines = []
    for message in messages:
        for item in message.get('attachments') or []:
            name = str(item.get('name', 'attachment')).replace(']', '').replace('[', '')[:120]
            url = item.get('stored_url') or item.get('url')
            if not url:
                continue
            size = item.get('size')
            detail = f' ({size / 1048576:.1f} MiB)' if isinstance(size, int) and size >= 1048576 else ''
            prefix = '!' if item.get('content_type') in IMAGE_TYPES else ''
            lines.append(f'- {prefix}[{name}]({url}){detail}')
    return lines


def format_issue(thread_title: str, messages: list[dict]) -> str:
    """The bot's draft bug report for a Discord thread, before the investigation."""
    facts = merged_facts(messages)
    errors = facts.get('errors') or []
    sections = [DRAFT_MARKER, '### Error']
    if errors:
        sections.append('```text\n' + '\n'.join(errors).replace('```', "'''") + '\n```')
    else:
        sections.append('No error text was found in the report or its attachments.')
    environment = [f'- {label}: {facts[key]}' for key, label in
                   (('version', 'WawConverter'), ('platform', 'Platform')) if facts.get(key)]
    if facts.get('project'):
        name = facts.get('map')
        environment.append(f"- Map: {name} (`{facts['project']}`)" if name and name != facts['project']
                           else f"- Map: `{facts['project']}`")
    if facts.get('options'):
        environment.append('- Options: ' + ', '.join(facts['options']))
    sections += ['### Environment', '\n'.join(environment) or
                 'Not provided. A Save Diagnostics ZIP (Reports → Save Diagnostics) includes it.']
    description = redact(str(messages[0].get('content', '')).strip()) if messages else ''
    sections += ['### Description', '\n'.join('> ' + line for line in description.splitlines())
                 if description else 'No description was given.']
    sections += ['### Attachments', '\n'.join(attachment_lines(messages)) or 'None.']
    url = messages[0].get('url') if messages else ''
    source = f'[Discord thread]({url})' if url else 'Discord'
    sections.append(f'---\n<sub>Reported on {source}. The automated investigation replaces this draft '
                    'with its analysis.</sub>')
    return '\n\n'.join(sections)


def chunks(text: str, size: int = 1900) -> list[str]:
    return [text[start:start + size] for start in range(0, len(text), size)] or ['No findings were returned.']


class State:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.executescript('''
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS threads (
                id INTEGER PRIMARY KEY, title TEXT NOT NULL, issue INTEGER,
                history_cursor INTEGER NOT NULL DEFAULT 0,
                reply_cursor INTEGER NOT NULL DEFAULT 0, acknowledged INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS inbox (
                id INTEGER PRIMARY KEY, thread INTEGER NOT NULL, payload TEXT NOT NULL,
                done INTEGER NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 0,
                retry_at REAL NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS replies (
                id INTEGER PRIMARY KEY, thread INTEGER NOT NULL, body TEXT NOT NULL,
                done INTEGER NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 0,
                retry_at REAL NOT NULL DEFAULT 0
            );
        ''')

    def cutoff(self, initial: int) -> int:
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO metadata VALUES (?, ?)', ('cutoff', str(initial)))
        return int(self.db.execute('SELECT value FROM metadata WHERE key = ?', ('cutoff',)).fetchone()[0])

    def enqueue(self, thread_id: int, title: str, payload: dict):
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO threads(id, title) VALUES (?, ?)', (thread_id, title))
            self.db.execute('INSERT OR IGNORE INTO inbox(id, thread, payload) VALUES (?, ?, ?)',
                            (payload['message_id'], thread_id, json.dumps(payload)))

    def thread(self, thread_id: int):
        return self.db.execute('SELECT * FROM threads WHERE id = ?', (thread_id,)).fetchone()

    def metadata(self, key: str, default: str) -> str:
        row = self.db.execute('SELECT value FROM metadata WHERE key = ?', (key,)).fetchone()
        return row[0] if row else default

    def set_metadata(self, key: str, value: str):
        with self.db:
            self.db.execute('INSERT OR REPLACE INTO metadata VALUES (?, ?)', (key, value))

    def queue_reply(self, comment_id: int, thread_id: int, body: str):
        with self.db:
            self.db.execute('INSERT OR IGNORE INTO replies(id, thread, body) VALUES (?, ?, ?)',
                            (comment_id, thread_id, body))
            self.db.execute('UPDATE threads SET reply_cursor = MAX(reply_cursor, ?) WHERE id = ?', (comment_id, thread_id))

    def replies(self):
        return self.db.execute('SELECT * FROM replies WHERE done = 0 AND retry_at <= ? ORDER BY id LIMIT 50',
                               (time.time(),)).fetchall()

    def finish_reply(self, comment_id: int):
        with self.db:
            self.db.execute('UPDATE replies SET done = 1 WHERE id = ?', (comment_id,))

    def retry_reply(self, comment_id: int, attempts: int):
        with self.db:
            self.db.execute('UPDATE replies SET attempts = ?, retry_at = ? WHERE id = ?',
                            (attempts + 1, time.time() + min(900, 5 * 2 ** min(attempts, 8)), comment_id))

    def threads(self):
        return self.db.execute('SELECT * FROM threads').fetchall()

    def issue_thread(self, issue_number: int):
        return self.db.execute('SELECT * FROM threads WHERE issue = ?', (issue_number,)).fetchone()

    def update(self, thread_id: int, **values):
        allowed = {'issue', 'history_cursor', 'reply_cursor', 'acknowledged'}
        if not values or not set(values) <= allowed:
            raise ValueError('Invalid thread state fields')
        with self.db:
            assignments = ', '.join(f'{key} = ?' for key in values)
            self.db.execute(f'UPDATE threads SET {assignments} WHERE id = ?', (*values.values(), thread_id))

    def pending(self):
        return self.db.execute('SELECT * FROM inbox WHERE done = 0 AND retry_at <= ? ORDER BY id LIMIT 50',
                               (time.time(),)).fetchall()

    def finish(self, message_id: int):
        with self.db:
            self.db.execute('UPDATE inbox SET done = 1 WHERE id = ?', (message_id,))

    def retry(self, message_id: int, attempts: int):
        with self.db:
            self.db.execute('UPDATE inbox SET attempts = ?, retry_at = ? WHERE id = ?',
                            (attempts + 1, time.time() + min(900, 5 * 2 ** min(attempts, 8)), message_id))
