// Steam shortcut files, account list parsing, and library artwork. No I/O here.
#include "hub_steam_vdf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace hub::steam {

using platform::Image;

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

constexpr uint8_t kEndOfMap = 0x08;
constexpr int kMaxDepth = 32;

struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t pos = 0;

    bool u8(uint8_t& v) {
        if (pos >= bytes.size()) return false;
        v = bytes[pos++];
        return true;
    }
    bool cstr(std::string& s) {
        size_t end = pos;
        while (end < bytes.size() && bytes[end] != 0) ++end;
        if (end >= bytes.size()) return false;
        s.assign(reinterpret_cast<const char*>(bytes.data()) + pos, end - pos);
        pos = end + 1;
        return true;
    }
    bool u32(uint32_t& v) {
        if (pos + 4 > bytes.size()) return false;
        v = static_cast<uint32_t>(bytes[pos]) | (static_cast<uint32_t>(bytes[pos + 1]) << 8) |
            (static_cast<uint32_t>(bytes[pos + 2]) << 16) | (static_cast<uint32_t>(bytes[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool u64(uint64_t& v) {
        uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) return false;
        v = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }
};

bool parse_children(Reader& r, std::vector<VdfNode>& out, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "nesting is too deep";
        return false;
    }
    while (true) {
        uint8_t type = 0;
        if (!r.u8(type)) {
            error = "unexpected end of file";
            return false;
        }
        if (type == kEndOfMap) return true;
        VdfNode node;
        node.type = type;
        if (!r.cstr(node.key)) {
            error = "truncated key";
            return false;
        }
        bool ok = true;
        switch (type) {
            case VdfNode::Map:
                if (!parse_children(r, node.children, depth + 1, error)) return false;
                break;
            case VdfNode::String:
                ok = r.cstr(node.str);
                break;
            case VdfNode::Int32:
            case VdfNode::Float32:
            case VdfNode::Pointer:
            case VdfNode::Color:
                ok = r.u32(node.i32);
                break;
            case VdfNode::UInt64:
            case VdfNode::Int64:
                ok = r.u64(node.u64);
                break;
            default:
                error = "unsupported value type " + std::to_string(type) + " for key \"" + node.key + "\"";
                return false;
        }
        if (!ok) {
            error = "truncated value for key \"" + node.key + "\"";
            return false;
        }
        out.push_back(std::move(node));
    }
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void write_children(const std::vector<VdfNode>& nodes, std::vector<uint8_t>& out) {
    for (const VdfNode& n : nodes) {
        out.push_back(n.type);
        out.insert(out.end(), n.key.begin(), n.key.end());
        out.push_back(0);
        switch (n.type) {
            case VdfNode::Map:
                write_children(n.children, out);
                break;
            case VdfNode::String:
                out.insert(out.end(), n.str.begin(), n.str.end());
                out.push_back(0);
                break;
            case VdfNode::UInt64:
            case VdfNode::Int64:
                put_u32(out, static_cast<uint32_t>(n.u64));
                put_u32(out, static_cast<uint32_t>(n.u64 >> 32));
                break;
            default:
                put_u32(out, n.i32);
                break;
        }
    }
    out.push_back(kEndOfMap);
}

// Splits LaunchOptions the way a command line is split: spaces separate
// arguments except inside double quotes.
std::vector<std::string> split_command_line(const std::string& s) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool have = false;
    for (char c : s) {
        if (c == '"') {
            quoted = !quoted;
            have = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (have) tokens.push_back(current);
            current.clear();
            have = false;
            continue;
        }
        current.push_back(c);
        have = true;
    }
    if (have) tokens.push_back(current);
    return tokens;
}

uint32_t app_id_of(const VdfNode& shortcut) {
    const VdfNode* n = shortcut.find("appid");
    return n && n->type == VdfNode::Int32 ? n->i32 : 0;
}

void renumber(VdfNode& map) {
    for (size_t i = 0; i < map.children.size(); ++i) map.children[i].key = std::to_string(i);
}

void merge_tags(VdfNode& shortcut, const std::vector<std::string>& tags) {
    VdfNode& list = shortcut.child_map("tags");
    for (const std::string& tag : tags) {
        bool present = false;
        for (const VdfNode& c : list.children) {
            if (c.type == VdfNode::String && iequals(c.str, tag)) present = true;
        }
        if (!present) {
            VdfNode n;
            n.type = VdfNode::String;
            n.str = tag;
            list.children.push_back(std::move(n));
        }
    }
    renumber(list);
}

}  // namespace

VdfNode* VdfNode::find(const std::string& k) {
    for (VdfNode& c : children) {
        if (iequals(c.key, k)) return &c;
    }
    return nullptr;
}

const VdfNode* VdfNode::find(const std::string& k) const {
    for (const VdfNode& c : children) {
        if (iequals(c.key, k)) return &c;
    }
    return nullptr;
}

VdfNode& VdfNode::set_string(const std::string& k, const std::string& v) {
    VdfNode* n = find(k);
    if (!n) {
        children.emplace_back();
        n = &children.back();
        n->key = k;
    }
    n->type = String;
    n->str = v;
    n->children.clear();
    return *n;
}

VdfNode& VdfNode::set_int(const std::string& k, uint32_t v) {
    VdfNode* n = find(k);
    if (!n) {
        children.emplace_back();
        n = &children.back();
        n->key = k;
    }
    n->type = Int32;
    n->i32 = v;
    n->str.clear();
    n->children.clear();
    return *n;
}

VdfNode& VdfNode::child_map(const std::string& k) {
    VdfNode* n = find(k);
    if (!n) {
        children.emplace_back();
        n = &children.back();
        n->key = k;
    }
    if (n->type != Map) {
        n->type = Map;
        n->str.clear();
    }
    return *n;
}

std::string VdfNode::get_string(const std::string& k) const {
    const VdfNode* n = find(k);
    return n && n->type == String ? n->str : "";
}

bool parse_binary_vdf(const std::vector<uint8_t>& bytes, VdfNode& root, std::string& error) {
    root = VdfNode{};
    if (bytes.empty()) return true;
    Reader r{bytes};
    if (!parse_children(r, root.children, 0, error)) return false;
    if (r.pos != bytes.size()) {
        error = "unexpected data after the end of the file";
        return false;
    }
    return true;
}

std::vector<uint8_t> write_binary_vdf(const VdfNode& root) {
    std::vector<uint8_t> out;
    write_children(root.children, out);
    return out;
}

uint32_t crc32(const std::string& data) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        c ^= ch;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t shortcut_app_id(const std::string& exe_field, const std::string& app_name) {
    return crc32(exe_field + app_name) | 0x80000000u;
}

std::string quote_path(const std::string& path) {
    return "\"" + path + "\"";
}

std::string hub_game_id_of(const VdfNode& shortcut, const std::string& hub_exe_file_name) {
    std::string exe = shortcut.get_string("Exe");
    exe.erase(std::remove(exe.begin(), exe.end(), '"'), exe.end());
    const size_t slash = exe.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? exe : exe.substr(slash + 1);
    if (!iequals(name, hub_exe_file_name)) return "";
    const std::vector<std::string> tokens = split_command_line(shortcut.get_string("LaunchOptions"));
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "--launch") return tokens[i + 1];
    }
    return "";
}

VdfNode* find_hub_shortcut(VdfNode& root, const std::string& game_id, const std::string& hub_exe_file_name) {
    VdfNode* list = root.find("shortcuts");
    if (!list || list->type != VdfNode::Map) return nullptr;
    for (VdfNode& shortcut : list->children) {
        if (shortcut.type == VdfNode::Map && hub_game_id_of(shortcut, hub_exe_file_name) == game_id) return &shortcut;
    }
    return nullptr;
}

UpsertOutcome upsert_shortcuts(VdfNode& root, const std::vector<ShortcutSpec>& specs, const std::string& hub_exe_file_name) {
    UpsertOutcome outcome;
    VdfNode& list = root.child_map("shortcuts");
    std::set<uint32_t> used;
    for (const VdfNode& shortcut : list.children) {
        if (uint32_t id = app_id_of(shortcut)) used.insert(id);
    }
    for (const ShortcutSpec& spec : specs) {
        const std::string exe_field = quote_path(spec.exe);
        if (VdfNode* existing = find_hub_shortcut(root, spec.game_id, hub_exe_file_name)) {
            existing->set_string("AppName", spec.app_name);
            existing->set_string("Exe", exe_field);
            existing->set_string("StartDir", quote_path(spec.start_dir));
            existing->set_string("LaunchOptions", spec.launch_options);
            if (!spec.icon.empty()) existing->set_string("icon", spec.icon);
            uint32_t id = app_id_of(*existing);
            if (id == 0) {
                id = shortcut_app_id(exe_field, spec.app_name);
                for (int n = 1; used.count(id); ++n) id = shortcut_app_id(exe_field, spec.app_name + "#" + std::to_string(n));
                existing->set_int("appid", id);
                used.insert(id);
            }
            merge_tags(*existing, spec.tags);
            outcome.app_ids[spec.game_id] = id;
            ++outcome.updated;
            continue;
        }
        uint32_t id = shortcut_app_id(exe_field, spec.app_name);
        for (int n = 1; used.count(id); ++n) id = shortcut_app_id(exe_field, spec.app_name + "#" + std::to_string(n));
        used.insert(id);
        // Field order and defaults follow what the Steam client writes itself.
        VdfNode shortcut;
        shortcut.type = VdfNode::Map;
        shortcut.set_int("appid", id);
        shortcut.set_string("AppName", spec.app_name);
        shortcut.set_string("Exe", exe_field);
        shortcut.set_string("StartDir", quote_path(spec.start_dir));
        shortcut.set_string("icon", spec.icon);
        shortcut.set_string("ShortcutPath", "");
        shortcut.set_string("LaunchOptions", spec.launch_options);
        shortcut.set_int("IsHidden", 0);
        shortcut.set_int("AllowDesktopConfig", 1);
        shortcut.set_int("AllowOverlay", 1);
        shortcut.set_int("OpenVR", 0);
        shortcut.set_int("Devkit", 0);
        shortcut.set_string("DevkitGameID", "");
        shortcut.set_int("DevkitOverrideAppID", 0);
        shortcut.set_int("LastPlayTime", 0);
        shortcut.set_string("FlatpakAppID", "");
        merge_tags(shortcut, spec.tags);
        list.children.push_back(std::move(shortcut));
        outcome.app_ids[spec.game_id] = id;
        ++outcome.added;
    }
    renumber(list);
    return outcome;
}

std::vector<uint32_t> remove_shortcuts(VdfNode& root, const std::vector<std::string>& ids, const std::string& hub_exe_file_name) {
    std::vector<uint32_t> removed;
    VdfNode* list = root.find("shortcuts");
    if (!list || list->type != VdfNode::Map) return removed;
    std::vector<VdfNode> kept;
    for (VdfNode& shortcut : list->children) {
        const std::string id = shortcut.type == VdfNode::Map ? hub_game_id_of(shortcut, hub_exe_file_name) : "";
        const bool matches = !id.empty() && (ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end());
        if (matches) {
            removed.push_back(app_id_of(shortcut));
        } else {
            kept.push_back(std::move(shortcut));
        }
    }
    list->children = std::move(kept);
    renumber(*list);
    return removed;
}

// ---------------------------------------------------------------------------
// Text KeyValues
// ---------------------------------------------------------------------------

const TextVdf* TextVdf::find(const std::string& k) const {
    for (const TextVdf& c : children) {
        if (iequals(c.key, k)) return &c;
    }
    return nullptr;
}

namespace {

struct TextLexer {
    const std::string& s;
    size_t i = 0;

    void skip() {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else {
                break;
            }
        }
    }

    // Returns 0 at the end, '{' or '}' for braces, and '"' for a token in tok.
    char next(std::string& tok) {
        skip();
        if (i >= s.size()) return 0;
        const char c = s[i];
        if (c == '{' || c == '}') {
            ++i;
            return c;
        }
        tok.clear();
        if (c == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[i + 1];
                    tok.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    i += 2;
                    continue;
                }
                tok.push_back(s[i++]);
            }
            if (i >= s.size()) return 0;  // unterminated string
            ++i;
            return '"';
        }
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' && s[i] != '}' && s[i] != '"') {
            tok.push_back(s[i++]);
        }
        return '"';
    }
};

bool parse_text_map(TextLexer& lx, std::vector<TextVdf>& out, bool top, int depth, std::string& error) {
    if (depth > kMaxDepth) {
        error = "nesting is too deep";
        return false;
    }
    std::string key, value;
    while (true) {
        const char t = lx.next(key);
        if (t == 0) {
            if (top) return true;
            error = "unexpected end of file";
            return false;
        }
        if (t == '}') {
            if (!top) return true;
            error = "unbalanced }";
            return false;
        }
        if (t == '{') {
            error = "unexpected {";
            return false;
        }
        TextVdf node;
        node.key = key;
        const char v = lx.next(value);
        if (v == '{') {
            node.is_map = true;
            if (!parse_text_map(lx, node.children, false, depth + 1, error)) return false;
        } else if (v == '"') {
            node.value = value;
        } else {
            error = "missing value for \"" + key + "\"";
            return false;
        }
        out.push_back(std::move(node));
    }
}

}  // namespace

bool parse_text_vdf(const std::string& text, TextVdf& root, std::string& error) {
    root = TextVdf{};
    root.is_map = true;
    TextLexer lx{text};
    return parse_text_map(lx, root.children, true, 0, error);
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

namespace {

bool valid(const Image& im) {
    return im.width > 0 && im.height > 0 && im.rgba.size() >= static_cast<size_t>(im.width) * im.height * 4;
}

Image blank(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    Image im;
    im.width = w;
    im.height = h;
    im.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < im.rgba.size(); i += 4) {
        im.rgba[i] = r;
        im.rgba[i + 1] = g;
        im.rgba[i + 2] = b;
        im.rgba[i + 3] = 255;
    }
    return im;
}

Image crop(const Image& src, int x0, int y0, int w, int h) {
    Image out = blank(w, h, 0, 0, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* from = &src.rgba[(static_cast<size_t>(y0 + y) * src.width + x0) * 4];
        std::copy(from, from + static_cast<size_t>(w) * 4, &out.rgba[static_cast<size_t>(y) * w * 4]);
    }
    return out;
}

Image resize_nearest(const Image& src, int w, int h) {
    Image out = blank(w, h, 0, 0, 0);
    for (int y = 0; y < h; ++y) {
        const int sy = static_cast<int>(static_cast<long long>(y) * src.height / h);
        for (int x = 0; x < w; ++x) {
            const int sx = static_cast<int>(static_cast<long long>(x) * src.width / w);
            const uint8_t* p = &src.rgba[(static_cast<size_t>(sy) * src.width + sx) * 4];
            std::copy(p, p + 4, &out.rgba[(static_cast<size_t>(y) * w + x) * 4]);
        }
    }
    return out;
}

// Box filter: each output pixel averages the source pixels it covers.
Image resize_area(const Image& src, int w, int h) {
    Image out = blank(w, h, 0, 0, 0);
    for (int y = 0; y < h; ++y) {
        const int sy0 = static_cast<int>(static_cast<long long>(y) * src.height / h);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<long long>(y + 1) * src.height / h));
        for (int x = 0; x < w; ++x) {
            const int sx0 = static_cast<int>(static_cast<long long>(x) * src.width / w);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<long long>(x + 1) * src.width / w));
            uint64_t sum[4] = {0, 0, 0, 0};
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* p = &src.rgba[(static_cast<size_t>(sy) * src.width + sx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            const uint64_t n = static_cast<uint64_t>(sy1 - sy0) * (sx1 - sx0);
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    return out;
}

Image resize_bilinear(const Image& src, int w, int h) {
    Image out = blank(w, h, 0, 0, 0);
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) * src.height / h - 0.5, 0.0, static_cast<double>(src.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) * src.width / w - 0.5, 0.0, static_cast<double>(src.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const double tx = fx - x0;
            const uint8_t* a = &src.rgba[(static_cast<size_t>(y0) * src.width + x0) * 4];
            const uint8_t* b = &src.rgba[(static_cast<size_t>(y0) * src.width + x1) * 4];
            const uint8_t* c = &src.rgba[(static_cast<size_t>(y1) * src.width + x0) * 4];
            const uint8_t* d = &src.rgba[(static_cast<size_t>(y1) * src.width + x1) * 4];
            uint8_t* o = &out.rgba[(static_cast<size_t>(y) * w + x) * 4];
            for (int k = 0; k < 4; ++k) {
                const double top = a[k] + (b[k] - a[k]) * tx;
                const double bottom = c[k] + (d[k] - c[k]) * tx;
                o[k] = static_cast<uint8_t>(std::lround(top + (bottom - top) * ty));
            }
        }
    }
    return out;
}

void blit_over(Image& dst, const Image& src, int ox, int oy) {
    for (int y = 0; y < src.height; ++y) {
        const int dy = oy + y;
        if (dy < 0 || dy >= dst.height) continue;
        for (int x = 0; x < src.width; ++x) {
            const int dx = ox + x;
            if (dx < 0 || dx >= dst.width) continue;
            const uint8_t* s = &src.rgba[(static_cast<size_t>(y) * src.width + x) * 4];
            uint8_t* d = &dst.rgba[(static_cast<size_t>(dy) * dst.width + dx) * 4];
            const unsigned alpha = s[3];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((s[c] * alpha + d[c] * (255 - alpha)) / 255);
            d[3] = 255;
        }
    }
}

// Largest centred region of the source with the target's aspect ratio.
Image crop_to_aspect(const Image& src, int w, int h) {
    const double target = static_cast<double>(w) / h;
    const double aspect = static_cast<double>(src.width) / src.height;
    if (aspect > target) {
        const int cw = std::clamp(static_cast<int>(std::lround(src.height * target)), 1, src.width);
        return crop(src, (src.width - cw) / 2, 0, cw, src.height);
    }
    const int ch = std::clamp(static_cast<int>(std::lround(src.width / target)), 1, src.height);
    return crop(src, 0, (src.height - ch) / 2, src.width, ch);
}

}  // namespace

Image compose_capsule(const Image& cover, int width, int height) {
    if (!valid(cover) || width <= 0 || height <= 0) return blank(std::max(1, width), std::max(1, height), 24, 26, 31);

    // Background: the cover shrunk to a few dozen pixels and stretched back up
    // reads as a soft blur of its colours, darkened so the cover stands out.
    const Image tiny = resize_area(crop_to_aspect(cover, width, height), std::max(1, width / 40), std::max(1, height / 40));
    Image out = resize_bilinear(tiny, width, height);
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) out.rgba[i + c] = static_cast<uint8_t>(out.rgba[i + c] * 45 / 100);
        out.rgba[i + 3] = 255;
    }

    // Foreground: the whole cover inside a 7% margin. Small pixel art is
    // enlarged by a whole factor so its pixels stay square and sharp.
    const double scale = std::min(width * 0.86 / cover.width, height * 0.86 / cover.height);
    Image fg;
    if (scale >= 1.0) {
        const int k = std::max(1, static_cast<int>(std::floor(scale)));
        fg = resize_nearest(cover, cover.width * k, cover.height * k);
    } else {
        fg = resize_area(cover, std::max(1, static_cast<int>(std::lround(cover.width * scale))),
                         std::max(1, static_cast<int>(std::lround(cover.height * scale))));
    }
    blit_over(out, fg, (width - fg.width) / 2, (height - fg.height) / 2);
    return out;
}

Image compose_icon(const Image& cover) {
    if (!valid(cover)) return blank(256, 256, 24, 26, 31);
    const int side = std::min(cover.width, cover.height);
    const Image square = crop(cover, (cover.width - side) / 2, (cover.height - side) / 2, side, side);
    if (side < 256) {
        const int k = std::max(1, 256 / side);
        return resize_nearest(square, side * k, side * k);
    }
    return resize_area(square, 256, 256);
}

}  // namespace hub::steam
