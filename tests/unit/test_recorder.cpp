// recorder_json: what the recorder writes, read back. Numbers keep every float bit; a file
// built from its lines is one the replay parses to the same values (the in-game recorder
// test checks that files are written, this checks what is in them).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../features/recorder/recorder_json.h"
#include "../../features/replay/replay_data.h"
#include "unit.h"

namespace rec = fdcore::recorder;
namespace d = fdcore::replay::data;

namespace {
uint32_t Bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

bool RoundTrips(float f) {
    rec::Json j;
    j.size = 0, j.ok = true, j.text[0] = 0;
    j.Number(f);
    if (!j.ok) return false;
    return Bits(strtof(j.text, nullptr)) == Bits(f);
}

void Append(char* file, int cap, rec::Json& j) {
    j.Raw("}\n");
    if (j.ok && strlen(file) + j.size < static_cast<size_t>(cap)) strcat(file, j.text);
}
} // namespace

void TestRecorderJson() {
    const float values[] = {1.0f, -1.0f, 0.1f, 1551.00696f, -3397.72583f, 3.40282347e38f, 1.17549435e-38f,
                            1.4e-45f /* denormal */, 123456789.0f, 9.99999999e-5f, 0.5f, 85.0f};
    for (float v : values) CHECK(RoundTrips(v));
    for (int i = 0; i < 2000; ++i) {  // a spread of magnitudes and mantissas
        const float v = ldexpf(1.0f + static_cast<float>(i) / 2000.0f, i % 120 - 60) * (i & 1 ? -1.0f : 1.0f);
        if (!RoundTrips(v)) {
            CHECK(RoundTrips(v));
            break;
        }
    }
    rec::Json j;
    j.Begin();
    j.Key("z");
    j.Number(-0.0f);
    CHECK(strcmp(j.text, "{\"z\":-0.0") == 0);
    j.Begin();
    j.Number(NAN);  // never written
    CHECK(!j.ok);
    j.Begin();
    j.Text("s", "a\"b\\c\n\xE9");
    CHECK(strcmp(j.text, "{\"s\":\"a\\\"b\\\\c\\u000a\\u00e9\"") == 0);
    j.Begin();
    for (int i = 0; i < 300 && j.ok; ++i) j.Text("k", "0123456789");
    CHECK(!j.ok);  // oversized: refused, not cut off

    // A recording made of the recorder's own lines, read by the replay.
    static char file[16384];
    file[0] = 0;
    j.Begin(), j.Text("type", "start"), j.Text("schema", "fdny-trajectory-v1"), j.Text("stage", "bank"), j.UInt("run", 1);
    Append(file, sizeof(file), j);
    const float pos[3][3] = {{144.25f, -47.0f, -1000.125f}, {150.5f, -47.0f, -1010.0f}, {0.1f, 1e-3f, -3397.72583f}};
    const float fwd[3] = {0.0f, 0.0f, 1.0f}, lo[3] = {-30, -30, -30}, hi[3] = {30, 85, 30};
    for (int i = 0; i < 3; ++i) {
        j.Begin(), j.Text("type", "sample"), j.UInt("n", i), j.UInt("t_ms", 16 * (i + 1));
        j.Key("game_dt_s"), j.Number(0.0166f), j.Vec("position", pos[i]), j.Vec("body_forward", fwd);
        j.Key("pitch_deg"), j.Number(-12.7999992f), j.UInt("player_state_raw", 3), j.UInt("ground_contact_raw", 1);
        j.Vec("bounds_min", lo), j.Vec("bounds_max", hi);
        Append(file, sizeof(file), j);
    }
    j.Begin(), j.Text("type", "end"), j.Text("reason", "next_player_initialization"), j.UInt("samples", 3);
    Append(file, sizeof(file), j);
    d::Recording r;
    char err[128];
    CHECK(d::Parse(file, static_cast<int>(strlen(file)), &r, err, sizeof(err)));
    CHECK(strcmp(r.stage, "bank") == 0 && r.n == 3 && r.duration == 48);
    bool same = r.n == 3;
    for (int i = 0; same && i < 3; ++i)
        for (int k = 0; k < 3; ++k) same = same && Bits(r.s[i].pos[k]) == Bits(pos[i][k]);
    CHECK(same);
    CHECK(Bits(r.s[1].pitch) == Bits(-12.7999992f) && r.s[2].maxs[1] == 85 && r.s[0].ground == 1);
    d::Free(&r);
    fdcore::replay::data::FileInfo f;
    d::ParseName("bank_2026-10-28T10-00-00.000Z_pid1_run0001.jsonl", &f);
    d::ReadTail(file, static_cast<int>(strlen(file)), &f);
    CHECK(f.complete && f.duration == 48);
}
