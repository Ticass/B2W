"""Discord forum listener. Investigations execute in GitHub Actions, not here."""
from __future__ import annotations

import asyncio
from datetime import datetime, timedelta, timezone
import json
import logging
import os
from pathlib import Path

import aiohttp
import discord

from core import LABEL, MAX_ATTACHMENT, REPLY_MARKER, State, attachment_text, chunks, format_report, message_marker

log = logging.getLogger('discord_support')


class GitHubError(Exception):
    def __init__(self, status: int):
        self.status = status
        super().__init__(f'GitHub HTTP {status}')


class GitHub:
    def __init__(self, session: aiohttp.ClientSession, repository: str):
        owner, repo = repository.split('/')
        if not owner or not repo or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._' for c in owner + repo):
            raise ValueError('GITHUB_REPOSITORY must be owner/repository')
        self.session = session
        self.root = f'https://api.github.com/repos/{owner}/{repo}'

    async def request(self, method: str, path: str, **kwargs):
        async with self.session.request(method, self.root + path, **kwargs) as response:
            if response.status >= 400:
                raise GitHubError(response.status)
            return await response.json() if response.status != 204 else None

    async def pages(self, path: str):
        separator = '&' if '?' in path else '?'
        page = 1
        while True:
            items = await self.request('GET', f'{path}{separator}per_page=100&page={page}')
            for item in items:
                yield item
            if len(items) < 100:
                break
            page += 1

    async def ensure_label(self):
        try:
            await self.request('GET', f'/labels/{LABEL}')
        except GitHubError as error:
            if error.status != 404:
                raise
            await self.request('POST', '/labels', json={'name': LABEL, 'color': '5865F2',
                'description': 'Error report forwarded from the configured Discord forum'})

    async def issue_for(self, thread_id: int, title: str, report: str):
        marker = f'<!-- discord-thread:{thread_id} -->'
        # Recover an issue created just before a service crash without duplicating it.
        async for issue in self.pages(f'/issues?state=all&labels={LABEL}'):
            if 'pull_request' not in issue and marker in (issue['body'] or ''):
                return issue
        return await self.request('POST', '/issues', json={
            'title': f'[Discord] {title}'[:240], 'body': marker + '\n' + report, 'labels': [LABEL]})

    async def forward(self, issue: int, payload: dict):
        marker = message_marker(payload['message_id'])
        existing = await self.request('GET', f'/issues/{issue}')
        if marker in (existing['body'] or ''):
            return
        async for comment in self.pages(f'/issues/{issue}/comments'):
            if marker in (comment['body'] or ''):
                return
        await self.request('POST', f'/issues/{issue}/comments', json={'body': format_report(payload)})


class SupportBot(discord.Client):
    def __init__(self):
        intents = discord.Intents.default()
        intents.message_content = True
        super().__init__(intents=intents, allowed_mentions=discord.AllowedMentions.none())
        self.guild_id = int(os.environ['DISCORD_GUILD_ID'])
        self.forum_id = int(os.environ['DISCORD_FORUM_CHANNEL_ID'])
        self.state = State(Path(os.getenv('STATE_PATH', '/data/support.sqlite3')))
        initial = datetime.now(timezone.utc) - timedelta(hours=int(os.getenv('BACKFILL_HOURS', '24')))
        self.cutoff = self.state.cutoff(discord.utils.time_snowflake(initial))
        self.background: list[asyncio.Task] = []

    async def setup_hook(self):
        self.github_session = aiohttp.ClientSession(headers={
            'Authorization': f"Bearer {os.environ['GITHUB_TOKEN']}",
            'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'},
            timeout=aiohttp.ClientTimeout(total=45))
        self.github = GitHub(self.github_session, os.environ['GITHUB_REPOSITORY'])
        await self.github.ensure_label()
        self.background = [asyncio.create_task(self.forward_loop()), asyncio.create_task(self.reconcile_loop())]

    def in_scope(self, channel) -> bool:
        return isinstance(channel, discord.Thread) and channel.parent_id == self.forum_id and channel.guild.id == self.guild_id

    async def ingest(self, message: discord.Message, *, include_context: bool = False):
        if (message.author.bot or message.webhook_id or
            (message.id < self.cutoff and not include_context) or not self.in_scope(message.channel)):
            return
        if not message.content and not message.attachments:
            return
        # Duplicate gateway/history events do not re-download attachments.
        if self.state.db.execute('SELECT 1 FROM inbox WHERE id = ?', (message.id,)).fetchone():
            return
        if message.id != message.channel.id and self.state.thread(message.channel.id) is None:
            # A recent reply to an older post still needs the original error context.
            try:
                starter = await message.channel.fetch_message(message.channel.id)
                await self.ingest(starter, include_context=True)
            except discord.HTTPException:
                log.info('Original report context unavailable for thread %s', message.channel.id)
        text, metadata = [message.content], []
        for attachment in message.attachments[:8]:
            metadata.append({'name': attachment.filename, 'url': attachment.url,
                             'content_type': attachment.content_type, 'size': attachment.size})
            if attachment.size > MAX_ATTACHMENT:
                text.append(f'{attachment.filename}: skipped; exceeds 8 MiB intake limit.')
            elif Path(attachment.filename).suffix.lower() in {'.zip', '.txt', '.log', '.json', '.csv'}:
                try:
                    data = await attachment.read()
                    text.append(f'Attachment {attachment.filename}:\n{attachment_text(attachment.filename, data)}')
                except discord.HTTPException:
                    # Leave the message unqueued so catch-up can retry the download.
                    raise
        payload = {'message_id': message.id, 'content': '\n\n'.join(text),
                   'author': str(message.author), 'author_id': message.author.id,
                   'url': message.jump_url, 'attachments': metadata}
        self.state.enqueue(message.channel.id, message.channel.name, payload)

    async def on_message(self, message):
        await self.ingest(message)

    async def on_thread_create(self, thread):
        if self.in_scope(thread):
            try:
                await thread.join()
                await self.ingest(await thread.fetch_message(thread.id))
            except discord.HTTPException:
                log.info('Starter message not available yet; catch-up will retry thread %s', thread.id)

    async def on_ready(self):
        log.info('Connected; watching server %s forum %s', self.guild_id, self.forum_id)

    async def send_text(self, thread_id: int, text: str):
        thread = self.get_channel(thread_id) or await self.fetch_channel(thread_id)
        if not self.in_scope(thread):
            raise ValueError('Refusing to reply outside the configured forum')
        for chunk in chunks(text[:15000]):
            await thread.send(chunk, allowed_mentions=discord.AllowedMentions.none())

    async def forward_loop(self):
        await self.wait_until_ready()
        while not self.is_closed():
            for row in self.state.pending():
                try:
                    thread = self.state.thread(row['thread'])
                    payload = json.loads(row['payload'])
                    if not thread['issue']:
                        issue = await self.github.issue_for(thread['id'], thread['title'], format_report(payload))
                        self.state.update(thread['id'], issue=issue['number'])
                        thread = self.state.thread(thread['id'])
                    await self.github.forward(thread['issue'], payload)
                    if not thread['acknowledged']:
                        # Recover replies produced before an issue mapping was saved.
                        async for comment in self.github.pages(f"/issues/{thread['issue']}/comments"):
                            body = comment['body'] or ''
                            if comment['user']['login'] == 'github-actions[bot]' and body.startswith(REPLY_MARKER):
                                self.state.queue_reply(comment['id'], thread['id'], body[len(REPLY_MARKER):].strip())
                        url = f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/issues/{thread['issue']}"
                        await self.send_text(thread['id'], 'I’ve received your report and queued an investigation. '
                            'I’ll post findings or questions here.\nTracking: ' + url)
                        self.state.update(thread['id'], acknowledged=1)
                    self.state.finish(row['id'])
                except (GitHubError, aiohttp.ClientError, TimeoutError, discord.HTTPException, ValueError) as error:
                    self.state.retry(row['id'], row['attempts'])
                    log.warning('Forwarding message %s failed (%s); retained for retry', row['id'], type(error).__name__)
            for reply in self.state.replies():
                try:
                    await self.send_text(reply['thread'], reply['body'])
                    self.state.finish_reply(reply['id'])
                except (discord.HTTPException, ValueError) as error:
                    self.state.retry_reply(reply['id'], reply['attempts'])
                    log.warning('Reply %s failed (%s); retained for retry', reply['id'], type(error).__name__)
            await asyncio.sleep(3)

    async def catch_up(self, thread):
        if not self.in_scope(thread):
            return
        row = self.state.thread(thread.id)
        cursor = row['history_cursor'] if row else 0
        after = discord.Object(id=max(cursor, self.cutoff))
        if not thread.archived:
            await thread.join()
        async for message in thread.history(limit=100, after=after, oldest_first=True):
            await self.ingest(message)
            # A history cursor advances only after a successful ingestion.
            if self.state.thread(thread.id):
                self.state.update(thread.id, history_cursor=message.id)

    async def reconcile_loop(self):
        await self.wait_until_ready()
        while not self.is_closed():
            try:
                forum = self.get_channel(self.forum_id) or await self.fetch_channel(self.forum_id)
                if not isinstance(forum, discord.ForumChannel) or forum.guild.id != self.guild_id:
                    raise ValueError('Configured channel must be the error-report forum')
                threads = [t for t in await forum.guild.active_threads() if self.in_scope(t)]
                async for archived in forum.archived_threads(limit=100):
                    if archived.archive_timestamp and archived.archive_timestamp < discord.utils.snowflake_time(self.cutoff):
                        break
                    threads.append(archived)
                for thread in threads:
                    try:
                        await self.catch_up(thread)
                    except discord.HTTPException:
                        log.warning('Cannot catch up thread %s; will retry', thread.id)
                # Poll repository comments once, rather than every issue forever.
                since = self.state.metadata('reply_since', discord.utils.snowflake_time(self.cutoff).isoformat()).replace('+00:00', 'Z')
                last_updated = datetime.fromisoformat(since.replace('Z', '+00:00'))
                initial_updated = last_updated
                async for comment in self.github.pages(f'/issues/comments?since={since}&sort=created&direction=asc'):
                    updated = datetime.fromisoformat(comment['updated_at'].replace('Z', '+00:00'))
                    last_updated = max(last_updated, updated)
                    row = self.state.issue_thread(int(comment['issue_url'].rsplit('/', 1)[-1]))
                    body = comment['body'] or ''
                    if (row and comment['id'] > row['reply_cursor'] and
                        comment['user']['login'] == 'github-actions[bot]' and body.startswith(REPLY_MARKER)):
                        self.state.queue_reply(comment['id'], row['id'], body[len(REPLY_MARKER):].strip())
                # One-second overlap prevents timestamp boundary losses; IDs deduplicate.
                if last_updated > initial_updated:
                    self.state.set_metadata('reply_since', (last_updated - timedelta(seconds=1)).isoformat())
            except (GitHubError, discord.HTTPException, aiohttp.ClientError, TimeoutError, ValueError) as error:
                log.warning('Reconciliation failed (%s); will retry', type(error).__name__)
            await asyncio.sleep(int(os.getenv('POLL_SECONDS', '30')))

    async def close(self):
        for task in self.background:
            task.cancel()
        await asyncio.gather(*self.background, return_exceptions=True)
        if hasattr(self, 'github_session'):
            await self.github_session.close()
        self.state.db.close()
        await super().close()


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(name)s %(message)s')
    SupportBot().run(os.environ['DISCORD_BOT_TOKEN'], log_handler=None)
