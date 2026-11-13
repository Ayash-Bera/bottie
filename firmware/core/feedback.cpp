#include "bottie/feedback.hpp"
#include <cstring>

namespace bottie {
void Feedback::begin(const Intent& intent,const Model& model,uint64_t now) {
    intent_=intent; std::memcpy(track_,model.snapshot.track_id,sizeof track_);
    began_=now; until_=now+(intent.action==Action::seek?1000:2000);
    if(intent.action==Action::seek && intent.release_known) until_=intent.released_ms+1000;
    observation_=model.snapshot.updated_at_ms; phase_=FeedbackPhase::queued; error_=ControlError::none;
}
void Feedback::result(const ControlOutcome& outcome,uint64_t now) {
    if(phase_==FeedbackPhase::none || phase_==FeedbackPhase::canceled || phase_==FeedbackPhase::expired ||
       outcome.intent.sequence!=intent_.sequence || outcome.intent.action!=intent_.action ||
       std::strcmp(outcome.intent.control_target,intent_.control_target) ||
       std::strcmp(outcome.intent.control_track,intent_.control_track) || now<began_ || now-outcome.at_ms>2000) return;
    phase_=outcome.reply.phase; error_=outcome.reply.error; until_=now+(phase_==FeedbackPhase::sending?7000:2000);
    if(intent_.action==Action::seek && outcome.reply.has_position) intent_.position_ms=outcome.reply.position_ms;
}
void Feedback::update(const Model& model,uint64_t now) {
    if(phase_==FeedbackPhase::none) return;
    if(now<began_ || model.effective_state(now)==Playback::unavailable ||
       std::strcmp(intent_.control_target,model.snapshot.control_target) || std::strcmp(track_,model.snapshot.track_id) ||
       (intent_.action==Action::seek && std::strcmp(intent_.control_track,model.snapshot.control_track))) {
        phase_=FeedbackPhase::none; return;
    }
    if(phase_==FeedbackPhase::sent && model.snapshot.updated_at_ms>observation_) {
        bool matched=false;
        switch(intent_.action) {
        case Action::play: matched=model.snapshot.state==Playback::playing; break;
        case Action::pause: matched=model.snapshot.state==Playback::paused; break;
        case Action::shuffle: matched=model.snapshot.shuffle==intent_.shuffle; break;
        case Action::repeat: matched=model.snapshot.repeat==intent_.repeat; break;
        case Action::volume: matched=model.snapshot.volume_percent==intent_.volume; break;
        case Action::seek: {
            const auto position=model.snapshot.progress_ms;
            matched=position>=intent_.position_ms && position-intent_.position_ms<=2000; break;
        }
        default: break;
        }
        if(matched) { phase_=FeedbackPhase::confirmed; until_=now+2000; }
    }
    if(now>=until_) {
        if(phase_==FeedbackPhase::queued) { phase_=FeedbackPhase::expired; until_=now+2000; }
        else if(phase_==FeedbackPhase::sending) { phase_=FeedbackPhase::unknown; until_=now+2000; }
        else phase_=FeedbackPhase::none;
    }
}
} // namespace bottie
