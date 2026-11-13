#pragma once

#include "touch_device.hpp"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace bottie {
struct InputHealth { uint32_t samples{}, valid{}, errors{}, overflows{}, max_gap_ms{}, max_read_us{}; };
class CgotchiInput final : private TouchBus {
public:
    ~CgotchiInput();
    bool initialize();
    bool pop(PointerSample& sample, uint64_t now) { return queue_.pop(sample, now); }
    InputHealth health() const;
private:
    bool read(uint8_t reg, uint8_t* output, size_t bytes, int timeout_ms) override;
    static void entry(void* self);
    void work();
    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    PointerQueue queue_;
    std::atomic<bool> running_{};
    std::atomic<uint32_t> samples_{}, valid_{}, errors_{}, overflows_{}, max_gap_ms_{}, max_read_us_{};
    std::array<StackType_t, 4096 / sizeof(StackType_t)> stack_{};
    StaticTask_t task_{};
    StaticSemaphore_t stopped_storage_{};
    SemaphoreHandle_t stopped_{};
    TaskHandle_t consumer_{};
};
} // namespace bottie
