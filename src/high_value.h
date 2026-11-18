#pragma once

#include <string_view>

namespace high_value {

inline bool bitcoin(std::string_view name) {
    constexpr std::string_view token = "_bitcoin";
    return name == "bitcoin" || (name.size() > token.size() &&
        name.substr(name.size() - token.size()) == token);
}

// Exact Ground Zero classes from https://groundzerodayz.co/prices, data dated 2026-11-02.
// Only sellable valuables with a listed maximum sale price strictly above 50,000 rubles.
inline bool matches(std::string_view name) {
    constexpr std::string_view classes[] = {
        "gzeft_hvegyptiantower",
        "gzeft_hvglobe",
        "gzeft_hvgoldenpendant",
        "gzeft_hvpainting",
        "gzeft_hvrhinohorn",
        "gzeft_hvvase",
        "gzeft_ledx",
        "gzeft_moonshine",
        "gzeft_tankbattery",
        "gzeft_virtex",
        "videocard_bl3060",
        "videocard_bl3080",
    };
    for (const auto candidate : classes) if (name == candidate) return true;
    // Physical Bitcoin is documented by the server. Its exact class still needs
    // a live sample. Match the complete terminal token, excluding miner parts.
    return bitcoin(name);
}

}
