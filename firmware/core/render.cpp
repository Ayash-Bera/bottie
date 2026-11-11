#include "bottie/core.hpp"

#include <algorithm>
#include <cstring>

namespace bottie {
namespace {
constexpr uint16_t ink = 0xeef7, dim = 0x62ec, background = 0x0843, accent = 0x7ff6;
uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>(((r & 31) << 11) | ((g & 63) << 5) | (b & 31));
}
uint32_t noise(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}
struct Canvas {
    Frame frame;
    void pixel(int x, int y, uint16_t color) {
        if (x >= 0 && y >= 0 && x < frame.width && y < frame.height)
            frame.pixels[static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x)] = color;
    }
    void rect(int x, int y, int width, int height, uint16_t color) {
        for (int py = std::max(y, 0); py < std::min(y + height, int(frame.height)); ++py)
            for (int px = std::max(x, 0); px < std::min(x + width, int(frame.width)); ++px) pixel(px, py, color);
    }
    void circle(int cx, int cy, int radius, uint16_t color) {
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                if (x*x + y*y <= radius*radius) pixel(cx + x, cy + y, color);
    }
};

const uint8_t* glyph(char c) {
    static constexpr uint8_t letters[][5] = {
        {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
        {65,65,127,65,65},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
        {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
        {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
        {3,4,120,4,3},{97,81,73,69,67}
    };
    static constexpr uint8_t digits[][5] = {
        {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
        {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
        {54,73,73,73,54},{6,73,73,41,30}
    };
    static constexpr uint8_t unknown[5] = {2,1,81,9,6}, space[5] = {}, dash[5] = {8,8,8,8,8};
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ') return space;
    if (c == '-') return dash;
    return unknown;
}
void text(Canvas& canvas, int x, int y, const char* value, int right, uint16_t color) {
    int scale = canvas.frame.width >= 200 ? 2 : 1;
    for (size_t i = 0; value[i] && x + 5 * scale <= right; ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if ((c & 0xc0) == 0x80) continue;
        const uint8_t* columns = glyph(c < 128 ? static_cast<char>(c) : '?');
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (columns[col] & (1 << row)) canvas.rect(x + col * scale, y + row * scale, scale, scale, color);
        x += 6 * scale;
    }
}

void planet(Canvas& canvas, const Model& model, uint64_t now) {
    int radius = std::min(int(canvas.frame.width) / 3, (int(canvas.frame.height) - 44) / 2);
    int cx = canvas.frame.width / 2, cy = (canvas.frame.height - 20) / 2;
    uint32_t family = model.planet_seed % 5;
    unsigned rotation = static_cast<unsigned>((now / 180) % 128);
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            int distance = x*x + y*y;
            if (distance > radius*radius) continue;
            int u = (x * 32 / radius + static_cast<int>(rotation)) & 127;
            int v = y * 32 / radius;
            uint32_t patch = noise(uint32_t(u / 5) + uint32_t((v + 32) / 5) * 71 + model.planet_seed);
            unsigned r, g, b;
            switch (family) {
            case 0:
                if (patch % 9 < 4) { r = 5; g = 39; b = 12; }
                else { r = 3; g = 19; b = 26; }
                if (v < -24 || v > 27) { r = 25; g = 56; b = 29; }
                break;
            case 1: r = 26; g = 22 + (patch % 17); b = 7; break;
            case 2:
                r = 17 + static_cast<unsigned>((v + 34 + (u / 8) % 3) / 5 % 3) * 5;
                g = 19 + static_cast<unsigned>((v + 34 + (u / 8) % 3) / 5 % 4) * 6;
                b = 15;
                break;
            case 3: r = 16 + patch % 9; g = 43 + patch % 14; b = 29; break;
            default:
                if (patch % 7 == 0) { r = 31; g = 13; b = 2; }
                else { r = 8; g = 6; b = 10; }
                break;
            }
            unsigned light = static_cast<unsigned>(std::clamp(24 - x * 12 / radius - y * 5 / radius, 7, 31));
            if (distance > (radius - 2) * (radius - 2)) light = light * 2 / 3;
            canvas.pixel(cx + x, cy + y, rgb(r * light / 31, g * light / 31, b * light / 31));
        }
    }
    if (family == 2) {
        for (int x = -radius - 8; x <= radius + 8; ++x) {
            int y = x / 4 + radius / 3;
            if (x*x + y*y > radius*radius || y > 0) {
                canvas.pixel(cx + x, cy + y, 0xc4b0);
                canvas.pixel(cx + x, cy + y + 1, 0x9b8c);
            }
        }
    }
}

void cover(Canvas& canvas, int x, int y, int side, const Artwork* art) {
    canvas.rect(x, y, side, side, 0x18e6);
    if (!art || !art->pixels || !art->width || !art->height || art->width > max_art_side ||
        art->height > max_art_side || art->stride != art->width * 2) {
        canvas.circle(x + side/2, y + side/2, side/4, dim);
        canvas.circle(x + side/2, y + side/2, side/12, background);
        return;
    }
    int width = side, height = side;
    if (art->width > art->height) height = std::max(1, side * art->height / art->width);
    else width = std::max(1, side * art->width / art->height);
    int left = x + (side - width)/2, top = y + (side - height)/2;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            size_t index = static_cast<size_t>(py * art->height / height) * art->stride +
                           static_cast<size_t>(px * art->width / width) * 2;
            canvas.pixel(left + px, top + py, static_cast<uint16_t>(art->pixels[index] | (uint16_t(art->pixels[index + 1]) << 8)));
        }
    }
}

void controls(Canvas& canvas, const Model& model, uint64_t now) {
    int scale = canvas.frame.width >= 200 ? 2 : 1;
    static constexpr uint16_t icons[][9] = {
        {64,128,455,168,80,168,455,128,64}, {0,129,193,225,249,225,193,129,0},
        {0,198,198,198,198,198,198,198,0}, {0,258,262,270,318,270,262,258,0},
        {32,64,254,66,170,132,254,4,8}
    };
    static constexpr uint16_t play[9] = {0,4,12,28,60,28,12,4,0};
    for (unsigned n = 0; n < 5; ++n) {
        Intent intent;
        bool enabled = model.intent(static_cast<Button>(n), now, intent);
        bool active = (n == 0 && model.snapshot.shuffle) || (n == 4 && model.snapshot.repeat != Repeat::off);
        uint16_t color = enabled ? (active ? accent : ink) : dim;
        int x = (int(n) * 2 + 1) * canvas.frame.width / 10 - 4 * scale;
        int y = canvas.frame.height - 13 * scale;
        const uint16_t* rows = n == 2 && model.effective_state(now) != Playback::playing ? play : icons[n];
        if (n == 2) {
            canvas.circle(x + 4 * scale, y + 4 * scale, 9 * scale - 1, enabled ? accent : dim);
            color = background;
        }
        for (int row = 0; row < 9; ++row)
            for (int col = 0; col < 9; ++col)
                if (rows[row] & (1 << col)) canvas.rect(x + col * scale, y + row * scale, scale, scale, color);
        if (n == 4 && model.snapshot.repeat == Repeat::track) canvas.rect(x + 3 * scale, y + 4 * scale, scale, scale, color);
    }
}
} // namespace

bool render(const Model& model, uint64_t now_ms, Frame frame, const Artwork* artwork) {
    if (!frame.pixels || frame.width < 128 || frame.height < 128 || frame.width > max_frame_width ||
        frame.height > max_frame_height || frame.stride < frame.width ||
        frame.capacity < size_t(frame.stride) * frame.height) return false;
    Canvas canvas{frame};
    int scale = frame.width >= 200 ? 2 : 1;
    canvas.rect(0, 0, frame.width, frame.height, background);
    Playback state = model.effective_state(now_ms);
    if (state == Playback::playing) {
        const auto& snapshot = model.snapshot;
        bool playlist = snapshot.context_type == Context::playlist && snapshot.context_name[0];
        text(canvas, 4 * scale, 3 * scale, playlist ? snapshot.context_name : snapshot.album, frame.width - 4 * scale, accent);
        if (playlist) text(canvas, 4 * scale, 12 * scale, snapshot.album, frame.width - 4 * scale, dim);
        int side = std::min(int(frame.width) * 3 / 5, int(frame.height) - 65 * scale);
        if (side <= 0) return false;
        int left = (frame.width - side - side/5 - side/3) / 2;
        int cy = 23 * scale + side/2, disc_x = left + side + side/5;
        canvas.circle(disc_x, cy, side/3, 0x2949);
        canvas.circle(disc_x, cy, side/4, 0x1084);
        canvas.circle(disc_x, cy, side/10, accent);
        cover(canvas, left, 23 * scale, side, snapshot.artwork_url[0] ? artwork : nullptr);
        text(canvas, 4 * scale, frame.height - 39 * scale, snapshot.title, frame.width - 4 * scale, ink);
        text(canvas, 4 * scale, frame.height - 30 * scale, snapshot.artist, frame.width - 4 * scale, dim);
        canvas.rect(4 * scale, frame.height - 20 * scale, frame.width - 8 * scale, 2 * scale, dim);
        if (snapshot.duration_ms) {
            int width = static_cast<int>(model.progress(now_ms) * (frame.width - 8 * scale) / snapshot.duration_ms);
            canvas.rect(4 * scale, frame.height - 20 * scale, width, 2 * scale, accent);
        }
    } else {
        for (int y = 14; y < frame.height - 16; ++y)
            for (int x = 2; x < frame.width - 2; ++x)
                if (noise(uint32_t(x / 2) + uint32_t(y / 2) * 997) % 733 == 0) canvas.pixel(x, y, dim);
        planet(canvas, model, now_ms);
        text(canvas, 4 * scale, 3 * scale, state == Playback::paused ? "PAUSED" : state == Playback::idle ? "ORBIT" : "OFFLINE", frame.width - 4 * scale, accent);
    }
    controls(canvas, model, now_ms);
    return true;
}

uint32_t frame_hash(const Frame& frame) {
    if (!frame.pixels || frame.stride < frame.width || frame.capacity < size_t(frame.stride) * frame.height) return 0;
    uint32_t hash = 2166136261U;
    for (size_t y = 0; y < frame.height; ++y) {
        for (size_t x = 0; x < frame.width; ++x) {
            uint16_t pixel = frame.pixels[y * frame.stride + x];
            hash = (hash ^ (pixel & 255)) * 16777619U;
            hash = (hash ^ (pixel >> 8)) * 16777619U;
        }
    }
    return hash;
}
} // namespace bottie
