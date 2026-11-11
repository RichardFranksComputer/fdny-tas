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

// --- Modes (D65) ---------------------------------------------------------------------------
// A mode is a set of named actions whose keys act only while it is active: the warp HUD's
// keys, the checkpoint editor, the recordings viewer, the flying camera. A feature never
// reads the keyboard itself: it registers its actions with their default keys and asks
// Pressed / Held each frame. The player rebinds them (`bind <mode> <key> <action>`), only
// changes are saved, and the reference and the tool's own on-screen hints come from the
// same table (tools/docgen.py reads the literals; KeyLabel gives the current key).
//
// Keys: one or more alternatives, "space ins"; "ctrl+c" needs Ctrl held (a plain "c" in the
// same mode then doesn't fire with Ctrl). All strings must be static.
using ModeActive = bool (*)();
enum ModeKind {
    M_NORMAL,     // its keys act while it is active, with ordinary binds and other modes
    M_EXCLUSIVE,  // while active, ordinary binds, the game and other modes get no keys
                  // (its key messages are consumed); M_SHARED modes stay live with it
    M_SHARED,     // a helper mode other modes build on (the camera's flight keys)
};
// False when the mode table is full (a logged error).
bool Mode(const char* name, const char* when, ModeActive active, ModeKind kind);
// An action, by how its key is read. A press or repeat action takes its key from ordinary
// binds and from modes registered earlier while its mode is active; a held action (a
// movement key, a modifier) never takes a key from anything. Returns a handle (-1: unknown
// mode or table full, logged; Pressed / Held on it are false).
int Action(const char* mode, const char* id, const char* keys, const char* help);        // once per press
int ActionRepeat(const char* mode, const char* id, const char* keys, const char* help);  // press, then repeats while held
int ActionHeld(const char* mode, const char* id, const char* keys, const char* help);    // read with Held
// This frame (since the last frame begin), while the mode is active and keys are live.
bool Pressed(int action);
bool Held(int action);
// The action's current key for a hint line: "Space", "Ctrl+C", "PgUp"; "unbound" if none.
const char* KeyLabel(int action);
// A mode's keys as hint lines for its own panel, from the keys bound now:
// "Space: place | Mouse1: select | Tab: next" (the action's id, _ as a space), wrapped at
// 76 columns into at most maxLines lines of kHintLen. Returns the lines written.
constexpr int kHintLen = 160;
int HintLines(const char* mode, char (*out)[kHintLen], int maxLines);
// While on, only this mode's actions (and ordinary binds) are live: a tool in the middle of
// a drag freezes the modes under it.
void Solo(const char* mode, bool on);
// A mode that ends on a key press (Escape) calls this as it ends: every key of the active
// modes still held stays consumed until it is released, so the game never sees the rest of
// that press.
void SuppressHeld();
// True when ordinary binds must not act on vk now: an exclusive mode is active, or an
// active mode's press action is on it.
bool KeyCaptured(int vk);
// The registry, for the Debug registry dump (tests/game/docs_registry.yaml).
int ModeCount();
bool ModeAt(int i, const char** name, const char** when, ModeKind* kind);
int ActionCount();
bool ActionAt(int i, const char** mode, const char** id, const char** keys, const char** help, int* how);
#ifdef FDCORE_UNIT
// Unit tests: the keyboard, "keys are live", and one frame's poll.
void SetKeyStateForUnitTest(bool (*down)(int vk));
void SetLiveForUnitTest(bool live);
void PollForUnitTest();
bool MessageForUnitTest(unsigned message, uintptr_t wparam, intptr_t lparam);
#endif
// A modal pointer tool takes mouse motion from free-camera look.
void SetPointerCapture(bool (*filter)());
bool PointerCaptured();
bool AddPointerCapture(bool (*filter)());

} // namespace fdcore::binds

// One key's press detection, from polling and (for an exclusive mode) its window messages:
// a tap shorter than a frame still counts once, a held or repeated key never twice.
namespace fdcore::binds::input {
struct Key {
    bool previous, pending;
    void Reset(bool down) {
        previous = down;
        pending = false;
    }
    void Message(bool repeat) {
        if (!repeat && !previous)
            pending = true;
    }
    void Release() { previous = false; }
    bool Poll(bool down, bool live) {
        const bool pressed = live && (pending || (down && !previous));
        pending = false;
        previous = down;
        return pressed;
    }
};
} // namespace fdcore::binds::input
