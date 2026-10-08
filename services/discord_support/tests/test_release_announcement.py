import unittest

from services.discord_support.release_announcement import make_message, release_source_sha, webhook_metadata


class ReleaseAnnouncementTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
