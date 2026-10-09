"""Turn a nightly snapshot into the next stable version.

Used by the Saturday stable promotion workflow:

    python tools/promote_stable.py next-version v0.2.14 v0.2.9 ...
    python tools/promote_stable.py release VERSION [--fallback-notes FILE]
    python tools/promote_stable.py sync-main VERSION RELEASED_CHANGELOG

``release`` runs in the nightly's checkout: it sets the version and turns the
``Unreleased`` changelog section into the version's section; when that section
is missing or empty, the lines of ``--fallback-notes`` (the commit subjects
since the previous stable) are used instead. ``sync-main`` runs
in a ``main`` checkout: it sets the version and moves only the entries that the
release shipped under the version heading, keeping later work in ``Unreleased``.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILES = {
    'pyproject.toml': re.compile(r'^(version\s*=\s*")[^"]+(")', re.M),
    'src/waw2bo2/__init__.py': re.compile(r'^(__version__\s*=\s*")[^"]+(")', re.M),
}
TAG = re.compile(r'^v(\d+)\.(\d+)\.(\d+)$')
UNRELEASED = '## Unreleased'


def next_version(tags: list[str]) -> str:
    versions = [tuple(map(int, m.groups())) for m in map(TAG.match, tags) if m]
    if not versions:
        raise ValueError('No vMAJOR.MINOR.PATCH tag exists to increment')
    major, minor, patch = max(versions)
    return f'{major}.{minor}.{patch + 1}'


def set_version(root: Path, version: str) -> None:
    for name, pattern in VERSION_FILES.items():
        path = root / name
        text = path.read_text(encoding='utf-8')
        updated, count = pattern.subn(rf'\g<1>{version}\g<2>', text, count=1)
        if count != 1:
            raise ValueError(f'No version assignment in {name}')
        path.write_text(updated, encoding='utf-8', newline='\n')


def _split_unreleased(text: str) -> tuple[str, list[str], str]:
    """Return (text before the section, its body lines, text after it)."""
    start = text.find(UNRELEASED + '\n')
    if start < 0:
        raise ValueError(f'CHANGELOG.md has no "{UNRELEASED}" section')
    body_start = start + len(UNRELEASED) + 1
    following = re.search(r'^## ', text[body_start:], re.M)
    body_end = body_start + following.start() if following else len(text)
    return text[:start], text[body_start:body_end].strip('\n').splitlines(), text[body_end:]


def _section(heading: str, lines: list[str]) -> str:
    body = '\n'.join(lines).strip('\n')
    return f'{heading}\n\n{body}\n\n' if body else f'{heading}\n\n'


def release_changelog(text: str, version: str, fallback: list[str] | None = None) -> str:
    if UNRELEASED + '\n' not in text:
        first = re.search(r'^## ', text, re.M)
        at = first.start() if first else len(text)
        text = text[:at].rstrip('\n') + '\n\n' + _section(UNRELEASED, []) + text[at:]
    before, lines, after = _split_unreleased(text)
    if not any(line.strip() for line in lines):
        lines = [f'- {line.strip()}' for line in fallback or [] if line.strip()]
        lines = lines or ['- Maintenance release; no user-facing changes were recorded.']
    return before + _section(UNRELEASED, []) + _section(f'## v{version}', lines) + after


def released_entries(text: str, version: str) -> list[str]:
    heading = f'## v{version}\n'
    start = text.find(heading)
    if start < 0:
        raise ValueError(f'Released changelog has no "{heading.strip()}" section')
    body = text[start + len(heading):]
    following = re.search(r'^## ', body, re.M)
    return body[:following.start() if following else len(body)].strip('\n').splitlines()


def sync_changelog(text: str, version: str, shipped: list[str]) -> str:
    if f'## v{version}\n' in text:
        return text
    before, lines, after = _split_unreleased(text)
    shipped_set = {line for line in shipped if line.strip()}
    pending = [line for line in lines if line not in shipped_set]
    return before + _section(UNRELEASED, pending) + _section(f'## v{version}', shipped) + after


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('next-version').add_argument('tags', nargs='*')
    release = sub.add_parser('release')
    release.add_argument('version')
    release.add_argument('--fallback-notes', type=Path)
    sync = sub.add_parser('sync-main')
    sync.add_argument('version')
    sync.add_argument('released_changelog', type=Path)
    for command in (release, sync):
        command.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args(argv)

    if args.command == 'next-version':
        print(next_version(args.tags))
        return 0
    changelog = args.root / 'CHANGELOG.md'
    text = changelog.read_text(encoding='utf-8')
    if args.command == 'release':
        fallback = args.fallback_notes.read_text(encoding='utf-8').splitlines() if args.fallback_notes else None
        text = release_changelog(text, args.version, fallback)
    else:
        shipped = released_entries(args.released_changelog.read_text(encoding='utf-8'), args.version)
        text = sync_changelog(text, args.version, shipped)
    set_version(args.root, args.version)
    changelog.write_text(text, encoding='utf-8', newline='\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
