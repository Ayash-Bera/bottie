#pragma once

#include "bottie/core.hpp"

namespace bottie {
// The runtime owns synchronization; this policy is also exercised without a scheduler.
class ControlMailbox {
public:
    bool pending() const { return count_ || has_volume_; }
    bool put(const Intent& intent, uint64_t now) {
        Pending item{intent, now};
        if (intent.action == Action::volume) { volume_ = item; has_volume_ = true; return true; }
        if (count_ == buttons_.size()) return false;
        buttons_[(first_ + count_) % buttons_.size()] = item;
        ++count_;
        return true;
    }
    bool take(const Model& model, uint64_t now, Intent& intent) {
        if (has_volume_ && !valid(volume_, model, now)) has_volume_ = false;
        while (count_) {
            auto item = buttons_[first_];
            first_ = (first_ + 1) % buttons_.size(); --count_;
            if (valid(item, model, now)) { intent = item.intent; return true; }
        }
        if (has_volume_ && (now < last_volume_ms_ || now - last_volume_ms_ >= 1000)) {
            intent = volume_.intent; has_volume_ = false; last_volume_ms_ = now;
            return true;
        }
        return false;
    }
private:
    struct Pending { Intent intent{}; uint64_t at_ms{}; };
    static bool valid(const Pending& item, const Model& model, uint64_t now) {
        return now >= item.at_ms && now - item.at_ms < 2000 && model.allows(item.intent, now);
    }
    std::array<Pending, 4> buttons_{};
    Pending volume_{};
    size_t first_{}, count_{};
    bool has_volume_{};
    uint64_t last_volume_ms_{};
};
} // namespace bottie
