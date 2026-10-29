"""The Debug override fixtures (tests/game/data/addr_override_*.txt) are the current address
table with one change each. The override loader leaves out every entry a fixture doesn't
list, so a stale fixture quietly disables half the table and addr_bad_signature /
addr_unknown_exe test something else. Fix: python -I tools/make_override_fixtures.py.
"""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import make_override_fixtures  # noqa: E402


class OverrideFixtures(unittest.TestCase):
    def test_fixtures_match_the_table(self):
        out = tempfile.mkdtemp(prefix="fdcore_fixtures_")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                make_override_fixtures.main(out)
            for name in ("addr_override_unknown.txt", "addr_override_badsig.txt"):
                with open(os.path.join(out, name), encoding="ascii") as f:
                    want = f.read()
                with open(os.path.join(make_override_fixtures.DATA, name), encoding="ascii") as f:
                    have = f.read()
                self.assertEqual(have, want, f"{name} is stale: run python -I tools/make_override_fixtures.py")
        finally:
            shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
