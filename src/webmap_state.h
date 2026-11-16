#pragma once
#include "radar.h"
#include "webmap.h"

#include <chrono>
#include <memory>

namespace webmap {

using Clock = std::chrono::steady_clock;

struct Snapshot {
    std::string map;
    radar::Frame frame;
    Clock::time_point captured;
};

}
