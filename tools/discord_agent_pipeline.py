"""Trusted CI preparation, patch validation and publication for Discord investigations."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

# The trusted copy outside the agent workspace keeps core.py next to it.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE if (_HERE / 'core.py').is_file() else _HERE.parent / 'services' / 'discord_support'))
from core import (MAX_ATTACHMENT, REPLY_MARKER, attachment_lines, attachment_text,  # noqa: E402
                  decode_messages, redact, with_messages)

ALLOWED_ROOTS = ('src/waw2bo2/', 'tests/', 'vendor/OpenAssetToolsT6/src/', 'docs/')
ALLOWED_TOOLS = {
    'tools/run_bridge.ps1', 'tools/xaudio_wma_decoder.cpp', 'tools/audit_material_args.py',
    'tools/desktop_entry.py', 'tools/desktop.spec', 'tools/build_desktop.py', 'tools/build_linux.py',
}
STATUSES = {'needs_info', 'diagnosed', 'fix_proposed', 'blocked', 'no_reply'}
IMAGE_SUFFIXES = {'.png', '.jpg', '.jpeg', '.webp', '.gif'}
TEXT_SUFFIXES = {'.zip', '.txt', '.log', '.json', '.csv'}
# Discord's CDN rejects urllib's default client name.
USER_AGENT = 'Mozilla/5.0 (compatible; B2W-discord-investigation/1.0; +https://github.com/Ticass/B2W)'
SEEN_RE = re.compile(r'<!-- discord-agent-seen:(\d+) -->')
LEGACY_RE = re.compile(r'<!-- discord-message:(\d+) -->')
ATTACHMENTS_DIR = Path('work/discord_attachments')


def git(*args: str, input: bytes | None = None) -> bytes:
    return subprocess.run(['git', *args], input=input, check=True, capture_output=True).stdout


def gh_api(path: str, method: str = 'GET', payload: dict | None = None):
    command = ['gh', 'api', '-X', method, f'repos/{os.environ["GITHUB_REPOSITORY"]}/{path}']
    if payload is not None:
        command += ['--input', '-']
    output = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                            check=True, capture_output=True, text=True).stdout
    return json.loads(output) if output.strip() else None


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


def fetch_attachment(url: str, target: Path, *, image: bool):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.hostname not in {'cdn.discordapp.com', 'media.discordapp.net'} or
        parsed.username or parsed.password or parsed.port not in {None, 443} or not parsed.path.startswith('/attachments/')):
        raise ValueError('Attachment must use the Discord attachment CDN')
    opener = urllib.request.build_opener(NoRedirect)
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with opener.open(request, timeout=20) as response:
        kind = response.headers.get_content_type()
        if image and kind not in {'image/png', 'image/jpeg', 'image/webp', 'image/gif'}:
            raise ValueError(f'Unsupported screenshot content type {kind}')
        data = response.read(MAX_ATTACHMENT + 1)
        if len(data) > MAX_ATTACHMENT:
            raise ValueError('Attachment exceeds 8 MiB')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def screenshot(url: str, target: Path):
    fetch_attachment(url, target, image=True)


def safe_name(name: str) -> str:
    return re.sub(r'[^A-Za-z0-9._-]', '_', name)[-100:].lstrip('.') or 'attachment'


def comments_of(number: int) -> list[dict]:
    output = subprocess.run(['gh', 'api', '--paginate', f'repos/{os.environ["GITHUB_REPOSITORY"]}/issues/{number}/comments',
        '--jq', '.[] | {body, user: .user.login}'], check=True, capture_output=True, text=True).stdout
    # gh streams compact JSON objects; consume each document without relying on lines.
    decoder, parsed, rest = json.JSONDecoder(), [], output.strip()
    while rest:
        item, offset = decoder.raw_decode(rest)
        parsed.append(item)
        rest = rest[offset:].lstrip()
    return parsed


def conversation(body: str, comments: list[dict]) -> list[dict]:
    """Reporter messages: the issue's hidden data block, plus messages that
    older issues received as comments."""
    messages = decode_messages(body)
    known = {str(m.get('message_id')) for m in messages}
    for comment in comments:
        text = comment.get('body') or ''
        match = LEGACY_RE.search(text)
        if not match or match.group(1) in known:
            continue
        attachments = []
        for metadata in re.findall(r'<!-- discord-attachments:(.*?) -->', text):
            try:
                attachments = json.loads(metadata)
            except ValueError:
                pass
        content = re.sub(r'<!--.*?-->', '', text, flags=re.DOTALL).strip()
        messages.append({'message_id': int(match.group(1)), 'content': content, 'attachments': attachments})
    return sorted(messages, key=lambda m: int(m.get('message_id') or 0))


def last_message_id(messages: list[dict]) -> int:
    return max((int(m.get('message_id') or 0) for m in messages), default=0)


def prepare():
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    number = int(event['issue']['number'])
    # A reporter often sends several messages in a row; answer them together.
    time.sleep(int(os.environ.get('DEBOUNCE_SECONDS', '90')))
    issue = gh_api(f'issues/{number}')
    body = issue.get('body') or ''
    comments = comments_of(number)
    messages = conversation(body, comments)
    last = last_message_id(messages)
    seen = max((int(value) for value in SEEN_RE.findall(body)), default=0)
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write('sha=' + git('rev-parse', 'HEAD').decode().strip() + '\n')
        output.write(f'last_message={last}\n')
        if last and last <= seen:
            # Already answered: an edit without a new message, or a burst handled by an earlier run.
            output.write('skip=true\n')
            print(f'Messages up to {last} were already answered; nothing to investigate.')
            return
        output.write('skip=false\n')

    directory = Path('work/discord_input')
    directory.mkdir(parents=True, exist_ok=True)
    image_args, unavailable, manifest = [], [], []
    for message in messages:
        for attachment in message.get('attachments') or []:
            name = str(attachment.get('name') or 'attachment')
            suffix = Path(name).suffix.lower()
            if suffix not in IMAGE_SUFFIXES | TEXT_SUFFIXES or not attachment.get('url') or attachment.get('stored_url'):
                continue
            relative = f"{message.get('message_id')}/{safe_name(name)}"
            target = ATTACHMENTS_DIR / relative
            try:
                fetch_attachment(attachment['url'], target, image=suffix in IMAGE_SUFFIXES)
            except (ValueError, OSError, urllib.error.URLError) as error:
                unavailable.append({'name': name, 'reason': redact(str(error))[:200]})
                print(f'::warning::Attachment {name} unavailable: {error}')
                continue
            manifest.append({'message_id': message.get('message_id'), 'name': name,
                             'url': attachment['url'], 'path': relative})
            if suffix in IMAGE_SUFFIXES and len(image_args) < 16:
                image_args.extend(['--image', str(target.resolve())])
            elif suffix in TEXT_SUFFIXES:
                text = attachment_text(name, target.read_bytes())
                (directory / 'diagnostics').mkdir(exist_ok=True)
                (directory / 'diagnostics' / f'{safe_name(relative)}.txt').write_text(text, encoding='utf-8')
    ATTACHMENTS_DIR.mkdir(parents=True, exist_ok=True)
    (ATTACHMENTS_DIR / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    report = {
        'title': issue['title'],
        'issue_body': re.sub(r'<!--.*?-->', '', body, flags=re.DOTALL).strip(),
        'messages': [{key: m.get(key) for key in ('message_id', 'content', 'attachments', 'facts')}
                     for m in messages],
        'agent_replies': [re.sub(r'<!--.*?-->', '', c['body'] or '', flags=re.DOTALL).strip()
                          for c in comments if (c.get('body') or '').startswith(REPLY_MARKER)][-20:],
        'maintainer_comments': [c['body'] for c in comments
                                if not (c.get('body') or '').startswith(('<!--', REPLY_MARKER))][-20:],
        'diagnostic_files': sorted(p.name for p in (directory / 'diagnostics').glob('*.txt')),
        'unavailable_attachments': unavailable,
    }
    (directory / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write('codex_args=' + json.dumps(image_args) + '\n')


def store_attachments(number: int, root: Path = ATTACHMENTS_DIR):
    """Keep the report's attachments in the repository (Discord links expire),
    on refs/discord-attachments/issue-N: no branch, so no build and no clone cost."""
    manifest_path = root / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.is_file() else []
    if not manifest:
        print('No attachments to store.')
        return
    ref = f'refs/discord-attachments/issue-{number}'
    with tempfile.TemporaryDirectory() as temp:
        env = {**os.environ, 'GIT_INDEX_FILE': str(Path(temp) / 'index')}
        def run(*args: str) -> str:
            return subprocess.run(['git', *args], check=True, capture_output=True, text=True, env=env).stdout.strip()
        parent = None
        if subprocess.run(['git', 'fetch', '--no-tags', 'origin', f'+{ref}:{ref}'], capture_output=True).returncode == 0:
            parent = run('rev-parse', ref)
            run('read-tree', parent)
        for item in manifest:
            blob = run('hash-object', '-w', str(root / item['path']))
            run('update-index', '--add', '--cacheinfo', f"100644,{blob},issue-{number}/{item['path']}")
        tree = run('write-tree')
        if parent and run('rev-parse', f'{parent}^{{tree}}') == tree:
            commit = parent
        else:
            commit = run('commit-tree', tree, *(['-p', parent] if parent else []),
                         '-m', f'Discord report attachments for issue #{number}')
            run('push', 'origin', f'{commit}:{ref}')
    repository = os.environ['GITHUB_REPOSITORY']
    stored = {(str(item['message_id']), item['name']):
              f"https://github.com/{repository}/raw/{commit}/issue-{number}/{urllib.parse.quote(item['path'])}"
              for item in manifest}
    issue = gh_api(f'issues/{number}')
    body = issue.get('body') or ''
    messages = decode_messages(body)
    for message in messages:
        for attachment in message.get('attachments') or []:
            url = stored.get((str(message.get('message_id')), attachment.get('name')))
            if url:
                body = body.replace(f"]({attachment['url']})", f']({url})')
                attachment['stored_url'] = url
    gh_api(f'issues/{number}', 'PATCH', {'body': with_messages(body, messages) if messages else body})
    print(f'Stored {len(manifest)} attachment(s) at {ref} ({commit[:7]}).')


def publish(number: int, seen: int, directory: Path):
    """Replace the issue description with the investigation's bug report and
    post its reply, unless the reporter wrote again meanwhile (the queued run
    answers everything together)."""
    result = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
    issue = gh_api(f'issues/{number}')
    body = issue.get('body') or ''
    messages = conversation(body, comments_of(number))
    if last_message_id(messages) > seen:
        print('Newer reporter messages arrived during the investigation; the queued run will answer them.')
        return
    head = ''.join(f'{m}\n' for m in re.findall(r'<!-- discord-thread:\d+ -->', body)[:1])
    head += f'<!-- discord-agent-seen:{seen} -->\n'
    url = next((m.get('url') for m in messages if m.get('url')), '')
    source = f'[Discord thread]({url})' if url else 'Discord'
    report = (f"{head}{result['issue_body'].strip()}\n\n### Attachments\n\n"
              f"{chr(10).join(attachment_lines(messages)) or 'None.'}\n\n"
              f'---\n<sub>Reported on {source}. Written by the automated investigation from the '
              'report and its attachments.</sub>')
    update = {'title': result['issue_title'].strip()[:240],
              'body': with_messages(report, messages) if decode_messages(body) else report}
    gh_api(f'issues/{number}', 'PATCH', update)
    if result['status'] == 'no_reply':
        print('The investigation had nothing new to tell the reporter.')
        return
    reply = REPLY_MARKER + '\n' + result['reply'].strip()
    pr = directory / 'pr-url.txt'
    if pr.is_file():
        reply += ('\n\nProposed fix (draft PR): ' + pr.read_text(encoding='utf-8').strip() +
                  '\nWindows/Linux build checks are queued. This fix is awaiting review and has not been released.')
    gh_api(f'issues/{number}/comments', 'POST', {'body': reply[:15000]})


def collect(result: Path, destination: Path):
    data = json.loads(result.read_text(encoding='utf-8'))
    keys = {'status', 'reply', 'summary', 'issue_title', 'issue_body'}
    if set(data) != keys or data['status'] not in STATUSES or not all(isinstance(data[key], str) for key in keys):
        raise ValueError('Invalid agent result')
    required = keys - ({'reply'} if data['status'] == 'no_reply' else set())
    if not all(data[key].strip() for key in required):
        raise ValueError('Agent result is missing required text')
    if (len(data['reply']) > 12000 or len(data['summary']) > 12000 or len(data['issue_title']) > 200
            or len(data['issue_body']) > 20000 or '<!--' in data['issue_body']):
        raise ValueError('Agent result exceeds its limits')
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
    parser.add_argument('command', choices=['prepare', 'collect', 'validate', 'store-attachments', 'publish'])
    parser.add_argument('--result', type=Path)
    parser.add_argument('--directory', type=Path, default=Path('work/discord_result'))
    parser.add_argument('--issue', type=int)
    parser.add_argument('--seen', type=int)
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare()
    elif args.command == 'collect':
        if args.result is None:
            parser.error('--result is required for collect')
        collect(args.result, args.directory)
    elif args.command == 'store-attachments':
        store_attachments(args.issue)
    elif args.command == 'publish':
        publish(args.issue, args.seen, args.directory)
    else:
        paths = validate_patch((args.directory / 'fix.patch').read_bytes())
        if paths:
            data = json.loads((args.directory / 'result.json').read_text(encoding='utf-8'))
            if data['status'] != 'fix_proposed':
                raise ValueError('Agent changed code without proposing a fix')
        print(json.dumps(paths))


if __name__ == '__main__':
    main()
