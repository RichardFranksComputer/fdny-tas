// Tests: tests/checkpoints.yaml
// User: Practice checkpoints -- place and edit native checkpoint boxes, save per mission and show elapsed level time on contact.
// Draft edits apply on mission restart; the game owns trigger contacts and hint rendering.
#include <windows.h>

#include "checkpoints.h"
#include "addr.h"
#include "binds.h"
#include "camera.h"
#include "cmd.h"
#include "console.h"
#include "entities.h"
#include "gfx.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "pause.h"
#include "state.h"
#include "textdraw.h"

namespace fdcore::checkpoints {
namespace {
bool Practice() {
    const cmd::Cvar* p = cmd::FindCvar("fd_profile");
    return p && !lstrcmpiA(p->value, "practice");
}

bool FilePath(const char* map, char* path) {
    if (!Mission(map)) return false;
    char relative[64]; wsprintfA(relative, "Checkpoints\\%s.txt", map);
    if (!console::GamePath(relative, path, MAX_PATH)) {
        console::Print("checkpoints: game directory path is too long"); return false;
    }
    return true;
}

bool LoadFile(const char* map, Set* set, bool* missing = nullptr) {
    if (missing) *missing = false;
    char path[MAX_PATH];
    if (!FilePath(map, path)) return false;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (missing) *missing = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
        return false;
    }
    static char text[kTextMax];
    const DWORD size = GetFileSize(file, nullptr); DWORD got = 0;
    const bool ok = size < sizeof(text) && ReadFile(file, text, size, &got, nullptr) && got == size;
    CloseHandle(file);
    if (!ok) { console::Print("checkpoints: cannot read %s", path); return false; }
    text[size] = 0;
    static Set candidate;
    if (lstrlenA(text) != static_cast<int>(size) || !Parse(text, &candidate) || !Equal(map, candidate.map)) {
        console::Print("checkpoints: invalid %s; no triggers loaded", path); return false;
    }
    *set = candidate; return true;
}

// In-game editor.
struct Snapshot { Set set; int selected; };
Snapshot g_draft{}, g_history[16];
int g_historyCount, g_historyAt, g_panel = -1;
Point g_clipboard{};
float g_cursorX, g_cursorY;
unsigned g_nextLabel;
struct Drag {
    bool active, changed;
    int axis, face;
    float x, y, plane[3], normal[3], grab[3];
    Point start;
};
Drag g_drag{};
bool g_hasClipboard, g_dirty, g_savePending, g_backup, g_ownsPause, g_syncing, g_open;
bool g_previous[256];
DWORD g_repeat[256], g_saveAt;
const char* g_message = "Saved; restart mission to apply";
cmd::Cvar g_edit = {"fd_checkpoints_edit", "0", "1 = pause and edit checkpoint boxes in the free camera; autosaves Checkpoints/<mission>.txt; 0 = exit; restart mission to apply", false, false, nullptr, {0}};
cmd::Cvar g_distance = {"fd_checkpoints_distance", "160", "new/pasted checkpoint distance ahead of the camera in units (32-2000)", false, false, nullptr, {0}};
cmd::Cvar g_step = {"fd_checkpoints_step", "8", "checkpoint move/resize step in units (1-256); hold Ctrl for 1 unit", false, false, nullptr, {0}};

bool Down(int key) { return (GetAsyncKeyState(key)&0x8000) != 0; }
bool Active() {
    return g_open && g_edit.value[0] == '1' && pause::Active() && state::Current().gameState == 1 && Practice();
}
bool Selected() { return g_draft.selected >= 0 && g_draft.selected < g_draft.set.count; }
float Setting(const cmd::Cvar& var, float fallback, float min, float max) {
    float n = 0; return Number(var.value, &n) && n >= min && n <= max ? n : fallback;
}
bool Capture(int vk) {
    if (!Active()) return false;
    if (g_drag.active && (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == 'E' || vk == 'Q')) return true;
    return vk == VK_LBUTTON || vk == VK_SPACE || vk == VK_INSERT || vk == VK_DELETE || vk == VK_BACK || vk == VK_TAB ||
        vk == VK_RETURN || vk == VK_HOME || vk == VK_END || (vk >= VK_PRIOR && vk <= VK_DOWN) ||
        (Down(VK_CONTROL) && (vk == 'C' || vk == 'V' || vk == 'D' || vk == 'S'));
}
bool CapturePointer() { return Active() && (g_drag.active || (Down(VK_LBUTTON) && !g_previous[VK_LBUTTON])); }
void CenterPointer() { int w, h; gfx::ViewSize(&w,&h); g_cursorX = w*.5f; g_cursorY = h*.5f; }
void ResetKeys() {
    const int keys[] = {VK_LBUTTON,VK_SPACE,VK_INSERT,VK_DELETE,VK_BACK,VK_TAB,VK_RETURN,VK_HOME,VK_END,
        VK_LEFT,VK_RIGHT,VK_NEXT,VK_PRIOR,VK_DOWN,VK_UP,'C','V','D','S'};
    for (int key : keys) g_previous[key] = Down(key), g_repeat[key] = GetTickCount()+300;
}
void SetEdit(bool on) {
    g_syncing = true; cmd::SetCvarUngated(&g_edit, on ? "1" : "0"); g_syncing = false;
}
void Remember() {
    // The last entry is the state before the next change; redo entries are discarded.
    g_historyCount = g_historyAt;
    if (g_historyCount == 16) {
        for (int i = 1; i < 16; ++i) g_history[i-1] = g_history[i];
        --g_historyCount;
    }
    g_history[g_historyCount++] = g_draft; g_historyAt = g_historyCount;
}
void DraftChanged() {
    g_dirty = g_savePending = true; g_saveAt = GetTickCount()+600; g_message = "Saving...; restart mission to apply";
}
void EndDrag() {
    if (g_drag.active && g_drag.changed) DraftChanged();
    g_drag.active = false;
}
bool Save() {
    if (!g_dirty) return true;
    g_savePending = false; // failed attempts wait for another edit or explicit save
    char path[MAX_PATH], temporary[MAX_PATH], backup[MAX_PATH], directory[MAX_PATH];
    static char text[kTextMax];
    const int length = Write(g_draft.set, text, sizeof(text));
    if (!length || !FilePath(g_draft.set.map, path) || lstrlenA(path)+5 >= MAX_PATH) {
        g_message = "SAVE FAILED: invalid draft or path; draft retained";
        console::Print("checkpoints: cannot save invalid draft or overlong path; draft retained"); return false;
    }
    lstrcpyA(directory, path);
    char* end = directory+lstrlenA(directory);
    while (end > directory && end[-1] != '\\') --end;
    *end = 0;
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) goto failed;
    wsprintfA(temporary, "%s.tmp", path); wsprintfA(backup, "%s.bak", path);
    if (!g_backup) {
        if (!CopyFileA(path, backup, FALSE) && GetLastError() != ERROR_FILE_NOT_FOUND) goto failed;
        g_backup = true;
    }
    {
        HANDLE f = CreateFileA(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) goto failed;
        DWORD written = 0;
        bool ok = WriteFile(f, text, length, &written, nullptr);
        DWORD writeError = GetLastError();
        if (ok && written != static_cast<DWORD>(length)) { ok = false; writeError = ERROR_WRITE_FAULT; }
        if (ok) { ok = FlushFileBuffers(f); writeError = GetLastError(); }
        CloseHandle(f);
        if (!ok) SetLastError(writeError);
        if (!ok || !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileA(temporary); SetLastError(error); goto failed;
        }
    }
    g_dirty = false; g_message = "Saved; restart mission to apply";
    console::Print("checkpoints: saved %d boxes to %s; restart mission to apply", g_draft.set.count, path);
    return true;
failed:
    g_message = "SAVE FAILED: Enter to retry; draft retained";
    console::Print("checkpoints: save failed (Windows error %lu); draft retained; fd_checkpoints_save retries", GetLastError());
    return false;
}
void OnEdit(cmd::Cvar&) {
    if (g_syncing) return;
    if (g_edit.value[0] != '1') {
        EndDrag();
        if (!Save()) { SetEdit(true); return; }
        if (g_ownsPause) {
            if (auto* var = cmd::FindCvar("fd_pause")) cmd::SetCvar(var, "0");
        }
        g_ownsPause = g_open = false;
        console::Print("checkpoints: editor closed; restart mission to apply saved boxes"); return;
    }
    const auto& s = state::Current();
    if (g_open && pause::Active() && Equal(s.map, g_draft.set.map)) return;
    if (s.gameState != 1 || !Mission(s.map) || !camera::WorldOverrideAvailable() || !gfx::WorldBoxesReady()) {
        console::Print("checkpoints: editor needs a loaded rescue mission, free camera and world drawing"); SetEdit(false); return;
    }
    if (!g_dirty) {
        static Set loaded;
        bool missing = false;
        if (!LoadFile(s.map, &loaded, &missing) && !missing) { SetEdit(false); return; }
        if (missing) { loaded.count = 0; lstrcpynA(loaded.map, s.map, sizeof(loaded.map)); }
        g_draft.set = loaded; g_draft.selected = -1;
        g_nextLabel = 0;
        // Resume after the greatest existing automatic label; keep user text untouched.
        for (int i = 0; i < loaded.count; ++i) {
            const auto& point = loaded.points[i];
            unsigned index = 0; const char* c = point.label;
            const char* prefix = "Checkpoint ";
            while (*prefix && *c == *prefix) ++c, ++prefix;
            if (*prefix || !*c) continue;
            while (*c >= 'A' && *c <= 'Z' && index < 1000000) index = index*26+static_cast<unsigned>(*c++-'A'+1);
            if (!*c && index > g_nextLabel) g_nextLabel = index;
        }
        g_backup = false; g_historyAt = g_historyCount = 0;
        g_message = "Saved; restart mission to apply";
    } else if (!Equal(s.map, g_draft.set.map)) {
        console::Print("checkpoints: save the previous mission's draft first with fd_checkpoints_save"); SetEdit(false); return;
    }
    auto* var = cmd::FindCvar("fd_pause");
    g_ownsPause = !pause::Active();
    if (!var || (g_ownsPause && !cmd::SetCvar(var, "1")) || !camera::WorldOverrideActive()) {
        console::Print("checkpoints: free camera unavailable");
        if (g_ownsPause && var) cmd::SetCvar(var, "0");
        g_ownsPause = false; SetEdit(false); return;
    }
    g_open = true; CenterPointer(); ResetKeys();
    console::Print("checkpoints: editing %s (%d boxes); Space places; drag box/arrows to move, cubes to resize; autosave; restart to apply", s.map, g_draft.set.count);
}
void SaveCommand(int, const char* const*) { Save(); }
void Name(int argc, const char* const* argv) {
    if (!Active() || !Selected()) { console::Print("checkpoints: select a box in fd_checkpoints_edit first"); return; }
    if (argc != 2 || !Label(argv[1])) {
        console::Print("usage: fd_checkpoints_name \"Display text\" (1-80 ASCII characters; no parentheses or |)"); return;
    }
    Remember(); lstrcpyA(g_draft.set.points[g_draft.selected].label, argv[1]); DraftChanged();
}
void Add(bool paste, bool duplicate) {
    if (g_draft.set.count == kMax) { g_message = "32 checkpoint limit reached"; return; }
    if ((paste && !g_hasClipboard) || (duplicate && !Selected())) return;
    Point point{};
    if (duplicate) point = g_draft.set.points[g_draft.selected];
    else {
        float origin[3], direction[3];
        if (!gfx::ScreenRay(g_cursorX,g_cursorY,origin,direction)) return;
        const float distance = Setting(g_distance, 160, 32, 2000);
        if (paste) point = g_clipboard;
        else { point.min[0] = point.min[2] = -48; point.max[0] = point.max[2] = 48; point.min[1] = -64; point.max[1] = 64; }
        for (int a = 0; a < 3; ++a) {
            const float half = (point.max[a]-point.min[a])*.5f;
            const float center = origin[a]+direction[a]*distance;
            point.min[a] = center-half; point.max[a] = center+half;
            if (!(point.min[a] >= -105000 && point.max[a] <= 105000)) return;
        }
    }
    AlphabetLabel(g_nextLabel++, point.label);
    Remember();
    g_draft.selected = g_draft.set.count; g_draft.set.points[g_draft.set.count++] = point; DraftChanged();
}
void Delete() {
    if (!Selected()) return;
    Remember();
    for (int i = g_draft.selected+1; i < g_draft.set.count; ++i) g_draft.set.points[i-1] = g_draft.set.points[i];
    --g_draft.set.count; g_draft.selected = -1; DraftChanged();
}
void Undo(bool redo) {
    if ((redo && g_historyAt >= g_historyCount) || (!redo && !g_historyAt)) return;
    const int index = redo ? g_historyAt++ : --g_historyAt;
    static Snapshot swap;
    swap = g_draft; g_draft = g_history[index]; g_history[index] = swap; DraftChanged();
}
bool Press(int key, bool repeat = false) {
    const bool down = Down(key);
    const DWORD now = GetTickCount();
    const bool press = down && (!g_previous[key] || (repeat && static_cast<int32_t>(now-g_repeat[key]) >= 0));
    if (down && !g_previous[key]) g_repeat[key] = now+300;
    else if (press) g_repeat[key] = now+100;
    g_previous[key] = down; return press;
}
void Center(const Point& p, float* out) { for (int a = 0; a < 3; ++a) out[a] = (p.min[a]+p.max[a])*.5f; }
// Handles: arrows outside the positive face move; cubes on either face resize.
void HandlePoint(const Point& box, int axis, int face, float* out) {
    Center(box,out);
    out[axis] = face < 0 ? box.min[axis] : box.max[axis]+(face ? 0 : 40);
}
int PickHandle(int* face) {
    const auto& box = g_draft.set.points[g_draft.selected];
    int axis = -1; float closest = 100;
    for (int a = 0; a < 3; ++a) {
        for (int f = -1; f <= 1; f += 2) {
            float point[3], x, y; HandlePoint(box,a,f,point);
            if (!gfx::ProjectWorld(point,&x,&y)) continue;
            const float d = (x-g_cursorX)*(x-g_cursorX)+(y-g_cursorY)*(y-g_cursorY);
            if (d < closest) closest = d, axis = a, *face = f;
        }
    }
    if (axis >= 0) return axis; // resize grips take precedence over the arrow shaft
    closest = 64;
    for (int a = 0; a < 3; ++a) {
        float start[3], end[3], x0,y0,x1,y1;
        HandlePoint(box,a,1,start); start[a] += 10; HandlePoint(box,a,0,end);
        if (!gfx::ProjectWorld(start,&x0,&y0) || !gfx::ProjectWorld(end,&x1,&y1)) continue;
        const float dx = x1-x0, dy = y1-y0, length2 = dx*dx+dy*dy;
        if (length2 < 4) continue;
        float t = ((g_cursorX-x0)*dx+(g_cursorY-y0)*dy)/length2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        const float x = x0+t*dx-g_cursorX, y = y0+t*dy-g_cursorY;
        if (x*x+y*y < closest) closest = x*x+y*y, axis = a, *face = 0;
    }
    return axis;
}
void BeginDrag() {
    float origin[3], direction[3];
    if (!gfx::ScreenRay(g_cursorX,g_cursorY,origin,direction)) return;
    int face = 0, axis = Selected() ? PickHandle(&face) : -1;
    if (axis < 0) {
        g_draft.selected = Pick(g_draft.set,origin,direction);
        if (!Selected()) return;
    }
    g_drag = {}; g_drag.axis = axis; g_drag.face = face;
    g_drag.start = g_draft.set.points[g_draft.selected]; g_drag.x = g_cursorX; g_drag.y = g_cursorY;
    Center(g_drag.start,g_drag.plane);
    camera::XForm view; if (!camera::ViewXForm(&view)) return;
    for (int a = 0; a < 3; ++a) g_drag.normal[a] = axis == a ? 0 : -view.m[a*3+2];
    const float normal2 = g_drag.normal[0]*g_drag.normal[0]+g_drag.normal[1]*g_drag.normal[1]+g_drag.normal[2]*g_drag.normal[2];
    if (normal2 < .01f || !RayPlane(origin,direction,g_drag.plane,g_drag.normal,g_drag.grab)) return;
    g_drag.active = true;
}
void Pointer() {
    if (!binds::KeysLive()) { EndDrag(); ResetKeys(); return; }
    const bool pressed = Press(VK_LBUTTON);
    if (!Down(VK_LBUTTON)) { EndDrag(); CenterPointer(); return; }
    if (!g_drag.active) {
        CenterPointer();
        if (pressed) BeginDrag();
        return;
    }
    int32_t mx = 0, my = 0;
    addr::ReadI32(addr::Id::controls_mouse_x,&mx); addr::ReadI32(addr::Id::controls_mouse_y,&my);
    int w,h; gfx::ViewSize(&w,&h);
    g_cursorX += static_cast<float>(mx); g_cursorY += static_cast<float>(my);
    if (g_cursorX < 0) g_cursorX = 0;
    if (g_cursorY < 0) g_cursorY = 0;
    if (g_cursorX > w-1) g_cursorX = static_cast<float>(w-1);
    if (g_cursorY > h-1) g_cursorY = static_cast<float>(h-1);
    const float dx = g_cursorX-g_drag.x, dy = g_cursorY-g_drag.y;
    if (!g_drag.changed && dx*dx+dy*dy < 9) return; // clicking selects without a spurious edit
    float delta[3] = {};
    float origin[3], direction[3], hit[3];
    if (!gfx::ScreenRay(g_cursorX,g_cursorY,origin,direction) ||
        !RayPlane(origin,direction,g_drag.plane,g_drag.normal,hit)) return;
    for (int a = 0; a < 3; ++a) delta[a] = hit[a]-g_drag.grab[a];
    Point next;
    if (!DragPoint(g_drag.start,delta,g_drag.axis,g_drag.face,&next)) return;
    if (!g_drag.changed) Remember();
    g_drag.changed = true; g_draft.set.points[g_draft.selected] = next; DraftChanged();
}
void Input() {
    if (!binds::KeysLive()) { EndDrag(); ResetKeys(); return; }
    const bool ctrl = Down(VK_CONTROL), resize = Down(VK_SHIFT);
    Pointer();
    if (g_drag.active) return;
    if (Press(VK_TAB) && g_draft.set.count) g_draft.selected = (g_draft.selected+1)%g_draft.set.count;
    if (Press(VK_SPACE) || Press(VK_INSERT)) Add(false, false);
    if (Press('C') && ctrl && Selected()) g_clipboard = g_draft.set.points[g_draft.selected], g_hasClipboard = true;
    if (Press('V') && ctrl) Add(true, false);
    if (Press('D') && ctrl) Add(false, true);
    if (Press(VK_DELETE) || Press(VK_BACK)) Delete();
    if (Press(VK_HOME)) Undo(false);
    if (Press(VK_END)) Undo(true);
    const bool save = Press('S') && ctrl;
    if (Press(VK_RETURN) || save) Save();
    const int keys[] = {VK_LEFT,VK_RIGHT,VK_NEXT,VK_PRIOR,VK_DOWN,VK_UP};
    for (int i = 0; i < 6; ++i) {
        if (!Press(keys[i], true) || !Selected()) continue;
        Point point = g_draft.set.points[g_draft.selected];
        const float step = ctrl ? 1 : Setting(g_step, 8, 1, 256);
        if (!Adjust(&point, i/2, (i%2 ? 1 : -1)*step, resize)) continue;
        Remember(); g_draft.set.points[g_draft.selected] = point; DraftChanged();
    }
}
void BoxOutline(const float* lo, const float* hi, uint32_t rgb, float (*points)[3], uint32_t* colors, gfx::WorldLine* lines) {
    constexpr int corners[] = {0,1,5,4,0,2,3,7,6,2,0,2,1,3,5,7,4,6};
    for (int j = 0; j < 18; ++j) {
        for (int a = 0; a < 3; ++a) points[j][a] = corners[j] & (1<<a) ? hi[a] : lo[a];
        colors[j] = rgb;
    }
    for (int j = 0; j < 6; ++j) {
        const int offset = j < 2 ? j*5 : 10+(j-2)*2;
        lines[j] = {points+offset,colors+offset,nullptr,j < 2 ? 5 : 2,2,0,2,255,0,0};
    }
}
void Draw() {
    // Custom editor boxes stay visible through walls; normal trigger overlays are unchanged.
    static float points[kMax][18][3];
    static uint32_t colors[kMax][18];
    static gfx::WorldLine lines[kMax*6];
    for (int i = 0; i < g_draft.set.count; ++i) {
        const auto& box = g_draft.set.points[i];
        BoxOutline(box.min,box.max,i == g_draft.selected ? 0xffdd44 : 0x44ddcc,points[i],colors[i],lines+i*6);
    }
    gfx::SubmitWorldLines(lines,g_draft.set.count*6,gfx::LAYER_OVERLAY);
    if (Selected()) {
        static float axes[3][5][3]; static uint32_t rgb[3][5];
        static gfx::WorldLine arrows[6]; static gfx::WorldBox handles[6];
        constexpr uint32_t colors[] = {0xff6655,0x77dd66,0x6699ff};
        const auto& box = g_draft.set.points[g_draft.selected];
        int hoverFace = 0;
        const int hoverAxis = g_drag.active ? g_drag.axis : (binds::KeysLive() ? PickHandle(&hoverFace) : -1);
        if (g_drag.active) hoverFace = g_drag.face;
        for (int a = 0; a < 3; ++a) {
            float center[3], tip[3]; Center(box,center); HandlePoint(box,a,0,tip);
            for (int c = 0; c < 3; ++c) {
                axes[a][0][c] = center[c]; axes[a][1][c] = axes[a][3][c] = tip[c];
                axes[a][2][c] = axes[a][4][c] = tip[c]-(a == c ? 12.0f : 0);
            }
            axes[a][2][(a+1)%3] += 7; axes[a][4][(a+1)%3] -= 7;
            for (auto& color : rgb[a]) color = colors[a];
            arrows[a*2] = {axes[a],rgb[a],nullptr,2,3,0,3,255,0,0};
            arrows[a*2+1] = {axes[a]+2,rgb[a]+2,nullptr,3,3,0,3,255,0,0};
            for (int side = 0; side < 2; ++side) {
                float point[3]; HandlePoint(box,a,side ? 1 : -1,point);
                auto& handle = handles[a*2+side];
                for (int c = 0; c < 3; ++c) handle.min[c] = point[c]-5, handle.max[c] = point[c]+5;
                handle.rgb = colors[a]; handle.alpha = handle.alphaInside = 255;
            }
        }
        // Use the exact picking result for hover and keep the active grip highlighted.
        // White borders use existing lines, with no new surface or drawing hook.
        static uint32_t white[18];
        for (auto& color : white) color = 0xffffff;
        static gfx::WorldLine outline[6];
        static float grip[18][3];
        if (hoverAxis >= 0) {
            if (!hoverFace) {
                outline[0] = {axes[hoverAxis],white,nullptr,2,7,0,7,255,0,0};
                outline[1] = {axes[hoverAxis]+2,white,nullptr,3,7,0,7,255,0,0};
                gfx::SubmitWorldLines(outline,2,gfx::LAYER_OVERLAY);
            } else {
                auto& handle = handles[hoverAxis*2+(hoverFace > 0 ? 1 : 0)];
                handle.rgb = 0xffdd44;
                BoxOutline(handle.min,handle.max,0xffffff,grip,white,outline);
                gfx::SubmitWorldLines(outline,6,gfx::LAYER_OVERLAY);
            }
        }
        gfx::SubmitWorldLines(arrows,6,gfx::LAYER_OVERLAY); gfx::SubmitWorldBoxes(handles,6,gfx::LAYER_OVERLAY);
    }

    char title[textdraw::kLineLen], selected[textdraw::kLineLen];
    wsprintfA(title, "CHECKPOINT EDITOR | %s | %d/32 | %s", g_draft.set.map, g_draft.set.count, g_dirty ? "UNSAVED" : "saved");
    if (Selected()) {
        const auto& p = g_draft.set.points[g_draft.selected];
        char label[55]; lstrcpynA(label, p.label, sizeof(label));
        wsprintfA(selected, "#%d %s", g_draft.selected+1, label);
    } else lstrcpyA(selected, "Click a box to select; empty click deselects");
    const char* rows[] = {title,selected,
        "WASD/EQ + mouse: fly | Space: place at crosshair",
        "Drag box: move | arrows: X red / Y green / Z blue",
        "Drag face cubes: resize | Ctrl+C/V/D: copy/paste/dup",
        "Del: delete | Home/End: undo/redo | P: exit",
        g_message};
    textdraw::SetLines(g_panel, rows, 7);
}
void Reticle() {
    if (!Active() || !binds::KeysLive()) return;
    const int x = static_cast<int>(g_cursorX), y = static_cast<int>(g_cursorY);
    gfx::SolidRect(x-5,y-1,11,3,0); gfx::SolidRect(x-1,y-5,3,11,0);
    gfx::SolidRect(x-4,y,9,1,0xffdd44); gfx::SolidRect(x,y-4,1,9,0xffdd44);
}
void Frame() {
    if (!Active()) {
        EndDrag(); textdraw::Show(g_panel, false);
        if (g_edit.value[0] == '1') {
            // Pause/menu transitions leave the editor; saving uses the draft's map key.
            Save(); SetEdit(false); g_ownsPause = g_open = false;
        }
        return;
    }
    Input();
    if (g_savePending && !g_drag.active && static_cast<int32_t>(GetTickCount()-g_saveAt) >= 0) Save();
    textdraw::Show(g_panel, true); Draw();
}
void EditorLevelLoad() {
    EndDrag();
    if (g_dirty) Save();
    SetEdit(false); g_ownsPause = g_open = false;
    textdraw::Show(g_panel, false);
}
void InitEditor() {
    g_draft.selected = -1;
    g_edit.onChange = OnEdit;
    cmd::RegisterCvar(&g_edit); cmd::RegisterCvar(&g_distance); cmd::RegisterCvar(&g_step);
    cmd::RegisterCommand("fd_checkpoints_name", "fd_checkpoints_name \"Display text\": rename the selected box in the checkpoint editor", Name);
    cmd::RegisterCommand("fd_checkpoints_save", "retry saving the checkpoint draft; autosaved after edits, restart mission to apply", SaveCommand);
    cmd::SetPracticeOnly("fd_checkpoints_edit"); cmd::SetPracticeOnly("fd_checkpoints_name"); cmd::SetPracticeOnly("fd_checkpoints_save");
    binds::DescribeKey("space ins mouse1 tab", "checkpoint editor: place (Space or Insert) at pointer / select / select next", "while fd_checkpoints_edit is 1");
    binds::DescribeKey("leftarrow rightarrow uparrow downarrow pgup pgdn", "checkpoint editor: move on world X/Z/Y axes; Shift resizes; Ctrl uses 1 unit", "while fd_checkpoints_edit is 1");
    binds::DescribeKey("ctrl+c ctrl+v ctrl+d del backspace", "checkpoint editor: copy / paste at pointer / duplicate / delete", "while fd_checkpoints_edit is 1");
    binds::DescribeKey("home end enter ctrl+s", "checkpoint editor: undo / redo / save (also autosaves)", "while fd_checkpoints_edit is 1");
    binds::DescribeKey("mouse1", "checkpoint editor: left-drag boxes/arrows to move, face cubes to resize; normal mouse look resumes on release", "while fd_checkpoints_edit is 1");
    binds::SetKeyCapture(Capture); binds::SetPointerCapture(CapturePointer);
    g_panel = textdraw::Create("checkpoints", textdraw::TOP_LEFT, 8, 80, 6, textdraw::C_TEXT, textdraw::STYLE_OPAQUE);
    textdraw::Show(g_panel, false);
    hooks::OnFrameEnd(Frame); gfx::OnLayer(gfx::LAYER_HUD, Reticle);
}

// Native contacts and attempt timer.
Set g_set{};
bool g_armed[kMax], g_hit[kMax], g_startValid;
uint32_t g_start, g_playerGeneration, g_levelGeneration;
bool g_entered;
void PlayerInitialized(void*) { ++g_playerGeneration; }
enum Phase { WaitingForPlayer, Ready, Failed, Inactive };
Phase g_phase = Inactive;
cmd::Cvar g_fdCheckpoints = {"fd_checkpoints", "0", "1 = checkpoint names and elapsed level time (N toggles); 0 = off", false, true, nullptr, {0}};
bool Clock(uint32_t* value) {
    uintptr_t slot = 0;
    return addr::Resolve(addr::Id::clock_elapsed_ms, &slot) && addr::SafeRead(slot, value, sizeof(*value));
}
void OnTimestamps(cmd::Cvar&) {
    console::Print("checkpoints: timestamps %s", g_fdCheckpoints.value[0] == '1' ? "on" : "off");
    if (g_fdCheckpoints.value[0] != '1') textdraw::ClearGameHint();
    const auto& s = state::Current();
    if (g_fdCheckpoints.value[0] == '1' && Practice() && s.gameState == 1 && g_phase == Inactive && LoadFile(s.map, &g_set)) g_phase = WaitingForPlayer;
}
void Status(int, const char* const*) {
    console::Print("checkpoints: timestamps %s; %d native checkpoints loaded for %s", g_fdCheckpoints.value[0] == '1' ? "on" : "off", g_phase == Ready ? g_set.count : 0, state::Current().map);
}
void LevelLoad(const char* map) {
    // LevelLoad also fires on menu -> play. Only a new player initialization
    // starts an attempt; resuming must not reset the timer or duplicate roots.
    if (g_entered && g_levelGeneration == g_playerGeneration) return;
    g_entered = true; g_levelGeneration = g_playerGeneration;
    EditorLevelLoad();
    entities::ResetNativeTriggers();
    textdraw::ClearGameHint(false);
    g_set.count = 0; g_phase = Inactive; g_startValid = Clock(&g_start);
    for (int i = 0; i < kMax; ++i) g_armed[i] = g_hit[i] = false;
    if (g_startValid && g_fdCheckpoints.value[0] == '1' && Practice() && LoadFile(map, &g_set)) g_phase = WaitingForPlayer;
}
void ContactEvent(int index, bool entering) {
    if (g_phase != Ready || index < 0 || index >= g_set.count) return;
    const bool enabled = g_fdCheckpoints.value[0] == '1' && Practice() && state::Current().gameState == 1;
    if (!Contact(entering, enabled, g_armed[index], g_hit[index])) return;
    uint32_t now = 0; if (!g_startValid || !Clock(&now)) { g_hit[index] = false; return; }
    const auto time = Elapsed(g_start, now);
    char line[textdraw::kLineLen];
    wsprintfA(line, "%s %02lu:%02lu:%03lu", g_set.points[index].label,
              static_cast<unsigned long>(time.minutes), static_cast<unsigned long>(time.seconds), static_cast<unsigned long>(time.millis));
    if (!textdraw::GameHint(line, 4000)) { g_hit[index] = false; return; }
    console::Print("checkpoint %s: %02lu:%02lu:%03lu", g_set.points[index].label,
                   static_cast<unsigned long>(time.minutes), static_cast<unsigned long>(time.seconds), static_cast<unsigned long>(time.millis));
}
void FrameEnd() {
    const auto& s = state::Current();
    if (g_phase != WaitingForPlayer || g_fdCheckpoints.value[0] != '1' || !Practice() || s.gameState != 1 || !s.playerValid) return;
    uintptr_t slot = 0; uint32_t player = 0; float pos[3], lo[3], hi[3];
    if (!addr::Resolve(addr::Id::player_object, &slot) || !addr::SafeRead(slot, &player, 4) || !player ||
        !addr::SafeRead(player+addr::Field(addr::Id::player_f_pos), pos, sizeof(pos)) ||
        !addr::SafeRead(player+addr::Field(addr::Id::player_f_mins), lo, sizeof(lo)) ||
        !addr::SafeRead(player+addr::Field(addr::Id::player_f_maxs), hi, sizeof(hi))) return;
    for (int a = 0; a < 3; ++a) {
        lo[a] += pos[a]; hi[a] += pos[a];
        if (!(lo[a] < hi[a] && lo[a] > -200000 && hi[a] < 200000)) return;
    }
    for (int i = 0; i < g_set.count; ++i) {
        const auto& point = g_set.points[i];
        // Only initial arming uses a body-box check. All hits come from native
        // touch/untouch events, not per-frame C++ overlap polling.
        g_armed[i] = !Inside(point, lo, hi);
        char name[48]; wsprintfA(name, "FDNY_PracticeCheckpoint_%d", i+1);
        if (!entities::AddNativeTrigger(name, point.min, point.max)) {
            g_phase = Failed; console::Print("checkpoints: native creation failed; restart the mission to retry"); return;
        }
    }
    g_phase = Ready;
    console::Print("checkpoints: loaded %d native checkpoints for %s", g_set.count, s.map);
}
const addr::Id kNeeds[] = {addr::Id::entity_trigger_factory, addr::Id::entity_admit,
    addr::Id::entity_trigger_initialize, addr::Id::game_session, addr::Id::entities_head,
    addr::Id::trigger_vtbl, addr::Id::script_fire_event, addr::Id::player_object,
    addr::Id::clock_elapsed_ms, addr::Id::hint_show, addr::Id::str_from_cstr,
    addr::Id::str_free_rep, addr::Id::str_empty_rep};
bool Init() {
    if (!entities::ObserveNativeContacts(ContactEvent) || !entities::ObservePlayer(PlayerInitialized, nullptr)) return false;
    if (!textdraw::InitGameHints()) return false;
    g_fdCheckpoints.onChange = OnTimestamps;
    cmd::RegisterCvar(&g_fdCheckpoints);
    cmd::RegisterCommand("fd_checkpoints_status", "show timestamp enable state and number of loaded native checkpoints", Status);
    cmd::SetPracticeOnly("fd_checkpoints");
    cmd::SetPracticeOnly("fd_checkpoints_status");
    binds::BindDefault("n", "toggle fd_checkpoints");
    InitEditor();
    hooks::OnLevelLoad(LevelLoad); hooks::OnFrameEnd(FrameEnd);
    log::Info("checkpoints: ready (Checkpoints/<mission>.txt; native contacts, elapsed level clock)"); return true;
}

} // namespace
} // namespace fdcore::checkpoints

FDCORE_MODULE(checkpoints, "checkpoints", fdcore::checkpoints::kNeeds, 13, nullptr, &fdcore::checkpoints::Init, 120)
