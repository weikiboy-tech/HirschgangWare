#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace player_identity {

// Reject incomplete or malformed strings instead of displaying a valid prefix.
inline std::string text(const uint8_t* bytes, size_t capacity) {
    size_t n = 0;
    while (n < capacity && bytes[n]) ++n;
    if (!n || n == capacity) return {};
    for (size_t i = 0; i < n;) {
        const unsigned c = bytes[i++];
        if (c < 0x20 || c == 0x7f) return {};
        if (c < 0x80) continue;
        unsigned remaining = 0, code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; code = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; code = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; code = c & 7; minimum = 0x10000; }
        else return {};
        if (i + remaining > n) return {};
        while (remaining--) {
            const auto next = bytes[i++];
            if ((next & 0xc0) != 0x80) return {};
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes), n);
}

using Names = std::unordered_map<uint32_t, std::string>;
inline void insert(Names& names, std::unordered_set<uint32_t>& conflicts, uint32_t id, const std::string& name) {
    if (!id || name.empty() || conflicts.count(id)) return;
    const auto inserted = names.emplace(id, name);
    if (!inserted.second && inserted.first->second != name) {
        names.erase(id);
        conflicts.insert(id);
    }
}

inline std::string key(std::string name) {
    for (auto& c : name) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return name;
}

// Multiple entity claims and repeated names are ambiguous, including the local player.
inline std::unordered_map<uint64_t, std::string> resolve(
        const Names& roster, const std::unordered_map<uint64_t, uint32_t>& entities) {
    std::unordered_map<uint32_t, unsigned> claims;
    std::unordered_map<std::string, unsigned> names;
    for (const auto& entry : roster) ++names[key(entry.second)];
    for (const auto& entry : entities) if (entry.second) ++claims[entry.second];
    std::unordered_map<uint64_t, std::string> result;
    for (const auto& entry : entities) {
        const auto found = roster.find(entry.second);
        if (found != roster.end() && claims[entry.second] == 1 && names[key(found->second)] == 1)
            result.emplace(entry.first, found->second);
    }
    return result;
}
}
