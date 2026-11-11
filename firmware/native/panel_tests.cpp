#include "recovered_profile.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Panel check failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

namespace {
class RecordingTransport : public bottie::recovered::Transport {
public:
    uint32_t sequence_hash{2166136261U};
    std::vector<unsigned> delays, commands;
    std::vector<bool> resets;
    std::vector<uint8_t> bytes;
    size_t chunks{}, stop_at{};
    bool started{}, ended{};
    bool reset_level(bool high) override { resets.push_back(high); return true; }
    void delay_ms(uint16_t ms) override { delays.push_back(ms); }
    bool command(uint8_t code, const uint8_t* data, size_t length) override {
        commands.push_back(code);
        sequence_hash = (sequence_hash ^ code) * 16777619U;
        sequence_hash = (sequence_hash ^ static_cast<uint8_t>(length)) * 16777619U;
        for (size_t i = 0; i < length; ++i) sequence_hash = (sequence_hash ^ data[i]) * 16777619U;
        return true;
    }
    bool begin_pixels() override { CHECK(!started); started = true; return true; }
    bool pixels(const uint8_t* data, size_t length) override {
        CHECK(started && !ended && length <= 3840 && length % 480 == 0);
        if (++chunks == stop_at) return false;
        bytes.insert(bytes.end(), data, data + length);
        return true;
    }
    bool end_pixels() override { ended = true; return true; }
};
}

int main() {
    RecordingTransport init;
    CHECK(bottie::recovered::initialize(init));
    // Independent fingerprint of the captured setup command stream.
    CHECK(init.commands.size() == 29 && init.sequence_hash == 0xb79ca1c4U);
    CHECK((init.delays == std::vector<unsigned>{10,20,100,120,10}));
    CHECK((init.resets == std::vector<bool>{false,true}));
    std::vector<uint16_t> pixels(242 * 296, 0xffff);
    for (size_t y = 0; y < 296; ++y)
        for (size_t x = 0; x < 240; ++x) pixels[y * 242 + x] = static_cast<uint16_t>((y << 8) | x);
    bottie::Frame frame{pixels.data(), pixels.size(), 240, 296, 242};
    RecordingTransport transfer;
    CHECK(bottie::recovered::present(transfer, frame));
    CHECK(transfer.started && transfer.ended && transfer.bytes.size() == 142080);
    CHECK((transfer.commands == std::vector<unsigned>{0x2a,0x2b}));
    for (size_t y = 0; y < 296; ++y) {
        for (size_t x = 0; x < 240; ++x) {
            size_t offset = (y * 240 + x) * 2;
            CHECK(transfer.bytes[offset] == (y & 255) && transfer.bytes[offset + 1] == x);
        }
    }
    RecordingTransport broken;
    broken.stop_at = 5;
    CHECK(!bottie::recovered::present(broken, frame) && broken.ended && broken.chunks == 5);
    RecordingTransport invalid;
    frame.capacity = 240;
    CHECK(!bottie::recovered::present(invalid, frame) && invalid.commands.empty() && !invalid.started);
    std::puts("BOTTIE_RECOVERED_TRANSPORT_OK trace/endianness/stride/bounds/cleanup");
}
