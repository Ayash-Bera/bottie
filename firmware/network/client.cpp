#include "bottie/network.hpp"
#include "bottie/network_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#if __has_include("network.local.hpp")
#include "network.local.hpp"
#else
namespace bottie::network { static Config local_config() { return {}; } }
#endif

namespace bottie::network {
namespace {
uint64_t clock_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

class Connection final : public policy::Stream {
public:
    explicit Connection(std::atomic<bool>& cancelled) : cancelled_(cancelled), deadline_(clock_ms() + request_timeout_ms) {}
    ~Connection() override { if (tls_) esp_tls_conn_destroy(tls_); }
    bool alive() const { return !cancelled_.load() && clock_ms() < deadline_; }
    bool connect(const policy::Origin& origin, const char* certificate) {
        tls_ = esp_tls_init();
        if (!tls_) return false;
        esp_tls_cfg_t cfg{};
        cfg.non_block = true;
        cfg.timeout_ms = 20;
        cfg.is_plain_tcp = !origin.tls;
        if (origin.tls) {
            if (!certificate || !*certificate) return false;
            cfg.cacert_buf = reinterpret_cast<const unsigned char*>(certificate);
            cfg.cacert_bytes = std::strlen(certificate) + 1;
        }
        while (alive()) {
            int result = esp_tls_conn_new_async(origin.host, std::strlen(origin.host), origin.port, &cfg, tls_);
            if (result < 0) return false;
            if (result == 1) {
                // Plain nonblocking TCP may still be connecting when ESP-TLS returns.
                int fd;
                if (esp_tls_get_conn_sockfd(tls_, &fd) != ESP_OK) return false;
                while (alive()) {
                    fd_set write_set;
                    FD_ZERO(&write_set);
                    FD_SET(fd, &write_set);
                    timeval timeout{0, 10000};
                    int selected = select(fd + 1, nullptr, &write_set, nullptr, &timeout);
                    if (selected < 0 && errno != EINTR) return false;
                    if (selected > 0) {
                        int error = 0;
                        socklen_t length = sizeof error;
                        return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
                    }
                }
                return false;
            }
            vTaskDelay(1);
        }
        return false;
    }
    bool write(const char* data, size_t length) {
        while (length && alive()) {
            errno = 0;
            ssize_t n = esp_tls_conn_write(tls_, data, length);
            if (n > 0) { data += n; length -= static_cast<size_t>(n); }
            else if (again(n)) vTaskDelay(1);
            else return false;
        }
        return !length && alive();
    }
    int read(uint8_t* output, size_t capacity) override {
        while (alive()) {
            errno = 0;
            ssize_t n = esp_tls_conn_read(tls_, output, capacity);
            if (n >= 0) return static_cast<int>(n);
            if (!again(n)) return -1;
            vTaskDelay(1);
        }
        return -1;
    }
private:
    static bool again(ssize_t result) {
        return result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE ||
               (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
    }
    std::atomic<bool>& cancelled_;
    uint64_t deadline_;
    esp_tls_t* tls_{};
};
}

struct Client::Impl {
    policy::Origin origin;
    char ssid[33]{}, password[65]{}, token[257]{};
    const char* certificate{};
    esp_netif_t* netif{};
    esp_event_handler_instance_t wifi_handler{}, ip_handler{};
    bool loop_owned{}, wifi_owned{}, wifi_started{}, handlers_attached{}, attempting{};
    std::atomic<bool> connected{false}, link_failed{false};
    uint64_t attempt_until{};
    policy::Backoff wifi_backoff, http_backoff, art_backoff;
    uint8_t art_bytes[artwork_capacity]{};
    char art_url[257]{};
    Artwork art;

    static void event(void* context, esp_event_base_t base, int32_t id, void*) {
        auto& self = *static_cast<Impl*>(context);
        if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) self.connected.store(true);
        else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
                 (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
            self.connected.store(false);
            self.link_failed.store(true);
        }
    }
    void clear_art() { art = {}; art_url[0] = 0; }
    bool link() {
        uint64_t now = clock_ms();
        if (connected.load()) { attempting = false; wifi_backoff.clear(); return true; }
        if (attempting) {
            if (!link_failed.load() && now < attempt_until) return false;
            esp_wifi_disconnect();
            attempting = false;
            wifi_backoff.fail(now);
        }
        if (wifi_backoff.ready(now)) {
            link_failed.store(false);
            attempting = true;
            attempt_until = now + 15000;
            if (esp_wifi_connect() != ESP_OK) link_failed.store(true);
        }
        return false;
    }
};

Client::~Client() { stop(); }
void Client::cancel() { cancelled_.store(true); }
Status Client::status() const { return status_; }
const Artwork* Client::artwork() const { return impl_ && impl_->art.pixels ? &impl_->art : nullptr; }
bool Client::start() { return start(local_config()); }

bool Client::start(const Config& config) {
    stop();
    cancelled_.store(false);
#ifndef CONFIG_BOTTIE_NETWORK_ENABLED
    (void)config;
    return false;
#else
    policy::Origin origin;
    if (!config.ssid || !config.password || !*config.ssid || std::strlen(config.ssid) > 32 ||
        std::strlen(config.password) < 8 || std::strlen(config.password) > 63 ||
        !policy::parse_origin(config.origin, config.allow_insecure_http, origin) ||
        !policy::token_valid(config.device_token) ||
        (origin.tls && (!config.server_certificate_pem || !*config.server_certificate_pem))) {
        status_ = Status::invalid_config;
        return false;
    }
    wifi_mode_t existing;
    if (esp_wifi_get_mode(&existing) != ESP_ERR_WIFI_NOT_INIT || esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        status_ = Status::transport_error;
        return false;
    }
    void* memory = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) { status_ = Status::no_memory; return false; }
    impl_ = new (memory) Impl;
    impl_->origin = origin;
    std::strcpy(impl_->ssid, config.ssid);
    std::strcpy(impl_->password, config.password);
    std::strcpy(impl_->token, config.device_token);
    impl_->certificate = config.server_certificate_pem;
    auto failed = [&]() { stop(); status_ = Status::transport_error; return false; };
    if (esp_netif_init() != ESP_OK) return failed();
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return failed();
    impl_->loop_owned = loop == ESP_OK;
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    wifi.nvs_enable = 0;
    if (esp_wifi_init(&wifi) != ESP_OK) return failed();
    impl_->wifi_owned = true;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    impl_->netif = esp_netif_new(&netif_config);
    if (!impl_->netif || esp_netif_attach_wifi_station(impl_->netif) != ESP_OK) return failed();
    impl_->handlers_attached = true;
    if (esp_wifi_set_default_wifi_sta_handlers() != ESP_OK) return failed();
    if (esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, Impl::event, impl_, &impl_->wifi_handler) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, Impl::event, impl_, &impl_->ip_handler) != ESP_OK) return failed();
    wifi_config_t station{};
    std::memcpy(station.sta.ssid, impl_->ssid, std::strlen(impl_->ssid));
    std::memcpy(station.sta.password, impl_->password, std::strlen(impl_->password));
    station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    station.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK || esp_wifi_start() != ESP_OK) return failed();
    impl_->wifi_started = true;
    impl_->link();
    status_ = Status::connecting;
    return true;
#endif
}

void Client::stop() {
    cancelled_.store(true);
    if (impl_) {
        if (impl_->wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, impl_->wifi_handler);
        if (impl_->ip_handler) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, impl_->ip_handler);
        if (impl_->wifi_started) esp_wifi_stop();
        if (impl_->handlers_attached) esp_wifi_clear_default_wifi_driver_and_handlers(impl_->netif);
        if (impl_->netif) esp_netif_destroy(impl_->netif);
        if (impl_->wifi_owned) esp_wifi_deinit();
        if (impl_->loop_owned) esp_event_loop_delete_default();
        impl_->~Impl();
        volatile uint8_t* wipe = reinterpret_cast<volatile uint8_t*>(impl_);
        for (size_t i = 0; i < sizeof(Impl); ++i) wipe[i] = 0;
        heap_caps_free(impl_);
        impl_ = nullptr;
    }
    status_ = Status::disabled;
}

namespace {
policy::ResponseError request(const policy::Origin& origin, const char* certificate, const char* token,
                              const char* path, const char* post, size_t post_size, policy::Media media,
                              uint8_t* output, size_t capacity, policy::Response& response, std::atomic<bool>& cancelled) {
    Connection connection(cancelled);
    if (!connection.connect(origin, certificate)) return policy::ResponseError::io;
    char headers[768];
    int n = std::snprintf(headers, sizeof headers,
        "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nAccept: %s\r\nConnection: close\r\nContent-Length: %u\r\nContent-Type: application/json\r\n\r\n",
        post ? "POST" : "GET", path, origin.authority, token,
        media == policy::Media::json ? "application/json" : "application/octet-stream", static_cast<unsigned>(post_size));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof headers || !connection.write(headers, static_cast<size_t>(n)) ||
        (post && !connection.write(post, post_size))) return policy::ResponseError::io;
    return policy::read_response(connection, media, output, capacity, response);
}
Status response_status(policy::ResponseError error, const policy::Response& response, bool cancelled) {
    if (cancelled) return Status::cancelled;
    if (response.status == 401 || response.status == 403) return Status::unauthorized;
    if (response.status == 429 || response.retry_seconds) return Status::backoff;
    return error == policy::ResponseError::io ? Status::transport_error : Status::invalid_response;
}
}

bool Client::playback(char* output, size_t capacity, size_t& length) {
    length = 0;
    if (output && capacity) output[0] = 0;
    if (!impl_) return false;
    auto& state = *impl_;
    if (cancelled_.load()) { state.clear_art(); status_ = Status::cancelled; return false; }
    if (!output || capacity < 2) { status_ = Status::invalid_response; return false; }
    if (!state.link()) { state.clear_art(); status_ = Status::offline; return false; }
    if (!state.http_backoff.ready(clock_ms())) { status_ = Status::backoff; return false; }
    policy::Response response;
    auto error = request(state.origin, state.certificate, state.token, "/v1/playback", nullptr, 0,
                         policy::Media::json, reinterpret_cast<uint8_t*>(output), std::min(capacity - 1, max_json_bytes), response, cancelled_);
    Snapshot snapshot;
    if (error != policy::ResponseError::none || parse_snapshot(output, response.length, snapshot) != Error::none) {
        output[0] = 0;
        state.clear_art();
        state.http_backoff.fail(clock_ms(), response.retry_seconds);
        status_ = response_status(error, response, cancelled_.load());
        return false;
    }
    output[response.length] = 0;
    length = response.length;
    state.http_backoff.clear();
    if (response.retry_seconds) state.http_backoff.fail(clock_ms(), response.retry_seconds);
    if (std::strcmp(snapshot.artwork_url, state.art_url) || (state.art.pixels &&
        (state.art.width != snapshot.artwork_width || state.art.height != snapshot.artwork_height))) {
        state.clear_art();
        std::strcpy(state.art_url, snapshot.artwork_url);
        state.art_backoff.clear();
    }
    if (snapshot.state == Playback::unavailable || !snapshot.artwork_url[0]) state.clear_art();
    else if (!state.art.pixels && state.art_backoff.ready(clock_ms())) {
        char path[257];
        if (snapshot.artwork_width <= 96 && snapshot.artwork_height <= 96 &&
            policy::artwork_path(state.origin, snapshot.artwork_url, path, sizeof path)) {
            policy::Response image_response;
            auto image_error = request(state.origin, state.certificate, state.token, path, nullptr, 0, policy::Media::artwork,
                                       state.art_bytes, sizeof state.art_bytes, image_response, cancelled_);
            if (image_error == policy::ResponseError::none && policy::artwork_valid(state.art_bytes, image_response.length,
                snapshot.artwork_width, snapshot.artwork_height, state.art)) state.art_backoff.clear();
            else { state.art = {}; state.art_backoff.fail(clock_ms(), image_response.retry_seconds); }
        }
    }
    if (cancelled_.load()) { length = 0; output[0] = 0; state.clear_art(); status_ = Status::cancelled; return false; }
    status_ = Status::ready;
    return true;
}

bool Client::control(const char* json, size_t length) {
    if (!impl_) return false;
    auto& state = *impl_;
    if (cancelled_.load()) { status_ = Status::cancelled; return false; }
    if (!policy::control_valid(json, length)) { status_ = Status::invalid_response; return false; }
    if (!state.link()) { state.clear_art(); status_ = Status::offline; return false; }
    if (!state.http_backoff.ready(clock_ms())) { status_ = Status::backoff; return false; }
    uint8_t body[control_capacity];
    policy::Response response;
    auto error = request(state.origin, state.certificate, state.token, "/v1/control", json, length, policy::Media::json,
                         body, sizeof body, response, cancelled_);
    if (cancelled_.load() || error != policy::ResponseError::none || !policy::control_ack(body, response.length)) {
        state.clear_art();
        state.http_backoff.fail(clock_ms(), response.retry_seconds);
        status_ = response_status(error, response, cancelled_.load());
        return false;
    }
    state.http_backoff.clear();
    status_ = Status::ready;
    return true;
}

} // namespace bottie::network
