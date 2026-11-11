#pragma once

#include "bottie/network.hpp"
#include "playback_cache.hpp"

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bottie {
struct RuntimeHealth {
    network::Status status{network::Status::disabled};
    uint32_t polls{}, accepted{}, rejected{}, stack_free{}, poll_ms{};
};

class LiveRuntime {
public:
    bool start();
    bool stop(uint32_t timeout_ms = 7000);
    void consume(Model& model);
    bool enqueue(const Intent& intent);
    const Artwork* artwork();
    RuntimeHealth health() const;
private:
    struct Control { char json[64]; uint64_t queued_ms; };
    static constexpr size_t stack_bytes = 24 * 1024;
    static void entry(void* self);
    void work();
    void publish(bool online, const Snapshot* snapshot = nullptr, const Artwork* art = nullptr, uint64_t observed_ms = 0);
    network::Client client_;
    CachedPlayback shared_, displayed_;
    Artwork displayed_view_;
    std::array<char, max_json_bytes + 1> body_{};
    std::array<StackType_t, stack_bytes / sizeof(StackType_t)> stack_{};
    std::array<uint8_t, 4 * sizeof(Control)> controls_{};
    StaticTask_t task_storage_{};
    StaticQueue_t queue_storage_{};
    StaticSemaphore_t mutex_storage_{}, stopped_storage_{};
    SemaphoreHandle_t mutex_{}, stopped_{};
    QueueHandle_t queue_{};
    std::atomic<bool> running_{false}, stopping_{false};
    std::atomic<network::Status> status_{network::Status::disabled};
    std::atomic<uint32_t> polls_{}, accepted_{}, rejected_{}, stack_free_{}, poll_ms_{};
    uint32_t published_{}, consumed_{};
    bool started_{};
};
} // namespace bottie
