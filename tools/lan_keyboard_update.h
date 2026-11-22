#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lan_update {
inline constexpr wchar_t kSenderWindow[] = L"HirschgangWareLanKeyboard";
inline constexpr wchar_t kReadyProperty[] = L"HirschgangWareInputReady";

struct Release {
    unsigned revision;
    std::array<uint8_t, 32> sha256;
};

inline std::optional<Release> parseRelease(std::string_view line) {
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const auto separator = line.find(' ');
    if (separator == line.npos || line.size() - separator - 1 != 64) return {};
    Release release{};
    const auto number = std::from_chars(line.data(), line.data() + separator, release.revision);
    if (number.ec != std::errc{} || number.ptr != line.data() + separator ||
        release.revision == 0 || release.revision > 65535) return {};
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < release.sha256.size(); ++i) {
        const auto hi = digit(line[separator + 1 + i * 2]);
        const auto lo = digit(line[separator + 2 + i * 2]);
        if (hi < 0 || lo < 0) return {};
        release.sha256[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return release;
}
}  // namespace lan_update
