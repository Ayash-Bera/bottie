#pragma once
#include <algorithm>
#include <cstdint>

namespace bottie {
enum class WorkerWork { control, metadata, asset, wait };
class WorkSchedule {
public:
    WorkerWork next(uint64_t now, bool pending) const {
        if (pending) return WorkerWork::control;
        if (now >= poll_at_) return WorkerWork::metadata;
        if (now >= asset_at_) return WorkerWork::asset;
        return WorkerWork::wait;
    }
    void polled(uint64_t now) { poll_at_ = now + 1000; asset_at_ = now; }
    void asset_attempted(uint64_t now) { asset_at_ = now + 250; }
    void control_attempted(uint64_t now) { poll_at_ = now; }
    uint32_t wait_ms(uint64_t now) const {
        const auto deadline = std::min(poll_at_, asset_at_);
        return deadline <= now ? 1 : static_cast<uint32_t>(std::min<uint64_t>(100, deadline - now));
    }
private:
    uint64_t poll_at_{}, asset_at_{};
};
} // namespace bottie
