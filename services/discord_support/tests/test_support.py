from __future__ import annotations

import asyncio
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from core import State, attachment_text, chunks, format_report, message_marker, redact
from bot import GitHub


class IntakeTests(unittest.TestCase):
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

    def test_issue_metadata_survives_diagnostic_truncation(self):
        payload = {'message_id': 42, 'author': 'reporter', 'author_id': 8,
                   'url': 'https://discord.com/channels/1/2/42', 'content': 'x' * 100000,
                   'attachments': [{'url': 'https://cdn.discordapp.com/attachments/1/2/image.png'}]}
        body = format_report(payload)
        self.assertLess(len(body), 65536)
        self.assertIn(message_marker(42), body)
        self.assertTrue(body.endswith(' -->'))
        self.assertTrue(all(len(part) <= 1900 for part in chunks('x' * 4100)))


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
    async def test_recovers_issue_after_crash_before_mapping_is_saved(self):
        github = GitHub(None, 'owner/repo')
        async def pages(path):
            yield {'number': 17, 'body': '<!-- discord-thread:99 -->\nreport'}
        github.pages = pages
        issue = await github.issue_for(99, 'Error', 'report')
        self.assertEqual(issue['number'], 17)

    async def test_retries_do_not_duplicate_a_forwarded_message(self):
        github = GitHub(None, 'owner/repo')
        async def request(method, path, **kwargs):
            self.assertEqual(method, 'GET')
            return {'body': '<!-- discord-message:123 -->\nexisting report'}
        github.request = request
        await github.forward(17, {'message_id': 123})


if __name__ == '__main__':
    unittest.main()
