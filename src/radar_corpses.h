#pragma once

#include "radar_events.h"

#include <memory>

namespace radar_corpse {

inline constexpr uint64_t kLifetimeMs = 7 * 60 * 1000;
inline constexpr uint64_t kObservationGapMs = 3000;
inline constexpr size_t kMaxMarkers = 128;

struct Marker {
    radar_event::Point pos;
    uint32_t id;
    uint64_t expiresAt;
};
using Markers = std::vector<Marker>;

class Tracker {
public:
    void begin(uint64_t world, uint64_t revision, uint64_t now) {
        if (world != world_ || revision != revision_) {
            clear();
            world_ = world;
            revision_ = revision;
        }
        if (!startedPrune_ || now - lastPrune_ >= 1000) {
            startedPrune_ = true;
            lastPrune_ = now;
            for (auto it = players_.begin(); it != players_.end();) {
                if (now - it->second.seenAt > kObservationGapMs) it = players_.erase(it);
                else ++it;
            }
        }
        while (!markers_.empty() && markers_.front().expiresAt <= now) {
            markers_.erase(markers_.begin());
            snapshot_.reset();
        }
    }

    // Call only with a freshly read death flag for a classified player.
    // A new dead pointer has no known time of death.
    bool observe(uint64_t player, bool dead, std::optional<radar_event::Point> pos, uint64_t now) {
        if (!dead) {
            players_[player] = {now, 0, State::Alive};
            return false;
        }
        const auto it = players_.find(player);
        if (it == players_.end() || now - it->second.seenAt > kObservationGapMs) return false;
        auto& state = it->second;
        state.seenAt = now;
        if (state.phase == State::Alive) {
            state.deathAt = now;
            state.phase = State::AwaitingPosition;
        }
        if (state.phase != State::AwaitingPosition) return false;
        if (now - state.deathAt > kObservationGapMs) {
            state.phase = State::Recorded;
            return false;
        }
        if (!pos || !radar_event::valid(*pos)) return true;
        if (markers_.size() == kMaxMarkers) markers_.erase(markers_.begin());
        markers_.push_back({*pos, ++nextId_, state.deathAt + kLifetimeMs});
        // A position read can arrive after another player's marker was recorded.
        std::sort(markers_.begin(), markers_.end(), [](const Marker& a, const Marker& b) {
            return a.expiresAt < b.expiresAt;
        });
        snapshot_.reset();
        state.phase = State::Recorded;
        return false;
    }

    std::shared_ptr<const Markers> snapshot() {
        if (!snapshot_ && !markers_.empty()) snapshot_ = std::make_shared<const Markers>(markers_);
        return snapshot_;
    }

    void clear() {
        players_.clear();
        markers_.clear();
        snapshot_.reset();
        startedPrune_ = false;
    }

private:
    enum class State { Alive, AwaitingPosition, Recorded };
    struct Player { uint64_t seenAt, deathAt; State phase; };

    std::unordered_map<uint64_t, Player> players_;
    Markers markers_;
    std::shared_ptr<const Markers> snapshot_;
    uint64_t world_ = 0, revision_ = 0, lastPrune_ = 0;
    uint32_t nextId_ = 0;
    bool startedPrune_ = false;
};

} // namespace radar_corpse
