import unittest
import io
import json
from unittest.mock import patch

from services.discord_support.release_announcement import DISCORD_USER_AGENT, make_message, nightly_message, post_message, release_source_sha, testing_message, webhook_metadata


class ReleaseAnnouncementTests(unittest.TestCase):
    @patch('services.discord_support.release_announcement.urllib.request.urlopen')
    def test_webhook_validation_identifies_client_to_discord(self, urlopen):
        urlopen.return_value = io.BytesIO(b'{"channel_id": "123"}')
        metadata = webhook_metadata('https://discord.com/api/webhooks/123/test-token')
        self.assertEqual(metadata['channel_id'], '123')
        request = urlopen.call_args.args[0]
        self.assertEqual(request.get_header('User-agent'), DISCORD_USER_AGENT)

    @patch('services.discord_support.release_announcement.urllib.request.urlopen')
    def test_post_waits_for_message_creation_and_returns_discord_receipt(self, urlopen):
        urlopen.return_value = io.BytesIO(b'{"id": "456", "channel_id": "123"}')
        result = post_message('https://discord.com/api/webhooks/123/test-token?wait=false',
                              {'allowed_mentions': {'parse': []}})
        self.assertEqual(result['id'], '456')
        request = urlopen.call_args.args[0]
        self.assertIn('wait=true', request.full_url)
        self.assertNotIn('wait=false', request.full_url)
        self.assertEqual(request.get_header('User-agent'), DISCORD_USER_AGENT)
        self.assertEqual(json.loads(request.data)['allowed_mentions'], {'parse': []})

    @patch('services.discord_support.release_announcement.urllib.request.urlopen')
    def test_post_does_not_claim_delivery_without_a_message_receipt(self, urlopen):
        urlopen.return_value = io.BytesIO(b'{}')
        with self.assertRaisesRegex(RuntimeError, 'did not confirm'):
            post_message('https://discord.com/api/webhooks/123/test-token', {})

    def completed_run(self, **changes):
        return {"conclusion": "success", "event": "push", "path": ".github/workflows/ci.yml",
                "head_repository": {"full_name": "owner/repo"}, "head_branch": "main",
                "head_sha": "a" * 40, "run_number": 42, **changes}

    @patch("services.discord_support.release_announcement.request_json")
    def test_testing_announcement_links_to_exact_completed_run_artifacts(self, request):
        request.side_effect = [self.completed_run(), {"artifacts": [
            {"name": "windows-package", "id": 101, "expired": False},
            {"name": "linux-package", "id": 102, "expired": False}]}]
        payload = testing_message("owner/repo", 7, "token")
        embed = payload["embeds"][0]
        self.assertEqual(embed["title"], "New testing build #42")
        self.assertEqual(embed["url"], "https://github.com/owner/repo/actions/runs/7")
        downloads = embed["fields"][0]["value"]
        self.assertIn("/runs/7/artifacts/101", downloads)
        self.assertIn("/runs/7/artifacts/102", downloads)
        self.assertEqual(payload["allowed_mentions"], {"parse": []})
        self.assertIn("`main`", embed["description"])

    @patch("services.discord_support.release_announcement.request_json")
    def test_testing_rejects_failed_pull_request_foreign_and_other_workflow_runs(self, request):
        for changes in ({"conclusion": "failure"}, {"event": "pull_request"},
                        {"head_repository": {"full_name": "other/repo"}},
                        {"path": ".github/workflows/release.yml"}):
            with self.subTest(changes=changes):
                request.return_value = self.completed_run(**changes)
                with self.assertRaises(RuntimeError):
                    testing_message("owner/repo", 7, "token")

    @patch("services.discord_support.release_announcement.request_json")
    def test_testing_requires_both_unexpired_packages(self, request):
        for artifacts in ([{"name": "windows-package", "id": 101, "expired": False}],
                          [{"name": "windows-package", "id": 101, "expired": False},
                           {"name": "linux-package", "id": 102, "expired": True}]):
            with self.subTest(artifacts=artifacts):
                request.side_effect = [self.completed_run(), {"artifacts": artifacts}]
                with self.assertRaises(RuntimeError):
                    testing_message("owner/repo", 7, "token")

    def test_message_has_links_to_all_three_tracks_and_patch_notes(self):
        stable = {"name": "v1.2.0", "tag_name": "v1.2.0", "html_url": "https://example.test/stable", "published_at": "2026-10-01T00:00:00Z"}
        nightly = {"name": "Nightly", "tag_name": "nightly-2026-10-08-1", "html_url": "https://example.test/nightly"}
        testing = {"run_number": 42, "id": 7, "head_branch": "main", "head_sha": "123456789abcdef", "html_url": "https://example.test/testing"}
        payload = make_message("owner/repo", stable, nightly, testing, "• `abcdef0` Fix map conversion", "https://example.test/compare")

        embed = payload["embeds"][0]
        fields = {field["name"]: field["value"] for field in embed["fields"]}
        self.assertIn("https://example.test/stable", fields["Stable"])
        self.assertIn("https://example.test/nightly", fields["Nightly"])
        self.assertIn("https://example.test/testing", fields["Testing · latest successful push build"])
        self.assertIn("Fix map conversion", fields["Patch notes · since previous nightly"])
        self.assertIn("https://example.test/compare", fields["Patch notes · since previous nightly"])
        self.assertEqual(payload["allowed_mentions"], {"parse": []})

    def test_missing_release_tracks_link_to_their_listing_pages(self):
        payload = make_message("owner/repo", None, None, None, "No patch notes", "")
        fields = {field["name"]: field["value"] for field in payload["embeds"][0]["fields"]}
        self.assertIn("https://github.com/owner/repo/releases", fields["Stable"])
        self.assertIn("https://github.com/owner/repo/releases", fields["Nightly"])
        self.assertIn("https://github.com/owner/repo/actions/workflows/ci.yml", fields["Testing · latest successful push build"])

    def test_nightly_source_commit_is_read_from_release_notes(self):
        sha = "a" * 40
        self.assertEqual(release_source_sha({"body": f"Built at commit `{sha}`."}), sha)
        self.assertIsNone(release_source_sha({"body": "No source listed"}))

    def test_webhook_metadata_rejects_non_discord_urls(self):
        with self.assertRaises(ValueError):
            webhook_metadata("https://example.com/webhooks/123/token")

    def nightly(self, tag, sha, published, **changes):
        return {"tag_name": tag, "name": "Nightly " + tag.removeprefix("nightly-"), "prerelease": True,
                "draft": False, "published_at": published,
                "html_url": f"https://github.com/owner/repo/releases/tag/{tag}",
                "body": f"Development preview built from `release/2026-10-09` (snapshot `x`) at commit `{sha}`.",
                "assets": [{"name": "WawConverter-Windows.zip", "browser_download_url": "https://dl/win.zip"},
                           {"name": "WawConverter-Linux-x86_64.tar.gz", "browser_download_url": "https://dl/linux.tgz"}],
                **changes}

    @patch("services.discord_support.release_announcement.request_json")
    def test_nightly_announcement_is_not_a_testing_build(self, request):
        current = self.nightly("nightly-2026-10-10-2", "b" * 40, "2026-10-10T07:00:00Z")
        previous = self.nightly("nightly-2026-10-09-1", "a" * 40, "2026-10-09T07:00:00Z")
        request.side_effect = [current, [previous, current], {
            "html_url": "https://github.com/owner/repo/compare/a...b",
            "commits": [{"sha": "c" * 40, "commit": {"message": "Fix the bootstrap\n\nbody"}}]}]
        payload = nightly_message("owner/repo", "nightly-2026-10-10-2", "release/2026-10-09", "token")
        embed = payload["embeds"][0]
        self.assertEqual(embed["title"], "New Nightly 2026-10-10-2")
        self.assertNotIn("testing", json.dumps(embed).lower())
        self.assertIn("`release/2026-10-09`", embed["description"])
        self.assertIn("`bbbbbbb`", embed["description"])
        fields = {field["name"]: field["value"] for field in embed["fields"]}
        self.assertIn("https://dl/win.zip", fields["Downloads"])
        self.assertIn("https://dl/linux.tgz", fields["Downloads"])
        self.assertIn("Fix the bootstrap", fields["Changes since the previous nightly"])
        self.assertIn("compare/a...b", fields["Changes since the previous nightly"])
        self.assertEqual(payload["allowed_mentions"], {"parse": []})
        self.assertIn("/compare/" + "a" * 40 + "..." + "b" * 40, request.call_args_list[2].args[0])

    @patch("services.discord_support.release_announcement.request_json")
    def test_nightly_announcement_requires_published_release_with_both_packages(self, request):
        cases = [None, self.nightly("nightly-1", "a" * 40, "x", draft=True),
                 self.nightly("nightly-1", "a" * 40, "x", prerelease=False),
                 self.nightly("nightly-1", "a" * 40, "x", assets=[])]
        for release in cases:
            request.side_effect = [release]
            with self.assertRaises(RuntimeError):
                nightly_message("owner/repo", "nightly-1", "main", "token")


if __name__ == "__main__":
    unittest.main()
