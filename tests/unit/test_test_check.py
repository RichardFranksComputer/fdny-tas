"""tools/test_check.py: the tests and the code are in step today, and each kind of drift it
exists for is caught (a test edited the way a rename in the code would leave it)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import test_check  # noqa: E402

TEST = os.path.join(REPO, "tests", "game", "console_basic.yaml")


class TestCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code = test_check.Code()

    def drift(self, old, new):
        """check() with one line of console_basic.yaml changed; the problems it reports."""
        real = test_check.read

        def read(path):
            text = real(path)
            if os.path.normcase(os.path.abspath(path)) == os.path.normcase(TEST):
                self.assertIn(old, text)
                text = text.replace(old, new, 1)
            return text
        test_check.read = read
        try:
            return test_check.check(self.code)
        finally:
            test_check.read = real

    def assertCaught(self, old, new, what):
        errors = self.drift(old, new)
        self.assertTrue(any(what in e for e in errors), f"not caught ({what}): {errors}")

    def test_repo_in_step(self):
        self.assertEqual(test_check.check(self.code), [])

    def test_drift_is_caught(self):
        self.assertCaught("- assert_same_pos:", "- assert_same_place:", "not one the harness runs")
        self.assertCaught("- type: \"cvarlist\"", "- console: \"fd_cvarlist\"", "not a registered")
        self.assertCaught("name: console.state", "name: console.status", "not an address-table entry")
        self.assertCaught("console> hello from fdnycore", "console> hello from fdnycorp", "no source string")
        self.assertCaught("console: game console present at", "console: game console missing at",
                          "no source string")
        self.assertCaught("tests/game/data/fdtest_exec.cfg", "tests/game/data/fdtest_exec2.cfg", "does not exist")
        self.assertCaught("setup: load_training", "setup: load_trainin", "does not exist")

    def test_pipe_fields_and_samples(self):
        self.assertCaught("- assert_mem: {name: cfg.im_a_developer, equals: 0}",
                          "- assert_pipe_stat: {cmd: glitchy, key: tick, min: 1}", "not registered")
        self.assertCaught("- assert_mem: {name: cfg.im_a_developer, equals: 0}",
                          "- assert_pipe_stat: {cmd: glitch, key: ticks_done, min: 1}", "written by no source")
        self.assertCaught("- assert_mem: {name: cfg.im_a_developer, equals: 0}",
                          "- assert_pipe_stat: {cmd: glitch, key: tick, min: 1, since: nowhere}",
                          "not a sample_pipe")

    def test_text_matching(self):
        c = self.code
        self.assertTrue(c.prints("module levelwatch: disabled (needs game.map_name)"))  # fields
        self.assertTrue(c.prints("pause: off (level change)"))                         # composed
        self.assertFalse(c.prints("glitch: hooks frobnicated"))
        self.assertFalse(c.prints("totally random words here"))
        self.assertTrue(c.prints("echo-me-9 placeholder", fed="echo echo-me-9 placeholder"))


if __name__ == "__main__":
    unittest.main()
