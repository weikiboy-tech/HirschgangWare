#pragma once
#include "keyboard_bitmap.h"
#include <cstddef>

namespace lan_keyboard {

// The producer owns the live keyboard bitmap. The consumer sends every change
// in order, including a press and release between two heartbeat frames.
class Outbox {
    using Bitmap = dma_keyboard::Bitmap;
    struct Change { Bitmap bits; uint64_t at; };
    std::array<Change, 64> pending_{};
    Bitmap current_{};
    uint64_t sampledAt_ = 0;
    std::size_t head_ = 0, count_ = 0;
    bool overflow_ = false;

    bool fresh(uint64_t now, uint64_t timeout) const {
        return sampledAt_ && now - sampledAt_ < timeout;
    }

public:
    bool publish(const Bitmap& bits, uint64_t now) {
        sampledAt_ = now;
        if (bits == current_) return false;
        current_ = bits;
        if (count_ == pending_.size()) overflow_ = true;
        else pending_[(head_ + count_++) % pending_.size()] = {bits, now};
        return true;
    }

    bool pending() const { return count_ || overflow_; }

    bool begin(uint64_t now, uint64_t timeout, Bitmap& bits) {
        head_ = count_ = 0;
        overflow_ = false;
        if (!fresh(now, timeout)) return false;
        bits = current_;
        return true;
    }

    bool next(uint64_t now, uint64_t timeout, Bitmap& bits) {
        if (!fresh(now, timeout) || overflow_) return false;
        if (!count_) { bits = current_; return true; }
        const Change& change = pending_[head_];
        if (now - change.at >= timeout) return false;
        bits = change.bits;
        head_ = (head_ + 1) % pending_.size();
        --count_;
        return true;
    }
};
}  // namespace lan_keyboard
