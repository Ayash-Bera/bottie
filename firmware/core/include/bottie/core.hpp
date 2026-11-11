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
constexpr uint64_t planet_period_ms = 30000;

enum class Playback { playing, paused, idle, unavailable };
enum class Repeat { off, context, track };
enum class Context { unknown, playlist, album };
enum class Button { shuffle, previous, play_pause, next, repeat };
enum class Action { play, pause, previous, next, shuffle, repeat };
enum class Error { none, size, syntax, depth, tokens, field, range, version, stale };

struct Capabilities {
    bool play{}, pause{}, previous{}, next{}, shuffle{}, repeat{};
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
};

Error parse_snapshot(const char* json, size_t bytes, Snapshot& output);
const char* error_name(Error error);
const char* playback_name(Playback state);

struct Intent {
    Action action{Action::play};
    bool shuffle{};
    Repeat repeat{Repeat::off};
};

bool encode_intent(const Intent& intent, char* output, size_t capacity);

struct Model {
    Snapshot snapshot{};
    bool received{}, online{};
    uint64_t received_ms{}, age_at_receive_ms{}, planet_changed_ms{};
    uint32_t planet_seed{0x74b39ac1U};

    Error ingest(const char* json, size_t bytes, uint64_t now_ms, uint64_t unix_ms = 0);
    // Accept only snapshots already validated by parse_snapshot.
    Error apply_snapshot(const Snapshot& candidate, uint64_t now_ms, uint64_t unix_ms = 0);
    void disconnected();
    Playback effective_state(uint64_t now_ms) const;
    uint64_t progress(uint64_t now_ms) const;
    bool intent(Button button, uint64_t now_ms, Intent& output) const;
    void tick(uint64_t now_ms);
    bool tap_planet(uint64_t now_ms);
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

bool render(const Model& model, uint64_t now_ms, Frame frame, const Artwork* artwork = nullptr);
uint32_t frame_hash(const Frame& frame);

} // namespace bottie
