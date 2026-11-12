#include "bottie/network_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace bottie::network::policy {
namespace {
bool decimal(const char* first, const char* last, unsigned maximum, unsigned& result) {
    if (first == last) return false;
    unsigned value = 0;
    for (; first != last; ++first) {
        if (*first < '0' || *first > '9') return false;
        unsigned digit = static_cast<unsigned>(*first - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    result = value;
    return true;
}
bool identifier(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool compact(const uint8_t* data, size_t length, char* output, size_t capacity) {
    size_t n = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; ++i) {
        char c = static_cast<char>(data[i]);
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) continue;
        if (c < 0x20 || c == '\\' || n + 1 >= capacity) return false;
        output[n++] = c;
    }
    output[n] = 0;
    return !quoted;
}
bool exact(Stream& stream, uint8_t* output, size_t count) {
    while (count) {
        int n = stream.read(output, count);
        if (n <= 0 || static_cast<size_t>(n) > count) return false;
        output += n;
        count -= static_cast<size_t>(n);
    }
    return true;
}
bool line(Stream& stream, char* output, size_t capacity, size_t& total, size_t limit) {
    size_t used = 0;
    while (used + 1 < capacity && total < limit) {
        uint8_t c;
        if (!exact(stream, &c, 1)) return false;
        ++total;
        if (c == '\r') {
            if (total == limit || !exact(stream, &c, 1) || c != '\n') return false;
            ++total;
            output[used] = 0;
            return true;
        }
        if (c < 0x20 || c > 0x7e) return false;
        output[used++] = static_cast<char>(c);
    }
    return false;
}
void lower(char* value) {
    for (; *value; ++value) if (*value >= 'A' && *value <= 'Z') *value += 'a' - 'A';
}
}

bool parse_origin(const char* text, bool allow_http, Origin& result) {
    if (!text || std::strlen(text) >= sizeof result.text) return false;
    Origin next;
    const char* authority;
    if (std::strncmp(text, "https://", 8) == 0) { next.tls = true; authority = text + 8; next.port = 443; }
    else if (allow_http && std::strncmp(text, "http://", 7) == 0) { authority = text + 7; next.port = 80; }
    else return false;
    const char* end = authority;
    for (unsigned part = 0; part < 4; ++part) {
        const char* start = end;
        while (*end >= '0' && *end <= '9') ++end;
        unsigned octet;
        if (!decimal(start, end, 255, octet) || (end - start > 1 && *start == '0')) return false;
        if (part == 0 && (octet == 0 || octet >= 224)) return false;
        if (part < 3 && *end++ != '.') return false;
    }
    size_t host_length = static_cast<size_t>(end - authority);
    if (host_length >= sizeof next.host) return false;
    std::memcpy(next.host, authority, host_length);
    if (*end == ':') {
        const char* port = ++end;
        while (*end >= '0' && *end <= '9') ++end;
        unsigned number;
        if (!decimal(port, end, 65535, number) || !number || (end - port > 1 && *port == '0')) return false;
        next.port = static_cast<uint16_t>(number);
    }
    if (*end || std::strlen(authority) >= sizeof next.authority) return false;
    std::strcpy(next.authority, authority);
    std::strcpy(next.text, text);
    result = next;
    return true;
}

bool artwork_path(const Origin& origin, const char* url, char* output, size_t capacity) {
    if (!url || !output || !capacity) return false;
    const char* path = url;
    if (*url != '/') {
        size_t prefix = std::strlen(origin.text);
        if (std::strncmp(url, origin.text, prefix) || url[prefix] != '/') return false;
        path += prefix;
    }
    constexpr char prefix[] = "/v1/artwork/";
    if (std::strncmp(path, prefix, sizeof prefix - 1)) return false;
    size_t length = std::strlen(path), id_length = length - (sizeof prefix - 1);
    if (!id_length || id_length > 128 || length >= capacity) return false;
    for (const char* p = path + sizeof prefix - 1; *p; ++p) if (!identifier(*p)) return false;
    std::memcpy(output, path, length + 1);
    return true;
}

bool token_valid(const char* token) {
    if (!token) return false;
    size_t length = std::strlen(token);
    if (length < 16 || length > 256) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = token[i];
        if (!identifier(c) && c != '.' && c != '~' && c != '+' && c != '/' && c != '=') return false;
    }
    return true;
}

bool control_valid(const char* json, size_t length) {
    if (!json || !length || length > control_capacity) return false;
    char text[128];
    if (!compact(reinterpret_cast<const uint8_t*>(json), length, text, sizeof text)) return false;
    bool guarded = false;
    if (char* guard = std::strstr(text, ",\"control_target\":\"")) {
        char* value = guard + std::strlen(",\"control_target\":\"");
        if (std::strlen(value) != 34 || value[32] != '"' || value[33] != '}') return false;
        for (unsigned n = 0; n < 32; ++n)
            if (!((value[n] >= '0' && value[n] <= '9') || (value[n] >= 'a' && value[n] <= 'f'))) return false;
        guard[0] = '}'; guard[1] = 0;
        guarded = true;
    }
    const char* const allowed[] = {
        "{\"action\":\"play\"}", "{\"action\":\"pause\"}", "{\"action\":\"previous\"}", "{\"action\":\"next\"}",
        "{\"action\":\"shuffle\",\"value\":true}", "{\"action\":\"shuffle\",\"value\":false}",
        "{\"action\":\"repeat\",\"value\":\"off\"}", "{\"action\":\"repeat\",\"value\":\"context\"}",
        "{\"action\":\"repeat\",\"value\":\"track\"}"
    };
    for (const char* candidate : allowed) if (!std::strcmp(text, candidate)) return true;
    constexpr char volume[] = "{\"action\":\"volume\",\"value\":";
    if (guarded && !std::strncmp(text, volume, sizeof volume - 1)) {
        const char* first = text + sizeof volume - 1;
        const char* last = text + std::strlen(text) - 1;
        unsigned value;
        return *last == '}' && !(last - first > 1 && *first == '0') && decimal(first, last, 100, value);
    }
    return false;
}
bool control_ack(const uint8_t* data, size_t length) {
    char text[32];
    return data && compact(data, length, text, sizeof text) && !std::strcmp(text, "{\"ok\":true}");
}
bool artwork_valid(const uint8_t* data, size_t length, uint16_t width, uint16_t height, Artwork& view) {
    Artwork candidate;
    if (length > artwork_capacity || !parse_artwork(data, length, candidate) ||
        candidate.width > 96 || candidate.height > 96 || candidate.width != width || candidate.height != height) return false;
    view = candidate;
    return true;
}
void Backoff::fail(uint64_t now, unsigned retry_seconds) {
    failures = std::min(failures + 1, 6U);
    unsigned seconds = std::max(1U << (failures - 1), std::min(retry_seconds, 300U));
    uint64_t duration = static_cast<uint64_t>(seconds) * 1000;
    until = now > std::numeric_limits<uint64_t>::max() - duration ? std::numeric_limits<uint64_t>::max() : now + duration;
}

ResponseError read_response(Stream& stream, Media media, uint8_t* output, size_t capacity, Response& response) {
    response = {};
    char text[512];
    size_t headers = 0;
    if (!line(stream, text, sizeof text, headers, 4096) || std::strlen(text) < 12 ||
        (std::strncmp(text, "HTTP/1.1 ", 9) && std::strncmp(text, "HTTP/1.0 ", 9)) ||
        !decimal(text + 9, text + 12, 599, response.status) || response.status < 100 ||
        (text[12] && text[12] != ' ')) return ResponseError::headers;
    bool has_length = false, chunked = false, has_type = false, type_matches = false, encoding = false;
    unsigned length = 0;
    for (;;) {
        if (!line(stream, text, sizeof text, headers, 4096)) return ResponseError::headers;
        if (!*text) break;
        char* colon = std::strchr(text, ':');
        if (!colon || colon == text || text[0] == ' ') return ResponseError::headers;
        *colon = 0;
        lower(text);
        char* value = colon + 1;
        while (*value == ' ') ++value;
        char* end = value + std::strlen(value);
        while (end > value && end[-1] == ' ') *--end = 0;
        if (!std::strcmp(text, "content-length")) {
            if (has_length || !decimal(value, end, 1000000, length)) return ResponseError::framing;
            has_length = true;
        } else if (!std::strcmp(text, "transfer-encoding")) {
            lower(value);
            if (chunked || std::strcmp(value, "chunked")) return ResponseError::framing;
            chunked = true;
        } else if (!std::strcmp(text, "content-type")) {
            if (has_type) return ResponseError::headers;
            if (char* parameter = std::strchr(value, ';')) *parameter = 0;
            lower(value);
            const char* wanted = media == Media::json ? "application/json" : "application/octet-stream";
            type_matches = !std::strcmp(value, wanted);
            has_type = true;
        } else if (!std::strcmp(text, "content-encoding")) {
            lower(value);
            if (encoding || std::strcmp(value, "identity")) return ResponseError::media;
            encoding = true;
        } else if (!std::strcmp(text, "retry-after")) {
            unsigned seconds;
            if (decimal(value, end, 1000000, seconds)) response.retry_seconds = std::min(seconds, 300U);
        }
    }
    if (response.status != 200) return ResponseError::status;
    if (!has_type || !type_matches) return ResponseError::media;
    if (has_length == chunked) return ResponseError::framing;
    if (has_length) {
        if (length > capacity) return ResponseError::size;
        if (!exact(stream, output, length)) return ResponseError::io;
        response.length = length;
        return ResponseError::none;
    }
    for (unsigned chunks = 0; chunks < 1024; ++chunks) {
        size_t metadata = 0;
        if (!line(stream, text, 128, metadata, 128)) return ResponseError::framing;
        size_t chunk = 0, digits = 0;
        for (char* p = text; *p && *p != ';'; ++p) {
            unsigned digit;
            if (*p >= '0' && *p <= '9') digit = *p - '0';
            else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
            else return ResponseError::framing;
            if (++digits > 8 || chunk > capacity / 16) return ResponseError::size;
            chunk = chunk * 16 + digit;
        }
        if (!digits) return ResponseError::framing;
        if (!chunk) {
            size_t trailers = 0;
            do {
                if (!line(stream, text, sizeof text, trailers, 1024)) return ResponseError::framing;
            } while (*text);
            return ResponseError::none;
        }
        if (chunk > capacity - response.length) return ResponseError::size;
        if (!exact(stream, output + response.length, chunk)) return ResponseError::io;
        response.length += chunk;
        uint8_t ending[2];
        if (!exact(stream, ending, 2) || ending[0] != '\r' || ending[1] != '\n') return ResponseError::framing;
    }
    return ResponseError::framing;
}

} // namespace bottie::network::policy
