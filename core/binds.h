// Key binds: `bind <key> <command>` runs a console line when a key is pressed
// in a level (console closed, game window focused; D58). Game keys aren't swallowed.
// Also the one owner of the game window's messages (OnWindowMessage).
#pragma once
#include <stdint.h>

namespace fdcore::binds {

void Init();  // registers bind / unbind / unbindall / bindlist, subscribes to frame_begin

// Appends `bind <key> "<line>"` lines for FDNYCore.cfg; returns chars written.
// A default key the player unbound is written as `unbind <key>` so it stays unbound.
int AppendConfig(char* out, int cap);

// A feature's default key: bound at module init, before FDNYCore.cfg runs, so the
// player's own binds / unbinds win. False if the key name is unknown or taken.
bool BindDefault(const char* key, const char* line);

// A key a feature reads itself, not through a bind (e.g. the free camera's W/A/S/D while
// paused). Description only: `bindlist` shows it and the reference lists it
// (tools/docgen.py reads the literals). `key` is a key name or a list ("w a s d"),
// `when` the condition ("while fd_pause is 1"). All three must be static strings.
void DescribeKey(const char* key, const char* action, const char* when);

// The default binds (BindDefault) and described keys, for the Debug registry dump.
int DefaultCount();
bool DefaultAt(int i, const char** key, const char** line);
int DescribedCount();
bool DescribedAt(int i, const char** key, const char** action, const char** when);

// True when in-game keys should act: a level is loaded, the game window has focus and
// the console is closed (the rule binds use; also used by the warp HUD's B/Z/X keys).
bool KeysLive();

// The game window's messages before the game sees them: binds subclasses the window once
// (on the next frame after the first filter) and offers each message to the filters in
// registration order; a filter returns true to consume it. Menus and modal tools read
// keys, typing and the wheel here (D58). Up to 4 filters; false when full.
using MessageFilter = bool (*)(unsigned message, uintptr_t wparam, intptr_t lparam);
bool OnWindowMessage(MessageFilter filter);

// A modal tool may reserve keys from ordinary binds and free-camera movement.
// The filter must include its own active-state check. Null releases the capture.
using KeyCapture = bool (*)(int vk);
void SetKeyCapture(KeyCapture filter);
bool KeyCaptured(int vk);
// Additional independent modal tools retain their active-state filters.
bool AddKeyCapture(KeyCapture filter);
// A modal pointer tool takes mouse motion from free-camera look.
void SetPointerCapture(bool (*filter)());
bool PointerCaptured();
bool AddPointerCapture(bool (*filter)());

} // namespace fdcore::binds
