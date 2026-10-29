"""compare_runs (the release gate behind baseline_route's expect: baseline_equivalent): each of
its seven checks passes on equivalent runs and fails on the one thing it is there to catch.
Synthetic run results, no game.
"""
import contextlib
import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "harness"))
import compare_runs  # noqa: E402

CHAINS = {f"addr.split.{i}": 0x1000 + i for i in range(12)}


def run(lap_x=(0, 100, 200), fps=(57, 57, 56), crash=None, errors_ok=True):
    samples = {"map_start": "training", "state_start": 1, "lap_state": [1] * len(lap_x),
               "module.exe": 0x400000, "module.genesis": 0x10000000,
               "lap_x": list(lap_x), "lap_y": [0] * len(lap_x), "lap_z": [0] * len(lap_x), "fps": list(fps)}
    samples.update(CHAINS)
    return {"crash": crash, "samples": samples,
            "assertions": [{"step": "log_errors", "pass": errors_ok, "detail": "0 ERROR lines"}]}


def compare(a, b, c):
    with contextlib.redirect_stdout(io.StringIO()):
        ok, checks = compare_runs.compare(a, b, c)
    return ok, {x["check"].split()[0]: x["pass"] for x in checks}


class CompareRuns(unittest.TestCase):
    def test_equivalent(self):
        ok, checks = compare(run(), run(lap_x=(0, 101, 205)), run(lap_x=(3, 110, 190)))
        self.assertTrue(ok, checks)
        self.assertEqual(len(checks), 7)

    def failing(self, core, a=None):
        ok, checks = compare(a or run(), run(), core)
        self.assertFalse(ok)
        return [k for k, v in checks.items() if not v]

    def test_crash(self):
        self.assertEqual(self.failing(run(crash="dialog")), ["1"])

    def test_other_level_or_state(self):
        c = run()
        c["samples"]["lap_state"][1] = 0
        self.assertEqual(self.failing(c), ["2"])

    def test_exe_moved(self):
        c = run()
        c["samples"]["module.exe"] = 0x500000
        self.assertEqual(self.failing(c), ["3"])

    def test_genesis_relocation_is_allowed(self):
        c = run()
        c["samples"]["module.genesis"] = 0x02390000
        self.assertTrue(compare(run(), run(), c)[0])

    def test_chain_unresolved_or_moved(self):
        c = run()
        c["samples"]["addr.split.3"] = None
        self.assertEqual(self.failing(c), ["4"])
        c = run()
        c["samples"]["addr.split.4"] = 0xDEAD
        self.assertEqual(self.failing(c), ["4"])
        # A chain the two baselines disagree on (heap) is not compared.
        a, c = run(), run()
        a["samples"]["addr.split.5"], c["samples"]["addr.split.5"] = 1, 2
        self.assertTrue(compare(a, run(), c)[0])

    def test_frame_rate(self):
        self.assertEqual(self.failing(run(fps=(50, 50, 50))), ["5"])  # 12 % slower
        self.assertTrue(compare(run(), run(), run(fps=(52, 52, 52)))[0])  # within 10 %

    def test_lap_positions(self):
        # Baselines agree within 1 u: the floor of 32 u applies.
        self.assertEqual(self.failing(run(lap_x=(0, 140, 200))), ["6"])
        self.assertTrue(compare(run(), run(), run(lap_x=(0, 131, 200)))[0])
        # Baselines 100 u apart: twice their spread is tolerated.
        self.assertTrue(compare(run(), run(lap_x=(0, 200, 200)), run(lap_x=(0, 390, 200)))[0])

    def test_core_log_errors(self):
        self.assertEqual(self.failing(run(errors_ok=False)), ["7"])


if __name__ == "__main__":
    unittest.main()
