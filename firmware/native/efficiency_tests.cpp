#include "bottie/dirty.hpp"
#include "bottie/network.hpp"
#include "bottie/network_policy.hpp"
#include "input_service.hpp"
#include "playback_cache.hpp"
#include "control_mailbox.hpp"
#include "work_schedule.hpp"
#include "recovered_profile.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Efficiency check line %d: %s\n",__LINE__,#x); std::exit(1); } } while(false)
using namespace bottie;
namespace {
Model playing() {
    Model m; m.received = m.online = true; m.snapshot.state = Playback::playing;
    m.snapshot.duration_ms = 180000; m.snapshot.capabilities = {true,true,true,true,true,true,true};
    std::strcpy(m.snapshot.control_target,"0123456789abcdef0123456789abcdef");
    std::strcpy(m.snapshot.title,"a title"); std::strcpy(m.snapshot.album,"an album");
    std::strcpy(m.snapshot.artist,"an artist"); std::strcpy(m.snapshot.artwork_url,"/v1/artwork/orbit");
    m.snapshot.artwork_width = m.snapshot.artwork_height = 96;
    return m;
}
void clocks() {
    const std::string base = R"({"schema_version":1,"state":"idle","track_id":"","title":"","artist":"","album":"","progress_ms":0,"duration_ms":0,"updated_at_ms":1,"shuffle":false,"repeat":"off","capabilities":{"play":false,"pause":false,"previous":false,"next":false,"shuffle":false,"repeat":false},"clock":)";
    for (const auto* value : {"null","{}","true",R"({"unix_ms":946684799999,"utc_offset_min":0})",R"({"unix_ms":4102444800000,"utc_offset_min":0})",
         R"({"unix_ms":946684800000,"utc_offset_min":841})",R"({"unix_ms":946684800000,"utc_offset_min":-841})",
         R"({"unix_ms":946684800000.0,"utc_offset_min":0})",R"({"unix_ms":946684800000,"utc_offset_min":0.0})"}) {
        const auto json = base+value+"}"; Snapshot snapshot;
        CHECK(parse_snapshot(json.data(),json.size(),snapshot)==Error::none && !snapshot.clock.valid());
    }
    Model m;
    CHECK(m.clock_minute(0)==-1);
    for (const auto offset : {-840,0,330,840}) {
        auto json = base + "{\"unix_ms\":946684800000,\"utc_offset_min\":"+std::to_string(offset)+"}}";
        CHECK(m.ingest(json.data(),json.size(),100)==Error::none);
        CHECK(m.clock_minute(100)==(1440+offset)%1440);
        CHECK(m.clock_minute(60099)==(1440+offset)%1440);
        CHECK(m.clock_minute(60100)==(1441+offset)%1440);
        CHECK(m.clock_minute(99)==-1);
    }
    m.disconnected(); CHECK(m.clock_minute(86400100)==840);
    auto absent = base+"null}"; CHECK(m.ingest(absent.data(),absent.size(),200)==Error::none);
    CHECK(m.clock_minute(60100)==841);
    Snapshot late = m.snapshot; late.updated_at_ms=0; late.clock={4102444799999ULL,0};
    CHECK(m.apply_snapshot(late,300)==Error::stale && m.clock_minute(60100)==841);
    m = {}; Snapshot last; last.updated_at_ms=1; last.clock={clock_max_ms,-840};
    CHECK(m.apply_snapshot(last,0)==Error::none && m.clock_minute(0)==599 && m.clock_minute(1)==-1);
}
struct Panel : recovered::Transport {
    std::vector<uint16_t> image = std::vector<uint16_t>(240*296,0xdead);
    size_t cursor{}, bytes{}, calls{}, windows{};
    int fail_call{-1};
    bool reset_level(bool) override { return true; }
    void delay_ms(uint16_t) override {}
    bool command(uint8_t code,const uint8_t* data,size_t) override {
        if (code==0x2b) { cursor=size_t((data[0]<<8)|data[1])*240; ++windows; }
        return true;
    }
    bool begin_pixels() override { return true; }
    bool pixels_write(const uint8_t* data,size_t length) {
        ++calls; if (fail_call==int(calls)) return false;
        bytes+=length;
        for(size_t n=0;n<length;n+=2) image[cursor++]=uint16_t((data[n]<<8)|data[n+1]);
        return true;
    }
    bool pixels(const uint8_t* data,size_t length) override { return pixels_write(data,length); }
    bool end_pixels() override { return true; }
};
void redraw() {
    auto model=playing(); DirtyBands dirty; SettingsView settings; CachedPlayback cache;
    std::array<uint8_t,96*96*2> art_bytes{}; Artwork art{art_bytes.data(),96,96,192};
    std::array<uint16_t,240*16> band{}; std::vector<uint16_t> full(240*296);
    Panel panel;
    auto run=[&](uint64_t now,bool fail=false) {
        cache.capture(model.snapshot,now, &art);
        dirty.prepare(model,now,settings,cache.art_revision,cache.text_revision);
        for(uint16_t y=0;y<296;y+=16) if(dirty.dirty(y)) {
            Frame f{band.data(),band.size(),240,296,240,y,uint16_t(std::min(16,296-int(y)))};
            CHECK(render(model,now,f,&art)); CHECK(render_settings(model,now,f,settings));
            const bool ok=recovered::present(panel,f); dirty.complete(y,ok);
            if(!ok) { CHECK(dirty.dirty(y)); break; }
        }
        if(!fail) {
            CHECK(!dirty.any()); CHECK(render(model,now,{full.data(),full.size(),240,296,240},&art));
            CHECK(render_settings(model,now,{full.data(),full.size(),240,296,240},settings));
            CHECK(full==panel.image);
        }
    };
    run(0); CHECK(panel.bytes==142080 && panel.calls==37 && panel.windows==19);
    size_t count=panel.calls; run(0); CHECK(panel.calls==count);
    for(uint64_t n=50;n<=1000;n+=50) run(n);
    std::strcpy(model.snapshot.title,"x"); run(1000);
    std::strcpy(model.snapshot.context_name,"playlist"); model.snapshot.context_type=Context::playlist; run(1000);
    std::strcpy(model.snapshot.album,"b"); run(1000);
    art_bytes.fill(0xff); run(1000); CHECK(cache.art_revision==2);
    settings.open=true; settings.volume_available=true; settings.volume=20; run(1000);
    count=panel.calls; run(2000); CHECK(panel.calls==count);
    settings.brightness=55; run(2000);
    model.snapshot.capabilities.volume=false; settings.volume_available=false; run(2000);
    settings.open=false; run(2000);
    panel.fail_call=int(panel.calls)+2; run(2500,true); panel.fail_call=-1; run(2500);
    model.snapshot.state=Playback::paused; run(2500);
    count=panel.calls; run(3000); CHECK(panel.calls==count);
    model.clock_anchor={946684800000ULL,330}; model.clock_received_ms=3000; run(3000);
    count=panel.calls; run(62999); CHECK(panel.calls>count); // Natural stale transition also clears the status.
    count=panel.calls; run(63000); CHECK(panel.calls>count && panel.calls-count<=4);
    model.disconnected(); run(64000);
}
void latency() {
    for(bool headers : {false,true}) {
        auto model=playing(); Interaction ui; ui.refresh(model,0); PointerQueue queue; InputServiceStats stats;
        ControlMailbox mailbox; WorkSchedule schedule; schedule.polled(100);
        network::AssetInterrupt interrupt; std::atomic<bool> cancelled{false};
        const auto ticket=interrupt.ticket(); network::RequestBudget request{cancelled,&interrupt,ticket,6100};
        uint64_t now=100, queued=0, dispatch=0; unsigned posts=0;
        CHECK(schedule.next(now,false)==WorkerWork::asset);
        auto pause=[&]() {
            now+=10;
            if(now==120) CHECK(queue.push({true,true,false,120,268,now}));
            if(now==180) CHECK(queue.push({true,false,false,0,0,now}));
            service_inputs(ui,model,240,296,[&]() {
                PointerSample sample; if(queue.pop(sample,now)) return InputEvent{EventType::pointer,{},sample};
                return InputEvent{};
            },[&](){return now;},[&](const Intent& intent){CHECK(mailbox.put(intent,now));queued=now;interrupt.interrupt();},[](uint8_t){},stats);
        };
        struct Held final : network::policy::Stream {
            const network::RequestBudget& budget; uint64_t& now; decltype(pause)& wait;
            std::string header; size_t offset{};
            Held(const network::RequestBudget& b,uint64_t& t,decltype(pause)& w,bool hold_headers)
                :budget(b),now(t),wait(w),header(hold_headers?"":"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 10\r\n\r\n"){}
            int read(uint8_t* out,size_t capacity) override {
                return network::read_until(budget,[&](){return now;},wait,[&](){
                    if(offset==header.size()) return -99;
                    const size_t size=std::min(capacity,header.size()-offset);
                    std::memcpy(out,header.data()+offset,size); offset+=size; return int(size);
                },[](int error){return error==-99;});
            }
        } stream(request,now,pause,headers);
        uint8_t output[16]; network::policy::Response response;
        CHECK(network::policy::read_response(stream,network::policy::Media::artwork,output,sizeof output,response)!=network::policy::ResponseError::none);
        CHECK(queued==180 && now==180 && schedule.next(now,mailbox.pending())==WorkerWork::control);
        Intent intent; CHECK(mailbox.take(model,now,intent)); char body[128]; CHECK(encode_intent(intent,body,sizeof body));
        ++posts; dispatch=now; CHECK(intent.action==Action::pause && dispatch-queued<=10 && posts==1);
        CHECK(!mailbox.take(model,now+1,intent));
        network::RequestBudget baseline{cancelled,nullptr,0,6100}; CHECK(baseline.alive(now));
        CHECK(!baseline.alive(6100)); CHECK(mailbox.put(intent,180));
        CHECK(!mailbox.take(model,6100,intent));
        std::printf("BOTTIE_LATENCY held=%s after_start=100 release=180 local_dispatch=180 worker_ready=%llu baseline_ready=6100 baseline_expired=1 posts=%u\n",headers?"headers":"body",(unsigned long long)dispatch,posts);
    }
}
void scene_fence() {
    auto model=playing(); Interaction ui; SceneInputFence fence;
    fence.observe(model,0,ui); CHECK(fence.waiting()); fence.presented(ui,10);
    ui.update({true,false,false,0,0,20},model,20);
    ui.update({true,true,false,120,268,40},model,40);
    model.snapshot.control_target[0]='f'; fence.observe(model,60,ui);
    fence.before_input(ui,100);
    CHECK(!ui.update({true,false,false,0,0,100},model,100).control);
    CHECK(!ui.update({true,true,false,120,268,120},model,120).control);
    fence.presented(ui,170);
    CHECK(!ui.update({true,false,false,0,0,140},model,170).control);
    CHECK(!ui.update({true,true,false,120,268,180},model,180).control);
    ui.update({true,false,false,0,0,200},model,200);
    ui.update({true,true,false,120,268,220},model,220);
    CHECK(ui.update({true,false,false,0,0,280},model,280).control);
}
void local_settings_during_io() {
    auto model=playing(); Interaction ui; ui.refresh(model,0); PointerQueue queue; InputServiceStats stats;
    uint64_t now=100, opened=0; unsigned commands=0;
    std::atomic<bool> stopped{false}; network::RequestBudget budget{stopped,nullptr,0,6100};
    const int result=network::read_until(budget,[&](){return now;},[&](){
        now+=10;
        if(now>=120 && now<=800 && now%20==0) CHECK(queue.push({true,true,false,120,148,now}));
        const bool changed=service_inputs(ui,model,240,296,[&](){
            PointerSample p;return queue.pop(p,now)?InputEvent{EventType::pointer,{},p}:InputEvent{};
        },[&](){return now;},[&](const Intent&){++commands;},[](uint8_t){},stats);
        if(changed && ui.settings.open && !opened)opened=now;
    },[](){return -99;},[](int e){return e==-99;});
    CHECK(result==-1 && opened==780 && commands==0 && now==6100);
    std::puts("BOTTIE_LOCAL_LATENCY held_request_start=100 down=120 long_press_threshold=770 settings_open=780 post_count=0");
}
}
int main(){ clocks(); redraw(); latency(); scene_fence(); local_settings_during_io(); std::puts("BOTTIE_EFFICIENCY_OK clock, reconstructed bands, late palette, failure retry, held-asset control cancellation"); }
