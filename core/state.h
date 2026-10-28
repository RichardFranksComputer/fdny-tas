// Game state snapshot (backlog item 9): one typed copy of the game state,
// refreshed at every frame end on the game thread. Modules read this instead
// of game memory.
#pragma once

#include <stdint.h>

namespace fdcore::state {

constexpr int kMaxVictims = 24;

struct Victim {
    float pos[3];
    int32_t state;  // 4 idle, 5 carried, 6 claimed/delivered, 7 roaming
};

struct State {
    uint32_t frame;          // frame_end events since start
    int32_t gameState;       // 1 in level, 2/3 loading, 0 menu (LiveSplit's gameState)
    char map[16];
    int32_t menuScreen;      // 0 Main .. 9 Splash; -1 unknown
    bool playerValid;
    float pos[3];
    float vel[3];            // units/s
    float speedXY;           // horizontal speed, units/s (sqrt(vx^2 + vz^2); y is up)
    float pitch, yaw;        // degrees
    int32_t moveState;       // 3 standing, 4 crouched, 5 climb/spectator, 6 passed out, 7 jump
    float boxTop;            // collision box maxs y: 85 standing, 25 crouched (crouchbug: not 4, top < 55)
    bool onGround;
    float timeLeft;          // mission time remaining, seconds
    int32_t victimsTotal, victimsRescued;
    int32_t victimCount;     // listed below (entities of kind 2)
    Victim victims[kMaxVictims];
    int32_t fires, firesBurning, firesOut;  // FireRescueEnt (kind 6): total, mode 4, mode 5
    int32_t botEnabled, botClaimed;
};

bool Init();  // needs the frame hooks; registers the frame_end refresh
const State& Current();

// The level is loaded (or loading) to be watched, not played: a recording viewer, or any
// later playback mode. What acts on play stands down while it is set: the recorder records
// nothing, editors and practice overlays stay off, fd_pause is refused. Its owner sets it
// before the stage loads and clears it when it leaves (`why` goes to the log).
void SetWatchOnly(bool on, const char* why);
bool WatchOnly();

// Debug pipe / tests: the snapshot as one "key=value ..." line.
int Format(char* out, int cap);

} // namespace fdcore::state
