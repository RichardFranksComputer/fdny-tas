"""docgen (D41, D45): the release-notes section the workflow publishes, and the player reference
read from the real source (D47).
"""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import docgen  # noqa: E402

CHANGES = """# FDNYCore release notes

Intro text.

## 0.20.0

**New:**
- ghosts.

## 0.19.0

**New:**
- pause.

### Not a version heading
- still 0.19.0.

## 0.19.0-rc1

- the candidate.
"""


class ReleaseNotes(unittest.TestCase):
    def test_section_up_to_next_version(self):
        self.assertEqual(docgen.release_notes(CHANGES, "0.20.0"), "**New:**\n- ghosts.")

    def test_subheadings_stay_inside(self):
        self.assertEqual(docgen.release_notes(CHANGES, "0.19.0"),
                         "**New:**\n- pause.\n\n### Not a version heading\n- still 0.19.0.")

    def test_prefix_is_not_a_match(self):
        # "0.19.0" must not pick up "## 0.19.0-rc1", nor "0.19" the "## 0.19.0" section.
        self.assertEqual(docgen.release_notes(CHANGES, "0.19.0-rc1"), "- the candidate.")
        self.assertIsNone(docgen.release_notes(CHANGES, "0.19"))

    def test_last_section_runs_to_the_end(self):
        self.assertEqual(docgen.release_notes(CHANGES, "0.19.0-rc1"), "- the candidate.")

    def test_crlf(self):
        self.assertEqual(docgen.release_notes(CHANGES.replace("\n", "\r\n"), "0.20.0"),
                         "**New:**\r\n- ghosts.")

    def test_missing_version(self):
        self.assertIsNone(docgen.release_notes(CHANGES, "9.9.9"))

    def test_repository_changes_has_the_current_version(self):
        # The version in VERSION has its notes: a bump without them fails here, not at release time.
        version = open(os.path.join(REPO, "VERSION"), encoding="utf-8").read().strip()
        notes = docgen.release_notes(open(os.path.join(REPO, "CHANGES.md"), encoding="utf-8").read(), version)
        self.assertTrue(notes, f"CHANGES.md has no '## {version}' section")

    def test_cli_exit_codes(self):
        run = lambda *a: subprocess.run([sys.executable, "-I", os.path.join(REPO, "tools", "docgen.py"), *a],
                                        capture_output=True, text=True)
        self.assertEqual(run("notes", "9.9.9").returncode, 1)
        version = open(os.path.join(REPO, "VERSION"), encoding="utf-8").read().strip()
        r = run("notes", version)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(r.stdout.strip())


class Sheet(unittest.TestCase):
    def test_reads_the_source(self):
        reg = docgen.read_source()
        names = {c["name"] for c in reg["cvars"]}
        for expected in ("fd_profile", "fd_warp", "fd_spawns", "fd_record"):
            self.assertIn(expected, names)
        self.assertTrue(all(s["title"] for s in reg["sections"]))

    def test_release_sheet_names_the_versioned_file(self):
        reg = docgen.read_source()
        asi = os.path.join(HERE, "test_docgen.py")  # any file: the sheet only hashes it
        text = docgen.sheet(reg, asi)
        version = docgen.version()
        self.assertIn(f"`FDNYCore-{version}.asi`", text)
        self.assertIn("remove any older", text)


if __name__ == "__main__":
    unittest.main()
