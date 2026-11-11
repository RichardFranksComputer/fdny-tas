// On-screen confirmations: one short line in the game's own hint text (its font, its
// place on screen), for an action the player took with a key. Binds fire only while the
// console is closed, so a console line alone confirms nothing they can see (D64).
//
// A feature keeps its console line (console::Print) and adds one notify::Show with what a
// player reads at a glance: "Position saved", "Trigger boxes on". `toggle <variable>` does
// it for every variable with a label (cmd::SetLabel), and the profile gate for a refusal.
// Shown for 2 s; the game's next hint replaces it. Not shown while paused (the game's hint
// timer is paused with it) or without a level; the text is always logged ("notify> ...").
#pragma once

namespace fdcore::notify {

bool Init();  // core startup, after textdraw (it uses textdraw's game hints)
void Show(const char* fmt, ...);

} // namespace fdcore::notify
