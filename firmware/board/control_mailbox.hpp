#pragma once

#include "bottie/core.hpp"

namespace bottie {
// The runtime owns synchronization; this policy is also exercised without a scheduler.
class ControlMailbox {
public:
    bool pending() const { return count_ || has_volume_ || has_seek_; }
    bool put(const Intent& intent, uint64_t now) {
        Pending item{intent, now};
        if(intent.action==Action::seek && intent.release_known) {
            if(now<intent.released_ms || now-intent.released_ms>=1000) return false;
            item.at_ms=intent.released_ms;
        }
        if (intent.action == Action::volume) { volume_ = item; has_volume_ = true; return true; }
        if (intent.action == Action::seek) { seek_ = item; has_seek_ = true; return true; }
        if (count_ == buttons_.size()) return false;
        buttons_[(first_ + count_) % buttons_.size()] = item;
        ++count_;
        return true;
    }
    bool take(const Model& model, uint64_t now, Intent& intent) {
        if (has_volume_ && !valid(volume_, model, now)) has_volume_ = false;
        if (has_seek_ && !valid(seek_, model, now)) has_seek_ = false;
        while (count_) {
            auto item = buttons_[first_];
            first_ = (first_ + 1) % buttons_.size(); --count_;
            if (valid(item, model, now)) { intent = item.intent; return true; }
        }
        if (has_seek_ && (!sent_seek_ || (now>=last_seek_ms_ && now-last_seek_ms_>=1000))) {
            intent=seek_.intent; has_seek_=false; sent_seek_=true; last_seek_ms_=now; return true;
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
        return now >= item.at_ms && now - item.at_ms < (item.intent.action==Action::seek ? 1000U : 2000U) && model.allows(item.intent, now);
    }
    std::array<Pending, 4> buttons_{};
    Pending volume_{}, seek_{};
    size_t first_{}, count_{};
    bool has_volume_{},has_seek_{},sent_seek_{};
    uint64_t last_volume_ms_{},last_seek_ms_{};
};
} // namespace bottie
