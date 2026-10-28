// The browser and viewer belong to the same replay module; calls run on the game thread.
#pragma once
namespace fdcore::replay {
bool PrepareView(const char* stage, const char* first, const char* second);
const char* ViewError();
bool RecordingDirectory(char* out, int cap);
namespace browser {
bool Init();
void Restore();
} // namespace browser
} // namespace fdcore::replay

// Portable, bounded display metadata. JSONL filenames and contents never change.
namespace fdcore::replay::browser {
constexpr int kNameLimit = 40;
constexpr int kPageSize = 6;
struct Metadata {
    char name[kNameLimit + 1];
    bool favorite;
    bool valid;
};
inline int Length(const char* s) {
    int n = 0;
    while (s[n])
        ++n;
    return n;
}
inline bool DisplayName(const char* text) {
    int n = Length(text);
    if (n > kNameLimit)
        return false;
    for (int i = 0; i < n; ++i)
        if (text[i] < 32 || text[i] > 126)
            return false;
    return true;
}
inline bool ParseMetadata(const char* text, int size, Metadata* out) {
    Metadata m = {"", false, true};
    constexpr char header[] = "FDNY_RECORDING_META_V1\n";
    int h = sizeof(header) - 1;
    if (size < h)
        return false;
    for (int i = 0; i < h; ++i)
        if (text[i] != header[i])
            return false;
    int at = h;
    bool name = false, fav = false;
    while (at < size) {
        int start = at;
        while (at < size && text[at] != '\n')
            ++at;
        if (at == size)
            return false;
        int n = at - start;
        ++at;
        if (n >= 9) {
            const char* k = "favorite=";
            bool same = true;
            for (int i = 0; i < 9; ++i)
                same &= text[start + i] == k[i];
            if (same) {
                if (fav || n != 10 || (text[start + 9] != '0' && text[start + 9] != '1'))
                    return false;
                m.favorite = text[start + 9] == '1';
                fav = true;
                continue;
            }
        }
        if (n >= 5) {
            const char* k = "name=";
            bool same = true;
            for (int i = 0; i < 5; ++i)
                same &= text[start + i] == k[i];
            if (same) {
                if (name || n - 5 > kNameLimit)
                    return false;
                for (int i = 5; i < n; ++i) {
                    char c = text[start + i];
                    if (c < 32 || c > 126)
                        return false;
                    m.name[i - 5] = c;
                }
                m.name[n - 5] = 0;
                name = true;
            }
        }
    }
    if (!name || !fav)
        return false;
    *out = m;
    return true;
}
inline int SerializeMetadata(const Metadata& m, char* out, int cap) {
    if (!m.valid || !DisplayName(m.name))
        return 0;
    const char* header = "FDNY_RECORDING_META_V1\nfavorite=";
    int n = Length(header), name = Length(m.name);
    if (n + 1 + 6 + name + 1 >= cap)
        return 0;
    int at = 0;
    while (header[at]) {
        out[at] = header[at];
        ++at;
    }
    out[at++] = m.favorite ? '1' : '0';
    out[at++] = '\n';
    const char* key = "name=";
    for (int i = 0; i < 5; ++i)
        out[at++] = key[i];
    for (int i = 0; i < name; ++i)
        out[at++] = m.name[i];
    out[at++] = '\n';
    out[at] = 0;
    return at;
}
inline int PageCount(int count) {
    return count > 0 ? (count + kPageSize - 1) / kPageSize : 1;
}
inline int ClampPage(int page, int count) {
    int pages = PageCount(count);
    if (page < 0)
        return 0;
    return page >= pages ? pages - 1 : page;
}
} // namespace fdcore::replay::browser
