// binds modes (D65): named actions that act only while their mode is active, read through one
// press detector; rebinding and what FDNYCore.cfg holds; an exclusive mode's key messages.
// The keyboard is a table here (binds::SetKeyStateForUnitTest). The game tests keep what
// needs the game: real key presses in the editor, the viewer and the warp HUD.
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../../core/binds.h"
#include "../../core/cmd.h"
#include "unit.h"

namespace cmd = fdcore::cmd;
namespace binds = fdcore::binds;

namespace {
bool g_keys[256];
bool g_baseOn, g_toolOn, g_modalOn;
char g_dir[MAX_PATH];

bool KeyDown(int vk) { return g_keys[vk]; }
bool BaseOn() { return g_baseOn; }
bool ToolOn() { return g_toolOn; }
bool ModalOn() { return g_modalOn; }
void Frame() { binds::PollForUnitTest(); }
// One frame with the key down, then one with it up.
void Tap(int vk) {
    g_keys[vk] = true, Frame();
    g_keys[vk] = false;
}
bool CfgHas(const char* text) {
    static char buf[8192];
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", g_dir, cmd::kConfigName);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, text) != nullptr;
}
} // namespace

void TestBindModes() {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(g_dir, sizeof(g_dir), "%sfdcore_unit_binds_%lu\\", tmp, GetCurrentProcessId());
    CreateDirectoryA(g_dir, nullptr);
    unit::SetGameDir(g_dir);
    unit::ClearLog();
    binds::SetKeyStateForUnitTest(KeyDown);
    binds::SetLiveForUnitTest(true);

    CHECK(binds::Mode("t_base", "a helper", BaseOn, binds::M_SHARED));
    const int forward = binds::ActionHeld("t_base", "forward", "w", "fly");
    const int right = binds::ActionHeld("t_base", "right", "d", "strafe");
    const int slow = binds::ActionHeld("t_base", "slow", "ctrl", "slow");
    const int baseUse = binds::Action("t_base", "use", "space", "an earlier mode's press on the same key");
    CHECK(binds::Mode("t_tool", "a tool", ToolOn, binds::M_NORMAL));
    CHECK(!binds::Mode("t_tool", "named twice", ToolOn, binds::M_NORMAL));
    const int place = binds::Action("t_tool", "place", "space ins", "place");
    const int dup = binds::Action("t_tool", "duplicate", "ctrl+d", "duplicate");
    const int move = binds::ActionRepeat("t_tool", "move", "leftarrow", "move");
    CHECK(binds::Mode("t_modal", "a viewer", ModalOn, binds::M_EXCLUSIVE));
    const int play = binds::Action("t_modal", "play", "space", "play");
    const int leave = binds::Action("t_modal", "exit", "escape", "exit");
    CHECK(forward >= 0 && place >= 0 && leave >= 0);
    CHECK(binds::Action("t_nosuch", "x", "x", "x") == -1 && !binds::Pressed(-1) && !binds::Held(-1));
    CHECK(binds::Action("t_tool", "bad", "nosuchkey", "x") >= 0 && unit::LogContains("key \"nosuchkey\" not understood"));

    // An inactive mode sees nothing; an active one sees a press once and the hold after it.
    Tap(VK_SPACE);
    CHECK(!binds::Pressed(place) && !binds::Held(place));
    Frame();
    g_toolOn = true;
    g_keys[VK_SPACE] = true, Frame();
    CHECK(binds::Pressed(place) && binds::Held(place));
    Frame();
    CHECK(!binds::Pressed(place) && binds::Held(place));
    g_keys[VK_SPACE] = false, Frame();
    CHECK(!binds::Held(place));
    // The second default key works the same; a key already down when the mode starts doesn't count.
    Tap(VK_INSERT);
    CHECK(binds::Pressed(place));
    Frame();
    g_toolOn = false, g_keys[VK_INSERT] = true, Frame();
    g_toolOn = true, Frame();
    CHECK(!binds::Pressed(place) && binds::Held(place));
    g_keys[VK_INSERT] = false, Frame();

    // A press action takes its key from ordinary binds and from a mode registered before it.
    g_baseOn = true;
    g_keys[VK_SPACE] = true, Frame();
    CHECK(binds::Pressed(place) && !binds::Pressed(baseUse) && binds::KeyCaptured(VK_SPACE));
    g_keys[VK_SPACE] = false, Frame();
    CHECK(!binds::KeyCaptured(VK_SPACE));
    g_toolOn = false;
    Tap(VK_SPACE);
    CHECK(binds::Pressed(baseUse));  // the tool closed: the key is the earlier mode's again
    Frame();
    g_toolOn = true;

    // Ctrl chords: Ctrl+D is "duplicate", not "strafe right"; Ctrl alone is still "slow".
    g_keys['D'] = true, Frame();
    CHECK(binds::Held(right) && !binds::Pressed(dup));
    g_keys['D'] = false, Frame();
    g_keys[VK_CONTROL] = true, Frame();
    CHECK(binds::Held(slow));
    g_keys['D'] = true, Frame();
    CHECK(binds::Pressed(dup) && !binds::Held(right) && binds::Held(slow));
    g_keys['D'] = g_keys[VK_CONTROL] = false, Frame();
    // A held action never takes a key: W flies and an ordinary bind on W still fires.
    g_keys['W'] = true, Frame();
    CHECK(binds::Held(forward) && !binds::KeyCaptured('W'));
    // Solo: a tool in a drag freezes the modes under it.
    binds::Solo("t_tool", true), Frame();
    CHECK(!binds::Held(forward));
    binds::Solo("t_tool", false), Frame();
    CHECK(binds::Held(forward));
    g_keys['W'] = false, Frame();

    // Repeat: once on the press, then again while held (300 ms, then every 100 ms).
    g_keys[VK_LEFT] = true, Frame();
    CHECK(binds::Pressed(move));
    Frame();
    CHECK(!binds::Pressed(move));
    Sleep(330), Frame();
    CHECK(binds::Pressed(move));
    Frame();
    CHECK(!binds::Pressed(move));
    g_keys[VK_LEFT] = false, Frame();

    // Rebinding: a key does one thing in a mode; FDNYCore.cfg holds only the changes.
    CHECK(strcmp(binds::KeyLabel(place), "Space") == 0 && strcmp(binds::KeyLabel(dup), "Ctrl+D") == 0);
    CHECK(cmd::SaveConfig() && !CfgHas("t_tool"));
    CHECK(cmd::Dispatch("bind t_tool f5 place") && CfgHas("bind t_tool f5 place") && !CfgHas("unbind t_tool"));
    Tap(VK_F5);
    CHECK(binds::Pressed(place));
    CHECK(cmd::Dispatch("unbind t_tool space") && CfgHas("unbind t_tool space"));
    CHECK(strcmp(binds::KeyLabel(place), "Ins") == 0);
    Frame(), Tap(VK_SPACE);
    CHECK(!binds::Pressed(place));
    CHECK(cmd::Dispatch("bind t_tool f5 duplicate") && CfgHas("bind t_tool f5 duplicate") && !CfgHas("bind t_tool f5 place"));
    CHECK(cmd::Dispatch("bind t_tool space place") && !CfgHas("unbind t_tool space"));  // back on its default: gone again
    CHECK(cmd::Dispatch("bind t_tool default") && !CfgHas("t_tool") && strcmp(binds::KeyLabel(place), "Space") == 0);
    CHECK(cmd::Dispatch("unbind t_tool space") && strcmp(binds::KeyLabel(place), "Ins") == 0);
    CHECK(cmd::Dispatch("bind t_tool f5 nosuch") && unit::LogContains("t_tool has no action \"nosuch\""));
    CHECK(cmd::Dispatch("bind t_tool") && unit::LogContains("console>   place"));
    char hint[2][binds::kHintLen];
    CHECK(binds::HintLines("t_tool", hint, 2) == 1 && strstr(hint[0], "Ins: place") && strstr(hint[0], "Left"));
    // More actions than lines: never more than it was given (a panel's rows are a fixed array).
    CHECK(binds::Mode("t_many", "many", ToolOn, binds::M_NORMAL));
    binds::Action("t_many", "a_rather_long_action_name_number_one", "f1", "x");
    binds::Action("t_many", "a_rather_long_action_name_number_two", "f2", "x");
    binds::Action("t_many", "a_rather_long_action_name_number_three", "f3", "x");
    binds::Action("t_many", "a_rather_long_action_name_number_four", "f4", "x");
    binds::Action("t_many", "a_rather_long_action_name_number_five", "f6", "x");
    CHECK(binds::HintLines("t_many", hint, 2) == 2 && binds::HintLines("t_many", hint, 1) == 1);
    CHECK(binds::HintLines("t_nosuch", hint, 2) == 0);
    Frame();

    // An exclusive mode: other modes and ordinary binds get no keys; a shared mode stays live.
    g_modalOn = true;
    g_keys[VK_SPACE] = g_keys['W'] = true, Frame();
    CHECK(binds::Pressed(play) && !binds::Pressed(place) && binds::Held(forward) && binds::KeyCaptured('P'));
    g_keys[VK_SPACE] = g_keys['W'] = false, Frame();
    // Its key messages are consumed, and a tap shorter than a frame still counts once.
    CHECK(binds::MessageForUnitTest(WM_KEYDOWN, VK_ESCAPE, 0) && binds::MessageForUnitTest(WM_KEYUP, VK_ESCAPE, 0));
    CHECK(!binds::MessageForUnitTest(WM_KEYDOWN, 'P', 0));  // not one of its keys: the game's
    Frame();
    CHECK(binds::Pressed(leave));
    Frame();
    CHECK(!binds::Pressed(leave));
    // Ending on a held key: that key stays consumed until its release.
    g_keys[VK_ESCAPE] = true, Frame();
    CHECK(binds::Pressed(leave));
    binds::SuppressHeld();
    g_modalOn = false, Frame();
    CHECK(binds::MessageForUnitTest(WM_KEYDOWN, VK_ESCAPE, 1L << 30));  // the rest of that press
    g_keys[VK_ESCAPE] = false;
    CHECK(binds::MessageForUnitTest(WM_KEYUP, VK_ESCAPE, 0));
    CHECK(!binds::MessageForUnitTest(WM_KEYDOWN, VK_ESCAPE, 0));  // the next press is the game's
    Frame();

    // Keys not live (console open, another window, no level): nothing acts.
    binds::SetLiveForUnitTest(false);
    Tap(VK_INSERT);
    CHECK(!binds::Pressed(place) && !binds::Held(place));

    binds::SetKeyStateForUnitTest(nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", g_dir, cmd::kConfigName);
    DeleteFileA(path);
    RemoveDirectoryA(g_dir);
}
