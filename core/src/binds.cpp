// User: Keys -- bind any key to a console line (bind / unbind / bindlist); keys act only while the game has focus and the console is closed.
#include "binds.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"

namespace fdcore::binds {
namespace {

// Key names -> Windows virtual-key codes. Detection uses GetAsyncKeyState
// (it also sees scan-code input), so VK codes, not DirectInput DIK codes.
struct KeyName {
    const char* name;
    int vk;
};
const KeyName kKeys[] = {
    {"mouse1", VK_LBUTTON}, {"mouse2", VK_RBUTTON}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1},
    {"mouse5", VK_XBUTTON2}, {"backspace", VK_BACK}, {"tab", VK_TAB}, {"enter", VK_RETURN},
    {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"pause", VK_PAUSE},
    {"capslock", VK_CAPITAL}, {"escape", VK_ESCAPE}, {"space", VK_SPACE}, {"pgup", VK_PRIOR},
    {"pgdn", VK_NEXT}, {"end", VK_END}, {"home", VK_HOME}, {"leftarrow", VK_LEFT}, {"uparrow", VK_UP},
    {"rightarrow", VK_RIGHT}, {"downarrow", VK_DOWN}, {"ins", VK_INSERT}, {"del", VK_DELETE},
    {"kp_0", VK_NUMPAD0}, {"kp_1", VK_NUMPAD1}, {"kp_2", VK_NUMPAD2}, {"kp_3", VK_NUMPAD3},
    {"kp_4", VK_NUMPAD4}, {"kp_5", VK_NUMPAD5}, {"kp_6", VK_NUMPAD6}, {"kp_7", VK_NUMPAD7},
    {"kp_8", VK_NUMPAD8}, {"kp_9", VK_NUMPAD9}, {"kp_multiply", VK_MULTIPLY}, {"kp_plus", VK_ADD},
    {"kp_minus", VK_SUBTRACT}, {"kp_del", VK_DECIMAL}, {"kp_slash", VK_DIVIDE},
    {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4}, {"f5", VK_F5}, {"f6", VK_F6},
    {"f7", VK_F7}, {"f8", VK_F8}, {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
    {"semicolon", VK_OEM_1}, {"equals", VK_OEM_PLUS}, {"comma", VK_OEM_COMMA}, {"minus", VK_OEM_MINUS},
    {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2}, {"lbracket", VK_OEM_4}, {"backslash", VK_OEM_5},
    {"rbracket", VK_OEM_6}, {"quote", VK_OEM_7},
};
// Keys the game itself uses (GameControls::Construct defaults, DIK in the
// game; listed here as VK). Binding them works but both actions happen.
const int kGameKeys[] = {'W', 'A', 'S', 'D', 'C', VK_SPACE, VK_SHIFT, VK_CONTROL, '1', '2', '3', '4',
                         VK_LBUTTON, VK_RBUTTON, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE};

constexpr int kVkCount = 256;
char g_binds[kVkCount][160];
bool g_prevDown[kVkCount];
KeyCapture g_capture = nullptr;
bool (*g_pointerCapture)() = nullptr;
bool g_isDefault[kVkCount];  // a feature's default key (BindDefault)
char g_default[kVkCount][160];  // ... and its line: FDNYCore.cfg only records changes from it
const char* g_defaultKey[kVkCount];  // ... and the name it was given, in BindDefault order
int g_nDefaults = 0;

struct Described {
    const char* key;
    const char* action;
    const char* when;
};
constexpr int kMaxDescribed = 32;
Described g_described[kMaxDescribed];
int g_nDescribed = 0;

// Name of a VK for display / FDNYCore.cfg; letters and digits are themselves.
void VkName(int vk, char* out) {
    for (const KeyName& k : kKeys)
        if (k.vk == vk) {
            lstrcpyA(out, k.name);
            return;
        }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = static_cast<char>(vk >= 'A' ? vk - 'A' + 'a' : vk);
        out[1] = 0;
        return;
    }
    wsprintfA(out, "vk%d", vk);
}

int ParseKey(const char* s) {
    for (const KeyName& k : kKeys)
        if (lstrcmpiA(k.name, s) == 0) return k.vk;
    if (s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    return -1;
}

bool IsGameKey(int vk) {
    for (int k : kGameKeys)
        if (k == vk) return true;
    return false;
}

void CmdBind(int argc, const char* const* argv) {
    if (argc < 2) {
        console::Print("usage: bind <key> [\"command\"]   (e.g. bind f5 \"fd_tp_save\")");
        return;
    }
    int vk = ParseKey(argv[1]);
    if (vk < 0) {
        console::Print("bind: unknown key \"%s\"", argv[1]);
        return;
    }
    if (vk == VK_OEM_3) {
        console::Print("bind: ` / ~ is reserved for the console");
        return;
    }
    char name[16];
    VkName(vk, name);
    if (argc == 2) {
        if (g_binds[vk][0]) console::Print("%s = \"%s\"", name, g_binds[vk]);
        else console::Print("%s is not bound", name);
        return;
    }
    // Everything after the key is the command (quotes already stripped).
    char line[160];
    int o = 0;
    for (int i = 2; i < argc; ++i) {
        for (const char* s = argv[i]; *s && o < static_cast<int>(sizeof(line)) - 2; ++s) line[o++] = *s;
        if (i + 1 < argc && o < static_cast<int>(sizeof(line)) - 2) line[o++] = ' ';
    }
    line[o] = 0;
    lstrcpynA(g_binds[vk], line, sizeof(g_binds[vk]));
    console::Print("%s = \"%s\"", name, line);
    if (IsGameKey(vk)) console::Print("note: the game also uses %s; both actions will happen", name);
    cmd::SaveConfig();
}

void CmdUnbind(int argc, const char* const* argv) {
    int vk = argc > 1 ? ParseKey(argv[1]) : -1;
    if (vk < 0) {
        console::Print("usage: unbind <key>");
        return;
    }
    g_binds[vk][0] = 0;
    cmd::SaveConfig();
}

void CmdUnbindAll(int, const char* const*) {
    for (auto& b : g_binds) b[0] = 0;
    cmd::SaveConfig();
}

void CmdBindList(int, const char* const*) {
    int n = 0;
    char name[16];
    for (int vk = 0; vk < kVkCount; ++vk)
        if (g_binds[vk][0]) {
            VkName(vk, name);
            console::Print("%s \"%s\"", name, g_binds[vk]);
            ++n;
        }
    console::Print("%d bind(s)", n);
    for (int i = 0; i < g_nDescribed; ++i)
        console::Print("%s: %s (%s; not a bind)", g_described[i].key, g_described[i].action, g_described[i].when);
}

bool OurWindowFocused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool ConsoleOpen() {
    int32_t state = 0;
    return addr::ReadI32(addr::Id::console_state, &state) && state != 0;
}

bool KeysLive_() { return OurWindowFocused() && !ConsoleOpen(); }

// Edge-triggered on key press. Ignored while the console is open (keys are
// typing there) or another window has focus; key state is still tracked so
// closing the console doesn't fire keys that were held.
void Poll() {
    const bool live = KeysLive_();
    for (int vk = 1; vk < kVkCount; ++vk) {
        if (!g_binds[vk][0]) continue;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_prevDown[vk] && live && !KeyCaptured(vk)) {
            char name[16];
            VkName(vk, name);
            log::Info("bind: %s -> %s", name, g_binds[vk]);
            if (!cmd::Dispatch(g_binds[vk])) cmd::QueueGameLine(g_binds[vk]);
        }
        g_prevDown[vk] = down;
    }
}

} // namespace

bool KeysLive() { return KeysLive_(); }
void SetPointerCapture(bool (*filter)()) { g_pointerCapture = filter; }
bool PointerCaptured() { return g_pointerCapture && g_pointerCapture(); }
void SetKeyCapture(KeyCapture filter) { g_capture = filter; }
bool KeyCaptured(int vk) { return g_capture && g_capture(vk); }

bool BindDefault(const char* key, const char* line) {
    const int vk = ParseKey(key);
    if (vk < 0 || g_binds[vk][0]) {
        log::Warn("bind: default %s -> %s not set (%s)", key, line, vk < 0 ? "unknown key" : "key taken");
        return false;
    }
    lstrcpynA(g_binds[vk], line, sizeof(g_binds[vk]));
    lstrcpynA(g_default[vk], line, sizeof(g_default[vk]));
    g_isDefault[vk] = true;
    g_defaultKey[g_nDefaults++] = key;
    return true;
}

void DescribeKey(const char* key, const char* action, const char* when) {
    if (g_nDescribed < kMaxDescribed) g_described[g_nDescribed++] = {key, action, when};
}

int DefaultCount() { return g_nDefaults; }

bool DefaultAt(int i, const char** key, const char** line) {
    if (i < 0 || i >= g_nDefaults) return false;
    *key = g_defaultKey[i];
    *line = g_default[ParseKey(g_defaultKey[i])];
    return true;
}

int DescribedCount() { return g_nDescribed; }

bool DescribedAt(int i, const char** key, const char** action, const char** when) {
    if (i < 0 || i >= g_nDescribed) return false;
    *key = g_described[i].key, *action = g_described[i].action, *when = g_described[i].when;
    return true;
}

void Init() {
    cmd::RegisterCommand("bind", "bind <key> \"<command>\": run a console line when the key is pressed; "
                                     "bind <key> shows it. Keys: a-z, 0-9, f1-f12, mouse1-mouse5, space, tab, enter, "
                                     "shift, ctrl, alt, uparrow (and the other arrows), kp_0-kp_9, ins, del, home, "
                                     "end, pgup, pgdn",
                         CmdBind);
    cmd::RegisterCommand("unbind", "unbind <key>: remove the key's bind (a default key stays unbound)", CmdUnbind);
    cmd::RegisterCommand("unbindall", "remove every bind, the default keys too", CmdUnbindAll);
    cmd::RegisterCommand("bindlist", "list the binds, then the keys features read themselves", CmdBindList);
    hooks::OnFrameBegin(Poll);
}

int AppendConfig(char* out, int cap) {
    int o = 0;
    char name[16];
    for (int vk = 0; vk < kVkCount; ++vk) {
        if (!g_binds[vk][0] && !g_isDefault[vk]) continue;
        // A default key still on its default line is not written: the file holds only the
        // player's changes, so a later default change reaches them (owner, 0.19.0).
        if (g_isDefault[vk] && lstrcmpA(g_binds[vk], g_default[vk]) == 0) continue;
        if (o + 200 > cap) break;
        VkName(vk, name);
        if (g_binds[vk][0]) o += wsprintfA(out + o, "bind %s \"%s\"\r\n", name, g_binds[vk]);
        else o += wsprintfA(out + o, "unbind %s\r\n", name);  // a default the player removed
    }
    return o;
}

} // namespace fdcore::binds
