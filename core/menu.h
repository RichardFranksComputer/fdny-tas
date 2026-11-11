// A native briefing button and page, reusing the game's widgets and fonts.
#pragma once
#include <stdint.h>
namespace fdcore::menu {
struct Rect {
    int left, right, top, bottom;
};
constexpr int kRows = 17;
constexpr int kInertRow = 999;  // the command id of a row SetRow hasn't given one (clicks are ignored)
struct Callbacks {
    void (*open)();
    void (*draw)();
    void (*afterDraw)();
    void (*button)(int id);
    bool (*input)(unsigned message, uintptr_t key, intptr_t detail);
};
bool RegisterPage(const char* title, const Rect& button, const Callbacks& callbacks);
int SelectedStage();
bool Active();
void Close();
void LaunchStage();
// Leave the level; if the page launched it (LaunchStage), show the page again over the briefing.
void ReturnFromLevel();
void HideRows();
void SetRow(int slot, int id, Rect rect, const char* text, bool large = false);
void Text(int slot, Rect rect, const char* text, float scale = 0.60f);
void HighlightTab(int slot, bool selected);

// --- Forms (D67) ---------------------------------------------------------------------------
// A page of the game's own Options widgets: option rows (on / off, a named choice) and
// sliders, eight to a page, opened from a row the service adds to the game's Options screen
// (reached from the main menu and from the menu a level's Escape shows). One form.
// The client describes its rows once and answers three questions; it never touches a widget.
struct FormRow {
    const char* label;    // static
    const char* options;  // "Off|On", "Off|Best run|Latest run"; null = a slider over min..max
    float min, max;
};
struct FormCallbacks {
    float (*get)(int row);                         // the option's index, or the slider's value
    void (*set)(int row, float value);             // after a click or a drag (it may refuse: get is asked again)
    void (*describe)(int row, char* out, int cap); // one status line for the row last changed ("Text size 12")
};
constexpr int kMaxFormRows = 48;
// entry: the row's text on the Options screen. rows must stay valid. False when the menu's
// functions aren't verified or a form is registered already.
bool RegisterForm(const char* entry, const FormRow* rows, int n, const FormCallbacks& callbacks);
bool FormOpen();
} // namespace fdcore::menu
