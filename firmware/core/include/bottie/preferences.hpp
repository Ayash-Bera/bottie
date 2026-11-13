#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bottie {
constexpr uint8_t minimum_brightness = 10;
constexpr size_t preference_bytes = 12;
struct Preferences {
    uint8_t brightness{100}, dim_mode{2};
    bool operator==(const Preferences& other) const {
        return brightness == other.brightness && dim_mode == other.dim_mode;
    }
    bool valid() const { return brightness >= minimum_brightness && brightness <= 100 && dim_mode <= 4; }
};
enum class StoreResult { ok, missing, corrupt, unknown_version, unavailable, full, write_error };
enum class SaveStatus { saved, defaults, pending, unsaved, read_only };
using PreferenceBytes = std::array<uint8_t, preference_bytes>;
PreferenceBytes encode_preferences(Preferences value);
StoreResult decode_preferences(const uint8_t* bytes, size_t size, Preferences& value);
uint32_t dim_timeout_ms(uint8_t mode);

struct PreferenceWrite { Preferences value{}; uint32_t sequence{}; };
class PreferenceState {
public:
    void loaded(StoreResult result, const uint8_t* bytes = nullptr, size_t size = 0);
    void change(Preferences value);
    void release(uint64_t now);
    bool take(uint64_t now, PreferenceWrite& write);
    void complete(const PreferenceWrite& write, StoreResult result);
    Preferences value() const { return value_; }
    SaveStatus status() const { return status_; }
    bool writable() const { return writable_; }
private:
    Preferences value_{}, saved_{};
    PreferenceWrite inflight_{};
    uint64_t due_{}, last_write_{};
    uint32_t sequence_{};
    SaveStatus status_{SaveStatus::defaults};
    bool writable_{true}, dirty_{}, released_{}, busy_{}, wrote_{};
};
class PreferenceBackend {
public:
    virtual ~PreferenceBackend() = default;
    virtual StoreResult initialize() = 0;
    virtual StoreResult read(PreferenceBytes& bytes,size_t& size) = 0;
    virtual StoreResult write(Preferences value) = 0;
};
void load_preferences(PreferenceBackend& backend,PreferenceState& state);

class DisplayPower {
public:
    void configure(Preferences preferences, uint64_t now);
    bool activity(uint64_t at, bool down);
    bool tick(uint64_t now);
    uint32_t wait_ms(uint64_t now) const;
    uint8_t applied() const { return applied_; }
    bool dimmed() const { return dimmed_; }
    bool swallowing() const { return swallowing_; }
private:
    Preferences preferences_{};
    uint8_t applied_{100};
    uint64_t active_at_{}, fade_at_{};
    bool dimmed_{}, swallowing_{};
};
} // namespace bottie
