#include "playback_cache.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Cache check failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

int main() {
    bottie::CachedPlayback cache;
    CHECK(!cache.artwork().pixels && !cache.online);
    bottie::Snapshot snapshot;
    snapshot.state = bottie::Playback::playing;
    snapshot.duration_ms = 100000;
    snapshot.updated_at_ms = 1;
    std::strcpy(snapshot.artwork_url, "/v1/artwork/example");
    std::array<uint8_t, 96 * 96 * 2> source{};
    source[0] = 0x12; source.back() = 0x34;
    bottie::Artwork art{source.data(), 96, 96, 192};
    cache.capture(snapshot, 100, &art);
    source.fill(0xff);
    CHECK(cache.online && cache.artwork().pixels[0] == 0x12 && cache.artwork().pixels[source.size() - 1] == 0x34);
    auto displayed = cache;
    cache.offline();
    CHECK(!cache.artwork().pixels && displayed.artwork().pixels[0] == 0x12);
    CHECK(displayed.artwork().pixels == displayed.pixels.data());
    bottie::Model model;
    CHECK(model.apply_snapshot(displayed.snapshot, displayed.observed_ms) == bottie::Error::none);
    CHECK(model.effective_state(14999) == bottie::Playback::playing);
    CHECK(model.effective_state(15100) == bottie::Playback::unavailable);
    art.width = 97;
    cache.capture(snapshot, 200, &art);
    CHECK(cache.online && !cache.artwork().pixels);
    art.width = 96; art.stride = 191;
    cache.capture(snapshot, 200, &art);
    CHECK(!cache.artwork().pixels);
    art.stride = 192;
    snapshot.artwork_url[0] = 0;
    cache.capture(snapshot, 200, &art);
    CHECK(!cache.artwork().pixels);
    cache.capture(snapshot, 200, nullptr);
    CHECK(cache.online && !cache.artwork().pixels);
    std::puts("BOTTIE_CACHE_OK ownership/lifetime/bounds/freshness");
}
