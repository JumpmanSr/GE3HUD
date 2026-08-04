// ActorList.h -- read the game's own enemy list.
//
// This is the difference between this tool and the sweep-based one. Instead of
// scanning gigabytes of heap for objects that look like actors, we ask the
// game: anubis::ActorManager keeps a vector of the enemies currently in the
// level. Walking it costs microseconds and cannot produce a false positive,
// because it is the same list the game itself iterates.
//
// Consequences of that, all of which the sweep needed workarounds for:
//   * no class-registry records mistaken for monsters
//   * no freed objects lingering with a stale vtable pointer
//   * no plausibility heuristics (HP floors, position checks, spacing rules)
//   * no rescan, manual or automatic -- the list is walked fresh every frame
//   * an empty list is a truthful "not in a mission"

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ActorList {

struct Enemy {
    std::uintptr_t address = 0;
    const char* name = nullptr;
    float life = 0.0f;
    float maxLife = 0.0f;
    float x = 0.0f, y = 0.0f, z = 0.0f;

    bool IsAlive() const { return life > 0.0f; }
    float Fraction() const { return maxLife > 0.0f ? life / maxLife : 0.0f; }
};

enum class Status {
    NotInitialised,
    NoSignature,      // could not locate ActorManager in this build
    NoRtti,           // could not resolve actor class names
    NoManager,        // signature fine, but the singleton is null (menu/hub)
    Ok,
};

// Resolve the ActorManager pointer and the RTTI class table. Safe to call
// repeatedly; the work happens once. Returns true when ready to walk.
bool Init();

// Re-read the enemy list. Cheap enough to call every frame.
void Refresh();

const std::vector<Enemy>& Enemies();

Status GetStatus();
const char* StatusText();

// Diagnostics for the console dump.
std::uintptr_t ManagerPointerAddress();  // where the singleton pointer lives
std::uintptr_t ManagerAddress();         // the ActorManager itself
std::size_t ListOffset();                // which field the list was found at
bool ListOffsetWasRecovered();           // true if auto-discovery kicked in

}  // namespace ActorList
