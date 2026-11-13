#pragma once

#include "bottie/core.hpp"
#include "bottie/preferences.hpp"
#include "bottie/detail.hpp"
#include "bottie/feedback.hpp"
#include <atomic>

namespace bottie {
constexpr uint64_t touch_stale_ms = 200, long_press_ms = 650;
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
    uint8_t dim_mode{2};
    SaveStatus save_status{SaveStatus::defaults};
    bool detail_open{}, detail_ready{}, seek_preview{};
    uint8_t detail_field{}, detail_page{}, detail_count{}, detail_flags{};
    uint32_t detail_sequence{}, seek_position{};
    int8_t pressed_button{-1};
    FeedbackPhase feedback{FeedbackPhase::none};
    ControlError feedback_error{ControlError::none};
};
struct UiAction {
    bool control{}, brightness_changed{}, preferences_released{}, wake{}, refresh{};
    Intent intent{};
};
struct Rect {
    int x{}, y{}, width{}, height{};
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};
struct SettingsLayout {
    Rect panel, brightness, volume, resume, close, dim_timeout;
};
SettingsLayout settings_layout(uint16_t width, uint16_t height, bool volume = false);
struct DetailLayout { Rect panel, body, previous, next, close; };
DetailLayout detail_layout(uint16_t width,uint16_t height);
Rect seek_rect(uint16_t width,uint16_t height);
uint16_t brightness_duty(unsigned percent);

class Interaction {
public:
    SettingsView settings{};
    void load_preferences(Preferences preferences, uint64_t now);
    uint8_t applied_brightness() const { return power_.applied(); }
    bool dimmed() const { return power_.dimmed(); }
    bool tick_power(uint64_t now) { return power_.tick(now); }
    uint32_t power_wait_ms(uint64_t now) const { return power_.wait_ms(now); }
    bool refresh_pending() const { return wake_refresh_; }
    const DetailSelection& detail_selection() const { return detail_; }
    void outcome(const ControlOutcome& result,uint64_t now) {
        feedback_.result(result,now); settings.feedback=feedback_.phase(); settings.feedback_error=feedback_.error();
    }
    void queued(Intent& intent,const Model& model,uint64_t now);
    void refresh(const Model& model, uint64_t now_ms);
    void expire(uint64_t now_ms);
    void barrier(uint64_t now_ms) { cancel(); cutoff_set_ = true; discard_through_ms_ = now_ms; }
    UiAction update(const PointerSample& sample, const Model& model, uint64_t now_ms,
                    uint16_t width = 240, uint16_t height = 296);
private:
    enum class Hit { none, shuffle, previous, play_pause, next, repeat, brightness, volume, resume, close, dim_timeout,
                     seek, title, artist, album, context, detail_previous, detail_next, detail_close };
    Hit hit(int x, int y, const Model& model, uint64_t now, uint16_t width, uint16_t height) const;
    void cancel();
    UiAction finish(UiAction action,const Model& model,uint64_t now);
    bool pressed_{}, suppressed_{}, moved_{}, held_{};
    uint16_t start_x_{}, start_y_{};
    uint64_t started_ms_{}, last_valid_ms_{}, last_sample_ms_{};
    Playback started_state_{Playback::unavailable};
    char started_track_[129]{};
    char started_target_[33]{}, volume_target_[33]{};
    char started_control_track_[33]{}, context_control_track_[33]{};
    uint64_t volume_updated_{};
    bool context_initialized_{}, context_fresh_{}, cutoff_set_{};
    uint64_t discard_through_ms_{};
    Playback context_state_{Playback::unavailable};
    char context_target_[33]{}, context_track_[129]{};
    Hit captured_{Hit::none};
    DisplayPower power_;
    bool wake_refresh_{};
    uint64_t wake_at_{};
    uint32_t sequence_{};
    DetailSelection detail_{};
    Feedback feedback_;
};

bool render_settings(const Model& model, uint64_t now_ms, Frame frame, const SettingsView& settings,
                     const DetailPage* detail = nullptr);
} // namespace bottie
