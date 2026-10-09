from __future__ import annotations

import asyncio
import io
import json
from pathlib import Path
import sys
import tempfile
from types import MethodType, SimpleNamespace
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from core import (DRAFT_MARKER, State, attachment_text, chunks, decode_messages, diagnostics_facts,
    encode_messages, format_issue, redact, reply_text, report_title, with_messages)
from bot import GitHub, SupportBot, discord_login_retry_delay


class IntakeTests(unittest.TestCase):
    def test_discord_login_rate_limit_uses_capped_backoff(self):
        self.assertEqual([discord_login_retry_delay(i) for i in range(1, 7)],
                         [60, 120, 240, 480, 960, 1800])
        self.assertEqual(discord_login_retry_delay(20), 1800)

    def test_zip_reads_logs_but_not_game_assets_or_traversal(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('console.log', 'StageError: unsupported sky shader')
            archive.writestr('summary.json', '{"version":"0.2.14"}')
            archive.writestr('../outside.log', 'must not read this')
            archive.writestr('map.ff', 'private game content')
            archive.writestr('run.ps1', 'do not execute')
            archive.writestr('large.log', 'x' * (3 * 1024 * 1024))
        text = attachment_text('failure.zip', buffer.getvalue())
        self.assertIn('StageError', text)
        self.assertIn('0.2.14', text)
        self.assertNotIn('must not read this', text)
        self.assertNotIn('private game content', text)
        self.assertNotIn('do not execute', text)
        self.assertIn('skipped oversized', text)

    def test_corrupt_zip_is_actionable(self):
        self.assertIn('fresh Save Diagnostics ZIP', attachment_text('bad.zip', b'not a zip'))

    def test_redacts_common_secrets_and_user_paths(self):
        text = redact('api_key=secret password:secret C:\\Users\\alice\\work /home/bob/file')
        self.assertNotIn('secret', text)
        self.assertNotIn('alice', text)
        self.assertNotIn('/home/bob', text)

    def test_chunks_fit_discord_messages(self):
        self.assertTrue(all(len(part) <= 1900 for part in chunks('x' * 4100)))


def diagnostics_zip(**settings):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('summary.json', json.dumps({
            'version': '0.2.14', 'platform': 'Linux-7.2.8-x86_64', 'error': '',
            'settings': {'project': 'zm_bankjob', 'menu_title': 'Bank Job', 'bo2_stock_perks': True,
                         'source_fx': True, 'work': '/home/humpy/builds', **settings}}))
        archive.writestr('console.log', '\n'.join([
            'warning: APPROXIMATED_SOUND_CURVE rcurve2 (max error 0.0628)',
            "waw2bo2: error: StageError: script compilation failed: [] "
            "missing=['clientscripts/mp/waw/_waw2bo2_zm.csc']; see /home/humpy/stage/linker.log",
            'waw2bo2: error: RuntimeError: WawConverter.CLI.exe failed (1)']))
    return buffer.getvalue()


def payload(message_id, content, **changes):
    return {'message_id': message_id, 'content': content, 'author': 'reporter', 'author_id': 8,
            'url': f'https://discord.com/channels/1/2/{message_id}', 'attachments': [], 'facts': {}, **changes}


class BugReportTests(unittest.TestCase):
    def test_diagnostics_zip_gives_map_environment_and_error_lines(self):
        facts = diagnostics_facts(diagnostics_zip())
        self.assertEqual(facts['project'], 'zm_bankjob')
        self.assertEqual(facts['map'], 'Bank Job')
        self.assertEqual(facts['version'], '0.2.14')
        self.assertEqual(facts['options'], ['BO2 stock perks', 'WaW source FX'])
        self.assertEqual(len(facts['errors']), 2)
        self.assertIn('_waw2bo2_zm.csc', facts['errors'][0])
        self.assertNotIn('humpy', json.dumps(facts))
        self.assertNotIn('APPROXIMATED_SOUND_CURVE', json.dumps(facts))
        self.assertEqual(diagnostics_facts(b'not a zip'), {})

    def test_issue_is_a_bug_report_not_a_chat_transcript(self):
        attachments = [{'name': 'shot.png', 'url': 'https://cdn.discordapp.com/attachments/1/2/shot.png',
                        'content_type': 'image/png', 'size': 1000},
                       {'name': 'failure.zip', 'url': 'https://cdn.discordapp.com/attachments/1/2/failure.zip',
                        'content_type': 'application/zip', 'size': 3 * 1048576}]
        messages = [payload(42, 'Build fails at the script step', attachments=attachments,
                            facts=diagnostics_facts(diagnostics_zip()))]
        self.assertEqual(report_title('Error', messages), 'Error when building Bank Job (zm_bankjob)')
        body = format_issue('Error', messages)
        self.assertTrue(body.startswith(DRAFT_MARKER))
        for heading in ('### Error', '### Environment', '### Description', '### Attachments'):
            self.assertIn(heading, body)
        self.assertIn("missing=['clientscripts/mp/waw/_waw2bo2_zm.csc']", body)
        self.assertIn('- Map: Bank Job (`zm_bankjob`)', body)
        self.assertIn('> Build fails at the script step', body)
        self.assertIn('- ![shot.png](https://cdn.discordapp.com/attachments/1/2/shot.png)', body)
        self.assertIn('- [failure.zip](https://cdn.discordapp.com/attachments/1/2/failure.zip) (3.0 MiB)', body)
        self.assertNotIn('Discord report from', body)
        self.assertNotIn('reporter', body)
        self.assertNotIn('(8)', body)

    def test_report_without_diagnostics_is_titled_from_the_thread(self):
        messages = [payload(1, 'it crashes')]
        self.assertEqual(report_title('Crash on load', messages), 'Error report: Crash on load')
        self.assertIn('Save Diagnostics', format_issue('Crash on load', messages))

    def test_hidden_report_data_round_trips_and_stays_bounded(self):
        messages = [payload(i, 'x' * 10000 + '-->') for i in range(1, 40)]
        block = encode_messages(messages)
        self.assertLess(len(block), 41000)
        self.assertEqual(block.count('-->'), 1)
        decoded = decode_messages('report\n\n' + block)
        self.assertEqual(decoded[0]['message_id'], 1)
        self.assertEqual(decoded[-1]['message_id'], 39)
        self.assertLessEqual(len(decoded[0]['content']), 4000)
        body = with_messages('visible\n\n' + block, [payload(5, 'new')])
        self.assertTrue(body.startswith('visible'))
        self.assertEqual([m['message_id'] for m in decode_messages(body)], [5])
        self.assertEqual(decode_messages('no data'), [])

    def test_reply_text_drops_hidden_markers(self):
        self.assertEqual(reply_text('<!-- discord-agent-reply -->\nFound it.\n<!-- discord-agent-seen:5 -->'),
                         'Found it.')


class DurableStateTests(unittest.TestCase):
    def test_restart_preserves_reports_and_duplicate_gateway_events(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'support.sqlite3'
            state = State(path)
            payload = {'message_id': 10, 'content': 'error'}
            state.enqueue(3, 'Report', payload)
            state.enqueue(3, 'Report', payload)
            state.update(3, issue=123, history_cursor=10)
            state.db.close()
            restarted = State(path)
            self.assertEqual(len(restarted.pending()), 1)
            self.assertEqual(restarted.thread(3)['issue'], 123)
            self.assertEqual(restarted.thread(3)['history_cursor'], 10)
            restarted.finish(10)
            self.assertEqual(restarted.pending(), [])
            restarted.db.close()

    def test_reply_outbox_survives_restart_and_deduplicates(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'support.sqlite3'
            state = State(path)
            state.enqueue(3, 'Report', {'message_id': 10})
            state.queue_reply(101, 3, 'Can you send a screenshot?')
            state.queue_reply(101, 3, 'Can you send a screenshot?')
            state.db.close()
            restarted = State(path)
            self.assertEqual(len(restarted.replies()), 1)
            self.assertEqual(restarted.thread(3)['reply_cursor'], 101)
            restarted.finish_reply(101)
            self.assertEqual(restarted.replies(), [])
            restarted.db.close()

    def test_retry_keeps_pending_report_and_state_fields_are_restricted(self):
        with tempfile.TemporaryDirectory() as temp:
            state = State(Path(temp) / 'support.sqlite3')
            state.enqueue(1, 'Report', {'message_id': 2})
            state.retry(2, 0)
            self.assertEqual(state.pending(), [])
            self.assertEqual(state.db.execute('SELECT done FROM inbox').fetchone()[0], 0)
            with self.assertRaises(ValueError):
                state.update(1, unexpected=123)
            self.assertEqual(state.cutoff(100), state.cutoff(200))
            state.db.close()


class GitHubRecoveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_recent_reply_includes_an_older_original_report(self):
        with tempfile.TemporaryDirectory() as temp:
            state = State(Path(temp) / 'support.sqlite3')
            thread = SimpleNamespace(id=10, name='Old report')
            author = SimpleNamespace(id=9, bot=False)
            starter = SimpleNamespace(id=10, channel=thread, author=author, webhook_id=None,
                content='Original StageError', attachments=[], jump_url='https://discord.com/channels/1/10/10')
            async def fetch(message_id):
                return starter
            thread.fetch_message = fetch
            listener = SimpleNamespace(state=state, cutoff=50, in_scope=lambda channel: True)
            listener.ingest = MethodType(SupportBot.ingest, listener)
            reply = SimpleNamespace(id=100, channel=thread, author=author, webhook_id=None,
                content='Here is the version', attachments=[], jump_url='https://discord.com/channels/1/10/100')
            await listener.ingest(reply)
            pending = state.pending()
            self.assertEqual([row['id'] for row in pending], [10, 100])
            self.assertIn('Original StageError', json.loads(pending[0]['payload'])['content'])
            state.db.close()

    async def test_recovers_issue_after_crash_before_mapping_is_saved(self):
        github = GitHub(None, 'owner/repo')
        async def pages(path):
            yield {'number': 17, 'body': '<!-- discord-thread:99 -->\nreport'}
        github.pages = pages
        issue = await github.issue_for(99, 'Error', payload(99, 'report'))
        self.assertEqual(issue['number'], 17)

    async def test_new_issue_holds_the_draft_report_and_hidden_messages(self):
        github = GitHub(None, 'owner/repo')
        async def pages(path):
            return
            yield
        created = {}
        async def request(method, path, **kwargs):
            created.update(kwargs['json'])
            return {'number': 3}
        github.pages, github.request = pages, request
        await github.issue_for(99, 'Error', payload(99, 'Build fails', facts=diagnostics_facts(diagnostics_zip())))
        self.assertEqual(created['title'], 'Error when building Bank Job (zm_bankjob)')
        self.assertTrue(created['body'].startswith('<!-- discord-thread:99 -->\n' + DRAFT_MARKER))
        self.assertEqual([m['message_id'] for m in decode_messages(created['body'])], [99])

    async def test_follow_up_edits_the_issue_instead_of_commenting(self):
        github = GitHub(None, 'owner/repo')
        first = [payload(99, 'Build fails')]
        body = '<!-- discord-thread:99 -->\n' + format_issue('Error', first) + '\n\n' + encode_messages(first)
        calls = []
        async def request(method, path, **kwargs):
            calls.append((method, path, kwargs.get('json')))
            return {'body': body}
        github.request = request
        await github.forward(17, payload(100, 'here are the logs', facts=diagnostics_facts(diagnostics_zip())), 'Error')
        method, path, update = calls[-1]
        self.assertEqual((method, path), ('PATCH', '/issues/17'))
        self.assertEqual([m['message_id'] for m in decode_messages(update['body'])], [99, 100])
        # Still a draft: the diagnostics from the reply update the title and error.
        self.assertEqual(update['title'], 'Error when building Bank Job (zm_bankjob)')
        self.assertIn('_waw2bo2_zm.csc', update['body'])
        self.assertTrue(update['body'].startswith('<!-- discord-thread:99 -->'))
        self.assertNotIn('POST', [c[0] for c in calls])

    async def test_follow_up_keeps_the_investigated_report(self):
        github = GitHub(None, 'owner/repo')
        body = '<!-- discord-thread:99 -->\n### Summary\nAnalysed.\n\n' + encode_messages([payload(99, 'a')])
        calls = []
        async def request(method, path, **kwargs):
            calls.append(kwargs.get('json'))
            return {'body': body}
        github.request = request
        await github.forward(17, payload(100, 'b'), 'Error')
        self.assertNotIn('title', calls[-1])
        self.assertIn('### Summary\nAnalysed.', calls[-1]['body'])

    async def test_retries_do_not_duplicate_a_forwarded_message(self):
        github = GitHub(None, 'owner/repo')
        for body in ('<!-- discord-message:123 -->\nlegacy report', encode_messages([payload(123, 'x')])):
            async def request(method, path, **kwargs):
                self.assertEqual(method, 'GET')
                return {'body': body}
            github.request = request
            await github.forward(17, payload(123, 'x'), 'Error')


if __name__ == '__main__':
    unittest.main()
