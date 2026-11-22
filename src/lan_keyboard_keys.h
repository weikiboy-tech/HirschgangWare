#pragma once
#include "lan_keyboard_wire.h"
#include <charconv>

namespace lan_keyboard {
inline void setKey(dma_keyboard::Bitmap& bits, unsigned vk, bool down) {
    const auto mask = uint8_t(1u << (2 * (vk % 4)));
    if (down) bits[vk / 4] |= mask;
    else bits[vk / 4] &= uint8_t(~mask);
}

inline bool parseAllowedKeys(std::string_view list, dma_keyboard::Bitmap& allowed) {
    dma_keyboard::Bitmap parsed{};
    if (list == "All" || list == "all" || list == "ALL") {
        for (unsigned vk = 8; dma_keyboard::validKey(vk); ++vk) setKey(parsed, vk, true);
        allowed = parsed;
        return true;
    }

    unsigned count = 0;
    while (!list.empty()) {
        const auto comma = list.find(',');
        auto key = list.substr(0, comma);
        while (!key.empty() && key.front() == ' ') key.remove_prefix(1);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        unsigned vk = 0;
        if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) {
            vk = unsigned(key[0]);
        } else if (key == "Escape") {
            vk = VK_ESCAPE;
        } else if (key.size() >= 2 && key[0] == 'F') {
            unsigned f = 0;
            const auto result = std::from_chars(key.data() + 1, key.data() + key.size(), f);
            if (result.ec == std::errc{} && result.ptr == key.data() + key.size() && f >= 1 && f <= 24)
                vk = VK_F1 + f - 1;
        } else if (key.size() > 3 && key.substr(0, 3) == "VK:") {
            const auto result = std::from_chars(key.data() + 3, key.data() + key.size(), vk);
            if (result.ec != std::errc{} || result.ptr != key.data() + key.size()) return false;
        }
        if (!dma_keyboard::validKey(vk) || ++count > 8) return false;
        setKey(parsed, vk, true);
        if (comma == std::string_view::npos) {
            allowed = parsed;
            return true;
        }
        list.remove_prefix(comma + 1);
    }
    return false;
}
}  // namespace lan_keyboard
