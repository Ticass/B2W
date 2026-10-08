# Daily Discord release announcements

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

The testing link opens the latest successful push run of **Bleeding edge
builds**; download its `windows-package` and `linux-package` artifacts from that
run. GitHub keeps these testing artifacts for 14 days and requires a GitHub
login to download them. A nightly or stable release is linked only after it is
published, so a failed or still-running package build is never described as a
released build.
