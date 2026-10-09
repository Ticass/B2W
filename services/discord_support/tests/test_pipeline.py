from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('discord_agent_pipeline', ROOT / 'tools/discord_agent_pipeline.py')
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)
from core import REPLY_MARKER, decode_messages, encode_messages  # noqa: E402  (path set by the pipeline)


class PatchBoundaryTests(unittest.TestCase):
    def test_rejects_binary_and_protected_files(self):
        for stat in (b'-\t-\tsrc/waw2bo2/data.bin\0', b'1\t0\t.github/workflows/ci.yml\0',
                     b'1\t0\tsrc/waw2bo2/AGENTS.md\0', b'1\t0\ttools/discord_agent_pipeline.py\0',
                     b'1\t0\t../outside.py\0'):
            with self.subTest(stat=stat), patch.object(pipeline, 'git', return_value=stat):
                with self.assertRaises(ValueError):
                    pipeline.validate_patch(b'diff --git a/file b/file\n')

    def test_rejects_symlinks(self):
        with self.assertRaises(ValueError):
            pipeline.validate_patch(b'diff --git a/src/waw2bo2/a.py b/src/waw2bo2/a.py\nnew file mode 120000\n')

    def test_validates_a_real_patch_against_a_clean_git_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def git(*args, input=None):
                return subprocess.run(['git', '-C', str(root), *args], input=input,
                    check=True, capture_output=True).stdout
            git('init')
            # Like the Actions checkout: no configured identity, and none guessed.
            git('config', 'user.useConfigOnly', 'true')
            source = root / 'src/waw2bo2/example.py'
            source.parent.mkdir(parents=True)
            source.write_text('value = 1\n')
            git('add', '.')
            git('commit', '-m', 'fixture')
            source.write_text('value = 2\n')
            fix = git('diff', 'HEAD')
            git('restore', '.')
            with patch.object(pipeline, 'git', side_effect=git):
                self.assertEqual(pipeline.validate_patch(fix), ['src/waw2bo2/example.py'])

    def test_screenshot_fetch_rejects_non_discord_urls_before_network(self):
        for url in ('http://cdn.discordapp.com/attachments/1/a.png', 'https://127.0.0.1/a.png',
                    'https://cdn.discordapp.com.evil.invalid/attachments/1/a.png',
                    'https://secret@cdn.discordapp.com/attachments/1/a.png'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                pipeline.screenshot(url, Path('unused.png'))


def message(message_id, content='text', **changes):
    return {'message_id': message_id, 'content': content, 'url': f'https://discord.com/channels/1/2/{message_id}',
            'attachments': [], 'facts': {}, **changes}


def result(**changes):
    return {'status': 'needs_info', 'reply': 'Please send the diagnostics ZIP.', 'summary': 'Missing logs.',
            'issue_title': 'Error when building Bank Job (zm_bankjob)',
            'issue_body': '### Summary\nScript compile fails.', **changes}


class FakeGitHub:
    """Issue body and comments behind gh_api/comments_of."""
    def __init__(self, body, comments=()):
        self.body, self.comments, self.calls = body, list(comments), []

    def api(self, path, method='GET', payload=None):
        self.calls.append((method, path, payload))
        if method == 'PATCH':
            self.body = payload.get('body', self.body)
        return {'title': 'Error', 'body': self.body}


class ConversationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.output = self.root / 'output.txt'
        event = self.root / 'event.json'
        event.write_text('{"issue": {"number": 12}}')
        self.env = patch.dict(os.environ, {'GITHUB_EVENT_PATH': str(event), 'GITHUB_OUTPUT': str(self.output),
                                           'GITHUB_REPOSITORY': 'owner/repo', 'DEBOUNCE_SECONDS': '0'})
        self.env.start()
        self.cwd = os.getcwd()
        os.chdir(self.root)

    def tearDown(self):
        os.chdir(self.cwd)
        self.env.stop()
        self.temp.cleanup()

    def run_with(self, github, function, *args):
        with patch.object(pipeline, 'gh_api', github.api), \
             patch.object(pipeline, 'comments_of', lambda number: github.comments), \
             patch.object(pipeline, 'git', return_value=b'abc\n'):
            return function(*args)

    def outputs(self):
        return dict(line.split('=', 1) for line in self.output.read_text().splitlines())

    def test_already_answered_messages_are_not_investigated_again(self):
        body = '<!-- discord-thread:1 -->\n<!-- discord-agent-seen:101 -->\n' + encode_messages(
            [message(100), message(101)])
        self.run_with(FakeGitHub(body), pipeline.prepare)
        self.assertEqual(self.outputs()['skip'], 'true')
        self.assertFalse((self.root / 'work/discord_input/report.json').exists())

    def test_new_message_is_investigated_with_unavailable_attachments_explained(self):
        attachment = {'name': 'shot.png', 'url': 'https://cdn.discordapp.com/attachments/1/2/shot.png',
                      'content_type': 'image/png'}
        body = ('<!-- discord-agent-seen:100 -->\n' +
                encode_messages([message(100), message(102, 'WAIT', attachments=[attachment])]))
        github = FakeGitHub(body, [{'body': '<!-- discord-agent-reply -->\nSend the ZIP.', 'user': 'bot'}])
        def unavailable(url, target, image):
            raise urllib.error.HTTPError(url, 403, 'Forbidden', None, None)
        with patch.object(pipeline, 'fetch_attachment', unavailable):
            self.run_with(github, pipeline.prepare)
        self.assertEqual(self.outputs()['skip'], 'false')
        self.assertEqual(self.outputs()['last_message'], '102')
        report = json.loads((self.root / 'work/discord_input/report.json').read_text())
        self.assertEqual(report['agent_replies'], ['Send the ZIP.'])
        self.assertEqual([m['message_id'] for m in report['messages']], [100, 102])
        self.assertIn('403', report['unavailable_attachments'][0]['reason'])

    def test_downloads_feed_the_agent_images_and_diagnostic_text(self):
        attachments = [{'name': 'shot.png', 'url': 'https://cdn.discordapp.com/attachments/1/2/shot.png'},
                       {'name': 'build.log', 'url': 'https://cdn.discordapp.com/attachments/1/2/build.log'}]
        def fetch(url, target, image):
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'png' if image else b'waw2bo2: error: StageError: boom')
        with patch.object(pipeline, 'fetch_attachment', fetch):
            self.run_with(FakeGitHub(encode_messages([message(5, attachments=attachments)])), pipeline.prepare)
        self.assertIn('--image', self.outputs()['codex_args'])
        diagnostics = list((self.root / 'work/discord_input/diagnostics').glob('*.txt'))
        self.assertIn('StageError: boom', diagnostics[0].read_text())
        manifest = json.loads((self.root / 'work/discord_attachments/manifest.json').read_text())
        self.assertEqual([item['path'] for item in manifest], ['5/shot.png', '5/build.log'])

    def write_result(self, **changes):
        directory = self.root / 'result'
        directory.mkdir(exist_ok=True)
        (directory / 'result.json').write_text(json.dumps(result(**changes)))
        return directory

    def test_publish_skips_when_the_reporter_wrote_again(self):
        github = FakeGitHub(encode_messages([message(100), message(103)]))
        self.run_with(github, pipeline.publish, 12, 100, self.write_result())
        self.assertEqual([c[0] for c in github.calls], ['GET'])

    def test_publish_writes_the_bug_report_and_one_reply(self):
        attachment = {'name': 'shot.png', 'url': 'https://cdn.discordapp.com/attachments/1/2/shot.png',
                      'stored_url': 'https://github.com/owner/repo/raw/abc/issue-12/100/shot.png',
                      'content_type': 'image/png'}
        github = FakeGitHub('<!-- discord-thread:9 -->\n<!-- discord-report-draft -->\ndraft\n\n' +
                            encode_messages([message(100, attachments=[attachment])]))
        self.run_with(github, pipeline.publish, 12, 100, self.write_result())
        patch_call = next(c for c in github.calls if c[0] == 'PATCH')
        body = patch_call[2]['body']
        self.assertEqual(patch_call[2]['title'], 'Error when building Bank Job (zm_bankjob)')
        self.assertTrue(body.startswith('<!-- discord-thread:9 -->\n<!-- discord-agent-seen:100 -->\n### Summary'))
        self.assertNotIn('discord-report-draft', body)
        self.assertIn('![shot.png](https://github.com/owner/repo/raw/abc/issue-12/100/shot.png)', body)
        self.assertEqual([m['message_id'] for m in decode_messages(body)], [100])
        posts = [c for c in github.calls if c[0] == 'POST']
        self.assertEqual(len(posts), 1)
        self.assertTrue(posts[0][2]['body'].startswith(REPLY_MARKER + '\nPlease send'))

    def test_no_reply_updates_the_report_without_messaging_discord(self):
        github = FakeGitHub(encode_messages([message(100)]))
        self.run_with(github, pipeline.publish, 12, 100, self.write_result(status='no_reply', reply=''))
        self.assertEqual([c[0] for c in github.calls], ['GET', 'PATCH'])

    def test_collect_requires_a_bug_report(self):
        output = self.root / 'agent.json'
        for changes in ({'issue_title': ''}, {'issue_body': 'x <!-- hidden -->'}, {'reply': ''},
                        {'status': 'other'}):
            output.write_text(json.dumps(result(**changes)))
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                pipeline.collect(output, self.root / 'collected')


class AttachmentStorageTests(unittest.TestCase):
    def test_attachments_are_kept_on_a_hidden_ref_and_linked_from_the_issue(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def git(*args, cwd=root / 'work'):
                return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout
            subprocess.run(['git', 'init', '--bare', str(root / 'remote.git')], check=True, capture_output=True)
            subprocess.run(['git', 'init', str(root / 'work')], check=True, capture_output=True)
            # Like the Actions checkout: no configured identity, and none guessed.
            git('config', 'user.useConfigOnly', 'true')
            git('remote', 'add', 'origin', str(root / 'remote.git'))
            files = root / 'work/work/discord_attachments'
            (files / '100').mkdir(parents=True)
            (files / '100/shot.png').write_bytes(b'image')
            url = 'https://cdn.discordapp.com/attachments/1/2/shot.png?ex=1'
            (files / 'manifest.json').write_text(json.dumps(
                [{'message_id': 100, 'name': 'shot.png', 'url': url, 'path': '100/shot.png'}]))
            github = FakeGitHub(f'### Attachments\n- ![shot.png]({url})\n\n' + encode_messages(
                [message(100, attachments=[{'name': 'shot.png', 'url': url}])]))
            cwd = os.getcwd()
            os.chdir(root / 'work')
            try:
                isolated = {'GITHUB_REPOSITORY': 'owner/repo', 'GIT_CONFIG_NOSYSTEM': '1',
                            'GIT_CONFIG_GLOBAL': str(root / 'no-global-config')}
                with patch.dict(os.environ, isolated), \
                     patch.object(pipeline, 'gh_api', github.api):
                    pipeline.store_attachments(12, Path('work/discord_attachments'))
                    pipeline.store_attachments(12, Path('work/discord_attachments'))
            finally:
                os.chdir(cwd)
            commit = git('rev-parse', 'refs/discord-attachments/issue-12', cwd=root / 'remote.git').strip()
            self.assertEqual(git('show', f'{commit}:issue-12/100/shot.png', cwd=root / 'remote.git'), 'image')
            self.assertEqual(git('branch', '--list', cwd=root / 'remote.git'), '')
            stored = f'https://github.com/owner/repo/raw/{commit}/issue-12/100/shot.png'
            self.assertIn(f'![shot.png]({stored})', github.body)
            self.assertEqual(decode_messages(github.body)[0]['attachments'][0]['stored_url'], stored)
            # The rerun added no second commit.
            self.assertEqual(git('rev-list', '--count', commit, cwd=root / 'remote.git').strip(), '1')

            self.assertEqual(git('log', '-1', '--format=%an', commit, cwd=root / 'remote.git').strip(),
                             'github-actions[bot]')


class VerifyTestsTests(unittest.TestCase):
    def run_suite(self, root: Path, body: str, **kwargs):
        (root / 'tests').mkdir(exist_ok=True)
        (root / 'tests/test_sample.py').write_text(
            'import unittest\nclass T(unittest.TestCase):\n' + body)
        cwd = os.getcwd()
        os.chdir(root)
        try:
            # Each run stands for a different tree: import the suite afresh.
            with patch.dict('sys.modules'), patch('sys.stderr'):
                pipeline.verify_tests(**kwargs)
        finally:
            os.chdir(cwd)

    def test_only_failures_absent_from_the_unpatched_baseline_fail_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            baseline = root / 'baseline.json'
            environment = '    def test_environment(self): raise OSError("no d3dcompiler")\n'
            self.run_suite(root, environment + '    def test_ok(self): pass\n',
                           output=baseline, baseline=None)
            self.assertEqual(json.loads(baseline.read_text()), ['test_sample.T.test_environment'])
            # The same environment-only failure passes verification...
            self.run_suite(root, environment + '    def test_new(self): pass\n', output=None, baseline=baseline)
            # ...a failure the patch introduced does not.
            with self.assertRaises(SystemExit) as raised:
                self.run_suite(root, environment + '    def test_ok(self): self.fail()\n',
                               output=None, baseline=baseline)
            self.assertIn('test_sample.T.test_ok', str(raised.exception))


if __name__ == '__main__':
    unittest.main()
