#include "bottie/core.hpp"
#include "simulated.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
    static bottie::Model model;
    static bottie::SimulatedBoard board;
    static std::array<uint16_t, 128 * 128> pixels;
    static std::array<char, bottie::max_json_bytes + 1> json;
    bottie::Frame frame{pixels.data(), pixels.size(), 128, 128, 128};
    std::puts("Bottie simulated headless profile. No display, touch, network or PSRAM driver is configured.");
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
