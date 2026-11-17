#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace radar_event {

enum class Kind { Airdrop, HelicopterCrash, LootCluster };

inline const char* label(Kind kind) {
    switch (kind) {
        case Kind::Airdrop: return "Airdrop";
        case Kind::HelicopterCrash: return "Helikopterabsturz";
        case Kind::LootCluster: return u8"Loot-H\u00e4ufung";
    }
    return "";
}

inline const char* key(Kind kind) {
    switch (kind) {
        case Kind::Airdrop: return "airdrop";
        case Kind::HelicopterCrash: return "helicopter-crash";
        case Kind::LootCluster: return "loot-cluster";
    }
    return "";
}

inline std::optional<Kind> knownType(std::string_view type) {
    constexpr std::string_view containers[] = {
        "ExpansionAirdropContainer", "ExpansionAirdropContainer_Grey", "ExpansionAirdropContainer_Blue",
        "ExpansionAirdropContainer_Olive", "ExpansionAirdropContainer_Medical", "ExpansionAirdropContainer_Military",
        "ExpansionAirdropContainer_Military_GreenCamo", "ExpansionAirdropContainer_Military_MarineCamo",
        "ExpansionAirdropContainer_Military_OliveCamo", "ExpansionAirdropContainer_Military_OliveCamo2",
        "ExpansionAirdropContainer_Military_WinterCamo", "ExpansionAirdropContainer_Basebuilding",
    };
    for (const auto container : containers)
        if (type == container) return Kind::Airdrop;
    if (type == "Wreck_Mi8_Crashed" || type == "Wreck_UH1Y") return Kind::HelicopterCrash;
    return {};
}

struct Point { float x = 0, z = 0; };
struct Marker { Point pos; Kind kind; };
struct Object {
    uint64_t id;
    std::optional<Point> pos;
    std::optional<Kind> event;
};

inline bool valid(Point p) {
    return p.x >= 0 && p.z >= 0 && p.x <= 100000 && p.z <= 100000;
}

inline bool withinRadius(Point a, Point b, float radius) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz <= radius * radius;
}

inline constexpr size_t kMaxMarkers = 128;

class Tracker {
public:
    std::vector<Marker> update(uint64_t world, uint64_t mapRevision, uint64_t now,
                               const std::vector<Object>& objects, std::optional<Point> observer, bool complete) {
        if (world != world_ || mapRevision != mapRevision_) {
            *this = Tracker{};
            world_ = world;
            mapRevision_ = mapRevision;
        }
        if (!world || (last_ && now <= last_)) return markers(now);
        for (auto it = confirmed_.begin(); it != confirmed_.end();) {
            if (now - it->second.at > 3000) it = confirmed_.erase(it);
            else ++it;
        }
        for (const auto& obj : objects) {
            if (obj.id && obj.event && obj.pos && valid(*obj.pos)) {
                confirmed_[obj.id] = {{*obj.pos, *obj.event}, now};
                births_.erase(obj.id);
            }
        }
        if (!complete) return markers(now);
        const bool consecutive = last_ && now - last_ <= 3000;
        const bool sameArea = consecutive && observer && observer_ && withinRadius(*observer, *observer_, 25.f);
        last_ = now;
        if (!sameArea) births_.clear();

        // Observe identities even when their positions are temporarily unreadable.
        for (const auto& obj : objects) {
            if (!obj.id) continue;
            const auto [seen, isNew] = seen_.try_emplace(obj.id, now);
            if (!isNew && seen->second == now) continue;
            seen->second = now;
            if (!obj.pos || !valid(*obj.pos)) continue;
            if (!confirmed_.count(obj.id) && !obj.event && isNew && sameArea &&
                withinRadius(*obj.pos, *observer_, 200.f) && withinRadius(*obj.pos, *observer, 200.f)) {
                births_[obj.id] = {*obj.pos, now};
            }
        }
        observer_ = observer;

        for (auto it = seen_.begin(); it != seen_.end();) {
            if (now - it->second > 120000) it = seen_.erase(it);
            else ++it;
        }
        for (auto it = births_.begin(); it != births_.end();) {
            if (now - it->second.at > 8000 || seen_.at(it->first) != now || overlapsConfirmed(it->second.pos, now))
                it = births_.erase(it);
            else ++it;
        }
        for (auto it = clusters_.begin(); it != clusters_.end();) {
            const auto remaining = std::count_if(it->members.begin(), it->members.end(), [&](uint64_t id) {
                const auto found = seen_.find(id);
                return found != seen_.end() && now - found->second <= 3000;
            });
            if (remaining < 4 || now - it->at > 90000 || overlapsConfirmed(it->pos, now)) it = clusters_.erase(it);
            else ++it;
        }
        findClusters(now);
        return markers(now);
    }

private:
    struct TimedMarker { Marker marker; uint64_t at; };
    struct Birth { Point pos; uint64_t at; };
    struct Cluster { Point pos; uint64_t at; std::vector<uint64_t> members; };

    bool overlapsConfirmed(Point p, uint64_t now) const {
        for (const auto& [id, event] : confirmed_)
            if (now - event.at <= 3000 && withinRadius(p, event.marker.pos, 35.f)) return true;
        return false;
    }

    void findClusters(uint64_t now) {
        using Cell = std::pair<int, int>;
        std::map<Cell, std::vector<uint64_t>> bins;
        for (const auto& [id, item] : births_)
            bins[{int(item.pos.x / 25.f), int(item.pos.z / 25.f)}].push_back(id);
        for (const auto& [cell, ids] : bins) {
            const uint64_t seed = *std::min_element(ids.begin(), ids.end());
            const Point origin = births_.at(seed).pos;
            std::vector<uint64_t> members;
            double x = 0, z = 0;
            for (int dx = -1; dx <= 1; ++dx) for (int dz = -1; dz <= 1; ++dz) {
                const auto bin = bins.find({cell.first + dx, cell.second + dz});
                if (bin == bins.end()) continue;
                for (const uint64_t id : bin->second) {
                    const Point p = births_.at(id).pos;
                    if (!withinRadius(origin, p, 25.f)) continue;
                    members.push_back(id);
                    x += p.x;
                    z += p.z;
                }
            }
            if (members.size() < 8) continue;
            const Point center{float(x / members.size()), float(z / members.size())};
            if (overlapsConfirmed(center, now)) continue;
            auto existing = std::find_if(clusters_.begin(), clusters_.end(),
                [&](const Cluster& c) { return withinRadius(c.pos, center, 35.f); });
            if (existing != clusters_.end()) {
                if (members.size() > existing->members.size()) {
                    existing->pos = center;
                    existing->members = std::move(members);
                }
            } else if (clusters_.size() < kMaxMarkers) {
                clusters_.push_back({center, now, std::move(members)});
            }
        }
    }

    std::vector<Marker> markers(uint64_t now) const {
        std::vector<Marker> result;
        for (const auto& [id, event] : confirmed_) {
            if (now - event.at <= 3000) result.push_back(event.marker);
            if (result.size() == kMaxMarkers) break;
        }
        for (const auto& cluster : clusters_) {
            if (result.size() == kMaxMarkers) break;
            if (now - cluster.at <= 90000 && !overlapsConfirmed(cluster.pos, now))
                result.push_back({cluster.pos, Kind::LootCluster});
        }
        return result;
    }

    uint64_t world_ = 0, mapRevision_ = 0, last_ = 0;
    std::optional<Point> observer_;
    std::unordered_map<uint64_t, uint64_t> seen_;
    std::map<uint64_t, TimedMarker> confirmed_;
    std::unordered_map<uint64_t, Birth> births_;
    std::vector<Cluster> clusters_;
};

}
