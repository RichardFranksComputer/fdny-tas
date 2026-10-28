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
} // namespace fdcore::menu
