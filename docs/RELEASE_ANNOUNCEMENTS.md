# Daily Discord release announcements

New successful push-triggered **Preview builds** also get an immediate
Discord announcement through `.github/workflows/testing-announcements.yml`.
This covers testing builds on fix branches. `main` gets no preview build and is
never announced as a testing build; it is announced only as a nightly.
The announcement links to the exact run's Windows and Linux artifacts and
identifies its branch and commit. Failed builds and pull-request runs are skipped;
both package artifacts must exist and be unexpired before posting. A successful
rerun can produce another announcement for that run.

The workflow uses the same `DISCORD_RELEASE_WEBHOOK_URL` secret and channel/server
variables as the daily digest below. It runs on GitHub Actions independently of
the Railway listener. Merge the workflow and announcement code into the default
branch to enable the completion trigger. No GitHub Release is created by this
announcement; testing packages remain Actions artifacts.

If an announcement fails, run **Discord testing build announcement** manually
with the successful build's numeric Actions run ID. This reposts that exact
build without rebuilding packages. Webhook requests identify the client using
Discord's required user-agent format and wait for Discord's message receipt
before reporting success.

Each published nightly gets its own Discord announcement from the `announce`
job of `.github/workflows/release.yml`, titled with the nightly's name (for
example **New Nightly 2026-10-10**) rather than as a testing build. It links
the release's Windows and Linux downloads (no GitHub login needed), names the
`main` commit it was cut from, and lists up to five commits since
the previous nightly with a link to the full comparison. It runs only after
the release is published, so a failed nightly is never announced. Stable
version releases are not announced by this job.

`.github/workflows/release-announcements.yml` runs every day at **15:17 UTC**
on a GitHub-hosted Ubuntu runner. It posts one Discord embed containing links
to the latest stable release, the latest published nightly, and the latest
successful push-triggered testing build (including its Actions artifacts).
Patch notes list up to five commits since the previous nightly and link to the
full comparison. It also gives the next scheduled nightly window. Use **Run
workflow** to post an on-demand digest.

The target is Discord server `1557610224641245245`, channel
`1557862642503254136`. Create an **Incoming Webhook** for that channel in
**Server Settings → Integrations → Webhooks**, then save its URL as the GitHub
Actions repository secret `DISCORD_RELEASE_WEBHOOK_URL` under
**Settings → Secrets and variables → Actions**. The workflow checks the
webhook's channel and server IDs before posting; a webhook aimed elsewhere is
rejected. Never commit the URL or substitute the bot token for it.

The channel and server IDs can be changed with repository Actions variables
`DISCORD_RELEASE_CHANNEL_ID` and `DISCORD_RELEASE_GUILD_ID`. The webhook URL
stays private in the secret. Until that secret exists, the daily job records a
warning and skips posting without exposing a credential or failing the run.

The testing link opens the latest successful push run of **Preview
builds**; download its `windows-package` and `linux-package` artifacts from that
run. GitHub keeps these testing artifacts for 14 days and requires a GitHub
login to download them. A nightly or stable release is linked only after it is
published, so a failed or still-running package build is never described as a
released build.
