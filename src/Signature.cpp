#include "Signature.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace Sig {
namespace {

std::uintptr_t g_base = 0;
std::uintptr_t g_text = 0;
std::size_t g_textSize = 0;
std::uintptr_t g_rdata = 0;
std::size_t g_rdataSize = 0;
std::uintptr_t g_data = 0;
std::size_t g_dataSize = 0;

struct Token {
    std::uint8_t value;
    bool wildcard;
};

// "48 8B ? ? 00" -> tokens. Accepts '?' and '??' for a wildcard byte.
std::vector<Token> Parse(const char* pattern) {
    std::vector<Token> out;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            out.push_back({0, true});
            ++p;
            if (*p == '?') ++p;
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hex(p[0]);
        int lo = p[1] ? hex(p[1]) : -1;
        if (hi < 0 || lo < 0) break;  // malformed; stop rather than guess
        out.push_back({static_cast<std::uint8_t>((hi << 4) | lo), false});
        p += 2;
    }
    return out;
}

bool MatchAt(const std::uint8_t* at, const std::vector<Token>& tokens) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].wildcard) continue;
        if (at[i] != tokens[i].value) return false;
    }
    return true;
}

std::uintptr_t Scan(const char* pattern, bool requireUnique) {
    if (!g_text) return 0;
    std::vector<Token> tokens = Parse(pattern);
    if (tokens.empty() || tokens.size() > g_textSize) return 0;

    const auto* begin = reinterpret_cast<const std::uint8_t*>(g_text);
    const std::size_t last = g_textSize - tokens.size();

    // Anchor on the first non-wildcard byte so memchr does the heavy lifting.
    std::size_t anchor = 0;
    while (anchor < tokens.size() && tokens[anchor].wildcard) ++anchor;
    if (anchor == tokens.size()) return 0;
    const std::uint8_t anchorByte = tokens[anchor].value;

    std::uintptr_t found = 0;
    std::size_t i = 0;
    while (i <= last) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(begin + i + anchor, anchorByte, last - i + 1));
        if (!hit) break;
        std::size_t pos = static_cast<std::size_t>(hit - begin) - anchor;
        if (pos > last) break;
        if (MatchAt(begin + pos, tokens)) {
            if (!requireUnique) return g_text + pos;
            if (found) return 0;  // ambiguous -- refuse rather than guess
            found = g_text + pos;
        }
        i = pos + 1;
    }
    return found;
}

std::uintptr_t ScanRange(std::uintptr_t base, std::size_t size,
                         const void* needle, std::size_t len) {
    if (!base || size < len) return 0;
    const auto* begin = reinterpret_cast<const std::uint8_t*>(base);
    const auto first = *static_cast<const std::uint8_t*>(needle);
    std::size_t i = 0;
    while (i + len <= size) {
        const auto* hit = static_cast<const std::uint8_t*>(
            std::memchr(begin + i, first, size - len - i + 1));
        if (!hit) return 0;
        std::size_t pos = static_cast<std::size_t>(hit - begin);
        if (!std::memcmp(begin + pos, needle, len)) return base + pos;
        i = pos + 1;
    }
    return 0;
}

}  // namespace

bool Init() {
    if (g_base) return true;
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    g_base = base;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        auto va = base + sec->VirtualAddress;
        std::size_t size = (std::max)(sec->Misc.VirtualSize, sec->SizeOfRawData);
        const char* name = reinterpret_cast<const char*>(sec->Name);
        if (!std::strncmp(name, ".text", 5)) {
            g_text = va;
            g_textSize = size;
        } else if (!std::strncmp(name, ".rdata", 6)) {
            g_rdata = va;
            g_rdataSize = size;
        } else if (!std::strncmp(name, ".data", 5)) {
            g_data = va;
            g_dataSize = size;
        }
    }
    return g_text != 0;
}

std::uintptr_t ModuleBase() { return g_base; }
std::uintptr_t TextBase() { return g_text; }
std::size_t TextSize() { return g_textSize; }

std::uintptr_t Find(const char* pattern) { return Scan(pattern, true); }
std::uintptr_t FindFirst(const char* pattern) { return Scan(pattern, false); }

std::uintptr_t ResolveRip(std::uintptr_t instruction, int dispOffset,
                          int length) {
    if (!instruction) return 0;
    std::int32_t disp = *reinterpret_cast<const std::int32_t*>(
        instruction + dispOffset);
    return instruction + length + static_cast<std::intptr_t>(disp);
}

std::uintptr_t FindString(const char* text) {
    std::size_t len = std::strlen(text) + 1;  // include the NUL
    std::uintptr_t hit = ScanRange(g_rdata, g_rdataSize, text, len);
    if (!hit) hit = ScanRange(g_data, g_dataSize, text, len);
    return hit;
}

std::uintptr_t FindLeaTo(std::uintptr_t address) {
    if (!g_text || !address) return 0;
    const auto* p = reinterpret_cast<const std::uint8_t*>(g_text);
    // 48 8D /r with mod=00, rm=101 is `LEA r64, [rip+disp32]`, 7 bytes.
    for (std::size_t i = 0; i + 7 <= g_textSize; ++i) {
        if (p[i] != 0x48 || p[i + 1] != 0x8D) continue;
        if ((p[i + 2] & 0xC7) != 0x05) continue;
        auto instr = g_text + i;
        if (ResolveRip(instr, 3, 7) == address) return instr;
    }
    return 0;
}

}  // namespace Sig
