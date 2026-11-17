#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace crosshair {

enum class Style { Plus, Ring, Dot };
constexpr int kMinSize = 2, kMaxSize = 20;
constexpr COLORREF kDefaultColor = RGB(80, 255, 140);

inline float center(LONG start, LONG end) {
    return float(start) + float(end - start) * 0.5f;
}

// Bounds are half open. Reflecting a pixel around the midpoint gives the
// same shape for odd and even output resolutions.
inline RECT centered(float x, float y, float rx, float ry) {
    return {LONG(std::ceil(x - rx - 0.5f)), LONG(std::ceil(y - ry - 0.5f)),
            LONG(std::floor(x + rx - 0.5f)) + 1, LONG(std::floor(y + ry - 0.5f)) + 1};
}

template<class Fill>
void draw(const RECT& view, Style style, int size, COLORREF color, Fill fill) {
    if (view.left >= view.right || view.top >= view.bottom) return;
    const float x = center(view.left, view.right), y = center(view.top, view.bottom);
    const float radius = float(std::clamp(size, kMinSize, kMaxSize));
    // Pure black is the overlay transparency key.
    if (!color) color = RGB(1, 1, 1);
    const int brightness = 3 * GetRValue(color) + 6 * GetGValue(color) + GetBValue(color);
    const COLORREF outline = brightness < 750 ? RGB(245, 245, 245) : RGB(1, 1, 1);
    if (style == Style::Plus) {
        for (int border = 1; border >= 0; --border) {
            const COLORREF ink = border ? outline : color;
            fill(centered(x, y, radius + border, 0.5f + border), ink);
            fill(centered(x, y, 0.5f + border, radius + border), ink);
        }
        return;
    }

    const bool ring = style == Style::Ring;
    const float circleRadius = ring ? radius : radius * 0.5f;
    for (int border = 1; border >= 0; --border) {
        const float outer = circleRadius + border;
        const float inner = ring ? (std::max)(0.75f, circleRadius - 1.5f - border) : 0.f;
        const COLORREF ink = border ? outline : color;
        const RECT bounds = centered(x, y, outer, outer);
        for (LONG row = bounds.top; row < bounds.bottom; ++row) {
            const float dy = float(row) + 0.5f - y;
            const float span = std::sqrt((std::max)(0.f, outer * outer - dy * dy));
            const RECT outerRow = centered(x, y, span, outer);
            if (std::fabs(dy) >= inner) {
                fill(RECT{outerRow.left, row, outerRow.right, row + 1}, ink);
            } else {
                const float hole = std::sqrt(inner * inner - dy * dy);
                const RECT innerRow = centered(x, y, hole, outer);
                fill(RECT{outerRow.left, row, innerRow.left, row + 1}, ink);
                fill(RECT{innerRow.right, row, outerRow.right, row + 1}, ink);
            }
        }
    }
}

} // namespace crosshair
