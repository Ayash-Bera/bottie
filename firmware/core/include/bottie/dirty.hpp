#pragma once

#include "bottie/interaction.hpp"

namespace bottie {
class DirtyBands {
public:
    static constexpr uint16_t band_rows = 16;
    void prepare(const Model& model, uint64_t now, const SettingsView& settings,
                 uint32_t artwork_revision, uint32_t text_revision,
                 uint16_t width = 240, uint16_t height = 296);
    bool dirty(uint16_t y) const { return y < height_ && (pending_ & (1U << (y / band_rows))); }
    bool any() const { return pending_ != 0; }
    void complete(uint16_t y, bool success) {
        if (!success) invalidate();
        else if (y < height_) pending_ &= ~(1U << (y / band_rows));
    }
    void invalidate();
private:
    void rows(int top, int bottom);
    Snapshot snapshot_{};
    SettingsView settings_{};
    Playback state_{Playback::unavailable};
    uint16_t width_{}, height_{};
    uint32_t pending_{}, artwork_{}, text_{};
    int minute_{-1}, progress_{}, record_x_{}, record_y_{};
    uint16_t controls_{};
    bool initialized_{}, resume_{};
};
} // namespace bottie
