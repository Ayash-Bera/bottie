#include "bottie/core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bottie {
namespace {
uint64_t elapsed(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
void advance_planet(Model& model, uint64_t now) {
    uint32_t seed = model.planet_seed;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    model.planet_seed = seed ? seed : 0x74b39ac1U;
    model.planet_changed_ms = now;
}
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
    if (snapshot.state == Playback::playing) planet_changed_ms = now_ms;
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
    } else length = std::snprintf(output, capacity, "{\"action\":\"%s\"}", action);
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

void Model::tick(uint64_t now_ms) {
    if (effective_state(now_ms) == Playback::playing) {
        planet_changed_ms = now_ms;
    } else if (now_ms < planet_changed_ms) {
        planet_changed_ms = now_ms;
    } else if (elapsed(now_ms, planet_changed_ms) >= planet_period_ms) advance_planet(*this, now_ms);
}

bool Model::tap_planet(uint64_t now_ms) {
    if (effective_state(now_ms) == Playback::playing) return false;
    advance_planet(*this, now_ms);
    return true;
}
} // namespace bottie
