#pragma once
#include <string>

namespace radar { struct Frame; }

namespace webmap {

enum Release : unsigned {
    CurrentMap = 1, Own = 2, Heading = 4, Players = 8, Infected = 16,
    Animals = 32, Vehicles = 64, Loot = 128, PlayerNames = 256, PlayerHeld = 512, Events = 1024, Corpses = 2048, PlayerHealth = 4096,
    StashLoot = 8192, HighValues = 16384
};
constexpr unsigned AllReleases = CurrentMap | Own | Heading | Players | Infected | Animals |
                                 Vehicles | Loot | PlayerNames | PlayerHeld | Events | Corpses | PlayerHealth | StashLoot | HighValues;
constexpr unsigned kStashLootLimit = 512;

struct Options {
    bool enabled = false;
    int port = 8765;
    int hz = 8;
    unsigned release = 0;
    unsigned stashLootMask = ~0u;
    std::string publicHost;

    bool shares(Release value) const { return (release & value) != 0; }
};

void configure(const Options& options, const std::string& directory);
void publish(const std::string& mapId, radar::Frame frame);
void clear();
void shutdown();
std::string status();

}
