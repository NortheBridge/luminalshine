"""Offline regression tests. Run: python3 -m unittest discover -s tests -v"""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

script = Path(__file__).resolve().parents[1] / "scripts" / "github_to_featurebase.py"
spec = importlib.util.spec_from_file_location("github_to_featurebase", script)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

PREFIX = "https://github.com/NortheBridge/luminalshine/releases/tag/"


def release(tag, date):
    return {
        "draft": False,
        "tag_name": tag,
        "name": tag,
        "body": "Some notes",
        "published_at": date,
        "html_url": PREFIX + tag,
    }


def entry(id, own_tag, created, additional_md=""):
    return {
        "id": id,
        "title": own_tag,
        "date": created,
        "markdownContent": additional_md + "\n---\n" +
        f"[Original GitHub release]({PREFIX}{own_tag})",
    }


class SynchronizationTests(unittest.TestCase):
    def setUp(self):
        self.july = release("26.07.0-beta.1", "2026-07-07T14:38:20Z")
        self.oct = release("26.10.0-beta.1", "2026-10-20T22:27:40Z")
        self.july_entry = entry("july-id", "26.07.0-beta.1", "2026-10-21T16:16:00Z")
        # In newer release notes, references to an old release MUST NOT match its entry.
        self.oct_entry = entry("oct-id", "26.10.0-beta.1", "2026-10-20T22:27:40Z",
            f"See [{self.july['tag_name']}]({self.july['html_url']}) for earlier notes.\n"
            f"Compare changes: {self.july['html_url']}\n")

    def test_source_match_ignores_other_links(self):
        matches = m.index_featurebase_entries([self.july_entry, self.oct_entry])
        self.assertEqual(matches[m.release_key(self.july["html_url"])], [self.july_entry])
        self.assertEqual(matches[m.release_key(self.oct["html_url"])], [self.oct_entry])

    def test_markdown_and_html_source_citations(self):
        html_entry = {"id": "html", "markdownContent": "", "content": (
            f'<p>Referenced: <a href="{self.oct["html_url"]}">older notes</a></p>'
            f'<p><a rel="noreferrer" href="{self.july["html_url"]}">Original '
            f'<strong>GitHub</strong> release</a></p>'
        )}
        self.assertEqual(m.source_release_keys(html_entry), {m.release_key(self.july["html_url"])})
        self.assertEqual(m.source_release_keys(self.july_entry), {m.release_key(self.july["html_url"])})

    def test_sync_fixes_only_incorrect_release_and_reports_not_ambiguous(self):
        patched = []
        def fake_call(url, **kw):
            patched.append((kw["method"], url, kw["payload"]))
            return {"date": kw["payload"]["date"]}
        with patch.object(m, "call_json", side_effect=fake_call), contextlib.redirect_stdout(io.StringIO()) as output:
            m.sync_dates([self.july, self.oct], [self.july_entry, self.oct_entry], "test", apply=True)
        self.assertEqual(patched, [("PATCH", f"{m.FEATUREBASE_URL}/july-id", {"date": "2026-07-07T14:38:20Z"})])
        self.assertIn("Matched: 2", output.getvalue())
        self.assertIn("ambiguous matches: 0", output.getvalue())
        self.assertIn("updated: 1", output.getvalue())

    def test_same_time_dry_run_and_import_detects_its_own_link(self):
        self.assertTrue(m.same_time("2026-07-07T14:38:20.000Z", "2026-07-07T07:38:20-07:00"))
        with patch.object(m, "call_json") as remote, contextlib.redirect_stdout(io.StringIO()) as output:
            m.sync_dates([self.july], [self.july_entry], "test", apply=False)
            remote.assert_not_called()
            self.assertIn("WOULD UPDATE:", output.getvalue())
        with patch.object(m, "call_json", return_value={"id": "new-id", "date": "2026-07-07T14:38:20Z"}) as remote, contextlib.redirect_stdout(io.StringIO()) as output:
            m.import_releases([self.july], [self.oct_entry], "test", publish=False)
            self.assertEqual(remote.call_count, 1, "Reference to older release must not prevent import")
            self.assertEqual(remote.call_args.kwargs["payload"]["date"], "2026-07-07T14:38:20Z")

    def test_real_duplicates_still_skip(self):
        duplicate = entry("duplicate-id", "26.07.0-beta.1", "2026-10-21T17:00:00Z")
        with patch.object(m, "call_json") as remote, contextlib.redirect_stdout(io.StringIO()) as output:
            m.sync_dates([self.july], [self.july_entry, duplicate], "test", apply=True)
            remote.assert_not_called()
            self.assertIn("duplicate source citation", output.getvalue())
            self.assertIn("ambiguous matches: 1", output.getvalue())

    def test_github_reference_url_normalization(self):
        self.assertEqual(
            m.release_key(self.july["html_url"] + "?utm_source=changelog"),
            m.release_key(self.july["html_url"]),
        )
        self.assertEqual(m.release_key(f"https://github.com/northebridge/LuminalShine/releases/tag/26.07.0-beta.1/"),
                         m.release_key(self.july["html_url"]))
        self.assertIsNone(m.release_key("https://fakegithub.com/x/y/releases/tag/26.07.0-beta.1"))


if __name__ == "__main__":
    unittest.main()
