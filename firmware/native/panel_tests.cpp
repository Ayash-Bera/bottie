#include "recovered_profile.hpp"
#include "frame_diagnostics.hpp"

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
    std::vector<std::vector<uint8_t>> parameters;
    size_t chunks{}, stop_at{};
    bool started{}, ended{};
    bool reset_level(bool high) override { resets.push_back(high); return true; }
    void delay_ms(uint16_t ms) override { delays.push_back(ms); }
    bool command(uint8_t code, const uint8_t* data, size_t length) override {
        commands.push_back(code);
        parameters.emplace_back(data, data + length);
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
    // Captured setup fingerprint with only the MADCTL color selector cleared.
    CHECK(init.commands.size() == 29 && init.sequence_hash == 0x0544cadcU);
    CHECK(init.commands[1] == 0x36 && init.parameters[1] == std::vector<uint8_t>{0x00});
    CHECK((init.delays == std::vector<unsigned>{10,20,100,120,10}));
    CHECK((init.resets == std::vector<bool>{false,true}));
    std::array<uint16_t, 240> primaries{};
    constexpr uint16_t colors[] = {0xf800,0x07e0,0x001f,0xffff,0x0000};
    constexpr uint8_t color_bytes[] = {0xf8,0x00,0x07,0xe0,0x00,0x1f,0xff,0xff,0x00,0x00};
    for (size_t i = 0; i < primaries.size(); ++i) primaries[i] = colors[i % 5];
    bottie::Frame primary_row{primaries.data(), primaries.size(), 240, 296, 240, 0, 1};
    RecordingTransport primary_transfer;
    CHECK(bottie::recovered::present(primary_transfer, primary_row));
    CHECK(primary_transfer.bytes.size() == 480);
    for (size_t i = 0; i < primary_transfer.bytes.size(); ++i)
        CHECK(primary_transfer.bytes[i] == color_bytes[i % 10]);
    std::vector<uint16_t> pixels(242 * 296, 0xffff);
    for (size_t y = 0; y < 296; ++y)
        for (size_t x = 0; x < 240; ++x) pixels[y * 242 + x] = static_cast<uint16_t>((y << 8) | x);
    bottie::Frame frame{pixels.data(), pixels.size(), 240, 296, 242};
    RecordingTransport transfer;
    CHECK(bottie::recovered::present(transfer, frame));
    CHECK(transfer.started && transfer.ended && transfer.bytes.size() == 142080);
    CHECK((transfer.commands == std::vector<unsigned>{0x2a,0x2b}));
    CHECK((transfer.parameters[1] == std::vector<uint8_t>{0,0,1,0x27}));
    bottie::PixelDiagnostics source;
    source.add(frame);
    bottie::WireDiagnostics wire;
    for (size_t start = 0; start < transfer.bytes.size(); start += 3840)
        wire.add(transfer.bytes.data() + start, std::min(size_t(3840), transfer.bytes.size() - start));
    CHECK(source.count == 240 * 296 && source.first == 0 && source.minimum == 0 && source.maximum == 0xffef);
    CHECK(source.little_hash == bottie::frame_hash(frame) && source.wire_hash == wire.hash);
    CHECK(wire.bytes == 142080 && wire.first0 == 0 && wire.first1 == 0);
    CHECK(source.different == source.count - 2);
    for (size_t y = 0; y < 296; ++y) {
        for (size_t x = 0; x < 240; ++x) {
            size_t offset = (y * 240 + x) * 2;
            CHECK(transfer.bytes[offset] == (y & 255) && transfer.bytes[offset + 1] == x);
        }
    }
    RecordingTransport broken;
    broken.stop_at = 5;
    CHECK(!bottie::recovered::present(broken, frame) && broken.ended && broken.chunks == 5);
    bottie::Frame band{pixels.data() + 288 * 242, 8 * 242, 240, 296, 242, 288, 8};
    RecordingTransport tail;
    CHECK(bottie::recovered::present(tail, band) && tail.ended && tail.chunks == 1);
    CHECK((tail.parameters[1] == std::vector<uint8_t>{1,0x20,1,0x27}));
    CHECK((tail.bytes == std::vector<uint8_t>(transfer.bytes.begin() + 288 * 480, transfer.bytes.end())));
    band.storage_height = 9;
    RecordingTransport overflow;
    CHECK(!bottie::recovered::present(overflow, band) && overflow.commands.empty());
    RecordingTransport invalid;
    frame.capacity = 240;
    CHECK(!bottie::recovered::present(invalid, frame) && invalid.commands.empty() && !invalid.started);
    std::puts("BOTTIE_RECOVERED_TRANSPORT_OK rgb-order/trace/endianness/stride/bounds/cleanup");
}
