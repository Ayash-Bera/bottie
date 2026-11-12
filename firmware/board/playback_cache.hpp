#pragma once

#include "bottie/core.hpp"
#include "bottie/text_mask.hpp"

#include <array>
#include <cstring>

namespace bottie {
struct CachedPlayback {
    static constexpr size_t pixel_capacity = 96 * 96 * 2;
    Snapshot snapshot{};
    std::array<uint8_t, pixel_capacity> pixels{};
    std::array<uint8_t, text_mask_capacity> text_bytes{};
    size_t text_length{};
    uint64_t observed_ms{};
    uint16_t art_width{}, art_height{}, art_stride{};
    bool online{};

    void offline() { online = false; art_width = art_height = art_stride = 0; text_length = 0; }
    void capture(const Snapshot& value, uint64_t observed, const Artwork* art, const TextMask* mask = nullptr) {
        snapshot = value;
        observed_ms = observed;
        online = true;
        art_width = art_height = art_stride = 0;
        text_length = 0;
        TextMask validated;
        if (mask && snapshot.text_mask_layout == 1 && text_mask_path(snapshot.text_mask_url) &&
            parse_text_mask(mask->bytes, mask->length, validated) && !(validated.flags & ~required_text_planes(snapshot))) {
            std::memcpy(text_bytes.data(), validated.bytes, validated.length);
            text_length = validated.length;
        }
        if (!art || !art->pixels || !snapshot.artwork_url[0] || !art->width || !art->height ||
            art->width > 96 || art->height > 96 || art->stride != art->width * 2 ||
            size_t(art->stride) * art->height > pixels.size()) return;
        std::memcpy(pixels.data(), art->pixels, size_t(art->stride) * art->height);
        art_width = art->width; art_height = art->height; art_stride = art->stride;
    }
    Artwork artwork() const {
        return online && art_width ? Artwork{pixels.data(), art_width, art_height, art_stride} : Artwork{};
    }
    TextMask text_mask() const {
        TextMask result;
        if (online) parse_text_mask(text_bytes.data(), text_length, result);
        return result;
    }
};
} // namespace bottie
