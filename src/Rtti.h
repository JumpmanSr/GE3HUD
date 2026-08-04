// Rtti.h -- MSVC x64 RTTI, used only to turn an actor pointer into a name.
//
// In the sweep-based tool this was how actors were *found*. Here the game's
// own ActorManager provides the list, so RTTI is demoted to a labelling step:
// given an object's vtable, what class is it?
//
//   ".?AVActor_Enemy_Anubis@anubis@@"  (type descriptor name, in .data)
//      -> TypeDescriptor           (name - 0x10)
//      -> CompleteObjectLocator    (validated via its pSelf RVA)
//      -> vtable                   (the qword pointing at the COL, + 8)

#pragma once

#include <cstddef>
#include <cstdint>

namespace Rtti {

// Resolve every known actor class. Returns how many were found.
std::size_t Init();

// Short display name for a vtable ("Anubis", "Vajra", "Player"), or nullptr
// if this vtable is not an actor class we know.
const char* NameForVtable(std::uintptr_t vtable);

// True if the vtable belongs to an Actor_Enemy_* class.
bool IsEnemy(std::uintptr_t vtable);

std::size_t ClassCount();

}  // namespace Rtti
