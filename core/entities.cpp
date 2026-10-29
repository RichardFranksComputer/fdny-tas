// Tests: tests/game/speedrun_static_spawns.yaml tests/game/recorder.yaml tests/game/triggers.yaml
#include "entities.h"

#include <windows.h>

#include "addr.h"
#include "hooks.h"
#include "log.h"

namespace fdcore::entities {
namespace {
using Method = uintptr_t(__fastcall*)(void*, void*);
using Destroy = uintptr_t(__fastcall*)(void*, void*, unsigned);
struct Listener { EntityFn init, update, destroy; };
constexpr int kMaxListeners = 8;
Listener g_players[kMaxListeners], g_victims[kMaxListeners];
int g_nPlayers, g_nVictims;
bool g_playerReady, g_victimReady, g_playerTried, g_victimTried;
Method g_playerInit, g_playerUpdate, g_victimSpawn, g_victimUpdate;
Destroy g_victimDestroy;

void Notify(Listener* listeners, int count, int event, void* entity) {
    // Observers may serialize floats or call Win32. Preserve the state left by
    // the game callback, including x87 registers/control and SSE/MXCSR.
    unsigned char fp[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave %0" : "=m"(fp));
    DWORD error = GetLastError();
    for (int i = 0; i < count; ++i) {
        EntityFn fn = event == 0 ? listeners[i].init : event == 1 ? listeners[i].update : listeners[i].destroy;
        if (fn) fn(entity);
    }
    SetLastError(error);
    __asm__ volatile("fxrstor %0" : : "m"(fp));
}

uintptr_t __fastcall PlayerInit(void* p, void* edx) {
    uintptr_t result = g_playerInit(p, edx);
    Notify(g_players, g_nPlayers, 0, p);
    return result;
}
uintptr_t __fastcall PlayerUpdate(void* p, void* edx) {
    uintptr_t result = g_playerUpdate(p, edx);
    Notify(g_players, g_nPlayers, 1, p);
    return result;
}
uintptr_t __fastcall VictimSpawn(void* p, void* edx) {
    uintptr_t result = g_victimSpawn(p, edx);
    Notify(g_victims, g_nVictims, 0, p);
    return result;
}
uintptr_t __fastcall VictimUpdate(void* p, void* edx) {
    uintptr_t result = g_victimUpdate(p, edx);
    Notify(g_victims, g_nVictims, 1, p);
    return result;
}
uintptr_t __fastcall VictimDestroy(void* p, void* edx, unsigned flags) {
    Notify(g_victims, g_nVictims, 2, p);
    return g_victimDestroy(p, edx, flags);
}

// Spot filter: SpawnAtSpot has copied the spot to the victim and, at
// patch_victim_spawn, does `mov ecx,ebp; call SnapToFloor` (EBP = victim). Those
// 7 bytes become a jmp to a trampoline that calls the filter, then runs them.
EntityFn g_spotFilter = nullptr;
uint8_t* g_spotTramp = nullptr;

extern "C" void __cdecl FdcoreVictimSpot(void* victim) { g_spotFilter(victim); }
} // namespace

bool FilterVictimSpot(EntityFn filter) {
    if (g_spotFilter || !filter) return false;
    uintptr_t site = addr::EarlyCodeAddress(addr::Id::patch_victim_spawn);
    uintptr_t snap = addr::EarlyCodeAddress(addr::Id::game_snap_to_floor);
    if (!site || !snap) {
        log::Warn("entities: victim-spawn site doesn't match (signature mismatch, see above); "
                  "no spot filter");
        return false;
    }
    g_spotTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_spotTramp) return false;
    g_spotFilter = filter;  // before the patch makes the trampoline reachable
    uint8_t* t = g_spotTramp;
    int k = 0;
    auto put4 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) t[k++] = static_cast<uint8_t>(v >> (8 * i));
    };
    t[k++] = 0x9C;  // pushfd
    t[k++] = 0x60;  // pushad
    t[k++] = 0x55;  // push ebp (victim this)
    t[k++] = 0xE8;  // call FdcoreVictimSpot
    put4(reinterpret_cast<uintptr_t>(&FdcoreVictimSpot) - (reinterpret_cast<uintptr_t>(t) + k + 4));
    t[k++] = 0x83, t[k++] = 0xC4, t[k++] = 0x04;  // add esp,4 (cdecl)
    t[k++] = 0x61;                                // popad
    t[k++] = 0x9D;                                // popfd
    t[k++] = 0x8B, t[k++] = 0xCD;                 // mov ecx,ebp   (displaced)
    t[k++] = 0xE8;                                // call SnapToFloor (displaced)
    put4(snap - (reinterpret_cast<uintptr_t>(t) + k + 4));
    t[k++] = 0xE9;  // jmp back after the 7 displaced bytes
    put4(site + 7 - (reinterpret_cast<uintptr_t>(t) + k + 4));

    uint8_t patch[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_spotTramp) - (site + 5));
    for (int i = 0; i < 4; ++i) patch[1 + i] = static_cast<uint8_t>(rel >> (8 * i));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), 7, PAGE_EXECUTE_READWRITE, &old)) {
        g_spotFilter = nullptr;
        return false;
    }
    for (int i = 0; i < 7; ++i) reinterpret_cast<volatile uint8_t*>(site)[i] = patch[i];
    VirtualProtect(reinterpret_cast<void*>(site), 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 7);
    log::Info("entities: victim-spot filter at %08lx -> trampoline %08lx", static_cast<unsigned long>(site),
              reinterpret_cast<unsigned long>(g_spotTramp));
    return true;
}

bool ObservePlayer(EntityFn initialized, EntityFn updated) {
    if (g_nPlayers == kMaxListeners) return false;
    if (!g_playerTried) {
        g_playerTried = true;
        g_playerReady = addr::Usable(addr::Id::player_initialize) && addr::Usable(addr::Id::player_root_update) &&
            hooks::Create("player_observers", addr::Id::player_initialize,
            reinterpret_cast<void*>(PlayerInit), reinterpret_cast<void**>(&g_playerInit)) &&
            hooks::Create("player_observers", addr::Id::player_root_update,
            reinterpret_cast<void*>(PlayerUpdate), reinterpret_cast<void**>(&g_playerUpdate));
        // Publish the subscription before enabling the hooks.
    }
    if (!g_playerReady) return false;
    g_players[g_nPlayers++] = {initialized, updated, nullptr};
    if (!hooks::SetEnabled("player_observers", true)) { --g_nPlayers; return false; }
    return true;
}

bool ObserveVictims(EntityFn spawned, EntityFn updated, EntityFn destroying) {
    if (g_nVictims == kMaxListeners) return false;
    if (!g_victimTried) {
        g_victimTried = true;
        g_victimReady = addr::Usable(addr::Id::victim_spawn) && addr::Usable(addr::Id::victim_root_update) &&
            addr::Usable(addr::Id::victim_destroy) &&
            hooks::Create("victim_observers", addr::Id::victim_spawn,
            reinterpret_cast<void*>(VictimSpawn), reinterpret_cast<void**>(&g_victimSpawn)) &&
            hooks::Create("victim_observers", addr::Id::victim_root_update,
            reinterpret_cast<void*>(VictimUpdate), reinterpret_cast<void**>(&g_victimUpdate)) &&
            hooks::Create("victim_observers", addr::Id::victim_destroy,
            reinterpret_cast<void*>(VictimDestroy), reinterpret_cast<void**>(&g_victimDestroy));
    }
    if (!g_victimReady) return false;
    g_victims[g_nVictims++] = {spawned, updated, destroying};
    if (!hooks::SetEnabled("victim_observers", true)) { --g_nVictims; return false; }
    return true;
}
namespace {
struct NativeDefinition {
    uint32_t reserved;
    const char* name;
    void* model;
    float reach;
    const char* scripts[10];
    float position[3];
};
static_assert(sizeof(NativeDefinition) == 0x44, "retail trigger definition");
static_assert(__builtin_offsetof(NativeDefinition, position) == 0x38, "retail trigger position");
struct NativeRequest { NativeDefinition* data; uint32_t unused; int message; void* descriptor; };
struct NativeStorage { NativeDefinition definition; char name[64]; uintptr_t actor; };
using FireEventFn = uintptr_t(__thiscall*)(void*, uint32_t);
FireEventFn g_nativeFireOriginal;
NativeContactFn g_nativeContact;
bool g_nativeHook;
NativeStorage g_native[32];
int g_nativeCount;
NativeDefinition g_donor;
uintptr_t g_descriptor;
bool NativeText(uintptr_t p, char* text, unsigned cap) {
    for (unsigned i = 0; i < cap; ++i) {
        if (!addr::SafeRead(p+i, text+i, 1)) return false;
        if (!text[i]) return true;
    }
    return false;
}
bool HasHint(const char* text) {
    for (; *text; ++text) {
        const char* match = "hint"; unsigned i = 0;
        while (match[i] && text[i] == match[i]) ++i;
        if (!match[i]) return true;
    }
    return false;
}
bool FindNativeDonor() {
    uintptr_t head = 0, type = 0; uint32_t node = 0;
    if (!addr::Resolve(addr::Id::entities_head, &head) || !addr::SafeRead(head, &node, 4) ||
        !addr::Resolve(addr::Id::trigger_vtbl, &type)) return false;
    bool found = false;
    for (int guard = 0; node && guard < 4096; ++guard) {
        uint32_t root = 0, next = 0, vtbl = 0, data = 0, descriptor = 0; uint8_t escape = 0;
        if (!addr::SafeRead(node+addr::Field(addr::Id::node_elem), &root, 4) ||
            !addr::SafeRead(node+addr::Field(addr::Id::node_next), &next, 4)) return false;
        if (addr::SafeRead(root, &vtbl, 4) && vtbl == type &&
            addr::SafeRead(root+addr::Field(addr::Id::trigger_userdata), &data, 4) && data &&
            addr::SafeRead(root+addr::Field(addr::Id::entity_class), &descriptor, 4) && descriptor &&
            addr::SafeRead(root+addr::Field(addr::Id::trigger_escape), &escape, 1) && !escape) {
            NativeDefinition d{};
            if (addr::SafeRead(data, &d, sizeof(d)) && d.model) {
                bool hint = false;
                for (const char* line : d.scripts) {
                    char text[256];
                    if (line && NativeText(reinterpret_cast<uintptr_t>(line), text, sizeof(text)) && HasHint(text)) hint = true;
                }
                if (!found || hint) { g_donor = d; g_descriptor = descriptor; found = true; }
                if (hint) return true;
            }
        }
        node = next;
    }
    return found;
}
bool NativeTriggersReady() {
    const addr::Id needs[] = {addr::Id::entity_trigger_factory, addr::Id::entity_admit,
        addr::Id::entity_trigger_initialize, addr::Id::game_session, addr::Id::entities_head, addr::Id::trigger_vtbl, addr::Id::script_fire_event};
    for (const auto id : needs) if (!addr::Usable(id)) return false;
    return true;
}
uintptr_t __fastcall NativeFireEvent(void* actor, void*, uint32_t event) {
    int index = -1;
    for (int i = 0; i < g_nativeCount; ++i) if (g_native[i].actor == reinterpret_cast<uintptr_t>(actor)) { index = i; break; }
    bool contact = false, entering = false;
    if (index >= 0) {
        const DWORD error = GetLastError(); char word[8] = {};
        const uintptr_t chars = event+addr::Field(addr::Id::string_chars);
        const bool readable = addr::SafeRead(chars, word, sizeof(word)) || NativeText(chars, word, sizeof(word));
        if (readable && (!word[5] || !word[7])) {
            entering = !word[5] && lstrcmpA(word, "touch") == 0;
            contact = entering || (!word[7] && lstrcmpA(word, "untouch") == 0);
        }
        SetLastError(error);
    }
    // The game releases its by-value event String; copy the event kind first.
    const uintptr_t result = g_nativeFireOriginal(actor, event);
    if (contact && g_nativeContact) {
        unsigned char fp[512] __attribute__((aligned(16)));
        __asm__ volatile("fxsave %0" : "=m"(fp));
        const DWORD error = GetLastError();
        g_nativeContact(index, entering);
        SetLastError(error);
        __asm__ volatile("fxrstor %0" : : "m"(fp));
    }
    return result;
}
}
void ResetNativeTriggers() { g_nativeCount = 0; g_descriptor = 0; }

bool ObserveNativeContacts(NativeContactFn callback) {
    if (g_nativeContact && g_nativeContact != callback) return false;
    g_nativeContact = callback; return true;
}
bool AddNativeTrigger(const char* name, const float* min, const float* max) {
    if (!NativeTriggersReady() || g_nativeCount == 32 || (!g_descriptor && !FindNativeDonor())) return false;
    if (!g_nativeHook) {
        if (!hooks::Create("native_contacts", addr::Id::script_fire_event, reinterpret_cast<void*>(NativeFireEvent), reinterpret_cast<void**>(&g_nativeFireOriginal)) ||
            !hooks::SetEnabled("native_contacts", true)) return false;
        g_nativeHook = true;
    }
    NativeStorage& storage = g_native[g_nativeCount];
    storage.definition = g_donor;
    lstrcpynA(storage.name, name, sizeof(storage.name));
    NativeDefinition& d = storage.definition;
    d.name = storage.name; d.reach = 0;
    for (auto& script : d.scripts) script = nullptr;
    // BoundsTrigger checks ScriptActor's event-table count before collecting
    // contacts (0x440ce0 / 0x440dc0). A binding for an event nothing fires keeps that
    // native path active without running an authored command on touch/untouch.
    d.scripts[0] = "fdcore_native_contact: hint 0 (native trigger)";
    for (int a = 0; a < 3; ++a) d.position[a] = (min[a]+max[a])*.5f;
    NativeRequest request = {&d, 0, 10000, reinterpret_cast<void*>(g_descriptor)};
    uintptr_t slot = 0; uint32_t session = 0;
    if (!addr::Resolve(addr::Id::game_session, &slot) || !addr::SafeRead(slot, &session, 4) || !session) return false;
    using Factory = void*(__thiscall*)(void*, void*);
    using Admit = void(__thiscall*)(void*, void*);
    using Initialize = void(__thiscall*)(void*);
    void* root = reinterpret_cast<Factory>(addr::ExportAddress(addr::Id::entity_trigger_factory))(request.descriptor, &request);
    if (!root) return false;
    storage.actor = reinterpret_cast<uintptr_t>(root)+addr::Field(addr::Id::entity_script_actor);
    ++g_nativeCount; // definition belongs to this root even if a later check fails
    reinterpret_cast<Admit>(addr::ExportAddress(addr::Id::entity_admit))(reinterpret_cast<void*>(session), root);
    reinterpret_cast<Initialize>(addr::ExportAddress(addr::Id::entity_trigger_initialize))(root);
    const uintptr_t box = reinterpret_cast<uintptr_t>(root)+addr::Field(addr::Id::trigger_box);
    return addr::SafeWrite(box, min, 3*sizeof(float)) && addr::SafeWrite(box+3*sizeof(float), max, 3*sizeof(float));
}
} // namespace fdcore::entities
