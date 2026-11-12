#include "bottie/text_mask.hpp"

#include <cstring>

namespace bottie {
bool text_mask_path(const char* path) {
    if (!path || std::strlen(path) != 73 || std::strncmp(path, "/v1/text/", 9)) return false;
    for (size_t n = 9; n < 73; ++n)
        if (!((path[n] >= '0' && path[n] <= '9') || (path[n] >= 'a' && path[n] <= 'f'))) return false;
    return true;
}

bool requires_text_mask(const char* value) {
    if (!value) return false;
    while (*value) {
        const auto first = static_cast<uint8_t>(*value++);
        if (first < 128) continue;
        const unsigned count = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
        uint32_t code = first & (count == 3 ? 7 : count == 2 ? 15 : 31);
        for (unsigned n = 0; n < count; ++n) {
            const auto next = static_cast<uint8_t>(*value);
            if (!next || (next & 0xc0) != 0x80) return true;
            code = (code << 6) | (next & 63);
            ++value;
        }
        if (code == 0xa0 || (code >= 0x2010 && code <= 0x2014) || code == 0x2018 ||
            code == 0x2019 || code == 0x201c || code == 0x201d) continue;
        return true;
    }
    return false;
}

uint8_t required_text_planes(const Snapshot& snapshot) {
    const bool playlist = snapshot.context_type == Context::playlist && snapshot.context_name[0];
    return (requires_text_mask(playlist ? snapshot.context_name : snapshot.album) ? 1 : 0) |
           (playlist && requires_text_mask(snapshot.album) ? 2 : 0) |
           (requires_text_mask(snapshot.title) ? 4 : 0) | (requires_text_mask(snapshot.artist) ? 8 : 0);
}

bool parse_text_mask(const uint8_t* bytes, size_t length, TextMask& output) {
    output = {};
    if (!bytes || length < 16 || length > text_mask_capacity || std::memcmp(bytes, "BTXT", 4) ||
        bytes[4] != 1 || bytes[5] != 1 || !bytes[6] || (bytes[6] & 0xf0) || bytes[7] != 1 ||
        bytes[8] != 240 || bytes[9] || bytes[10] != 40 || bytes[11] != 1) return false;
    const uint32_t payload = bytes[12] | (uint32_t(bytes[13]) << 8) | (uint32_t(bytes[14]) << 16) |
                             (uint32_t(bytes[15]) << 24);
    if (payload != length - 16) return false;
    TextMask next{bytes, length, bytes[6], {}};
    size_t offset = 16;
    for (size_t n = 0; n < 4; ++n) if (next.flags & (1 << n)) {
        const size_t count = size_t(text_boxes[n].width / 2) * text_boxes[n].height;
        if (count > length - offset) return false;
        next.planes[n] = bytes + offset;
        offset += count;
    }
    if (offset != length) return false;
    output = next;
    return true;
}

void TextIdentity::assign(const Snapshot& s) {
    std::strcpy(track_id, s.track_id); std::strcpy(title, s.title); std::strcpy(artist, s.artist);
    std::strcpy(album, s.album); std::strcpy(context_name, s.context_name); std::strcpy(url, s.text_mask_url);
    context_type = s.context_type; layout = s.text_mask_layout;
}
bool TextIdentity::matches(const Snapshot& s) const {
    return context_type == s.context_type && layout == s.text_mask_layout &&
        !std::strcmp(track_id, s.track_id) && !std::strcmp(title, s.title) && !std::strcmp(artist, s.artist) &&
        !std::strcmp(album, s.album) && !std::strcmp(context_name, s.context_name) && !std::strcmp(url, s.text_mask_url);
}
bool TextCache::observe(const Snapshot& snapshot) {
    if (identity_.matches(snapshot)) return false;
    length_ = 0;
    identity_.assign(snapshot);
    return true;
}
void TextCache::clear() { length_ = 0; identity_ = {}; }
bool TextCache::commit(const Snapshot& snapshot, size_t length) {
    if (!identity_.matches(snapshot)) return false;
    length_ = 0;
    TextMask parsed;
    if (snapshot.text_mask_layout != 1 || !text_mask_path(snapshot.text_mask_url) ||
        !parse_text_mask(bytes_.data(), length, parsed) || (parsed.flags & ~required_text_planes(snapshot))) return false;
    length_ = length;
    return true;
}
TextMask TextCache::view(const Snapshot& snapshot) const {
    TextMask result;
    if (identity_.matches(snapshot)) parse_text_mask(bytes_.data(), length_, result);
    return result;
}
} // namespace bottie
