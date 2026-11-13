#include "bottie/detail.hpp"
#include <cstring>

namespace bottie {
bool parse_digest(const char* hex, Digest& digest) {
    if (!hex || std::strlen(hex) != 64) return false;
    Digest result{};
    for (unsigned n=0;n<64;++n) {
        const char c=hex[n];
        const int value=c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : -1;
        if(value<0) return false;
        result[n/2] |= static_cast<uint8_t>(value << (n%2 ? 0 : 4));
    }
    digest=result; return true;
}
void format_digest(const Digest& digest,char* hex) {
    constexpr char alphabet[]="0123456789abcdef";
    for(unsigned n=0;n<32;++n) { hex[2*n]=alphabet[digest[n]>>4]; hex[2*n+1]=alphabet[digest[n]&15]; }
    hex[64]=0;
}
void MetadataIdentity::assign(const Snapshot& value) {
    std::memcpy(track,value.track_id,sizeof track); std::memcpy(title,value.title,sizeof title);
    std::memcpy(artist,value.artist,sizeof artist); std::memcpy(album,value.album,sizeof album);
    std::memcpy(context,value.context_name,sizeof context); context_type=value.context_type;
}
bool MetadataIdentity::matches(const Snapshot& value) const {
    return !std::strcmp(track,value.track_id) && !std::strcmp(title,value.title) && !std::strcmp(artist,value.artist) &&
        !std::strcmp(album,value.album) && !std::strcmp(context,value.context_name) && context_type==value.context_type;
}
bool DetailSelection::select(const Snapshot& value,uint8_t f,uint8_t p,uint32_t request) {
    active=false;
    if(!value.details.valid || f>=4 || p>=value.details.fields[f].count) return false;
    metadata.assign(value); identity=value.details.identity; digest=value.details.fields[f].pages[p];
    field=f; page=p; count=value.details.fields[f].count; flags=value.details.fields[f].flags;
    sequence=request; active=true; return true;
}
bool DetailSelection::matches(const Snapshot& value) const {
    return active && value.details.valid && field<4 && page<value.details.fields[field].count &&
        identity==value.details.identity && digest==value.details.fields[field].pages[page] &&
        count==value.details.fields[field].count && flags==value.details.fields[field].flags && metadata.matches(value);
}
bool DetailSelection::same(const DetailSelection& other) const {
    return active && other.active && sequence==other.sequence && identity==other.identity && digest==other.digest &&
        field==other.field && page==other.page && count==other.count && flags==other.flags &&
        !std::strcmp(metadata.track,other.metadata.track) && !std::strcmp(metadata.title,other.metadata.title) &&
        !std::strcmp(metadata.artist,other.metadata.artist) && !std::strcmp(metadata.album,other.metadata.album) &&
        !std::strcmp(metadata.context,other.metadata.context) && metadata.context_type==other.metadata.context_type;
}
bool parse_detail(const uint8_t* bytes,size_t length,const DetailSelection& selection,DetailPage& page) {
    page={};
    if(!selection.active || selection.field>=4 || !bytes || length!=detail_capacity || std::memcmp(bytes,"BTDT",4) ||
       bytes[4]!=1 || bytes[5]!=1 || bytes[6]!=1 || bytes[7]!=selection.field || bytes[8]!=selection.page ||
       bytes[9]!=selection.count || !bytes[9] || bytes[9]>4 || bytes[8]>=bytes[9] ||
       bytes[10]!=selection.flags || bytes[10]>3 || bytes[11] || bytes[12]!=0x80 || bytes[13]!=0x28 || bytes[14] || bytes[15]) return false;
    page={bytes+16,bytes[7],bytes[8],bytes[9],bytes[10]}; return true;
}
bool CachedDetail::capture(const DetailSelection& request,const uint8_t* body,size_t length) {
    clear(); DetailPage page;
    if(!parse_detail(body,length,request,page)) return false;
    selection=request; std::memcpy(bytes.data(),body,length); ready=true; return true;
}
DetailPage CachedDetail::view(const Snapshot& snapshot,const DetailSelection& request) const {
    DetailPage page;
    if(ready && selection.same(request) && request.matches(snapshot)) parse_detail(bytes.data(),bytes.size(),selection,page);
    return page;
}
} // namespace bottie
