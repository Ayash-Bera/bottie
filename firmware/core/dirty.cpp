#include "bottie/dirty.hpp"

#include <algorithm>
#include <cstring>

namespace bottie {
void DirtyBands::rows(int top, int bottom) {
    for (int y = std::max(0, top) / band_rows * band_rows; y < std::min(int(height_), bottom); y += band_rows)
        pending_ |= 1U << (y / band_rows);
}
void DirtyBands::invalidate() { rows(0, height_); }

void DirtyBands::prepare(const Model& model, uint64_t now, const SettingsView& input_settings,
                         uint32_t artwork, uint32_t text, uint16_t width, uint16_t height) {
    if (!width || !height || width > max_frame_width || height > max_frame_height) return;
    const auto state = model.effective_state(now);
    const auto& value = model.snapshot;
    const int scale = width >= 200 ? 2 : 1;
    const int progress = value.duration_ms ? int(model.progress(now) * (width - 20 * scale) / value.duration_ms) : -1;
    const int minute = model.clock_minute(now);
    uint16_t controls = uint16_t(value.shuffle) | (uint16_t(value.repeat) << 1);
    Intent intent;
    auto settings = input_settings;
    settings.volume_available = model.volume_intent(0,now,intent);
    for (unsigned n = 0; n < 5; ++n) if (model.intent(static_cast<Button>(n), now, intent)) controls |= 1U << (n + 3);
    const bool resume = model.resume_intent(now, intent);
    static constexpr int directions[][2] = {{256,0},{237,98},{181,181},{98,237},{0,256},{-98,237},{-181,181},{-237,98},
        {-256,0},{-237,-98},{-181,-181},{-98,-237},{0,-256},{98,-237},{181,-181},{237,-98}};
    const unsigned phase = unsigned(now % 28000), sector = phase / 1750, fraction = phase % 1750;
    const int dx = directions[sector][0] + (directions[(sector+1)%16][0]-directions[sector][0]) * int(fraction) / 1750;
    const int dy = directions[sector][1] + (directions[(sector+1)%16][1]-directions[sector][1]) * int(fraction) / 1750;
    const bool dimensions = width != width_ || height != height_;
    width_ = width; height_ = height;
    if (!initialized_ || dimensions || state != state_ || settings.open != settings_.open) invalidate();
    if (state == Playback::playing) {
        if (artwork != artwork_ || std::strcmp(value.artwork_url, snapshot_.artwork_url)) invalidate();
        if (value.context_type != snapshot_.context_type || std::strcmp(value.context_name, snapshot_.context_name) ||
            std::strcmp(value.album, snapshot_.album) || text != text_ || std::strcmp(value.text_mask_url, snapshot_.text_mask_url)) rows(0, 46);
        if (std::strcmp(value.title, snapshot_.title) || std::strcmp(value.artist, snapshot_.artist) || text != text_ ||
            std::strcmp(value.text_mask_url, snapshot_.text_mask_url)) rows(std::min(184, int(height)*194/296), std::max(230, int(height)*216/296+7));
        if (!settings.open) {
            if (dx != record_x_ || dy != record_y_) {
                const int side = std::min(int(width)*128/240, int(height)*128/296);
                const int cell = width >= 200 ? 2 : 1, radius = (side*7/16)/cell*cell;
                const int cy = height*50/296 + side/2;
                rows(cy-radius, cy+radius+cell);
            }
            if (progress != progress_) rows(height*234/296-1, height*234/296+2);
        }
        if (controls != controls_) rows(height-25*scale, height);
    } else if (minute != minute_ && !settings.open) rows(height*210/296, height*210/296+7*scale);
    if (settings.open && (settings.brightness != settings_.brightness || settings.volume != settings_.volume ||
        settings.volume_available != settings_.volume_available || resume != resume_ || controls != controls_)) {
        // Both old and new panel bounds are covered when volume availability changes.
        const auto old_panel = settings_layout(width,height,settings_.volume_available).panel;
        const auto panel = settings_layout(width,height,settings.volume_available).panel;
        rows(std::min(old_panel.y,panel.y), std::max(old_panel.y+old_panel.height,panel.y+panel.height));
    }
    initialized_ = true; snapshot_ = value; state_ = state; settings_ = settings;
    artwork_ = artwork; text_ = text; minute_ = minute; progress_ = progress;
    record_x_ = dx; record_y_ = dy; controls_ = controls; resume_ = resume;
}
} // namespace bottie
