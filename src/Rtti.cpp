#include "Rtti.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Signature.h"

namespace Rtti {
namespace {

// The 31 Actor_Enemy_* classes present in ge3.exe.
const char* const kEnemies[] = {
    "Anotherouga", "Anubis", "Archaic", "Borg", "Caligula", "Chromgawain",
    "Cocoonmaiden", "Dreadpike", "Gaou", "Gboro", "Guuzou", "Hannibal",
    "Kongou", "Kyuubi", "Lion", "Marduk", "Myouou", "Ougatail", "Pita",
    "Quadriga", "Ra", "Samurai", "Sariel", "Shiyu", "Taizai", "Target",
    "Ukonvasara", "Vajra", "Vampire", "Yaeger", "Zygote",
};

struct Entry {
    const char* name;
    bool enemy;
};

std::unordered_map<std::uintptr_t, Entry> g_byVtable;
std::vector<std::string> g_names;
bool g_done = false;

struct Sections {
    const std::uint8_t* rdata;
    std::size_t rdataSize;
    std::uintptr_t rdataVa;
};

Sections g_sec{};

bool LoadSections() {
    auto base = Sig::ModuleBase();
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::strncmp(reinterpret_cast<const char*>(sec->Name), ".rdata", 6))
            continue;
        g_sec.rdataVa = base + sec->VirtualAddress;
        g_sec.rdata = reinterpret_cast<const std::uint8_t*>(g_sec.rdataVa);
        g_sec.rdataSize = (std::max)(sec->Misc.VirtualSize, sec->SizeOfRawData);
        return true;
    }
    return false;
}

}  // namespace

std::size_t Init() {
    if (g_done) return g_byVtable.size();
    if (!Sig::Init() || !LoadSections()) return 0;

    auto base = Sig::ModuleBase();

    // Two linear passes over .rdata build tdRva -> COL and COL -> vtable.
    // Doing this once beats re-scanning the section for each of 32 classes.
    std::unordered_map<std::uint32_t, std::uintptr_t> colByTd;
    for (std::size_t i = 0; i + 0x18 <= g_sec.rdataSize; i += 4) {
        auto* c = reinterpret_cast<const std::uint32_t*>(g_sec.rdata + i);
        if (c[0] != 1) continue;  // x64 COL signature
        std::uintptr_t colVa = g_sec.rdataVa + i;
        if (c[5] != static_cast<std::uint32_t>(colVa - base)) continue;  // pSelf
        colByTd.emplace(c[3], colVa);
    }
    std::unordered_map<std::uintptr_t, std::uintptr_t> vtByCol;
    for (std::size_t i = 0; i + 8 <= g_sec.rdataSize; i += 8) {
        auto v = *reinterpret_cast<const std::uintptr_t*>(g_sec.rdata + i);
        if (v < g_sec.rdataVa || v >= g_sec.rdataVa + g_sec.rdataSize) continue;
        vtByCol.emplace(v, g_sec.rdataVa + i + 8);  // vtable[-1] holds the COL
    }

    g_names.reserve(std::size(kEnemies) + 4);

    auto add = [&](const char* decorated, const char* display, bool enemy) {
        std::uintptr_t nameVa = Sig::FindString(decorated);
        if (!nameVa) return;
        auto tdRva = static_cast<std::uint32_t>((nameVa - 0x10) - base);
        auto col = colByTd.find(tdRva);
        if (col == colByTd.end()) return;
        auto vt = vtByCol.find(col->second);
        if (vt == vtByCol.end()) return;
        g_names.emplace_back(display);
        g_byVtable.emplace(vt->second, Entry{g_names.back().c_str(), enemy});
    };

    char decorated[128];
    for (const char* n : kEnemies) {
        std::snprintf(decorated, sizeof(decorated),
                      ".?AVActor_Enemy_%s@anubis@@", n);
        add(decorated, n, true);
    }
    add(".?AVActorPlayer@anubis@@", "Player", false);

    g_done = true;
    return g_byVtable.size();
}

const char* NameForVtable(std::uintptr_t vtable) {
    auto it = g_byVtable.find(vtable);
    return it == g_byVtable.end() ? nullptr : it->second.name;
}

bool IsEnemy(std::uintptr_t vtable) {
    auto it = g_byVtable.find(vtable);
    return it != g_byVtable.end() && it->second.enemy;
}

std::size_t ClassCount() { return g_byVtable.size(); }

}  // namespace Rtti
