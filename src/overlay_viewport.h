#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace overlay_viewport {

// Stored in hundredths of a percent of the actual overlay client size.
constexpr int kFull = 10000;
constexpr int kMaxOffset = 2500;
constexpr int kMinScale = 5000, kMaxScale = 15000;

struct Alignment {
    int x = 0, y = 0;
    int width = kFull, height = kFull;
};

inline Alignment validated(Alignment value) {
    value.x = std::clamp(value.x, -kMaxOffset, kMaxOffset);
    value.y = std::clamp(value.y, -kMaxOffset, kMaxOffset);
    value.width = std::clamp(value.width, kMinScale, kMaxScale);
    value.height = std::clamp(value.height, kMinScale, kMaxScale);
    return value;
}

inline RECT projectInto(const RECT& output, Alignment value) {
    const LONG width = output.right - output.left, height = output.bottom - output.top;
    if (width <= 0 || height <= 0) return {};
    value = validated(value);
    const LONG w = (std::max)(1L, std::lround(double(width) * value.width / kFull));
    const LONG h = (std::max)(1L, std::lround(double(height) * value.height / kFull));
    const LONG x = output.left + std::lround((width - w) * 0.5 + double(width) * value.x / kFull);
    const LONG y = output.top + std::lround((height - h) * 0.5 + double(height) * value.y / kFull);
    return {x, y, x + w, y + h};
}

}  // namespace overlay_viewport
