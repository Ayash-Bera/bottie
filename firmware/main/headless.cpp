#include "bottie/core.hpp"
#include "simulated.hpp"
#if BOTTIE_LIVE_COMPANION
#include "live_runtime.hpp"
#endif
#if BOTTIE_RECOVERED_PANEL
#include "recovered_spi.hpp"
#endif

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef BOTTIE_FRAME_WIDTH
#define BOTTIE_FRAME_WIDTH 128
#define BOTTIE_FRAME_HEIGHT 128
#endif

extern "C" void app_main(void) {
    static bottie::Model model;
#if BOTTIE_RECOVERED_PANEL
    static bottie::RecoveredBoard board;
#else
    static bottie::SimulatedBoard board;
#endif
#if BOTTIE_LIVE_COMPANION
    constexpr uint16_t stored_rows = 16;
    static bottie::LiveRuntime live;
#else
    constexpr uint16_t stored_rows = BOTTIE_FRAME_HEIGHT;
    static std::array<char, bottie::max_json_bytes + 1> json;
#endif
    static std::array<uint16_t, BOTTIE_FRAME_WIDTH * stored_rows> pixels;
    bottie::Frame frame{pixels.data(), pixels.size(), BOTTIE_FRAME_WIDTH, BOTTIE_FRAME_HEIGHT, BOTTIE_FRAME_WIDTH};
#if BOTTIE_RECOVERED_PANEL
    std::puts("Recovered panel profile: physical behavior unverified; no touch or PSRAM driver.");
    if (!board.initialize()) { std::puts("Recovered panel initialization stopped"); return; }
#else
    std::puts("Bottie headless output. No display, touch or PSRAM driver is configured.");
#endif
#if BOTTIE_LIVE_COMPANION
    std::puts("Explicit live companion mode; network runs in a background task. Input is unconfigured.");
    if (!live.start()) std::puts("Companion task unavailable; continuing local animation");
#else
    std::puts("Synthetic playback and input profile");
    uint64_t poll_at = 0;
#endif
    for (;;) {
        uint64_t now = board.monotonic_ms();
#if BOTTIE_LIVE_COMPANION
        live.consume(model);
        bottie::InputEvent event{};
#else
        if (now >= poll_at) {
            size_t length = 0;
            if (board.playback(json.data(), json.size(), length)) {
                auto result = model.ingest(json.data(), length, now);
                if (result != bottie::Error::none) std::printf("Snapshot rejected: %s\n", bottie::error_name(result));
            } else model.disconnected();
            poll_at = now + 1000;
        }
        auto event = board.input();
#endif
        if (event.type == bottie::EventType::planet_tap) model.tap_planet(now);
        if (event.type == bottie::EventType::control) {
            bottie::Intent intent;
#if BOTTIE_LIVE_COMPANION
            if (model.intent(event.button, now, intent) && !live.enqueue(intent)) model.disconnected();
#else
            char body[64];
            if (model.intent(event.button, now, intent) && bottie::encode_intent(intent, body, sizeof body) &&
                !board.control(body, std::strlen(body))) model.disconnected();
#endif
        }
        model.tick(now);
#if BOTTIE_LIVE_COMPANION
        const bottie::Artwork* artwork = live.artwork();
#else
        const bottie::Artwork* artwork = nullptr;
#endif
        for (uint16_t y = 0; y < frame.height; y += stored_rows) {
            frame.storage_y = y;
            frame.storage_height = std::min(stored_rows, static_cast<uint16_t>(frame.height - y));
            if (!bottie::render(model, now, frame, artwork) || !board.present(frame)) {
                std::puts("Frame rejected");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
