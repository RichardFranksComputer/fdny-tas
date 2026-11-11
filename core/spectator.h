// Spectator camera service: FDNYCore's one flying camera, over a paused game. Two modes:
// - the viewer (Start): modal. It follows target poses its client supplies (orbit, zoom,
//   switch target) or flies free (F), takes the keys (binds and the game get none), and
//   emits playback actions; Escape exits. It knows no file format (the replay feeds it).
// - the free camera (StartFree): not modal. It flies from the game's last view with the
//   mouse always looking; binds stay live and other tools' key/pointer captures are
//   honoured (the checkpoint editor). freecam's fd_pause.
// Free flight in both: W/S along the view, A/D strafe, E/Q up/down (one speed in every
// direction), Shift x4, Ctrl x0.25. The keys are two binds modes (D65): "camera" (flight,
// shared by both) and "viewer" (exclusive: playback, follow, exit), so they are rebindable
// and listed like every other mode's. It ends itself when the pause or the level ends.
#pragma once
#include "binds.h"
#include "camera.h"
namespace fdcore::spectator {
constexpr int kMaxTargets = 8;
struct Target {
    float pos[3], forward[3];
    bool valid;
};
enum class Action { TogglePlay, Exit, SeekBack, SeekForward, Restart, ToggleXray };
using ActionHandler = void (*)(Action action);
// Free-flight settings: units/s, degrees per mouse count, and mouse look without the
// right button held (the free camera; the viewer looks while right-dragging).
struct Flight {
    float speed, sens;
    bool lookAlways;
};
struct State {
    bool active, free, modal;
    int followed, targets;
    float distance, yaw, pitch, pos[3];
    float dt;       // the last frame's real-time step, s
    unsigned keys;  // flight keys held that frame: W 1, S 2, D 4, A 8, E 16, Q 32, Shift 64, Ctrl 128
};
bool Init();       // core startup (after pause, camera, binds)
bool Available();  // ready: pause, camera override and mouse input all verified
bool Prepare();     // preconfigure follow pose, input and hooks without affecting menu camera
void PrimeCamera(); // apply the prepared pose before the selected stage first renders
bool Start();       // the viewer: a loaded level only; preserves a pause that was already active
// The free camera from the game's last view: needs an active pause (its client owns it);
// false without a camera pose yet.
bool StartFree(const Flight& flight);
void SetFlight(const Flight& flight);  // the free camera's settings, live
// The paused free camera as a tool others build on (an editor). Its owner, the module with
// the pause setting, registers how it is entered and left; another module asks here, so
// neither names the other. False when there is no owner or it refuses (not in a level,
// `fd_profile run`, a watched level).
using FreePauseFn = bool (*)(bool on);
void SetFreePauseHost(FreePauseFn host);
bool RequestFreePause(bool on);
void Stop();
bool Active();
bool Modal();  // the viewer is running (not the free camera)
void SetPending(bool pending);
bool Pending();
void SetTargets(const Target* targets, int count);
void SetActionHandler(ActionHandler handler);
void SetFree(bool free);
void Follow(int index);   // re-hook to an available target
void Zoom(float notches); // positive moves closer; wheel or equivalent UI control
void ReadState(State* state);
} // namespace fdcore::spectator

namespace fdcore::spectator::input {
using Key = binds::input::Key;  // the press detector lives with the keys (binds, D65)
} // namespace fdcore::spectator::input
