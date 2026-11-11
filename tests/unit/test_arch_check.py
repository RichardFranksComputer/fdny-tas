"""arch_check (docs/architecture.md, R1-R13) on a copy of the source: clean, it passes; with one
seeded violation per rule, it reports exactly that rule (D47). Without this, a rule that
silently stopped matching would only show as a passing build.
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
import arch_check  # noqa: E402

FEATURE = """// Tests: tests/game/core_loads.yaml
// User: Probe -- a fixture feature for the arch_check unit test.
#include "log.h"
{body}
FDCORE_MODULE(probe, "probe", nullptr, 0, nullptr, nullptr, 99)
"""
LOG_CPP = os.path.join(REPO, "core", "log.cpp")


class ArchCheck(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="fdcore_arch_")
        shutil.copytree(os.path.join(REPO, "core"), os.path.join(self.root, "core"),
                        ignore=shutil.ignore_patterns("gen"))
        shutil.copytree(os.path.join(REPO, "features"), os.path.join(self.root, "features"))
        shutil.copytree(os.path.join(REPO, "tests"), os.path.join(self.root, "tests"),  # // Tests: names them
                        ignore=shutil.ignore_patterns("__pycache__"))
        for k, v in {"REPO": self.root, "CORE": os.path.join(self.root, "core"),
                     "FEAT": os.path.join(self.root, "features")}.items():
            setattr(arch_check, k, v)
        arch_check.violations.clear()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def write(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def rules(self):
        arch_check.violations.clear()
        with contextlib.redirect_stdout(io.StringIO()):
            code = arch_check.main()
        found = sorted({v.split("]")[0][1:].split()[0] for v in arch_check.violations})
        return code, found

    def test_clean_source_passes(self):
        self.assertEqual(self.rules(), (0, []))

    def test_r1_core_includes_feature(self):
        self.write("core/log.cpp", open(LOG_CPP, encoding="utf-8").read() + '\n#include "glitch.h"\n')
        self.assertEqual(self.rules(), (1, ["R1"]))

    def test_r1_core_string_names_feature_setting(self):
        # A feature's variable inside a core string, also as the start of a longer name.
        self.write("core/log.cpp", open(LOG_CPP, encoding="utf-8").read()
                   + '\nconst char* kProbe = "fd_hud_warp_contact: hint 0";\n')
        self.assertEqual(self.rules(), (1, ["R1"]))

    def test_r1_core_names_in_comments_are_fine(self):
        self.write("core/log.cpp", open(LOG_CPP, encoding="utf-8").read() + "\n// fd_warp turns the warp HUD on\n")
        self.assertEqual(self.rules(), (0, []))

    def test_r10_init_refuses_silently(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="bool Init() {\n    if (!g) return false;\n"
                                                                   "    return true;\n}\nbool g;"))
        self.assertEqual(self.rules(), (1, ["R10"]))

    def test_r10_init_logs_its_refusal(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="bool g;\nbool Init() {\n    if (!g) {\n"
                                                                   "        log::Warn(\"probe: no g\",\n"
                                                                   "                  1);\n        return false;\n"
                                                                   "    }\n    return true;\n}"))
        self.assertEqual(self.rules(), (0, []))

    def test_r10_refusal_logged_elsewhere_allowed(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="bool g;\nbool Init() {\n    if (!g) return false;"
                                                                   "  // arch-allow: R10 logged by the early step\n"
                                                                   "    return true;\n}"))
        self.assertEqual(self.rules(), (0, []))

    def test_r11_feature_names_other_features_setting(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body='void f() { cmd::FindCvar("fd_warp"); }'))
        self.assertEqual(self.rules(), (1, ["R11"]))

    def test_r11_own_and_core_settings_are_fine(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='cmd::Cvar g_x = {"fd_probe", "0", "x", false, false, nullptr, {0}};\n'
                 'void f() { cmd::FindCvar("fd_probe"); cmd::FindCvar("fd_profile"); }'))
        self.assertEqual(self.rules(), (0, []))

    def test_r11_allowed_with_reason(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='void f() { cmd::FindCvar("fd_warp"); }  // arch-allow: R11 fixture'))
        self.assertEqual(self.rules(), (0, []))

    def test_r12_console_line_named_after_a_command(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body='void f() { console::Print("fd_probe_do: done"); }'))
        self.assertEqual(self.rules(), (1, ["R12"]))

    def test_r12_bare_sentence(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body='void f() { console::Print("Exit the viewer first."); }'))
        self.assertEqual(self.rules(), (1, ["R12"]))

    def test_r12_tag_usage_and_list_rows_are_fine(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='void f() { console::Print("probe: done %d", 1); console::Print("usage: fd_probe <x>");\n'
                 '  console::Print("  %s", "row"); console::Print("%2d %s", 1, "row"); }'))
        self.assertEqual(self.rules(), (0, []))

    def test_r13_toggle_command(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='void f() { cmd::RegisterCommand("fd_probe_toggle", "x", nullptr); }'))
        self.assertEqual(self.rules(), (1, ["R13"]))

    def test_r13_three_nouns_in_one_feature(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='void f() { cmd::RegisterCommand("fd_probe_do", "x", nullptr); cmd::RegisterCommand("fd_other_do", "x", nullptr);\n'
                 '  cmd::RegisterCommand("fd_third_do", "x", nullptr); }'))
        self.assertEqual(self.rules(), (1, ["R13"]))

    def test_r13_names_in_the_grammar_are_fine(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='cmd::Cvar g_x = {"fd_probe", "0", "x", false, false, nullptr, {0}};\n'
                 'void f() { cmd::RegisterCommand("fd_probe_list", "x", nullptr); cmd::RegisterCommand("fd_dbg_probe", "x", nullptr); }'))
        self.assertEqual(self.rules(), (0, []))

    def test_r2_feature_includes_other_feature(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body='#include "glitch.h"'))
        self.assertEqual(self.rules(), (1, ["R2"]))

    def test_r3_second_module_in_folder(self):
        self.write("features/glitch/glitch_extra.cpp", FEATURE.format(body="").replace("probe,", "glitch_extra,"))
        self.assertEqual(self.rules(), (1, ["R3"]))

    def test_r3_named_test_missing(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="").replace("core_loads", "no_such_test"))
        self.assertEqual(self.rules(), (1, ["R3"]))

    def test_r4_raw_address(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="int* p = (int*)0x0045D717;"))
        self.assertEqual(self.rules(), (1, ["R4"]))

    def test_r4_allowed_with_reason(self):
        self.write("features/probe/probe.cpp",
                   FEATURE.format(body="int* p = (int*)0x0045D717;  // arch-allow: R4 fixture"))
        self.assertEqual(self.rules(), (0, []))

    def test_r4_address_in_comment_is_fine(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="// SpawnAtSpot is 0x0045D5B0"))
        self.assertEqual(self.rules(), (0, []))

    def test_r5_drawing_outside_gfx(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="auto id = Id::genesis_draw_bitmap;"))
        self.assertEqual(self.rules(), (1, ["R5"]))

    def test_r8_late_frame_outside_gfx(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="void f() { hooks::OnFrameEndLate(nullptr); }"))
        self.assertEqual(self.rules(), (1, ["R8"]))

    def test_r9_missing_user_line(self):
        self.write("features/probe/probe.cpp",
                   FEATURE.format(body="").replace("// User: Probe -- a fixture feature for the arch_check unit test.\n",
                                                   ""))
        self.assertEqual(self.rules(), (1, ["R9"]))

    def test_r9_keys_without_describekey(self):
        self.write("features/probe/probe.cpp", FEATURE.format(body="bool k() { return GetAsyncKeyState('Q'); }"))
        self.assertEqual(self.rules(), (1, ["R9"]))

    def test_r9_reading_the_keyboard_is_a_binds_action(self):
        # Declared for the sheet, but still read directly: a control must be a binds action.
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='bool k() { return GetAsyncKeyState(81); }\nvoid d() { binds::DescribeKey("q", "x", "y"); }'))
        self.assertEqual(self.rules(), (1, ["R9"]))

    def test_r9_observed_key_allowed_with_reason(self):
        self.write("features/probe/probe.cpp", FEATURE.format(
            body='bool k() { return GetAsyncKeyState(81); }  // arch-allow: R9 observed, not a control\n'
                 'void d() { binds::DescribeKey("q", "x", "y"); }'))
        self.assertEqual(self.rules(), (0, []))


if __name__ == "__main__":
    unittest.main()
