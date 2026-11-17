#pragma once

#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>

namespace player_health {

inline constexpr int unknown = -1;
inline constexpr uint64_t maxAgeMs = 2500;
inline constexpr size_t maxFeedBytes = 48 * 1024;

inline bool valid(int percent) { return percent >= 0 && percent <= 100; }
inline std::string label(int percent) {
    return valid(percent) ? "HP " + std::to_string(percent) : std::string{};
}

enum class Status { Off, Missing, StaleOrWrongSession, NoMatch, Ready };

inline const char* statusText(Status status) {
    switch (status) {
        case Status::Off: return "";
        case Status::Missing: return "Waiting for player-health.tsv";
        case Status::StaleOrWrongSession: return "HP feed expired or for a different session";
        case Status::NoMatch: return "HP feed available, waiting for matching players";
        case Status::Ready: return "HP feed active";
    }
    return "";
}

struct Sample {
    float x, z;
    int percent;
};

struct Snapshot {
    uint64_t at = 0;
    std::string map, server;
    std::unordered_map<uint32_t, Sample> players;

    bool current(uint64_t now, const std::string& worldMap, const std::string& host) const {
        return at && now >= at && now - at < maxAgeMs && !worldMap.empty() && !host.empty() &&
               map == worldMap && server == host;
    }

    int find(uint32_t id, float x, float z) const {
        const auto it = players.find(id);
        if (it == players.end()) return unknown;
        const float dx = x - it->second.x, dz = z - it->second.z;
        return dx * dx + dz * dz <= 50.f * 50.f ? it->second.percent : unknown;
    }
};

// The authorised producer supplies real current/max HP and the session peer ID
// for each player. Reject the complete update if any row is ambiguous.
inline Snapshot parse(const std::string& input) {
    Snapshot result;
    if (input.empty() || input.size() >= maxFeedBytes) return {};
    std::istringstream rows(input);
    std::string line, header;
    if (!std::getline(rows, line)) return {};
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream fields(line);
    if (!(fields >> header >> result.at >> result.map) || header != "HIRSCHGANG-HP-1" || !result.at) return {};
    std::getline(fields >> std::ws, result.server);
    if (result.server.empty()) return {};
    while (std::getline(rows, line)) {
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        int64_t id;
        float x, z, hp, maxHp;
        std::string extra;
        if (!(row >> id >> x >> z >> hp >> maxHp) || (row >> extra) ||
            id <= 0 || id > UINT32_MAX || !(x >= 0 && x <= 100000 && z >= 0 && z <= 100000) ||
            !std::isfinite(maxHp) || !std::isfinite(hp) || maxHp <= 0 || hp < 0 || hp > maxHp ||
            result.players.size() >= 512) return {};
        if (!result.players.emplace(uint32_t(id), Sample{x, z, int(std::lround(100.f * (hp / maxHp)))}).second)
            return {};
    }
    return result;
}

} // namespace player_health
