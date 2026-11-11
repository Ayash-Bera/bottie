#include "bottie/core.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Band check failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

void compare(const bottie::Model& model, uint64_t now, const bottie::Artwork* art) {
    constexpr uint16_t width = 240, height = 296, stride = 242;
    std::vector<uint16_t> full(width * height), reconstructed(width * height);
    CHECK(bottie::render(model, now, {full.data(), full.size(), width, height, width}, art));
    for (uint16_t band_height : {uint16_t(1), uint16_t(16), uint16_t(31)}) {
        std::vector<uint16_t> band(stride * band_height + 2, 0xbeef);
        for (uint16_t y = 0; y < height; y += band_height) {
            uint16_t rows = std::min(band_height, static_cast<uint16_t>(height - y));
            std::fill(band.begin(), band.end(), 0xbeef);
            bottie::Frame frame{band.data() + 1, size_t(stride) * rows, width, height, stride, y, rows};
            CHECK(bottie::render(model, now, frame, art));
            for (size_t row = 0; row < rows; ++row) {
                std::copy_n(frame.pixels + row * stride, width, reconstructed.data() + (y + row) * width);
                CHECK(frame.pixels[row * stride + width] == 0xbeef && frame.pixels[row * stride + width + 1] == 0xbeef);
            }
            CHECK(band.front() == 0xbeef);
            for (size_t i = size_t(stride) * rows + 1; i < band.size(); ++i) CHECK(band[i] == 0xbeef);
        }
        CHECK(full == reconstructed);
    }
}

int main() {
    bottie::Model model;
    model.received = model.online = true;
    model.snapshot.state = bottie::Playback::playing;
    model.snapshot.duration_ms = 180000;
    model.snapshot.progress_ms = 12000;
    model.snapshot.capabilities = {true, true, true, true, true, true};
    std::strcpy(model.snapshot.title, "Orbital Sunrise");
    std::strcpy(model.snapshot.artist, "Bottie Ensemble");
    std::strcpy(model.snapshot.album, "Small Worlds");
    std::strcpy(model.snapshot.context_name, "Desk Orbits");
    std::strcpy(model.snapshot.artwork_url, "/v1/artwork/orbit");
    model.snapshot.context_type = bottie::Context::playlist;
    std::array<uint8_t, 96 * 96 * 2> pixels{};
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 71);
    bottie::Artwork art{pixels.data(), 96, 96, 192};
    compare(model, 1800, &art);
    compare(model, 1800, nullptr);
    compare(model, 16000, &art);
    model.disconnected();
    compare(model, 1800, &art);
    model.online = true;
    art.height = 48;
    compare(model, 1800, &art);
    model.snapshot.context_type = bottie::Context::album;
    compare(model, 1800, &art);
    model.snapshot.artwork_url[0] = model.snapshot.context_name[0] = 0;
    compare(model, 1800, &art);
    for (auto state : {bottie::Playback::paused, bottie::Playback::idle, bottie::Playback::unavailable}) {
        model.snapshot.state = state;
        for (uint32_t seed = 10; seed < 15; ++seed) {
            model.planet_seed = seed;
            compare(model, 1800, nullptr);
        }
    }
    bottie::Frame invalid{reinterpret_cast<uint16_t*>(pixels.data()), pixels.size() / 2, 240, 296, 240, 288, 9};
    CHECK(!bottie::render(model, 0, invalid) && !bottie::frame_hash(invalid));
    invalid.storage_height = 8;
    invalid.capacity = 240 * 8 - 1;
    CHECK(!bottie::render(model, 0, invalid));
    invalid.capacity = pixels.size() / 2;
    invalid.storage_y = 296;
    CHECK(!bottie::render(model, 0, invalid));
    std::puts("BOTTIE_BANDS_OK full/banded equality, all states/families, short tail, stride and guards");
}
