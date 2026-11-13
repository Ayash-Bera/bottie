#include "bottie/core.hpp"
#include "bottie/text_mask.hpp"

#include <cstring>
#include <limits>

namespace bottie {
namespace {
enum class Kind { object, array, string, number, boolean, null_value };
struct Token {
    Kind kind{};
    size_t start{}, length{}, next{};
};

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool decode(const char* text, size_t length, char* output, size_t capacity) {
    size_t used = 0;
    auto emit = [&](uint8_t byte) {
        if (byte == 0 || used + 1 >= capacity) return false;
        if (output) output[used] = static_cast<char>(byte);
        ++used;
        return true;
    };
    auto unicode = [&](size_t& pos, uint32_t& value) {
        value = 0;
        if (length - pos < 4) return false;
        for (unsigned n = 0; n < 4; ++n) {
            int digit = hex(text[pos++]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return true;
    };
    for (size_t pos = 0; pos < length;) {
        uint8_t c = static_cast<uint8_t>(text[pos++]);
        if (c == '\\') {
            if (pos == length) return false;
            c = static_cast<uint8_t>(text[pos++]);
            switch (c) {
            case '"': case '\\': case '/': if (!emit(c)) return false; break;
            case 'b': if (!emit('\b')) return false; break;
            case 'f': if (!emit('\f')) return false; break;
            case 'n': if (!emit('\n')) return false; break;
            case 'r': if (!emit('\r')) return false; break;
            case 't': if (!emit('\t')) return false; break;
            case 'u': {
                uint32_t value;
                if (!unicode(pos, value)) return false;
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (length - pos < 6 || text[pos] != '\\' || text[pos + 1] != 'u') return false;
                    pos += 2;
                    uint32_t low;
                    if (!unicode(pos, low) || low < 0xdc00 || low > 0xdfff) return false;
                    value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                } else if (value >= 0xdc00 && value <= 0xdfff) return false;
                if (value < 0x80) { if (!emit(static_cast<uint8_t>(value))) return false; }
                else if (value < 0x800) {
                    if (!emit(static_cast<uint8_t>(0xc0 | (value >> 6))) ||
                        !emit(static_cast<uint8_t>(0x80 | (value & 63)))) return false;
                } else if (value < 0x10000) {
                    if (!emit(static_cast<uint8_t>(0xe0 | (value >> 12))) ||
                        !emit(static_cast<uint8_t>(0x80 | ((value >> 6) & 63))) ||
                        !emit(static_cast<uint8_t>(0x80 | (value & 63)))) return false;
                } else {
                    if (!emit(static_cast<uint8_t>(0xf0 | (value >> 18))) ||
                        !emit(static_cast<uint8_t>(0x80 | ((value >> 12) & 63))) ||
                        !emit(static_cast<uint8_t>(0x80 | ((value >> 6) & 63))) ||
                        !emit(static_cast<uint8_t>(0x80 | (value & 63)))) return false;
                }
                break;
            }
            default: return false;
            }
        } else if (c < 0x80) {
            if (c < 0x20 || c == '"' || !emit(c)) return false;
        } else {
            unsigned extra;
            uint32_t value, minimum;
            if (c >= 0xc2 && c <= 0xdf) { extra = 1; value = c & 31; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { extra = 2; value = c & 15; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { extra = 3; value = c & 7; minimum = 0x10000; }
            else return false;
            if (length - pos < extra || !emit(c)) return false;
            for (unsigned n = 0; n < extra; ++n) {
                uint8_t next = static_cast<uint8_t>(text[pos++]);
                if ((next & 0xc0) != 0x80 || !emit(next)) return false;
                value = (value << 6) | (next & 63);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        }
    }
    if (output) output[used] = 0;
    return true;
}

struct Parser {
    const char* data;
    size_t length, pos{}, count{};
    std::array<Token, 256> tokens{};
    Error error{Error::syntax};

    void space() {
        while (pos < length && (data[pos] == ' ' || data[pos] == '\r' || data[pos] == '\n' || data[pos] == '\t')) ++pos;
    }
    bool take(char c) {
        if (pos < length && data[pos] == c) { ++pos; return true; }
        return false;
    }
    bool string(Token& token) {
        if (!take('"')) return false;
        token.kind = Kind::string;
        token.start = pos;
        bool escaped = false;
        while (pos < length) {
            char c = data[pos++];
            if (!escaped && c == '"') {
                token.length = pos - token.start - 1;
                return decode(data + token.start, token.length, nullptr, max_json_bytes + 1);
            }
            if (!escaped && c == '\\') escaped = true;
            else escaped = false;
        }
        return false;
    }
    bool value(unsigned depth) {
        space();
        if (pos == length) return false;
        if (depth > 8) { error = Error::depth; return false; }
        if (count == tokens.size()) { error = Error::tokens; return false; }
        size_t index = count++;
        Token& token = tokens[index];
        token.start = pos;
        char c = data[pos];
        if (c == '{' || c == '[') {
            bool object = c == '{';
            token.kind = object ? Kind::object : Kind::array;
            ++pos;
            space();
            char close = object ? '}' : ']';
            if (!take(close)) {
                do {
                    if (object) {
                        if (count == tokens.size()) { error = Error::tokens; return false; }
                        size_t key = count++;
                        if (!string(tokens[key])) return false;
                        tokens[key].next = count;
                        char key_text[65];
                        if (!copy(key, key_text, sizeof key_text)) return false;
                        for (size_t old = index + 1; old < key; old = tokens[old + 1].next) {
                            char previous[65];
                            if (!copy(old, previous, sizeof previous) || std::strcmp(previous, key_text) == 0) return false;
                        }
                        space();
                        if (!take(':')) return false;
                    }
                    if (!value(depth + 1)) return false;
                    space();
                    if (take(close)) break;
                    if (!take(',')) return false;
                    space();
                } while (true);
            }
        } else if (c == '"') {
            if (!string(token)) return false;
        } else if (c == 't' || c == 'f' || c == 'n') {
            const char* literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
            size_t n = std::strlen(literal);
            if (length - pos < n || std::memcmp(data + pos, literal, n) != 0) return false;
            pos += n;
            token.kind = c == 'n' ? Kind::null_value : Kind::boolean;
            token.length = n;
        } else {
            token.kind = Kind::number;
            take('-');
            if (!take('0')) {
                if (pos == length || data[pos] < '1' || data[pos] > '9') return false;
                while (pos < length && data[pos] >= '0' && data[pos] <= '9') ++pos;
            }
            if (take('.')) {
                size_t begin = pos;
                while (pos < length && data[pos] >= '0' && data[pos] <= '9') ++pos;
                if (pos == begin) return false;
            }
            if (take('e') || take('E')) {
                if (!take('+')) take('-');
                size_t begin = pos;
                while (pos < length && data[pos] >= '0' && data[pos] <= '9') ++pos;
                if (pos == begin) return false;
            }
            token.length = pos - token.start;
        }
        token.next = count;
        return true;
    }
    size_t find(size_t object, const char* key) const {
        if (object >= count || tokens[object].kind != Kind::object) return count;
        for (size_t i = object + 1; i < tokens[object].next; i = tokens[i + 1].next) {
            char name[65];
            if (copy(i, name, sizeof name) && std::strcmp(name, key) == 0) return i + 1;
        }
        return count;
    }
    bool copy(size_t index, char* output, size_t capacity) const {
        if (index >= count || tokens[index].kind != Kind::string) return false;
        const Token& token = tokens[index];
        return decode(data + token.start, token.length, output, capacity);
    }
    bool number(size_t index, uint64_t& result) const {
        if (index >= count || tokens[index].kind != Kind::number) return false;
        const Token& token = tokens[index];
        uint64_t value = 0;
        for (size_t i = 0; i < token.length; ++i) {
            char c = data[token.start + i];
            if (c < '0' || c > '9') return false;
            unsigned digit = static_cast<unsigned>(c - '0');
            if (value > (max_json_integer - digit) / 10) return false;
            value = value * 10 + digit;
        }
        result = value;
        return true;
    }
    bool boolean(size_t index, bool& result) const {
        if (index >= count || tokens[index].kind != Kind::boolean) return false;
        result = data[tokens[index].start] == 't';
        return true;
    }
    bool offset(size_t index, int16_t& result) const {
        if (index >= count || tokens[index].kind != Kind::number) return false;
        const auto& token = tokens[index];
        size_t i = 0;
        const bool negative = data[token.start] == '-';
        if (negative) ++i;
        unsigned value = 0;
        for (; i < token.length; ++i) {
            const char c = data[token.start + i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + unsigned(c - '0');
            if (value > 840) return false;
        }
        result = static_cast<int16_t>(negative ? -int(value) : int(value));
        return true;
    }
};

template <typename T, size_t N>
bool choice(const Parser& parser, size_t index, const char* const (&names)[N], T& result) {
    char value[24];
    if (!parser.copy(index, value, sizeof value)) return false;
    for (size_t n = 0; n < N; ++n) {
        if (std::strcmp(value, names[n]) == 0) { result = static_cast<T>(n); return true; }
    }
    return false;
}
} // namespace

Error parse_snapshot(const char* json, size_t bytes, Snapshot& output) {
    if (!json || bytes == 0 || bytes > max_json_bytes) return Error::size;
    Parser parser{json, bytes};
    if (!parser.value(0)) return parser.error;
    parser.space();
    if (parser.pos != bytes || parser.tokens[0].kind != Kind::object) return Error::syntax;
    Snapshot candidate;
    auto field = [&](const char* name) { return parser.find(0, name); };
    uint64_t version;
    if (!parser.number(field("schema_version"), version)) return Error::field;
    if (version != 1) return Error::version;
    const char* const states[] = {"playing", "paused", "idle", "unavailable"};
    const char* const repeats[] = {"off", "context", "track"};
    const char* const contexts[] = {"unknown", "playlist", "album"};
    if (!choice(parser, field("state"), states, candidate.state) ||
        !choice(parser, field("repeat"), repeats, candidate.repeat) ||
        !parser.copy(field("track_id"), candidate.track_id, sizeof candidate.track_id) ||
        !parser.copy(field("title"), candidate.title, sizeof candidate.title) ||
        !parser.copy(field("artist"), candidate.artist, sizeof candidate.artist) ||
        !parser.copy(field("album"), candidate.album, sizeof candidate.album) ||
        !parser.boolean(field("shuffle"), candidate.shuffle)) return Error::field;
    if (!parser.number(field("progress_ms"), candidate.progress_ms) ||
        !parser.number(field("duration_ms"), candidate.duration_ms) ||
        !parser.number(field("updated_at_ms"), candidate.updated_at_ms) ||
        candidate.progress_ms > candidate.duration_ms || candidate.duration_ms > 86400000) return Error::range;
    size_t context = field("context");
    if (context != parser.count && parser.tokens[context].kind != Kind::null_value) {
        if (!choice(parser, parser.find(context, "type"), contexts, candidate.context_type) ||
            !parser.copy(parser.find(context, "name"), candidate.context_name, sizeof candidate.context_name)) return Error::field;
    }
    size_t artwork = field("artwork");
    if (artwork != parser.count && parser.tokens[artwork].kind != Kind::null_value) {
        uint64_t width, height;
        if (!parser.copy(parser.find(artwork, "url"), candidate.artwork_url, sizeof candidate.artwork_url) ||
            !parser.number(parser.find(artwork, "width"), width) ||
            !parser.number(parser.find(artwork, "height"), height) ||
            width > max_art_side || height > max_art_side || ((width == 0) != (height == 0)) ||
            (candidate.artwork_url[0] && (width < 16 || height < 16))) return Error::range;
        candidate.artwork_width = static_cast<uint16_t>(width);
        candidate.artwork_height = static_cast<uint16_t>(height);
    }
    const size_t mask = field("text_mask");
    if (mask != parser.count && parser.tokens[mask].kind == Kind::object) {
        uint64_t layout = 0;
        char path[74]{};
        if (parser.number(parser.find(mask, "layout"), layout) && layout == 1 &&
            parser.copy(parser.find(mask, "url"), path, sizeof path) && text_mask_path(path)) {
            std::strcpy(candidate.text_mask_url, path);
            candidate.text_mask_layout = 1;
        }
    }
    const size_t clock = field("clock");
    if (clock != parser.count && parser.tokens[clock].kind == Kind::object) {
        Clock value;
        if (parser.number(parser.find(clock, "unix_ms"), value.unix_ms) &&
            parser.offset(parser.find(clock, "utc_offset_min"), value.utc_offset_min) && value.valid())
            candidate.clock = value;
    }
    size_t caps = field("capabilities");
    auto& cap = candidate.capabilities;
    if (!parser.boolean(parser.find(caps, "play"), cap.play) ||
        !parser.boolean(parser.find(caps, "pause"), cap.pause) ||
        !parser.boolean(parser.find(caps, "previous"), cap.previous) ||
        !parser.boolean(parser.find(caps, "next"), cap.next) ||
        !parser.boolean(parser.find(caps, "shuffle"), cap.shuffle) ||
        !parser.boolean(parser.find(caps, "repeat"), cap.repeat)) return Error::field;
    const size_t volume_cap = parser.find(caps, "volume");
    if (volume_cap != parser.count && !parser.boolean(volume_cap, cap.volume)) return Error::field;
    const size_t volume = field("volume_percent");
    if (volume != parser.count && parser.tokens[volume].kind != Kind::null_value) {
        uint64_t percent;
        if (!parser.number(volume, percent) || percent > 100) return Error::range;
        candidate.volume_percent = static_cast<int16_t>(percent);
    }
    const size_t target = field("control_target");
    candidate.target_known = target != parser.count;
    if (candidate.target_known) {
        if (!parser.copy(target, candidate.control_target, sizeof candidate.control_target)) return Error::field;
        const size_t length = std::strlen(candidate.control_target);
        if (length != 0 && length != 32) return Error::field;
        for (size_t n = 0; n < length; ++n) {
            const char c = candidate.control_target[n];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return Error::field;
        }
    }
    output = candidate;
    return Error::none;
}

const char* error_name(Error error) {
    switch (error) {
    case Error::none: return "none";
    case Error::size: return "size";
    case Error::syntax: return "syntax";
    case Error::depth: return "depth";
    case Error::tokens: return "tokens";
    case Error::field: return "field";
    case Error::range: return "range";
    case Error::version: return "version";
    case Error::stale: return "stale";
    }
    return "unknown";
}

const char* playback_name(Playback state) {
    switch (state) {
    case Playback::playing: return "playing";
    case Playback::paused: return "paused";
    case Playback::idle: return "idle";
    case Playback::unavailable: return "unavailable";
    }
    return "unavailable";
}
} // namespace bottie
