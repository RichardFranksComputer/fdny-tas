// Profiles + watermark (backlog item 12).
//
// fd_profile practice (default) / run. In `run`, practice-only variables and
// commands are refused; switching to `run` puts the practice-only variables
// back to their defaults and remembers their practice values, which come back
// on switching to `practice` (that remembered set is persisted in
// FDNYCore.cfg). A switch also runs `profile_<name>.cfg` if it exists, so a
// player can keep their own variable set per profile (the same exec as
// autoexec.cfg).
//
// The watermark is one line drawn in-frame with the engine's own font
// (geEngine_Printf, so recordings show it): core version, profile, every
// FDNYCore variable not at its default, and the other .asi mods loaded. It
// can't be turned off in `run`.
#pragma once

namespace fdcore::profile {

bool Init();  // after every module has registered its variables

// True in `fd_profile practice` (the default). A practice tool that acts on its own (an
// editor, a viewer, a timer) asks here; variables and commands are gated by the profile itself.
bool Practice();

// Debug pipe / tests: the watermark text drawn on the last frame.
int LastWatermark(char* out, int cap);

} // namespace fdcore::profile
