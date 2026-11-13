#include "bottie/preferences.hpp"

#include <algorithm>
#include <cstring>

namespace bottie {
namespace {
uint32_t crc(const uint8_t* data, size_t size) {
    uint32_t value = ~0U;
    for (size_t n = 0; n < size; ++n) {
        value ^= data[n];
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
}
PreferenceBytes encode_preferences(Preferences value) {
    PreferenceBytes bytes{'B','T','P','R',1,value.brightness,value.dim_mode,0};
    const auto checksum = crc(bytes.data(), 8);
    for (unsigned n = 0; n < 4; ++n) bytes[8+n] = static_cast<uint8_t>(checksum >> (n*8));
    return bytes;
}
StoreResult decode_preferences(const uint8_t* bytes, size_t size, Preferences& value) {
    value = {};
    if (!bytes || size < 5 || std::memcmp(bytes,"BTPR",4)) return StoreResult::corrupt;
    if (bytes[4] != 1) return StoreResult::unknown_version;
    if (size != preference_bytes || bytes[7]) return StoreResult::corrupt;
    uint32_t stored = 0;
    for (unsigned n = 0; n < 4; ++n) stored |= uint32_t(bytes[8+n]) << (n*8);
    Preferences candidate{bytes[5],bytes[6]};
    if (!candidate.valid() || stored != crc(bytes,8)) return StoreResult::corrupt;
    value = candidate;
    return StoreResult::ok;
}
uint32_t dim_timeout_ms(uint8_t mode) {
    constexpr uint32_t values[]{0,30000,60000,120000,300000};
    return mode <= 4 ? values[mode] : values[2];
}
void PreferenceState::loaded(StoreResult result, const uint8_t* bytes, size_t size) {
    *this = {};
    if (result == StoreResult::ok) result = decode_preferences(bytes,size,value_);
    committed_ = result == StoreResult::ok;
    saved_ = value_;
    writable_ = result == StoreResult::ok || result == StoreResult::missing || result == StoreResult::corrupt;
    status_ = result == StoreResult::ok ? SaveStatus::saved : result == StoreResult::missing ? SaveStatus::defaults :
        writable_ ? SaveStatus::unsaved : SaveStatus::read_only;
}
void PreferenceState::change(Preferences value) {
    if (!value.valid() || value == value_) return;
    value_ = value; ++sequence_;
    dirty_ = busy_ || !committed_ || !(value_ == saved_);
    released_ = false;
    status_ = !writable_ ? SaveStatus::read_only : dirty_ ? SaveStatus::pending : SaveStatus::saved;
}
void PreferenceState::release(uint64_t now) {
    if (!dirty_ || !writable_) return;
    released_ = true; due_ = now + 500;
}
bool PreferenceState::take(uint64_t now, PreferenceWrite& write) {
    if (!writable_ || !dirty_ || !released_ || busy_ || now < due_ ||
        (wrote_ && (now < last_write_ || now-last_write_ < 2000))) return false;
    write = inflight_ = {value_,sequence_}; busy_ = true; released_ = false;
    wrote_ = true; last_write_ = now;
    return true;
}
void PreferenceState::complete(const PreferenceWrite& write, StoreResult result) {
    if (!busy_ || write.sequence != inflight_.sequence || !(write.value == inflight_.value)) return;
    busy_ = false;
    if (result == StoreResult::ok) { saved_ = write.value; committed_ = true; }
    dirty_ = !committed_ || !(value_ == saved_);
    if (write.sequence == sequence_) status_ = result == StoreResult::ok ? SaveStatus::saved : SaveStatus::unsaved;
    else status_ = dirty_ ? SaveStatus::pending : SaveStatus::saved;
    // A failed write gets another attempt only after a new deliberate release.
    if (result != StoreResult::ok && write.sequence == sequence_) released_ = false;
}
void load_preferences(PreferenceBackend& backend,PreferenceState& state) {
    const auto initialized=backend.initialize();
    if(initialized!=StoreResult::ok) { state.loaded(initialized); return; }
    PreferenceBytes bytes{}; size_t size=bytes.size();
    const auto result=backend.read(bytes,size);
    state.loaded(result,bytes.data(),size);
}
void DisplayPower::configure(Preferences preferences, uint64_t now) {
    if (!preferences.valid()) return;
    preferences_ = preferences; applied_ = preferences.brightness;
    active_at_ = fade_at_ = now; dimmed_ = false;
}
bool DisplayPower::activity(uint64_t at, bool down) {
    if (at < active_at_) return false;
    if (down) {
        active_at_ = at;
        if (dimmed_) {
            applied_ = preferences_.brightness; dimmed_ = false; swallowing_ = true;
            return true;
        }
    } else if (swallowing_) {
        active_at_ = at; swallowing_ = false;
    }
    return false;
}
bool DisplayPower::tick(uint64_t now) {
    const auto timeout = dim_timeout_ms(preferences_.dim_mode);
    if (!timeout || now < active_at_ || now-active_at_ < timeout) return false;
    if (!dimmed_) { dimmed_ = true; fade_at_ = now; }
    const auto target = std::min<uint8_t>(preferences_.brightness,20);
    if (now < fade_at_ || applied_ <= target) return false;
    const auto steps = (now-fade_at_)/20;
    const auto next = static_cast<uint8_t>(std::max<uint64_t>(target,applied_-std::min<uint64_t>(applied_,steps)));
    if (next == applied_) return false;
    applied_ = next; fade_at_ = now; return true;
}
uint32_t DisplayPower::wait_ms(uint64_t now) const {
    if (dimmed_ && applied_ > std::min<uint8_t>(preferences_.brightness,20)) return 20;
    const auto timeout = dim_timeout_ms(preferences_.dim_mode);
    if (!timeout || dimmed_) return 1000;
    if (now < active_at_ || now-active_at_ >= timeout) return 1;
    return static_cast<uint32_t>(std::min<uint64_t>(1000,timeout-(now-active_at_)));
}
} // namespace bottie
