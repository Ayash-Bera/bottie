#include "bottie/core.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

using namespace bottie;
namespace {
const std::string valid = R"({"schema_version":1,"state":"playing","track_id":"fixture","title":"Orbital Sunrise","artist":"Bottie Ensemble","album":"Small Worlds","context":{"type":"playlist","name":"Desk Orbits"},"progress_ms":12000,"duration_ms":180000,"updated_at_ms":1700000000000,"shuffle":false,"repeat":"off","artwork":{"url":"/v1/artwork/fixture","width":96,"height":96},"capabilities":{"play":true,"pause":true,"previous":true,"next":true,"shuffle":true,"repeat":true}})";
std::string replace(std::string input, const std::string& from, const std::string& to) {
    auto pos = input.find(from);
    CHECK(pos != std::string::npos);
    return input.replace(pos, from.size(), to);
}
Error parse(const std::string& input, Snapshot& snapshot) { return parse_snapshot(input.data(), input.size(), snapshot); }
Error ingest(Model& model, const std::string& input, uint64_t now, uint64_t wall = 0) {
    return model.ingest(input.data(), input.size(), now, wall);
}

void parser_checks() {
    Snapshot snapshot;
    CHECK(parse(valid, snapshot) == Error::none);
    CHECK(snapshot.state == Playback::playing && snapshot.progress_ms == 12000);
    CHECK(snapshot.context_type == Context::playlist && std::strcmp(snapshot.context_name, "Desk Orbits") == 0);
    auto optional = replace(valid, R"("context":{"type":"playlist","name":"Desk Orbits"},)", "");
    optional = replace(optional, R"("artwork":{"url":"/v1/artwork/fixture","width":96,"height":96},)", "");
    CHECK(parse(optional, snapshot) == Error::none && snapshot.artwork_url[0] == 0 && snapshot.context_type == Context::unknown);
    CHECK(parse(replace(valid, R"({"type":"playlist","name":"Desk Orbits"})", "null"), snapshot) == Error::none);
    CHECK(parse(replace(valid, "Orbital Sunrise", R"(Star \uD83C\uDF0D \u00e9)"), snapshot) == Error::none);
    CHECK(std::strcmp(snapshot.title, "Star \xf0\x9f\x8c\x8d \xc3\xa9") == 0);
    CHECK(parse(replace(valid, "schema_version", R"(\u0073chema_version)"), snapshot) == Error::none);
    CHECK(parse(replace(valid, "Orbital Sunrise", std::string(256, 'x')), snapshot) == Error::none);
    CHECK(parse(replace(valid, "fixture\"", std::string(128, 'x') + '"'), snapshot) == Error::none);
    CHECK(parse(replace(valid, "1700000000000", "9007199254740991"), snapshot) == Error::none);
    auto extensions = replace(valid, "\"state\":", "\"extension\":{\"values\":[null,true,-1.2e+3]},\"state\":");
    CHECK(parse(extensions, snapshot) == Error::none);
    const std::vector<std::string> malformed = {
        "", "{}", "[]", valid + " false", valid.substr(0, valid.size() - 1), valid + std::string(8192, ' '),
        replace(valid, "\"schema_version\":1", "\"schema_version\":2"),
        replace(valid, "\"schema_version\":1", "\"schema_version\":1,\"schema_version\":1"),
        replace(valid, "\"schema_version\":1", "\"schema_version\":1,\"\\u0073chema_version\":1"),
        replace(valid, "Orbital Sunrise", std::string(257, 'x')),
        replace(valid, "Orbital Sunrise", R"(\u0000)"), replace(valid, "Orbital Sunrise", R"(\uD800x)"),
        replace(valid, "Orbital Sunrise", R"(\uDC00)"), replace(valid, "Orbital Sunrise", R"(\q)"),
        replace(valid, "Orbital Sunrise", "\xc0\xaf"), replace(valid, "Orbital Sunrise", "\xed\xa0\x80"),
        replace(valid, "Orbital Sunrise", "\xf4\x90\x80\x80"), replace(valid, "Orbital Sunrise", "\xe2\x82"),
        replace(valid, "12000", "-1"), replace(valid, "12000", "1.5"), replace(valid, "12000", "01"),
        replace(valid, "12000", "1e3"), replace(valid, "12000", "180001"), replace(valid, "180000", "86400001"),
        replace(valid, "1700000000000", "9007199254740992"), replace(valid, "1700000000000", "99999999999999999999999999"),
        replace(valid, "\"shuffle\":false", "\"shuffle\":0"), replace(valid, "\"repeat\":\"off\"", "\"repeat\":\"forever\""),
        replace(valid, "\"width\":96", "\"width\":241"), replace(valid, "\"width\":96", "\"width\":0"),
        replace(valid, "\"pause\":true", "\"pause\":null"), replace(valid, "\"pause\":true", "\"pause\":true,\"pause\":true"),
        replace(valid, "\"title\":", "\"title\" : :"), replace(valid, "\"next\":true,", "\"next\":true,,")
    };
    for (const auto& input : malformed) {
        std::strcpy(snapshot.title, "unchanged");
        CHECK(parse(input, snapshot) != Error::none);
        CHECK(std::strcmp(snapshot.title, "unchanged") == 0);
    }
    auto deep = replace(valid, "\"state\":", "\"extra\":[[[[[[[[[0]]]]]]]]],\"state\":");
    CHECK(parse(deep, snapshot) == Error::depth);
    std::string many = "[0";
    for (int n = 0; n < 260; ++n) many += ",0";
    many += "]";
    CHECK(parse(many, snapshot) == Error::tokens);
    uint32_t seed = 7;
    for (unsigned iteration = 0; iteration < 6000; ++iteration) {
        std::string mutated = valid;
        seed = seed * 1664525U + 1013904223U;
        size_t at = seed % mutated.size();
        mutated[at] = static_cast<char>(seed >> 24);
        parse(mutated, snapshot);
    }
}

void state_checks() {
    Model model;
    Intent intent;
    CHECK(model.effective_state(0) == Playback::unavailable && !model.intent(Button::next, 0, intent));
    CHECK(ingest(model, valid, 1000) == Error::none);
    CHECK(model.progress(2000) == 13000 && model.effective_state(15999) == Playback::playing);
    CHECK(model.effective_state(16000) == Playback::unavailable && model.progress(16000) == 12000);
    CHECK(model.effective_state(999) == Playback::unavailable);
    CHECK(ingest(model, valid, 14000) == Error::none);
    CHECK(model.effective_state(16000) == Playback::unavailable);
    CHECK(ingest(model, replace(valid, "playing", "unavailable"), 14000) == Error::none);
    CHECK(model.effective_state(14000) == Playback::unavailable);
    auto paused = replace(replace(valid, "playing", "paused"), "1700000000000", "1700000000001");
    CHECK(ingest(model, paused, 16000) == Error::none);
    CHECK(model.progress(17000) == 12000 && model.intent(Button::play_pause, 17000, intent) && intent.action == Action::play);
    CHECK(ingest(model, valid, 17000) == Error::stale);
    CHECK(model.effective_state(17000) == Playback::unavailable);
    auto fresh = replace(valid, "1700000000000", "1700000000002");
    CHECK(ingest(model, fresh, 18000) == Error::none);
    model.disconnected();
    CHECK(!model.intent(Button::next, 18001, intent));
    CHECK(ingest(model, "{bad}", 18002) != Error::none);
    CHECK(model.effective_state(18002) == Playback::unavailable);
    Model wall;
    CHECK(ingest(wall, valid, 0, 1700000015000) == Error::none);
    CHECK(wall.effective_state(0) == Playback::unavailable);
    Model young;
    CHECK(ingest(young, valid, 0, 1700000002000) == Error::none);
    CHECK(young.progress(1000) == 15000 && young.effective_state(13000) == Playback::unavailable);
    Model future;
    CHECK(ingest(future, valid, 0, 1699999990000) == Error::stale);
    Model ended;
    CHECK(ingest(ended, replace(valid, "12000", "179999"), 0) == Error::none);
    CHECK(ended.progress(1000) == 180000);

    Model idle;
    uint32_t initial = idle.planet_seed;
    idle.tick(29999);
    CHECK(idle.planet_seed == initial);
    idle.tick(30000);
    CHECK(idle.planet_seed != initial);
    initial = idle.planet_seed;
    CHECK(idle.tap_planet(30001) && idle.planet_seed != initial);
    CHECK(ingest(idle, valid, 40000) == Error::none);
    initial = idle.planet_seed;
    CHECK(!idle.tap_planet(40001));
    idle.tick(41000);
    CHECK(idle.planet_seed == initial);
    CHECK(ingest(idle, paused, 42000) == Error::none && idle.tap_planet(42001));
}

void control_checks() {
    Model model;
    CHECK(ingest(model, valid, 0) == Error::none);
    const char* expected[] = {R"({"action":"shuffle","value":true})", R"({"action":"previous"})",
        R"({"action":"pause"})", R"({"action":"next"})", R"({"action":"repeat","value":"context"})"};
    for (unsigned n = 0; n < 5; ++n) {
        Intent intent;
        char body[64];
        CHECK(model.intent(static_cast<Button>(n), 1, intent));
        CHECK(encode_intent(intent, body, sizeof body) && std::strcmp(body, expected[n]) == 0);
        char small[4] = "x";
        CHECK(!encode_intent(intent, small, sizeof small) && small[0] == 0);
    }
    Intent intent;
    model.snapshot.capabilities.next = false;
    CHECK(!model.intent(Button::next, 1, intent));
    model.snapshot.repeat = Repeat::context;
    CHECK(model.intent(Button::repeat, 1, intent) && intent.repeat == Repeat::track);
    model.snapshot.repeat = Repeat::track;
    CHECK(model.intent(Button::repeat, 1, intent) && intent.repeat == Repeat::off);
    model.snapshot.shuffle = true;
    CHECK(model.intent(Button::shuffle, 1, intent) && !intent.shuffle);
    model.snapshot.state = Playback::idle;
    CHECK(!model.intent(Button::play_pause, 1, intent));
    model.snapshot.state = Playback::paused;
    CHECK(model.intent(Button::play_pause, 1, intent) && intent.action == Action::play);
    model.snapshot.capabilities.play = false;
    CHECK(!model.intent(Button::play_pause, 1, intent));
    CHECK(!model.intent(static_cast<Button>(99), 1, intent));
}

std::vector<uint8_t> artwork_bytes(uint16_t width, uint16_t height) {
    uint32_t payload = uint32_t(width) * height * 2;
    std::vector<uint8_t> bytes(16 + payload);
    std::memcpy(bytes.data(), "BTAR", 4);
    bytes[4] = bytes[5] = 1;
    bytes[6] = width & 255; bytes[7] = width >> 8;
    bytes[8] = height & 255; bytes[9] = height >> 8;
    bytes[10] = (width * 2) & 255; bytes[11] = (width * 2) >> 8;
    for (int n = 0; n < 4; ++n) bytes[12 + n] = static_cast<uint8_t>(payload >> (8*n));
    for (size_t n = 16; n < bytes.size(); n += 2) { bytes[n] = 0; bytes[n + 1] = 0xf8; }
    return bytes;
}

void artwork_render_checks() {
    Artwork art;
    auto bytes = artwork_bytes(16, 8);
    CHECK(parse_artwork(bytes.data(), bytes.size(), art) && art.width == 16 && art.height == 8);
    for (size_t length = 0; length < bytes.size(); ++length) CHECK(!parse_artwork(bytes.data(), length, art));
    auto extra = bytes; extra.push_back(0);
    CHECK(!parse_artwork(extra.data(), extra.size(), art));
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(6), size_t(8), size_t(10), size_t(12)}) {
        auto corrupt = bytes; corrupt[offset] ^= 0xff;
        CHECK(!parse_artwork(corrupt.data(), corrupt.size(), art));
    }
    auto maximum = artwork_bytes(240, 240);
    CHECK(parse_artwork(maximum.data(), maximum.size(), art));
    auto too_big = artwork_bytes(241, 240);
    CHECK(!parse_artwork(too_big.data(), too_big.size(), art));
    CHECK(parse_artwork(bytes.data(), bytes.size(), art));
    std::vector<uint16_t> pixels(130 * 128 + 2, 0xdead);
    Frame frame{pixels.data() + 1, pixels.size() - 2, 128, 128, 130};
    Model model;
    CHECK(ingest(model, valid, 0) == Error::none);
    CHECK(render(model, 0, frame, &art));
    CHECK(frame.pixels[111 * 130 + 34] == 0xeef7 && frame.pixels[111 * 130 + 40] == 0x0882);
    CHECK(frame.pixels[111 * 130 + 93] == 0xeef7 && frame.pixels[111 * 130 + 85] == 0x0882);
    CHECK(pixels.front() == 0xdead && pixels.back() == 0xdead);
    for (size_t y = 0; y < 128; ++y) CHECK(frame.pixels[y*130 + 128] == 0xdead && frame.pixels[y*130 + 129] == 0xdead);
    for (int y = 35; y < 62; ++y)
        for (int x = 11; x < 66; ++x) CHECK(frame.pixels[y*130 + x] == 0xf800);
    CHECK(frame.pixels[21*130 + 11] != 0xf800 && frame.pixels[75*130 + 65] != 0xf800);
    uint32_t first = frame_hash(frame);
    CHECK(render(model, 0, frame, &art) && first == frame_hash(frame));
    Frame invalid = frame; invalid.capacity = 1;
    CHECK(!render(model, 0, invalid, &art));
    invalid = frame; invalid.stride = 127;
    CHECK(!render(model, 0, invalid));
    std::set<uint32_t> families;
    Model idle;
    for (uint32_t family = 0; family < 6; ++family) {
        idle.planet_seed = 12 + family;
        CHECK(render(idle, 0, frame)); families.insert(frame_hash(frame));
        first = frame_hash(frame);
        CHECK(render(idle, 1800, frame) && first != frame_hash(frame));
    }
    CHECK(families.size() == 6);
    idle.planet_seed = 10;
    CHECK(render(idle, 0, frame)); first = frame_hash(frame);
    CHECK(render(idle, 1800, frame) && first != frame_hash(frame));
    std::array<uint16_t, 240 * 240> large{};
    CHECK(render(idle, 0, {large.data(), large.size(), 240, 240, 240}));
    std::vector<uint16_t> portrait(240 * 296 + 2, 0xbeef);
    Frame recovered{portrait.data() + 1, portrait.size() - 2, 240, 296, 240};
    CHECK(render(idle, 0, recovered));
    CHECK(render(model, 0, recovered, &art));
    CHECK(portrait.front() == 0xbeef && portrait.back() == 0xbeef);
    for (int y = 82; y < 146; ++y)
        for (int x = 22; x < 150; ++x) CHECK(recovered.pixels[y * 240 + x] == 0xf800);
    CHECK(recovered.pixels[118 * 240 + 239] == 0x0882);

    std::strcpy(model.snapshot.title, "A'B-\"LIVE\" (2026)");
    CHECK(render(model, 0, recovered, &art));
    first = frame_hash(recovered);
    const auto punctuation_hash = first;
    std::strcpy(model.snapshot.title, "A\xe2\x80\x99" "B\xe2\x80\x93\xe2\x80\x9c" "LIVE\xe2\x80\x9d (2026)");
    CHECK(render(model, 0, recovered, &art) && frame_hash(recovered) == first);
    std::strcpy(model.snapshot.title, "ABCDEFGHIJKLMNO...");
    CHECK(render(model, 0, recovered, &art));
    first = frame_hash(recovered);
    std::strcpy(model.snapshot.title, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    CHECK(render(model, 0, recovered, &art) && frame_hash(recovered) == first);
    std::strcpy(model.snapshot.title, "A?B-?LIVE? ?2026?");
    CHECK(render(model, 0, recovered, &art) && frame_hash(recovered) != punctuation_hash);
}

void ui_checks() {
    std::vector<uint16_t> pixels(240*296), lower(pixels.size()), band(240*16);
    Frame frame{pixels.data(),pixels.size(),240,296,240};
    Model model;
    CHECK(ingest(model,valid,0) == Error::none);
    const char* alphabet = "abcdefghijklmnopqrstuvwxyz";
    for (char* field : {model.snapshot.title,model.snapshot.artist,model.snapshot.album,model.snapshot.context_name})
        std::strcpy(field,alphabet);
    CHECK(render(model,0,frame)); lower = pixels;
    for (char* field : {model.snapshot.title,model.snapshot.artist,model.snapshot.album,model.snapshot.context_name})
        for (char* p = field; *p; ++p) *p = char(*p-'a'+'A');
    CHECK(render(model,0,frame) && pixels == lower);
    std::strcpy(model.snapshot.title,"a");
    CHECK(render(model,0,frame));
    for (int y = 194; y < 198; ++y) for (int x = 115; x < 125; ++x) CHECK(pixels[y*240+x] == 0x0882);
    CHECK(pixels[198*240+117] == 0xeef7);
    std::strcpy(model.snapshot.title,"b");
    CHECK(render(model,0,frame) && pixels[194*240+115] == 0xeef7);

    auto bytes = artwork_bytes(96,96);
    Artwork art;
    CHECK(parse_artwork(bytes.data(),bytes.size(),art));
    CHECK(render(model,0,frame,&art));
    auto red = pixels[234*240+20];
    for (size_t n = 16; n < bytes.size(); n += 2) { bytes[n] = 31; bytes[n+1] = 0; }
    CHECK(render(model,0,frame,&art));
    auto blue = pixels[234*240+20];
    CHECK(red != blue && (red>>11) > (red&31) && (blue&31) > (blue>>11));
    CHECK(render(model,0,frame));
    auto neutral = pixels[234*240+20];
    for (size_t n = 16; n < bytes.size(); n += 2) { bytes[n] = 0x10; bytes[n+1] = 0x84; }
    CHECK(render(model,0,frame,&art) && pixels[234*240+20] == neutral);
    model.snapshot.artwork_url[0] = 0;
    for (size_t n = 16; n < bytes.size(); n += 2) { bytes[n] = 31; bytes[n+1] = 0; }
    CHECK(render(model,0,frame,&art) && pixels[234*240+20] == neutral);
    std::strcpy(model.snapshot.artwork_url,"/v1/artwork/new");
    CHECK(render(model,0,frame,&art) && pixels[234*240+20] == blue);

    for (unsigned y = 0; y < 96; ++y) for (unsigned x = 0; x < 96; ++x) {
        uint16_t value = uint16_t((x*31/95)<<11 | (y*63/95)<<5 | ((x+y)%32));
        bytes[16+(y*96+x)*2] = value & 255; bytes[17+(y*96+x)*2] = value>>8;
    }
    CHECK(render(model,0,frame,&art));
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        size_t offset = 16+((y*96/128)*96+x*96/128)*2;
        CHECK(pixels[(50+y)*240+22+x] == uint16_t(bytes[offset] | (uint16_t(bytes[offset+1])<<8)));
    }
    uint32_t playing = frame_hash(frame);
    CHECK(render(model,4000,frame,&art) && playing != frame_hash(frame));
    std::set<uint16_t> grooves;
    for (int y = 72; y < 155; ++y) for (int x = 184; x < 208; ++x) grooves.insert(pixels[y*240+x]);
    CHECK(grooves.size() >= 8);

    for (auto state : {Playback::playing,Playback::idle,Playback::paused,Playback::unavailable}) {
        model.snapshot.state = state;
        for (unsigned family = 0; family < 6; ++family) {
            model.planet_seed = 12+family;
            CHECK(render(model,1800,frame,&art));
            for (uint16_t y = 0; y < 296; y += 16) {
                uint16_t rows = std::min<uint16_t>(16,296-y);
                CHECK(render(model,1800,{band.data(),band.size(),240,296,240,y,rows},&art));
                CHECK(std::equal(band.begin(),band.begin()+240*rows,pixels.begin()+240*y));
            }
            if (state != Playback::playing) for (int y = 280; y < 296; ++y)
                for (int x = 0; x < 240; ++x) CHECK(pixels[y*240+x] == 0x0882);
        }
    }
}
} // namespace

int main() {
    parser_checks();
    state_checks();
    control_checks();
    artwork_render_checks();
    ui_checks();
    std::puts("BOTTIE_CORE_CHECKS_OK parser/state/controls/artwork/render");
}
