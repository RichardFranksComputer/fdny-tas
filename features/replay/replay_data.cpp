#include "replay_data.h"
#include "num.h"

#include <windows.h>
#include <xmmintrin.h>

namespace fdcore::replay::data {
namespace {

// --- a JSON reader for the recorder's flat lines ----------------------------------------
// Each line is one object of numbers, strings and number arrays (recorder.cpp's writer).
struct Line {
    const char* p;
    const char* end;
};

// The value after "key": in this line, or null.
const char* Find(const Line& l, const char* key) {
    int kl = lstrlenA(key);
    for (const char* p = l.p; p + kl + 3 <= l.end; ++p) {
        if (p[0] != '"' || p[kl + 1] != '"') continue;
        bool same = true;
        for (int i = 0; i < kl && same; ++i) same = p[1 + i] == key[i];
        if (!same) continue;
        const char* v = p + kl + 2;
        while (v < l.end && (*v == ' ' || *v == ':')) ++v;
        return v;
    }
    return nullptr;
}

bool Number(const char*& p, const char* end, double* out) {
    double sign = 1, v = 0, scale = 0;
    bool digits = false;
    if (p < end && *p == '-') sign = -1, ++p;
    while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'), digits = true;
    if (p < end && *p == '.') {
        ++p, scale = 1;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'), scale *= 10, digits = true;
    }
    if (!digits) return false;
    if (scale) v /= scale;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        int es = 1, e = 0;
        if (p < end && (*p == '-' || *p == '+')) es = *p++ == '-' ? -1 : 1;
        while (p < end && *p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
        for (int i = 0; i < e; ++i) v = es > 0 ? v * 10 : v / 10;
    }
    *out = sign * v;
    return true;
}

bool Num(const Line& l, const char* key, double* out) {
    const char* v = Find(l, key);
    return v && Number(v, l.end, out);
}

bool Vec(const Line& l, const char* key, float out[3]) {
    const char* v = Find(l, key);
    if (!v || *v != '[') return false;
    ++v;
    for (int i = 0; i < 3; ++i) {
        while (v < l.end && (*v == ' ' || *v == ',')) ++v;
        double d;
        if (!Number(v, l.end, &d) || !(d > -1e8 && d < 1e8)) return false;  // also NaN
        out[i] = static_cast<float>(d);
    }
    return true;
}

bool Str(const Line& l, const char* key, char* out, int cap) {
    const char* v = Find(l, key);
    if (!v || *v != '"') return false;
    int o = 0;
    for (++v; v < l.end && *v != '"' && o + 1 < cap; ++v) out[o++] = *v == '\\' && v + 1 < l.end ? *++v : *v;
    out[o] = 0;
    return true;
}

bool Is(const Line& l, const char* type) {
    char t[24];
    return Str(l, "type", t, sizeof(t)) && lstrcmpA(t, type) == 0;
}

void Fail(char* err, int cap, const char* why, int line) { wsprintfA(err, "line %d: %s", line, why), (void)cap; }

// sqrt without the CRT (SSE, like glitch_bsp's Sqrt).

// sin / cos for |x| <= pi (pitch), no CRT: reduce to [-pi/2, pi/2], then Taylor to x^9.
float Sin(float x) {
    const float kPi = 3.14159265f;
    if (x > kPi / 2) x = kPi - x;
    if (x < -kPi / 2) x = -kPi - x;
    const float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
float Cos(float x) { return Sin(x + 3.14159265f / 2); }  // |x| <= pi/2: a pitch

uint32_t Lerp(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float ca = static_cast<float>((a >> sh) & 0xFF), cb = static_cast<float>((b >> sh) & 0xFF);
        out |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

} // namespace

bool Parse(const char* text, int len, Recording* out, char* err, int errCap) {
    *out = Recording{};
    err[0] = 0;
    if (len >= 3 && static_cast<uint8_t>(text[0]) == 0xEF) text += 3, len -= 3;  // BOM
    int lines = 0;
    for (int i = 0; i < len; ++i) lines += text[i] == '\n';
    out->s = static_cast<Sample*>(VirtualAlloc(nullptr, (lines + 1) * sizeof(Sample), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!out->s) return Fail(err, errCap, "out of memory", 0), false;
    bool started = false;
    int lineNo = 0;
    for (const char* p = text; p < text + len;) {
        const char* e = p;
        while (e < text + len && *e != '\n') ++e;
        if (e == text + len) break;  // an unfinished last line is dropped, like the site
        Line l = {p, e};
        p = e + 1, ++lineNo;
        while (l.p < l.end && (*l.p == ' ' || *l.p == '\t' || *l.p == '\r')) ++l.p;
        while (l.end > l.p && (l.end[-1] == ' ' || l.end[-1] == '\t' || l.end[-1] == '\r')) --l.end;
        if (l.p == l.end) continue;
        if (Is(l, "start")) {
            char schema[32];
            if (started) return Fail(err, errCap, "second start record", lineNo), false;
            if (!Str(l, "schema", schema, sizeof(schema)) || lstrcmpA(schema, "fdny-trajectory-v1") != 0)
                return Fail(err, errCap, "not fdny-trajectory-v1", lineNo), false;
            char stage[128];
            if (!Str(l, "stage", stage, sizeof(stage))) return Fail(err, errCap, "start without a stage", lineNo), false;
            const char* base = stage;  // last path part, no .bsp, lower case
            for (char* c = stage; *c; ++c) {
                if (*c == '\\' || *c == '/') base = c + 1;
                if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c - 'A' + 'a');
            }
            lstrcpynA(out->stage, base, sizeof(out->stage));
            const int sl = lstrlenA(out->stage);
            if (sl > 4 && lstrcmpA(out->stage + sl - 4, ".bsp") == 0) out->stage[sl - 4] = 0;
            started = true;
        } else if (Is(l, "sample")) {
            if (!started) return Fail(err, errCap, "sample before the start record", lineNo), false;
            Sample& s = out->s[out->n];
            double t, n, v;
            if (!Num(l, "n", &n) || static_cast<int>(n) != out->n) return Fail(err, errCap, "samples out of sequence (n)", lineNo), false;
            if (!Num(l, "t_ms", &t) || !(t >= 0 && t <= 31.0 * 86400000.0)) return Fail(err, errCap, "bad t_ms", lineNo), false;
            if (out->n && t < out->s[out->n - 1].t) return Fail(err, errCap, "t_ms goes backwards", lineNo), false;
            if (!Vec(l, "position", s.pos)) return Fail(err, errCap, "bad position", lineNo), false;
            s.t = static_cast<float>(t);
            if (!Vec(l, "body_forward", s.fwd)) s.fwd[0] = 0, s.fwd[1] = 0, s.fwd[2] = 1;
            s.pitch = Num(l, "pitch_deg", &v) ? static_cast<float>(v) : 0.0f;
            s.state = Num(l, "player_state_raw", &v) ? static_cast<int>(v) : 0;
            s.ground = Num(l, "ground_contact_raw", &v) ? static_cast<int>(v) : 1;
            bool bounds = Vec(l, "bounds_min", s.mins) && Vec(l, "bounds_max", s.maxs);
            for (int i = 0; bounds && i < 3; ++i) bounds = s.mins[i] < s.maxs[i];
            if (!bounds) {  // the site's boundsFor default: 60 x 60, 85 high (25 crouched)
                s.mins[0] = s.mins[1] = s.mins[2] = -30;
                s.maxs[0] = s.maxs[2] = 30, s.maxs[1] = s.state == 4 ? 25.0f : 85.0f;
            }
            ++out->n;
        }
        // victim_sample / victim_end / end: not drawn by the replay (player traces only)
    }
    if (!started) return Fail(err, errCap, "no start record", lineNo), false;
    if (!out->n) return Fail(err, errCap, "no samples", lineNo), false;
    out->duration = out->s[out->n - 1].t;
    return true;
}

void Free(Recording* r) {
    if (r->s) VirtualFree(r->s, 0, MEM_RELEASE);
    *r = Recording{};
}

bool CanJoin(const Sample& a, const Sample& b) {
    const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
    return b.t - a.t <= 500 && dx * dx + dy * dy + dz * dz <= 2000.0f * 2000.0f;
}

int IndexAt(const Recording& r, float t) {
    int lo = 0, hi = r.n - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (r.s[mid].t <= t) best = mid, lo = mid + 1;
        else hi = mid - 1;
    }
    return best;
}

Sample FrameAt(const Recording& r, float t) {
    int i = IndexAt(r, t);
    if (i < 0) i = 0;
    const Sample& a = r.s[i];
    if (i + 1 >= r.n || !CanJoin(a, r.s[i + 1]) || r.s[i + 1].t <= a.t) return a;
    const Sample& b = r.s[i + 1];
    const float k = (t - a.t) / (b.t - a.t);
    Sample s = a;
    for (int j = 0; j < 3; ++j) s.pos[j] = a.pos[j] + (b.pos[j] - a.pos[j]) * k;
    s.pitch = a.pitch + (b.pitch - a.pitch) * k;
    // Facing in the horizontal plane, the short way round: the normalised blend of the two
    // directions (the site's shortest-angle yaw blend, without trigonometry).
    float x = a.fwd[0] + (b.fwd[0] - a.fwd[0]) * k, z = a.fwd[2] + (b.fwd[2] - a.fwd[2]) * k;
    const float len = num::Sqrt(x * x + z * z);
    if (len > 1e-4f) s.fwd[0] = x / len, s.fwd[1] = 0, s.fwd[2] = z / len;
    return s;
}

void ArrowDir(const Sample& s, float out[3]) {
    const float p = s.pitch * 3.14159265f / 180.0f, c = Cos(p);
    out[0] = s.fwd[0] * c, out[1] = -Sin(p), out[2] = s.fwd[2] * c;
    const float len = num::Sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-6f)
        for (int i = 0; i < 3; ++i) out[i] /= len;
}

int Simplify(const Sample* s, int a, int b, float eps, const uint8_t* force, uint8_t* keep, int* work) {
    int sp = 0;
    for (int i = a, prev = a; i <= b; ++i) {
        keep[i] = i == a || i == b || (force && force[i]);
        if (keep[i] && i > prev) work[sp++] = prev, work[sp++] = i, prev = i;
    }
    while (sp) {  // ranges between kept points: keep the farthest point while it's beyond eps
        const int hi = work[--sp], lo = work[--sp];
        const float* p = s[lo].pos;
        const float ab[3] = {s[hi].pos[0] - p[0], s[hi].pos[1] - p[1], s[hi].pos[2] - p[2]};
        const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float best = eps * eps;
        int pick = -1;
        for (int k = lo + 1; k < hi; ++k) {
            const float ap[3] = {s[k].pos[0] - p[0], s[k].pos[1] - p[1], s[k].pos[2] - p[2]};
            float t = l2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float dx = ap[0] - ab[0] * t, dy = ap[1] - ab[1] * t, dz = ap[2] - ab[2] * t;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > best) best = d2, pick = k;
        }
        if (pick >= 0) keep[pick] = 1, work[sp++] = lo, work[sp++] = pick, work[sp++] = pick, work[sp++] = hi;
    }
    int n = 0;
    for (int i = a; i <= b; ++i) n += keep[i];
    return n;
}

void AirFlags(const Sample* s, int n, uint8_t* air) {
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && !s[j].ground) ++j;
        for (int k = i; k < j; ++k) air[k] = j - i >= kAirSamples;
        if (j < n) air[j++] = 0;
        i = j;
    }
}

namespace {
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool Digits(const char* p, int n) {
    for (int i = 0; i < n; ++i)
        if (p[i] < '0' || p[i] > '9') return false;
    return true;
}
unsigned UInt(const char*& p) {
    unsigned v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<unsigned>(*p++ - '0');
    return v;
}
// The last occurrence of s in [p, end), or null.
const char* FindLast(const char* p, const char* end, const char* s) {
    const int n = lstrlenA(s);
    for (const char* q = end - n; q >= p; --q) {
        int i = 0;
        while (i < n && q[i] == s[i]) ++i;
        if (i == n) return q;
    }
    return nullptr;
}
} // namespace

bool ParseName(const char* file, FileInfo* out) {
    *out = FileInfo{};
    lstrcpynA(out->name, file, sizeof(out->name));
    // From the end: <map>_YYYY-MM-DDTHH-MM-SS.mmmZ_pid<n>_run<n>.jsonl
    const char* pid = nullptr;
    for (const char* p = file; *p; ++p)
        if (p[0] == '_' && p[1] == 'p' && p[2] == 'i' && p[3] == 'd') pid = p;
    constexpr int kStamp = 24;
    if (!pid || pid - file < kStamp + 2) return false;
    const char* st = pid - kStamp;
    if (st[-1] != '_' || !Digits(st, 4) || st[4] != '-' || st[10] != 'T' || st[23] != 'Z') return false;
    const char* p = pid + 4;
    out->pid = UInt(p);
    if (p[0] != '_' || p[1] != 'r' || p[2] != 'u' || p[3] != 'n') return false;
    p += 4;
    out->run = UInt(p);
    const int mapLen = static_cast<int>(st - 1 - file);
    if (mapLen >= static_cast<int>(sizeof(out->map))) return false;
    for (int i = 0; i < mapLen; ++i) out->map[i] = Lower(file[i]);
    out->map[mapLen] = 0;
    lstrcpynA(out->stamp, st, kStamp + 1);
    return true;
}

void ReadTail(const char* tail, int len, FileInfo* f) {
    const char* end = tail + len;
    f->complete = FindLast(tail, end, "{\"type\":\"end\"") != nullptr;
    const char* s = FindLast(tail, end, "{\"type\":\"sample\"");
    if (!s) return;
    const char* eol = s;
    while (eol < end && *eol != '\n') ++eol;
    const char* t = FindLast(s, eol, "\"t_ms\":");  // in that line (victim lines follow it)
    if (!t) return;
    t += 7;
    f->duration = static_cast<float>(UInt(t));
}

void MarkFinished(FileInfo* f, int n) {
    for (int i = 0; i < n; ++i) {
        f[i].finished = false;
        if (!f[i].map[0] || !f[i].complete) continue;
        for (int j = 0; j < n; ++j)
            if (f[j].map[0] && f[j].pid == f[i].pid && f[j].run == f[i].run + 1)
                f[i].finished = lstrcmpA(f[j].map, f[i].map) != 0;
    }
}

int Best(const FileInfo* f, int n, const char* map) {
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (f[i].finished && f[i].duration > 0 && lstrcmpiA(f[i].map, map) == 0 && (best < 0 || f[i].duration < f[best].duration)) best = i;
    return best;
}

int Latest(const FileInfo* f, int n, const char* map) {
    int latest = -1;
    for (int i = 0; i < n; ++i)
        if (f[i].complete && f[i].duration > 0 && lstrcmpiA(f[i].map, map) == 0 && (latest < 0 || lstrcmpA(f[i].stamp, f[latest].stamp) > 0)) latest = i;
    return latest;
}

int Match(const FileInfo* f, int n, const char* text) {
    int pick = -1;
    const int tl = lstrlenA(text);
    for (int i = 0; i < n; ++i) {
        bool hit = false;
        for (const char* p = f[i].name; *p && !hit; ++p) {
            int k = 0;
            while (k < tl && p[k] && Lower(p[k]) == Lower(text[k])) ++k;
            hit = k == tl;
        }
        if (hit && (pick < 0 || lstrcmpiA(f[i].name, f[pick].name) < 0)) pick = i;
    }
    return pick;
}

uint32_t TimeColor(float f) {
    if (!(f > 0)) f = 0;
    if (f > 1) f = 1;
    constexpr uint32_t kRed = 0xFF0000, kBlue = 0x0000FF, kGreen = 0x00FF00;
    return f < 0.5f ? Lerp(kRed, kBlue, f * 2) : Lerp(kBlue, kGreen, (f - 0.5f) * 2);
}

uint32_t RankColor(int rank, int count) { return count <= 1 ? TimeColor(1) : TimeColor(1.0f - static_cast<float>(rank) / (count - 1)); }

} // namespace fdcore::replay::data
