#include "bottie/core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bottie {
namespace {
uint64_t elapsed(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
} // namespace

Error Model::ingest(const char* json, size_t bytes, uint64_t now_ms, uint64_t unix_ms) {
    Snapshot candidate;
    Error error = parse_snapshot(json, bytes, candidate);
    if (error != Error::none) { disconnected(); return error; }
    return apply_snapshot(candidate, now_ms, unix_ms);
}

Error Model::apply_snapshot(const Snapshot& candidate, uint64_t now_ms, uint64_t unix_ms) {
    if (received && candidate.updated_at_ms < snapshot.updated_at_ms) {
        disconnected();
        return Error::stale;
    }
    if (unix_ms && candidate.updated_at_ms > unix_ms && candidate.updated_at_ms - unix_ms > 5000) {
        disconnected();
        return Error::stale;
    }
    if (candidate.clock.valid() && (!clock_anchor.valid() || candidate.clock.unix_ms > clock_anchor.unix_ms ||
        (candidate.clock.unix_ms == clock_anchor.unix_ms && candidate.clock.utc_offset_min != clock_anchor.utc_offset_min))) {
        clock_anchor = candidate.clock;
        clock_received_ms = now_ms;
    }
    // A repeated cached snapshot must not renew freshness or restart progress.
    if (received && candidate.updated_at_ms == snapshot.updated_at_ms) {
        snapshot = candidate;
        online = true;
        return Error::none;
    }
    snapshot = candidate;
    received = online = true;
    received_ms = now_ms;
    age_at_receive_ms = unix_ms > candidate.updated_at_ms ? unix_ms - candidate.updated_at_ms : 0;
    return Error::none;
}

void Model::disconnected() { online = false; }

Playback Model::effective_state(uint64_t now_ms) const {
    if (!received || !online || now_ms < received_ms || age_at_receive_ms >= stale_after_ms ||
        elapsed(now_ms, received_ms) >= stale_after_ms - age_at_receive_ms)
        return Playback::unavailable;
    return snapshot.state;
}

uint64_t Model::progress(uint64_t now_ms) const {
    uint64_t value = snapshot.progress_ms;
    if (effective_state(now_ms) == Playback::playing)
        value += std::min(elapsed(now_ms, received_ms) + age_at_receive_ms, snapshot.duration_ms - value);
    return std::min(value, snapshot.duration_ms);
}

bool Model::intent(Button button, uint64_t now_ms, Intent& output) const {
    Playback state = effective_state(now_ms);
    if (state == Playback::unavailable || state == Playback::idle) return false;
    if (snapshot.target_known && !snapshot.control_target[0]) return false;
    const auto& caps = snapshot.capabilities;
    Intent result;
    switch (button) {
    case Button::play_pause:
        if (state == Playback::playing) {
            if (!caps.pause) return false;
            result.action = Action::pause;
        } else {
            if (!caps.play) return false;
            result.action = Action::play;
        }
        break;
    case Button::previous: if (!caps.previous) return false; result.action = Action::previous; break;
    case Button::next: if (!caps.next) return false; result.action = Action::next; break;
    case Button::shuffle:
        if (!caps.shuffle) return false;
        result.action = Action::shuffle;
        result.shuffle = !snapshot.shuffle;
        break;
    case Button::repeat:
        if (!caps.repeat) return false;
        result.action = Action::repeat;
        result.repeat = snapshot.repeat == Repeat::off ? Repeat::context :
                        snapshot.repeat == Repeat::context ? Repeat::track : Repeat::off;
        break;
    default: return false;
    }
    std::memcpy(result.control_target, snapshot.control_target, sizeof result.control_target);
    output = result;
    return true;
}

bool Model::resume_intent(uint64_t now_ms, Intent& output) const {
    const auto state = effective_state(now_ms);
    if ((state != Playback::paused && state != Playback::idle) || !snapshot.capabilities.play) return false;
    if (!snapshot.control_target[0]) return false;
    output = {}; output.action = Action::play;
    std::memcpy(output.control_target, snapshot.control_target, sizeof output.control_target);
    return true;
}

bool Model::volume_intent(uint8_t percent, uint64_t now, Intent& output) const {
    if (percent > 100 || effective_state(now) == Playback::unavailable || !snapshot.capabilities.volume ||
        !snapshot.control_target[0]) return false;
    output = {}; output.action = Action::volume; output.volume = percent;
    std::memcpy(output.control_target, snapshot.control_target, sizeof output.control_target);
    return true;
}
bool Model::seek_intent(uint32_t position,uint64_t now,Intent& output) const {
    const auto state=effective_state(now);
    if((state!=Playback::playing && state!=Playback::paused) || !snapshot.capabilities.seek ||
       !snapshot.control_target[0] || !snapshot.control_track[0] || !snapshot.duration_ms || position>86400000) return false;
    output={}; output.action=Action::seek;
    output.position_ms=static_cast<uint32_t>(std::min<uint64_t>(position,snapshot.duration_ms-1));
    std::memcpy(output.control_target,snapshot.control_target,sizeof output.control_target);
    std::memcpy(output.control_track,snapshot.control_track,sizeof output.control_track);
    return true;
}

bool Model::allows(const Intent& intent, uint64_t now) const {
    if (effective_state(now) == Playback::unavailable ||
        !intent.control_target[0] || !snapshot.control_target[0] ||
        std::strcmp(intent.control_target, snapshot.control_target) != 0) return false;
    const auto& cap = snapshot.capabilities;
    const auto state = effective_state(now);
    switch (intent.action) {
    case Action::play: return cap.play && (state == Playback::paused || state == Playback::idle);
    case Action::pause: return cap.pause && state == Playback::playing;
    case Action::previous: return cap.previous && state != Playback::idle;
    case Action::next: return cap.next && state != Playback::idle;
    case Action::shuffle: return cap.shuffle && state != Playback::idle;
    case Action::repeat: return cap.repeat && state != Playback::idle;
    case Action::volume: return cap.volume && intent.control_target[0] && intent.volume <= 100;
    case Action::seek: return cap.seek && state!=Playback::idle && intent.control_track[0] &&
        !std::strcmp(intent.control_track,snapshot.control_track) && snapshot.duration_ms && intent.position_ms<snapshot.duration_ms;
    }
    return false;
}

bool encode_intent(const Intent& intent, char* output, size_t capacity) {
    if (!output || capacity == 0) return false;
    const char* action;
    switch (intent.action) {
    case Action::play: action = "play"; break;
    case Action::pause: action = "pause"; break;
    case Action::previous: action = "previous"; break;
    case Action::next: action = "next"; break;
    case Action::shuffle: action = "shuffle"; break;
    case Action::repeat: action = "repeat"; break;
    case Action::volume: action = "volume"; break;
    case Action::seek: action = "seek"; break;
    default: output[0] = 0; return false;
    }
    int length;
    if (intent.action == Action::shuffle)
        length = std::snprintf(output, capacity, "{\"action\":\"shuffle\",\"value\":%s}", intent.shuffle ? "true" : "false");
    else if (intent.action == Action::repeat) {
        const char* repeat;
        switch (intent.repeat) {
        case Repeat::off: repeat = "off"; break;
        case Repeat::context: repeat = "context"; break;
        case Repeat::track: repeat = "track"; break;
        default: output[0] = 0; return false;
        }
        length = std::snprintf(output, capacity, "{\"action\":\"repeat\",\"value\":\"%s\"}", repeat);
    } else if (intent.action == Action::volume) {
        if (!intent.control_target[0] || intent.volume > 100) { output[0] = 0; return false; }
        length = std::snprintf(output, capacity, "{\"action\":\"volume\",\"value\":%u}", unsigned(intent.volume));
    } else if(intent.action==Action::seek) {
        if(!intent.control_target[0] || intent.position_ms>86400000 ||
           !std::memchr(intent.control_track,0,sizeof intent.control_track) || std::strlen(intent.control_track)!=32) {
            output[0]=0; return false;
        }
        for(char c:intent.control_track) if(c && !((c>='0'&&c<='9')||(c>='a'&&c<='f'))) { output[0]=0; return false; }
        length=std::snprintf(output,capacity,"{\"action\":\"seek\",\"value\":%u,\"control_track\":\"%s\"}",
            unsigned(intent.position_ms),intent.control_track);
    } else length = std::snprintf(output, capacity, "{\"action\":\"%s\"}", action);
    if(intent.action!=Action::seek && intent.control_track[0]) { output[0]=0; return false; }
    if (length < 0 || static_cast<size_t>(length) >= capacity) { output[0] = 0; return false; }
    if (intent.control_target[0]) {
        if (!std::memchr(intent.control_target, 0, sizeof intent.control_target) || std::strlen(intent.control_target) != 32) {
            output[0] = 0; return false;
        }
        for (char c : intent.control_target) {
            if (c && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { output[0] = 0; return false; }
        }
        const int extra = std::snprintf(output + length - 1, capacity - length + 1,
                                        ",\"control_target\":\"%s\"}", intent.control_target);
        if (extra < 0 || static_cast<size_t>(length - 1 + extra) >= capacity) { output[0] = 0; return false; }
    }
    return true;
}

int Model::clock_minute(uint64_t now) const {
    if (!clock_anchor.valid() || now < clock_received_ms || now - clock_received_ms > clock_max_ms - clock_anchor.unix_ms) return -1;
    const int64_t minutes = static_cast<int64_t>((clock_anchor.unix_ms + now - clock_received_ms) / 60000) + clock_anchor.utc_offset_min;
    return static_cast<int>((minutes % 1440 + 1440) % 1440);
}
} // namespace bottie
