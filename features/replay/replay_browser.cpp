// Native Recordings page: per-stage JSONL browsing and display metadata.
#include <windows.h>
#include "binds.h"
#include "cmd.h"
#include "console.h"
#include "log.h"
#include "menu.h"
#include "replay_browser.h"
#include "replay_data.h"
#include "testpipe.h"
namespace fdcore::replay::browser {
namespace {
namespace data = fdcore::replay::data;
namespace model = fdcore::replay::browser;
using menu::HideRows;
using menu::HighlightTab;
using menu::Rect;
using menu::SetRow;
using menu::Text;
int g_selected = -1, g_page, g_savedPage, g_group = 1, g_count, g_filtered, g_stageIndex;
int* g_order;
data::FileInfo* g_files;
Metadata* g_meta;
char g_folder[MAX_PATH], g_stage[40], g_stageName[64];
char g_stageKeys[64][40], g_stageTitles[64][64];
int g_stageCount;
char g_notice[128], g_rename[kNameLimit + 1];
bool g_selectAll;
enum View { LIST, DETAIL, COMPARE, RENAME };
View g_view = LIST;
int g_request = -1;
char g_tail[65536];
void Notice(const char* s) {
    lstrcpynA(g_notice, s, sizeof(g_notice));
    log::Info("replay browser: %s", s);
}
void Heading(const char* text) {
    int length = lstrlenA(text);
    float scale = length > 32 ? 0.95f : length > 24 ? 1.05f : 1.20f;
    Text(0, {190, 620, 56, 104}, text, scale);
    Text(3, {190, 620, 101, 112}, "--------------------------------------------------", 0.55f);
}
void FullPath(int i, char* out) { wsprintfA(out, "%s\\%s", g_folder, g_files[i].name); }
bool MetaPath(int i, char* out, int cap) {
    int n = lstrlenA(g_folder) + 1 + lstrlenA(g_files[i].name) + 10;
    if (n >= cap)
        return false;
    wsprintfA(out, "%s\\%s.fdny-meta", g_folder, g_files[i].name);
    return true;
}
void ReadMetadata(int i) {
    g_meta[i] = model::Metadata{"", false, true};
    char path[MAX_PATH];
    if (!MetaPath(i, path, sizeof(path))) {
        g_meta[i].valid = false;
        return;
    }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            g_meta[i].valid = false;
        return;
    }
    char text[256];
    DWORD got = 0, size = GetFileSize(f, nullptr);
    bool ok = size < sizeof(text) && ReadFile(f, text, size, &got, nullptr) && got == size &&
              model::ParseMetadata(text, static_cast<int>(got), &g_meta[i]);
    CloseHandle(f);
    if (!ok)
        g_meta[i].valid = false;
}
bool SaveMetadata(int i, const model::Metadata& meta) {
    if (!g_meta[i].valid) {
        Notice("Metadata is damaged; original file preserved.");
        return false;
    }
    char path[MAX_PATH], tmp[MAX_PATH], text[160];
    if (!MetaPath(i, path, sizeof(path)) || lstrlenA(path) + 16 >= MAX_PATH) {
        Notice("Recording path is too long to save metadata.");
        return false;
    }
    wsprintfA(tmp, "%s.%lu.tmp", path, GetCurrentProcessId());
    int size = model::SerializeMetadata(meta, text, sizeof(text));
    if (!size) {
        Notice("Name must be at most 40 printable characters.");
        return false;
    }
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, size, &wrote, nullptr) &&
              wrote == static_cast<DWORD>(size) && FlushFileBuffers(f);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (ok)
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileA(tmp);
        Notice("Could not save metadata; original is unchanged.");
        return false;
    }
    g_meta[i] = meta;
    return true;
}
void ParseStageCatalog() {
    char path[MAX_PATH];
    if (lstrlenA(console::GameDir()) + 36 >= MAX_PATH)
        return;
    wsprintfA(path, "%sData\\ResDefs\\thegame.lev", console::GameDir());
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD got = 0;
    bool ok = ReadFile(f, g_tail, sizeof(g_tail) - 1, &got, nullptr) != 0;
    CloseHandle(f);
    if (!ok)
        return;
    g_tail[got] = 0;
    for (char* p = g_tail; *p;) {
        char* line = p;
        while (*p && *p != '\r' && *p != '\n')
            ++p;
        if (*p)
            *p++ = 0;
        while (*p == '\r' || *p == '\n')
            ++p;
        if (line[0] == 'l' && line[1] == 'e' && line[2] == 'v' && line[3] == 'e' && line[4] == 'l' && line[5] == ':' &&
            g_stageCount < 64) {
            char* key = line + 6;
            while (*key == ' ' || *key == '\t')
                ++key;
            int n = 0;
            while (key[n] && key[n] != ' ' && key[n] != '\t' && n < 39) {
                g_stageKeys[g_stageCount][n] = key[n];
                ++n;
            }
            g_stageKeys[g_stageCount][n] = 0;
            lstrcpynA(g_stageTitles[g_stageCount], g_stageKeys[g_stageCount], 64);
            ++g_stageCount;
        } else if (g_stageCount && line[0] == 't' && line[1] == 'i' && line[2] == 't' && line[3] == 'l' &&
                   line[4] == 'e' && line[5] == ':') {
            char* text = line + 6;
            while (*text == ' ' || *text == '\t' || *text == '"')
                ++text;
            int n = 0;
            while (text[n] && text[n] != '"' && n < 63) {
                g_stageTitles[g_stageCount - 1][n] = text[n];
                ++n;
            }
            g_stageTitles[g_stageCount - 1][n] = 0;
        }
    }
}
void Stage() {
    g_stageIndex = menu::SelectedStage();
    g_stage[0] = g_stageName[0] = 0;
    if (g_stageIndex >= 0 && g_stageIndex < g_stageCount) {
        lstrcpynA(g_stage, g_stageKeys[g_stageIndex], sizeof(g_stage));
        lstrcpynA(g_stageName, g_stageTitles[g_stageIndex], sizeof(g_stageName));
    }
}
bool InStage(int i) { return g_files[i].map[0] && lstrcmpiA(g_files[i].map, g_stage) == 0; }
void Order() {
    g_filtered = 0;
    for (int i = 0; i < g_count; ++i) {
        if (!InStage(i))
            continue;
        if (g_view == COMPARE) {
            if (i == g_selected || g_files[i].duration <= 0)
                continue;
        } else if (g_group == 3 && !g_meta[i].favorite)
            continue;
        int at = g_filtered;
        while (at > 0) {
            int j = g_order[at - 1];
            if (lstrcmpA(g_files[i].stamp, g_files[j].stamp) <= 0)
                break;
            g_order[at] = j;
            --at;
        }
        g_order[at] = i;
        ++g_filtered;
    }
    g_page = model::ClampPage(g_page, g_filtered);
}
void Scan() {
    if (g_files)
        VirtualFree(g_files, 0, MEM_RELEASE);
    if (g_meta)
        VirtualFree(g_meta, 0, MEM_RELEASE);
    if (g_order)
        VirtualFree(g_order, 0, MEM_RELEASE);
    g_files = nullptr;
    g_meta = nullptr;
    g_order = nullptr;
    g_count = g_filtered = 0;
    if (!RecordingDirectory(g_folder, sizeof(g_folder))) {
        Notice("Recordings folder path is too long.");
        return;
    }
    char pattern[MAX_PATH];
    if (lstrlenA(g_folder) + 9 >= MAX_PATH) {
        Notice("Recordings folder path is too long.");
        return;
    }
    wsprintfA(pattern, "%s\\*.jsonl", g_folder);
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        Notice("No recordings for this stage yet.");
        return;
    }
    int count = 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && lstrlenA(found.cFileName) < 112)
            ++count;
    } while (FindNextFileA(find, &found));
    FindClose(find);
    if (!count) {
        Notice("No recordings found.");
        return;
    }
    if (count > 100000) {
        Notice("Too many recordings to index safely.");
        return;
    }
    g_files = static_cast<data::FileInfo*>(
        VirtualAlloc(nullptr, count * sizeof(data::FileInfo), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_meta = static_cast<model::Metadata*>(
        VirtualAlloc(nullptr, count * sizeof(model::Metadata), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_order = static_cast<int*>(VirtualAlloc(nullptr, count * sizeof(int), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_files || !g_meta || !g_order) {
        Notice("Not enough memory to index recordings.");
        return;
    }
    find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || lstrlenA(found.cFileName) >= 112 || g_count >= count)
            continue;
        data::FileInfo info = {};
        if (!data::ParseName(found.cFileName, &info))
            continue;
        char path[MAX_PATH];
        if (lstrlenA(g_folder) + 1 + lstrlenA(found.cFileName) >= MAX_PATH)
            continue;
        wsprintfA(path, "%s\\%s", g_folder, found.cFileName);
        HANDLE f =
            CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        DWORD size = GetFileSize(f, nullptr), want = size < sizeof(g_tail) ? size : sizeof(g_tail), got = 0;
        if (size != INVALID_FILE_SIZE &&
            SetFilePointer(f, size - want, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
            ReadFile(f, g_tail, want, &got, nullptr))
            data::ReadTail(g_tail, static_cast<int>(got), &info);
        CloseHandle(f);
        g_files[g_count] = info;
        ReadMetadata(g_count);
        ++g_count;
    } while (FindNextFileA(find, &found));
    FindClose(find);
    Order();
    if (!g_filtered)
        Notice("No recordings for this stage yet.");
    else
        g_notice[0] = 0;
}
int Digits(const char* s, int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + s[i] - '0';
    }
    return v;
}
void Name(int i, char* out, int cap) {
    if (g_meta[i].name[0]) {
        lstrcpynA(out, g_meta[i].name, cap);
        return;
    }
    const char* s = g_files[i].stamp;
    SYSTEMTIME utc = {}, local = {};
    utc.wYear = static_cast<WORD>(Digits(s, 4));
    utc.wMonth = static_cast<WORD>(Digits(s + 5, 2));
    utc.wDay = static_cast<WORD>(Digits(s + 8, 2));
    utc.wHour = static_cast<WORD>(Digits(s + 11, 2));
    utc.wMinute = static_cast<WORD>(Digits(s + 14, 2));
    utc.wSecond = static_cast<WORD>(Digits(s + 17, 2));
    FILETIME ft, lt;
    if (SystemTimeToFileTime(&utc, &ft) && FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &local)) {
        char text[64];
        wsprintfA(text, "%02u/%02u %02u:%02u", local.wMonth, local.wDay, local.wHour, local.wMinute);
        lstrcpynA(out, text, cap);
    } else
        lstrcpynA(out, g_files[i].name, cap);
}
void Duration(int i, char* out) {
    int ms = static_cast<int>(g_files[i].duration);
    wsprintfA(out, "%d:%02d.%01d", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
}
void PageRows() {
    for (int r = 0; r < model::kPageSize; ++r) {
        int slot = g_page * model::kPageSize + r;
        if (slot >= g_filtered)
            break;
        int i = g_order[slot];
        char name[64], dur[24], text[128];
        Name(i, name, sizeof(name));
        Duration(i, dur);
        name[33] = 0;
        int y = 160 + r * 42;
        wsprintfA(text, "%02d   %s%s", slot + 1, g_meta[i].favorite ? "* " : "", name);
        SetRow(4 + r, 100 + r, {190, 620, y, y + 37}, text, true);
        wsprintfA(text, "%s%s", dur, g_files[i].complete ? "" : "  |  incomplete");
        Text(4 + r, {224, 620, y + 21, y + 36}, text, 0.72f);
        Text(10 + r, {190, 620, y + 33, y + 40}, "--------------------------------------------------", 0.45f);
    }
    if (g_page > 0)
        SetRow(14, 30, {190, 375, 417, 445}, "< Previous", true);
    if ((g_page + 1) * model::kPageSize < g_filtered)
        SetRow(15, 31, {475, 620, 417, 445}, "Next >", true);
    char footer[128];
    if (g_notice[0])
        lstrcpynA(footer, g_notice, sizeof(footer));
    else
        wsprintfA(footer, "Page %d/%d  |  %d recordings", g_page + 1, model::PageCount(g_filtered), g_filtered);
    Text(16, {190, 620, 450, 470}, footer, 0.68f);
}
void Render() {
    HideRows();
    char text[160];
    if (g_view != LIST) {
        HighlightTab(1, false);
        HighlightTab(2, false);
    }
    if (g_view == LIST || g_view == COMPARE) {
        if (g_view == LIST) {
            wsprintfA(text, "%s recordings", g_stageName);
            Heading(text);
            SetRow(1, 19, {190, 390, 117, 150}, g_group == 1 ? "> Recent" : "Recent", true);
            SetRow(2, 21, {415, 620, 117, 150}, g_group == 3 ? "> Favorites" : "Favorites", true);
            HighlightTab(1, g_group == 1);
            HighlightTab(2, g_group == 3);
        } else {
            Heading("Compare recordings");
            char name[64];
            Name(g_selected, name, sizeof(name));
            wsprintfA(text, "First: %.40s", name);
            Text(1, {190, 620, 119, 148}, text, 0.80f);
        }
        PageRows();
        if (!g_filtered)
            Text(4, {190, 620, 180, 225},
                 g_view == COMPARE ? "No other recordings to compare."
                 : g_group == 3    ? "No favorites yet."
                                   : "No recordings yet.",
                 0.85f);
    } else if (g_selected >= 0 && g_selected < g_count) {
        char name[64], dur[24];
        Name(g_selected, name, sizeof(name));
        Duration(g_selected, dur);
        if (g_view == DETAIL) {
            Heading(name);
            wsprintfA(text, "%s  |  %s%s", g_stageName, dur, g_files[g_selected].complete ? "" : "  |  incomplete");
            Text(1, {190, 620, 120, 153}, text, 0.85f);
            if (g_files[g_selected].duration > 0) {
                SetRow(2, 40, {190, 620, 177, 219}, "Show ghost", true);
                SetRow(3, 41, {190, 620, 231, 273}, "Compare", true);
            }
            SetRow(4, 42, {190, 620, 285, 327}, "Rename", true);
            SetRow(5, 43, {190, 620, 339, 381}, g_meta[g_selected].favorite ? "Remove favorite" : "Favorite", true);
            if (g_notice[0])
                Text(16, {190, 620, 415, 455}, g_notice, 0.72f);
        } else {
            Heading("Rename recording");
            wsprintfA(text, "%s%s", g_rename[0] ? g_rename : "(default name)", GetTickCount() % 1000 < 500 ? "_" : "");
            Text(1, {190, 620, 161, 219}, text, 1.0f);
            Text(2, {190, 620, 230, 270}, "Type a name (40 characters maximum).", 0.75f);
            SetRow(3, 44, {190, 620, 290, 330}, "Save name", true);
            SetRow(4, 45, {190, 620, 340, 380}, "Cancel", true);
            Text(16, {190, 620, 415, 455}, g_notice[0] ? g_notice : "Blank restores the original display name.", 0.68f);
        }
    }
}
void Return() {
    g_notice[0] = 0;
    if (g_view == LIST)
        menu::Close();
    else if (g_view == COMPARE || g_view == RENAME)
        g_view = DETAIL;
    else {
        g_view = LIST;
        g_page = g_savedPage;
        Order();
    }
}
bool StartViewer(int second) {
    if (g_selected < 0 || g_selected >= g_count)
        return false;
    char a[MAX_PATH], b[MAX_PATH];
    FullPath(g_selected, a);
    b[0] = 0;
    if (second >= 0)
        FullPath(second, b);
    if (!PrepareView(g_stage, a, b)) {
        Notice(ViewError());
        return false;
    }
    g_view = LIST;
    g_notice[0] = 0;
    menu::LaunchStage();
    return true;
}
void Button(int id) {
    if (id == 0) {
        Return();
        return;
    }
    if (id == menu::kInertRow)
        return;
    g_notice[0] = 0;
    if ((id == 19 || id == 21) && g_view == LIST) {
        g_group = id == 19 ? 1 : 3;
        g_page = 0;
        Order();
    } else if (id == 30 || id == 31) {
        g_page = model::ClampPage(g_page + (id == 30 ? -1 : 1), g_filtered);
    } else if (id >= 100 && id < 100 + model::kPageSize) {
        int row = g_page * model::kPageSize + id - 100;
        if (row >= g_filtered)
            return;
        int i = g_order[row];
        if (g_view == COMPARE) {
            StartViewer(i);
            return;
        }
        g_savedPage = g_page;
        g_selected = i;
        g_view = DETAIL;
    } else if (id == 40 && g_view == DETAIL)
        StartViewer(-1);
    else if (id == 41 && g_view == DETAIL) {
        g_view = COMPARE;
        g_page = 0;
        Order();
    } else if (id == 42 && g_view == DETAIL) {
        g_view = RENAME;
        lstrcpynA(g_rename, g_meta[g_selected].name, sizeof(g_rename));
        g_selectAll = true;
    } else if (id == 43 && g_view == DETAIL) {
        auto meta = g_meta[g_selected];
        meta.favorite = !meta.favorite;
        if (SaveMetadata(g_selected, meta))
            Notice(meta.favorite ? "Added to favorites." : "Removed from favorites.");
    } else if (id == 44 && g_view == RENAME) {
        auto meta = g_meta[g_selected];
        lstrcpynA(meta.name, g_rename, sizeof(meta.name));
        if (SaveMetadata(g_selected, meta)) {
            g_view = DETAIL;
            Notice("Display name saved.");
        }
    } else if (id == 45 && g_view == RENAME)
        g_view = DETAIL;
}
bool Input(unsigned msg, uintptr_t w, intptr_t) {
    if (menu::Active()) {
        if (msg == WM_KEYDOWN &&
            (w == VK_ESCAPE || ((g_view == LIST || g_view == COMPARE) && (w == VK_PRIOR || w == VK_NEXT)))) {
            g_request = w == VK_ESCAPE ? 0 : w == VK_PRIOR ? 30 : 31;
            return true;
        }
        if (msg == WM_MOUSEWHEEL && (g_view == LIST || g_view == COMPARE)) {
            g_request = static_cast<short>(HIWORD(w)) > 0 ? 30 : 31;
            return true;
        }
        if (g_view == RENAME) {
            if (msg == WM_KEYDOWN) {
                if (w == VK_RETURN) {
                    g_request = 44;
                    return true;
                }
                if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    g_selectAll = true;
                    return true;
                }
                if (w == VK_BACK) {
                    int n = lstrlenA(g_rename);
                    if (g_selectAll)
                        g_rename[0] = 0;
                    else if (n)
                        g_rename[n - 1] = 0;
                    g_selectAll = false;
                    return true;
                }
                return true;
            }
            if (msg == WM_KEYUP)
                return true;
            if (msg == WM_CHAR) {
                if (w >= 32 && w <= 126) {
                    if (g_selectAll)
                        g_rename[0] = 0;
                    g_selectAll = false;
                    int n = lstrlenA(g_rename);
                    if (n < model::kNameLimit) {
                        g_rename[n] = static_cast<char>(w);
                        g_rename[n + 1] = 0;
                    }
                }
                return true;
            }
        }
    }
    return false;
}
void AfterDraw() {
    const int request = g_request;
    g_request = -1;
    if (request >= 0)
        Button(request);
}
void Open() {
    Stage();
    g_view = LIST;
    g_group = 1;
    g_page = 0;
    g_selected = -1;
    Scan();
}
} // namespace
bool Init() {
    ParseStageCatalog();
    if (!g_stageCount) {
        log::Warn("replay browser: stage catalog unavailable");
        return false;
    }
    const menu::Callbacks callbacks = {Open, Render, AfterDraw, Button, Input};
    if (!menu::RegisterPage("Recordings", {410, 620, 375, 411}, callbacks)) {
        log::Warn("replay browser: native menu unavailable");
        return false;
    }
#ifdef FDCORE_DEBUG
    // Test hook: `recordings`: the page's state (view 0 list, 1 details, 2 compare, 3 rename).
    testpipe::Register("recordings", [](const char*, char* out, int) {
        const bool sel = g_selected >= 0 && g_selected < g_count;
        wsprintfA(out, "open=%d view=%d group=%d filtered=%d page=%d favorite=%d stage=%s name=%s", menu::Active(),
                  g_view, g_group, g_filtered, g_page, sel && g_meta[g_selected].favorite, g_stage,
                  sel ? g_meta[g_selected].name : "");
    });
#endif
    log::Info("replay browser: ready");
    return true;
}
void Restore() {
    g_view = LIST;
    g_page = g_savedPage;
    g_notice[0] = 0;
    Order();
    menu::ReturnFromLevel();
}
} // namespace fdcore::replay::browser
