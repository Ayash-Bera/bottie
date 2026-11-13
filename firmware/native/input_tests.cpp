#include "bottie/interaction.hpp"
#include "control_mailbox.hpp"
#include "touch_device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Input check line%d: %s\n", __LINE__, #c); std::exit(1); } } while(false)
using namespace bottie;
namespace {
constexpr char target[] = "0123456789abcdef0123456789abcdef";
Model playing() {
    Model m; m.received = m.online = true;
    m.snapshot.state = Playback::playing;
    m.snapshot.updated_at_ms = 1;
    m.snapshot.duration_ms = 180000;
    m.snapshot.capabilities = {true, true, true, true, true, true, true};
    m.snapshot.target_known = true;
    m.snapshot.volume_percent = 45;
    std::strcpy(m.snapshot.control_target, target);
    return m;
}
PointerSample point(unsigned x, unsigned y, uint64_t at, bool down = true) {
    return {true, down, false, uint16_t(x), uint16_t(y), at};
}
class FakeBus final : public TouchBus {
public:
    bool ok{true};
    uint8_t bytes[5]{1, 0, 27, 0, 120};
    bool read(uint8_t reg, uint8_t* out, size_t length, int timeout) override {
        CHECK(reg == 2 && length == 5 && timeout == 10);
        std::memcpy(out, bytes, length); return ok;
    }
};
void reports_and_queue() {
    struct Corner { unsigned rx, ry, x, y; };
    for (auto c : {Corner{0,0,0,295}, {295,0,0,0}, {0,239,239,295}, {295,239,239,0}}) {
        uint8_t bytes[]{1, uint8_t(c.rx >> 8), uint8_t(c.rx), uint8_t(c.ry >> 8), uint8_t(c.ry)};
        PointerSample p;
        CHECK(decode_cst810(bytes, 5, 10, p) && p.down && p.x == c.x && p.y == c.y);
    }
    for (auto bytes : {std::array<uint8_t,5>{1,1,40,0,0}, {1,0,0,0,240}, {2,0,0,0,0}}) {
        PointerSample p; CHECK(!decode_cst810(bytes.data(), 5, 0, p) && !p.valid);
    }
    PointerSample p;
    CHECK(!decode_cst810(nullptr, 5, 0, p));
    uint8_t zero[5]{};
    CHECK(!decode_cst810(zero, 4, 0, p));
    CHECK(decode_cst810(zero, 5, 0, p) && p.valid && !p.down);
    FakeBus bus;
    CHECK(sample_touch(bus, 40).x == 120 && sample_touch(bus, 40).y == 268);
    PointerQueue queue;
    CHECK(queue.push(sample_touch(bus, 40)));
    CHECK(queue.push(sample_touch(bus, 60)));
    bus.bytes[0] = 0;
    CHECK(queue.push(sample_touch(bus, 100)));
    CHECK(queue.push(sample_touch(bus, 120)));
    auto model = playing(); Interaction ui;
    unsigned dispatched = 0, read = 0;
    const uint64_t expected[] = {40,60,100,120};
    while (queue.pop(p, 170)) {
        CHECK(p.at_ms == expected[read++]);
        const auto action = ui.update(p, model, 170);
        if (action.control) { ++dispatched; CHECK(action.intent.action == Action::pause); }
    }
    CHECK(read == 4 && dispatched == 1);
    bus.ok = false; CHECK(!sample_touch(bus, 180).valid);
    for (unsigned n = 0; n < 32; ++n) CHECK(queue.push(point(120,268,200+n)));
    CHECK(!queue.push(point(120,268,240)));
    CHECK(queue.pop(p, 250) && p.cancel);
    CHECK(!ui.update(p, model, 250).control);
    CHECK(!ui.update(point(120,268,260), model, 260).control);
    CHECK(!ui.update(point(0,0,280,false), model, 280).control);
}
void gestures() {
    auto model = playing();
    const Action expected[]{Action::shuffle,Action::previous,Action::pause,Action::next,Action::repeat};
    for (unsigned n = 0; n < 5; ++n) {
        Interaction ui;
        CHECK(!ui.update(point(24+48*n,268,40), model, 40).control);
        auto action = ui.update(point(0,0,100,false), model, 100);
        CHECK(action.control && action.intent.action == expected[n]);
        CHECK(!ui.update(point(0,0,120,false), model, 120).control);
    }
    Interaction held;
    for (uint64_t t = 40; t <= 700; t += 20) CHECK(!held.update(point(120,268,t), model, t).control);
    CHECK(held.settings.open);
    CHECK(!held.update(point(0,0,720,false), model, 720).control);
    Interaction drag;
    drag.update(point(120,268,40), model, 40);
    drag.update(point(170,268,80), model, 80);
    CHECK(!drag.update(point(0,0,120,false), model, 120).control);
    CHECK(!drag.settings.open);
    Interaction lost;
    lost.update(point(120,268,40), model, 40);
    PointerSample invalid{}; invalid.at_ms = 260;
    CHECK(!lost.update(invalid, model, 260).control);
    CHECK(!lost.update(point(120,268,280), model, 280).control);
    CHECK(!lost.update(point(0,0,300,false), model, 300).control);
    lost.update(point(120,268,320), model, 320);
    CHECK(lost.update(point(0,0,380,false), model, 380).control);
    Interaction delayed;
    CHECK(!delayed.update(point(120,268,40), model, 400).control);
    CHECK(!delayed.update(point(0,0,100,false), model, 400).control);
    Interaction changed;
    changed.update(point(120,268,40), model, 40);
    model.snapshot.control_target[0] = 'f';
    CHECK(!changed.update(point(0,0,100,false), model, 100).control);
    model = playing(); model.snapshot.control_target[0] = 0;
    Interaction legacy;
    legacy.update(point(120,268,40), model, 40);
    CHECK(!legacy.update(point(0,0,100,false), model, 100).control);
    model = playing(); model.snapshot.state = Playback::idle;
    Interaction idle;
    idle.update(point(120,148,40), model, 40);
    CHECK(!idle.update(point(0,0,100,false), model, 100).control);
    for (uint64_t t = 200; t <= 860; t += 20) CHECK(!idle.update(point(120,148,t), model, t).control);
    CHECK(idle.settings.open);
    idle.update(point(0,0,880,false), model, 880);
    idle.update(point(72,228,900), model, 900);
    auto resume = idle.update(point(0,0,960,false), model, 960);
    CHECK(resume.control && resume.intent.action == Action::play);
    model.disconnected();
    idle.update(point(72,228,1000), model, 1000);
    CHECK(!idle.update(point(0,0,1060,false), model, 1060).control);
    idle.update(point(168,228,1100), model, 1100);
    idle.update(point(0,0,1160,false), model, 1160);
    CHECK(!idle.settings.open);
}
void sliders_and_mailbox() {
    auto model = playing(); Interaction ui;
    ui.settings.open = true; ui.refresh(model, 0);
    CHECK(ui.settings.volume == 45);
    CHECK(ui.update(point(30,101,40),model,40).brightness_changed && ui.settings.brightness == 10);
    ui.update(point(239,220,80),model,80);
    CHECK(ui.settings.brightness == 100 && ui.settings.open);
    CHECK(!ui.update(point(0,0,100,false),model,100).control && ui.settings.open);
    CHECK(brightness_duty(0) == 102 && brightness_duty(10) == 102 && brightness_duty(100) == 1023 && brightness_duty(255) == 1023);
    auto volume = ui.update(point(30,147,140),model,140);
    CHECK(volume.control && volume.intent.action == Action::volume && volume.intent.volume == 0);
    auto max = ui.update(point(239,147,180),model,180);
    CHECK(max.control && max.intent.volume == 100);
    model.snapshot.control_target[0] = 'f'; ui.refresh(model,200);
    CHECK(!ui.update(point(120,147,220),model,220).control);
    ui.update(point(0,0,240,false),model,240);
    model.snapshot.capabilities.volume = false; ui.refresh(model,260);
    CHECK(ui.settings.volume == -1);
    model = playing(); model.snapshot.volume_percent = -1; ui.refresh(model,280);
    CHECK(ui.settings.volume == -1);
    model = playing(); ControlMailbox box; Intent intent, out;
    for (uint8_t n = 0; n <= 100; ++n) { CHECK(model.volume_intent(n, 500, intent)); CHECK(box.put(intent,500)); }
    CHECK(box.take(model,1000,out) && out.action == Action::volume && out.volume == 100);
    CHECK(!box.take(model,1001,out));
    CHECK(model.volume_intent(20,1100,intent) && box.put(intent,1100));
    CHECK(!box.take(model,1500,out));
    CHECK(box.take(model,2000,out) && out.volume == 20);
    CHECK(model.intent(Button::next,2000,intent));
    for (unsigned i = 0; i < 4; ++i) CHECK(box.put(intent,2000));
    CHECK(!box.put(intent,2000));
    model.snapshot.control_target[0] = 'f';
    CHECK(!box.take(model,2100,out));
    model = playing(); CHECK(model.volume_intent(15,2200,intent) && box.put(intent,2200));
    model.disconnected(); CHECK(!box.take(model,2300,out));
    model.online = true; CHECK(!box.take(model,2400,out));
    CHECK(box.put(intent,2500)); CHECK(!box.take(model,4500,out));
}
void queued_context_changes() {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        for (bool volume : {false, true}) {
            auto model = playing();
            Interaction ui; ui.settings.open = volume; ui.refresh(model, 0);
            PointerQueue queue;
            const unsigned y = volume ? 147 : 268;
            for (auto p : {point(120,y,40), point(0,0,80,false), point(180,y,100), point(0,0,140,false)})
                CHECK(queue.push(p));
            if (scenario == 0) model.snapshot.control_target[0] = 'f';
            if (scenario == 1) model.snapshot.control_target[0] = 0;
            if (scenario == 2) model.disconnected();
            if (scenario == 3) model.age_at_receive_ms = stale_after_ms;
            if (scenario == 4) model.snapshot.state = Playback::paused;
            if (scenario == 5) std::strcpy(model.snapshot.track_id, "new-track");
            if (scenario == 6) model.snapshot.capabilities.volume = false;
            ui.refresh(model, 170);
            PointerSample sample;
            while (queue.pop(sample,170)) CHECK(!ui.update(sample,model,170).control);
            CHECK(!ui.update(point(0,0,170,false),model,170).control);
            CHECK(!ui.update(point(180,y,180),model,180).control);
            // Reauthorization, including returning to the same target, creates another cutoff.
            model = playing(); model.received_ms = 200;
            ui.refresh(model,210);
            CHECK(!ui.update(point(0,0,200,false),model,210).control);
            CHECK(!ui.update(point(120,y,220),model,220).control);
            CHECK(!ui.update(point(0,0,240,false),model,240).control);
            CHECK(!ui.update(point(180,y,100),model,240).control);
            const auto down = ui.update(point(30,y,260),model,260);
            const auto up = ui.update(point(0,0,300,false),model,300);
            CHECK(volume ? down.control && down.intent.action == Action::volume : up.control && up.intent.action == Action::shuffle);
        }
    }
    auto model = playing(); Interaction local;
    local.settings.open = true; local.refresh(model,0);
    CHECK(local.update(point(30,101,40),model,40).brightness_changed);
    model.snapshot.control_target[0] = 'f'; local.refresh(model,100);
    CHECK(local.settings.open && local.settings.brightness == minimum_brightness);
    CHECK(!local.update(point(239,101,80),model,100).brightness_changed);
    local.update(point(0,0,120,false),model,120);
    CHECK(local.update(point(209,101,140),model,140).brightness_changed);
    CHECK(local.settings.brightness == 100);
    Interaction first;
    Model empty; first.refresh(empty,0);
    first.refresh(model,170);
    CHECK(!first.update(point(120,268,40),model,170).control);
    CHECK(!first.update(point(0,0,100,false),model,170).control);
    CHECK(!first.update(point(120,268,180),model,180).control);
    first.update(point(0,0,200,false),model,200);
    first.update(point(120,268,220),model,220);
    CHECK(first.update(point(0,0,280,false),model,280).control);
    Interaction expires; model = playing(); expires.refresh(model,0);
    expires.refresh(model,stale_after_ms);
    CHECK(!expires.update(point(120,268,stale_after_ms-100),model,stale_after_ms).control);
    CHECK(!expires.update(point(0,0,stale_after_ms-40,false),model,stale_after_ms).control);
    model.received_ms = stale_after_ms + 20;
    expires.refresh(model,stale_after_ms+40);
    CHECK(!expires.update(point(0,0,stale_after_ms+20,false),model,stale_after_ms+40).control);
    CHECK(!expires.update(point(120,268,stale_after_ms+60),model,stale_after_ms+60).control);
    expires.update(point(0,0,stale_after_ms+80,false),model,stale_after_ms+80);
    expires.update(point(120,268,stale_after_ms+100),model,stale_after_ms+100);
    CHECK(expires.update(point(0,0,stale_after_ms+140,false),model,stale_after_ms+140).control);
}
void protocol() {
    const std::string base = R"({"schema_version":1,"state":"playing","track_id":"x","title":"t","artist":"a","album":"b","progress_ms":0,"duration_ms":1000,"updated_at_ms":1,"shuffle":false,"repeat":"off","capabilities":{"play":true,"pause":true,"previous":true,"next":true,"shuffle":true,"repeat":true,"volume":true},"control_target":"0123456789abcdef0123456789abcdef","volume_percent":)";
    for (const char* value : {"null","0","100"}) {
        Snapshot s; const auto json = base + value + "}";
        CHECK(parse_snapshot(json.data(),json.size(),s) == Error::none);
        CHECK(s.volume_percent == (std::strcmp(value,"null") == 0 ? -1 : std::atoi(value)));
    }
    for (const char* value : {"-1","101","1.1","true","\"50\"","1e1"}) {
        Snapshot s; const auto json = base + value + "}";
        CHECK(parse_snapshot(json.data(),json.size(),s) != Error::none);
    }
    for (const std::string& value : std::vector<std::string>{"x", "0123456789abcdef0123456789abcdeF", std::string(33, 'a')}) {
        auto json = base + "40}";
        json.replace(json.find(target), 32, value);
        Snapshot s; CHECK(parse_snapshot(json.data(),json.size(),s) != Error::none);
    }
    auto model = playing(); Intent intent; char body[128];
    CHECK(model.volume_intent(0,0,intent) && encode_intent(intent,body,sizeof body));
    CHECK(std::string(body) == R"({"action":"volume","value":0,"control_target":"0123456789abcdef0123456789abcdef"})");
    CHECK(!encode_intent(intent,body,32));
    intent.control_target[0] = 0; CHECK(!encode_intent(intent,body,sizeof body));
    std::memset(intent.control_target, 'a', sizeof intent.control_target);
    CHECK(!encode_intent(intent,body,sizeof body));
}
void render_checks() {
    auto model = playing();
    for (auto dims : {std::pair<uint16_t,uint16_t>{128,128}, {240,296}}) {
        const auto [width,height] = dims;
        std::vector<uint16_t> original(size_t(width)*height), full(original.size()), bands(original.size());
        Frame f{original.data(),original.size(),width,height,width};
        CHECK(render(model,1000,f));
        auto saved = original;
        CHECK(render_settings(model,1000,f,{}) && saved == original);
        for (int volume : {-1,0,100}) {
            SettingsView settings{true,10,int16_t(volume)};
            f.pixels = full.data(); CHECK(render(model,1000,f)); CHECK(render_settings(model,1000,f,settings));
            CHECK(full != original);
            for (uint16_t count : {uint16_t(1),uint16_t(16),uint16_t(31)}) {
                for (uint16_t y = 0; y < height; y += count) {
                    uint16_t rows = std::min(count,uint16_t(height-y));
                    Frame band{bands.data()+size_t(y)*width,size_t(width)*rows,width,height,width,y,rows};
                    CHECK(render(model,1000,band)); CHECK(render_settings(model,1000,band,settings));
                }
                CHECK(full == bands);
            }
        }
    }
}
}
int main() {
    reports_and_queue(); gestures(); sliders_and_mailbox(); queued_context_changes(); protocol(); render_checks();
    std::puts("BOTTIE_INPUT_OK raw mapping, timed queue, gestures, stale cancel, sliders, target guards and overlay bands");
}
