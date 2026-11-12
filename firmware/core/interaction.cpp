#include "bottie/interaction.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bottie {
bool decode_cst810(const uint8_t* bytes, size_t length, uint64_t at, PointerSample& sample) {
    sample = {}; sample.at_ms = at;
    if (!bytes || length != 5 || (bytes[0] & 15) > 1) return false;
    if (!(bytes[0] & 15)) { sample.valid = true; return true; }
    const unsigned x = ((bytes[1] & 15) << 8) | bytes[2];
    const unsigned y = ((bytes[3] & 15) << 8) | bytes[4];
    if (x >= 296 || y >= 240) return false;
    sample.valid = sample.down = true;
    // Invert the vendor landscape MY/MV transform; its 24 hidden rows are already excluded.
    sample.x = static_cast<uint16_t>(y);
    sample.y = static_cast<uint16_t>(295 - x);
    return true;
}

bool PointerQueue::push(const PointerSample& sample) {
    const uint32_t write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) == capacity) {
        overflow_.store(true, std::memory_order_release);
        return false;
    }
    samples_[write % capacity] = sample;
    write_.store(write + 1, std::memory_order_release);
    return true;
}
bool PointerQueue::pop(PointerSample& sample, uint64_t now) {
    if (overflow_.exchange(false, std::memory_order_acq_rel)) {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
        sample = {}; sample.cancel = true; sample.at_ms = now;
        return true;
    }
    const uint32_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return false;
    sample = samples_[read % capacity];
    read_.store(read + 1, std::memory_order_release);
    return true;
}

SettingsLayout settings_layout(uint16_t width, uint16_t height, bool volume) {
    auto rect = [=](int x, int y, int w, int h) {
        return Rect{x * width / 240, y * height / 296, w * width / 240, h * height / 296};
    };
    return {rect(12, 46, 216, volume ? 240 : 204), rect(30, 112, 180, 32),
            rect(30, 162, 180, 32), rect(30, volume ? 208 : 168, 180, 30), rect(30, volume ? 246 : 208, 180, 30)};
}
uint16_t brightness_duty(unsigned percent) {
    return static_cast<uint16_t>((1023 * std::clamp(percent, unsigned(minimum_brightness), 100U) + 50) / 100);
}

Interaction::Hit Interaction::hit(int x, int y, const Model& model, uint64_t now, uint16_t width, uint16_t height) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return Hit::none;
    if (settings.open) {
        auto layout = settings_layout(width, height, settings.volume_available);
        if (layout.brightness.contains(x, y)) return Hit::brightness;
        if (settings.volume_available && layout.volume.contains(x, y)) return Hit::volume;
        if (layout.resume.contains(x, y)) return Hit::resume;
        if (layout.close.contains(x, y)) return Hit::close;
        return Hit::none;
    }
    if (model.effective_state(now) != Playback::playing) return Hit::planet;
    const int scale = width >= 200 ? 2 : 1;
    if (y < height - 25 * scale || y >= height - 3 * scale) return Hit::none;
    return static_cast<Hit>(static_cast<int>(Hit::shuffle) + std::min(4, x * 5 / width));
}
void Interaction::cancel() { pressed_ = false; suppressed_ = true; captured_ = Hit::none; }
void Interaction::expire(uint64_t now) {
    if (pressed_ && (now < last_valid_ms_ || now - last_valid_ms_ >= touch_stale_ms)) cancel();
}

void Interaction::refresh(const Model& model, uint64_t now) {
    Intent volume;
    const bool available = model.volume_intent(0, now, volume);
    const auto state = model.effective_state(now);
    const bool fresh = state != Playback::unavailable && model.snapshot.control_target[0];
    const bool context_changed = context_fresh_ != fresh || context_state_ != state ||
        settings.volume_available != available ||
        std::strcmp(context_target_, model.snapshot.control_target) != 0 ||
        std::strcmp(context_track_, model.snapshot.track_id) != 0;
    if (context_initialized_ && context_changed) {
        cancel();
        cutoff_set_ = true;
        discard_through_ms_ = now;
    }
    context_initialized_ = true;
    context_fresh_ = fresh;
    context_state_ = state;
    std::memcpy(context_target_, model.snapshot.control_target, sizeof context_target_);
    std::memcpy(context_track_, model.snapshot.track_id, sizeof context_track_);
    const bool changed = std::strcmp(volume_target_, model.snapshot.control_target) != 0;
    settings.volume_available = available;
    if ((!available || changed) && captured_ == Hit::volume) cancel();
    if (!available) settings.volume = -1;
    else if (changed || settings.volume < 0 || (!pressed_ && volume_updated_ != model.snapshot.updated_at_ms))
        settings.volume = model.snapshot.volume_percent;
    volume_updated_ = model.snapshot.updated_at_ms;
    std::memcpy(volume_target_, model.snapshot.control_target, sizeof volume_target_);
}

UiAction Interaction::update(const PointerSample& sample, const Model& model, uint64_t now, uint16_t width, uint16_t height) {
    UiAction result;
    // A pre-refresh release must not rearm another queued press for the new target.
    if (cutoff_set_ && sample.at_ms <= discard_through_ms_) return result;
    if (sample.cancel || sample.at_ms < last_sample_ms_ || sample.at_ms > now || now - sample.at_ms > 300) {
        cancel(); return result;
    }
    last_sample_ms_ = sample.at_ms;
    if (!sample.valid || sample.x >= width || sample.y >= height) {
        if (pressed_ && sample.at_ms - last_valid_ms_ >= touch_stale_ms) cancel();
        return result;
    }
    if (pressed_ && sample.at_ms - last_valid_ms_ >= touch_stale_ms) cancel();
    last_valid_ms_ = sample.at_ms;
    if (!sample.down) {
        if (pressed_ && !suppressed_ && !moved_ && !held_ && sample.at_ms - started_ms_ >= 20 &&
            sample.at_ms - started_ms_ < long_press_ms) {
            const bool same = started_state_ == model.effective_state(now) &&
                              std::strcmp(started_track_, model.snapshot.track_id) == 0 &&
                              std::strcmp(started_target_, model.snapshot.control_target) == 0;
            if (captured_ == Hit::close) settings.open = false;
            else if (captured_ == Hit::resume && same) {
                result.control = model.resume_intent(now, result.intent);
            } else if (captured_ == Hit::planet && same) result.planet = true;
            else if (same && captured_ >= Hit::shuffle && captured_ <= Hit::repeat)
                result.control = model.snapshot.control_target[0] &&
                    model.intent(static_cast<Button>(static_cast<int>(captured_) - static_cast<int>(Hit::shuffle)), now, result.intent);
        }
        pressed_ = suppressed_ = moved_ = held_ = false;
        captured_ = Hit::none;
        return result;
    }
    if (suppressed_) return result;
    if (!pressed_) {
        pressed_ = true; started_ms_ = sample.at_ms;
        start_x_ = sample.x; start_y_ = sample.y;
        captured_ = hit(sample.x, sample.y, model, now, width, height);
        started_state_ = model.effective_state(now);
        std::memcpy(started_track_, model.snapshot.track_id, sizeof started_track_);
        std::memcpy(started_target_, model.snapshot.control_target, sizeof started_target_);
    }
    if (captured_ == Hit::brightness) {
        const auto bar = settings_layout(width, height).brightness;
        const int value = minimum_brightness + (std::clamp(int(sample.x) - bar.x, 0, bar.width - 1) * (100 - minimum_brightness) + (bar.width - 1) / 2) / (bar.width - 1);
        result.brightness_changed = settings.brightness != value;
        settings.brightness = static_cast<uint8_t>(value);
        moved_ = true;
        return result;
    }
    if (captured_ == Hit::volume) {
        if (std::strcmp(started_target_, model.snapshot.control_target) != 0) { cancel(); return result; }
        const auto bar = settings_layout(width, height, true).volume;
        const auto value = static_cast<uint8_t>((std::clamp(int(sample.x) - bar.x, 0, bar.width - 1) * 100 + (bar.width - 1) / 2) / (bar.width - 1));
        if (settings.volume != value) result.control = model.volume_intent(value, now, result.intent);
        if (result.control) settings.volume = value;
        moved_ = true;
        return result;
    }
    if (std::abs(int(sample.x) - start_x_) > 12 || std::abs(int(sample.y) - start_y_) > 12) moved_ = true;
    if (hit(sample.x, sample.y, model, now, width, height) != captured_) moved_ = true;
    if (!moved_ && !held_ && sample.at_ms - started_ms_ >= long_press_ms) {
        settings.open = true; held_ = true; captured_ = Hit::none;
    }
    return result;
}
} // namespace bottie
