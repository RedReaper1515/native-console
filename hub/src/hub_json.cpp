#include "hub_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hub {

const Json* Json::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Json* Json::get(const std::string& key) {
    if (type != Type::Object) return nullptr;
    for (auto& kv : object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Json& Json::set(const std::string& key, Json value) {
    type = Type::Object;
    for (auto& kv : object) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return kv.second;
        }
    }
    object.emplace_back(key, std::move(value));
    return object.back().second;
}

std::string Json::get_string(const std::string& key, const std::string& fallback) const {
    const Json* v = get(key);
    return (v && v->type == Type::String) ? v->string : fallback;
}

bool Json::get_bool(const std::string& key, bool fallback) const {
    const Json* v = get(key);
    return (v && v->type == Type::Bool) ? v->boolean : fallback;
}

double Json::get_number(const std::string& key, double fallback) const {
    const Json* v = get(key);
    return (v && v->type == Type::Number) ? v->number : fallback;
}

long long Json::get_int(const std::string& key, long long fallback) const {
    const Json* v = get(key);
    return (v && v->type == Type::Number) ? static_cast<long long>(v->number) : fallback;
}

namespace {

struct Parser {
    const std::string& text;
    size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& t) : text(t) {}

    void skip_ws() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
            ++pos;
        }
    }

    bool fail(const std::string& msg) {
        if (error.empty()) {
            error = msg + " at offset " + std::to_string(pos);
        }
        return false;
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(unsigned& out) {
        if (pos + 4 > text.size()) return fail("truncated unicode escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') return fail("expected string");
        ++pos;
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= text.size()) return fail("truncated escape");
                char e = text[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parse_hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
                            pos += 2;
                            unsigned lo = 0;
                            if (!parse_hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parse_value(Json& out) {
        skip_ws();
        if (pos >= text.size()) return fail("unexpected end of input");
        char c = text[pos];
        if (c == '{') {
            ++pos;
            out = Json::make_object();
            skip_ws();
            if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
            while (true) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (pos >= text.size() || text[pos] != ':') return fail("expected ':'");
                ++pos;
                Json value;
                if (!parse_value(value)) return false;
                out.object.emplace_back(std::move(key), std::move(value));
                skip_ws();
                if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos;
            out = Json::make_array();
            skip_ws();
            if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
            while (true) {
                Json value;
                if (!parse_value(value)) return false;
                out.array.push_back(std::move(value));
                skip_ws();
                if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return parse_string(out.string);
        }
        if (text.compare(pos, 4, "true") == 0) { pos += 4; out = Json(true); return true; }
        if (text.compare(pos, 5, "false") == 0) { pos += 5; out = Json(false); return true; }
        if (text.compare(pos, 4, "null") == 0) { pos += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = text.c_str() + pos;
            char* end = nullptr;
            double v = std::strtod(start, &end);
            if (end == start) return fail("bad number");
            pos += static_cast<size_t>(end - start);
            out = Json(v);
            return true;
        }
        return fail("unexpected character");
    }
};

void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void dump_value(std::string& out, const Json& j, int indent, int depth) {
    auto newline = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(d * indent), ' ');
    };
    switch (j.type) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.boolean ? "true" : "false"; break;
        case Json::Type::Number: {
            if (std::isfinite(j.number) && std::floor(j.number) == j.number && std::fabs(j.number) < 1e15) {
                out += std::to_string(static_cast<long long>(j.number));
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", j.number);
                out += buf;
            }
            break;
        }
        case Json::Type::String: dump_string(out, j.string); break;
        case Json::Type::Array: {
            if (j.array.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < j.array.size(); ++i) {
                newline(depth + 1);
                dump_value(out, j.array[i], indent, depth + 1);
                if (i + 1 < j.array.size()) out.push_back(',');
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Json::Type::Object: {
            if (j.object.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (size_t i = 0; i < j.object.size(); ++i) {
                newline(depth + 1);
                dump_string(out, j.object[i].first);
                out += indent > 0 ? ": " : ":";
                dump_value(out, j.object[i].second, indent, depth + 1);
                if (i + 1 < j.object.size()) out.push_back(',');
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string& error) {
    Parser p(text);
    if (!p.parse_value(out)) {
        error = p.error;
        return false;
    }
    p.skip_ws();
    if (p.pos != text.size()) {
        error = "trailing characters at offset " + std::to_string(p.pos);
        return false;
    }
    return true;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_value(out, *this, indent, 0);
    if (indent > 0) out.push_back('\n');
    return out;
}

bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_text_file_atomic(const std::string& path, const std::string& text, std::string& error) {
    namespace fs = std::filesystem;
    fs::path target(reinterpret_cast<const char8_t*>(path.c_str()));
    fs::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open " + path + " for writing";
            return false;
        }
        out << text;
        if (!out) {
            error = "short write to " + path;
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Some platforms refuse to rename over an existing file; fall back to remove + rename.
        ec.clear();
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
        if (ec) {
            error = "cannot replace " + path + ": " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace hub
