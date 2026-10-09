"""Post daily release digests and completed testing builds to Discord."""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

API = "https://api.github.com"
DISCORD_HOSTS = {"discord.com", "discordapp.com"}
DISCORD_USER_AGENT = "DiscordBot (https://github.com/Ticass/B2W, 1.0) WawConverter-release-announcement"
SOURCE_SHA_RE = re.compile(r"\bat commit `([0-9a-f]{40})`", re.IGNORECASE)


def request_json(url: str, token: str = "") -> Any:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "WawConverter-release-announcement",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=25) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise RuntimeError(f"GitHub API request failed with HTTP {error.code}") from None


def release_source_sha(release: dict[str, Any] | None) -> str | None:
    if not release:
        return None
    match = SOURCE_SHA_RE.search(release.get("body") or "")
    return match.group(1) if match else None


def commit_patch_notes(
    repo: str,
    current: dict[str, Any] | None,
    previous: dict[str, Any] | None,
    token: str,
) -> tuple[str, str]:
    current_sha = release_source_sha(current)
    previous_sha = release_source_sha(previous)
    if not current or not current_sha:
        return "Nightly source details are not available yet.", ""
    if not previous or not previous_sha:
        return "First nightly snapshot; see the release notes for this build.", current["html_url"]
    if current_sha == previous_sha:
        return "No new commits have reached the latest nightly snapshot.", current["html_url"]

    comparison = request_json(
        f"{API}/repos/{repo}/compare/{previous_sha}...{current_sha}", token
    )
    if not comparison:
        return "Could not load the commit comparison; open the nightly release for details.", current["html_url"]
    commits = comparison.get("commits", [])
    if not commits:
        notes = "No new commits between the latest nightly snapshots."
    else:
        lines = []
        for item in reversed(commits[-5:]):
            sha = item.get("sha", "")[:7]
            message = (item.get("commit", {}).get("message", "Update").splitlines()[0])
            message = " ".join(message.replace("@", "＠").split())
            if len(message) > 150:
                message = message[:147] + "…"
            lines.append(f"• `{sha}` {message}")
        notes = "\n".join(lines)
        remaining = max(0, len(commits) - len(lines))
        if remaining:
            notes += f"\n…and {remaining} more commit(s)."
    return notes[:1000], comparison.get("html_url", current["html_url"])


def run_url(repo: str) -> str:
    return f"https://github.com/{repo}/actions/workflows/ci.yml"


def make_message(
    repo: str,
    stable: dict[str, Any] | None,
    nightly: dict[str, Any] | None,
    testing: dict[str, Any] | None,
    patch_notes: str,
    compare_url: str,
    next_nightly: str = "06:17 UTC daily (subject to GitHub Actions scheduling and build checks)",
) -> dict[str, Any]:
    releases_url = f"https://github.com/{repo}/releases"
    if stable:
        stable_value = f"[{stable.get('name') or stable.get('tag_name', 'Latest stable')}]({stable['html_url']})"
        stable_date = stable.get("published_at", "")[:10]
        if stable_date:
            stable_value += f"\nPublished {stable_date}"
    else:
        stable_value = f"[Stable releases]({releases_url})\nNo stable release has been published yet."

    if nightly:
        nightly_value = f"[{nightly.get('name') or nightly.get('tag_name', 'Latest nightly')}]({nightly['html_url']})"
        nightly_value += f"\nSnapshot `{nightly.get('tag_name', 'nightly')}`"
    else:
        nightly_value = f"[Releases page]({releases_url})\nNo nightly release has completed yet."

    if testing:
        branch = testing.get("head_branch", "testing")
        sha = testing.get("head_sha", "")[:7]
        testing_value = f"[Successful Actions build #{testing.get('run_number', testing.get('id'))}]({testing['html_url']})"
        testing_value += f"\nBranch `{branch}` · commit `{sha}` · artifacts on the run page"
    else:
        testing_value = f"[Bleeding-edge workflow]({run_url(repo)})\nNo successful testing build is available yet."

    notes_value = patch_notes or "No patch notes are available for this snapshot."
    if compare_url:
        notes_value += f"\n[Full comparison]({compare_url})"
    return {
        "username": "WawConverter Releases",
        "allowed_mentions": {"parse": []},
        "embeds": [
            {
                "title": "Daily release update",
                "description": f"Next scheduled nightly snapshot: **{next_nightly}**.",
                "color": 0x79C98B,
                "fields": [
                    {"name": "Stable", "value": stable_value[:1024], "inline": True},
                    {"name": "Nightly", "value": nightly_value[:1024], "inline": True},
                    {"name": "Testing · latest successful push build", "value": testing_value[:1024], "inline": False},
                    {"name": "Patch notes · since previous nightly", "value": notes_value[:1024], "inline": False},
                ],
                "footer": {"text": "WawConverter · automated GitHub Actions digest"},
                "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
            }
        ],
    }


def testing_message(repo: str, run_id: int, token: str) -> dict[str, Any]:
    run = request_json(f"{API}/repos/{repo}/actions/runs/{run_id}", token)
    if (not run or run.get("conclusion") != "success" or run.get("event") != "push"
            or run.get("head_repository", {}).get("full_name") != repo
            or run.get("path") != ".github/workflows/ci.yml"):
        raise RuntimeError("Testing announcement requires a successful repository push build of ci.yml")

    artifacts = {}
    page = 1
    while True:
        result = request_json(
            f"{API}/repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100&page={page}", token
        ) or {}
        batch = result.get("artifacts", [])
        for artifact in batch:
            if not artifact.get("expired"):
                artifacts[artifact["name"]] = artifact
        if len(batch) < 100:
            break
        page += 1
    if not {"windows-package", "linux-package"}.issubset(artifacts):
        raise RuntimeError("Testing build is missing unexpired Windows or Linux artifacts")

    build_url = f"https://github.com/{repo}/actions/runs/{run_id}"
    downloads = []
    for label, name in (("Windows", "windows-package"), ("Linux", "linux-package")):
        artifact_id = int(artifacts[name]["id"])
        downloads.append(f"[{label} download]({build_url}/artifacts/{artifact_id})")
    branch = str(run.get("head_branch", "testing")).replace("`", "").replace("@", "＠")[:200]
    sha = str(run.get("head_sha", ""))[:7]
    return {
        "username": "WawConverter Releases",
        "allowed_mentions": {"parse": []},
        "embeds": [{
            "title": f"New testing build #{run.get('run_number', run_id)}",
            "url": build_url,
            "description": f"Windows and Linux builds passed.\nBranch `{branch}` · commit `{sha}`",
            "color": 0x5899DA,
            "fields": [{"name": "Downloads", "value": " · ".join(downloads)},
                       {"name": "Testing build", "value": "Requires a GitHub login to download. "
                        "Artifacts are retained for 14 days. This is a testing build; "
                        "stable and nightly releases are listed separately."}],
            "footer": {"text": "WawConverter · testing build announcement"},
            "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        }],
    }


def published_nightlies(repo: str, token: str) -> list[dict[str, Any]]:
    """Published nightly prereleases, newest first."""
    releases = request_json(f"{API}/repos/{repo}/releases?per_page=100", token) or []
    nightlies = [
        release for release in releases
        if not release.get("draft") and release.get("prerelease") and release.get("tag_name", "").startswith("nightly-")
    ]
    nightlies.sort(key=lambda release: release.get("published_at") or "", reverse=True)
    return nightlies


def nightly_message(repo: str, tag: str, source_branch: str, token: str) -> dict[str, Any]:
    """Announcement for one published nightly: its release downloads, the
    release branch it was built from, and the commits since the previous nightly."""
    release = request_json(f"{API}/repos/{repo}/releases/tags/{urllib.parse.quote(tag, safe='')}", token)
    if not release or release.get("draft") or not release.get("prerelease") or not tag.startswith("nightly-"):
        raise RuntimeError(f"Nightly announcement requires the published nightly prerelease {tag}")
    assets = {asset.get("name"): asset for asset in release.get("assets", [])}
    downloads = []
    for label, name in (("Windows", "WawConverter-Windows.zip"), ("Linux", "WawConverter-Linux-x86_64.tar.gz")):
        if name not in assets:
            raise RuntimeError(f"Nightly {tag} is missing {name}")
        downloads.append(f"[{label} download]({assets[name]['browser_download_url']})")

    nightlies = published_nightlies(repo, token)
    tags = [item.get("tag_name") for item in nightlies]
    previous = nightlies[tags.index(tag) + 1] if tag in tags and tags.index(tag) + 1 < len(nightlies) else None
    notes, compare_url = commit_patch_notes(repo, release, previous, token)
    if compare_url and compare_url != release["html_url"]:
        notes += f"\n[Full comparison]({compare_url})"

    sha = (release_source_sha(release) or "")[:7]
    branch = source_branch.replace("`", "").replace("@", "＠")[:200] or "main"
    title = release.get("name") or tag
    return {
        "username": "WawConverter Releases",
        "allowed_mentions": {"parse": []},
        "embeds": [{
            "title": f"New {title}" if title.lower().startswith("nightly") else f"New nightly {title}",
            "url": release["html_url"],
            "description": f"Tonight's nightly release is published.\nBuilt from `{branch}` · commit `{sha}`",
            "color": 0x9B6BD6,
            "fields": [{"name": "Downloads", "value": " · ".join(downloads)},
                       {"name": "Changes since the previous nightly", "value": notes[:1024]},
                       {"name": "Nightly release", "value": "No GitHub login required. Nightlies are development "
                        "previews; the latest stable release stays the recommended download."}],
            "footer": {"text": "WawConverter · nightly release announcement"},
            "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        }],
    }


def webhook_metadata(webhook_url: str) -> dict[str, Any]:
    parsed = urllib.parse.urlsplit(webhook_url)
    parts = parsed.path.strip("/").split("/")
    if parsed.scheme != "https" or parsed.hostname not in DISCORD_HOSTS:
        raise ValueError("Webhook URL must be an HTTPS Discord webhook")
    if len(parts) < 4 or parts[-3] != "webhooks" or not parts[-2].isdigit():
        raise ValueError("Webhook URL does not have the expected Discord format")
    request = urllib.request.Request(webhook_url, headers={
        "Accept": "application/json", "User-Agent": DISCORD_USER_AGENT,
    })
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            metadata = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"Discord webhook validation failed with HTTP {error.code}") from None
    except urllib.error.URLError:
        raise RuntimeError("Discord webhook validation request failed") from None
    return metadata


def post_message(webhook_url: str, message: dict[str, Any]) -> dict[str, Any]:
    # wait=true asks Discord to confirm creation and return the posted message.
    parsed = urllib.parse.urlsplit(webhook_url)
    query = dict(urllib.parse.parse_qsl(parsed.query))
    query['wait'] = 'true'
    confirmed_url = urllib.parse.urlunsplit(parsed._replace(query=urllib.parse.urlencode(query)))
    data = json.dumps(message, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        confirmed_url,
        data=data,
        headers={"Content-Type": "application/json", "User-Agent": DISCORD_USER_AGENT},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            posted = json.load(response)
        if not str(posted.get('id', '')).isdigit() or not str(posted.get('channel_id', '')).isdigit():
            raise RuntimeError('Discord did not confirm creation of the announcement message')
        return posted
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"Discord webhook post failed with HTTP {error.code}") from None
    except urllib.error.URLError:
        raise RuntimeError("Discord webhook post request failed") from None


def main() -> int:
    webhook = os.environ.get("DISCORD_RELEASE_WEBHOOK_URL", "").strip()
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if not webhook:
        message = "Discord release announcement skipped: add the DISCORD_RELEASE_WEBHOOK_URL Actions secret."
        print(f"::warning::{message}")
        if summary:
            with open(summary, "a", encoding="utf-8") as stream:
                stream.write(f"{message}\n")
        return 0

    metadata = webhook_metadata(webhook)
    expected_channel = os.environ.get("DISCORD_RELEASE_CHANNEL_ID", "").strip()
    expected_guild = os.environ.get("DISCORD_RELEASE_GUILD_ID", "").strip()
    if expected_channel and str(metadata.get("channel_id", "")) != expected_channel:
        raise RuntimeError("Configured Discord webhook targets a different channel than DISCORD_RELEASE_CHANNEL_ID")
    if expected_guild and str(metadata.get("guild_id", "")) != expected_guild:
        raise RuntimeError("Configured Discord webhook targets a different server than DISCORD_RELEASE_GUILD_ID")

    token = os.environ.get("GITHUB_TOKEN", "")
    repo = os.environ["GITHUB_REPOSITORY"]
    testing_run_id = os.environ.get("TESTING_RUN_ID", "").strip()
    if testing_run_id:
        payload = testing_message(repo, int(testing_run_id), token)
        posted = post_message(webhook, payload)
        print(f"Testing build {int(testing_run_id)} announcement confirmed: "
              f"message {posted['id']} in channel {posted['channel_id']}.")
        if summary:
            with open(summary, "a", encoding="utf-8") as stream:
                stream.write(f"Testing build `{int(testing_run_id)}` announced on Discord.\n")
        return 0

    nightly_tag = os.environ.get("NIGHTLY_TAG", "").strip()
    if nightly_tag:
        payload = nightly_message(repo, nightly_tag, os.environ.get("NIGHTLY_SOURCE_BRANCH", "").strip(), token)
        posted = post_message(webhook, payload)
        print(f"Nightly {nightly_tag} announcement confirmed: "
              f"message {posted['id']} in channel {posted['channel_id']}.")
        if summary:
            with open(summary, "a", encoding="utf-8") as stream:
                stream.write(f"Nightly `{nightly_tag}` announced on Discord.\n")
        return 0

    latest_stable = request_json(f"{API}/repos/{repo}/releases/latest", token)
    nightlies = published_nightlies(repo, token)
    nightly = nightlies[0] if nightlies else None
    previous_nightly = nightlies[1] if len(nightlies) > 1 else None
    notes, compare_url = commit_patch_notes(repo, nightly, previous_nightly, token)

    workflow_path = urllib.parse.quote("ci.yml", safe="")
    runs = request_json(
        f"{API}/repos/{repo}/actions/workflows/{workflow_path}/runs?event=push&status=success&per_page=1",
        token,
    ) or {}
    testing_runs = runs.get("workflow_runs", [])
    testing = testing_runs[0] if testing_runs else None
    payload = make_message(repo, latest_stable, nightly, testing, notes, compare_url)
    post_message(webhook, payload)

    channel = expected_channel or metadata.get("channel_id", "unknown")
    print(f"Release announcement posted to Discord channel {channel}.")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"Daily release digest posted to Discord channel `{channel}`.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
