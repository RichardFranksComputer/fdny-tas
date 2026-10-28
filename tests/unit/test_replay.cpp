// replay_data against the fdny-maps website's rules, on the sample recording in tests/game/data/
// (expected numbers computed from the file with the site's canJoin, 2026-10-25).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../features/replay/replay_data.h"
#include "../../features/replay/replay_browser.h"
#include "unit.h"

namespace d = fdcore::replay::data;

namespace {
char* ReadFile(const char* path, int* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *len = static_cast<int>(ftell(f));
    fseek(f, 0, SEEK_SET);
    char* b = static_cast<char*>(malloc(*len + 1));
    *len = static_cast<int>(fread(b, 1, *len, f));
    fclose(f);
    return b;
}
bool Near(float a, float b, float eps = 1e-3f) { return fabsf(a - b) <= eps; }
bool Parse(const char* text, d::Recording* r, char* err) { return d::Parse(text, static_cast<int>(strlen(text)), r, err, 128); }
} // namespace

void TestReplayParsesSample() {
    int len = 0;
    char* text = ReadFile("tests/game/data/mbase_2026-10-23T21-11-10.274Z_pid204_run0001.jsonl", &len);
    CHECK(text != nullptr);
    if (!text) return;
    d::Recording r;
    char err[128];
    CHECK(d::Parse(text, len, &r, err, sizeof(err)));
    CHECK(strcmp(r.stage, "mbase") == 0);
    CHECK(r.n == 1391);
    CHECK(Near(r.duration, 33847));
    CHECK(Near(r.s[0].t, 16));
    // Values straight from the file, unchanged (sample 700).
    CHECK(Near(r.s[700].t, 16552));
    CHECK(Near(r.s[700].pos[0], 1551.00696f) && Near(r.s[700].pos[1], -181.826859f) && Near(r.s[700].pos[2], -3397.72583f));
    CHECK(Near(r.s[700].pitch, -12.7999992f));
    CHECK(Near(r.s[700].mins[0], -30) && Near(r.s[700].maxs[1], 85));
    // The site's gap rule breaks this run in exactly two places: before samples 3 and 1390.
    int breaks = 0, first = -1, second = -1;
    for (int i = 1; i < r.n; ++i)
        if (!d::CanJoin(r.s[i - 1], r.s[i])) (breaks++ ? second : first) = i;
    CHECK(breaks == 2 && first == 3 && second == 1390);
    // frameAt halfway between 700 and 701: position and pitch linear.
    const d::Sample m = d::FrameAt(r, (16552 + 16569) / 2.0f);
    CHECK(Near(m.pos[0], (1551.00696f + 1556.71606f) / 2, 0.01f));
    CHECK(d::IndexAt(r, 15) == -1 && d::IndexAt(r, 16) == 0 && d::IndexAt(r, 1e9f) == r.n - 1);
    d::Free(&r);
    free(text);
}

void TestReplayRejectsBadFiles() {
    d::Recording r;
    char err[128];
    CHECK(!Parse("{\"type\":\"sample\",\"n\":0,\"t_ms\":1,\"position\":[0,0,0]}\n", &r, err));  // no start first
    CHECK(!Parse("{\"type\":\"start\",\"schema\":\"other\",\"stage\":\"bank\"}\n", &r, err));
    const char* gap = "{\"type\":\"start\",\"schema\":\"fdny-trajectory-v1\",\"stage\":\"bank\"}\n"
                      "{\"type\":\"sample\",\"n\":0,\"t_ms\":1,\"position\":[0,0,0]}\n"
                      "{\"type\":\"sample\",\"n\":2,\"t_ms\":2,\"position\":[0,0,0]}\n";
    CHECK(!Parse(gap, &r, err) && strstr(err, "sequence"));
    // No end record: keep complete samples and ignore the unfinished final line.
    const char* ok = "{\"type\":\"start\",\"schema\":\"fdny-trajectory-v1\",\"stage\":\"Levels\\\\Bank.BSP\"}\n"
                     "{\"type\":\"sample\",\"n\":0,\"t_ms\":1,\"position\":[1,2,3],\"player_state_raw\":4}\n"
                     "{\"type\":\"sample\",\"n\":1,\"t_ms\":2,\"position\":[3,2,3]}\n"
                     "{\"type\":\"sample\",\"n\":2,\"t_ms\":3,\"position\":[";
    CHECK(Parse(ok, &r, err));
    CHECK(strcmp(r.stage, "bank") == 0 && r.n == 2);
    CHECK(Near(r.s[0].maxs[1], 25) && Near(r.s[0].fwd[2], 1));  // crouched default box, default facing
    CHECK(Near(r.duration, 2) && Near(d::FrameAt(r, 1.5f).pos[0], 2));
    CHECK(Near(d::FrameAt(r, 200).pos[0], 3));
    d::Free(&r);
}

namespace {
float SegDist(const float* p, const float* a, const float* b) {
    float ab[3], ap[3], l2 = 0, t = 0;
    for (int i = 0; i < 3; ++i) ab[i] = b[i] - a[i], ap[i] = p[i] - a[i], l2 += ab[i] * ab[i], t += ap[i] * ab[i];
    t = l2 > 0 ? t / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float d2 = 0;
    for (int i = 0; i < 3; ++i) d2 += (ap[i] - ab[i] * t) * (ap[i] - ab[i] * t);
    return sqrtf(d2);
}
} // namespace

// The drawn line through the reference recording: under a third of the samples, every
// dropped sample within eps of the line kept (Python's Douglas-Peucker: 476 of 1391 at 0.5).
void TestReplaySimplify() {
    int len = 0;
    char* text = ReadFile("tests/game/data/mbase_2026-10-23T21-11-10.274Z_pid204_run0001.jsonl", &len);
    CHECK(text != nullptr);
    if (!text) return;
    d::Recording r;
    char err[128];
    CHECK(d::Parse(text, len, &r, err, sizeof(err)));
    unsigned char* keep = static_cast<unsigned char*>(calloc(r.n, 1));
    int* work = static_cast<int*>(malloc(2 * r.n * sizeof(int)));
    const int kept = d::Simplify(r.s, 0, r.n - 1, 0.5f, nullptr, keep, work);
    CHECK(kept > 400 && kept < 500);
    CHECK(keep[0] && keep[r.n - 1]);
    bool within = true;
    for (int i = 0, prev = 0; i < r.n; ++i) {
        if (!keep[i]) continue;
        for (int k = prev + 1; k < i; ++k) within = within && SegDist(r.s[k].pos, r.s[prev].pos, r.s[i].pos) <= 0.5f + 1e-3f;
        prev = i;
    }
    CHECK(within);
    // Forced points stay: a straight line keeps only its ends and the forced one.
    d::Sample line[5] = {};
    for (int i = 0; i < 5; ++i) line[i].pos[0] = static_cast<float>(i * 10);
    unsigned char force[5] = {0, 0, 1, 0, 0}, k5[5];
    int w5[10];
    CHECK(d::Simplify(line, 0, 4, 0.5f, force, k5, w5) == 3 && k5[0] && k5[2] && k5[4] && !k5[1] && !k5[3]);
    free(keep), free(work);
    d::Free(&r);
    free(text);
}

// In the air: off the ground for 3 samples or more; one or two off (stairs) don't count.
void TestReplayAirFlags() {
    const int g[10] = {1, 0, 1, 0, 0, 1, 0, 0, 0, 1};
    d::Sample s[10] = {};
    for (int i = 0; i < 10; ++i) s[i].ground = g[i];
    unsigned char air[10];
    d::AirFlags(s, 10, air);
    const unsigned char want[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 0};
    CHECK(memcmp(air, want, sizeof(want)) == 0);
}

void TestReplayColors() {
    CHECK(d::TimeColor(0) == 0xFF0000);
    CHECK(d::TimeColor(0.5f) == 0x0000FF);
    CHECK(d::TimeColor(1) == 0x00FF00);
    CHECK(d::RankColor(0, 3) == 0x00FF00 && d::RankColor(1, 3) == 0x0000FF && d::RankColor(2, 3) == 0xFF0000);
    d::Sample s = {};
    s.fwd[2] = 1, s.pitch = 90;  // straight down
    float dir[3];
    d::ArrowDir(s, dir);
    CHECK(Near(dir[1], -1, 1e-3f) && Near(dir[2], 0, 1e-3f));
}

// The folder index (D51): the recorder's names, the end of a file, finished / best / latest / match.
void TestReplayIndex() {
    d::FileInfo f;
    CHECK(d::ParseName("mbase_2026-10-23T21-11-10.274Z_pid204_run0001.jsonl", &f));
    CHECK(strcmp(f.map, "mbase") == 0 && strcmp(f.stamp, "2026-10-23T21-11-10.274Z") == 0 && f.pid == 204 && f.run == 1);
    CHECK(d::ParseName("oil_rig_a_2026-10-23T21-11-10.274Z_pid9_run0012.jsonl", &f) && strcmp(f.map, "oil_rig_a") == 0 && f.run == 12);
    CHECK(!d::ParseName("my route.jsonl", &f) && strcmp(f.name, "my route.jsonl") == 0 && !f.map[0]);
    // The reference file's last 64 KB: its last sample (victim lines follow it) and its end record.
    int len = 0;
    char* text = ReadFile("tests/game/data/mbase_2026-10-23T21-11-10.274Z_pid204_run0001.jsonl", &len);
    CHECK(text != nullptr);
    if (text) {
        const int tail = len < 65536 ? len : 65536;
        d::ParseName("mbase_2026-10-23T21-11-10.274Z_pid204_run0001.jsonl", &f);
        d::ReadTail(text + len - tail, tail, &f);
        CHECK(f.complete && Near(f.duration, 33847));
        free(text);
    }
    // One game (pid 7): mbase twice (restart), then bank (mbase's second run finished), then
    // bank still being written; another game's mbase run with no next run.
    const char* names[5] = {"mbase_2026-10-24T10-00-00.000Z_pid7_run0001.jsonl", "mbase_2026-10-24T10-01-00.000Z_pid7_run0002.jsonl",
                            "bank_2026-10-24T10-02-00.000Z_pid7_run0003.jsonl", "mbase_2026-10-25T09-00-00.000Z_pid8_run0001.jsonl",
                            "Test_A.jsonl"};
    const float dur[5] = {50000, 40000, 30000, 20000, 0};
    const bool complete[5] = {true, true, false, true, true};
    d::FileInfo set[5];
    for (int i = 0; i < 5; ++i) d::ParseName(names[i], &set[i]), set[i].duration = dur[i], set[i].complete = complete[i];
    d::MarkFinished(set, 5);
    CHECK(!set[0].finished && set[1].finished && !set[2].finished && !set[3].finished);
    CHECK(d::Best(set, 5, "mbase") == 1);    // only the finished one, though run 4 is shorter
    CHECK(d::Latest(set, 5, "mbase") == 3);  // newest complete
    CHECK(d::Latest(set, 5, "bank") == -1);  // still being written
    CHECK(d::Match(set, 5, "MBASE") == 0 && d::Match(set, 5, "pid8") == 3 && d::Match(set, 5, "te") == 4 && d::Match(set, 5, "zz") == -1);
}

// Display metadata must reject damaged sidecars instead of overwriting them.
void TestReplayBrowser() {
    namespace b = fdcore::replay::browser;
    b::Metadata meta = {"My = fastest run", true, true}, parsed = {};
    char text[160];
    int n = b::SerializeMetadata(meta, text, sizeof(text));
    CHECK(n > 0 && b::ParseMetadata(text, n, &parsed));
    CHECK(parsed.favorite && strcmp(meta.name, parsed.name) == 0);
    CHECK(!b::ParseMetadata(text, n - 1, &parsed));
    const char duplicate[] = "FDNY_RECORDING_META_V1\nfavorite=0\nname=one\nname=two\n";
    CHECK(!b::ParseMetadata(duplicate, sizeof(duplicate) - 1, &parsed));
    CHECK(!b::DisplayName("bad\nname"));
    CHECK(!b::DisplayName("12345678901234567890123456789012345678901"));
    meta.favorite = false; meta.name[0] = 0;
    n = b::SerializeMetadata(meta, text, sizeof(text));
    CHECK(b::ParseMetadata(text, n, &parsed) && !parsed.favorite && !parsed.name[0]);
    CHECK(!b::SerializeMetadata(meta, text, 10));
    CHECK(b::PageCount(0) == 1 && b::PageCount(6) == 1 && b::PageCount(7) == 2);
    CHECK(b::ClampPage(9, 7) == 1 && b::ClampPage(-1, 7) == 0 && b::ClampPage(2, 0) == 0);
}
