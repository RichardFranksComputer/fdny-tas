// fdcore_unit.exe (D47): core code built for the host, run without the game.
//   fdcore_unit.exe                         the C++ unit tests below; exit 1 on a failure
//   fdcore_unit.exe glitch-replay <rec> <mapsdir> <log out> <box out>
//       the warp HUD's replay driver (glitch_replay.cpp, the one the game's Debug pipe
//       runs) over a .glitchrec; core/unit/test_glitch_parity.py compares the outputs
//       with the original Python overlay.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/features/glitch_replay.h"
#include "../src/log.h"
#include "unit.h"

namespace unit {
int g_checks = 0, g_failures = 0;
const char* g_test = "";
} // namespace unit

void TestCheckpoints();
void TestAddrAllVerify();
void TestAddrGenesisAtPreferredBase();
void TestAddrUnknownVersion();
void TestAddrBadSignature();
void TestAddrPatchSiteOccupied();
void TestAddrResolveChain();
void TestAddrFieldOffsets();
void TestCrashlogReportsFault();
void TestReplayParsesSample();
void TestReplayRejectsBadFiles();
void TestReplayColors();
void TestReplaySimplify();
void TestReplayAirFlags();
void TestReplayIndex();

namespace {

struct Test {
    const char* name;
    void (*fn)();
};
const Test kTests[] = {
    {"checkpoints", TestCheckpoints},
    {"addr_all_verify", TestAddrAllVerify},
    {"addr_genesis_at_preferred_base", TestAddrGenesisAtPreferredBase},
    {"addr_unknown_version", TestAddrUnknownVersion},
    {"addr_bad_signature", TestAddrBadSignature},
    {"addr_patch_site_occupied", TestAddrPatchSiteOccupied},
    {"addr_resolve_chain", TestAddrResolveChain},
    {"addr_field_offsets", TestAddrFieldOffsets},
    {"replay_parses_sample", TestReplayParsesSample},
    {"replay_rejects_bad_files", TestReplayRejectsBadFiles},
    {"replay_colors", TestReplayColors},
    {"replay_simplify", TestReplaySimplify},
    {"replay_air_flags", TestReplayAirFlags},
    {"replay_index", TestReplayIndex},
    {"crashlog_reports_fault", TestCrashlogReportsFault},  // last: it leaves the handler installed
};

FILE* g_box = nullptr;
void BoxOut(const char* s) { fputs(s, g_box); }

int GlitchReplay(const char* rec, const char* maps, const char* logOut, const char* boxOut) {
    FILE* f = fopen(rec, "rb");
    if (!f) return printf("cannot open %s\n", rec), 2;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    auto* data = static_cast<uint32_t*>(calloc(1, size + fdcore::glitch::replay::kRecSize));
    const size_t got = fread(data, 1, size, f);
    fclose(f);
    g_box = fopen(boxOut, "wb");
    if (!fdcore::log::Open(logOut) || !g_box) return printf("cannot write %s / %s\n", logOut, boxOut), 2;
    const auto r = fdcore::glitch::replay::Run(data, static_cast<uint32_t>(got / fdcore::glitch::replay::kRecSize),
                                               maps, BoxOut);
    fdcore::log::Close();
    fclose(g_box);
    free(data);
    printf("ok sessions=%d frames=%d\n", r.sessions, r.frames);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 6 && strcmp(argv[1], "glitch-replay") == 0) return GlitchReplay(argv[2], argv[3], argv[4], argv[5]);
    if (argc != 1) return printf("usage: fdcore_unit.exe | fdcore_unit.exe glitch-replay <rec> <maps> <log> <box>\n"), 2;
    for (const Test& t : kTests) {
        unit::g_test = t.name;
        const int before = unit::g_failures;
        t.fn();
        printf("%s %s\n", unit::g_failures == before ? "PASS" : "FAIL", t.name);
    }
    printf("%d checks, %d failed\n", unit::g_checks, unit::g_failures);
    return unit::g_failures ? 1 : 0;
}
