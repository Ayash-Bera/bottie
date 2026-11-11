#include "bottie/core.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace {
bool number(const char* value, uint64_t& output) {
    auto end = value + std::strlen(value);
    auto parsed = std::from_chars(value, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}
}

int main(int argc, char** argv) {
    bool offline = false, control = false;
    uint64_t elapsed = 0, wall = 0;
    const char* art_path = nullptr;
    const char* frame_path = nullptr;
    uint64_t seed = 0x74b39ac1U;
    uint16_t width = 128, height = 128;
    bottie::Button button{};
    for (int n = 1; n < argc; ++n) {
        if (std::strcmp(argv[n], "--offline") == 0) offline = true;
        else if (std::strcmp(argv[n], "--elapsed-ms") == 0 && n + 1 < argc) { if (!number(argv[++n], elapsed)) return 2; }
        else if (std::strcmp(argv[n], "--wall-ms") == 0 && n + 1 < argc) { if (!number(argv[++n], wall)) return 2; }
        else if (std::strcmp(argv[n], "--art") == 0 && n + 1 < argc) art_path = argv[++n];
        else if (std::strcmp(argv[n], "--frame") == 0 && n + 1 < argc) frame_path = argv[++n];
        else if (std::strcmp(argv[n], "--profile") == 0 && n + 1 < argc) {
            if (std::strcmp(argv[++n], "recovered") != 0) return 2;
            width = 240; height = 296;
        }
        else if (std::strcmp(argv[n], "--seed") == 0 && n + 1 < argc) {
            if (!number(argv[++n], seed) || seed > UINT32_MAX) return 2;
        }
        else if (std::strcmp(argv[n], "--control") == 0 && n + 1 < argc) {
            const char* name = argv[++n];
            const char* names[] = {"shuffle", "previous", "play_pause", "next", "repeat"};
            bool found = false;
            for (size_t index = 0; index < 5; ++index) {
                if (std::strcmp(name, names[index]) == 0) { button = static_cast<bottie::Button>(index); found = true; }
            }
            if (!found) return 2;
            control = true;
        } else {
            std::fputs("Usage: bottie_fixture [--control BUTTON] [--elapsed-ms N] [--wall-ms N] [--offline] [--art FILE] [--frame FILE.ppm] [--seed N] [--profile recovered] < playback.json\n", stderr);
            return 2;
        }
    }
    std::array<char, bottie::max_json_bytes + 1> bytes{};
    size_t length = std::fread(bytes.data(), 1, bytes.size(), stdin);
    if (std::ferror(stdin)) return 2;
    bottie::Model model;
    model.planet_seed = static_cast<uint32_t>(seed);
    auto error = model.ingest(bytes.data(), length, 0, wall);
    if (error != bottie::Error::none) {
        std::fprintf(stderr, "Snapshot rejected: %s\n", bottie::error_name(error));
        return 1;
    }
    if (offline) model.disconnected();
    if (control) {
        bottie::Intent intent;
        char body[64];
        if (!model.intent(button, elapsed, intent)) { std::fputs("Control unavailable\n", stderr); return 3; }
        if (!bottie::encode_intent(intent, body, sizeof body)) return 1;
        std::puts(body);
        return 0;
    }
    std::array<uint8_t, 16 + bottie::max_art_side * bottie::max_art_side * 2 + 1> image{};
    bottie::Artwork artwork;
    if (art_path) {
        FILE* file = std::fopen(art_path, "rb");
        if (!file) return 2;
        size_t size = std::fread(image.data(), 1, image.size(), file);
        bool read_ok = !std::ferror(file);
        std::fclose(file);
        if (!read_ok || !bottie::parse_artwork(image.data(), size, artwork)) {
            std::fputs("Artwork rejected\n", stderr);
            return 1;
        }
    }
    std::array<uint16_t, bottie::max_frame_width * bottie::max_frame_height> pixels{};
    bottie::Frame frame{pixels.data(), pixels.size(), width, height, width};
    model.tick(elapsed);
    if (!bottie::render(model, elapsed, frame, art_path ? &artwork : nullptr)) return 1;
    if (frame_path) {
        FILE* file = std::fopen(frame_path, "wb");
        if (!file) return 2;
        bool ok = std::fprintf(file, "P6\n%u %u\n255\n", unsigned(width), unsigned(height)) > 0;
        for (size_t index = 0; index < size_t(width) * height; ++index) {
            uint16_t pixel = pixels[index];
            uint8_t rgb[] = {static_cast<uint8_t>(((pixel >> 11) & 31) * 255 / 31),
                             static_cast<uint8_t>(((pixel >> 5) & 63) * 255 / 63),
                             static_cast<uint8_t>((pixel & 31) * 255 / 31)};
            if (std::fwrite(rgb, 1, sizeof rgb, file) != sizeof rgb) { ok = false; break; }
        }
        if (std::fclose(file) != 0) ok = false;
        if (!ok) return 2;
    }
    std::printf("{\"state\":\"%s\",\"progress_ms\":%llu,\"frame_hash\":%lu,\"artwork\":%s}\n",
        bottie::playback_name(model.effective_state(elapsed)), static_cast<unsigned long long>(model.progress(elapsed)),
        static_cast<unsigned long>(bottie::frame_hash(frame)), art_path ? "true" : "false");
    return 0;
}
