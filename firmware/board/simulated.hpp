#pragma once

#include "board.hpp"

namespace bottie {
class SimulatedBoard final : public Board {
public:
    uint64_t monotonic_ms() override;
    bool playback(char* bytes, size_t capacity, size_t& length) override;
    InputEvent input() override;
    bool control(const char* json, size_t length) override;
    bool present(const Frame& frame) override;
private:
    uint64_t last_input_ms_{};
    uint32_t presents_{};
};
} // namespace bottie
