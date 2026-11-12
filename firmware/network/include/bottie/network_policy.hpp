#pragma once

#include "bottie/network.hpp"

namespace bottie::network::policy {

struct Origin {
    char text[64]{}, host[16]{}, authority[24]{};
    uint16_t port{};
    bool tls{};
};
bool parse_origin(const char* text, bool allow_http, Origin& result);
bool artwork_path(const Origin& origin, const char* url, char* output, size_t capacity);
bool token_valid(const char* token);
bool control_valid(const char* json, size_t length);
bool control_ack(const uint8_t* data, size_t length);
bool artwork_valid(const uint8_t* data, size_t length, uint16_t width, uint16_t height, Artwork& view);
bool text_digest_valid(const char* path, const uint8_t* data, size_t length);

enum class Asset { none, text, artwork };
struct AssetSchedule {
    bool previous_text{};
    Asset choose(bool text_ready, bool art_ready) {
        if (text_ready && (!art_ready || !previous_text)) { previous_text = true; return Asset::text; }
        if (art_ready) { previous_text = false; return Asset::artwork; }
        return Asset::none;
    }
};

struct Backoff {
    uint64_t until{};
    unsigned failures{};
    bool ready(uint64_t now) const { return now >= until; }
    void fail(uint64_t now, unsigned retry_seconds = 0);
    void clear() { until = 0; failures = 0; }
};

class Stream {
public:
    virtual ~Stream() = default;
    // Implementations enforce a shared deadline and cancellation for every call.
    virtual int read(uint8_t* output, size_t capacity) = 0;
};

enum class Media { json, artwork };
enum class ResponseError { none, io, headers, status, media, size, framing };
struct Response {
    unsigned status{}, retry_seconds{};
    size_t length{};
};
ResponseError read_response(Stream& stream, Media media, uint8_t* output, size_t capacity, Response& response);

} // namespace bottie::network::policy
