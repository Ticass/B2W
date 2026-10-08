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
from discord import app_commands
from discord.ext import commands

from core import LABEL, MAX_ATTACHMENT, REPLY_MARKER, State, attachment_text, chunks, format_report, message_marker, redact
from map_compatibility import (CodRepoCatalog, MAX_UPLOAD_TOTAL_BYTES,
    REPORT_LABEL, encode_report_marker, search_maps, valid_video_url, validate_upload)

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
        for name, color, description in (
            (LABEL, '5865F2', 'Error report forwarded from the configured Discord forum'),
            (REPORT_LABEL, '2DA44E', 'Community map compatibility report submitted from Discord'),
        ):
            try:
                await self.request('GET', f'/labels/{name}')
            except GitHubError as error:
                if error.status != 404:
                    raise
                await self.request('POST', '/labels', json={
                    'name': name, 'color': color, 'description': description})

    async def map_report_issue(self, report: dict, interaction_id: int):
        report = {**report, 'interaction_id': str(interaction_id)}
        marker = encode_report_marker(report)
        async for issue in self.pages(f'/issues?state=all&labels={REPORT_LABEL}'):
            if 'pull_request' not in issue and (issue.get('body') or '').splitlines()[:1] == [marker]:
                return issue
        title = f"[Map compatibility] {report['map_title']} — {report['outcome']}"[:240]
        files = report.get('attachments', [])
        file_lines = '\n'.join(f"- [{item['name']}]({item['url']})" for item in files) or '- None attached'
        details = report.get('known_issues') or 'No additional issues or notes supplied.'
        video = report.get('video_url') or 'Not supplied'
        body = (f'{marker}\n\n'
            '## Community-submitted map compatibility report\n\n'
            '> Community evidence is not maintainer verification.\n\n'
            f"- Map: [{report['map_title']}]({report['map_url']}) (CodRepo ID `{report['map_id']}`)\n"
            f"- Result: **{report['outcome']}**\n"
            f"- WawConverter version: `{report['tool_version']}`\n"
            f"- Platform: **{report['platform']}**\n"
            f"- Reporter: {report['reporter']} (`{report['reporter_id']}`)\n"
            f"- Submitted: {report['submitted_at']}\n"
            f"- Discord report: {report['discord_url']}\n"
            f"- Gameplay video: {video}\n\n"
            f'### Known issues and test notes\n\n{details}\n\n'
            f'### Screenshots, logs, and crash dumps\n\n{file_lines}\n')
        return await self.request('POST', '/issues', json={
            'title': title, 'body': body, 'labels': [REPORT_LABEL]})

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


def map_report_embed(map_record: dict, outcome: str, platform: str, version: str,
                     notes: str, video_url: str) -> discord.Embed:
    label = 'Playable' if outcome == 'playable' else 'Broken'
    embed = discord.Embed(
        title=f'Community map report · {label}',
        description=f"[{discord.utils.escape_markdown(map_record['title'])}]({map_record['url']})",
        colour=discord.Colour.green() if outcome == 'playable' else discord.Colour.red())
    embed.add_field(name='WawConverter', value=discord.utils.escape_markdown(version)[:200], inline=True)
    embed.add_field(name='Platform', value=platform.title(), inline=True)
    embed.add_field(name='Known issues / test notes', value=(discord.utils.escape_mentions(notes)[:1000] or 'None supplied'), inline=False)
    if video_url:
        embed.add_field(name='Gameplay video', value=video_url[:1000], inline=False)
    embed.set_footer(text='Community report · details and evidence are public on GitHub')
    return embed


class CompatibilityReportModal(discord.ui.Modal):
    def __init__(self, bot: 'SupportBot', map_record: dict, outcome: str,
                 interaction: discord.Interaction):
        title = f"{map_record['title']} · {'Playable' if outcome == 'playable' else 'Broken'}"
        super().__init__(title=title[:45], timeout=900,
                         custom_id=f'map-report:{interaction.id}')
        self.bot = bot
        self.map_record = map_record
        self.outcome = outcome
        self.channel_id = interaction.channel_id
        self.reporter = str(interaction.user)
        self.reporter_id = interaction.user.id
        self.interaction_id = interaction.id

        self.version = discord.ui.TextInput(
            placeholder='For example, 0.2.14', max_length=64, required=True)
        self.add_item(discord.ui.Label(text='WawConverter version', component=self.version,
            description='Use the version shown in the app or About dialog.'))
        self.platform = discord.ui.RadioGroup(required=True)
        self.platform.add_option(label='Windows', value='windows')
        self.platform.add_option(label='Linux', value='linux')
        self.add_item(discord.ui.Label(text='Platform', component=self.platform))
        self.video = discord.ui.TextInput(
            placeholder='https://… (optional)', max_length=1000, required=False)
        self.add_item(discord.ui.Label(text='Gameplay video link', component=self.video,
            description='Share an https link if you have one.'))
        self.notes = discord.ui.TextInput(
            style=discord.TextStyle.paragraph, placeholder='What worked or failed?',
            max_length=1500, required=False)
        self.add_item(discord.ui.Label(text='Known issues and test notes', component=self.notes))
        self.files = discord.ui.FileUpload(min_values=0, max_values=5, required=False)
        self.add_item(discord.ui.Label(text='Screenshots, logs, or crash dump', component=self.files,
            description='Optional · up to 5 files, 8 MiB each. This report is public on GitHub.'))

    async def on_submit(self, interaction: discord.Interaction):
        await interaction.response.defer(ephemeral=True, thinking=True)
        uploaded = list(self.files.values or [])
        total_size = sum(item.size for item in uploaded)
        for attachment in uploaded:
            reason = validate_upload(attachment.filename, attachment.size)
            if reason:
                await interaction.edit_original_response(content=f'{attachment.filename}: {reason}')
                return
        if total_size > MAX_UPLOAD_TOTAL_BYTES:
            await interaction.edit_original_response(content='Attachments must total 24 MiB or less.')
            return
        try:
            video_url = valid_video_url(self.video.value or '')
        except ValueError as error:
            await interaction.edit_original_response(content=str(error))
            return

        file_objects = []
        try:
            for attachment in uploaded:
                file_objects.append(await attachment.to_file(use_cached=True))
            channel = interaction.client.get_channel(self.channel_id) or await interaction.client.fetch_channel(self.channel_id)
            note_text = redact((self.notes.value or '').strip())
            version = redact(self.version.value.strip())
            platform = self.platform.value
            public_message = await channel.send(
                embed=map_report_embed(self.map_record, self.outcome, platform, version, note_text, video_url),
                files=file_objects, allowed_mentions=discord.AllowedMentions.none())
            report = {
                'map_id': self.map_record['id'], 'map_title': self.map_record['title'],
                'map_url': self.map_record['url'], 'outcome': self.outcome,
                'platform': platform, 'tool_version': version,
                'video_url': video_url, 'known_issues': note_text[:1500],
                'attachments': [
                    {'name': item.filename, 'url': item.url, 'size': item.size,
                     'content_type': item.content_type or ''}
                    for item in public_message.attachments
                ],
                'discord_url': public_message.jump_url,
                'reporter': self.reporter[:100], 'reporter_id': self.reporter_id,
                'submitted_at': discord.utils.utcnow().isoformat(),
            }
            issue = await self.bot.github.map_report_issue(report, self.interaction_id)
            embed = public_message.embeds[0]
            embed.add_field(name='Compatibility record', value=f"[GitHub issue #{issue['number']}]({issue['html_url']})", inline=False)
            await public_message.edit(embed=embed, allowed_mentions=discord.AllowedMentions.none())
            await interaction.edit_original_response(content=(
                f"Thanks — the **{self.outcome}** report for **{self.map_record['title']}** was added to the "
                f"[public compatibility record]({issue['html_url']}). The map page refreshes automatically."))
        except (aiohttp.ClientError, discord.HTTPException, GitHubError, OSError, ValueError) as error:
            log.warning('Map compatibility report submission failed (%s)', type(error).__name__)
            await interaction.edit_original_response(content=(
                'I could not finish saving this report to GitHub. Please retry `/map-report`; '
                'the form contents were not saved as a compatibility record.'))
        finally:
            for file in file_objects:
                file.close()


class ReportStartView(discord.ui.View):
    def __init__(self, bot: 'SupportBot', map_record: dict, owner_id: int):
        super().__init__(timeout=180)
        self.bot = bot
        self.map_record = map_record
        self.owner_id = owner_id

    async def interaction_check(self, interaction: discord.Interaction) -> bool:
        if interaction.user.id != self.owner_id:
            await interaction.response.send_message('Run `/map-report` to start your own report form.', ephemeral=True)
            return False
        return True

    @discord.ui.button(label='Playable', style=discord.ButtonStyle.success)
    async def playable(self, interaction: discord.Interaction, _button: discord.ui.Button):
        await interaction.response.send_modal(CompatibilityReportModal(
            self.bot, self.map_record, 'playable', interaction))

    @discord.ui.button(label='Broken', style=discord.ButtonStyle.danger)
    async def broken(self, interaction: discord.Interaction, _button: discord.ui.Button):
        await interaction.response.send_modal(CompatibilityReportModal(
            self.bot, self.map_record, 'broken', interaction))


class CompatibilityReportCommands(commands.Cog):
    def __init__(self, bot: 'SupportBot'):
        self.bot = bot

    async def map_autocomplete(self, interaction: discord.Interaction, current: str):
        if interaction.guild_id != self.bot.guild_id:
            return []
        return [app_commands.Choice(name=item['title'][:100], value=str(item['id']))
                for item in search_maps(self.bot.catalog.maps, current)]

    @app_commands.command(name='map-report', description='Submit a public map compatibility report for the GitHub page.')
    @app_commands.guild_only()
    @app_commands.checks.cooldown(1, 60.0, key=lambda interaction: interaction.user.id)
    @app_commands.autocomplete(map_id=map_autocomplete)
    @app_commands.rename(map_id='map')
    @app_commands.describe(map_id='Search CodRepo maps by name')
    async def map_report(self, interaction: discord.Interaction, map_id: str):
        if interaction.guild_id != self.bot.guild_id:
            await interaction.response.send_message('Map reports are enabled in the configured support server only.', ephemeral=True)
            return
        if isinstance(interaction.channel, discord.Thread) and interaction.channel.parent_id == self.bot.forum_id:
            await interaction.response.send_message('Use `/map-report` in a regular text channel, not the error-report forum.', ephemeral=True)
            return
        try:
            wanted = int(map_id)
        except ValueError:
            wanted = -1
        map_record = next((item for item in self.bot.catalog.maps if item['id'] == wanted), None)
        if map_record is None:
            await interaction.response.send_message('I could not match that map to the CodRepo index. Search again and select a result.', ephemeral=True)
            return
        embed = discord.Embed(
            title='Choose the map result',
            description=(f"[{discord.utils.escape_markdown(map_record['title'])}]({map_record['url']})\n\n"
                'Choose the result you tested. The next form asks for tool version, platform, video, notes, and evidence files. '
                'Submissions are public GitHub issues and appear on the compatibility page.'),
            colour=discord.Colour.blurple())
        await interaction.response.send_message(embed=embed,
            view=ReportStartView(self.bot, map_record, interaction.user.id), ephemeral=True)


class SupportBot(commands.Bot):
    def __init__(self):
        intents = discord.Intents.default()
        intents.message_content = True
        super().__init__(command_prefix='!', intents=intents, allowed_mentions=discord.AllowedMentions.none())
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
        self.codrepo_session = aiohttp.ClientSession(
            headers={'User-Agent': 'B2W-map-compatibility-bot/1.0 (+https://github.com/Ticass/B2W)',
                     'Accept': 'application/json'},
            timeout=aiohttp.ClientTimeout(total=90))
        self.catalog = CodRepoCatalog(self.codrepo_session, Path('/data/codrepo-maps.json'),
                                      Path('/app/codrepo-maps.json'))
        await self.catalog.refresh()
        await self.add_cog(CompatibilityReportCommands(self))
        guild = discord.Object(id=self.guild_id)
        self.tree.copy_global_to(guild=guild)
        await self.tree.sync(guild=guild)
        self.background = [asyncio.create_task(self.forward_loop()),
            asyncio.create_task(self.reconcile_loop()), asyncio.create_task(self.refresh_catalog_loop())]

    async def refresh_catalog_loop(self):
        await self.wait_until_ready()
        while not self.is_closed():
            await asyncio.sleep(12 * 60 * 60)
            await self.catalog.refresh()

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
        if hasattr(self, 'codrepo_session'):
            await self.codrepo_session.close()
        self.state.db.close()
        await super().close()


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(name)s %(message)s')
    SupportBot().run(os.environ.get('DISCORD_TOKEN') or os.environ['DISCORD_BOT_TOKEN'], log_handler=None)
