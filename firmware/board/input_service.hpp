#pragma once
#include "board.hpp"
#include <algorithm>
#include <cstring>

namespace bottie {
struct InputServiceStats { uint32_t samples{}, max_age_ms{}; };
class SceneInputFence {
public:
    void observe(const Model& model,uint64_t now,Interaction& interaction) {
        const auto state=model.effective_state(now);
        if (!initialized_ || state!=state_ || std::strcmp(target_,model.snapshot.control_target) ||
            std::strcmp(track_,model.snapshot.track_id) || std::strcmp(control_track_,model.snapshot.control_track)) {
            waiting_=true; interaction.barrier(now);
        }
        initialized_=true; state_=state;
        std::memcpy(target_,model.snapshot.control_target,sizeof target_);
        std::memcpy(track_,model.snapshot.track_id,sizeof track_);
        std::memcpy(control_track_,model.snapshot.control_track,sizeof control_track_);
    }
    void before_input(Interaction& interaction,uint64_t now) const { if(waiting_) interaction.barrier(now); }
    void presented(Interaction& interaction,uint64_t now) {
        if(waiting_) { interaction.barrier(now); waiting_=false; }
    }
    bool waiting() const { return waiting_; }
private:
    bool initialized_{},waiting_{true};
    Playback state_{Playback::unavailable};
    char target_[33]{},track_[129]{},control_track_[33]{};
};
struct IgnoreUiAction { void operator()(const UiAction&) const {} };
template<class Pop, class Now, class Dispatch, class Brightness, class Observe = IgnoreUiAction>
bool service_inputs(Interaction& interaction, const Model& model, uint16_t width, uint16_t height,
                    Pop pop, Now now, Dispatch dispatch, Brightness brightness, InputServiceStats& stats, Observe observe = {}) {
    const auto before = interaction.settings;
    interaction.refresh(model,now());
    for (unsigned n = 0; n < 33; ++n) {
        const auto event = pop();
        if (event.type == EventType::none) break;
        const auto current = now();
        if (event.type == EventType::control) {
            Intent intent;
            if (!interaction.refresh_pending() && model.intent(event.button,current,intent)) dispatch(intent);
        } else if (event.type == EventType::pointer) {
            ++stats.samples;
            if (current >= event.pointer.at_ms && current - event.pointer.at_ms < UINT32_MAX)
                stats.max_age_ms = std::max(stats.max_age_ms,static_cast<uint32_t>(current-event.pointer.at_ms));
            const auto action = interaction.update(event.pointer,model,current,width,height);
            if (action.control) dispatch(action.intent);
            if (action.brightness_changed) brightness(interaction.applied_brightness());
            observe(action);
        }
    }
    interaction.expire(now());
    if (interaction.tick_power(now())) brightness(interaction.applied_brightness());
    const auto& after = interaction.settings;
    return before.open != after.open || before.brightness != after.brightness || before.volume != after.volume ||
           before.volume_available != after.volume_available || before.dim_mode != after.dim_mode ||
           before.save_status != after.save_status || before.detail_open!=after.detail_open ||
           before.detail_sequence!=after.detail_sequence || before.detail_ready!=after.detail_ready ||
           before.seek_preview!=after.seek_preview || before.seek_position!=after.seek_position ||
           before.pressed_button!=after.pressed_button || before.feedback!=after.feedback ||
           before.feedback_error!=after.feedback_error;
}
} // namespace bottie
