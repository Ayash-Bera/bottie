#include "bottie/core.hpp"

#include <cstring>

namespace bottie {
bool parse_artwork(const uint8_t* bytes, size_t length, Artwork& output) {
    if (!bytes || length < 16 || std::memcmp(bytes, "BTAR", 4) || bytes[4] != 1 || bytes[5] != 1) return false;
    auto u16 = [&](size_t offset) { return static_cast<uint16_t>(bytes[offset] | (uint16_t(bytes[offset + 1]) << 8)); };
    uint16_t width = u16(6), height = u16(8), stride = u16(10);
    uint32_t payload = uint32_t(bytes[12]) | (uint32_t(bytes[13]) << 8) |
                       (uint32_t(bytes[14]) << 16) | (uint32_t(bytes[15]) << 24);
    if (width == 0 || height == 0 || width > max_art_side || height > max_art_side || stride != width * 2 ||
        payload != uint32_t(stride) * height || length != 16 + static_cast<size_t>(payload)) return false;
    output = {bytes + 16, width, height, stride};
    return true;
}
} // namespace bottie
