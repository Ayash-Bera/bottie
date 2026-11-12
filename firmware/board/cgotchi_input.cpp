#include "cgotchi_input.hpp"

#include <algorithm>
#include <cstdio>
#include "esp_timer.h"

namespace bottie {
CgotchiInput::~CgotchiInput() {
    if (running_.exchange(false)) xSemaphoreTake(stopped_, portMAX_DELAY);
    if (device_) i2c_master_bus_rm_device(device_);
    if (bus_) i2c_del_master_bus(bus_);
}
bool CgotchiInput::initialize() {
    if (bus_) return false;
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = GPIO_NUM_12;
    config.scl_io_num = GPIO_NUM_11;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    esp_err_t result = i2c_new_master_bus(&config, &bus_);
    if (result != ESP_OK) { std::printf("BOTTIE_TOUCH init_bus=%d\n", int(result)); return false; }
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = 0x15;
    device.scl_speed_hz = 100000;
    result = i2c_master_bus_add_device(bus_, &device, &device_);
    if (result != ESP_OK) { std::printf("BOTTIE_TOUCH init_device=%d\n", int(result)); return false; }
    stopped_ = xSemaphoreCreateBinaryStatic(&stopped_storage_);
    if (!stopped_) return false;
    running_ = true;
    if (!xTaskCreateStatic(entry, "touch", stack_.size() * sizeof(StackType_t), this, 4, stack_.data(), &task_)) {
        running_ = false; return false;
    }
    return true;
}
bool CgotchiInput::read(uint8_t reg, uint8_t* output, size_t bytes, int timeout_ms) {
    timeout_ms = std::max(timeout_ms, int(portTICK_PERIOD_MS));
    return i2c_master_transmit_receive(device_, &reg, 1, output, bytes, timeout_ms) == ESP_OK;
}
void CgotchiInput::entry(void* self) {
    auto& input = *static_cast<CgotchiInput*>(self);
    input.work();
    xSemaphoreGive(input.stopped_);
    vTaskDelete(nullptr);
}
void CgotchiInput::work() {
    TickType_t tick = xTaskGetTickCount();
    uint64_t previous = 0;
    while (running_) {
        const uint64_t start = esp_timer_get_time();
        const uint64_t now = start / 1000;
        if (previous) max_gap_ms_ = std::max(max_gap_ms_.load(), static_cast<uint32_t>(now - previous));
        previous = now;
        auto sample = sample_touch(*this, now);
        max_read_us_ = std::max(max_read_us_.load(), static_cast<uint32_t>(esp_timer_get_time() - start));
        ++samples_;
        if (sample.valid) ++valid_; else ++errors_;
        if (!queue_.push(sample)) ++overflows_;
        vTaskDelayUntil(&tick, pdMS_TO_TICKS(20));
    }
}
InputHealth CgotchiInput::health() const {
    return {samples_.load(), valid_.load(), errors_.load(), overflows_.load(), max_gap_ms_.load(), max_read_us_.load()};
}
} // namespace bottie
