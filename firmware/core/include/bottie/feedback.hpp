#pragma once
#include "bottie/core.hpp"

namespace bottie {
enum class FeedbackPhase : uint8_t { none, queued, sending, sent, confirmed, failed, expired, canceled, unknown };
enum class ControlError : uint8_t { none, target_changed, track_changed, unsupported, not_authorized,
                          keyring_locked, keyring_unavailable, rate_limited, upstream_error };
struct ControlReply {
    FeedbackPhase phase{FeedbackPhase::unknown};
    ControlError error{ControlError::none};
    uint32_t position_ms{};
    bool has_position{};
};
bool parse_control_reply(const uint8_t* bytes,size_t length,unsigned http_status,ControlReply& reply);
struct ControlOutcome { Intent intent{}; ControlReply reply{}; uint64_t at_ms{}; };
class Feedback {
public:
    void begin(const Intent& intent,const Model& model,uint64_t now);
    void result(const ControlOutcome& outcome,uint64_t now);
    void update(const Model& model,uint64_t now);
    FeedbackPhase phase() const { return phase_; }
    ControlError error() const { return error_; }
private:
    Intent intent_{};
    char track_[129]{};
    uint64_t began_{}, until_{}, observation_{};
    FeedbackPhase phase_{FeedbackPhase::none};
    ControlError error_{ControlError::none};
};
} // namespace bottie
