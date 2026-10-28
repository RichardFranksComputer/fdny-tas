#include "menu.h"
#include <windows.h>
#include "addr.h"
#include "binds.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "state.h"
namespace fdcore::menu {
namespace {
using Byte = unsigned char;
using addr::Id;
// The game's menu, verified in Ghidra (docs/evidence/recordings/README.md, "The native menu").
constexpr int kTitleScreen = 2;        // MenuManager screen index of the title/briefing screen
constexpr int kNativeScreens = 10;     // screens MenuManager::Init builds; the page is appended as the 11th
constexpr int kListBackgroundA = 0xB;  // MenuScreen_List's background bitmaps (MenuScreen::Construct)
constexpr int kListBackgroundB = 0x11;
constexpr int kBackCommand = 0;        // AddStandardButton's command: the page's Back button
constexpr int kPageButton = 900;       // the briefing's Recordings button (outside the game's command ids)
using DrawFn = void(__thiscall*)(void*);
using ButtonFn = void(__thiscall*)(void*, int);
DrawFn g_titleDraw;
ButtonFn g_titleButton;
void* g_title;
void* g_button;
void* g_list;
void* g_manager;
void* g_rows[kRows];
void* g_labels[kRows];
bool g_labelVisible[kRows];
uintptr_t g_titleVtable[8], g_listVtable[8];
int g_listIndex = -1, g_selectedStage = -1;
bool g_registered, g_pageReady;
char g_buttonTitle[64];
Rect g_buttonRect;
Callbacks g_callbacks;
struct RowCache {
    char text[160];
    Rect rect;
    float scale;
    bool valid;
};
RowCache g_cache[kRows], g_labelCache[kRows];
bool g_tabSelected[kRows];
template <class T> T& Field(void* p, unsigned o) { return *reinterpret_cast<T*>(static_cast<Byte*>(p) + o); }
template <class T> T Fn(uintptr_t a) { return reinterpret_cast<T>(a); }
void* Allocate(unsigned n) {
    void* p = Fn<void*(__cdecl*)(unsigned)>(addr::ExportAddress(Id::menu_allocate))(n);
    if (p)
        ZeroMemory(p, n);
    return p;
}
void Append(void* owner, unsigned o, void* value) {
    int n = Field<int>(owner, o + addr::Field(Id::menu_f_count));
    Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_array_resize))(static_cast<Byte*>(owner) + o, n + 1);
    Field<void**>(owner, o)[n] = value;
}
void Label(void* row, const char* text, const Rect& r) {
    Fn<uintptr_t(__thiscall*)(void*, uint32_t, const Rect*, int, int)>(addr::ExportAddress(Id::menu_text_set))(
        static_cast<Byte*>(row) + addr::Field(Id::menu_f_text), console::MakeGameString(text), &r, 0, 0);
}
void* Row(void* owner, int id, Rect r, const char* text, bool interactive = true) {
    void* row = Allocate(addr::Field(Id::menu_f_row_size));
    if (!row)
        return nullptr;
    Fn<void*(__thiscall*)(void*, void*, int, const Rect*)>(addr::ExportAddress(Id::menu_row_construct))(row, owner, id,
                                                                                                        &r);
    Field<Byte>(row, addr::Field(Id::menu_f_row_flag)) = 0;
    Field<Byte>(row, addr::Field(Id::menu_f_row_style)) = 0;
    Label(row, text, r);
    if (interactive)
        Append(owner, addr::Field(Id::menu_f_children), row);
    return row;
}
void SetWidget(void* row, RowCache& cache, Rect r, const char* text, float scale) {
    if (!row)
        return;
    Field<Rect>(row, addr::Field(Id::menu_f_rect)) = r;
    bool same = cache.valid && cache.scale == scale && lstrcmpA(cache.text, text) == 0 && cache.rect.left == r.left &&
                cache.rect.right == r.right && cache.rect.top == r.top && cache.rect.bottom == r.bottom;
    if (!same) {
        // The profile row's animated resource selects the large native font atlas.
        // Preserve its matching metrics; changing only style0 metrics while retaining
        // that atlas produces scrambled glyph UVs. Native RichText scale changes
        // geometry/layout without changing the texture/metrics pairing.
        Field<float>(row, addr::Field(Id::menu_f_text_scale_x)) = scale;
        Field<float>(row, addr::Field(Id::menu_f_text_scale_y)) = scale;
        Label(row, text, r);
        lstrcpynA(cache.text, text, sizeof(cache.text));
        cache.rect = r;
        cache.scale = scale;
        cache.valid = true;
    }
    Field<Byte>(row, addr::Field(Id::menu_f_visible)) = 1;
}
} // namespace
void SetRow(int slot, int id, Rect r, const char* text, bool large) {
    if (!g_rows[slot])
        return;
    Field<int>(g_rows[slot], addr::Field(Id::menu_f_id)) = id;
    SetWidget(g_rows[slot], g_cache[slot], r, text, large ? 1.0f : 0.65f);
}
void HighlightTab(int slot, bool selected) {
    if (!g_rows[slot])
        return;
    // Profile rows use font animation frame 1 for their native hover highlight.
    // Reapply it for the selected tab even after the pointer leaves that control.
    void* font = Field<void*>(g_rows[slot], addr::Field(Id::menu_f_font));
    if (font &&
        ((selected && Field<int>(font, addr::Field(Id::menu_f_font_frame)) != 1) || (!selected && g_tabSelected[slot])))
        Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_font_frame))(font, selected ? 1 : 0);
    g_tabSelected[slot] = selected;
}
void Text(int slot, Rect r, const char* text, float scale) {
    SetWidget(g_labels[slot], g_labelCache[slot], r, text, scale);
    g_labelVisible[slot] = true;
}
void HideRows() {
    for (auto* row : g_rows)
        if (row)
            Field<Byte>(row, addr::Field(Id::menu_f_visible)) = 0;
    for (bool& visible : g_labelVisible)
        visible = false;
}
void Go(int id) { Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_go))(g_manager, id); }
void Back() { Fn<void(__thiscall*)(void*)>(addr::ExportAddress(Id::menu_back))(g_manager); }
bool Active() { return g_list && g_manager && Field<void*>(g_manager, addr::Field(Id::menu_f_active)) == g_list; }

int SelectedStage() { return g_title ? Field<int>(g_title, addr::Field(Id::menu_f_stage)) : -1; }
void Close() {
    Back();
    Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_select_stage))(g_title, g_selectedStage);
}
void LaunchStage() {
    Close();
    g_titleButton(g_title, 0);
}
void ReturnFromLevel() {
    // Leave the level the page launched the game's own way: Escape down/up through
    // GameSession::OnKey (it opens the game's menu, which the page then replaces).
    uintptr_t slot = 0, session = 0;
    if (state::Current().gameState == 1 && addr::Usable(Id::session_on_key) && addr::Resolve(Id::game_session, &slot) &&
        addr::SafeRead(slot, &session, sizeof(session)) && session) {
        const auto key = Fn<void(__thiscall*)(void*, int, int)>(addr::ExportAddress(Id::session_on_key));
        key(reinterpret_cast<void*>(session), VK_ESCAPE, 1);
        key(reinterpret_cast<void*>(session), VK_ESCAPE, 0);
    }
    if (!g_title || !g_manager || !g_pageReady)
        return;  // not launched from the page (fd_replay_view): the game's own menu stays
    // The briefing under the page, as if the player had come from it: Back returns there.
    Go(kTitleScreen);
    Field<int>(g_manager, addr::Field(Id::menu_f_previous)) = kTitleScreen;
    Field<int>(g_manager, addr::Field(Id::menu_f_history)) = 1;
    Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_select_stage))(g_title, g_selectedStage);
    Go(g_listIndex);
}
namespace {
// The page's keys and typing, from the window messages binds owns (D58).
bool Input(unsigned msg, uintptr_t w, intptr_t l) {
    return Active() && g_callbacks.input && g_callbacks.input(msg, w, l);
}
void __fastcall ListButton(void*, void*, int id) { g_callbacks.button(id); }
void __fastcall ListDraw(void* screen, void*) {
    g_callbacks.draw();
    Fn<DrawFn>(addr::ExportAddress(Id::menu_page_draw))(screen);
    for (int i = 0; i < kRows; ++i)
        if (g_labelVisible[i] && g_labels[i])
            Fn<DrawFn>(addr::ExportAddress(Id::menu_text_draw))(static_cast<Byte*>(g_labels[i]) +
                                                                addr::Field(Id::menu_f_text));
    if (g_callbacks.afterDraw)
        g_callbacks.afterDraw();
}
bool CreateList() {
    if (g_list)
        return g_pageReady;
    const unsigned pages = addr::Field(Id::menu_f_pages);
    if (Field<int>(g_manager, pages + addr::Field(Id::menu_f_count)) != kNativeScreens) {
        log::Warn("menu: unexpected native page count");
        return false;
    }
    g_list = Allocate(addr::Field(Id::menu_f_page_size));
    if (!g_list)
        return false;
    Fn<void*(__thiscall*)(void*, void*, int, int)>(addr::ExportAddress(Id::menu_page_construct))(g_list, g_manager, kListBackgroundA,
                                                                                                 kListBackgroundB);
    auto* v = Field<uintptr_t*>(g_list, addr::Field(Id::menu_f_vtable));
    for (int i = 0; i < 8; ++i)
        g_listVtable[i] = v[i];
    g_listVtable[addr::Field(Id::menu_f_vtable_button) / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&ListButton);
    g_listVtable[addr::Field(Id::menu_f_vtable_draw) / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&ListDraw);
    Field<uintptr_t*>(g_list, addr::Field(Id::menu_f_vtable)) = g_listVtable;
    Fn<void(__thiscall*)(void*, int)>(addr::ExportAddress(Id::menu_page_setup))(g_list, kBackCommand);
    for (int i = 0; i < kRows; ++i) {
        g_rows[i] = Row(g_list, kInertRow, {230, 620, 60, 90}, "");
        g_labels[i] = Row(g_list, kInertRow, {230, 620, 60, 90}, "", false);
        if (!g_rows[i] || !g_labels[i])
            return false;
    }
    g_listIndex = Field<int>(g_manager, pages + addr::Field(Id::menu_f_count));
    Append(g_manager, pages, g_list);
    g_pageReady = true;
    return true;
}
void __fastcall TitleButton(void* screen, void*, int id) {
    if (id == kPageButton) {
        if (CreateList()) {
            g_selectedStage = SelectedStage();
            g_callbacks.open();
            Go(g_listIndex);
        } else
            log::Warn("menu: could not create custom page");
        return;
    }
    g_titleButton(screen, id);
}
void __fastcall TitleDraw(void* screen, void*) {
    if (!g_title) {
        g_title = screen;
        g_manager = Field<void*>(screen, addr::Field(Id::menu_f_title_manager));
        auto* v = Field<uintptr_t*>(screen, addr::Field(Id::menu_f_vtable));
        for (int i = 0; i < 8; ++i)
            g_titleVtable[i] = v[i];
        const unsigned button = addr::Field(Id::menu_f_vtable_button) / sizeof(uintptr_t);
        g_titleButton = Fn<ButtonFn>(v[button]);
        g_titleVtable[button] = reinterpret_cast<uintptr_t>(&TitleButton);
        Field<uintptr_t*>(screen, addr::Field(Id::menu_f_vtable)) = g_titleVtable;
        g_button = Row(screen, kPageButton, g_buttonRect, g_buttonTitle);
    }
    if (g_button)
        Field<Byte>(g_button, addr::Field(Id::menu_f_visible)) = SelectedStage() >= 0;
    g_titleDraw(screen);
}
} // namespace
bool RegisterPage(const char* title, const Rect& button, const Callbacks& callbacks) {
    if (g_registered || !callbacks.open || !callbacks.draw || !callbacks.button) {
        log::Warn("menu: page already registered or missing callbacks");
        return false;
    }
    const Id needs[] = {Id::menu_allocate,       Id::menu_array_resize, Id::menu_text_set,
                        Id::menu_row_construct,  Id::menu_font_frame,   Id::menu_go,
                        Id::menu_back,           Id::menu_page_draw,    Id::menu_text_draw,
                        Id::menu_page_construct, Id::menu_page_setup,   Id::menu_select_stage,
                        Id::menu_title_draw,     Id::str_from_cstr};
    for (Id id : needs)
        if (!addr::Usable(id)) {
            log::Warn("menu: unavailable (needs %s)", addr::Name(id));
            return false;
        }
    lstrcpynA(g_buttonTitle, title, sizeof(g_buttonTitle));
    g_buttonRect = button;
    g_callbacks = callbacks;
    if (!hooks::Create("menu", Id::menu_title_draw, reinterpret_cast<void*>(&TitleDraw),
                       reinterpret_cast<void**>(&g_titleDraw)) ||
        !hooks::SetEnabled("menu", true)) {
        log::Warn("menu: native briefing hook unavailable");
        return false;
    }
    if (callbacks.input && !binds::OnWindowMessage(Input))
        log::Warn("menu: no window-message slot; the page's keys and typing are unavailable");
    g_registered = true;
    log::Info("menu: native page ready");
    return true;
}
} // namespace fdcore::menu
