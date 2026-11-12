#pragma once

#include "recovered_profile.hpp"
#include "frame_diagnostics.hpp"
#include "simulated.hpp"
#include "cgotchi_input.hpp"

#include "driver/spi_master.h"

namespace bottie {
class RecoveredBoard final : public Board, private recovered::Transport {
public:
    ~RecoveredBoard() override;
    bool initialize();
    uint64_t monotonic_ms() override { return fixture_.monotonic_ms(); }
    bool playback(char* bytes, size_t capacity, size_t& length) override { return fixture_.playback(bytes, capacity, length); }
    InputEvent input() override;
    bool brightness(uint8_t percent);
    InputHealth input_health() const { return input_.health(); }
    bool control(const char*, size_t) override { return false; }
    bool present(const Frame& frame) override;
    void diagnose_frame() { diagnostics_ = {}; diagnosing_ = true; }
private:
    bool reset_level(bool high) override;
    void delay_ms(uint16_t milliseconds) override;
    bool command(uint8_t code, const uint8_t* bytes, size_t length) override;
    bool begin_pixels() override;
    bool pixels(const uint8_t* bytes, size_t length) override;
    bool end_pixels() override;
    bool send(const uint8_t* bytes, size_t length);
    SimulatedBoard fixture_;
    CgotchiInput input_;
    spi_device_handle_t device_{};
    bool bus_ready_{}, configured_{}, initialized_{};
    WireDiagnostics diagnostics_;
    bool diagnosing_{};
    bool pwm_ready_{};
};
} // namespace bottie
