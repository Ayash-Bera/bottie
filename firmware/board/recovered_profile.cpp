#include "recovered_profile.hpp"

#include <algorithm>
#include <array>

namespace bottie::recovered {
bool initialize(Transport& transport) {
    if (!transport.reset_level(false)) return false;
    transport.delay_ms(10);
    if (!transport.reset_level(true)) return false;
    transport.delay_ms(20);
    for (const auto& item : init) {
        if (!transport.command(item.code, item.data, item.length)) return false;
        if (item.delay_ms) transport.delay_ms(item.delay_ms);
    }
    return true;
}

bool present(Transport& transport, const Frame& frame) {
    if (!frame.valid() || frame.width != width || frame.height != height) return false;
    constexpr uint8_t columns[] = {0,0,0,0xef};
    const uint16_t last = frame.storage_y + frame.rows() - 1;
    const uint8_t rows[] = {static_cast<uint8_t>(frame.storage_y >> 8), static_cast<uint8_t>(frame.storage_y),
                            static_cast<uint8_t>(last >> 8), static_cast<uint8_t>(last)};
    if (!transport.command(0x2a, columns, sizeof columns) || !transport.command(0x2b, rows, sizeof rows) ||
        !transport.begin_pixels()) return false;
    alignas(4) std::array<uint8_t, width * 2 * 8> bytes{};
    for (size_t y = 0; y < frame.rows(); y += 8) {
        size_t count = std::min(size_t(8), size_t(frame.rows()) - y);
        for (size_t row = 0; row < count; ++row) {
            for (size_t x = 0; x < width; ++x) {
                uint16_t pixel = frame.pixels[(y + row) * frame.stride + x];
                size_t offset = (row * width + x) * 2;
                bytes[offset] = static_cast<uint8_t>(pixel >> 8);
                bytes[offset + 1] = static_cast<uint8_t>(pixel);
            }
        }
        if (!transport.pixels(bytes.data(), count * width * 2)) {
            transport.end_pixels();
            return false;
        }
    }
    return transport.end_pixels();
}
} // namespace bottie::recovered
