// Shared, lazy Player/Victim lifecycle observations. Only this service owns
// these game functions; subscribers run on the game thread, after the original
// callback (except destruction, which is observed before memory is freed).
#pragma once

namespace fdcore::entities {
using EntityFn = void (*)(void* entity);
bool ObservePlayer(EntityFn initialized, EntityFn updated);
bool ObserveVictims(EntityFn spawned, EntityFn updated, EntityFn destroying);

// One filter on a victim's spawn spot (speedrun's static spawns). It runs inside
// SpawnAtSpot after the RNG-chosen spot is copied to the victim and before the game
// floor-snaps it, so it may rewrite the victim's position. The site is patched at
// once, so call this from a module's early step (DllMain, before any level). False if
// the site doesn't match (another mod patched it) or a filter is already set.
bool FilterVictimSpot(EntityFn filter);

// Runtime RCS_BoundsTrigger support. The world owns the native roots; this
// service owns their backing definitions/scripts until the next level entry.
void ResetNativeTriggers(); // only after level entry, once old world teardown finished
using NativeContactFn = void (*)(int index, bool entering);
bool ObserveNativeContacts(NativeContactFn callback); // one owner of the runtime trigger pool
bool AddNativeTrigger(const char* name, const float* min, const float* max);
} // namespace fdcore::entities
