// Tests: tests/game/settings_page.yaml tests/unit/test_cmd.cpp
#include "settings.h"

#include <windows.h>

#include "cmd.h"
#include "log.h"
#include "menu.h"
#include "num.h"

namespace fdcore::settings {
namespace {

menu::FormRow g_rows[menu::kMaxFormRows];
cmd::Cvar* g_var[menu::kMaxFormRows];
int g_n = 0;

// The n-th "|"-separated option, into out; false past the end.
bool Option(const char* options, int n, char* out, int cap) {
    for (const char* s = options; *s; ++s) {
        if (n == 0) {
            int o = 0;
            for (; *s && *s != '|' && o < cap - 1; ++s) out[o++] = *s;
            out[o] = 0;
            return true;
        }
        while (*s && *s != '|') ++s;
        if (!*s) break;
        --n;
    }
    return false;
}

// A text variable's options are its values ("practice|run"); a number's are names for
// min, min + 1, ... ("Off|Best run|Latest run").
float Get(int row) {
    const cmd::Cvar& v = *g_var[row];
    if (v.type == cmd::T_BOOL) return cmd::Bool(v) ? 1.0f : 0.0f;
    if (!g_rows[row].options) return cmd::Float(v);
    if (v.type != cmd::T_TEXT) return cmd::Float(v) - v.min;
    char option[64];
    for (int i = 0; Option(g_rows[row].options, i, option, sizeof(option)); ++i)
        if (lstrcmpiA(option, v.value) == 0) return static_cast<float>(i);
    return 0;
}

void Set(int row, float value) {
    cmd::Cvar& v = *g_var[row];
    char text[64];
    if (v.type == cmd::T_TEXT) {
        if (!Option(g_rows[row].options, static_cast<int>(value), text, sizeof(text))) return;
    } else if (v.type == cmd::T_BOOL) {
        lstrcpyA(text, value != 0.0f ? "1" : "0");
    } else {
        if (g_rows[row].options) value += v.min;
        if (value < v.min) value = v.min;
        if (value > v.max) value = v.max;
        if (v.type == cmd::T_INT) num::Fixed(text, value, 0);
        else num::Short(text, value);
    }
    if (lstrcmpA(text, v.value) != 0) cmd::SetCvar(&v, text);  // may be refused (the profile): the row then snaps back
}

void Describe(int row, char* out, int cap) {
    const cmd::Cvar& v = *g_var[row];
    char value[64];
    lstrcpynA(value, v.value, sizeof(value));
    if (g_rows[row].options) Option(g_rows[row].options, static_cast<int>(Get(row)), value, sizeof(value));
    char line[160];
    wsprintfA(line, "%s: %s", g_rows[row].label, value);
    lstrcpynA(out, line, cap);
}

} // namespace

bool Init() {
    for (int i = 0; i < cmd::CvarCount() && g_n < menu::kMaxFormRows; ++i) {
        cmd::Cvar* v = cmd::CvarAt(i);
        const char* label = v ? cmd::Label(v->name) : nullptr;
        if (!label || v->readOnly) continue;
        const char* options = cmd::Options(v->name);
        if (v->type == cmd::T_BOOL && !options) options = "Off|On";
        if (v->type == cmd::T_TEXT && !options) continue;  // free text has no widget
        g_rows[g_n] = {label, options, v->min, v->max};
        g_var[g_n++] = v;
    }
    if (!g_n) {
        log::Info("settings: no labelled variables; no page");
        return false;
    }
    const menu::FormCallbacks callbacks = {Get, Set, Describe};
    if (!menu::RegisterForm("FDNYCore", g_rows, g_n, callbacks)) {
        log::Warn("settings: the menu has no form; settings stay in the console");
        return false;
    }
    log::Info("settings: ready (%d rows on the game's Options screen)", g_n);
    return true;
}

} // namespace fdcore::settings
