#include "ActorList.h"

#include <windows.h>

#include <cstring>

#include "Rtti.h"
#include "Signature.h"

namespace ActorList {
namespace {

// --- what we look for -----------------------------------------------------
//
// anubis::ActorManager is a singleton guarded like this:
//
//   CMP  qword ptr [rip+disp], 0        48 83 3D ?? ?? ?? ?? 00
//   JZ   create                         74 0E
//   LEA  RCX, ["...ActorManager already exist."]
//   CALL log
//   JMP  done
//   MOV  RCX, RDI                       48 8B CF
//   CALL ActorManager::create
//
// The CMP's operand is the singleton pointer. Two ways to reach it, tried in
// order of how well they survive a game patch.

const char* const kGuardString = "[VIEWER] Error: ActorManager already exist.\n";

// Fallback: the guard sequence itself. Distinctive, but instruction encodings
// change more readily than string literals do.
const char* const kGuardPattern =
    "48 83 3D ? ? ? ? 00 74 0E 48 8D 0D ? ? ? ? E8 ? ? ? ? EB 08 48 8B CF";

// Offsets within ActorManager. These are struct layout, not addresses, so a
// signature cannot find them -- but they are validated on use, and if the
// validation fails the list is re-discovered by scanning the object.
constexpr std::size_t kEnemyListOffset = 0xB8;   // vector<Element*> begin/end
constexpr std::size_t kPlayerListOffset = 0xA0;  // the user players
constexpr std::size_t kElementActorOffset = 8;   // actor = *(*element + 8)

// Fields within an Actor_Enemy_*. Verified live: an untouched enemy reads
// 350/350, and 112 damage takes it to exactly 238/350.
constexpr std::uintptr_t kOffLife = 0x20F0;      // float
constexpr std::uintptr_t kOffMaxLife = 0x20F4;   // float
constexpr std::uintptr_t kOffPosition = 0x08F0;  // float[3]

constexpr std::size_t kManagerScanBytes = 0x4000;
constexpr std::size_t kMaxEntries = 4096;
constexpr std::uintptr_t kMinPtr = 0x10000;
constexpr std::uintptr_t kMaxPtr = 0x7FF000000000;

std::uintptr_t g_managerPtrVa = 0;
std::size_t g_listOffset = kEnemyListOffset;
bool g_recovered = false;
Status g_status = Status::NotInitialised;
std::vector<Enemy> g_enemies;
bool g_init = false;

// Every read of game memory goes through here. The list is walked while the
// game is mutating it, so a pointer can go stale mid-walk; an unguarded fault
// inside the Present hook would take the game down.
bool SafeRead(const void* src, void* dst, std::size_t len) {
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPtr(std::uintptr_t at, std::uintptr_t* out) {
    return SafeRead(reinterpret_cast<const void*>(at), out, sizeof(*out));
}

bool PlausiblePtr(std::uintptr_t v) {
    return v > kMinPtr && v < kMaxPtr && (v % 8) == 0;
}

std::uintptr_t ResolveManagerPointer() {
    // Preferred: anchor on the log string, find the LEA that references it,
    // then step back to the CMP that guards it. The literal is far more stable
    // across patches than the surrounding code.
    std::uintptr_t str = Sig::FindString(kGuardString);
    if (str) {
        std::uintptr_t lea = Sig::FindLeaTo(str);
        if (lea) {
            // CMP(8) + JZ(2) sits immediately before the LEA.
            std::uintptr_t cmp = lea - 10;
            std::uint8_t head[3] = {};
            if (SafeRead(reinterpret_cast<const void*>(cmp), head, sizeof(head)) &&
                head[0] == 0x48 && head[1] == 0x83 && head[2] == 0x3D) {
                return Sig::ResolveRip(cmp, 3, 8);
            }
        }
    }
    // Fallback: match the guard sequence directly.
    std::uintptr_t cmp = Sig::Find(kGuardPattern);
    if (cmp) return Sig::ResolveRip(cmp, 3, 8);
    return 0;
}

// Walk one candidate vector, appending any recognised enemies.
// Returns how many entries resolved to a known actor class.
int WalkVector(std::uintptr_t mgr, std::size_t offset,
               std::vector<Enemy>* out, bool enemiesOnly) {
    std::uintptr_t begin = 0, end = 0;
    if (!ReadPtr(mgr + offset, &begin)) return 0;
    if (!ReadPtr(mgr + offset + 8, &end)) return 0;
    if (!PlausiblePtr(begin) || !PlausiblePtr(end)) return 0;
    if (end < begin || ((end - begin) % 8) != 0) return 0;

    std::size_t count = (end - begin) / 8;
    if (count == 0 || count > kMaxEntries) return 0;

    int resolved = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t element = 0, inner = 0, actor = 0, vtable = 0;
        if (!ReadPtr(begin + i * 8, &element) || !PlausiblePtr(element)) continue;
        if (!ReadPtr(element, &inner) || !PlausiblePtr(inner)) continue;
        if (!ReadPtr(inner + kElementActorOffset, &actor) ||
            !PlausiblePtr(actor))
            continue;
        if (!ReadPtr(actor, &vtable)) continue;

        const char* name = Rtti::NameForVtable(vtable);
        if (!name) continue;
        if (enemiesOnly && !Rtti::IsEnemy(vtable)) continue;
        ++resolved;

        if (!out) continue;
        Enemy e;
        e.address = actor;
        e.name = name;
        struct { float life, maxLife; } hp{};
        if (SafeRead(reinterpret_cast<const void*>(actor + kOffLife), &hp,
                     sizeof(hp))) {
            e.life = hp.life;
            e.maxLife = hp.maxLife;
        }
        float pos[3]{};
        if (SafeRead(reinterpret_cast<const void*>(actor + kOffPosition), pos,
                     sizeof(pos))) {
            e.x = pos[0];
            e.y = pos[1];
            e.z = pos[2];
        }
        out->push_back(e);
    }
    return resolved;
}

// If the known offset stops producing enemies after a patch, look for the
// list rather than silently showing nothing. Costs one pass over a 16 KB
// object and only runs when the expected offset has failed.
bool RediscoverListOffset(std::uintptr_t mgr) {
    std::size_t best = 0;
    int bestCount = 0;
    for (std::size_t off = 0; off + 16 <= kManagerScanBytes; off += 8) {
        if (off == kPlayerListOffset) continue;  // known to be the players
        int n = WalkVector(mgr, off, nullptr, true);
        if (n > bestCount) {
            bestCount = n;
            best = off;
        }
    }
    if (!bestCount) return false;
    g_listOffset = best;
    g_recovered = true;
    return true;
}

}  // namespace

bool Init() {
    if (g_init) return g_status != Status::NoSignature &&
                       g_status != Status::NoRtti;
    g_init = true;

    if (!Sig::Init()) {
        g_status = Status::NoSignature;
        return false;
    }
    if (Rtti::Init() == 0) {
        g_status = Status::NoRtti;
        return false;
    }
    g_managerPtrVa = ResolveManagerPointer();
    if (!g_managerPtrVa) {
        g_status = Status::NoSignature;
        return false;
    }
    g_status = Status::NoManager;
    return true;
}

void Refresh() {
    g_enemies.clear();
    if (!Init()) return;

    std::uintptr_t mgr = 0;
    if (!ReadPtr(g_managerPtrVa, &mgr) || !PlausiblePtr(mgr)) {
        g_status = Status::NoManager;
        return;
    }

    int n = WalkVector(mgr, g_listOffset, &g_enemies, true);
    if (n == 0 && !g_enemies.size()) {
        // Either there are genuinely no enemies, or the layout moved. Only pay
        // for rediscovery when the manager exists but the field looks wrong.
        std::uintptr_t begin = 0, end = 0;
        bool shaped = ReadPtr(mgr + g_listOffset, &begin) &&
                      ReadPtr(mgr + g_listOffset + 8, &end) &&
                      PlausiblePtr(begin) && PlausiblePtr(end) && end >= begin;
        if (!shaped && RediscoverListOffset(mgr))
            WalkVector(mgr, g_listOffset, &g_enemies, true);
    }
    g_status = Status::Ok;
}

const std::vector<Enemy>& Enemies() { return g_enemies; }
Status GetStatus() { return g_status; }

const char* StatusText() {
    switch (g_status) {
    case Status::NotInitialised: return "not started";
    case Status::NoSignature:    return "ActorManager signature not found";
    case Status::NoRtti:         return "actor class names not resolved";
    case Status::NoManager:      return "not in a mission";
    case Status::Ok:             return "ok";
    }
    return "?";
}

std::uintptr_t ManagerPointerAddress() { return g_managerPtrVa; }

std::uintptr_t ManagerAddress() {
    std::uintptr_t mgr = 0;
    if (g_managerPtrVa) ReadPtr(g_managerPtrVa, &mgr);
    return mgr;
}

std::size_t ListOffset() { return g_listOffset; }
bool ListOffsetWasRecovered() { return g_recovered; }

}  // namespace ActorList
