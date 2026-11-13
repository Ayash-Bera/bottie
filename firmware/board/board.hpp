#pragma once

#include "bottie/core.hpp"
#include "bottie/interaction.hpp"

namespace bottie {

enum class EventType { none, control, pointer };
struct InputEvent {
    EventType type{EventType::none};
    Button button{Button::play_pause};
    PointerSample pointer{};
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
