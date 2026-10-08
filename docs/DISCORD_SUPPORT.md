# Discord error-report bot

The bot watches the `error-logs` forum in server `1557610224641245245`, channel
`1557762745993142343`. New posts and human replies are forwarded to one GitHub
issue per Discord thread. The **Investigate Discord error report** workflow
starts a Codex investigation on a GitHub-hosted Ubuntu runner. It reads source,
diagnostic logs and available screenshots, tries to reproduce the failure,
and either asks a specific question, explains its findings, or proposes a fix.
Findings and questions are posted back into the original Discord thread.

A proposed fix must pass patch validation and Python regression tests before
the workflow creates a draft PR. It explicitly dispatches the Windows/Linux
packaging workflow for that branch. Review and merge the PR before the change
enters a nightly build. The bot does not merge changes or claim an unpublished
fix is released. Further replies start a new investigation with the complete
issue conversation and the current repository snapshot.

Investigations, verification and builds run on **GitHub-hosted runners**. The
small Discord listener runs continuously in Docker on a remote server. It
does not build the converter or run AI on that server. GitHub Actions jobs
are temporary, so the listener needs an always-on host to receive Discord
Gateway events. This is a separate Codex agent started by Actions; it does
not wake this desktop chat or depend on your PC remaining on.

## Credentials and repository setup

1. Merge the workflows and bot code into the default branch and enable Actions.
2. Add repository secret **`OPENAI_API_KEY`**. The [Codex GitHub Action](https://github.com/openai/codex-action)
   uses API credentials; the Discord bot token alone does not provide AI access.
3. Create a fine-grained GitHub token restricted to this repository, with
   **Issues: read and write**. Supply it to the listener as `GITHUB_TOKEN`.
   The listener needs no repository contents write access. A dedicated service
   account makes its report activity easier to distinguish.
4. Set repository variable **`DISCORD_BOT_GITHUB_LOGIN`** to the GitHub username
   that owns that token. Only report issues and follow-up comments from this
   identity, carrying the `discord-report` label/message marker, trigger AI work.
   The listener creates the label at startup if it does not exist.
5. In repository **Settings → Actions → General → Workflow permissions**, enable
   **Allow GitHub Actions to create and approve pull requests**. The publisher
   requests the permissions it needs in its own job. Branch/repository rules
   must permit `codex/discord-report-*` branches and draft PR creation.

The runner's OpenAI credential is handled by the pinned official Codex Action,
with its workspace permission profile and privilege restrictions. Report-driven
code and tests run in jobs without repository write credentials. Publication
runs separately, applies a validated artifact in a fresh checkout, and executes
no proposed converter or test code there. Automated patches cannot change CI,
bot/deployment code, credentials or instruction files.

## Discord setup

In the [Discord Developer Portal](https://discord.com/developers/applications),
enable **Message Content Intent** for the bot. The code enables this intent as
well; Discord requires both settings to read messages and attachments. See
the [discord.py intent guide](https://discordpy.readthedocs.io/en/stable/intents.html).

Invite the bot with the `bot` OAuth2 scope. Grant it **View Channels**, **Read
Message History**, **Send Messages**, and **Send Messages in Threads** in
`error-logs`. Also grant **Use Application Commands** (or invite with the
`applications.commands` OAuth2 scope), plus **Send Messages**, **Embed Links**,
and **Attach Files** in the regular channel where map reports are submitted.
The `/map-report` command is registered to server `1557610224641245245` and
searches CodRepo's live World at War map catalog. It asks for Playable/Broken,
tool version, Windows/Linux, optional video, test notes, and up to five
screenshots/log/crash files (8 MiB each; 24 MiB total). Evidence is posted in
that channel and a public GitHub issue is created; issue changes automatically
rebuild the public compatibility page. Do not submit private data. The bot
joins active error-report threads. Administrator permission is not required.
Moderators must unlock inaccessible/locked archived threads when a reply
cannot be delivered. The listener retries failed deliveries.

## Deploy the listener on your remote host

On the remote Linux host with Docker Compose installed, clone the repository,
then create `services/discord_support/.env` from `.env.example` and inject the
repository secret `DISCORD_TOKEN` into the host's secret store under the same
environment variable name. Set `GITHUB_TOKEN` there as well. GitHub repository
secrets are available to Actions workflows, but do not automatically transfer
to a separately hosted Docker service; the host's secret manager must receive
the value through your deployment process. Never commit the token or paste it
into logs. The server and forum IDs are already filled in. `.env` is ignored by
Git and excluded from the Docker image. Keep the OpenAI key in GitHub's secret
settings; it is not needed on the listener host.

```sh
docker compose -f services/discord_support/compose.yml up -d --build
docker compose -f services/discord_support/compose.yml logs -f --tail=100
```

The image runs as an unprivileged user and stores its SQLite inbox, thread/issue
mapping and reply outbox in the `support-state` volume. Preserve and back up that
volume across redeployments. Restarting the service recovers pending work and
deduplicates report messages. A crash immediately after a Discord send can
repeat that reply; delivery to an external API cannot be made atomic with the
local database. Deleting the volume loses the delivery checkpoints.

`BACKFILL_HOURS=24` processes recent existing reports on first startup. Set it
to `0` before the first startup to handle new reports only. The cutoff is then
persisted. Reconciliation reads up to 100 messages per thread per poll and
up to 100 recently archived threads, supplementing live events after a
disconnect. `POLL_SECONDS=30` controls catch-up and result polling. GitHub runner
queue times add to investigation latency. Closing a tracking issue does not
disable new human replies in its Discord thread.

If Railway logs show Discord HTTP 429 with a Cloudflare 1015 page, the egress
IP is temporarily rate-limited. Stop the deployment retry loop by setting the
Railway service restart policy to **Never**, wait before trying again, then
start one replica once. The bot now backs off exponentially (up to 30 minutes)
on Discord 429 responses so a temporary block does not cause rapid relaunches.

## What reports include

The listener reads text/log/JSON/CSV attachments and diagnostic ZIPs up to
8 MiB. ZIP entries are inspected in memory with bounded text reads; executable
files, game assets, traversal paths, encrypted entries and oversized entries
are skipped. It never runs an attachment or extracts it onto disk. Screenshot
links are downloaded on the runner only from the Discord attachment CDN, with
size/type checks and redirects disabled. Expired links lead to a request to
attach the screenshot again if it is needed.

Report text and diagnostic excerpts are copied to GitHub issues, which are
public if the repository is public. Common token/password fields and user-home
paths are redacted, but automatic redaction cannot cover every sensitive value;
reporters should inspect diagnostics before posting. Bot-generated Discord
messages disable mentions, and bot/webhook posts are not forwarded back into
the investigation loop.

## Verification

**Discord support bot checks** runs on a GitHub-hosted Ubuntu runner. It checks
diagnostic parsing, credential redaction, inbox/outbox recovery, duplicate
delivery handling, allowed patch paths and screenshot URL restrictions, then
validates Compose and builds the listener image. It requires no live tokens.
Use **Run workflow** or push changes to these bot/workflow files to run it.

Once the host and credentials are configured, post one real test report in
`error-logs`. Confirm that it gets an acknowledgment and tracking issue, the
investigation workflow starts, a response returns to the same thread, and a
human follow-up triggers another investigation. The listener logs IDs and
failure types, not token values or report contents. If a job fails or times out,
the reporter receives a run link for a maintainer to investigate.
