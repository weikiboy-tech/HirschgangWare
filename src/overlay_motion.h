#pragma once

#include <cstdint>

namespace overlay_motion {

// Predict short sampling gaps, then smoothly return to the last measured
// position when movement updates stop. Camera projection remains immediate.
inline float leadSeconds(uint64_t sampledAt, uint64_t now) {
    if (!sampledAt || now <= sampledAt) return 0.f;
    const double age = double(now - sampledAt) / 1000000.0;
    constexpr double hold = 0.025, stop = 0.080;
    if (age <= hold) return float(age);
    if (age >= stop) return 0.f;
    const double t = (age - hold) / (stop - hold);
    const double fade = 1.0 - t * t * (3.0 - 2.0 * t);
    return float(age * fade);
}

}
