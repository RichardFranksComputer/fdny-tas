// The settings page (D67): every FDNYCore variable that has a label, as a row of the game's
// own Options widgets, on a page opened from the game's Options screen. Built from the
// registry: a 0/1 variable is an Off / On row, a variable with named choices
// (cmd::SetOptions) a choice row, a number a slider over its range. A feature gets its row
// by giving its variable a type and a label; it writes no menu code. Changes go through
// cmd::SetCvar like a console line, so the profile gate and the config apply.
#pragma once

namespace fdcore::settings {

bool Init();  // after every module has registered its variables and labels

} // namespace fdcore::settings
