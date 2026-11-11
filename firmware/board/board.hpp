#pragma once

#include "bottie/core.hpp"

namespace bottie {

enum class EventType { none, planet_tap, control };
struct InputEvent {
    EventType type{EventType::none};
    Button button{Button::play_pause};
};

// A real adapter must supply verified wiring, transport and artwork ownership.
class Board {
public:
    virtual ~Board() = default;
    virtual uint64_t monotonic_ms() = 0;
    virtual bool playback(char* bytes, size_t capacity, size_t& length) = 0;
    virtual InputEvent input() = 0;
    virtual bool control(const char* json, size_t length) = 0;
    virtual bool present(const Frame& frame) = 0;
};

} // namespace bottie
