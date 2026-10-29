// Command + variable registry behind the console. Lines typed into the game's
// console are offered here first; anything not registered goes to the game.
#pragma once

namespace fdcore::cmd {

constexpr int kMaxArgs = 16;

using CommandFn = void (*)(int argc, const char* const* argv);

struct Cvar {
    const char* name;
    const char* defaultValue;
    const char* help;
    bool readOnly;
    bool archive;               // persisted in FDNYCore.cfg when not at default
    void (*onChange)(Cvar& v);  // may be null
    char value[128];
};

void RegisterCommand(const char* name, const char* help, CommandFn fn);
// The Cvar must outlive the core (static storage). Value starts at its default.
void RegisterCvar(Cvar* v);
Cvar* FindCvar(const char* name);
bool SetCvar(Cvar* v, const char* value);  // false if read-only or refused by the gate

// Profiles (item 12): gates that may refuse a variable value or a command;
// they print their own reason. Null = everything allowed.
using CvarGate = bool (*)(const Cvar& v, const char* value);
using CommandGate = bool (*)(const char* name);
void SetGates(CvarGate cvarGate, CommandGate commandGate);
// Features declare which of their variables / commands are practice-only
// (anything that shows information the game doesn't, or moves the player);
// the profile gate refuses them in `fd_profile run`. Name must be static.
void SetPracticeOnly(const char* name);
bool IsPracticeOnly(const char* name);
// Profile switches put practice-only vars back without the gate.
bool SetCvarUngated(Cvar* v, const char* value);
// Every registered variable (watermark: what is not at its default).
int CvarCount();
Cvar* CvarAt(int i);

// Executes one line if it's ours. Returns false (untouched) if the first
// token isn't a registered command or variable, so the caller can hand the
// line to the game.
bool Dispatch(const char* line);

// Registers echo / cvarlist / cmdlist / exec and the core's own variables.
void RegisterBuiltins();

constexpr const char* kConfigName = "FDNYCore.cfg";
// Writes FDNYCore.cfg (archived non-default vars + binds). False while loading it.
bool SaveConfig();
// Modules that persist more than variables (binds, spawn pins) append their own
// FDNYCore.cfg lines; returns chars written.
using ConfigWriter = int (*)(char* out, int cap);
void RegisterConfigWriter(ConfigWriter fn);

// Runs FDNYCore.cfg, then autoexec.cfg (game thread, once the console exists).
void ExecStartup();

// Lines from `exec` that belong to the game are queued and handed to the
// game one per frame (its script engine stores one pending line at a time).
void QueueGameLine(const char* line);
bool PopGameLine(char* out, int cap);

} // namespace fdcore::cmd
