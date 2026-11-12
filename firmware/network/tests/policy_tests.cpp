#include "bottie/network_policy.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace bottie;
using namespace bottie::network;
using namespace bottie::network::policy;

class FakeStream final : public Stream {
public:
    explicit FakeStream(std::string bytes, size_t fragment = 7) : bytes_(std::move(bytes)), fragment_(fragment) {}
    int read(uint8_t* output, size_t capacity) override {
        size_t count = std::min({capacity, fragment_, bytes_.size() - position_});
        std::memcpy(output, bytes_.data() + position_, count);
        position_ += count;
        return static_cast<int>(count);
    }
private:
    std::string bytes_;
    size_t fragment_, position_{};
};

ResponseError response(const std::string& wire, Response& result, size_t capacity = 8192, Media media = Media::json) {
    FakeStream stream(wire);
    std::vector<uint8_t> output(capacity + 2, 0xaa);
    auto error = read_response(stream, media, output.data() + 1, capacity, result);
    assert(output.front() == 0xaa && output.back() == 0xaa);
    return error;
}

int main() {
    const std::string guard = ",\"control_target\":\"0123456789abcdef0123456789abcdef\"}";
    for (const std::string prefix : {"{\"action\":\"play\"", "{\"action\":\"volume\",\"value\":0", "{\"action\":\"volume\",\"value\":100", "{\"action\":\"repeat\",\"value\":\"context\""}) {
        const auto body = prefix + guard;
        assert(control_valid(body.data(), body.size()));
    }
    for (const std::string prefix : {"{\"action\":\"volume\",\"value\":-1", "{\"action\":\"volume\",\"value\":101", "{\"action\":\"volume\",\"value\":1.0", "{\"action\":\"volume\",\"value\":true", "{\"action\":\"volume\",\"value\":\"50\"", "{\"action\":\"volume\",\"value\":01", "{\"action\":\"volume\",\"value\":1e1"}) {
        const auto body = prefix + guard;
        assert(!control_valid(body.data(), body.size()));
    }
    for (const std::string body : {"{\"action\":\"volume\",\"value\":50}", "{\"action\":\"play\",\"control_target\":\"\"}", "{\"action\":\"play\",\"control_target\":\"0123456789abcdef0123456789abcdeF\"}"})
        assert(!control_valid(body.data(), body.size()));
    Origin origin;
    assert(parse_origin("http://192.168.1.2:8787", true, origin));
    assert(origin.port == 8787 && !origin.tls);
    assert(!parse_origin("http://192.168.1.2", false, origin));
    assert(parse_origin("https://192.168.1.2", false, origin) && origin.tls && origin.port == 443);
    const char* invalid_origins[] = {"http://localhost", "http://1.2.3", "http://256.2.3.4", "http://01.2.3.4",
        "http://1.2.3.4/", "http://1.2.3.4@evil", "http://1.2.3.4:0", "http://1.2.3.4:65536", "http://1.2.3.4?x",
        "http://1.2.3.4\r\nX: yes", "http://224.0.0.1", "http://[::1]", "http://0.0.0.0", "http://1.2.3.4:00080"};
    for (const char* text : invalid_origins) assert(!parse_origin(text, true, origin));
    assert(parse_origin("http://192.168.1.2:8787", true, origin));
    char path[257];
    assert(artwork_path(origin, "/v1/artwork/ab_1-c", path, sizeof path));
    assert(artwork_path(origin, "http://192.168.1.2:8787/v1/artwork/id", path, sizeof path));
    const char* invalid_art[] = {"//evil/v1/artwork/x", "http://192.168.1.2:8787.evil/v1/artwork/x",
        "http://192.168.1.3:8787/v1/artwork/x", "https://192.168.1.2:8787/v1/artwork/x", "/v1/artwork/../control",
        "/v1/artwork/%2e", "/v1/artwork/x?token=secret", "/v1/artwork/x#y", "/v1/control", "/v1/artwork/"};
    for (const char* text : invalid_art) assert(!artwork_path(origin, text, path, sizeof path));
    assert(!artwork_path(origin, "/v1/artwork/ok", path, 4));
    assert(token_valid("fixture-token-0123456789"));
    assert(!token_valid("") && !token_valid("short") && !token_valid("fixture-token\r\nInjected: yes"));
    assert(!token_valid(std::string(257, 'x').c_str()));

    for (Action action : {Action::play, Action::pause, Action::previous, Action::next, Action::shuffle, Action::repeat}) {
        for (Repeat repeat : {Repeat::off, Repeat::context, Repeat::track}) {
            for (bool shuffle : {false, true}) {
                char encoded[128];
                assert(encode_intent({action, shuffle, repeat}, encoded, sizeof encoded));
                assert(control_valid(encoded, std::strlen(encoded)));
            }
        }
    }
    std::string valid = " \n{\"action\": \"next\"}\t";
    assert(control_valid(valid.data(), valid.size()));
    for (std::string text : {"{\"action\":\"next\",\"extra\":1}", "{\"action\":\"shuffle\"}",
         "{\"action\":\"repeat\",\"value\":\"all\"}", "{\"action\":\"next\"}{}", "{\"action\":\"n ext\"}"})
        assert(!control_valid(text.data(), text.size()));
    assert(!control_valid(valid.data(), 1025));
    assert(control_ack(reinterpret_cast<const uint8_t*>("{ \"ok\": true }\n"), 15));
    assert(!control_ack(reinterpret_cast<const uint8_t*>("{\"ok\":false}"), 12));

    Backoff backoff;
    assert(backoff.ready(0));
    backoff.fail(100);
    assert(backoff.until == 1100 && !backoff.ready(1099) && backoff.ready(1100));
    for (unsigned i = 0; i < 100; ++i) backoff.fail(100);
    assert(backoff.until == 32100 && backoff.failures == 6);
    backoff.fail(100, 900);
    assert(backoff.until == 300100);
    backoff.clear();
    assert(backoff.ready(0) && backoff.failures == 0);
    backoff.fail(std::numeric_limits<uint64_t>::max() - 1);
    assert(backoff.until == std::numeric_limits<uint64_t>::max());

    Response result;
    std::string prefix = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
    assert(response(prefix + "Content-Length: 2\r\n\r\n{}", result) == ResponseError::none && result.length == 2);
    assert(response(prefix + "Transfer-Encoding: chunked\r\n\r\n1\r\n{\r\n1\r\n}\r\n0\r\n\r\n", result) == ResponseError::none && result.length == 2);
    assert(response(prefix + "Content-Length: 8192\r\n\r\n" + std::string(8192, 'x'), result) == ResponseError::none);
    assert(response(prefix + "Content-Length: 8193\r\n\r\n", result) == ResponseError::size);
    assert(response(prefix + "Content-Length: 9\r\n\r\nshort", result) == ResponseError::io);
    assert(response(prefix + "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", result) == ResponseError::framing);
    assert(response(prefix + "Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n", result) == ResponseError::framing);
    assert(response(prefix + "\r\n{}", result) == ResponseError::framing);
    assert(response(prefix + "Transfer-Encoding: chunked\r\n\r\n2001\r\n", result) == ResponseError::size);
    assert(response(prefix + "Transfer-Encoding: chunked\r\n\r\n-1\r\n", result) == ResponseError::framing);
    assert(response(prefix + "Transfer-Encoding: chunked\r\n\r\n2\r\n{}xx", result) == ResponseError::framing);
    assert(response("HTTP/1.1 302 Found\r\nLocation: https://evil/\r\n\r\n", result) == ResponseError::status);
    assert(response("HTTP/1.1 429 Slow\r\nContent-Type: application/json\r\nRetry-After: 9999\r\nContent-Length: 2\r\n\r\n{}", result, 10, Media::artwork) == ResponseError::status);
    assert(result.status == 429 && result.retry_seconds == 300);
    assert(response("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\n{}", result) == ResponseError::media);
    assert(response(prefix + "Content-Encoding: gzip\r\nContent-Length: 2\r\n\r\n{}", result) == ResponseError::media);
    assert(response(prefix + "X-Huge: " + std::string(600, 'a') + "\r\n\r\n", result) == ResponseError::headers);
    std::string many = prefix;
    for (int i = 0; i < 1000; ++i) many += "X: y\r\n";
    assert(response(many + "\r\n", result) == ResponseError::headers);
    assert(response("HTTP/1.1 200 OK\n\n", result) == ResponseError::headers);

    std::vector<uint8_t> image(artwork_capacity);
    std::memcpy(image.data(), "BTAR\1\1", 6);
    image[6] = image[8] = 96;
    image[10] = 192;
    image[12] = 0; image[13] = 72;
    Artwork art;
    assert(artwork_valid(image.data(), image.size(), 96, 96, art));
    assert(!artwork_valid(image.data(), image.size(), 95, 96, art));
    assert(!artwork_valid(image.data(), image.size() - 1, 96, 96, art));
    image[6] = 97;
    assert(!artwork_valid(image.data(), image.size(), 97, 96, art));
    image[0] = 'X';
    assert(!artwork_valid(image.data(), image.size(), 96, 96, art));
    const std::string binary_header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    assert(response(binary_header + "Content-Length: 8494\r\n\r\n" + std::string(8494, char(0)),
                    result, text_mask_capacity, Media::artwork) == ResponseError::none && result.length == 8494);
    assert(response(binary_header + "Content-Length: 8495\r\n\r\n",
                    result, text_mask_capacity, Media::artwork) == ResponseError::size);
    assert(response(binary_header + "Transfer-Encoding: chunked\r\n\r\n212e\r\n" + std::string(8494, char(0)) + "\r\n0\r\n\r\n",
                    result, text_mask_capacity, Media::artwork) == ResponseError::none && result.length == 8494);
    assert(response(binary_header + "Transfer-Encoding: chunked\r\n\r\n212f\r\n",
                    result, text_mask_capacity, Media::artwork) == ResponseError::size);
    std::cout << "BOTTIE_NETWORK_NATIVE_OK\n";
}
