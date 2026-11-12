#pragma once

#include "bottie/core.hpp"

namespace bottie {
constexpr size_t text_mask_capacity = 8494;
struct TextBox { uint16_t x, y, width, height; };
inline constexpr TextBox text_boxes[] = {{14,6,212,18}, {19,27,202,18}, {12,184,216,26}, {12,212,216,18}};
struct TextMask {
    const uint8_t* bytes{};
    size_t length{};
    uint8_t flags{};
    std::array<const uint8_t*, 4> planes{};
};
bool text_mask_path(const char* path);
bool requires_text_mask(const char* value);
uint8_t required_text_planes(const Snapshot& snapshot);
bool parse_text_mask(const uint8_t* bytes, size_t length, TextMask& output);

struct TextIdentity {
    char track_id[129]{}, title[257]{}, artist[257]{}, album[257]{}, context_name[257]{}, url[74]{};
    Context context_type{};
    uint8_t layout{};
    void assign(const Snapshot& snapshot);
    bool matches(const Snapshot& snapshot) const;
};

class TextCache {
public:
    bool observe(const Snapshot& snapshot);
    void clear();
    uint8_t* storage() { length_ = 0; return bytes_.data(); }
    bool commit(const Snapshot& snapshot, size_t length);
    TextMask view(const Snapshot& snapshot) const;
private:
    TextIdentity identity_;
    std::array<uint8_t, text_mask_capacity> bytes_{};
    size_t length_{};
};
} // namespace bottie
