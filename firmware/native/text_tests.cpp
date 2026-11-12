#include "bottie/text_mask.hpp"
#include "bottie/network_policy.hpp"
#include "playback_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <openssl/evp.h>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Text check line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)
using namespace bottie;
namespace {
const std::string base = R"({"schema_version":1,"state":"playing","track_id":"fixture","title":"বাংলা 日本語 ABC","artist":"শ্রেয়া ঘোষাল","album":"東京","context":{"type":"playlist","name":"শ্রাবণ"},"progress_ms":12000,"duration_ms":180000,"updated_at_ms":1700000000000,"shuffle":false,"repeat":"off","artwork":null,"capabilities":{"play":true,"pause":true,"previous":true,"next":true,"shuffle":true,"repeat":true}})";

std::vector<uint8_t> bundle(uint8_t flags) {
    size_t payload = 0;
    for (unsigned n = 0; n < 4; ++n) if (flags & (1 << n)) payload += text_boxes[n].width / 2 * text_boxes[n].height;
    std::vector<uint8_t> bytes(16 + payload);
    std::memcpy(bytes.data(), "BTXT\1\1", 6);
    bytes[6] = flags; bytes[7] = 1; bytes[8] = 240; bytes[10] = 40; bytes[11] = 1;
    bytes[12] = payload & 255; bytes[13] = (payload >> 8) & 255;
    for (size_t n = 16; n < bytes.size(); ++n) bytes[n] = static_cast<uint8_t>(n);
    return bytes;
}
std::string path_for(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32]; unsigned length = 0;
    CHECK(EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) == 1 && length == 32);
    std::string path = "/v1/text/";
    for (uint8_t byte : digest) {
        constexpr char hex[] = "0123456789abcdef";
        path += hex[byte >> 4]; path += hex[byte & 15];
    }
    return path;
}
Snapshot snapshot_for(const std::vector<uint8_t>& bytes) {
    Snapshot snapshot;
    const std::string json = base.substr(0, base.size()-1) + ",\"text_mask\":{\"url\":\"" + path_for(bytes) + "\",\"layout\":1}}";
    CHECK(parse_snapshot(json.data(), json.size(), snapshot) == Error::none);
    CHECK(snapshot.text_mask_layout == 1 && std::string(snapshot.text_mask_url) == path_for(bytes));
    return snapshot;
}
void structure() {
    auto bytes = bundle(15);
    CHECK(bytes.size() == text_mask_capacity);
    TextMask parsed;
    CHECK(parse_text_mask(bytes.data(), bytes.size(), parsed) && parsed.flags == 15);
    CHECK(parsed.planes[1] - parsed.planes[0] == 1908 && parsed.planes[2] - parsed.planes[1] == 1818 &&
          parsed.planes[3] - parsed.planes[2] == 2808);
    for (size_t n = 0; n < bytes.size(); ++n) CHECK(!parse_text_mask(bytes.data(), n, parsed));
    auto long_body = bytes; long_body.push_back(0);
    CHECK(!parse_text_mask(long_body.data(), long_body.size(), parsed));
    for (size_t n = 0; n < 16; ++n) {
        auto bad = bytes; bad[n] ^= 128;
        CHECK(!parse_text_mask(bad.data(), bad.size(), parsed) && !parsed.bytes);
    }
    for (unsigned flags = 1; flags < 16; ++flags) {
        auto partial = bundle(flags);
        CHECK(parse_text_mask(partial.data(), partial.size(), parsed));
        for (unsigned n = 0; n < 4; ++n) CHECK(bool(parsed.planes[n]) == bool(flags & (1 << n)));
    }
    auto empty = bundle(0);
    CHECK(!parse_text_mask(empty.data(), empty.size(), parsed));
    const auto path = path_for(bytes);
    CHECK(network::policy::text_digest_valid(path.c_str(), bytes.data(), bytes.size()));
    for (size_t n : {size_t(0), size_t(15), bytes.size()-1}) {
        bytes[n] ^= 1;
        CHECK(!network::policy::text_digest_valid(path.c_str(), bytes.data(), bytes.size()));
        bytes[n] ^= 1;
    }
    for (const auto& bad : {path + "?x", path + "/", std::string("http://1.2.3.4") + path,
                           path.substr(0,72), std::string("/v1/text/") + std::string(64,'A'), std::string("/v1/text/../control")}) {
        CHECK(!text_mask_path(bad.c_str()));
        const auto json = base.substr(0,base.size()-1) + ",\"text_mask\":{\"layout\":1,\"url\":\"" + bad + "\"}}";
        Snapshot snapshot;
        CHECK(parse_snapshot(json.data(), json.size(), snapshot) == Error::none && !snapshot.text_mask_url[0]);
    }
    for (const auto* descriptor : {"null", "false", "1", "{}", "{\"layout\":2}", "{\"layout\":1,\"url\":false}"}) {
        const auto json = base.substr(0,base.size()-1) + ",\"text_mask\":" + descriptor + "}";
        Snapshot snapshot;
        CHECK(parse_snapshot(json.data(), json.size(), snapshot) == Error::none && !snapshot.text_mask_url[0]);
        CHECK(snapshot.state == Playback::playing && snapshot.capabilities.pause);
    }
    CHECK(!requires_text_mask("ASCII ABC 123 -’‘\"“”‐‑‒–— "));
    CHECK(requires_text_mask("বাংলা") && requires_text_mask("日本語") && requires_text_mask("ABC 東京"));
}
void ownership() {
    auto bytes = bundle(15);
    auto snapshot = snapshot_for(bytes);
    CHECK(required_text_planes(snapshot) == 15);
    TextCache cache;
    CHECK(cache.observe(snapshot));
    std::memcpy(cache.storage(), bytes.data(), bytes.size());
    CHECK(cache.commit(snapshot, bytes.size()));
    CHECK(cache.view(snapshot).bytes);
    CHECK(!cache.observe(snapshot));
    auto progress = snapshot;
    ++progress.progress_ms; ++progress.updated_at_ms; progress.control_target[0] = 'a';
    CHECK(!cache.observe(progress) && cache.view(progress).bytes);
    for (unsigned n = 0; n < 8; ++n) {
        auto changed = snapshot;
        switch (n) {
        case 0: changed.track_id[0] = 'x'; break;
        case 1: changed.title[0] = 'x'; break;
        case 2: changed.artist[0] = 'x'; break;
        case 3: changed.album[0] = 'x'; break;
        case 4: changed.context_name[0] = 'x'; break;
        case 5: changed.context_type = Context::album; break;
        case 6: changed.text_mask_url[9] = changed.text_mask_url[9] == 'a' ? 'b' : 'a'; break;
        case 7: changed.text_mask_layout = 2; break;
        }
        CHECK(!cache.view(changed).bytes);
        CHECK(cache.observe(changed));
        CHECK(!cache.view(snapshot).bytes && !cache.view(changed).bytes);
        CHECK(!cache.commit(snapshot, bytes.size()));
        CHECK(cache.observe(snapshot));
        std::memcpy(cache.storage(), bytes.data(), bytes.size());
        CHECK(cache.commit(snapshot, bytes.size()));
    }
    CachedPlayback shared;
    auto view = cache.view(snapshot);
    shared.capture(snapshot, 0, nullptr, &view);
    auto displayed = shared;
    CHECK(displayed.text_mask().planes[0][1] == bytes[17]);
    std::memset(cache.storage(), 0, bytes.size());
    shared.offline();
    CHECK(!shared.text_mask().bytes && displayed.text_mask().planes[0][1] == bytes[17]);
    CHECK(displayed.text_mask().bytes == displayed.text_bytes.data());
    displayed.capture(snapshot, 100, nullptr, nullptr);
    CHECK(displayed.online && !displayed.text_mask().bytes);
    cache.clear(); CHECK(!cache.view(snapshot).bytes);

    auto album = snapshot;
    album.context_type = Context::album;
    CHECK(required_text_planes(album) == 13);
    std::strcpy(album.title, "ASCII"); std::strcpy(album.artist, "ASCII"); std::strcpy(album.album, "ASCII");
    CHECK(required_text_planes(album) == 0);
    cache.observe(album); std::memcpy(cache.storage(), bytes.data(), bytes.size());
    CHECK(!cache.commit(album, bytes.size()));
}
void pixels() {
    auto bytes = bundle(15);
    const auto snapshot = snapshot_for(bytes);
    Model model;
    CHECK(model.apply_snapshot(snapshot, 0) == Error::none);
    TextMask mask;
    CHECK(parse_text_mask(bytes.data(), bytes.size(), mask));
    std::vector<uint16_t> whole(240*296), plain(whole.size());
    Frame full{whole.data(),whole.size(),240,296,240};
    CHECK(render(model,100,full,nullptr,&mask));
    CHECK(render(model,100,{plain.data(),plain.size(),240,296,240}));
    size_t differences = 0;
    for (unsigned y = 0; y < 296; ++y) for (unsigned x = 0; x < 240; ++x) {
        if (whole[y*240+x] == plain[y*240+x]) continue;
        ++differences;
        bool inside = false;
        for (const auto box : text_boxes) inside |= x >= box.x && x < box.x+box.width && y >= box.y && y < box.y+box.height;
        CHECK(inside);
    }
    CHECK(differences > 1000);
    const auto title = text_boxes[2];
    // Even/odd nibbles and opaque/transparent samples are independent of font rasterization.
    bytes[16+1908+1818] = 0xf0;
    CHECK(render(model,100,full,nullptr,&mask));
    CHECK(whole[title.y*240+title.x] == 0xeef7 && whole[title.y*240+title.x+1] == 0x0882);
    for (const auto state : {Playback::playing,Playback::paused,Playback::idle,Playback::unavailable}) {
        model.snapshot.state = state;
        CHECK(render(model,100,full,nullptr,&mask));
        for (unsigned rows : {1U,16U,31U}) {
            std::vector<uint16_t> reconstructed(whole.size()), band(240*rows+2,0xdead);
            for (unsigned y = 0; y < 296; y += rows) {
                unsigned count = std::min(rows,296-y);
                Frame part{band.data()+1,240*rows,240,296,240,uint16_t(y),uint16_t(count)};
                CHECK(render(model,100,part,nullptr,&mask));
                CHECK(band.front() == 0xdead && band.back() == 0xdead);
                std::copy_n(band.data()+1,240*count,reconstructed.data()+240*y);
            }
            CHECK(reconstructed == whole);
        }
    }
    model.snapshot.state = Playback::playing;
    for (char* field : {model.snapshot.title,model.snapshot.artist,model.snapshot.album,model.snapshot.context_name})
        std::strcpy(field,"ASCII ’ “ -");
    CHECK(render(model,100,full,nullptr,&mask));
    CHECK(render(model,100,{plain.data(),plain.size(),240,296,240}));
    CHECK(whole == plain);
    CHECK(render(model,100,{plain.data(),plain.size(),128,128,128},nullptr,&mask));
}
void scheduling() {
    using namespace network::policy;
    AssetSchedule schedule;
    CHECK(schedule.choose(true,true) == Asset::text);
    CHECK(schedule.choose(true,true) == Asset::artwork);
    CHECK(schedule.choose(false,false) == Asset::none);
    CHECK(schedule.choose(true,false) == Asset::text);
    CHECK(schedule.choose(false,true) == Asset::artwork);
}
}
int main() {
    structure(); ownership(); pixels(); scheduling();
    std::puts("BOTTIE_TEXT_OK format/digest/metadata/ownership/alpha/bands/scheduling");
}
