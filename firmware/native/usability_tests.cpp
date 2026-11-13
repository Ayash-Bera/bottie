#include "bottie/preferences.hpp"
#include "bottie/interaction.hpp"
#include "input_service.hpp"
#include "work_schedule.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "bottie/detail.hpp"
#include "bottie/feedback.hpp"
#include "control_mailbox.hpp"
#include "bottie/dirty.hpp"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"Usability line%d: %s\n",__LINE__,#c); std::exit(1); } } while(false)
using namespace bottie;
namespace {
struct FakePreferences final : PreferenceBackend {
    StoreResult init_result{StoreResult::ok},read_result{StoreResult::missing},write_result{StoreResult::ok};
    PreferenceBytes stored=encode_preferences({40,1});
    uint32_t foreign{0x12345678}; unsigned reads{},writes{},initializations{};
    StoreResult initialize() override { ++initializations; return init_result; }
    StoreResult read(PreferenceBytes& bytes,size_t& size) override { ++reads; bytes=stored; size=stored.size(); return read_result; }
    StoreResult write(Preferences value) override { ++writes; if(write_result==StoreResult::ok) stored=encode_preferences(value); return write_result; }
};
void storage() {
    for (unsigned brightness = 10; brightness <= 100; ++brightness) for (unsigned mode = 0; mode < 5; ++mode) {
        Preferences value{uint8_t(brightness),uint8_t(mode)}, decoded;
        const auto bytes = encode_preferences(value);
        CHECK(decode_preferences(bytes.data(),bytes.size(),decoded) == StoreResult::ok && decoded == value);
        for (size_t n = 0; n < bytes.size(); ++n) {
            auto broken = bytes; broken[n] ^= 1;
            CHECK(decode_preferences(broken.data(),broken.size(),decoded) != StoreResult::ok);
        }
    }
    PreferenceState state; PreferenceWrite write;
    state.loaded(StoreResult::missing);
    CHECK(state.value().brightness == 100 && state.value().dim_mode == 2 && state.writable());
    CHECK(!state.take(10000,write));
    state.change({25,2}); CHECK(!state.take(11000,write));
    state.release(11000); CHECK(!state.take(11499,write)); CHECK(state.take(11500,write));
    CHECK(!state.take(12000,write));
    state.complete(write,StoreResult::ok); CHECK(state.status() == SaveStatus::saved);
    state.change({30,1}); state.release(12000); CHECK(!state.take(13499,write)); CHECK(state.take(13500,write));
    state.complete(write,StoreResult::full); CHECK(state.status() == SaveStatus::unsaved);
    CHECK(!state.take(50000,write));
    state.release(50000); CHECK(state.take(50500,write));
    state.complete(write,StoreResult::write_error); CHECK(!state.take(100000,write));
    for (auto result : {StoreResult::unknown_version,StoreResult::full,StoreResult::unavailable,StoreResult::write_error}) {
        state.loaded(result); state.change({20,0}); state.release(100);
        CHECK(!state.writable() && !state.take(10000,write) && state.value().brightness == 20);
    }
    state.loaded(StoreResult::corrupt); state.change({42,4}); state.release(10); CHECK(state.take(510,write));
    auto first = write;
    state.change({100,2}); state.release(600);
    state.complete(first,StoreResult::ok);
    CHECK(state.status() == SaveStatus::pending && !state.take(2509,write) && state.take(2510,write));
    CHECK(write.value.brightness == 100); state.complete(write,StoreResult::ok);
    state.complete(first,StoreResult::write_error); CHECK(state.status() == SaveStatus::saved);
    auto version = encode_preferences({70,2}); version[4] = 2;
    state.loaded(StoreResult::ok,version.data(),version.size()); CHECK(!state.writable());
    for(auto error:{StoreResult::full,StoreResult::unknown_version,StoreResult::unavailable}) {
        FakePreferences backend; backend.init_result=error; load_preferences(backend,state);
        CHECK(backend.initializations==1 && backend.reads==0 && backend.writes==0 && backend.foreign==0x12345678);
        state.change({60,1}); state.release(10); CHECK(!state.take(510,write));
    }
    for(auto error:{StoreResult::missing,StoreResult::ok,StoreResult::corrupt,StoreResult::write_error}) {
        FakePreferences backend; backend.read_result=error; load_preferences(backend,state);
        CHECK(backend.reads==1 && backend.writes==0 && backend.foreign==0x12345678);
        CHECK(!state.take(10000,write));
        if(state.writable()) {
            state.change({60,1}); state.release(10000); CHECK(state.take(10500,write));
            backend.write_result=StoreResult::write_error;
            state.complete(write,backend.write(write.value));
            CHECK(state.status()==SaveStatus::unsaved && backend.foreign==0x12345678 && backend.writes==1);
        }
    }
}
void uncommitted_defaults() {
    for (auto initial : {StoreResult::missing,StoreResult::corrupt}) {
        PreferenceState state; PreferenceWrite write;
        state.loaded(initial);
        state.release(0);
        CHECK(!state.take(10000,write) && state.status()!=SaveStatus::saved);
        state.change({50,2}); state.change({100,2}); state.release(100);
        CHECK(state.status()==SaveStatus::pending && !state.take(599,write) && state.take(600,write));
        CHECK((write.value==Preferences{100,2}));
        state.complete(write,StoreResult::write_error);
        CHECK(state.status()==SaveStatus::unsaved && !state.take(10000,write));
        state.release(10000); CHECK(state.take(10500,write));
        state.complete(write,StoreResult::ok); CHECK(state.status()==SaveStatus::saved);
        state.change({50,2}); state.change({100,2}); state.release(12000);
        CHECK(state.status()==SaveStatus::saved && !state.take(12500,write));

        state.loaded(initial); state.change({50,2}); state.release(100); CHECK(state.take(600,write));
        state.change({100,2}); state.release(650);
        state.complete(write,StoreResult::write_error);
        CHECK(state.status()==SaveStatus::pending && !state.take(2599,write) && state.take(2600,write));
        CHECK((write.value==Preferences{100,2}));
        state.complete(write,StoreResult::ok); CHECK(state.status()==SaveStatus::saved);
    }
}
Model playing(uint64_t received = 0) {
    Model model; model.received = model.online = true; model.received_ms = received;
    model.snapshot.state = Playback::playing; model.snapshot.duration_ms = 100000;
    model.snapshot.capabilities = {true,true,true,true,true,true,true};
    std::strcpy(model.snapshot.control_target,"0123456789abcdef0123456789abcdef");
    return model;
}
PointerSample point(uint64_t at, bool down, unsigned x=120, unsigned y=268) {
    return {true,down,false,uint16_t(x),uint16_t(y),at};
}
void power_and_wake() {
    DisplayPower power;
    power.configure({100,2},0);
    CHECK(!power.tick(59999) && !power.dimmed());
    CHECK(!power.tick(60000) && power.dimmed());
    CHECK(power.tick(60020) && power.applied() == 99);
    CHECK(power.tick(61600) && power.applied() == 20 && power.wait_ms(61600) == 1000);
    CHECK(power.activity(62000,true) && power.applied() == 100 && power.swallowing());
    CHECK(!power.activity(62020,true)); CHECK(!power.activity(62040,false) && !power.swallowing());
    power.configure({10,1},63000); power.tick(94000); CHECK(power.applied() == 10);
    power.configure({80,0},95000); CHECK(!power.tick(900000) && !power.dimmed());

    auto model = playing(60000); Interaction ui;
    ui.refresh(model,60000); ui.tick_power(60000); ui.tick_power(61600);
    auto wake = ui.update(point(62000,true),model,62000);
    CHECK(wake.wake && wake.brightness_changed && !wake.control && ui.refresh_pending());
    CHECK(!ui.update(point(62040,false),model,62040).control);
    CHECK(!ui.update(point(62060,true),model,62060).control);
    CHECK(!ui.update(point(62100,false),model,62100).control);
    model.received_ms = 62200; ui.refresh(model,62200);
    CHECK(!ui.refresh_pending());
    CHECK(!ui.update(point(62120,true),model,62200).control);
    CHECK(!ui.update(point(62160,false),model,62200).control);
    CHECK(!ui.update(point(62200,false),model,62200).control);
    CHECK(!ui.update(point(62220,true),model,62220).control);
    CHECK(!ui.update(point(62240,false),model,62240).control);
    CHECK(!ui.update(point(62260,true),model,62260).control);
    CHECK(ui.update(point(62300,false),model,62300).control);
    ui.tick_power(130000); ui.tick_power(132000);
    wake = ui.update(point(132020,true),model,132020); CHECK(wake.wake);
    PointerSample invalid{}; invalid.at_ms=132300; ui.update(invalid,model,132300);
    CHECK(!ui.update(point(132320,true),model,132320).control);
    CHECK(!ui.update(point(132340,false),model,132340).control);
    model.disconnected(); ui.refresh(model,132360);
    ui.update(point(132380,false),model,132380);
    for (uint64_t t=132400;t<=133060;t+=20) ui.update(point(t,true,120,148),model,t);
    CHECK(ui.settings.open);
}
void release_and_cadence() {
    auto model = playing(); Interaction ui; ui.settings.open=true;
    const auto layout = settings_layout(240,296);
    CHECK(ui.update(point(40,true,30,layout.brightness.y+5),model,40).brightness_changed);
    CHECK(ui.update(point(100,false),model,100).preferences_released);
    CHECK(!ui.update(point(120,false),model,120).preferences_released);
    ui.update(point(140,true,100,layout.dim_timeout.y+5),model,140);
    const auto mode = ui.update(point(200,false),model,200);
    CHECK(mode.preferences_released && ui.settings.dim_mode == 3);
    WorkSchedule schedule; schedule.polled(100,false);
    CHECK(schedule.next(5099,false) != WorkerWork::metadata && schedule.next(5100,false) == WorkerWork::metadata);
    schedule.refresh(5200); CHECK(schedule.next(5200,false) == WorkerWork::metadata);
    schedule.polled(5200,true); CHECK(schedule.next(6200,false) == WorkerWork::metadata);
}
const std::string base = R"("schema_version":1,"state":"playing","track_id":"track","title":"song","artist":"artist","album":"album","progress_ms":10,"duration_ms":100000,"updated_at_ms":1,"shuffle":false,"repeat":"off","capabilities":{"play":true,"pause":true,"next":true,"previous":true,"shuffle":true,"repeat":true})";
void optional_parser_bounds() {
    const std::string hash(64,'a');
    const std::string valid=R"({"layout":1,"id":")"+hash+R"(","title":{"pages":[")"+hash+R"("],"flags":1}})";
    std::string large="[0"; for(unsigned n=0;n<500;++n) large+=",0"; large+="]";
    unsigned accepted=0;
    for(unsigned n=0;n<140;++n) {
        std::string padding;
        for(unsigned i=0;i<n;++i) padding+=",\"x"+std::to_string(i)+"\":0";
        const auto without="{"+base+padding+"}";
        Snapshot control;
        const auto expected=parse_snapshot(without.data(),without.size(),control);
        for(const auto& value: {std::string("null"),valid,large}) for(bool first:{false,true}) {
            const auto added="\"details\":"+value;
            const auto json=first?"{"+added+","+base+padding+"}":"{"+base+padding+","+added+"}";
            CHECK(json.size()<=max_json_bytes);
            Snapshot output;
            const auto error=parse_snapshot(json.data(),json.size(),output);
            if(expected==Error::none) { CHECK(error==Error::none); CHECK(!std::strcmp(output.title,"song")); ++accepted; }
            else CHECK(error!=Error::none);
        }
    }
    CHECK(accepted>500);
    const auto json="{"+base+",\"details\":"+valid+"}";
    Snapshot snapshot; CHECK(parse_snapshot(json.data(),json.size(),snapshot)==Error::none && snapshot.details.valid);
    CHECK(snapshot.details.fields[0].count==1 && snapshot.details.fields[0].flags==1);
}
void seek_and_feedback() {
    auto model=playing(); model.snapshot.capabilities.seek=true;
    std::strcpy(model.snapshot.track_id,"track"); std::strcpy(model.snapshot.control_track,"abcdef0123456789abcdef0123456789");
    Intent seek; CHECK(model.seek_intent(100000,10,seek) && seek.position_ms==99999);
    char json[256]; CHECK(encode_intent(seek,json,sizeof json) && std::strstr(json,"control_track"));
    ControlMailbox mailbox; Intent out;
    CHECK(mailbox.put(seek,100) && mailbox.take(model,110,out));
    seek.position_ms=20; CHECK(mailbox.put(seek,200));
    CHECK(!mailbox.take(model,1099,out)); CHECK(mailbox.take(model,1110,out) && out.position_ms==20);
    CHECK(mailbox.put(seek,1200)); CHECK(!mailbox.take(model,2200,out));
    CHECK(mailbox.put(seek,2300)); model.snapshot.control_track[0]='0'; CHECK(!mailbox.take(model,2301,out));
    seek.release_known=true; seek.released_ms=2500;
    CHECK(mailbox.put(seek,2670)); CHECK(!mailbox.take(model,3500,out));
    CHECK(!mailbox.put(seek,3500));
    model.snapshot.control_track[0]='a'; Interaction ui; ui.refresh(model,0);
    ui.update(point(40,true,20,230),model,40);
    for(uint64_t t=60;t<=180;t+=20) CHECK(!ui.update(point(t,true,219,234),model,t).control);
    const auto released=ui.update(point(200,false),model,200);
    CHECK(released.control && released.intent.action==Action::seek && released.intent.position_ms==99999);
    CHECK(!ui.update(point(220,false),model,220).control);
    Feedback feedback; Intent action; CHECK(model.intent(Button::play_pause,300,action)); action.sequence=1;
    feedback.begin(action,model,300); auto old=action; action.sequence=2; feedback.begin(action,model,320);
    feedback.result({old,{FeedbackPhase::sent},340},340); CHECK(feedback.phase()==FeedbackPhase::queued);
    feedback.result({action,{FeedbackPhase::sent},360},360); CHECK(feedback.phase()==FeedbackPhase::sent);
    feedback.update(model,400); CHECK(feedback.phase()==FeedbackPhase::sent);
    model.snapshot.state=Playback::paused; model.snapshot.updated_at_ms=2;
    feedback.update(model,420); CHECK(feedback.phase()==FeedbackPhase::confirmed);
    model.snapshot.control_target[0]='f'; feedback.update(model,440); CHECK(feedback.phase()==FeedbackPhase::none);
}
void detail_and_dirty() {
    auto model=playing();
    std::strcpy(model.snapshot.title,"song"); std::strcpy(model.snapshot.artist,"artist");
    std::strcpy(model.snapshot.album,"album");
    model.snapshot.details.valid=true;
    for(auto& field:model.snapshot.details.fields) { field.count=4; field.flags=3; }
    std::array<uint8_t,detail_capacity> bytes{};
    std::memcpy(bytes.data(),"BTDT",4); bytes[4]=bytes[5]=bytes[6]=1;
    bytes[9]=4; bytes[10]=3; bytes[12]=0x80; bytes[13]=0x28;
    for(size_t n=16;n<bytes.size();++n) bytes[n]=uint8_t(n*17);
    CachedDetail shared,displayed; DetailSelection selected;
    CHECK(selected.select(model.snapshot,0,0,1)); CHECK(shared.capture(selected,bytes.data(),bytes.size()));
    displayed=shared; shared.clear(); bytes.fill(0);
    auto view=displayed.view(model.snapshot,selected); CHECK(view.pixels && view.pixels[1]==uint8_t(17*17));
    auto changed=model.snapshot;
    for(unsigned kind=0;kind<10;++kind) {
        changed=model.snapshot;
        switch(kind) {
        case 0: changed.title[0]='x'; break;
        case 1: changed.artist[0]='x'; break;
        case 2: changed.album[0]='x'; break;
        case 3: changed.context_name[0]='x'; break;
        case 4: changed.track_id[0]='x'; break;
        case 5: changed.context_type=Context::playlist; break;
        case 6: ++changed.details.identity[0]; break;
        case 7: ++changed.details.fields[0].pages[0][0]; break;
        case 8: --changed.details.fields[0].count; break;
        case 9: --changed.details.fields[0].flags; break;
        }
        CHECK(!selected.matches(changed) && !displayed.view(changed,selected).pixels);
    }
    changed=model.snapshot; changed.progress_ms=400; changed.control_target[0]='f';
    changed.control_track[0]='a'; changed.clock={946684800000ULL,330};
    CHECK(selected.matches(changed) && displayed.view(changed,selected).pixels);
    auto new_request=selected; ++new_request.sequence;
    CHECK(!displayed.view(model.snapshot,new_request).pixels);
    new_request=selected; new_request.active=false; CHECK(!displayed.view(model.snapshot,new_request).pixels);
    for(size_t n=0;n<16;++n) {
        bytes=displayed.bytes; bytes[n]^=0x80; DetailPage invalid;
        CHECK(!parse_detail(bytes.data(),bytes.size(),selected,invalid));
    }
    DetailPage invalid;
    CHECK(!parse_detail(displayed.bytes.data(),detail_capacity-1,selected,invalid));

    DirtyBands dirty; SettingsView settings;
    std::vector<uint16_t> screen(240*296),full(screen.size()); std::array<uint16_t,240*16> band{};
    auto reconstruct=[&](uint64_t now,int fail_y=-1) {
        dirty.prepare(model,now,settings,0,0);
        bool injected=false;
        for(uint16_t y=0;y<296;y+=16) if(dirty.dirty(y)) {
            const uint16_t rows=std::min<unsigned>(16,296-y);
            Frame target{band.data(),band.size(),240,296,240,y,rows};
            CHECK(render(model,now,target)); CHECK(render_settings(model,now,target,settings,&view));
            if(y==fail_y) { dirty.complete(y,false); injected=true; break; }
            std::copy_n(band.data(),rows*240,screen.data()+y*240); dirty.complete(y,true);
        }
        if(injected) { CHECK(dirty.any()); return; }
        CHECK(!dirty.any()); Frame target{full.data(),full.size(),240,296,240};
        CHECK(render(model,now,target)); CHECK(render_settings(model,now,target,settings,&view));
        CHECK(screen==full);
    };
    reconstruct(0);
    settings.detail_open=settings.detail_ready=true; settings.detail_count=4; settings.detail_flags=3; settings.detail_sequence=1;
    reconstruct(100); reconstruct(150);
    settings.detail_ready=false; ++settings.detail_sequence; view={}; reconstruct(200);
    view=displayed.view(model.snapshot,selected); settings.detail_ready=true; reconstruct(250,64); reconstruct(250);
    settings.detail_open=false; reconstruct(300);
    settings.seek_preview=true; settings.seek_position=90000; reconstruct(350);
    settings.seek_position=1000; reconstruct(400); settings.seek_preview=false; reconstruct(450);
    settings.feedback=FeedbackPhase::sent; reconstruct(500);
    settings.open=true; reconstruct(550);
    settings.feedback=FeedbackPhase::failed; settings.feedback_error=ControlError::track_changed; reconstruct(600);
    settings.feedback_error=ControlError::rate_limited; reconstruct(650);
    settings.feedback=FeedbackPhase::none; reconstruct(700);
    settings.save_status=SaveStatus::saved; reconstruct(750);
    settings.open=false; reconstruct(800);
}
void queued_track_and_geometry() {
    auto model=playing(); model.snapshot.capabilities.seek=true;
    std::strcpy(model.snapshot.control_track,"abcdef0123456789abcdef0123456789");
    std::strcpy(model.snapshot.artist,"artist");
    model.snapshot.details.valid=true; model.snapshot.details.fields[1].count=1;
    for(unsigned y:{228U,229U,230U,245U,246U}) {
        Interaction ui; ui.refresh(model,0); ui.update(point(40,true,20,y),model,40);
        auto release=ui.update(point(100,false),model,170);
        if(y<230) CHECK(ui.settings.detail_open && ui.settings.detail_field==1 && !release.control);
        else if(y<246) CHECK(release.control && release.intent.action==Action::seek && release.intent.released_ms==100);
        else CHECK(release.control && release.intent.action==Action::shuffle);
    }
    Interaction ui; ui.refresh(model,0); SceneInputFence fence;
    fence.observe(model,0,ui); fence.presented(ui,1); ui.update(point(20,false),model,20);
    PointerQueue queue; CHECK(queue.push(point(40,true,120,234))); CHECK(queue.push(point(100,false)));
    model.snapshot.control_track[0]='0'; fence.observe(model,170,ui); ui.refresh(model,170);
    unsigned commands=0; InputServiceStats stats;
    fence.before_input(ui,170);
    service_inputs(ui,model,240,296,[&] { PointerSample sample; if(!queue.pop(sample,170)) return InputEvent{};
        InputEvent event; event.type=EventType::pointer; event.pointer=sample; return event; },
        [] { return uint64_t(170); },[&](const Intent&) { ++commands; },[](uint8_t) {},stats);
    CHECK(commands==0 && fence.waiting()); fence.presented(ui,180);
    ui.update(point(200,false),model,200); ui.update(point(220,true,120,234),model,220);
    auto release=ui.update(point(280,false),model,300); CHECK(release.control);
    ControlMailbox mailbox; Intent output;
    CHECK(mailbox.put(release.intent,450)); CHECK(!mailbox.take(model,1280,output));
    ui.update(point(1300,true,120,234),model,1300);
    PointerSample overflow{}; overflow.cancel=true; overflow.at_ms=1320;
    ui.update(overflow,model,1320); CHECK(!ui.update(point(1340,false),model,1340).control);
}
}
int main() {
    storage(); uncommitted_defaults(); power_and_wake(); release_and_cadence(); optional_parser_bounds(); seek_and_feedback();
    detail_and_dirty(); queued_track_and_geometry(); std::puts("BOTTIE_USABILITY_NATIVE_OK");
}
