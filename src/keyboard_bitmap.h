#pragma once
#include <array>
#include <cstdint>

namespace dma_keyboard {

using Bitmap = std::array<uint8_t, 64>;

struct Sample {
    uint32_t pid = 0;
    Bitmap bits{};
};

inline bool validKey(unsigned vk) { return vk >= 8 && vk < 255; }

inline bool down(const Bitmap& bits, unsigned vk) {
    return (bits[vk / 4] & (1u << (2 * (vk % 4)))) != 0;
}

}  // namespace dma_keyboard
