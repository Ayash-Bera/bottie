#include "recovered_spi.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace bottie {
namespace {
bool checked(esp_err_t result) {
    if (result == ESP_OK) return true;
    std::printf("Recovered panel transport error: %s\n", esp_err_to_name(result));
    return false;
}
bool level(int pin, bool high) { return checked(gpio_set_level(static_cast<gpio_num_t>(pin), high ? 1 : 0)); }
}

RecoveredBoard::~RecoveredBoard() {
    if (configured_) level(recovered::cs, true);
    if (device_) checked(spi_bus_remove_device(device_));
    if (bus_ready_) checked(spi_bus_free(SPI2_HOST));
}

bool RecoveredBoard::initialize() {
    if (configured_ || bus_ready_) return false;
    gpio_config_t gpio{};
    gpio.pin_bit_mask = (1ULL << recovered::cs) | (1ULL << recovered::dc) | (1ULL << recovered::reset) |
                        (1ULL << recovered::startup_low_pin) | (1ULL << recovered::post_init_high_pin);
    gpio.mode = GPIO_MODE_OUTPUT;
    if (!checked(gpio_config(&gpio))) return false;
    configured_ = true;
    if (!level(recovered::startup_low_pin, false) || !level(recovered::post_init_high_pin, false) ||
        !level(recovered::cs, true) || !level(recovered::dc, true) || !level(recovered::reset, true)) return false;
    spi_bus_config_t bus{};
    bus.mosi_io_num = recovered::mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = recovered::sck;
    bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = recovered::width * 2 * 8;
    if (!checked(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO))) return false;
    bus_ready_ = true;
    spi_device_interface_config_t device{};
    device.clock_speed_hz = recovered::clock_hz;
    device.mode = 0;
    device.spics_io_num = -1;
    device.queue_size = 1;
    if (!checked(spi_bus_add_device(SPI2_HOST, &device, &device_))) return false;
    initialized_ = recovered::initialize(*this) && level(recovered::post_init_high_pin, true);
    if (!initialized_) level(recovered::cs, true);
    return initialized_;
}

bool RecoveredBoard::reset_level(bool high) { return level(recovered::reset, high); }
void RecoveredBoard::delay_ms(uint16_t milliseconds) { vTaskDelay(pdMS_TO_TICKS(milliseconds)); }

bool RecoveredBoard::send(const uint8_t* bytes, size_t length) {
    if (!device_ || !bytes || length == 0 || length > recovered::width * 2 * 8) return false;
    spi_transaction_t transaction{};
    transaction.length = length * 8;
    if (length <= sizeof transaction.tx_data) {
        transaction.flags = SPI_TRANS_USE_TXDATA;
        std::memcpy(transaction.tx_data, bytes, length);
    } else transaction.tx_buffer = bytes;
    return checked(spi_device_polling_transmit(device_, &transaction));
}

bool RecoveredBoard::command(uint8_t code, const uint8_t* bytes, size_t length) {
    bool ok = level(recovered::cs, false) && level(recovered::dc, false) && send(&code, 1);
    if (ok && length) ok = level(recovered::dc, true) && send(bytes, length);
    bool released = level(recovered::cs, true);
    return ok && released;
}

bool RecoveredBoard::begin_pixels() {
    const uint8_t write = 0x2c;
    bool ok = level(recovered::cs, false) && level(recovered::dc, false) && send(&write, 1) && level(recovered::dc, true);
    if (!ok) level(recovered::cs, true);
    return ok;
}
bool RecoveredBoard::pixels(const uint8_t* bytes, size_t length) { return send(bytes, length); }
bool RecoveredBoard::end_pixels() { return level(recovered::cs, true); }
bool RecoveredBoard::present(const Frame& frame) {
    if (!initialized_) return false;
    if (recovered::present(*this, frame)) return true;
    initialized_ = false;
    return false;
}
} // namespace bottie
