"""Durable intake and bounded diagnostics parsing; no Discord connection here."""
from __future__ import annotations

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


def format_report(payload: dict) -> str:
    metadata = json.dumps(payload.get('attachments', []), ensure_ascii=True)
    content = redact(payload['content'])[:MAX_REPORT]
    return (f"{message_marker(payload['message_id'])}\n"
            f"Discord report from {payload['author']} ({payload['author_id']})\n"
            f"Thread: {payload['url']}\n\n{content}\n\n"
            f'<!-- discord-attachments:{metadata} -->')[:60000]


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
