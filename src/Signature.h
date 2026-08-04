// Signature.h -- pattern scanning and RIP-relative resolution.
//
// The point of this module is that nothing downstream hardcodes an address.
// The module base already moves with ASLR (we take it from GetModuleHandle),
// but a game patch also shifts every RVA, and a hardcoded offset then reads
// something arbitrary and reports it as a monster. Finding the reference in
// code instead means the mod either resolves correctly or reports honestly
// that it could not.

#pragma once

#include <cstdint>
#include <cstddef>

namespace Sig {

// Bounds of the main module and its sections. Call once before anything else.
bool Init();

std::uintptr_t ModuleBase();
std::uintptr_t TextBase();
std::size_t TextSize();

// IDA-style pattern: "48 83 3D ? ? ? ? 00 74 0E". '?' or '??' is a wildcard.
// Returns 0 if not found, or if the pattern matches more than once (an
// ambiguous signature is a broken signature -- better to fail loudly).
std::uintptr_t Find(const char* pattern);

// Same, but returns the first match even when several exist.
std::uintptr_t FindFirst(const char* pattern);

// Resolve a RIP-relative operand.
//   instruction: address of the first byte of the instruction
//   dispOffset:  byte offset of the disp32 within the instruction
//   length:      total instruction length
// target = instruction + length + (int32)disp
std::uintptr_t ResolveRip(std::uintptr_t instruction, int dispOffset, int length);

// Locate a NUL-terminated string in the module's read-only data.
std::uintptr_t FindString(const char* text);

// Find a `LEA reg, [rip+disp]` whose target is `address`. This is the most
// patch-resilient anchor available: string literals survive recompilation far
// better than instruction encodings do.
std::uintptr_t FindLeaTo(std::uintptr_t address);

}  // namespace Sig
