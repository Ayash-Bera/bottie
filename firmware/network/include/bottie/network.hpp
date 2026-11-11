#pragma once

#include "bottie/core.hpp"
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

// One worker owns all calls and views. Only cancel() may run concurrently.
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
    bool control(const char* json, size_t length);
    const Artwork* artwork() const;
    Status status() const;
private:
    struct Impl;
    Impl* impl_{};
    std::atomic<bool> cancelled_{false};
    Status status_{Status::disabled};
};

} // namespace bottie::network
