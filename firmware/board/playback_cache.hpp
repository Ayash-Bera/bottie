#pragma once

#include "bottie/core.hpp"

#include <array>
#include <cstring>

namespace bottie {
struct CachedPlayback {
    static constexpr size_t pixel_capacity = 96 * 96 * 2;
    Snapshot snapshot{};
    std::array<uint8_t, pixel_capacity> pixels{};
    uint64_t observed_ms{};
    uint16_t art_width{}, art_height{}, art_stride{};
    bool online{};

    void offline() { online = false; art_width = art_height = art_stride = 0; }
    void capture(const Snapshot& value, uint64_t observed, const Artwork* art) {
        snapshot = value;
        observed_ms = observed;
        online = true;
        art_width = art_height = art_stride = 0;
        if (!art || !art->pixels || !snapshot.artwork_url[0] || !art->width || !art->height ||
            art->width > 96 || art->height > 96 || art->stride != art->width * 2 ||
            size_t(art->stride) * art->height > pixels.size()) return;
        std::memcpy(pixels.data(), art->pixels, size_t(art->stride) * art->height);
        art_width = art->width; art_height = art->height; art_stride = art->stride;
    }
    Artwork artwork() const {
        return online && art_width ? Artwork{pixels.data(), art_width, art_height, art_stride} : Artwork{};
    }
};
} // namespace bottie
