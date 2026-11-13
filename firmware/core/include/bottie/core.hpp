#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bottie {

constexpr size_t max_json_bytes = 8192;
constexpr size_t max_art_side = 240;
constexpr uint16_t max_frame_width = 240;
constexpr uint16_t max_frame_height = 296;
constexpr uint64_t max_json_integer = 9007199254740991ULL;
constexpr uint64_t stale_after_ms = 15000;
constexpr uint64_t clock_min_ms = 946684800000ULL, clock_max_ms = 4102444799999ULL;

enum class Playback { playing, paused, idle, unavailable };
enum class Repeat { off, context, track };
enum class Context { unknown, playlist, album };
enum class Button { shuffle, previous, play_pause, next, repeat };
enum class Action { play, pause, previous, next, shuffle, repeat, volume, seek };
enum class OfflineReason { unknown, login_required, no_active_player, restricted, rate_limited,
                           upstream_unavailable, wallet_locked, wallet_unavailable, stale,
                           wifi_disconnected, gateway_unreachable };
enum class Error { none, size, syntax, depth, tokens, field, range, version, stale };

struct Capabilities {
    bool play{}, pause{}, previous{}, next{}, shuffle{}, repeat{}, volume{}, seek{};
};

using Digest = std::array<uint8_t,32>;
struct DetailField { std::array<Digest,4> pages{}; uint8_t count{}, flags{}; };
struct DetailDescriptor { Digest identity{}; std::array<DetailField,4> fields{}; bool valid{}; };

struct Clock {
    uint64_t unix_ms{};
    int16_t utc_offset_min{};
    bool valid() const { return unix_ms >= clock_min_ms && unix_ms <= clock_max_ms &&
                              utc_offset_min >= -840 && utc_offset_min <= 840; }
};

struct Snapshot {
    Playback state{Playback::unavailable};
    char track_id[129]{};
    char title[257]{}, artist[257]{}, album[257]{};
    Context context_type{Context::unknown};
    char context_name[257]{};
    uint64_t progress_ms{}, duration_ms{}, updated_at_ms{};
    bool shuffle{};
    Repeat repeat{Repeat::off};
    char artwork_url[257]{};
    uint16_t artwork_width{}, artwork_height{};
    Capabilities capabilities{};
    int16_t volume_percent{-1};
    char control_target[33]{};
    bool target_known{};
    char control_track[33]{};
    OfflineReason offline_reason{OfflineReason::unknown};
    DetailDescriptor details{};
    char text_mask_url[74]{};
    uint8_t text_mask_layout{};
    Clock clock{};
};

Error parse_snapshot(const char* json, size_t bytes, Snapshot& output);
const char* error_name(Error error);
const char* playback_name(Playback state);

struct Intent {
    Action action{Action::play};
    bool shuffle{};
    Repeat repeat{Repeat::off};
    uint8_t volume{};
    char control_target[33]{};
    char control_track[33]{};
    uint32_t position_ms{}, sequence{};
    uint64_t released_ms{};
    bool release_known{};
};

bool encode_intent(const Intent& intent, char* output, size_t capacity);

struct Model {
    Snapshot snapshot{};
    bool received{}, online{};
    uint64_t received_ms{}, age_at_receive_ms{};
    Clock clock_anchor{};
    uint64_t clock_received_ms{};

    Error ingest(const char* json, size_t bytes, uint64_t now_ms, uint64_t unix_ms = 0);
    // Accept only snapshots already validated by parse_snapshot.
    Error apply_snapshot(const Snapshot& candidate, uint64_t now_ms, uint64_t unix_ms = 0);
    void disconnected();
    Playback effective_state(uint64_t now_ms) const;
    uint64_t progress(uint64_t now_ms) const;
    bool intent(Button button, uint64_t now_ms, Intent& output) const;
    bool resume_intent(uint64_t now_ms, Intent& output) const;
    bool volume_intent(uint8_t percent, uint64_t now_ms, Intent& output) const;
    bool seek_intent(uint32_t position_ms, uint64_t now_ms, Intent& output) const;
    bool allows(const Intent& intent, uint64_t now_ms) const;
    int clock_minute(uint64_t now_ms) const;
};

struct Artwork {
    const uint8_t* pixels{};
    uint16_t width{}, height{}, stride{};
};

// The caller owns the encoded bytes for the lifetime of the returned view.
bool parse_artwork(const uint8_t* bytes, size_t length, Artwork& output);

struct Frame {
    uint16_t* pixels{};
    size_t capacity{};
    uint16_t width{}, height{}, stride{};
    uint16_t storage_y{}, storage_height{};

    uint16_t rows() const { return storage_height ? storage_height : height; }
    bool valid() const {
        return pixels && width && height && stride >= width && storage_y < height &&
               rows() <= height - storage_y && capacity >= size_t(stride) * rows();
    }
};

struct TextMask;
bool render(const Model& model, uint64_t now_ms, Frame frame, const Artwork* artwork = nullptr,
            const TextMask* text_mask = nullptr);
uint32_t frame_hash(const Frame& frame);

} // namespace bottie
