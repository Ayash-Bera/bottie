#pragma once

#include "bottie/core.hpp"

namespace bottie {
constexpr size_t detail_capacity = 10384;
constexpr uint16_t detail_width = 216, detail_height = 96;
bool parse_digest(const char* hex, Digest& digest);
void format_digest(const Digest& digest, char* hex);
struct MetadataIdentity {
    char track[129]{}, title[257]{}, artist[257]{}, album[257]{}, context[257]{};
    Context context_type{};
    void assign(const Snapshot& snapshot);
    bool matches(const Snapshot& snapshot) const;
};
struct DetailSelection {
    MetadataIdentity metadata{};
    Digest identity{}, digest{};
    uint8_t field{}, page{}, count{}, flags{};
    uint32_t sequence{};
    bool active{};
    bool select(const Snapshot& snapshot, uint8_t field_index, uint8_t page_index, uint32_t request);
    bool matches(const Snapshot& snapshot) const;
    bool same(const DetailSelection& other) const;
};
struct DetailPage { const uint8_t* pixels{}; uint8_t field{}, page{}, count{}, flags{}; };
bool parse_detail(const uint8_t* bytes, size_t length, const DetailSelection& selection, DetailPage& page);
struct CachedDetail {
    std::array<uint8_t,detail_capacity> bytes{};
    DetailSelection selection{};
    bool ready{};
    void clear() { ready = false; selection.active = false; }
    bool capture(const DetailSelection& request, const uint8_t* body, size_t length);
    DetailPage view(const Snapshot& snapshot, const DetailSelection& request) const;
};
} // namespace bottie
