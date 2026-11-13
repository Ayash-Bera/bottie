#pragma once

#include "bottie/core.hpp"
#include "bottie/text_mask.hpp"
#include <atomic>

namespace bottie::network {

constexpr size_t artwork_capacity = 16 + 96 * 96 * 2;
constexpr size_t control_capacity = 1024;
constexpr uint32_t request_timeout_ms = 6000;

struct Config {
    const char* ssid = "";
    const char* password = "";
    const char* origin = "";
    const char* device_token = "";
    bool allow_insecure_http = false;
    const char* server_certificate_pem = nullptr;
};

enum class Status {
    disabled, connecting, offline, ready, backoff, unauthorized,
    invalid_config, invalid_response, transport_error, cancelled, no_memory
};

struct AssetInterrupt {
    uint32_t ticket() const { return generation.load(std::memory_order_acquire); }
    void interrupt() { generation.fetch_add(1, std::memory_order_release); }
    bool valid(uint32_t value) const { return ticket() == value; }
private:
    std::atomic<uint32_t> generation{};
};

struct RequestBudget {
    const std::atomic<bool>& cancelled;
    const AssetInterrupt* interrupt{};
    uint32_t ticket{};
    uint64_t deadline{};
    bool alive(uint64_t now) const { return !cancelled.load() && (!interrupt || interrupt->valid(ticket)) && now < deadline; }
};
template<class Now,class Pause,class Read,class Again>
int read_until(const RequestBudget& budget,Now now,Pause pause,Read read,Again again) {
    while (budget.alive(now())) {
        const int result=read();
        if (result>=0) return result;
        if (!again(result)) return -1;
        pause();
    }
    return -1;
}

// One worker owns calls/views; cancel and asset interruption are cross-task signals.
class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    bool start();
    bool start(const Config& config);
    void stop();
    void cancel();
    bool playback(char* output, size_t capacity, size_t& length);
    uint32_t asset_ticket() const { return asset_interrupt_.ticket(); }
    void interrupt_assets() { asset_interrupt_.interrupt(); }
    bool fetch_asset(const Snapshot& snapshot, uint32_t ticket);
    bool control(const char* json, size_t length);
    const Artwork* artwork() const;
    TextMask text_mask(const Snapshot& snapshot) const;
    Status status() const;
private:
    struct Impl;
    Impl* impl_{};
    std::atomic<bool> cancelled_{false};
    AssetInterrupt asset_interrupt_;
    Status status_{Status::disabled};
};

} // namespace bottie::network
