#pragma once

namespace brand {

inline constexpr char name[] = "HirschgangWare";
inline constexpr int iconResource = 101;

struct Segment {
    float x1, y1, x2, y2;
};

// One side of the antlers and head, reflected around the vertical axis.
inline constexpr Segment deer[] = {
    {0.00f, 0.65f, 0.21f, 0.47f},
    {0.21f, 0.47f, 0.31f, 0.08f},
    {0.31f, 0.08f, 0.52f, 0.00f},
    {0.31f, 0.08f, 0.25f, -0.29f},
    {0.25f, -0.29f, 0.43f, -0.53f},
    {0.43f, -0.53f, 0.45f, -0.72f},
    {0.43f, -0.53f, 0.68f, -0.57f},
    {0.68f, -0.57f, 0.76f, -0.75f},
    {0.25f, -0.29f, 0.12f, -0.46f},
    {0.31f, 0.08f, 0.00f, 0.16f},
};

}  // namespace brand
