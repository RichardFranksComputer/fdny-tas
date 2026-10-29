// Tests: tests/game/addr_verified.yaml tests/game/addr_bad_signature.yaml tests/game/addr_unknown_exe.yaml tests/unit/test_addr.cpp
#include "addr.h"

#include <windows.h>

#include "log.h"

namespace fdcore::addr {

#ifdef FDCORE_DEBUG
bool LoadOverride(const char* path);
#endif

namespace {

constexpr int kCount = static_cast<int>(Id::Count);

const Version* g_version = nullptr;
bool g_usable[kCount];
// Entries verified by EarlyCodeAddress before a startup patch changed their bytes.
bool g_earlyVerified[kCount];
uintptr_t g_bases[2];

#ifdef FDCORE_DEBUG
// Debug-only override table (FDNYCore.addroverride.txt next to the .asi), in
// the flat format emitted by `addrgen.py text`. Lets tests simulate an unknown
// exe or a bad signature without touching the real exe. Compiled out of Release.
Version g_override;
char g_overrideStrings[32 * 1024];
#endif

bool StrEq(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

constexpr uint8_t kModuleField = 2;  // struct offset entry: nothing to verify in memory

bool VerifySig(const Entry& e) {
    if (e.module == kModuleField) return true;
    if (!e.sigRva || !e.sigLen) return false;
    if (e.exportName) {
        // Hooked exports are resolved by name; the table's rva must agree.
        FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(g_bases[e.module]), e.exportName);
        if (reinterpret_cast<uintptr_t>(p) != g_bases[e.module] + e.rva) return false;
    }
    uint8_t live[kMaxSig];
    if (!SafeRead(g_bases[e.module] + e.sigRva, live, e.sigLen)) return false;
    // Genesis.dll is loaded dynamically and relocates when its preferred base
    // 0x10000000 is taken (seen live: 0x02390000). The loader then rebases
    // every absolute pointer in its code, so a 4-byte window that differs from
    // the file bytes by exactly the base delta (and held a Genesis address) is
    // a relocated pointer, not a mismatch.
    const uint32_t delta = static_cast<uint32_t>(g_bases[1] - 0x10000000u);
    bool relocOk[kMaxSig] = {};
    if (e.module == 1 && delta)
        for (int j = 0; j + 4 <= e.sigLen; ++j) {
            uint32_t f = 0, l = 0;
            for (int k = 0; k < 4; ++k) f |= uint32_t(e.sig[j + k]) << (8 * k), l |= uint32_t(live[j + k]) << (8 * k);
            if (f >= 0x10000000u && f < 0x10400000u && l - f == delta)
                for (int k = 0; k < 4; ++k) relocOk[j + k] = true;
        }
    for (int i = 0; i < e.sigLen; ++i)
        if (live[i] != e.sig[i] && !relocOk[i]) return false;
    return true;
}

} // namespace

// Same as SafeRead, for data the core changes (teleporter): a fault is a failure.
bool SafeWrite(uintptr_t addr, const void* in, uint32_t size) {
    if (!addr) return false;
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), in, size, &n) && n == size;
}

bool SafeRead(uintptr_t addr, void* out, uint32_t size) {
    // ReadProcessMemory on our own process: the kernel copies and reports a
    // fault as failure. A VirtualQuery check followed by a plain read raced
    // (2026-10-21: AV inside the copy loop on a page another thread -- the D3D
    // wrapper -- changed between the check and the read).
    if (!addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, size, &got) &&
           got == size;
}

void Init(const char* exeSha256, const char* genesisSha256, uintptr_t exeBase,
          uintptr_t genesisBase) {
    g_bases[0] = exeBase;
    g_bases[1] = genesisBase;
    const Version* candidates = kVersions;
    int count = kVersionCount;
#ifdef FDCORE_DEBUG
    char path[MAX_PATH];
    HMODULE self = nullptr;  // by address: the file may be FDNYCore-<version>.asi
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Init), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    for (char* p = path + lstrlenA(path); p > path; --p)
        if (p[-1] == '\\') {
            lstrcpyA(p, "FDNYCore.addroverride.txt");
            break;
        }
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        if (LoadOverride(path)) {
            log::Info("address table: Debug override loaded from %s", path);
            candidates = &g_override;
            count = 1;
        } else {
            log::Error("address table: Debug override %s is malformed; ignored", path);
        }
    }
#endif
    for (int i = 0; i < count && !g_version; ++i)
        if (StrEq(candidates[i].exeSha256, exeSha256) && StrEq(candidates[i].genesisSha256, genesisSha256))
            g_version = &candidates[i];
    if (!g_version) {
        log::Warn("unknown exe/Genesis (exe sha256=%s, Genesis sha256=%s): no address-table "
                  "version matches; all modules disabled",
                  exeSha256, genesisSha256);
        return;
    }
    log::Info("address table: version %s selected", g_version->name);

    int ok = 0, total = 0;
    for (int i = 0; i < kCount; ++i) {
        const Entry& e = g_version->entries[i];
        if (!e.rva) continue;  // absent in this version
        g_usable[i] = g_earlyVerified[i] || VerifySig(e);
        ++total;
        if (g_usable[i]) ++ok;
        else log::Warn("signature mismatch: %s (expected at %s+%08lx); entry disabled", e.name,
                       e.module ? "Genesis.dll" : "exe", static_cast<unsigned long>(e.sigRva));
    }
    if (ok == total) log::Info("signatures: all OK (%d/%d)", ok, total);
    else log::Warn("signatures: %d/%d OK", ok, total);
}

#ifdef FDCORE_UNIT
void ResetForUnitTest() {
    g_version = nullptr;
    for (int i = 0; i < kCount; ++i) g_usable[i] = g_earlyVerified[i] = false;
}
#endif

bool VersionKnown() { return g_version != nullptr; }
const char* VersionName() { return g_version ? g_version->name : "unknown"; }
bool Usable(Id id) { return g_version && g_usable[static_cast<int>(id)]; }

const char* Name(Id id) {
    const Version& v = g_version ? *g_version : kVersions[0];
    return v.entries[static_cast<int>(id)].name;
}

bool Resolve(Id id, uintptr_t* out) {
    if (!Usable(id)) return false;
    const Entry& e = g_version->entries[static_cast<int>(id)];
    uintptr_t a = g_bases[e.module] + e.rva;
    if (e.nchain == 0) {
        *out = a;
        return true;
    }
    if (!e.derefRoot) {
        for (int i = 0; i < e.nchain; ++i) a += e.chain[i];
        *out = a;
        return true;
    }
    uint32_t p = 0;
    if (!SafeRead(a, &p, 4) || !p) return false;
    for (int i = 0; i < e.nchain - 1; ++i)
        if (!SafeRead(p + e.chain[i], &p, 4) || !p) return false;
    *out = p + e.chain[e.nchain - 1];
    return true;
}

uintptr_t ModuleBase(int module) { return module == 0 || module == 1 ? g_bases[module] : 0; }

uint32_t Field(Id id) { return kVersions[0].entries[static_cast<int>(id)].rva; }

uintptr_t EarlyCodeAddress(Id id) {
    uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (int v = 0; v < kVersionCount; ++v) {
        const Entry& e = kVersions[v].entries[static_cast<int>(id)];
        if (e.module != 0 || !e.sigLen) continue;
        uint8_t live[kMaxSig];
        if (!SafeRead(exe + e.sigRva, live, e.sigLen)) continue;
        bool match = true;
        for (int i = 0; i < e.sigLen && match; ++i) match = live[i] == e.sig[i];
        if (match) {
            g_earlyVerified[static_cast<int>(id)] = true;
            return exe + e.rva;
        }
    }
    return 0;
}

uintptr_t ExportAddress(Id id) {
    if (!Usable(id)) return 0;
    const Entry& e = g_version->entries[static_cast<int>(id)];
    return g_bases[e.module] + e.rva;
}

bool WriteU8(Id id, uint8_t v) {
    uintptr_t a;
    SIZE_T n = 0;
    return Resolve(id, &a) && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &v, 1, &n) && n == 1;
}

bool WriteI32(Id id, int32_t v) {
    uintptr_t a;
    SIZE_T n = 0;
    return Resolve(id, &a) && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &v, 4, &n) && n == 4;
}

bool ReadI32(Id id, int32_t* out) {
    uintptr_t a;
    return Resolve(id, &a) && SafeRead(a, out, 4);
}

bool ReadF32(Id id, float* out) {
    uintptr_t a;
    return Resolve(id, &a) && SafeRead(a, out, 4);
}

bool ReadStr(Id id, char* out, int cap) {
    uintptr_t a;
    if (cap <= 0 || !Resolve(id, &a)) return false;
    int i = 0;
    for (; i < cap - 1; ++i) {
        if (!SafeRead(a + i, &out[i], 1)) return false;
        if (!out[i]) return true;
    }
    out[i] = 0;
    return true;
}

#ifdef FDCORE_DEBUG
namespace {

// Minimal tokenizer over the override text (no CRT).
struct Cursor {
    const char* p;
    const char* end;
    bool Line() {  // skip to the next non-empty, non-comment line; false at EOF
        while (p < end) {
            while (p < end && (*p == '\r' || *p == '\n' || *p == ' ')) ++p;
            if (p < end && *p == '#') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            return p < end;
        }
        return false;
    }
    bool Token(const char** s, int* n) {
        while (p < end && *p == ' ') ++p;
        if (p >= end || *p == '\r' || *p == '\n') return false;
        *s = p;
        while (p < end && *p != ' ' && *p != '\r' && *p != '\n') ++p;
        *n = static_cast<int>(p - *s);
        return true;
    }
};

uint32_t ParseNum(const char* s, int n) {
    uint32_t v = 0;
    int base = 10;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2, n -= 2, base = 16;
    for (int i = 0; i < n; ++i) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
        v = v * base + d;
    }
    return v;
}

int HexNib(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool TokEq(const char* s, int n, const char* lit) {
    int i = 0;
    for (; i < n && lit[i]; ++i)
        if (s[i] != lit[i]) return false;
    return i == n && !lit[i];
}

} // namespace

bool LoadOverride(const char* path) {
    static char buf[64 * 1024];
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);

    char* strs = g_overrideStrings;
    char* strsEnd = g_overrideStrings + sizeof(g_overrideStrings);
    auto keep = [&](const char* s, int n) -> const char* {
        if (strs + n + 1 > strsEnd) return "";
        char* d = strs;
        for (int i = 0; i < n; ++i) d[i] = s[i];
        d[n] = 0;
        strs += n + 1;
        return d;
    };

    Cursor c{buf, buf + got};
    bool haveVersion = false;
    while (c.Line()) {
        const char* t;
        int n;
        if (!c.Token(&t, &n)) continue;
        if (TokEq(t, n, "version")) {
            if (haveVersion) break;  // first version only
            const char *a, *b, *d;
            int na, nb, nd;
            if (!c.Token(&a, &na) || !c.Token(&b, &nb) || !c.Token(&d, &nd)) return false;
            g_override.name = keep(a, na);
            g_override.exeSha256 = keep(b, nb);
            g_override.genesisSha256 = keep(d, nd);
            haveVersion = true;
        } else if (TokEq(t, n, "entry") && haveVersion) {
            const char* nm;
            int nn;
            if (!c.Token(&nm, &nn)) return false;
            int idx = -1;
            for (int i = 0; i < kCount; ++i)
                if (TokEq(nm, nn, kVersions[0].entries[i].name)) idx = i;
            if (idx < 0) continue;  // unknown name: ignore
            Entry& e = g_override.entries[idx];
            e.name = kVersions[0].entries[idx].name;
            const char* f[7];
            int fn[7];
            for (int i = 0; i < 7; ++i)
                if (!c.Token(&f[i], &fn[i])) return false;
            e.module = static_cast<uint8_t>(ParseNum(f[0], fn[0]));
            e.type = static_cast<uint8_t>(ParseNum(f[1], fn[1]));
            e.derefRoot = static_cast<uint8_t>(ParseNum(f[2], fn[2]));
            e.rva = ParseNum(f[3], fn[3]);
            e.sigRva = ParseNum(f[4], fn[4]);
            e.sigLen = 0;
            if (!(fn[5] == 1 && f[5][0] == '-'))
                for (int i = 0; i + 1 < fn[5] && e.sigLen < kMaxSig; i += 2)
                    e.sig[e.sigLen++] = static_cast<uint8_t>(HexNib(f[5][i]) << 4 | HexNib(f[5][i + 1]));
            e.exportName = (fn[6] == 1 && f[6][0] == '-') ? nullptr : keep(f[6], fn[6]);
            e.nchain = 0;
            const char* ch;
            int cn;
            while (e.nchain < kMaxChain && c.Token(&ch, &cn)) e.chain[e.nchain++] = ParseNum(ch, cn);
        }
    }
    return haveVersion;
}
#endif

} // namespace fdcore::addr
