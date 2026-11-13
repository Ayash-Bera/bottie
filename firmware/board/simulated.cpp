#include "simulated.hpp"

#include <cstdio>
#include <cstring>

#include "esp_timer.h"

namespace bottie {
uint64_t SimulatedBoard::monotonic_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

bool SimulatedBoard::playback(char* bytes, size_t capacity, size_t& length) {
    uint64_t now = monotonic_ms();
    const char* state = (now / 20000) % 3 == 0 ? "idle" : (now / 20000) % 3 == 1 ? "playing" : "paused";
    int size = std::snprintf(bytes, capacity,
        "{\"schema_version\":1,\"state\":\"%s\",\"track_id\":\"simulated\","
        "\"title\":\"Orbital Sunrise\",\"artist\":\"Bottie Ensemble\",\"album\":\"Small Worlds\","
        "\"context\":{\"type\":\"playlist\",\"name\":\"Desk Orbits\"},"
        "\"progress_ms\":%llu,\"duration_ms\":180000,\"updated_at_ms\":%llu,"
        "\"shuffle\":false,\"repeat\":\"off\","
        "\"capabilities\":{\"play\":true,\"pause\":true,\"previous\":true,\"next\":true,\"shuffle\":true,\"repeat\":true}}",
        state, static_cast<unsigned long long>(now % 180000), static_cast<unsigned long long>(now + 1));
    if (size < 0 || static_cast<size_t>(size) >= capacity) return false;
    length = static_cast<size_t>(size);
    return true;
}

InputEvent SimulatedBoard::input() {
    uint64_t now = monotonic_ms();
    if (now - last_input_ms_ < 5000) return {};
    last_input_ms_ = now;
    if ((now / 20000) % 3 == 0) return {};
    return {EventType::control, static_cast<Button>((now / 5000) % 5)};
}

bool SimulatedBoard::control(const char* json, size_t length) {
    if (!json || length == 0 || length > 64) return false;
    std::printf("Simulated intent: %.*s\n", static_cast<int>(length), json);
    return true;
}

bool SimulatedBoard::present(const Frame& frame) {
    if (!frame.valid()) return false;
    if (frame.storage_y + frame.rows() != frame.height) return true;
    if (++presents_ % 100 == 0) std::printf("Headless %s hash: %08lx\n", frame.storage_y ? "final band" : "frame",
                                         static_cast<unsigned long>(frame_hash(frame)));
    return true;
}
} // namespace bottie
