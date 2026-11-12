#pragma once

#include "bottie/core.hpp"
#include <atomic>

namespace bottie {
constexpr uint64_t touch_stale_ms = 200, long_press_ms = 650;
constexpr uint8_t minimum_brightness = 10;
struct PointerSample {
    bool valid{}, down{}, cancel{};
    uint16_t x{}, y{};
    uint64_t at_ms{};
};

bool decode_cst810(const uint8_t* report, size_t length, uint64_t at_ms, PointerSample& sample);

class PointerQueue {
public:
    bool push(const PointerSample& sample);
    bool pop(PointerSample& sample, uint64_t now_ms);
private:
    static constexpr uint32_t capacity = 32;
    std::array<PointerSample, capacity> samples_{};
    std::atomic<uint32_t> write_{}, read_{};
    std::atomic<bool> overflow_{};
};

struct SettingsView {
    bool open{};
    uint8_t brightness{100};
    int16_t volume{-1};
    bool volume_available{};
};
struct UiAction {
    bool planet{}, control{}, brightness_changed{};
    Intent intent{};
};
struct Rect {
    int x{}, y{}, width{}, height{};
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};
struct SettingsLayout {
    Rect panel, brightness, volume, resume, close;
};
SettingsLayout settings_layout(uint16_t width, uint16_t height, bool volume = false);
uint16_t brightness_duty(unsigned percent);

class Interaction {
public:
    SettingsView settings{};
    void refresh(const Model& model, uint64_t now_ms);
    void expire(uint64_t now_ms);
    UiAction update(const PointerSample& sample, const Model& model, uint64_t now_ms,
                    uint16_t width = 240, uint16_t height = 296);
private:
    enum class Hit { none, planet, shuffle, previous, play_pause, next, repeat, brightness, volume, resume, close };
    Hit hit(int x, int y, const Model& model, uint64_t now, uint16_t width, uint16_t height) const;
    void cancel();
    bool pressed_{}, suppressed_{}, moved_{}, held_{};
    uint16_t start_x_{}, start_y_{};
    uint64_t started_ms_{}, last_valid_ms_{}, last_sample_ms_{};
    Playback started_state_{Playback::unavailable};
    char started_track_[129]{};
    char started_target_[33]{}, volume_target_[33]{};
    uint64_t volume_updated_{};
    bool context_initialized_{}, context_fresh_{}, cutoff_set_{};
    uint64_t discard_through_ms_{};
    Playback context_state_{Playback::unavailable};
    char context_target_[33]{}, context_track_[129]{};
    Hit captured_{Hit::none};
};

bool render_settings(const Model& model, uint64_t now_ms, Frame frame, const SettingsView& settings);
} // namespace bottie
