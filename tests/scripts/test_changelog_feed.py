"""Offline tests: python3 -B -m unittest discover -s tests/scripts -p 'test_changelog_feed.py'."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("release_import", Path(__file__).parents[2] / "scripts/github_to_featurebase.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)
URL = "https://github.com/NortheBridge/luminalshine/releases/tag/v1.0.0"
RELEASE = {"tag_name": "v1.0.0", "name": "Version 1", "html_url": URL,
           "body": "Public GitHub notes", "published_at": "2026-10-01T10:30:00Z", "prerelease": False}
PUBLIC = {"url": "https://northebridge.featurebase.app/changelog/v1", "title": "Public Featurebase title",
          "markdown": "Public edited notes\n[Original GitHub release](" + URL + ")"}


class PublicFeedTests(unittest.TestCase):
    def test_public_featurebase_overrides_exact_release(self):
        feed = module.build_public_feed([RELEASE], [PUBLIC])
        self.assertEqual(feed["schema"], 1)
        result = feed["releases"][0]
        self.assertEqual(result["version"], "v1.0.0")
        self.assertEqual(result["markdown"], PUBLIC["markdown"])
        self.assertEqual(result["published_at"], "2026-10-01T10:30:00Z")
        self.assertFalse(result["prerelease"])

    def test_exact_match_avoids_prefix_release(self):
        other = dict(PUBLIC, markdown="Notes " + URL + "-beta.1")
        result = module.build_public_feed([RELEASE], [other])["releases"][0]
        self.assertEqual(result["markdown"], RELEASE["body"])
        self.assertEqual(result["featurebase_url"], "")

    def test_incidental_link_does_not_assign_another_releases_notes(self):
        other = dict(PUBLIC, markdown="See previous release " + URL + "\n[Original GitHub release](" + URL + "-beta.1)")
        result = module.build_public_feed([RELEASE], [other])["releases"][0]
        self.assertEqual(result["markdown"], RELEASE["body"])
        self.assertEqual(result["featurebase_url"], "")

    def test_drafts_and_unpublished_releases_excluded(self):
        feed = module.build_public_feed([dict(RELEASE, draft=True), dict(RELEASE, published_at=None)], [PUBLIC])
        self.assertEqual(feed["releases"], [])

    def test_ambiguous_matches_fall_back(self):
        result = module.build_public_feed([RELEASE], [PUBLIC, dict(PUBLIC, url=PUBLIC["url"] + "-copy")])["releases"][0]
        self.assertEqual(result["markdown"], RELEASE["body"])

    def test_origins_exclude_credentials_ports_and_foreign_hosts(self):
        for url in ["http://a.featurebase.app", "https://a.featurebase.app.evil.test", "https://a.featurebase.app:443", "https://x@a.featurebase.app", "https://127.0.0.1", "file:///tmp/a"]:
            self.assertIsNone(module.featurebase_origin(url))
        self.assertEqual(module.featurebase_origin(PUBLIC["url"]), "https://northebridge.featurebase.app")

    def test_export_never_uses_privileged_content_or_github_token(self):
        private = {"url": PUBLIC["url"], "title": "PRIVATE TITLE", "markdownContent": "SECRET CONTENT", "state": "draft"}
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "feed.json"
            with patch.object(module, "github_releases", return_value=[RELEASE]) as github, patch.object(module, "featurebase_changelogs", return_value=[private]), patch.object(module, "public_featurebase_entries", return_value=[]):
                module.export_public_feed("NortheBridge/luminalshine", "secret-key", target)
            github.assert_called_once_with("NortheBridge/luminalshine", None)
            text = target.read_text()
            self.assertNotIn("SECRET", text)
            self.assertNotIn("PRIVATE", text)
            self.assertNotIn("secret-key", text)
            self.assertEqual(json.loads(text)["releases"][0]["markdown"], RELEASE["body"])

    def test_featurebase_outage_preserves_github_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "feed.json"
            with patch.object(module, "github_releases", return_value=[RELEASE]), patch.object(module, "featurebase_changelogs", side_effect=RuntimeError("offline")):
                module.export_public_feed("NortheBridge/luminalshine", "key", target)
            self.assertEqual(json.loads(target.read_text())["releases"][0]["markdown"], RELEASE["body"])

    def test_rss_request_is_anonymous_and_extracts_public_notes(self):
        import io
        from unittest.mock import MagicMock
        rss = b'<rss><channel><item><title>Update</title><description>Public notes</description><link>https://northebridge.featurebase.app/changelog/v1</link></item></channel></rss>'
        opener = MagicMock()
        opener.open.return_value = io.BytesIO(rss)
        with patch.object(module.urllib.request, "build_opener", return_value=opener):
            entries = module.public_featurebase_entries("https://northebridge.featurebase.app")
        request = opener.open.call_args.args[0]
        self.assertIsNone(request.get_header("Authorization"))
        self.assertIsNone(request.get_header("Cookie"))
        self.assertEqual(entries[0]["markdown"], "Public notes")

    def test_redirect_outside_featurebase_is_rejected(self):
        with self.assertRaises(RuntimeError):
            module.PublicFeaturebaseRedirect().redirect_request(None, None, 302, "", {}, "https://example.com/private")


if __name__ == "__main__":
    unittest.main()
