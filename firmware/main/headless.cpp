#include "bottie/core.hpp"
#include "simulated.hpp"
#if BOTTIE_RECOVERED_PANEL
#include "recovered_spi.hpp"
#endif

#include <array>
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
    static std::array<uint16_t, BOTTIE_FRAME_WIDTH * BOTTIE_FRAME_HEIGHT> pixels;
    static std::array<char, bottie::max_json_bytes + 1> json;
    bottie::Frame frame{pixels.data(), pixels.size(), BOTTIE_FRAME_WIDTH, BOTTIE_FRAME_HEIGHT, BOTTIE_FRAME_WIDTH};
#if BOTTIE_RECOVERED_PANEL
    std::puts("Recovered panel profile: physical behavior unverified; synthetic playback, no touch/network/PSRAM.");
    if (!board.initialize()) { std::puts("Recovered panel initialization stopped"); return; }
#else
    std::puts("Bottie simulated headless profile. No display, touch, network or PSRAM driver is configured.");
#endif
    uint64_t poll_at = 0;
    for (;;) {
        uint64_t now = board.monotonic_ms();
        if (now >= poll_at) {
            size_t length = 0;
            if (board.playback(json.data(), json.size(), length)) {
                auto result = model.ingest(json.data(), length, now);
                if (result != bottie::Error::none) std::printf("Snapshot rejected: %s\n", bottie::error_name(result));
            } else model.disconnected();
            poll_at = now + 1000;
        }
        auto event = board.input();
        if (event.type == bottie::EventType::planet_tap) model.tap_planet(now);
        if (event.type == bottie::EventType::control) {
            bottie::Intent intent;
            char body[64];
            if (model.intent(event.button, now, intent) && bottie::encode_intent(intent, body, sizeof body) &&
                !board.control(body, std::strlen(body))) model.disconnected();
        }
        model.tick(now);
        if (!bottie::render(model, now, frame) || !board.present(frame)) std::puts("Headless frame rejected");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
