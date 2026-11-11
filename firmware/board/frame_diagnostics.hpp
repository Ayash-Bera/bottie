#pragma once

#include "bottie/core.hpp"
#include <algorithm>

namespace bottie {
inline uint32_t diagnostic_hash(uint32_t hash, uint8_t byte) {
    return (hash ^ byte) * 16777619U;
}

struct PixelDiagnostics {
    uint32_t little_hash{2166136261U}, wire_hash{2166136261U}, count{}, different{};
    uint16_t first{}, minimum{0xffff}, maximum{};

    void add(const Frame& frame) {
        for (size_t y = 0; y < frame.rows(); ++y) for (size_t x = 0; x < frame.width; ++x) {
            const uint16_t pixel = frame.pixels[y * frame.stride + x];
            if (!count) first = pixel;
            ++count;
            if (pixel != 0x0882) ++different;
            minimum = std::min(minimum, pixel);
            maximum = std::max(maximum, pixel);
            little_hash = diagnostic_hash(diagnostic_hash(little_hash, uint8_t(pixel)), uint8_t(pixel >> 8));
            wire_hash = diagnostic_hash(diagnostic_hash(wire_hash, uint8_t(pixel >> 8)), uint8_t(pixel));
        }
    }
};

struct WireDiagnostics {
    uint32_t hash{2166136261U}, bytes{};
    uint8_t first0{}, first1{};

    void add(const uint8_t* data, size_t size) {
        if (!bytes && size >= 2) { first0 = data[0]; first1 = data[1]; }
        for (size_t n = 0; n < size; ++n) hash = diagnostic_hash(hash, data[n]);
        bytes += static_cast<uint32_t>(size);
    }
};
} // namespace bottie
