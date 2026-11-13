#include "bottie/core.hpp"
#include "bottie/text_mask.hpp"
#include "bottie/dirty.hpp"
#include "input_service.hpp"
#include "frame_diagnostics.hpp"
#include "simulated.hpp"
#if BOTTIE_LIVE_COMPANION
#include "live_runtime.hpp"
#endif
#if BOTTIE_RECOVERED_PANEL
#include "recovered_spi.hpp"
#include "preference_store.hpp"
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
    static bottie::Interaction interaction;
    static bottie::DirtyBands dirty;
    static bottie::InputServiceStats input_stats;
    static bottie::SceneInputFence input_fence;
    static bottie::PreferenceState preferences;
#if BOTTIE_RECOVERED_PANEL
    static bottie::RecoveredBoard board;
    static bottie::PreferenceStore preference_store;
#else
    static bottie::SimulatedBoard board;
#endif
#if BOTTIE_LIVE_COMPANION
    constexpr uint16_t stored_rows = 16;
    static bottie::LiveRuntime live;
#else
    constexpr uint16_t stored_rows = bottie::DirtyBands::band_rows;
    static std::array<char, bottie::max_json_bytes + 1> json;
#endif
    static std::array<uint16_t, BOTTIE_FRAME_WIDTH * stored_rows> pixels;
    bottie::Frame frame{pixels.data(), pixels.size(), BOTTIE_FRAME_WIDTH, BOTTIE_FRAME_HEIGHT, BOTTIE_FRAME_WIDTH};
#if BOTTIE_RECOVERED_PANEL
    preference_store.start(preferences);
    interaction.load_preferences(preferences.value(),board.monotonic_ms());
    std::puts("Cgotchi profile: documented CST810 input and PWM; PSRAM disabled.");
    if (!board.initialize()) { std::puts("Recovered panel initialization stopped"); return; }
    if (!board.brightness(interaction.applied_brightness())) std::puts("BOTTIE_BRIGHTNESS write_error=1");
#else
    std::puts("Bottie headless output. No display, touch or PSRAM driver is configured.");
#endif
#if BOTTIE_LIVE_COMPANION
    std::puts("Explicit live companion mode; network and touch sampling use background tasks.");
    if (!live.start()) std::puts("Companion task unavailable; continuing local animation");
#else
    std::puts("Synthetic playback and input profile");
    uint64_t poll_at = 0;
#endif
    uint64_t report_at = 0, frame_count = 0, frame_errors = 0, max_frame_us = 0;
    bool diagnosed_idle = false, diagnosed_playing = false;
    uint64_t bands = 0, pixel_bytes = 0, render_us = 0, present_us = 0;
    for (;;) {
        const uint64_t started_us = esp_timer_get_time();
        uint64_t now = board.monotonic_ms();
#if BOTTIE_LIVE_COMPANION
        live.consume(model);
        bottie::ControlOutcome outcome;
        if(live.outcome(outcome)) interaction.outcome(outcome,now);
#else
        if (now >= poll_at) {
            size_t length = 0;
            if (board.playback(json.data(), json.size(), length)) {
                auto result = model.ingest(json.data(), length, now);
                if (result != bottie::Error::none) std::printf("Snapshot rejected: %s\n", bottie::error_name(result));
            } else model.disconnected();
            poll_at = now + 1000;
        }
#endif
        now = board.monotonic_ms();
#if BOTTIE_RECOVERED_PANEL
        bottie::PreferenceWrite completed;
        bottie::StoreResult stored;
        if (preference_store.result(completed,stored)) preferences.complete(completed,stored);
#endif
        interaction.settings.save_status = preferences.status();
        input_fence.observe(model,now,interaction);
        auto dispatch = [&](bottie::Intent intent) {
            if(!intent.sequence) interaction.queued(intent,model,board.monotonic_ms());
#if BOTTIE_LIVE_COMPANION
            if (!live.enqueue(intent)) {
                std::puts("BOTTIE_CONTROL queue_rejected=1");
                interaction.outcome({intent,{bottie::FeedbackPhase::failed},board.monotonic_ms()},board.monotonic_ms());
            }
#else
            char body[256];
            if (bottie::encode_intent(intent, body, sizeof body) && !board.control(body, std::strlen(body))) model.disconnected();
#endif
        };
        auto input = [&]() {
            input_fence.before_input(interaction,board.monotonic_ms());
            const bool changed=bottie::service_inputs(interaction,model,frame.width,frame.height,
                [&]() {
#if BOTTIE_LIVE_COMPANION && !BOTTIE_RECOVERED_PANEL
                    return bottie::InputEvent{};
#else
                    return board.input();
#endif
                },[&]() { return board.monotonic_ms(); },dispatch,[&](uint8_t value) {
#if BOTTIE_RECOVERED_PANEL
                    if (!board.brightness(value)) std::puts("BOTTIE_BRIGHTNESS write_error=1");
#else
                    (void)value;
#endif
                },input_stats,[&](const bottie::UiAction& action) {
                    preferences.change({interaction.settings.brightness,interaction.settings.dim_mode});
                    if (action.preferences_released) preferences.release(board.monotonic_ms());
#if BOTTIE_LIVE_COMPANION
                    if (action.wake || action.refresh) live.refresh();
#endif
                });
#if BOTTIE_LIVE_COMPANION
            live.select_detail(interaction.detail_selection());
#endif
            return changed;
        };
        input();
#if BOTTIE_RECOVERED_PANEL
        bottie::PreferenceWrite pending;
        if (preferences.take(board.monotonic_ms(),pending) && !preference_store.submit(pending))
            preferences.complete(pending,bottie::StoreResult::write_error);
#endif
        interaction.settings.save_status = preferences.status();
#if BOTTIE_LIVE_COMPANION
        const bottie::Artwork* artwork = live.artwork();
        const auto text_mask = live.text_mask();
        const auto detail=live.detail(model.snapshot,interaction.detail_selection());
#else
        const bottie::Artwork* artwork = nullptr;
        const bottie::TextMask text_mask{};
        const bottie::DetailPage detail{};
#endif
        interaction.settings.detail_ready=detail.pixels!=nullptr;
        const auto settings = interaction.settings;
#if BOTTIE_LIVE_COMPANION
        dirty.prepare(model,now,settings,live.artwork_revision(),live.text_revision(),frame.width,frame.height);
#else
        dirty.prepare(model,now,settings,0,0,frame.width,frame.height);
#endif
        const bool playing = model.effective_state(now) == bottie::Playback::playing;
        const bool diagnose = (!playing && !diagnosed_idle) || (playing && artwork && !diagnosed_playing);
        bottie::PixelDiagnostics frame_diagnostics;
        if (diagnose) {
            dirty.invalidate();
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
        bool interrupted = false, drew = false;
        for (uint16_t y = 0; y < frame.height; y += stored_rows) {
            if (!dirty.dirty(y)) continue;
            drew = true;
            const auto render_start = esp_timer_get_time();
            frame.storage_y = y;
            frame.storage_height = std::min(stored_rows, static_cast<uint16_t>(frame.height - y));
            bool rendered = bottie::render(model, now, frame, artwork, &text_mask);
            if (rendered) rendered = bottie::render_settings(model, now, frame, settings,&detail);
            render_us += esp_timer_get_time() - render_start;
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
            const auto present_start = esp_timer_get_time();
            const bool presented = rendered && board.present(frame);
            present_us += esp_timer_get_time() - present_start;
            dirty.complete(y,presented);
            if (!presented) {
                std::puts("Frame rejected");
                ++frame_errors;
                break;
            }
            ++bands; pixel_bytes += uint64_t(frame.width) * frame.rows() * 2;
            if (input()) { interrupted = true; break; }
        }
        if (drew) ++frame_count;
        if (!dirty.any()) input_fence.presented(interaction,board.monotonic_ms());
        max_frame_us = std::max(max_frame_us, static_cast<uint64_t>(esp_timer_get_time()) - started_us);
        if (now >= report_at) {
            constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            std::printf("BOTTIE_HEALTH uptime_ms=%llu frames=%llu frame_errors=%llu max_frame_us=%llu heap=%u min_heap=%u largest=%u main_stack_free=%u state=%s art=%u text_flags=%u text_bytes=%u\n",
                static_cast<unsigned long long>(now), static_cast<unsigned long long>(frame_count),
                static_cast<unsigned long long>(frame_errors), static_cast<unsigned long long>(max_frame_us),
                static_cast<unsigned>(heap_caps_get_free_size(caps)), static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(caps)), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                bottie::playback_name(model.effective_state(now)), artwork ? 1U : 0U,
                static_cast<unsigned>(text_mask.flags), static_cast<unsigned>(text_mask.length));
#if BOTTIE_LIVE_COMPANION
            const auto health = live.health();
            std::printf("BOTTIE_NETWORK status=%u polls=%u accepted=%u rejected=%u stack_free=%u poll_ms=%u controls=%u control_errors=%u\n",
                static_cast<unsigned>(health.status), static_cast<unsigned>(health.polls), static_cast<unsigned>(health.accepted),
                static_cast<unsigned>(health.rejected), static_cast<unsigned>(health.stack_free), static_cast<unsigned>(health.poll_ms),
                static_cast<unsigned>(health.controls), static_cast<unsigned>(health.control_errors));
#endif
#if BOTTIE_RECOVERED_PANEL
            const auto input = board.input_health();
            std::printf("BOTTIE_TOUCH samples=%u valid=%u errors=%u overflows=%u max_gap_ms=%u max_read_us=%u brightness=%u settings=%u\n",
                        unsigned(input.samples), unsigned(input.valid), unsigned(input.errors), unsigned(input.overflows),
                        unsigned(input.max_gap_ms), unsigned(input.max_read_us), unsigned(settings.brightness), unsigned(settings.open));
            std::printf("BOTTIE_PREFS preferred=%u applied=%u dim=%u dim_mode=%u save=%u stack_free=%u writes=%u errors=%u last_write_us=%u max_write_us=%u init_heap=%d sdk=%d\n",
                unsigned(settings.brightness),unsigned(interaction.applied_brightness()),unsigned(interaction.dimmed()),
                unsigned(settings.dim_mode),unsigned(preferences.status()),unsigned(preference_store.stack_free()),unsigned(preference_store.writes()),
                unsigned(preference_store.errors()),unsigned(preference_store.last_write_us()),unsigned(preference_store.max_write_us()),
                int(preference_store.heap_delta()),int(preference_store.sdk_result()));
#endif
            std::printf("BOTTIE_REDRAW bands=%llu pixel_bytes=%llu render_us=%llu present_us=%llu input_samples=%u max_input_age_ms=%u clock_minute=%d\n",
                static_cast<unsigned long long>(bands),static_cast<unsigned long long>(pixel_bytes),
                static_cast<unsigned long long>(render_us),static_cast<unsigned long long>(present_us),
                unsigned(input_stats.samples),unsigned(input_stats.max_age_ms),model.clock_minute(now));
            report_at = now + 10000;
        }
        auto wait_ms = std::min<uint32_t>(playing ? 50 : 1000,interaction.power_wait_ms(board.monotonic_ms()));
        if (preferences.status() == bottie::SaveStatus::pending) wait_ms = std::min<uint32_t>(wait_ms,100);
        if (!interrupted) ulTaskNotifyTake(pdTRUE,std::max<TickType_t>(1,pdMS_TO_TICKS(wait_ms)));
    }
}
