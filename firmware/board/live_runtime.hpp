#pragma once

#include "bottie/network.hpp"
#include "playback_cache.hpp"
#include "control_mailbox.hpp"

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bottie {
struct RuntimeHealth {
    network::Status status{network::Status::disabled};
    uint32_t polls{}, accepted{}, rejected{}, stack_free{}, poll_ms{}, controls{}, control_errors{};
};

class LiveRuntime {
public:
    bool start();
    bool stop(uint32_t timeout_ms = 7000);
    void consume(Model& model);
    bool enqueue(const Intent& intent);
    const Artwork* artwork();
    TextMask text_mask() const;
    uint32_t artwork_revision() const { return displayed_.art_revision; }
    uint32_t text_revision() const { return displayed_.text_revision; }
    RuntimeHealth health() const;
private:
    static constexpr size_t stack_bytes = 24 * 1024;
    static void entry(void* self);
    void work();
    void publish(bool online, const Snapshot* snapshot = nullptr, const Artwork* art = nullptr,
                 uint64_t observed_ms = 0, const TextMask* mask = nullptr);
    network::Client client_;
    CachedPlayback shared_, displayed_;
    Artwork displayed_view_;
    std::array<char, max_json_bytes + 1> body_{};
    std::array<StackType_t, stack_bytes / sizeof(StackType_t)> stack_{};
    ControlMailbox controls_;
    Model worker_model_;
    StaticTask_t task_storage_{};
    StaticSemaphore_t mutex_storage_{}, stopped_storage_{};
    SemaphoreHandle_t mutex_{}, stopped_{};
    std::atomic<bool> running_{false}, stopping_{false};
    std::atomic<network::Status> status_{network::Status::disabled};
    std::atomic<uint32_t> polls_{}, accepted_{}, rejected_{}, stack_free_{}, poll_ms_{};
    std::atomic<uint32_t> sent_{}, control_errors_{};
    uint32_t published_{}, consumed_{};
    bool started_{};
    TaskHandle_t consumer_{};
    std::atomic<TaskHandle_t> worker_{};
};
} // namespace bottie
