#include "bottie/core.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bottie {
namespace {
constexpr uint16_t ink = 0xeef7, dim = 0x7c50, background = 0x0882;
uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>(((r & 31) << 11) | ((g & 63) << 5) | (b & 31));
}
uint32_t noise(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}
struct Color { int r, g, b; };
constexpr uint8_t bayer[] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
uint16_t color565(Color color, int shade = 256, int dither = 0) {
    return rgb(std::clamp(color.r * shade / 256 + dither, 0, 255) >> 3,
               std::clamp(color.g * shade / 256 + dither, 0, 255) >> 2,
               std::clamp(color.b * shade / 256 + dither, 0, 255) >> 3);
}
unsigned root(unsigned value) {
    unsigned result = 0, bit = 1U << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) { value -= result + bit; result = (result >> 1) + bit; }
        else result >>= 1;
        bit >>= 2;
    }
    return result;
}
bool valid_art(const Artwork* art) {
    return art && art->pixels && art->width && art->height && art->width <= max_art_side &&
           art->height <= max_art_side && art->stride == art->width * 2;
}
Color artwork_color(const Artwork* art) {
    constexpr Color fallback{213,182,139};
    if (!valid_art(art)) return fallback;
    unsigned r = 0, g = 0, b = 0, weights = 0;
    // Fixed sampling cost also avoids stale colors when a cache reuses its pixel address.
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        size_t offset = ((2*y + 1) * art->height / 16) * art->stride + ((2*x + 1) * art->width / 16) * 2;
        unsigned pixel = art->pixels[offset] | (unsigned(art->pixels[offset + 1]) << 8);
        unsigned red = (pixel >> 11) * 255 / 31, green = ((pixel >> 5) & 63) * 255 / 63, blue = (pixel & 31) * 255 / 31;
        unsigned high = std::max({red,green,blue}), low = std::min({red,green,blue});
        if (high - low < 24 || high < 45) continue;
        unsigned weight = high - low;
        r += red * weight; g += green * weight; b += blue * weight; weights += weight;
    }
    if (!weights) return fallback;
    Color color{int(r / weights), int(g / weights), int(b / weights)};
    int luminance = (54*color.r + 183*color.g + 19*color.b) / 256;
    int lift = std::max(0, (150 - luminance) * 256 / (255 - luminance));
    color.r += (255-color.r)*lift/256;
    color.g += (255-color.g)*lift/256;
    color.b += (255-color.b)*lift/256;
    return color;
}
struct Canvas {
    Frame frame;
    int top() const { return frame.storage_y; }
    int bottom() const { return frame.storage_y + frame.rows(); }
    void pixel(int x, int y, uint16_t color) {
        if (x >= 0 && y >= top() && x < frame.width && y < bottom())
            frame.pixels[static_cast<size_t>(y - top()) * frame.stride + static_cast<size_t>(x)] = color;
    }
    void rect(int x, int y, int width, int height, uint16_t color) {
        for (int py = std::max(y, top()); py < std::min(y + height, bottom()); ++py)
            for (int px = std::max(x, 0); px < std::min(x + width, int(frame.width)); ++px) pixel(px, py, color);
    }
    void circle(int cx, int cy, int radius, uint16_t color) {
        for (int y = std::max(-radius, top() - cy); y <= std::min(radius, bottom() - cy - 1); ++y)
            for (int x = std::max(-radius, -cx); x <= std::min(radius, int(frame.width) - cx - 1); ++x)
                if (x*x + y*y <= radius*radius) pixel(cx + x, cy + y, color);
    }
};

const uint8_t* glyph(char c) {
    static constexpr uint8_t letters[][5] = {
        {32,84,84,84,120},{127,72,68,68,56},{56,68,68,68,40},{56,68,68,72,127},
        {56,84,84,84,24},{8,126,9,1,2},{12,82,82,82,62},{127,8,4,4,120},
        {0,68,125,64,0},{32,64,68,61,0},{127,16,40,68,0},{0,65,127,64,0},
        {124,4,120,4,120},{124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},
        {8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,36},{4,63,68,64,32},
        {60,64,64,32,124},{28,32,64,32,28},{60,64,48,64,60},{68,40,16,40,68},
        {12,80,80,80,60},{68,100,84,76,68}
    };
    static constexpr uint8_t digits[][5] = {
        {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
        {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
        {54,73,73,73,54},{6,73,73,41,30}
    };
    static constexpr uint8_t unknown[5] = {2,1,81,9,6}, space[5] = {}, dash[5] = {8,8,8,8,8};
    static constexpr uint8_t punctuation[][5] = {
        {0,96,96,0,0}, {0,64,32,0,0}, {0,54,54,0,0}, {0,0,3,0,0}, {0,3,0,3,0},
        {0,28,34,65,0}, {0,65,34,28,0}, {32,16,8,4,2}, {54,73,85,34,80},
        {0,0,95,0,0}, {8,8,62,8,8}, {64,64,64,64,64}
    };
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ') return space;
    if (c == '-') return dash;
    const char* marks = ".,:'\"()/&!+_";
    for (size_t n = 0; marks[n]; ++n) if (c == marks[n]) return punctuation[n];
    return unknown;
}

char display_character(const char*& value) {
    const auto first = static_cast<unsigned char>(*value++);
    if (first < 128) return static_cast<char>(first);
    unsigned continuation = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
    uint32_t codepoint = first & (continuation == 3 ? 7 : continuation == 2 ? 15 : 31);
    for (unsigned n = 0; n < continuation; ++n) {
        const auto next = static_cast<unsigned char>(*value);
        if (!next || (next & 0xc0) != 0x80) return '?';
        codepoint = (codepoint << 6) | (next & 63);
        ++value;
    }
    if (codepoint == 0x2018 || codepoint == 0x2019) return '\'';
    if (codepoint == 0x201c || codepoint == 0x201d) return '"';
    if (codepoint >= 0x2010 && codepoint <= 0x2014) return '-';
    if (codepoint == 0xa0) return ' ';
    return '?';
}

void text(Canvas& canvas, int x, int y, const char* value, int right, uint16_t color, int scale = 1) {
    if (y + 7 * scale <= canvas.top() || y >= canvas.bottom()) return;
    int slots = std::max(0, (right - x + scale) / (6 * scale));
    const char* scan = value;
    int length = 0;
    while (*scan) { display_character(scan); ++length; }
    bool clipped = length > slots;
    int visible = clipped ? std::max(0, slots - 3) : length;
    for (int n = 0; n < std::min(length, slots); ++n) {
        char c = clipped && n >= visible ? '.' : display_character(value);
        const uint8_t* columns = glyph(c);
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (columns[col] & (1 << row)) canvas.rect(x + col * scale, y + row * scale, scale, scale, color);
        x += 6 * scale;
    }
}

void centered_text(Canvas& canvas, int y, const char* value, uint16_t color, int scale = 1) {
    const char* scan = value;
    int length = 0;
    while (*scan) { display_character(scan); ++length; }
    int slots = (canvas.frame.width-24+scale)/(6*scale);
    int width = std::min(length,slots)*6*scale-scale;
    text(canvas, (canvas.frame.width - width)/2, y, value, (canvas.frame.width + width)/2, color, scale);
}

int surface_noise(int u, int v, unsigned shift, uint32_t seed) {
    unsigned mask = (512U >> shift) - 1, cell = 1U << shift;
    unsigned x = unsigned(u) >> shift, y = unsigned(v) >> shift;
    int fx = (unsigned(u) & (cell-1)) * 256 / cell, fy = (unsigned(v) & (cell-1)) * 256 / cell;
    fx = fx*fx*(768-2*fx)/65536; fy = fy*fy*(768-2*fy)/65536;
    auto sample = [&](unsigned a, unsigned b) { return int(noise((a & mask) + (b & mask)*71 + seed) & 255); };
    int a = sample(x,y), b = sample(x+1,y), c = sample(x,y+1), d = sample(x+1,y+1);
    return (a*256 + (b-a)*fx + (c-a)*fy + (a-b-c+d)*fx*fy/256)/256;
}

void planet(Canvas& canvas, const Model& model, uint64_t now) {
    static constexpr Color palettes[][6] = {
        {{56,41,49},{103,66,57},{161,107,73},{191,136,91},{221,179,122},{238,211,162}},
        {{23,44,72},{36,83,116},{66,127,153},{104,136,103},{169,179,145},{224,226,207}},
        {{51,50,55},{85,83,82},{130,121,114},{164,155,136},{197,191,167},{228,220,199}},
        {{36,61,88},{68,113,139},{113,155,175},{165,195,204},{208,225,220},{237,240,223}},
        {{45,37,44},{73,52,54},{115,80,68},{174,105,69},{226,155,80},{242,203,112}},
        {{45,40,63},{81,65,97},{125,96,141},{167,126,171},{198,163,195},{232,207,215}}
    };
    int cell = canvas.frame.width >= 200 ? 2 : 1;
    int radius = std::min(canvas.frame.width*11/40, canvas.frame.height/3) / cell;
    int cx = canvas.frame.width/2, cy = canvas.frame.height/2-4;
    if (cy + radius*cell + cell <= canvas.top() || cy-radius*cell >= canvas.bottom()) return;
    uint32_t seed = model.planet_seed, family = seed % 6;
    int bias = int(noise(seed+31)%15)-7;
    int rotation = int((now/140) % 512);
    struct Crater { int u, v, r; };
    Crater craters[8];
    for (unsigned n = 0; n < 8; ++n) {
        uint32_t hash = noise(seed + n*117);
        craters[n] = {int(hash & 255)+128, int((hash>>8) & 255)+128, 10+int((hash>>16)%22)};
    }
    // Include the cell crossing a band edge, then let rect clip its rows.
    int origin_y = cy-radius*cell;
    int first = std::max(0,(canvas.top()-origin_y)/cell)-radius;
    int last = std::min(radius*2,(canvas.bottom()-1-origin_y)/cell)-radius;
    for (int y = first; y <= last; ++y) for (int x = -radius; x <= radius; ++x) {
        int distance = x*x + y*y;
        if (distance > radius*radius) continue;
        int z = int(root(unsigned(radius*radius-distance)*256));
        int u = (x*4096/(z+radius*16) + rotation + 256) & 511;
        int v = (y*4096/(z+radius*16) + 256) & 511;
        int broad = surface_noise(u,v,7,seed), medium = surface_noise(u,v,5,seed), fine = surface_noise(u,v,3,seed);
        int terrain = (3*broad+2*medium+fine)/6;
        int index = 3, relief = 256, emission = 0;
        switch (family) {
        case 0: case 5: {
            int band = ((v*(family == 0 ? 3 : 2)+terrain/2) & 127);
            band = std::min(band,127-band);
            index = std::clamp(1+band/14,1,5);
            int dx = u-300, dy = v-225;
            if (dx*dx/3 + dy*dy*2 < 600) index = (dx*dx/3+dy*dy*2)/160+1;
            break;
        }
        case 1:
            index = terrain > 138 ? (fine > 125 ? 4 : 3) : terrain > 110 ? 2 : 1;
            if ((medium*2+fine)/3 > 170 || std::abs(y)*10 > radius*9) index = 5;
            break;
        case 2:
            index = fine > 130 ? 3 : 4;
            for (const auto& crater : craters) {
                int dx = ((u-crater.u+768)&511)-256, dy = v-crater.v;
                int d = dx*dx+dy*dy, r2 = crater.r*crater.r;
                if (d < r2) relief = relief*(160+70*d/r2)/256;
                else if (d < r2*5/4) relief = relief*290/256;
            }
            break;
        case 3:
            index = terrain > 132 ? 4 : 3;
            if (std::abs((medium+fine)/2-128) < 8) index = 1;
            if (std::abs(y)*10 > radius*9) index = 5;
            break;
        default: {
            int fissure = std::abs(terrain-128);
            index = fine > 128 ? 1 : 0;
            if (fissure < 12) { index = fissure < 5 ? 5 : 4; emission = 190; }
            else if (fissure < 20) index = 2;
            break;
        }
        }
        int light = std::clamp((-x*148-y*82+z*194/16)/radius,0,256);
        int shade = std::max(emission,(69+light*200/256)*relief/256);
        Color color = palettes[family][index];
        color.r += bias; color.g += bias; color.b += bias;
        int dither = int(bayer[((y+radius)&3)*4+((x+radius)&3)])-8;
        canvas.rect(cx+x*cell,cy+y*cell,cell,cell,color565(color,shade,dither/2));
    }
}

void record(Canvas& canvas, int cx, int cy, int radius, Color tint, uint64_t now) {
    int cell = canvas.frame.width >= 200 ? 2 : 1;
    radius /= cell;
    if (cy+radius*cell+cell <= canvas.top() || cy-radius*cell >= canvas.bottom()) return;
    static constexpr int directions[][2] = {{256,0},{237,98},{181,181},{98,237},{0,256},{-98,237},{-181,181},{-237,98},
        {-256,0},{-237,-98},{-181,-181},{-98,-237},{0,-256},{98,-237},{181,-181},{237,-98}};
    unsigned phase = unsigned(now%28000), sector = phase/1750, fraction = phase%1750;
    int dx = directions[sector][0] + (directions[(sector+1)%16][0]-directions[sector][0])*int(fraction)/1750;
    int dy = directions[sector][1] + (directions[(sector+1)%16][1]-directions[sector][1])*int(fraction)/1750;
    int origin_y = cy-radius*cell;
    int first = std::max(0,(canvas.top()-origin_y)/cell)-radius;
    int last = std::min(radius*2,(canvas.bottom()-1-origin_y)/cell)-radius;
    for (int y = first; y <= last; ++y) for (int x = -radius; x <= radius; ++x) {
        int distance = x*x+y*y;
        if (distance > radius*radius) continue;
        int radial = int(root(unsigned(distance)*256));
        Color color;
        if (radial < radius*6) {
            int stripe = std::abs((x*dx+y*dy)*45/100+(-x*dy+y*dx))/256;
            int shade = stripe%6 < 1 ? 185 : 246;
            int grain = int(noise(unsigned(x+radius)+unsigned(y+radius)*71)&15)-7;
            color = {tint.r*shade/256+grain,tint.g*shade/256+grain,tint.b*shade/256+grain};
            if (radial < radius*13/10) color = {217,203,174};
            if (radial < radius*4/5) color = {10,16,17};
        } else {
            int sheen = std::min(256,std::abs(x*dx+y*dy)*16/std::max(1,radial));
            for (int n = 0; n < 4; ++n) sheen = sheen*sheen/256;
            int level = 20+(radial*14/160%2)*4+sheen*16/256;
            color = {level*91/100,level,level*98/100};
            if (radial > radius*154/10) color = {73,79,71};
            else if (radial < radius*66/10) color = {48,53,47};
        }
        canvas.rect(cx+x*cell,cy+y*cell,cell,cell,color565(color));
    }
}

void cover(Canvas& canvas, int x, int y, int side, const Artwork* art) {
    canvas.rect(x, y, side, side, 0x18e6);
    if (!art || !art->pixels || !art->width || !art->height || art->width > max_art_side ||
        art->height > max_art_side || art->stride != art->width * 2) {
        canvas.circle(x + side/2, y + side/2, side/4, dim);
        canvas.circle(x + side/2, y + side/2, side/12, background);
        return;
    }
    int width = side, height = side;
    if (art->width > art->height) height = std::max(1, side * art->height / art->width);
    else width = std::max(1, side * art->width / art->height);
    int left = x + (side - width)/2, top = y + (side - height)/2;
    for (int py = std::max(0, canvas.top() - top); py < std::min(height, canvas.bottom() - top); ++py) {
        for (int px = 0; px < width; ++px) {
            size_t index = static_cast<size_t>(py * art->height / height) * art->stride +
                           static_cast<size_t>(px * art->width / width) * 2;
            canvas.pixel(left + px, top + py, static_cast<uint16_t>(art->pixels[index] | (uint16_t(art->pixels[index + 1]) << 8)));
        }
    }
}

void controls(Canvas& canvas, const Model& model, uint64_t now, uint16_t accent) {
    int scale = canvas.frame.width >= 200 ? 2 : 1;
    static constexpr uint16_t icons[][9] = {
        {64,128,455,168,80,168,455,128,64}, {0,129,193,225,249,225,193,129,0},
        {0,198,198,198,198,198,198,198,0}, {0,258,262,270,318,270,262,258,0},
        {32,64,254,66,170,132,254,4,8}
    };
    static constexpr uint16_t play[9] = {0,4,12,28,60,28,12,4,0};
    for (unsigned n = 0; n < 5; ++n) {
        Intent intent;
        bool enabled = model.intent(static_cast<Button>(n), now, intent);
        bool active = (n == 0 && model.snapshot.shuffle) || (n == 4 && model.snapshot.repeat != Repeat::off);
        uint16_t color = enabled ? (active ? accent : ink) : dim;
        int x = (int(n) * 2 + 1) * canvas.frame.width / 10 - 4 * scale;
        int y = canvas.frame.height - 18 * scale;
        const uint16_t* rows = n == 2 && model.effective_state(now) != Playback::playing ? play : icons[n];
        if (n == 2) {
            canvas.circle(x + 4 * scale, y + 4 * scale, 9 * scale - 1, enabled ? accent : dim);
            color = background;
        }
        for (int row = 0; row < 9; ++row)
            for (int col = 0; col < 9; ++col)
                if (rows[row] & (1 << col)) canvas.rect(x + col * scale, y + row * scale, scale, scale, color);
        if (n == 4 && model.snapshot.repeat == Repeat::track) canvas.rect(x + 3 * scale, y + 4 * scale, scale, scale, color);
    }
}
} // namespace

bool render(const Model& model, uint64_t now_ms, Frame frame, const Artwork* artwork) {
    if (!frame.valid() || frame.width < 128 || frame.height < 128 || frame.width > max_frame_width ||
        frame.height > max_frame_height) return false;
    Canvas canvas{frame};
    int scale = frame.width >= 200 ? 2 : 1;
    canvas.rect(0, 0, frame.width, frame.height, background);
    Playback state = model.effective_state(now_ms);
    if (state == Playback::playing) {
        const auto& snapshot = model.snapshot;
        const Artwork* active_art = snapshot.artwork_url[0] ? artwork : nullptr;
        Color tint = artwork_color(active_art);
        uint16_t accent = color565(tint);
        bool playlist = snapshot.context_type == Context::playlist && snapshot.context_name[0];
        text(canvas, 7 * scale, 6 * scale, playlist ? snapshot.context_name : snapshot.album, frame.width - 7 * scale, accent);
        if (playlist) text(canvas, 12 * scale, 12 * scale, snapshot.album, frame.width - 7 * scale, dim);
        int side = std::min(frame.width * 128/240, frame.height * 128/296);
        if (side <= 0) return false;
        int left = frame.width*22/240, top = frame.height*50/296;
        record(canvas,left+side*35/32,top+side/2,side*7/16,tint,now_ms);
        cover(canvas,left,top,side,active_art);
        centered_text(canvas,frame.height*194/296,snapshot.title,ink,scale);
        centered_text(canvas,frame.height*216/296,snapshot.artist,dim);
        int progress_y = frame.height*234/296, progress_width = frame.width-20*scale;
        canvas.rect(10*scale,progress_y,progress_width,1,dim);
        if (snapshot.duration_ms) {
            int width = static_cast<int>(model.progress(now_ms)*progress_width/snapshot.duration_ms);
            canvas.rect(10*scale,progress_y,width,1,accent);
            canvas.rect(10*scale+width-1,progress_y-1,2,3,accent);
        }
        controls(canvas,model,now_ms,accent);
    } else {
        canvas.rect(0,0,frame.width,frame.height,color565({10,16,17}));
        static constexpr int stars[][2] = {{28,98},{182,90},{206,186},{40,210},{144,78},{192,226},{74,84},{16,166},{206,132},{64,226}};
        for (unsigned n = 0; n < 10; ++n) {
            int x = stars[n][0]*frame.width/240, y = stars[n][1]*frame.height/296;
            canvas.pixel(x,y,0x42ca);
            if (n == 1) { canvas.rect(x-1,y,3,1,0x632c); canvas.rect(x,y-1,1,3,0x632c); }
        }
        planet(canvas, model, now_ms);
        constexpr uint16_t warm = 0xc570;
        canvas.rect(7*scale,6*scale+2,2,2,warm);
        text(canvas,11*scale,6*scale,state == Playback::paused ? "paused" : state == Playback::idle ? "in orbit" : "offline",frame.width-7*scale,dim);
        static constexpr const char* names[] = {"amber giant","ocean world","crater moon","frozen world","ember world","violet giant"};
        centered_text(canvas,frame.height*246/296,names[model.planet_seed%6],warm);
        centered_text(canvas,frame.height*266/296,"auto 30s",dim);
    }
    return true;
}

uint32_t frame_hash(const Frame& frame) {
    if (!frame.valid()) return 0;
    uint32_t hash = 2166136261U;
    for (size_t y = 0; y < frame.rows(); ++y) {
        for (size_t x = 0; x < frame.width; ++x) {
            uint16_t pixel = frame.pixels[y * frame.stride + x];
            hash = (hash ^ (pixel & 255)) * 16777619U;
            hash = (hash ^ (pixel >> 8)) * 16777619U;
        }
    }
    return hash;
}
} // namespace bottie
