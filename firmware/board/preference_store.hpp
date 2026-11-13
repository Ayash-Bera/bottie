#pragma once

#include "bottie/preferences.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>

namespace bottie {
class PreferenceStore : private PreferenceBackend {
public:
    void start(PreferenceState& state);
    bool submit(const PreferenceWrite& write);
    bool result(PreferenceWrite& write, StoreResult& result);
    uint32_t stack_free() const { return stack_free_; }
    uint32_t writes() const { return writes_; }
    uint32_t errors() const { return errors_; }
    int32_t heap_delta() const { return heap_delta_; }
    int32_t sdk_result() const { return sdk_result_; }
private:
    static void entry(void* self);
    StoreResult initialize() override;
    StoreResult read(PreferenceBytes& bytes,size_t& size) override;
    StoreResult write(Preferences value) override;
    std::array<StackType_t,3072/sizeof(StackType_t)> stack_{};
    StaticTask_t task_storage_{};
    StaticSemaphore_t mutex_storage_{};
    SemaphoreHandle_t mutex_{};
    TaskHandle_t task_{}, consumer_{};
    PreferenceWrite pending_{};
    StoreResult result_{StoreResult::unavailable};
    bool busy_{}, complete_{};
    std::atomic<uint32_t> stack_free_{};
    std::atomic<uint32_t> writes_{},errors_{};
    std::atomic<int32_t> sdk_result_{};
    int32_t heap_delta_{};
};
} // namespace bottie
