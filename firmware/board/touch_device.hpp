#pragma once

#include "bottie/interaction.hpp"

namespace bottie {
class TouchBus {
public:
    virtual ~TouchBus() = default;
    virtual bool read(uint8_t reg, uint8_t* output, size_t bytes, int timeout_ms) = 0;
};
inline PointerSample sample_touch(TouchBus& bus, uint64_t at_ms) {
    uint8_t report[5]{};
    PointerSample sample{}; sample.at_ms = at_ms;
    if (bus.read(0x02, report, sizeof report, 10)) decode_cst810(report, sizeof report, at_ms, sample);
    return sample;
}
} // namespace bottie
