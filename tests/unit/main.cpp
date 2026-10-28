// fdcore_unit.exe (D47): core code built for the host, run without the game.
//   fdcore_unit.exe                         the C++ unit tests below; exit 1 on a failure
#include <stdio.h>

#include "unit.h"

namespace unit {
int g_checks = 0, g_failures = 0;
const char* g_test = "";
} // namespace unit

void TestCheckpoints();
void TestSpectatorCamera();
void TestAddrAllVerify();
void TestAddrGenesisAtPreferredBase();
void TestAddrUnknownVersion();
void TestAddrBadSignature();
void TestAddrPatchSiteDetoured();
void TestAddrResolveChain();
void TestAddrFieldOffsets();
void TestCrashlogReportsFault();
void TestReplayParsesSample();
void TestReplayRejectsBadFiles();
void TestReplayColors();
void TestReplaySimplify();
void TestReplayAirFlags();
void TestReplayIndex();
void TestReplayBrowser();

namespace {

struct Test {
    const char* name;
    void (*fn)();
};
const Test kTests[] = {
    {"checkpoints", TestCheckpoints},
    {"spectator_camera", TestSpectatorCamera},
    {"addr_all_verify", TestAddrAllVerify},
    {"addr_genesis_at_preferred_base", TestAddrGenesisAtPreferredBase},
    {"addr_unknown_version", TestAddrUnknownVersion},
    {"addr_bad_signature", TestAddrBadSignature},
    {"addr_patch_site_detoured", TestAddrPatchSiteDetoured},
    {"addr_resolve_chain", TestAddrResolveChain},
    {"addr_field_offsets", TestAddrFieldOffsets},
    {"replay_parses_sample", TestReplayParsesSample},
    {"replay_rejects_bad_files", TestReplayRejectsBadFiles},
    {"replay_colors", TestReplayColors},
    {"replay_simplify", TestReplaySimplify},
    {"replay_air_flags", TestReplayAirFlags},
    {"replay_index", TestReplayIndex},
    {"replay_browser", TestReplayBrowser},
    {"crashlog_reports_fault", TestCrashlogReportsFault},  // last: it leaves the handler installed
};

} // namespace

int main() {
    for (const Test& t : kTests) {
        unit::g_test = t.name;
        const int before = unit::g_failures;
        t.fn();
        printf("%s %s\n", unit::g_failures == before ? "PASS" : "FAIL", t.name);
    }
    printf("%d checks, %d failed\n", unit::g_checks, unit::g_failures);
    return unit::g_failures ? 1 : 0;
}
