#include "bottie/core.hpp"
#include "bottie/interaction.hpp"
#include "bottie/text_mask.hpp"
#include "recovered_profile.hpp"
#ifndef BOTTIE_BASELINE
#include "bottie/dirty.hpp"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace bottie;
struct Counter : recovered::Transport {
    uint64_t bytes{}, calls{}, windows{}; volatile uint32_t sample{};
    bool reset_level(bool) override{return true;} void delay_ms(uint16_t) override{}
    bool command(uint8_t code,const uint8_t*,size_t) override{if(code==0x2b)++windows;return true;}
    bool begin_pixels() override{return true;}
    bool pixels(const uint8_t* data,size_t length) override{bytes+=length;++calls;sample+=data[0]+data[length-1];return true;}
    bool end_pixels() override{return true;}
};
std::vector<uint8_t> load(const std::string& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){
    std::vector<std::string> cases{"idle","minute","playing","settings","metadata"};
    if(argc==3) for(auto name:{"bengali","japanese","mixed","album-only","long"}) cases.emplace_back(name);
    for(const auto& name:cases){
        Model original; original.received=original.online=true; original.snapshot.state=Playback::playing;
        original.snapshot.duration_ms=180000; original.snapshot.capabilities={true,true,true,true,true,true,true};
        std::strcpy(original.snapshot.title,"unchanged title");std::strcpy(original.snapshot.album,"unchanged album");
        std::strcpy(original.snapshot.artist,"artist");std::strcpy(original.snapshot.control_target,"0123456789abcdef0123456789abcdef");
        std::array<uint8_t,96*96*2> fallback{}; for(size_t n=0;n<fallback.size();++n)fallback[n]=uint8_t(n*71);
        Artwork art{fallback.data(),96,96,192};std::strcpy(original.snapshot.artwork_url,"/v1/artwork/orbit");
        std::vector<uint8_t> text_bytes,art_bytes;TextMask mask;
        if(argc==3){art_bytes=load(argv[2]);if(!parse_artwork(art_bytes.data(),art_bytes.size(),art))return 2;}
        if(name!="idle"&&name!="minute"&&name!="playing"&&name!="settings"&&name!="metadata"){
            auto json=load(std::string(argv[1])+"/"+name+".json");
            if(parse_snapshot(reinterpret_cast<const char*>(json.data()),json.size(),original.snapshot)!=Error::none)return 2;
            text_bytes=load(std::string(argv[1])+"/"+name+".btxt");
            if(!parse_text_mask(text_bytes.data(),text_bytes.size(),mask))return 2;
        }
        if(name=="idle"||name=="minute")original.snapshot.state=Playback::idle;
        SettingsView settings;settings.open=name=="settings";settings.volume_available=settings.open;
#ifndef BOTTIE_BASELINE
        original.clock_anchor={946684800000ULL,330};
#endif
        std::vector<double> render_times,wall_times;
        uint64_t calls=0,bytes=0,windows=0,render_calls=0;
        const unsigned frames=name=="minute"?130:400;
        for(unsigned repetition=0;repetition<7;++repetition){
            auto model=original;Counter transport;std::array<uint16_t,240*16> pixels{};
#ifndef BOTTIE_BASELINE
            DirtyBands dirty;
#endif
            double render_ns=0;uint64_t renders=0;const auto start=std::chrono::steady_clock::now();
            for(unsigned n=0;n<frames;++n){
                uint64_t now=uint64_t(n)*(name=="minute"?1000:100);model.received_ms=now;
                model.snapshot.progress_ms=now;
                if(name=="metadata"&&n==200){std::strcpy(model.snapshot.title,"x");std::strcpy(model.snapshot.album,"b");}
#ifndef BOTTIE_BASELINE
                dirty.prepare(model,now,settings,1,mask.length?1:0);
#endif
                for(uint16_t y=0;y<296;y+=16){
#ifndef BOTTIE_BASELINE
                    if(!dirty.dirty(y))continue;
#endif
                    Frame f{pixels.data(),pixels.size(),240,296,240,y,uint16_t(std::min(16,296-int(y)))};
                    const auto before=std::chrono::steady_clock::now();
                    if(!render(model,now,f,&art,&mask)||!render_settings(model,now,f,settings))return 1;
                    render_ns+=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-before).count();++renders;
                    if(!recovered::present(transport,f))return 1;
#ifndef BOTTIE_BASELINE
                    dirty.complete(y,true);
#endif
                }
            }
            wall_times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            render_times.push_back(render_ns/1e6);calls=transport.calls;bytes=transport.bytes;windows=transport.windows;render_calls=renders;
        }
        std::sort(render_times.begin(),render_times.end());std::sort(wall_times.begin(),wall_times.end());
        std::printf("{\"case\":\"%s\",\"frames\":%u,\"repetitions\":7,\"render_calls\":%llu,\"present_windows\":%llu,\"pixel_calls\":%llu,\"pixel_bytes\":%llu,\"render_ms_median\":%.6f,\"render_ms_min\":%.6f,\"render_ms_max\":%.6f,\"total_ms_median\":%.6f}\n",
            name.c_str(),frames,(unsigned long long)render_calls,(unsigned long long)windows,(unsigned long long)calls,(unsigned long long)bytes,render_times[3],render_times[0],render_times[6],wall_times[3]);
    }
}
