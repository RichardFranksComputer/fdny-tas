#include "state.h"

#include <windows.h>

#include "addr.h"
#include "hooks.h"
#include "log.h"

namespace fdcore::state {
namespace {

using addr::Id;

constexpr int32_t kKindVictim = 2;  // PlayerRescueEnt::EndMission 0x0044FBA0
constexpr int32_t kKindFire = 6;    // FireRescueEnt (vptr 0x004E1780), verified live
constexpr int32_t kFireBurning = 4, kFireOut = 5;

State g_cur;
CRITICAL_SECTION g_lock;  // the pipe thread reads while the game thread refreshes

float Sqrt(float x) {
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

// One-hop reads relative to an entity / node (struct-field entries).
template <typename T>
bool At(uintptr_t base, Id field, T* out) {
    return addr::SafeRead(base + addr::Field(field), out, sizeof(T));
}

void Refresh() {
    State s{};
    s.frame = g_cur.frame + 1;
    s.gameState = -1;
    addr::ReadI32(Id::game_state, &s.gameState);
    if (!addr::ReadStr(Id::game_map_name, s.map, sizeof(s.map))) s.map[0] = 0;

    s.menuScreen = -1;
    uintptr_t a;
    uint32_t cur = 0, list = 0;
    int32_t count = 0;
    if (addr::Resolve(Id::menu_screen_current, &a) && addr::SafeRead(a, &cur, 4) &&
        addr::Resolve(Id::menu_screen_list, &a) && addr::SafeRead(a, &list, 4) &&
        addr::ReadI32(Id::menu_screen_count, &count) && list && count > 0 && count < 32)
        for (int i = 0; i < count; ++i) {
            uint32_t p = 0;
            if (addr::SafeRead(list + 4u * i, &p, 4) && p == cur) {
                s.menuScreen = i;
                break;
            }
        }

    s.playerValid = addr::ReadF32(Id::player_pos_x, &s.pos[0]) && addr::ReadF32(Id::player_pos_y, &s.pos[1]) &&
                    addr::ReadF32(Id::player_pos_z, &s.pos[2]);
    if (s.playerValid) {
        addr::ReadF32(Id::player_vel_x, &s.vel[0]);
        addr::ReadF32(Id::player_vel_y, &s.vel[1]);
        addr::ReadF32(Id::player_vel_z, &s.vel[2]);
        s.speedXY = Sqrt(s.vel[0] * s.vel[0] + s.vel[2] * s.vel[2]);
        addr::ReadF32(Id::player_pitch, &s.pitch);
        addr::ReadF32(Id::player_yaw, &s.yaw);
        addr::ReadI32(Id::player_state, &s.moveState);
        uintptr_t slot = 0;
        uint32_t pl = 0;
        if (addr::Resolve(Id::player_object, &slot) && addr::SafeRead(slot, &pl, 4) && pl)
            addr::SafeRead(pl + addr::Field(Id::player_f_maxs) + 4, &s.boxTop, 4);  // maxs.y
        int32_t ground = 0;
        addr::ReadI32(Id::player_ground, &ground);
        s.onGround = (ground & 0xFF) != 0;
        addr::ReadF32(Id::mission_time_left, &s.timeLeft);
        addr::ReadI32(Id::victims_total, &s.victimsTotal);
        addr::ReadI32(Id::victims_rescued, &s.victimsRescued);
        uint8_t bot = 0;
        if (addr::Resolve(Id::rescuebot_enabled, &a) && addr::SafeRead(a, &bot, 1)) s.botEnabled = bot;
        addr::ReadI32(Id::rescuebot_claimed, &s.botClaimed);
    }

    // Walk the level entity list once: victims and fires. Only in a level:
    // at the menus the list head is stale (garbage pointers), and walking
    // junk every menu frame was both a fault risk and a frame-time cost.
    uint32_t node = 0;
    if (s.gameState == 1 && addr::Resolve(Id::entities_head, &a) && addr::SafeRead(a, &node, 4))
        for (int guard = 0; node && guard < 4096; ++guard) {
            uint32_t ent = 0, next = 0;
            int32_t kind = -1;
            if (!At(node, Id::node_elem, &ent) || !At(node, Id::node_next, &next)) break;
            if (ent && At(ent, Id::entity_kind, &kind)) {
                if (kind == kKindVictim && s.victimCount < kMaxVictims) {
                    Victim& v = s.victims[s.victimCount++];
                    At(ent, Id::victim_pos, &v.pos);
                    At(ent, Id::victim_state, &v.state);
                } else if (kind == kKindFire) {
                    int32_t mode = 0;
                    At(ent, Id::fire_mode, &mode);
                    ++s.fires;
                    if (mode == kFireBurning) ++s.firesBurning;
                    else if (mode == kFireOut) ++s.firesOut;
                }
            }
            node = next;
        }
    EnterCriticalSection(&g_lock);
    g_cur = s;
    LeaveCriticalSection(&g_lock);
}

const Id kNeeds[] = {Id::game_state, Id::game_map_name, Id::player_pos_x, Id::player_vel_x, Id::player_yaw,
                     Id::mission_time_left, Id::entities_head, Id::rescuebot_enabled, Id::menu_screen_current};

// "%d.%03d" (wsprintf has no %f)
int F(char* out, float v) {
    long m = static_cast<long>(v * 1000.0f + (v >= 0 ? 0.5f : -0.5f));
    long a = m < 0 ? -m : m;
    return wsprintfA(out, "%s%ld.%03ld", m < 0 ? "-" : "", a / 1000, a % 1000);
}

} // namespace

bool Init() {
    for (Id id : kNeeds)
        if (!addr::Usable(id)) {
            log::Warn("state: disabled (needs %s)", addr::Name(id));
            return false;
        }
    InitializeCriticalSection(&g_lock);
    hooks::OnFrameEnd(Refresh);
    log::Info("state: snapshot ready (refreshed at frame end)");
    return true;
}

const State& Current() { return g_cur; }

namespace {
volatile LONG g_watchOnly = 0;  // read by the pipe thread too
}
void SetWatchOnly(bool on, const char* why) {
    if ((InterlockedExchange(&g_watchOnly, on ? 1 : 0) != 0) != on)
        log::Info("state: watch-only %s (%s)", on ? "on" : "off", why);
}
bool WatchOnly() { return g_watchOnly != 0; }

int Format(char* out, int cap) {
    EnterCriticalSection(&g_lock);
    const State s = g_cur;
    LeaveCriticalSection(&g_lock);
    if (cap < 1024) return 0;
    int o = wsprintfA(out, "frame=%lu game_state=%ld map=%s menu=%ld player=%d", static_cast<unsigned long>(s.frame),
                      static_cast<long>(s.gameState), s.map[0] ? s.map : "-", static_cast<long>(s.menuScreen),
                      s.playerValid ? 1 : 0);
    const char* names[] = {"pos_x", "pos_y", "pos_z", "vel_x", "vel_y", "vel_z", "speed_xy", "pitch", "yaw", "time_left"};
    const float vals[] = {s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2], s.speedXY, s.pitch, s.yaw, s.timeLeft};
    for (int i = 0; i < 10; ++i) {
        o += wsprintfA(out + o, " %s=", names[i]);
        o += F(out + o, vals[i]);
    }
    o += wsprintfA(out + o, " move_state=%ld ground=%d victims_total=%ld victims_rescued=%ld victims=%ld",
                   static_cast<long>(s.moveState), s.onGround ? 1 : 0, static_cast<long>(s.victimsTotal),
                   static_cast<long>(s.victimsRescued), static_cast<long>(s.victimCount));
    for (int i = 0; i < s.victimCount && o < cap - 200; ++i) {
        o += wsprintfA(out + o, " v%d=", i);
        o += F(out + o, s.victims[i].pos[0]);
        out[o++] = ',';
        o += F(out + o, s.victims[i].pos[1]);
        out[o++] = ',';
        o += F(out + o, s.victims[i].pos[2]);
        o += wsprintfA(out + o, ",%ld", static_cast<long>(s.victims[i].state));
    }
    o += wsprintfA(out + o, " fires=%ld fires_burning=%ld fires_out=%ld bot_enabled=%ld bot_claimed=%ld watch_only=%d",
                   static_cast<long>(s.fires), static_cast<long>(s.firesBurning), static_cast<long>(s.firesOut),
                   static_cast<long>(s.botEnabled), static_cast<long>(s.botClaimed), WatchOnly() ? 1 : 0);
    return o;
}

} // namespace fdcore::state
