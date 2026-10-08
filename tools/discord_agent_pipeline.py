"""Trusted CI preparation and patch validation for Discord investigations."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request

ALLOWED_ROOTS = ('src/waw2bo2/', 'tests/', 'vendor/OpenAssetToolsT6/src/', 'docs/')
ALLOWED_TOOLS = {
    'tools/run_bridge.ps1', 'tools/xaudio_wma_decoder.cpp', 'tools/audit_material_args.py',
    'tools/desktop_entry.py', 'tools/desktop.spec', 'tools/build_desktop.py', 'tools/build_linux.py',
}
REPLY_MARKER = '<!-- discord-agent-reply -->'


def git(*args: str, input: bytes | None = None) -> bytes:
    return subprocess.run(['git', *args], input=input, check=True, capture_output=True).stdout


def validate_patch(patch: bytes) -> list[str]:
    if not patch:
        return []
    if len(patch) > 2 * 1024 * 1024:
        raise ValueError('Patch exceeds 2 MiB')
    # No symlinks, submodules, executable files or binary patches in automated fixes.
    if re.search(rb'^(?:new|old|deleted) (?:file )?mode (?!100644\b)', patch, re.MULTILINE):
        raise ValueError('Patch changes file modes')
    paths = []
    stats = git('apply', '--numstat', '-z', '-', input=patch)
    for record in stats.split(b'\0'):
        if not record:
            continue
        parts = record.split(b'\t', 2)
        if len(parts) != 3 or parts[0] == b'-' or parts[1] == b'-':
            raise ValueError('Binary or renamed paths are not supported')
        path = parts[2].decode('utf-8')
        pure = PurePosixPath(path)
        if not path or pure.is_absolute() or '..' in pure.parts or '\\' in path or ':' in path:
            raise ValueError('Invalid patch path')
        if not (path.startswith(ALLOWED_ROOTS) or path in ALLOWED_TOOLS or path == 'vendor/OpenAssetTools.patch'):
            raise ValueError(f'Automated patch may not modify {path}')
        if path.endswith(('AGENTS.md', 'SKILL.md')) or pure.suffix.lower() not in {'.py', '.ps1', '.cpp', '.h', '.hpp', '.lua', '.md', '.json', '.csv', '.gsc', '.csc', '.spec', '.patch', '.txt'}:
            raise ValueError(f'Unsupported automated patch file: {path}')
        paths.append(path)
    git('apply', '--check', '-', input=patch)
    return paths


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError('Attachment redirects are not allowed')


def screenshot(url: str, target: Path):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.hostname not in {'cdn.discordapp.com', 'media.discordapp.net'} or
        parsed.username or parsed.password or parsed.port not in {None, 443} or not parsed.path.startswith('/attachments/')):
        raise ValueError('Screenshot must use the Discord attachment CDN')
    opener = urllib.request.build_opener(NoRedirect)
    with opener.open(url, timeout=20) as response:
        kind = response.headers.get_content_type()
        if kind not in {'image/png', 'image/jpeg', 'image/webp'}:
            raise ValueError('Unsupported screenshot content type')
        data = response.read(8 * 1024 * 1024 + 1)
        if len(data) > 8 * 1024 * 1024:
            raise ValueError('Screenshot exceeds 8 MiB')
    target.write_bytes(data)


def prepare():
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    issue = event['issue']
    number = int(issue['number'])
    directory = Path('work/discord_input')
    directory.mkdir(parents=True, exist_ok=True)
    comments = subprocess.run(['gh', 'api', '--paginate', f'repos/{os.environ["GITHUB_REPOSITORY"]}/issues/{number}/comments',
        '--jq', '.[] | {body, user: .user.login}'], check=True, capture_output=True, text=True).stdout
    # gh streams compact JSON objects; consume each document without relying on lines.
    decoder, parsed, rest = json.JSONDecoder(), [], comments.strip()
    while rest:
        item, offset = decoder.raw_decode(rest)
        parsed.append(item)
        rest = rest[offset:].lstrip()
    conversation = {'title': issue['title'], 'body': issue['body'], 'comments': parsed[-100:]}
    (directory / 'report.json').write_text(json.dumps(conversation, indent=2), encoding='utf-8')
    blocks = [issue['body'] or '', *(c['body'] or '' for c in parsed[-100:])]
    image_args, missed = [], 0
    for text in reversed(blocks):
        for metadata in re.findall(r'<!-- discord-attachments:(.*?) -->', text):
            try:
                attachments = json.loads(metadata)
            except ValueError:
                continue
            for attachment in attachments:
                if len(image_args) >= 8:
                    break
                if attachment.get('content_type') not in {'image/png', 'image/jpeg', 'image/webp'}:
                    continue
                target = directory / f'screenshot-{len(image_args) // 2}.png'
                try:
                    screenshot(attachment['url'], target)
                except (ValueError, OSError, urllib.error.URLError):
                    missed += 1
                    continue
                image_args.extend(['--image', str(target.resolve())])
    if missed:
        conversation['unavailable_screenshots'] = missed
        (directory / 'report.json').write_text(json.dumps(conversation, indent=2), encoding='utf-8')
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write('sha=' + git('rev-parse', 'HEAD').decode().strip() + '\n')
        output.write('codex_args=' + json.dumps(image_args) + '\n')


def collect(result: Path, destination: Path):
    data = json.loads(result.read_text(encoding='utf-8'))
    if (set(data) != {'status', 'reply', 'summary'} or data['status'] not in {'needs_info', 'diagnosed', 'fix_proposed', 'blocked'} or
        not all(isinstance(data[key], str) and data[key].strip() for key in data)):
        raise ValueError('Invalid agent result')
    if len(data['reply']) > 12000 or len(data['summary']) > 12000:
        raise ValueError('Agent result exceeds reply limit')
    # Include new regression files; never include incoming diagnostic data.
    subprocess.run(['git', 'add', '--intent-to-add', '--', 'src/waw2bo2', 'tests', 'tools',
                    'vendor/OpenAssetToolsT6/src', 'vendor/OpenAssetTools.patch', 'docs'], check=True)
    patch = git('diff', '--binary', 'HEAD', '--')
    # The separate verification job checks paths and applicability in a clean tree.
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'fix.patch').write_bytes(patch)
    (destination / 'result.json').write_text(json.dumps(data), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['prepare', 'collect', 'validate'])
    parser.add_argument('--result', type=Path)
    parser.add_argument('--directory', type=Path, default=Path('work/discord_result'))
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare()
    elif args.command == 'collect':
        if args.result is None:
            parser.error('--result is required for collect')
        collect(args.result, args.directory)
    else:
        paths = validate_patch((args.directory / 'fix.patch').read_bytes())
        if paths:
            data = json.loads((args.directory / 'result.json').read_text(encoding='utf-8'))
            if data['status'] != 'fix_proposed':
                raise ValueError('Agent changed code without proposing a fix')
        print(json.dumps(paths))


if __name__ == '__main__':
    main()
