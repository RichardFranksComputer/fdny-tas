// Checkpoint file format, editing math and attempt timer rules.
#pragma once
#include <stdint.h>
namespace fdcore::checkpoints {
constexpr int kMax = 32, kTextMax = 8192;
struct Point { char label[81]; float min[3], max[3]; };
struct Set { char map[16]; int count; Point points[kMax]; };
inline bool Equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
inline bool Mission(const char* name) {
    const char* maps[] = {"training","bank","bar_r","gas_r","mbase","boat_a","boat_b","subway_a","subway_b","oil_rig_a","oil_rig_b","oil_rig_c","oil_rig_d"};
    for (const auto* map : maps) if (Equal(name, map)) return true;
    return false;
}
inline bool Number(const char* text, float* out) {
    bool negative = false, digit = false;
    if (*text == '-' || *text == '+') negative = *text++ == '-';
    double value = 0, place = .1;
    while (*text >= '0' && *text <= '9') {
        digit = true; value = value*10+(*text++-'0'); if (value > 110000) return false;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') { digit = true; value += (*text++-'0')*place; place *= .1; }
    }
    if (!digit || *text || value > 110000) return false;
    *out = static_cast<float>(negative ? -value : value); return true;
}
inline char* Line(char*& cursor) {
    if (!*cursor) return nullptr;
    char* line = cursor;
    while (*cursor && *cursor != '\n') ++cursor;
    char* end = cursor; if (*cursor) *cursor++ = 0;
    if (end > line && end[-1] == '\r') end[-1] = 0;
    return line;
}
inline bool Fields(char* line, char** f, int count) {
    for (int i = 0; i < count; ++i) {
        f[i] = line; while (*line && *line != '|') ++line;
        if (i == count-1) return !*line;
        if (!*line) return false;
        *line++ = 0;
    }
    return false;
}
inline bool Label(const char* text) {
    int n = 0;
    for (; text[n]; ++n) {
        const unsigned char c = text[n];
        if (n == 80 || c < 32 || c > 126 || c == '(' || c == ')' || c == '|') return false;
    }
    return n != 0;
}
inline bool Parse(char* text, Set* out) {
    char* cursor = text; char* header = Line(cursor); char* f[7];
    if (!header || !Fields(header, f, 3) || !Equal(f[0], "FDNY_CHECKPOINTS_V1") || !Mission(f[1])) return false;
    int count = 0;
    if (!*f[2]) return false;
    for (const char* n = f[2]; *n; ++n) {
        if (*n < '0' || *n > '9' || count > kMax) return false;
        count = count*10+*n-'0';
    }
    if (count < 0 || count > kMax) return false;
    int m = 0; do { out->map[m] = f[1][m]; } while (f[1][m++]);
    out->count = count;
    for (int i = 0; i < count; ++i) {
        char* line = Line(cursor); if (!line || !Fields(line, f, 7)) return false;
        Point& p = out->points[i];
        for (int a = 0; a < 3; ++a)
            if (!Number(f[a], &p.min[a]) || !Number(f[a+3], &p.max[a]) || p.max[a]-p.min[a] < 7.99f || p.max[a]-p.min[a] > 10000.01f) return false;
        if (!Label(f[6])) return false;
        int n = 0; do { p.label[n] = f[6][n]; } while (f[6][n++]);
    }
    return !*cursor;
}
// Editing uses world axes: X/Z horizontal, Y vertical. Changes are transactional.
inline bool Adjust(Point* p, int axis, float delta, bool resize) {
    if (axis < 0 || axis > 2) return false;
    Point next = *p;
    next.min[axis] += resize ? -delta*.5f : delta;
    next.max[axis] += resize ? delta*.5f : delta;
    const float size = next.max[axis]-next.min[axis];
    if (!(size >= 8 && size <= 10000 && next.min[axis] >= -105000 && next.max[axis] <= 105000)) return false;
    *p = next; return true;
}
inline int Pick(const Set& set, const float* origin, const float* direction) {
    int selected = -1; float nearest = 200000;
    for (int i = 0; i < set.count; ++i) {
        float entry = 0, exit = nearest;
        for (int a = 0; a < 3 && exit >= entry; ++a) {
            if (direction[a] > -.000001f && direction[a] < .000001f) {
                if (origin[a] < set.points[i].min[a] || origin[a] > set.points[i].max[a]) exit = -1;
            } else {
                float lo = (set.points[i].min[a]-origin[a])/direction[a];
                float hi = (set.points[i].max[a]-origin[a])/direction[a];
                if (lo > hi) { const float swap = lo; lo = hi; hi = swap; }
                if (lo > entry) entry = lo;
                if (hi < exit) exit = hi;
            }
        }
        if (exit >= entry && entry < nearest) nearest = entry, selected = i;
    }
    return selected;
}
// Screen-space drag math shared by mouse manipulation and unit tests.
inline bool RayPlane(const float* origin, const float* direction, const float* plane, const float* normal, float* out) {
    float denominator = 0, distance = 0;
    for (int a = 0; a < 3; ++a) denominator += direction[a]*normal[a], distance += (plane[a]-origin[a])*normal[a];
    if (denominator > -.00001f && denominator < .00001f) return false;
    distance /= denominator;
    if (distance < 0) return false;
    for (int a = 0; a < 3; ++a) out[a] = origin[a]+direction[a]*distance;
    return true;
}
inline bool DragPoint(const Point& start, const float* delta, int axis, int face, Point* out) {
    Point next = start;
    for (int a = 0; a < 3; ++a) {
        if (axis >= 0 && a != axis) continue;
        if (!face) next.min[a] += delta[a], next.max[a] += delta[a];
        else {
            float value = (face < 0 ? start.min[a] : start.max[a])+delta[a];
            const float lo = face < 0 ? start.max[a]-10000 : start.min[a]+8;
            const float hi = face < 0 ? start.max[a]-8 : start.min[a]+10000;
            if (value < lo) value = lo;
            if (value > hi) value = hi;
            (face < 0 ? next.min[a] : next.max[a]) = value;
        }
    }
    for (int a = 0; a < 3; ++a)
        if (!(next.min[a] >= -105000 && next.max[a] <= 105000)) return false;
    *out = next; return true;
}
inline void AlphabetLabel(unsigned index, char* out) {
    const char* prefix = "Checkpoint "; while (*prefix) *out++ = *prefix++;
    char letters[8]; int n = 0;
    do { letters[n++] = static_cast<char>('A'+index%26); index = index/26; if (!index) break; --index; } while (n < 8);
    while (n) *out++ = letters[--n];
    *out = 0;
}
// Four decimal places; never locale-dependent or exponential.
inline int Write(const Set& set, char* out, int cap) {
    if (!Mission(set.map) || set.count < 0 || set.count > kMax || cap < 1) return 0;
    int n = 0;
    auto put = [&](char c) { if (n < cap-1) out[n++] = c; else n = cap; };
    auto text = [&](const char* s) { while (*s) put(*s++); };
    auto integer = [&](uint32_t v) {
        char digits[10]; int k = 0;
        do { digits[k++] = static_cast<char>('0'+v%10); v /= 10; } while (v);
        while (k) put(digits[--k]);
    };
    text("FDNY_CHECKPOINTS_V1|"); text(set.map); put('|'); integer(set.count); put('\n');
    for (int i = 0; i < set.count; ++i) {
        const Point& p = set.points[i];
        for (int k = 0; k < 6; ++k) {
            const float value = k < 3 ? p.min[k] : p.max[k-3];
            if (!(value >= -110000 && value <= 110000)) return 0;
            const double magnitude = value < 0 ? -static_cast<double>(value) : value;
            const uint32_t fixed = static_cast<uint32_t>(magnitude*10000+.5);
            if (value < 0 && fixed) put('-');
            integer(fixed/10000);
            uint32_t fraction = fixed%10000;
            if (fraction) {
                put('.');
                for (uint32_t place = 1000; fraction; place /= 10) {
                    put(static_cast<char>('0'+fraction/place)); fraction %= place;
                }
            }
            put('|');
        }
        for (int a = 0; a < 3; ++a)
            if (!(p.max[a]-p.min[a] >= 7.99f && p.max[a]-p.min[a] <= 10000.01f)) return 0;
        if (!Label(p.label)) return 0;
        text(p.label);
        put('\n');
    }
    if (n >= cap) return 0;
    out[n] = 0; return n;
}
inline bool Inside(const Point& p, const float* lo, const float* hi) {
    for (int a = 0; a < 3; ++a) if (lo[a] > p.max[a] || hi[a] < p.min[a]) return false;
    return true;
}
// These are native contact notifications, not a separate overlap detector.
inline bool Contact(bool entering, bool enabled, bool& armed, bool& hit) {
    if (!entering) { armed = true; return false; }
    if (!enabled || !armed || hit) return false;
    hit = true; return true;
}
struct Time { uint32_t minutes, seconds, millis; };
inline Time Elapsed(uint32_t start, uint32_t now) {
    const uint32_t elapsed = now-start;
    return {elapsed/60000, elapsed/1000%60, elapsed%1000};
}
} // namespace fdcore::checkpoints
