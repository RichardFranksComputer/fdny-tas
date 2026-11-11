// speedrun module (backlog item 8): static victim spawns + RescueBot off
// (port of asi/FDNYDeterministic).
#pragma once

namespace fdcore::speedrun {

// DllMain (before WinMain): install the victim-spawn trampoline.
bool ApplyEarlyPatch();

// After the address table and console are up: fd_spawns, fd_spawns_pin, fd_rescuebot.
bool Init();

} // namespace fdcore::speedrun
