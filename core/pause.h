// pause (core service, fd_pause; docs/evidence/freecam): freezes the game's simulation while
// rendering goes on. Owns the GameSession::TickSubsystems hook (sim.tick_subsystems):
// while active the call is skipped, so entities, victims, fire, the mission timer and
// sound updates stand still. The game clock is paused with the game's own
// GameClock::Pause / Resume on the active subtimer (as on alt-tab), so elapsed time and
// the render-side particle keyframes stop, and the first frame after resuming has a
// normal delta. Game input is blocked (controls.block_input) and paused frames are
// capped at ~60 fps (the game's own cap lives in the paused clock). Leaving the level
// (load, menu) resumes.
#pragma once

#include <stdint.h>

namespace fdcore::pause {

bool Init();        // after camera/state; hooks are created here, enabled while paused
bool Available();   // every entry it needs verified

// Starts / ends a pause; `why` goes to the log. False if unavailable or not in a level.
bool Set(bool on, const char* why);
bool Active();
// Install the pause hooks during loading, without pausing the game yet.
bool Prepare();
void CancelPrepare();
uint32_t PausedFrames();  // frames rendered during the current (or last) pause

} // namespace fdcore::pause
