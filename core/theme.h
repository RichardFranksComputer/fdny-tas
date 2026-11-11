// One palette for everything FDNYCore draws (D66): the game's own colours where it has one
// (read from its HUD and menu code, docs/evidence/theme), so a panel, a box in the level and
// a menu row look like parts of the same game. A feature takes its colours from here; a
// colour that carries data (a replay's time fade, the warp HUD's source types) is the
// feature's own.
#pragma once
#include <stdint.h>

namespace fdcore::theme {

constexpr uint32_t kPanel = 0x000000;   // a panel's backing: black, like the game's HUD bar
constexpr uint32_t kText = 0xFFFFFF;    // the hint and a focused menu row: white
constexpr uint32_t kSoft = 0xBFBFBF;    // an unfocused menu row: grey (0.75)
constexpr uint32_t kDim = 0x808080;     // a menu row's second label: grey (0.5)
constexpr uint32_t kAccent = 0xFFBF00;  // the menu's highlight: orange (1, 0.75, 0)
constexpr uint32_t kGood = 0x40FF40;    // the HUD status line: green (0.25, 1, 0.25)
constexpr uint32_t kBad = 0xFF3030;     // the mission timer's digits: red
constexpr uint32_t kInfo = 0x4080FF;    // ours (the game has no blue): a neutral zone

} // namespace fdcore::theme
