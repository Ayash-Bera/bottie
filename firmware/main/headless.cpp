#include "bottie/core.hpp"
#include "frame_diagnostics.hpp"
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
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#ifndef BOTTIE_FRAME_WIDTH
#define BOTTIE_FRAME_WIDTH 128
#define BOTTIE_FRAME_HEIGHT 128
#endif

extern "C" void app_main(void) {
    std::printf("BOTTIE_BOOT reset_reason=%d\n", static_cast<int>(esp_reset_reason()));
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
    uint64_t report_at = 0, frame_count = 0, frame_errors = 0, max_frame_us = 0;
    bool diagnosed_idle = false, diagnosed_playing = false;
    for (;;) {
        const uint64_t started_us = esp_timer_get_time();
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
        const bool playing = model.effective_state(now) == bottie::Playback::playing;
        const bool diagnose = (!playing && !diagnosed_idle) || (playing && artwork && !diagnosed_playing);
        bottie::PixelDiagnostics frame_diagnostics;
        if (diagnose) {
            std::printf("BOTTIE_FRAME playing=%u\n", static_cast<unsigned>(playing));
            if (playing) {
                diagnosed_playing = true;
                bottie::WireDiagnostics art_diagnostics;
                art_diagnostics.add(artwork->pixels, size_t(artwork->stride) * artwork->height);
                std::printf("BOTTIE_ART bytes=%u first0=%u first1=%u hash=%u\n",
                    static_cast<unsigned>(art_diagnostics.bytes), static_cast<unsigned>(art_diagnostics.first0),
                    static_cast<unsigned>(art_diagnostics.first1), static_cast<unsigned>(art_diagnostics.hash));
            } else diagnosed_idle = true;
#if BOTTIE_RECOVERED_PANEL
            board.diagnose_frame();
#endif
        }
        for (uint16_t y = 0; y < frame.height; y += stored_rows) {
            frame.storage_y = y;
            frame.storage_height = std::min(stored_rows, static_cast<uint16_t>(frame.height - y));
            bool rendered = bottie::render(model, now, frame, artwork);
            if (rendered && diagnose) {
                bottie::PixelDiagnostics band;
                band.add(frame);
                frame_diagnostics.add(frame);
                std::printf("BOTTIE_PIXELS y=%u rows=%u first=%u min=%u max=%u different=%u hash=%u wire=%u cumulative=%u\n",
                    static_cast<unsigned>(y), static_cast<unsigned>(frame.rows()), static_cast<unsigned>(band.first),
                    static_cast<unsigned>(band.minimum), static_cast<unsigned>(band.maximum), static_cast<unsigned>(band.different),
                    static_cast<unsigned>(band.little_hash), static_cast<unsigned>(band.wire_hash),
                    static_cast<unsigned>(frame_diagnostics.wire_hash));
            }
            if (!rendered || !board.present(frame)) {
                std::puts("Frame rejected");
                ++frame_errors;
                break;
            }
        }
        ++frame_count;
        max_frame_us = std::max(max_frame_us, static_cast<uint64_t>(esp_timer_get_time()) - started_us);
        if (now >= report_at) {
            constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            std::printf("BOTTIE_HEALTH uptime_ms=%llu frames=%llu frame_errors=%llu max_frame_us=%llu heap=%u min_heap=%u largest=%u main_stack_free=%u state=%s art=%u\n",
                static_cast<unsigned long long>(now), static_cast<unsigned long long>(frame_count),
                static_cast<unsigned long long>(frame_errors), static_cast<unsigned long long>(max_frame_us),
                static_cast<unsigned>(heap_caps_get_free_size(caps)), static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(caps)), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                bottie::playback_name(model.effective_state(now)), artwork ? 1U : 0U);
#if BOTTIE_LIVE_COMPANION
            const auto health = live.health();
            std::printf("BOTTIE_NETWORK status=%u polls=%u accepted=%u rejected=%u stack_free=%u poll_ms=%u\n",
                static_cast<unsigned>(health.status), static_cast<unsigned>(health.polls), static_cast<unsigned>(health.accepted),
                static_cast<unsigned>(health.rejected), static_cast<unsigned>(health.stack_free), static_cast<unsigned>(health.poll_ms));
#endif
            report_at = now + 10000;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
