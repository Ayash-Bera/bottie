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
            std::strcmp(track_,model.snapshot.track_id)) {
            waiting_=true; interaction.barrier(now);
        }
        initialized_=true; state_=state;
        std::memcpy(target_,model.snapshot.control_target,sizeof target_);
        std::memcpy(track_,model.snapshot.track_id,sizeof track_);
    }
    void before_input(Interaction& interaction,uint64_t now) const { if(waiting_) interaction.barrier(now); }
    void presented(Interaction& interaction,uint64_t now) {
        if(waiting_) { interaction.barrier(now); waiting_=false; }
    }
    bool waiting() const { return waiting_; }
private:
    bool initialized_{},waiting_{true};
    Playback state_{Playback::unavailable};
    char target_[33]{},track_[129]{};
};
template<class Pop, class Now, class Dispatch, class Brightness>
bool service_inputs(Interaction& interaction, const Model& model, uint16_t width, uint16_t height,
                    Pop pop, Now now, Dispatch dispatch, Brightness brightness, InputServiceStats& stats) {
    const auto before = interaction.settings;
    interaction.refresh(model,now());
    for (unsigned n = 0; n < 33; ++n) {
        const auto event = pop();
        if (event.type == EventType::none) break;
        const auto current = now();
        if (event.type == EventType::control) {
            Intent intent;
            if (model.intent(event.button,current,intent)) dispatch(intent);
        } else if (event.type == EventType::pointer) {
            ++stats.samples;
            if (current >= event.pointer.at_ms && current - event.pointer.at_ms < UINT32_MAX)
                stats.max_age_ms = std::max(stats.max_age_ms,static_cast<uint32_t>(current-event.pointer.at_ms));
            const auto action = interaction.update(event.pointer,model,current,width,height);
            if (action.control) dispatch(action.intent);
            if (action.brightness_changed) brightness(interaction.settings.brightness);
        }
    }
    interaction.expire(now());
    const auto& after = interaction.settings;
    return before.open != after.open || before.brightness != after.brightness || before.volume != after.volume ||
           before.volume_available != after.volume_available;
}
} // namespace bottie
