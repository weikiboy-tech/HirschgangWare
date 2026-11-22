#pragma once
#include <array>
#include <cstdint>

namespace dma_keyboard {

using Bitmap = std::array<uint8_t, 64>;
enum class Transport { Pcie, Lan };

struct EdgeCounts {
    uint32_t pressed = 0;
    uint32_t released = 0;
};

struct Sample {
    uint32_t pid = 0;
    Bitmap bits{};
    Transport transport = Transport::Pcie;
    std::array<EdgeCounts, 256> edges{};
};

inline bool validKey(unsigned vk) { return vk >= 8 && vk < 255; }

inline bool down(const Bitmap& bits, unsigned vk) {
    return (bits[vk / 4] & (1u << (2 * (vk % 4)))) != 0;
}

inline bool sameSource(const Sample& previous, const Sample& current) {
    return previous.pid && previous.pid == current.pid && previous.transport == current.transport;
}

inline EdgeCounts edgesSince(const Sample& previous, const Sample& current, unsigned vk) {
    if (!validKey(vk) || current.transport != Transport::Lan || !sameSource(previous, current)) return {};
    return {current.edges[vk].pressed - previous.edges[vk].pressed,
            current.edges[vk].released - previous.edges[vk].released};
}

}  // namespace dma_keyboard
