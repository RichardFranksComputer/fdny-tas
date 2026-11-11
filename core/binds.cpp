// Tests: tests/game/keys_modes.yaml tests/unit/test_binds.cpp tests/game/keys_default.yaml tests/game/vars_binds_persist.yaml tests/game/autoexec.yaml tests/unit/test_cmd.cpp
// User: Keys -- bind any key to a console line (bind / unbind / bindlist); keys act only inside a level while the game has focus and the console is closed; menu typing never toggles gameplay bindings.
#include "binds.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "state.h"

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
bool (*g_pointerCapture)() = nullptr;
bool (*g_extraPointer[4])();
int g_nExtraPointer;
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

// --- Modes (binds.h) ---------------------------------------------------------------------
struct Chord {
    int vk;
    bool ctrl;
};
constexpr int kMaxModes = 12, kMaxActions = 96, kChords = 3;
enum How { H_PRESS, H_REPEAT, H_HELD };
struct ModeRec {
    const char* name;
    const char* when;
    ModeActive active;
    ModeKind kind;
    bool on;  // this frame
};
struct ActionRec {
    int mode;
    const char* id;
    const char* keys;
    const char* help;
    How how;
    Chord chord[kChords], def[kChords];
    int n, nDef;
    bool pressed, held, armed;
    DWORD repeatAt;
    char label[24];
};
ModeRec g_modes[kMaxModes];
ActionRec g_actions[kMaxActions];
int g_nModes = 0, g_nActions = 0, g_exclusive = -1, g_solo = -1;
input::Key g_key[kVkCount];
bool g_down[kVkCount], g_edge[kVkCount];
bool g_suppress[kVkCount];  // consumed until released (an exclusive mode's keys)
constexpr DWORD kRepeatFirstMs = 300, kRepeatMs = 100;

#ifdef FDCORE_UNIT
bool (*g_unitDown)(int) = nullptr;
int g_unitLive = -1;
#endif
bool Down(int vk) {
#ifdef FDCORE_UNIT
    if (g_unitDown) return g_unitDown(vk);
#endif
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int FindMode(const char* name) {
    for (int i = 0; i < g_nModes; ++i)
        if (lstrcmpiA(g_modes[i].name, name) == 0) return i;
    return -1;
}

bool SameChord(const Chord& a, const Chord& b) { return a.vk == b.vk && a.ctrl == b.ctrl; }

// "ctrl+c", "space", "x".
bool ParseChord(const char* s, Chord* out) {
    out->ctrl = false;
    if ((s[0] == 'c' || s[0] == 'C') && (s[1] == 't' || s[1] == 'T') && (s[2] == 'r' || s[2] == 'R') &&
        (s[3] == 'l' || s[3] == 'L') && s[4] == '+' && s[5])
        out->ctrl = true, s += 5;
    out->vk = ParseKey(s);
    return out->vk > 0;
}

// "Ctrl+C", "Space", "PgUp": for hint lines and lists.
void ChordLabel(const Chord& c, char* out) {
    char name[16];
    VkName(c.vk, name);
    if (name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
    if (name[0] == 'P' && name[1] == 'g' && name[2] >= 'a') name[2] = static_cast<char>(name[2] - 'a' + 'A');
    wsprintfA(out, "%s%s", c.ctrl ? "Ctrl+" : "", name);
}
void ChordName(const Chord& c, char* out) {  // as typed: "ctrl+c"
    char name[16];
    VkName(c.vk, name);
    wsprintfA(out, "%s%s", c.ctrl ? "ctrl+" : "", name);
}
void Relabel(ActionRec& a) {
    if (a.n) ChordLabel(a.chord[0], a.label);
    else lstrcpyA(a.label, "unbound");
}

int AddAction(const char* mode, const char* id, const char* keys, const char* help, How how) {
    const int m = FindMode(mode);
    if (m < 0 || g_nActions >= kMaxActions) {
        log::Error("binds: action %s %s not registered (%s)", mode, id, m < 0 ? "unknown mode" : "table full");
        return -1;
    }
    ActionRec& a = g_actions[g_nActions];
    a = {};
    a.mode = m, a.id = id, a.keys = keys, a.help = help, a.how = how;
    char tok[24];
    for (const char* s = keys; *s;) {
        while (*s == ' ') ++s;
        int o = 0;
        while (*s && *s != ' ' && o < static_cast<int>(sizeof(tok)) - 1) tok[o++] = *s++;
        tok[o] = 0;
        Chord c;
        if (!o) break;
        if (!ParseChord(tok, &c) || a.n >= kChords) {
            log::Error("binds: action %s %s: key \"%s\" not understood", mode, id, tok);
            continue;
        }
        a.chord[a.n] = a.def[a.n] = c, ++a.n;
    }
    a.nDef = a.n;
    Relabel(a);
    return g_nActions++;
}

// Does this mode's action see keys now? (active, not shut out by an exclusive mode or a solo)
bool ModeLive(int m) {
    if (!g_modes[m].on) return false;
    if (g_solo >= 0 && m != g_solo) return false;
    return g_exclusive < 0 || m == g_exclusive || g_modes[m].kind == M_SHARED;
}

// Is a Ctrl chord on vk bound in a live mode? Then the plain key doesn't act while Ctrl is down.
bool CtrlChordOn(int vk) {
    for (int i = 0; i < g_nActions; ++i) {
        const ActionRec& a = g_actions[i];
        if (!ModeLive(a.mode)) continue;
        for (int k = 0; k < a.n; ++k)
            if (a.chord[k].ctrl && a.chord[k].vk == vk) return true;
    }
    return false;
}
bool ChordDown(const Chord& c) {
    if (!g_down[c.vk]) return false;
    const bool ctrl = g_down[VK_CONTROL];
    return c.ctrl ? ctrl : !(ctrl && c.vk != VK_CONTROL && CtrlChordOn(c.vk));
}
// A press action of a live mode registered after `mode` holds this chord's key now.
bool Claimed(int mode, const Chord& c) {
    for (int i = 0; i < g_nActions; ++i) {
        const ActionRec& a = g_actions[i];
        if (a.how == H_HELD || a.mode <= mode || !ModeLive(a.mode)) continue;
        for (int k = 0; k < a.n; ++k)
            if (a.chord[k].vk == c.vk && ChordDown(a.chord[k])) return true;
    }
    return false;
}

// A key an exclusive mode consumes: bound in a live mode, and not a modifier.
bool Reserved(uintptr_t vk) {
    if (vk >= kVkCount || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return false;
    for (int i = 0; i < g_nActions; ++i) {
        const ActionRec& a = g_actions[i];
        if (!ModeLive(a.mode)) continue;
        for (int k = 0; k < a.n; ++k)
            if (a.chord[k].vk == static_cast<int>(vk)) return true;
    }
    return false;
}

bool KeysLive_();

// An exclusive mode's keys, from the window's messages: consumed (the game and the console
// see none), and a tap shorter than a frame is kept for the next poll.
bool ModeMessage(unsigned message, uintptr_t w, intptr_t l) {
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool altTab = w == VK_TAB && Down(VK_MENU);
    if ((keyDown || keyUp) && w < kVkCount && g_suppress[w] && !altTab) {
        if (keyUp) g_suppress[w] = false, g_key[w].Release();
        return true;
    }
    if (message == WM_CHAR) {
        const uintptr_t upper = w >= 'a' && w <= 'z' ? w - 'a' + 'A' : w;
        if (upper < kVkCount && g_suppress[upper]) return true;
    }
    if (g_exclusive < 0 || !KeysLive_()) return false;
    // Alt+Tab and the console key stay with Windows and the game's console.
    if ((keyDown || keyUp) && Reserved(w) && !altTab) {
        if (keyDown) g_key[w].Message((l & (1L << 30)) != 0);
        else g_key[w].Release();
        g_suppress[w] = keyDown;
        return true;
    }
    if (message == WM_CHAR) {
        const uintptr_t upper = w >= 'a' && w <= 'z' ? w - 'a' + 'A' : w;
        if (Reserved(upper)) return true;
    }
    return false;
}

// Frame begin: one press detector for every key, then every mode's actions.
void PollModes(bool live) {
    for (int vk = 1; vk < kVkCount; ++vk) {
        g_down[vk] = Down(vk);
        g_edge[vk] = g_key[vk].Poll(g_down[vk], live);
    }
    g_exclusive = -1;
    for (int m = 0; m < g_nModes; ++m) {
        g_modes[m].on = live && g_modes[m].active && g_modes[m].active();
        if (g_modes[m].on && g_modes[m].kind == M_EXCLUSIVE) g_exclusive = m;
    }
    if (g_solo >= 0 && !g_modes[g_solo].on) g_solo = -1;
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_nActions; ++i) {
        ActionRec& a = g_actions[i];
        bool held = false, edge = false;
        if (ModeLive(a.mode))
            for (int k = 0; k < a.n; ++k) {
                const Chord& c = a.chord[k];
                const bool down = ChordDown(c);
                // A tap that began and ended between two polls (an exclusive mode's key message).
                const bool tap = !c.ctrl && g_edge[c.vk] && !g_down[c.vk];
                if ((!down && !tap) || Claimed(a.mode, c)) continue;
                held = held || down;
                edge = edge || g_edge[c.vk];
            }
        a.held = held;
        a.pressed = edge;
        if (a.pressed) a.armed = true, a.repeatAt = now + kRepeatFirstMs;
        else if (!held) a.armed = false;
        else if (a.how == H_REPEAT && a.armed && static_cast<int32_t>(now - a.repeatAt) >= 0)
            a.pressed = true, a.repeatAt = now + kRepeatMs;
    }
}

// bind <mode> [<key> <action>] / unbind <mode> <key>
void PrintMode(int m) {
    console::Print("%s (%s):", g_modes[m].name, g_modes[m].when);
    for (int i = 0; i < g_nActions; ++i) {
        const ActionRec& a = g_actions[i];
        if (a.mode != m) continue;
        char keys[80] = "";
        int o = 0;
        for (int k = 0; k < a.n; ++k) {
            char name[24];
            ChordName(a.chord[k], name);
            o += wsprintfA(keys + o, "%s%s", k ? " " : "", name);
        }
        console::Print("  %-12s %-18s %s", a.id, a.n ? keys : "(unbound)", a.help);
    }
}
void UnbindIn(int m, const Chord& c) {
    for (int i = 0; i < g_nActions; ++i) {
        ActionRec& a = g_actions[i];
        if (a.mode != m) continue;
        for (int k = 0; k < a.n; ++k)
            if (SameChord(a.chord[k], c)) {
                for (int j = k; j + 1 < a.n; ++j) a.chord[j] = a.chord[j + 1];
                --a.n, --k;
            }
        Relabel(a);
    }
}
void CmdBindMode(int m, int argc, const char* const* argv) {
    if (argc == 2) return PrintMode(m);
    if (argc == 3 && lstrcmpiA(argv[2], "default") == 0) {  // the mode's keys as shipped
        for (int i = 0; i < g_nActions; ++i) {
            ActionRec& a = g_actions[i];
            if (a.mode != m) continue;
            for (int k = 0; k < a.nDef; ++k) a.chord[k] = a.def[k];
            a.n = a.nDef;
            Relabel(a);
        }
        console::Print("%s: default keys", g_modes[m].name);
        cmd::SaveConfig();
        return;
    }
    Chord c;
    if (argc != 4 || !ParseChord(argv[2], &c)) {
        console::Print("usage: bind %s <key> <action>   (bind %s lists the actions, bind %s default restores "
                       "its keys; keys like x, space, ctrl+c)", g_modes[m].name, g_modes[m].name, g_modes[m].name);
        return;
    }
    for (int i = 0; i < g_nActions; ++i) {
        ActionRec& a = g_actions[i];
        if (a.mode != m || lstrcmpiA(a.id, argv[3]) != 0) continue;
        UnbindIn(m, c);  // a key does one thing in a mode
        if (a.n >= kChords) {
            console::Print("bind: %s %s has %d keys already (unbind %s <key> first)", g_modes[m].name, a.id, kChords,
                           g_modes[m].name);
            return;
        }
        a.chord[a.n++] = c;
        Relabel(a);
        char name[24];
        ChordName(c, name);
        console::Print("%s: %s = %s", g_modes[m].name, name, a.id);
        cmd::SaveConfig();
        return;
    }
    console::Print("bind: %s has no action \"%s\" (bind %s lists them)", g_modes[m].name, argv[3], g_modes[m].name);
}

void CmdBind(int argc, const char* const* argv) {
    if (argc < 2) {
        console::Print("usage: bind <key> [\"command\"]   (e.g. bind f5 \"echo saved\"); bind <mode> <key> <action>");
        return;
    }
    if (const int m = FindMode(argv[1]); m >= 0) return CmdBindMode(m, argc, argv);
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
    if (const int m = argc > 2 ? FindMode(argv[1]) : -1; m >= 0) {
        Chord c;
        if (!ParseChord(argv[2], &c)) return console::Print("unbind: unknown key \"%s\"", argv[2]);
        UnbindIn(m, c);
        cmd::SaveConfig();
        return;
    }
    int vk = argc > 1 ? ParseKey(argv[1]) : -1;
    if (vk < 0) {
        console::Print("usage: unbind <key>   or   unbind <mode> <key>");
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
    // The modes by name only: every action of every mode is some 60 console lines, which
    // held the game's window for seconds on the VM (docs_registry, 2026-11-11).
    char modes[160] = "";
    int o = 0;
    for (int m = 0; m < g_nModes && o < 140; ++m) o += wsprintfA(modes + o, " %s", g_modes[m].name);
    if (g_nModes) console::Print("modes:%s (bind <mode> lists a mode's actions and keys)", modes);
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

bool KeysLive_() {
#ifdef FDCORE_UNIT
    if (g_unitLive >= 0) return g_unitLive != 0;
#endif
    return state::Current().gameState == 1 && OurWindowFocused() && !ConsoleOpen();
}

// The game window's messages (OnWindowMessage): one subclass, installed from Poll.
MessageFilter g_msgFilters[4];
int g_nMsgFilters = 0;
HWND g_window = nullptr;
WNDPROC g_gameProc = nullptr;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ModeMessage(message, w, l)) return 0;
    for (int i = 0; i < g_nMsgFilters; ++i)
        if (g_msgFilters[i](message, w, l)) return 0;
    return CallWindowProcA(g_gameProc, window, message, w, l);
}

// The game's main window: our process's largest visible top-level window without an owner
// (the console and message boxes are smaller or owned).
BOOL CALLBACK FindGameWindow(HWND window, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    RECT r;
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        !GetClientRect(window, &r))
        return TRUE;
    auto* best = reinterpret_cast<HWND*>(found);
    RECT b = {};
    if (!*best || (GetClientRect(*best, &b) && r.right * r.bottom > b.right * b.bottom)) *best = window;
    return TRUE;
}

void InstallWindowProc() {
    bool exclusive = false;  // an exclusive mode needs the window's messages too
    for (int m = 0; m < g_nModes; ++m) exclusive = exclusive || g_modes[m].kind == M_EXCLUSIVE;
    if ((!g_nMsgFilters && !exclusive) || (g_window && IsWindow(g_window))) return;
    g_window = nullptr;
    HWND found = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
    if (!found) return;
    auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (!previous) {
        static bool warned = false;
        if (!warned) log::Warn("binds: window messages unavailable (%lu)", GetLastError()), warned = true;
        return;
    }
    g_window = found, g_gameProc = previous;
    log::Info("binds: window messages from %p (%d filter(s))", found, g_nMsgFilters);
}

// Edge-triggered on key press. Ignored while the console is open (keys are
// typing there) or another window has focus; key state is still tracked so
// closing the console doesn't fire keys that were held.
void Poll() {
    InstallWindowProc();
    const bool live = KeysLive_();
    PollModes(live);
    for (int vk = 1; vk < kVkCount; ++vk) {
        if (!g_binds[vk][0] || !g_edge[vk] || KeyCaptured(vk)) continue;
        char name[16];
        VkName(vk, name);
        log::Info("bind: %s -> %s", name, g_binds[vk]);
        if (!cmd::Dispatch(g_binds[vk])) cmd::QueueGameLine(g_binds[vk]);
    }
}

} // namespace

bool KeysLive() { return KeysLive_(); }
bool OnWindowMessage(MessageFilter filter) {
    if (!filter) return false;
    for (int i = 0; i < g_nMsgFilters; ++i) if (g_msgFilters[i] == filter) return true;
    if (g_nMsgFilters >= 4) return false;
    g_msgFilters[g_nMsgFilters++] = filter; return true;
}
void SetPointerCapture(bool (*filter)()) { g_pointerCapture = filter; }
bool PointerCaptured() {
    if (g_pointerCapture && g_pointerCapture()) return true;
    for (int i = 0; i < g_nExtraPointer; ++i) if (g_extraPointer[i]()) return true;
    return false;
}
bool AddPointerCapture(bool (*filter)()) {
    if (!filter) return false;
    for (int i = 0; i < g_nExtraPointer; ++i) if (g_extraPointer[i] == filter) return true;
    if (g_nExtraPointer >= 4) return false;
    g_extraPointer[g_nExtraPointer++] = filter; return true;
}
bool KeyCaptured(int vk) {
    if (g_exclusive >= 0) return true;
    if (vk <= 0 || vk >= kVkCount) return false;
    for (int i = 0; i < g_nActions; ++i) {
        const ActionRec& a = g_actions[i];
        if (a.how == H_HELD || !ModeLive(a.mode)) continue;
        for (int k = 0; k < a.n; ++k)
            if (a.chord[k].vk == vk && ChordDown(a.chord[k])) return true;
    }
    return false;
}

bool Mode(const char* name, const char* when, ModeActive active, ModeKind kind) {
    if (g_nModes >= kMaxModes || FindMode(name) >= 0) {
        log::Error("binds: mode %s not registered (%s)", name, g_nModes >= kMaxModes ? "table full" : "named twice");
        return false;
    }
    g_modes[g_nModes++] = {name, when, active, kind, false};
    return true;
}
int Action(const char* mode, const char* id, const char* keys, const char* help) {
    return AddAction(mode, id, keys, help, H_PRESS);
}
int ActionRepeat(const char* mode, const char* id, const char* keys, const char* help) {
    return AddAction(mode, id, keys, help, H_REPEAT);
}
int ActionHeld(const char* mode, const char* id, const char* keys, const char* help) {
    return AddAction(mode, id, keys, help, H_HELD);
}
bool Pressed(int action) { return action >= 0 && action < g_nActions && g_actions[action].pressed; }
bool Held(int action) { return action >= 0 && action < g_nActions && g_actions[action].held; }
const char* KeyLabel(int action) { return action >= 0 && action < g_nActions ? g_actions[action].label : "unbound"; }
int HintLines(const char* mode, char (*out)[kHintLen], int maxLines) {
    const int m = FindMode(mode);
    int line = 0, o = 0;
    for (int i = 0; m >= 0 && i < g_nActions && line < maxLines; ++i) {
        const ActionRec& a = g_actions[i];
        if (a.mode != m || !a.n) continue;
        char item[64];
        int n = wsprintfA(item, "%s: ", a.label);
        for (const char* s = a.id; *s && n < 60; ++s) item[n++] = *s == '_' ? ' ' : *s;
        item[n] = 0;
        if (o && o + 3 + n > 76) {
            if (line + 1 >= maxLines) break;  // no room: the rest is in `bind <mode>` and the reference
            ++line, o = 0;
        }
        o += wsprintfA(out[line] + o, "%s%s", o ? " | " : "", item);
    }
    return o ? line + 1 : 0;
}
void Solo(const char* mode, bool on) {
    const int m = FindMode(mode);
    if (on && m >= 0) g_solo = m;
    else if (!on && g_solo == m) g_solo = -1;
}
void SuppressHeld() {
    for (int vk = 1; vk < kVkCount; ++vk)
        if (Reserved(static_cast<uintptr_t>(vk)) && Down(vk)) g_suppress[vk] = true;
}
int ModeCount() { return g_nModes; }
bool ModeAt(int i, const char** name, const char** when, ModeKind* kind) {
    if (i < 0 || i >= g_nModes) return false;
    *name = g_modes[i].name, *when = g_modes[i].when, *kind = g_modes[i].kind;
    return true;
}
int ActionCount() { return g_nActions; }
bool ActionAt(int i, const char** mode, const char** id, const char** keys, const char** help, int* how) {
    if (i < 0 || i >= g_nActions) return false;
    const ActionRec& a = g_actions[i];
    *mode = g_modes[a.mode].name, *id = a.id, *keys = a.keys, *help = a.help, *how = a.how;
    return true;
}
#ifdef FDCORE_UNIT
void SetKeyStateForUnitTest(bool (*down)(int vk)) { g_unitDown = down; }
void SetLiveForUnitTest(bool live) { g_unitLive = live ? 1 : 0; }
void PollForUnitTest() { Poll(); }
bool MessageForUnitTest(unsigned message, uintptr_t wparam, intptr_t lparam) { return ModeMessage(message, wparam, lparam); }
#endif

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
    else log::Error("binds: described-key table full: %s not listed", key);
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
                                     "bind <key> shows it. bind <mode> <key> <action>: a mode's key (bind <mode> "
                                     "lists its actions, bind <mode> default restores them; keys also as ctrl+c). Keys: a-z, 0-9, f1-f12, mouse1-mouse5, "
                                     "space, tab, enter, escape, shift, ctrl, alt, uparrow (and the other arrows), "
                                     "kp_0-kp_9, ins, del, home, end, pgup, pgdn",
                         CmdBind);
    cmd::RegisterCommand("unbind", "unbind <key>: remove the key's bind (a default key stays unbound); "
                                   "unbind <mode> <key>: remove a mode's key", CmdUnbind);
    cmd::RegisterCommand("unbindall", "remove every ordinary bind, the default keys too (mode keys stay)", CmdUnbindAll);
    cmd::RegisterCommand("bindlist", "list the binds, the keys that are read as they are, and the modes (bind <mode> lists a mode's keys)", CmdBindList);
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
    // Mode keys, changes only: a removed default first, so a key moved to another action loads right.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < g_nActions && o + 120 < cap; ++i) {
            const ActionRec& a = g_actions[i];
            const Chord* from = pass ? a.chord : a.def;
            const Chord* other = pass ? a.def : a.chord;
            const int nFrom = pass ? a.n : a.nDef, nOther = pass ? a.nDef : a.n;
            for (int k = 0; k < nFrom; ++k) {
                bool both = false;
                for (int j = 0; j < nOther; ++j) both = both || SameChord(from[k], other[j]);
                if (both) continue;
                char key[24];
                ChordName(from[k], key);
                if (pass) o += wsprintfA(out + o, "bind %s %s %s\r\n", g_modes[a.mode].name, key, a.id);
                else o += wsprintfA(out + o, "unbind %s %s\r\n", g_modes[a.mode].name, key);
            }
        }
    return o;
}

} // namespace fdcore::binds
