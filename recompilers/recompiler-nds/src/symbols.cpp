#include "symbols.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace ndsrecomp {

namespace {

namespace fs = std::filesystem;
using std::string_view;
constexpr size_t npos = string_view::npos;

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

string_view trim(string_view text)
{
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

std::string in_quotes(string_view text)
{
    return "'" + std::string(text) + "'";
}

std::string at_line(const std::string &where, int line, const std::string &message)
{
    return where + ":" + std::to_string(line) + ": " + message;
}

// ---------------------------------------------------------------------------
// The line format symbols.txt, relocs.txt and delinks.txt share
// ---------------------------------------------------------------------------

// Calls line(number, text) for every line that still has something on it once
// a "//" comment is cut off. The text keeps its indentation, which delinks.txt
// gives a meaning to. Stops and returns false when the callback does.
template <typename Callback> bool for_each_line(string_view text, Callback &&line)
{
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    int number = 0;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        string_view row = text.substr(0, end);
        text.remove_prefix(end == npos ? text.size() : end + 1);
        number++;
        const size_t comment = row.find("//");
        if (comment != npos) row = row.substr(0, comment);
        while (!row.empty() && is_space(row.back())) row.remove_suffix(1);
        if (row.empty()) continue;
        if (!line(number, row)) return false;
    }
    return true;
}

// Splits at white space that is not inside parentheses, so that
// "kind:function(arm, size=0x4)" stays one word.
void split_words(string_view text, std::vector<string_view> &words)
{
    words.clear();
    size_t start = npos;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        const bool at_end = i == text.size();
        const char c = at_end ? ' ' : text[i];
        if (c == '(' && !at_end) depth++;
        if (c == ')' && depth > 0) depth--;
        if (at_end || (depth == 0 && is_space(c))) {
            if (start != npos) words.push_back(text.substr(start, i - start));
            start = npos;
        } else if (start == npos) {
            start = i;
        }
    }
}

// Splits at commas that are not inside parentheses; empty items are dropped.
void split_commas(string_view text, std::vector<string_view> &items)
{
    items.clear();
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        const bool at_end = i == text.size();
        const char c = at_end ? ',' : text[i];
        if (c == '(' && !at_end) depth++;
        if (c == ')' && depth > 0) depth--;
        if (at_end || (depth == 0 && c == ',')) {
            if (i > start) items.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
}

string_view strip_parens(string_view text)
{
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')') return text.substr(1, text.size() - 2);
    return text;
}

// "key<delimiter>value" into its halves; the value is empty when there is no
// delimiter, and loses one pair of surrounding parentheses.
std::pair<string_view, string_view> split_attribute(string_view word, char delimiter)
{
    const size_t at = word.find(delimiter);
    if (at == npos) return {word, string_view()};
    return {word.substr(0, at), strip_parens(word.substr(at + 1))};
}

// "name(options)" into its halves.
std::pair<string_view, string_view> split_call(string_view text)
{
    const size_t open = text.find('(');
    if (open == npos) return {text, string_view()};
    string_view options = text.substr(open + 1);
    if (!options.empty() && options.back() == ')') options.remove_suffix(1);
    return {text.substr(0, open), options};
}

bool parse_u64(string_view text, uint64_t limit, uint64_t &out)
{
    unsigned base = 10;
    if (text.size() >= 2 && text[0] == '0' && text[1] == 'x') {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
        else return false;
        if (value > (limit - digit) / base) return false;
        value = value * base + digit;
    }
    out = value;
    return true;
}

bool parse_u32(string_view text, uint32_t &out)
{
    uint64_t value = 0;
    if (!parse_u64(text, 0xFFFFFFFFull, value)) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool parse_i64(string_view text, int64_t &out)
{
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) text.remove_prefix(1);
    uint64_t value = 0;
    if (!parse_u64(text, 0x7FFFFFFFFFFFFFFFull, value)) return false;
    out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    return true;
}

bool parse_mode(string_view text, bool &thumb)
{
    if (text == "arm") thumb = false;
    else if (text == "thumb") thumb = true;
    else return false;
    return true;
}

// ---------------------------------------------------------------------------
// symbols.txt:  NAME kind:KIND addr:ADDRESS [local|weak] [ambiguous]
// ---------------------------------------------------------------------------

bool parse_function_options(string_view options, DsdSymbol &symbol, std::string &detail)
{
    bool have_mode = false;
    bool have_size = false;
    std::vector<string_view> items;
    split_commas(options, items);
    for (const string_view item : items) {
        const auto [key, value] = split_attribute(item, '=');
        if (!value.empty()) {
            if (key == "size") {
                if (!parse_u32(value, symbol.size)) {
                    detail = "cannot read the function size " + in_quotes(value);
                    return false;
                }
                have_size = true;
            } else if (key == "dsprot" || key == "dsprot_range") {
                // Which bytes of the function DS Protect keeps encrypted in
                // the ROM. Accepted so that such a config loads; nothing
                // here decrypts them.
            } else {
                detail = "unknown function attribute " + in_quotes(key);
                return false;
            }
        } else if (key == "unknown") {
            symbol.unknown = true;
        } else if (parse_mode(key, symbol.thumb)) {
            have_mode = true;
        } else {
            detail = "expected the instruction set 'arm' or 'thumb', found " + in_quotes(key);
            return false;
        }
    }
    if (!have_mode) {
        detail = "a function needs an instruction set, 'arm' or 'thumb'";
        return false;
    }
    if (!have_size) {
        detail = "a function needs a size";
        return false;
    }
    symbol.size_stated = true;
    return true;
}

// any | byte | short | word, the last three optionally with [] or [COUNT].
bool parse_data_options(string_view options, DsdSymbol &symbol, std::string &detail)
{
    if (options.empty()) {
        detail = "expected the data type 'any', 'byte', 'short' or 'word'";
        return false;
    }
    string_view type = options;
    bool array = false;
    bool counted = false;
    uint32_t count = 1;
    const size_t open = options.find('[');
    if (open != npos) {
        const size_t close = options.find(']', open);
        if (close == npos || close + 1 != options.size()) {
            detail = "malformed array in the data type " + in_quotes(options);
            return false;
        }
        type = options.substr(0, open);
        array = true;
        const string_view inside = options.substr(open + 1, close - open - 1);
        if (!inside.empty()) {
            if (!parse_u32(inside, count)) {
                detail = "cannot read the element count " + in_quotes(inside);
                return false;
            }
            counted = true;
        }
    }
    uint32_t element = 1;
    if (type == "any") {
        if (array) {
            detail = "the data type 'any' cannot be an array";
            return false;
        }
        symbol.data_type = DsdDataType::Any;
        return true; // size left open
    }
    if (type == "byte") {
        symbol.data_type = DsdDataType::Byte;
    } else if (type == "short") {
        symbol.data_type = DsdDataType::Short;
        element = 2;
    } else if (type == "word") {
        symbol.data_type = DsdDataType::Word;
        element = 4;
    } else {
        detail = "expected the data type 'any', 'byte', 'short' or 'word', found " + in_quotes(type);
        return false;
    }
    if (array && !counted) return true; // "word[]": size left open
    if (static_cast<uint64_t>(count) * element > 0xFFFFFFFFull) {
        detail = "the data symbol is larger than the address space";
        return false;
    }
    symbol.size = count * element;
    symbol.size_stated = true;
    return true;
}

bool parse_bss_options(string_view options, DsdSymbol &symbol, std::string &detail)
{
    std::vector<string_view> items;
    split_commas(options, items);
    for (const string_view item : items) {
        const auto [key, value] = split_attribute(item, '=');
        if (key != "size") {
            detail = "unknown bss attribute " + in_quotes(key);
            return false;
        }
        if (!parse_u32(value, symbol.size)) {
            detail = "cannot read the bss size " + in_quotes(value);
            return false;
        }
        symbol.size_stated = true;
    }
    return true;
}

bool parse_symbol(const std::vector<string_view> &words, DsdSymbol &symbol, std::string &detail)
{
    symbol = DsdSymbol();
    symbol.name = std::string(words[0]);
    bool have_kind = false;
    bool have_address = false;
    for (size_t i = 1; i < words.size(); i++) {
        const auto [key, value] = split_attribute(words[i], ':');
        if (key == "kind") {
            const auto [kind, options] = split_call(value);
            bool ok = false;
            if (kind == "function") {
                symbol.kind = DsdSymbolKind::Function;
                ok = parse_function_options(options, symbol, detail);
            } else if (kind == "label") {
                symbol.kind = DsdSymbolKind::Label;
                ok = parse_mode(options, symbol.thumb);
                if (!ok) detail = "expected the instruction set 'arm' or 'thumb', found " + in_quotes(options);
            } else if (kind == "data") {
                symbol.kind = DsdSymbolKind::Data;
                ok = parse_data_options(options, symbol, detail);
            } else if (kind == "bss") {
                symbol.kind = DsdSymbolKind::Bss;
                ok = parse_bss_options(options, symbol, detail);
            } else {
                detail = "unknown symbol kind " + in_quotes(kind) + ", expected function, label, data or bss";
            }
            if (!ok) return false;
            have_kind = true;
        } else if (key == "addr") {
            if (!parse_u32(value, symbol.address)) {
                detail = "cannot read the address " + in_quotes(value);
                return false;
            }
            have_address = true;
        } else if (key == "ambiguous") {
            symbol.ambiguous = true;
        } else if (key == "local" || key == "weak") {
            if (symbol.local || symbol.weak) {
                detail = "a symbol is local or weak, not both, and only once";
                return false;
            }
            (key == "local" ? symbol.local : symbol.weak) = true;
        } else {
            detail = "unknown symbol attribute " + in_quotes(key) + ", expected kind, addr, ambiguous, local or weak";
            return false;
        }
    }
    if (!have_kind || !have_address) {
        detail = std::string("the symbol has no '") + (have_kind ? "addr" : "kind") + "' attribute";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// relocs.txt:  from:FROM kind:KIND [to:TO] [add:ADD] [module:MODULE]
// ---------------------------------------------------------------------------

bool parse_link_time_const(string_view name, DsdLinkTimeConst &out)
{
    static const std::pair<string_view, DsdLinkTimeConst> table[] = {
        {"__DTCM_LO", DsdLinkTimeConst::DtcmLo},
        {"__ITCM_HI", DsdLinkTimeConst::ItcmHi},
        {"__CODE_HI", DsdLinkTimeConst::CodeHi},
        {"__OVERLAY_COUNT", DsdLinkTimeConst::OverlayCount},
        {"ARM9_CTOR_START", DsdLinkTimeConst::Arm9CtorStart},
        {"__exception_table_start__", DsdLinkTimeConst::ExceptionTableStart},
        {"__exception_table_end__", DsdLinkTimeConst::ExceptionTableEnd},
    };
    for (const auto &entry : table) {
        if (entry.first == name) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool parse_reloc_kind(string_view text, DsdRelocation &reloc, std::string &detail)
{
    static const std::pair<string_view, DsdRelocKind> table[] = {
        {"arm_call", DsdRelocKind::ArmCall},          {"thumb_call", DsdRelocKind::ThumbCall},
        {"arm_call_thumb", DsdRelocKind::ArmCallThumb}, {"thumb_call_arm", DsdRelocKind::ThumbCallArm},
        {"arm_branch", DsdRelocKind::ArmBranch},      {"load", DsdRelocKind::Load},
        {"overlay_id", DsdRelocKind::OverlayId},
    };
    for (const auto &entry : table) {
        if (entry.first == text) {
            reloc.kind = entry.second;
            return true;
        }
    }
    const string_view prefix = "link_time_const(";
    if (text.substr(0, prefix.size()) == prefix && text.back() == ')') {
        const string_view name = text.substr(prefix.size(), text.size() - prefix.size() - 1);
        if (!parse_link_time_const(name, reloc.constant)) {
            detail = "unknown link-time constant " + in_quotes(name);
            return false;
        }
        reloc.kind = DsdRelocKind::LinkTimeConst;
        return true;
    }
    detail = "unknown relocation kind " + in_quotes(text);
    return false;
}

bool parse_overlay_id(string_view text, uint16_t &out)
{
    uint64_t value = 0;
    if (!parse_u64(text, 0xFFFFull, value)) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

// none | main | itcm | dtcm | overlay(N) | overlays(N,N,...) | autoload(N)
bool parse_reloc_module(string_view text, DsdRelocation &reloc, std::string &detail)
{
    const auto [name, options] = split_call(text);
    reloc.overlays.clear();
    if (name == "overlay") {
        uint16_t id = 0;
        if (!parse_overlay_id(options, id)) {
            detail = "cannot read the overlay ID " + in_quotes(options);
            return false;
        }
        reloc.target = DsdRelocTarget::Overlay;
        reloc.overlays.push_back(id);
        return true;
    }
    if (name == "overlays") {
        size_t start = 0;
        for (size_t i = 0; i <= options.size(); i++) {
            if (i != options.size() && options[i] != ',') continue;
            uint16_t id = 0;
            if (!parse_overlay_id(options.substr(start, i - start), id)) {
                detail = "cannot read the overlay ID " + in_quotes(options.substr(start, i - start));
                return false;
            }
            reloc.overlays.push_back(id);
            start = i + 1;
        }
        if (reloc.overlays.size() < 2) {
            detail = "'overlays' needs two or more overlay IDs";
            return false;
        }
        reloc.target = DsdRelocTarget::Overlays;
        return true;
    }
    if (name == "autoload") {
        if (!parse_u32(options, reloc.autoload_index)) {
            detail = "cannot read the autoload index " + in_quotes(options);
            return false;
        }
        reloc.target = DsdRelocTarget::Autoload;
        return true;
    }
    if (name == "none") reloc.target = DsdRelocTarget::None;
    else if (name == "main") reloc.target = DsdRelocTarget::Main;
    else if (name == "itcm") reloc.target = DsdRelocTarget::Itcm;
    else if (name == "dtcm") reloc.target = DsdRelocTarget::Dtcm;
    else {
        detail = "unknown module " + in_quotes(name) + ", expected none, main, itcm, dtcm, overlay(N), overlays(N,...) "
                                                    "or autoload(N)";
        return false;
    }
    if (!options.empty()) {
        detail = "the module " + in_quotes(name) + " takes no options, found " + in_quotes(options);
        return false;
    }
    return true;
}

bool parse_relocation(const std::vector<string_view> &words, DsdRelocation &reloc, std::string &detail)
{
    reloc = DsdRelocation();
    bool have_from = false;
    bool have_to = false;
    bool have_kind = false;
    bool have_module = false;
    for (const string_view word : words) {
        const auto [key, value] = split_attribute(word, ':');
        if (key == "from") {
            if (!parse_u32(value, reloc.from)) {
                detail = "cannot read the 'from' address " + in_quotes(value);
                return false;
            }
            have_from = true;
        } else if (key == "to") {
            if (!parse_u32(value, reloc.to)) {
                detail = "cannot read the 'to' address " + in_quotes(value);
                return false;
            }
            have_to = true;
        } else if (key == "add") {
            if (!parse_i64(value, reloc.addend)) {
                detail = "cannot read the addend " + in_quotes(value);
                return false;
            }
        } else if (key == "kind") {
            if (!parse_reloc_kind(value, reloc, detail)) return false;
            have_kind = true;
        } else if (key == "module") {
            if (!parse_reloc_module(value, reloc, detail)) return false;
            have_module = true;
        } else {
            detail = "unknown relocation attribute " + in_quotes(key) + ", expected from, kind, to, add or module";
            return false;
        }
    }
    const char *missing = !have_from ? "from" : !have_kind ? "kind" : nullptr;
    if (!missing) {
        if (reloc.kind == DsdRelocKind::LinkTimeConst) {
            // Neither an address nor a module: the linker supplies the value.
            reloc.to = 0;
            reloc.target = DsdRelocTarget::None;
            reloc.overlays.clear();
        } else if (!have_to) {
            missing = "to";
        } else if (reloc.kind == DsdRelocKind::OverlayId) {
            reloc.target = DsdRelocTarget::None;
            reloc.overlays.clear();
        } else if (!have_module) {
            missing = "module";
        }
    }
    if (missing) {
        detail = std::string("the relocation has no '") + missing + "' attribute";
        return false;
    }
    return true;
}

bool same_relocation(const DsdRelocation &a, const DsdRelocation &b)
{
    return a.from == b.from && a.to == b.to && a.addend == b.addend && a.kind == b.kind && a.target == b.target &&
           a.overlays == b.overlays && a.autoload_index == b.autoload_index && a.constant == b.constant;
}

// ---------------------------------------------------------------------------
// config.yaml: the part of YAML that file uses, and no more
// ---------------------------------------------------------------------------
//
// Block mappings and block sequences nested by indentation (a sequence may sit
// at its key's own indentation, as serde writes it), plain, single-quoted and
// double-quoted scalars, "[]" and "{}", tags on scalars, and "#" comments.
// Anything else YAML allows (flow collections with content, block scalars,
// anchors, multi-line scalars) is an error rather than a guess.

struct YamlNode {
    enum class Type { Null, Scalar, Map, Sequence };
    Type type = Type::Null;
    int line = 0;
    std::string text; // Scalar
    std::string tag;  // "!Unknown 3" has the tag "Unknown"
    std::vector<std::string> keys; // Map, with `values`
    std::vector<YamlNode> values;
    std::vector<YamlNode> items;   // Sequence

    const YamlNode *get(string_view key) const
    {
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i] == key) return &values[i];
        }
        return nullptr;
    }
};

struct YamlLine {
    int number = 0;
    size_t indent = 0;
    string_view text;
};

bool is_sequence_item(string_view text)
{
    return text == "-" || text.substr(0, 2) == "- ";
}

// Cuts a "#" comment: one that starts the text or follows white space, and is
// not inside a quoted scalar.
string_view strip_yaml_comment(string_view text)
{
    char quote = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (quote) {
            if (c == '\\' && quote == '"') i++;
            else if (c == quote) quote = 0;
        } else if ((c == '\'' || c == '"') && (i == 0 || text[i - 1] == ' ')) {
            quote = c;
        } else if (c == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
            return text.substr(0, i);
        }
    }
    return text;
}

// The ':' that ends a mapping key: followed by a space or the end of the
// line, and not inside a quoted key.
size_t yaml_key_end(string_view text)
{
    char quote = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (quote) {
            if (c == '\\' && quote == '"') i++;
            else if (c == quote) quote = 0;
        } else if ((c == '\'' || c == '"') && i == 0) {
            quote = c;
        } else if (c == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) {
            return i;
        }
    }
    return npos;
}

struct YamlReader {
    static constexpr int kMaxDepth = 32;

    std::vector<YamlLine> lines;
    size_t pos = 0;
    std::string where;
    std::string error;

    bool fail(int line, const std::string &message)
    {
        error = at_line(where, line, message);
        return false;
    }

    bool split(string_view text)
    {
        if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
        int number = 0;
        while (!text.empty()) {
            const size_t end = text.find('\n');
            string_view row = text.substr(0, end);
            text.remove_prefix(end == npos ? text.size() : end + 1);
            number++;
            size_t indent = 0;
            while (indent < row.size() && row[indent] == ' ') indent++;
            row = strip_yaml_comment(row.substr(indent));
            while (!row.empty() && is_space(row.back())) row.remove_suffix(1);
            if (row.empty() || row == "---" || row[0] == '%') continue;
            if (row == "...") break;
            if (row[0] == '\t') return fail(number, "a tab in the indentation");
            lines.push_back({number, indent, row});
        }
        return true;
    }

    bool value(string_view text, int line, YamlNode &out)
    {
        out.line = line;
        if (!text.empty() && text[0] == '!') {
            const size_t end = text.find(' ');
            string_view tag = text.substr(0, end);
            while (!tag.empty() && tag[0] == '!') tag.remove_prefix(1);
            out.tag = std::string(tag);
            text = end == npos ? string_view() : trim(text.substr(end));
        }
        if (text.empty()) return true;
        if (text == "[]") {
            out.type = YamlNode::Type::Sequence;
            return true;
        }
        if (text == "{}") {
            out.type = YamlNode::Type::Map;
            return true;
        }
        const char first = text[0];
        if (first == '[' || first == '{') return fail(line, "flow collections are not supported by this reader");
        if (first == '|' || first == '>') return fail(line, "block scalars are not supported by this reader");
        if (first == '&' || first == '*') return fail(line, "anchors and aliases are not supported by this reader");
        out.type = YamlNode::Type::Scalar;
        if (first != '\'' && first != '"') {
            out.text = std::string(text);
            return true;
        }
        size_t i = 1;
        for (;; i++) {
            if (i >= text.size()) return fail(line, "a quoted scalar is not closed");
            const char c = text[i];
            if (first == '\'') {
                if (c != '\'') {
                    out.text += c;
                } else if (i + 1 < text.size() && text[i + 1] == '\'') {
                    out.text += '\'';
                    i++;
                } else {
                    break;
                }
            } else if (c == '\\') {
                if (++i >= text.size()) return fail(line, "a quoted scalar is not closed");
                switch (text[i]) {
                case '\\': out.text += '\\'; break;
                case '"': out.text += '"'; break;
                case '/': out.text += '/'; break;
                case 'n': out.text += '\n'; break;
                case 't': out.text += '\t'; break;
                case 'r': out.text += '\r'; break;
                case '0': out.text += '\0'; break;
                default: return fail(line, "this reader does not know the escape in a quoted scalar");
                }
            } else if (c == '"') {
                break;
            } else {
                out.text += c;
            }
        }
        if (i + 1 != text.size()) return fail(line, "text after the closing quote");
        return true;
    }

    bool block(size_t indent, int depth, YamlNode &out)
    {
        if (depth > kMaxDepth) return fail(lines[pos].number, "nested too deeply");
        out.line = lines[pos].number;
        return is_sequence_item(lines[pos].text) ? sequence(indent, depth, out) : map(indent, depth, out);
    }

    bool map(size_t indent, int depth, YamlNode &out)
    {
        out.type = YamlNode::Type::Map;
        while (pos < lines.size() && lines[pos].indent == indent && !is_sequence_item(lines[pos].text)) {
            const YamlLine line = lines[pos];
            const size_t colon = yaml_key_end(line.text);
            if (colon == npos) return fail(line.number, "expected 'key: value'");
            YamlNode key;
            if (!value(trim(line.text.substr(0, colon)), line.number, key)) return false;
            if (key.type != YamlNode::Type::Scalar) return fail(line.number, "expected a plain mapping key");
            if (out.get(key.text)) return fail(line.number, "the key " + in_quotes(key.text) + " appears twice");
            const string_view rest = trim(line.text.substr(colon + 1));
            pos++;
            YamlNode node;
            node.line = line.number;
            if (!rest.empty()) {
                if (!value(rest, line.number, node)) return false;
            } else if (pos < lines.size() && lines[pos].indent > indent) {
                if (!block(lines[pos].indent, depth + 1, node)) return false;
            } else if (pos < lines.size() && lines[pos].indent == indent && is_sequence_item(lines[pos].text)) {
                if (!sequence(indent, depth + 1, node)) return false;
            }
            out.keys.push_back(std::move(key.text));
            out.values.push_back(std::move(node));
        }
        if (pos < lines.size() && lines[pos].indent > indent) {
            return fail(lines[pos].number, "unexpected indentation");
        }
        return true;
    }

    bool sequence(size_t indent, int depth, YamlNode &out)
    {
        out.type = YamlNode::Type::Sequence;
        while (pos < lines.size() && lines[pos].indent == indent && is_sequence_item(lines[pos].text)) {
            const YamlLine line = lines[pos];
            size_t offset = 1;
            while (offset < line.text.size() && line.text[offset] == ' ') offset++;
            const string_view rest = line.text.substr(offset);
            YamlNode node;
            node.line = line.number;
            if (rest.empty()) {
                pos++;
                if (pos < lines.size() && lines[pos].indent > indent) {
                    if (!block(lines[pos].indent, depth + 1, node)) return false;
                }
            } else if (is_sequence_item(rest) || yaml_key_end(rest) != npos) {
                // "- key: value": the item is a block that starts on this line,
                // at the column of the text after the dash.
                lines[pos].indent = indent + offset;
                lines[pos].text = rest;
                if (!block(indent + offset, depth + 1, node)) return false;
            } else {
                pos++;
                if (!value(rest, line.number, node)) return false;
            }
            out.items.push_back(std::move(node));
        }
        return true;
    }

    bool parse(string_view text, YamlNode &root)
    {
        if (!split(text)) return false;
        if (lines.empty()) return true;
        if (!block(lines[0].indent, 0, root)) return false;
        if (pos < lines.size()) return fail(lines[pos].number, "unexpected indentation");
        return true;
    }
};

// A string field of a module's mapping.
bool yaml_string(const YamlNode &map, string_view key, bool required, const std::string &where, std::string &out,
                 std::string &error)
{
    const YamlNode *node = map.get(key);
    if (!node || node->type == YamlNode::Type::Null) {
        if (!required) return true;
        error = at_line(where, map.line, "the key " + in_quotes(key) + " is missing");
        return false;
    }
    if (node->type != YamlNode::Type::Scalar) {
        error = at_line(where, node->line, "the key " + in_quotes(key) + " is not a string");
        return false;
    }
    out = node->text;
    return true;
}

bool yaml_module(const YamlNode &node, const std::string &where, DsdModule &module, std::string &error)
{
    if (node.type != YamlNode::Type::Map) {
        error = at_line(where, node.line, "expected a module (name, object, hash, delinks, symbols, relocations)");
        return false;
    }
    std::string hash;
    if (!yaml_string(node, "name", true, where, module.name, error) ||
        !yaml_string(node, "object", false, where, module.object_path, error) ||
        !yaml_string(node, "hash", true, where, hash, error) ||
        !yaml_string(node, "delinks", false, where, module.delinks_path, error) ||
        !yaml_string(node, "symbols", true, where, module.symbols_path, error) ||
        !yaml_string(node, "relocations", true, where, module.relocations_path, error)) {
        return false;
    }
    bool ok = !hash.empty() && hash.size() <= 16;
    module.hash = 0;
    for (const char c : hash) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
        else ok = false;
        module.hash = (module.hash << 4) | digit;
    }
    if (!ok) {
        error = at_line(where, node.line,
                        "module " + in_quotes(module.name) + ": the hash " + in_quotes(hash) +
                            " is not a 64-bit hexadecimal number");
        return false;
    }
    return true;
}

bool yaml_bool(const YamlNode &map, string_view key, bool &out)
{
    const YamlNode *node = map.get(key);
    if (!node || node->type != YamlNode::Type::Scalar) return false;
    out = node->text == "true";
    return true;
}

bool read_text(const fs::path &path, std::string &out, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

const char *dsd_reloc_kind_name(DsdRelocKind kind)
{
    switch (kind) {
    case DsdRelocKind::ArmCall: return "arm_call";
    case DsdRelocKind::ThumbCall: return "thumb_call";
    case DsdRelocKind::ArmCallThumb: return "arm_call_thumb";
    case DsdRelocKind::ThumbCallArm: return "thumb_call_arm";
    case DsdRelocKind::ArmBranch: return "arm_branch";
    case DsdRelocKind::Load: return "load";
    case DsdRelocKind::OverlayId: return "overlay_id";
    case DsdRelocKind::LinkTimeConst: return "link_time_const";
    }
    return "?";
}

bool parse_dsd_symbols(const std::string &text, const std::string &where, std::vector<DsdSymbol> &out,
                       std::string &error)
{
    out.clear();
    std::vector<string_view> words;
    return for_each_line(text, [&](int number, string_view row) {
        split_words(row, words);
        if (words.empty()) return true;
        DsdSymbol symbol;
        std::string detail;
        if (!parse_symbol(words, symbol, detail)) {
            error = at_line(where, number, detail);
            return false;
        }
        out.push_back(std::move(symbol));
        return true;
    });
}

bool parse_dsd_relocations(const std::string &text, const std::string &where, std::vector<DsdRelocation> &out,
                           std::string &error)
{
    out.clear();
    std::vector<string_view> words;
    return for_each_line(text, [&](int number, string_view row) {
        split_words(row, words);
        if (words.empty()) return true;
        DsdRelocation reloc;
        std::string detail;
        if (!parse_relocation(words, reloc, detail)) {
            error = at_line(where, number, detail);
            return false;
        }
        out.push_back(std::move(reloc));
        return true;
    });
}

bool parse_dsd_sections(const std::string &text, const std::string &where, std::vector<DsdSection> &out,
                        std::string &error)
{
    out.clear();
    std::vector<string_view> words;
    bool in_files = false;
    return for_each_line(text, [&](int number, string_view row) {
        // The module's sections are indented; the first line that is not
        // starts the list of source files, which is not read.
        if (in_files || !is_space(row[0])) {
            in_files = true;
            return true;
        }
        split_words(row, words);
        if (words.empty() || words[0].back() == ':') return true; // an option, such as "categories: ..."
        DsdSection section;
        section.name = std::string(words[0]);
        bool have_kind = false;
        bool have_start = false;
        bool have_end = false;
        bool have_align = false;
        for (size_t i = 1; i < words.size(); i++) {
            const auto [key, value] = split_attribute(words[i], ':');
            bool ok = true;
            if (key == "start") {
                ok = have_start = parse_u32(value, section.start);
            } else if (key == "end") {
                ok = have_end = parse_u32(value, section.end);
            } else if (key == "align") {
                ok = have_align = parse_u32(value, section.alignment);
            } else if (key == "kind") {
                have_kind = true;
                if (value == "code") section.kind = DsdSectionKind::Code;
                else if (value == "data") section.kind = DsdSectionKind::Data;
                else if (value == "rodata") section.kind = DsdSectionKind::Rodata;
                else if (value == "bss") section.kind = DsdSectionKind::Bss;
                else ok = false;
            } else {
                error = at_line(where, number,
                                "unknown section attribute " + in_quotes(key) + ", expected start, end, kind or align");
                return false;
            }
            if (!ok) {
                error = at_line(where, number, "cannot read the section's " + in_quotes(key) + " " + in_quotes(value));
                return false;
            }
        }
        if (!have_kind || !have_start || !have_end || !have_align) {
            error = at_line(where, number, "a section needs start, end, kind and align");
            return false;
        }
        if (section.end < section.start) {
            error = at_line(where, number, "the section ends before it starts");
            return false;
        }
        out.push_back(std::move(section));
        return true;
    });
}

bool parse_dsd_config(const std::string &text, const std::string &where, DsdConfig &out, std::string &error)
{
    out = DsdConfig();
    YamlReader reader;
    reader.where = where;
    YamlNode root;
    if (!reader.parse(text, root)) {
        error = reader.error;
        return false;
    }
    if (root.type != YamlNode::Type::Map) {
        error = where + ": not a ds-decomp config.yaml (no top-level mapping)";
        return false;
    }
    if (!yaml_string(root, "rom_config", false, where, out.rom_config, error) ||
        !yaml_string(root, "build_path", false, where, out.build_path, error) ||
        !yaml_string(root, "delinks_path", false, where, out.delinks_dir, error)) {
        return false;
    }
    yaml_bool(root, "enable_dsprot", out.enable_dsprot);

    const YamlNode *main_module = root.get("main_module");
    if (!main_module) {
        error = where + ": not a ds-decomp config.yaml (no 'main_module')";
        return false;
    }
    DsdModule module;
    module.kind = DsdModuleKind::Main;
    if (!yaml_module(*main_module, where, module, error)) return false;
    out.modules.push_back(std::move(module));

    for (const char *list : {"autoloads", "overlays"}) {
        const bool overlays = std::string_view(list) == "overlays";
        const YamlNode *node = root.get(list);
        if (!node || node->type == YamlNode::Type::Null) continue;
        if (node->type != YamlNode::Type::Sequence) {
            error = at_line(where, node->line, std::string("'") + list + "' is not a list");
            return false;
        }
        for (const YamlNode &item : node->items) {
            module = DsdModule();
            if (!yaml_module(item, where, module, error)) return false;
            if (overlays) {
                const YamlNode *id = item.get("id");
                uint16_t value = 0;
                if (!id || id->type != YamlNode::Type::Scalar || !parse_overlay_id(id->text, value)) {
                    error = at_line(where, item.line, "overlay " + in_quotes(module.name) + " has no 16-bit 'id'");
                    return false;
                }
                module.kind = DsdModuleKind::Overlay;
                module.overlay_id = value;
                yaml_bool(item, "signed", module.overlay_signed);
            } else {
                // kind: Itcm | Dtcm | Unknown with an index, which serde
                // writes as a tagged scalar or as a one-entry mapping.
                const YamlNode *kind = item.get("kind");
                const YamlNode *index = nullptr;
                if (kind && kind->type == YamlNode::Type::Scalar && kind->tag.empty() && kind->text == "Itcm") {
                    module.kind = DsdModuleKind::Itcm;
                } else if (kind && kind->type == YamlNode::Type::Scalar && kind->tag.empty() && kind->text == "Dtcm") {
                    module.kind = DsdModuleKind::Dtcm;
                } else if (kind && kind->tag == "Unknown") {
                    index = kind;
                } else if (kind && kind->type == YamlNode::Type::Map) {
                    index = kind->get("Unknown");
                }
                if (index) {
                    uint32_t value = 0;
                    if (index->type != YamlNode::Type::Scalar || !parse_u32(index->text, value) || value > 0x7FFFFFFFu) {
                        index = nullptr;
                    } else {
                        module.kind = DsdModuleKind::Autoload;
                        module.autoload_index = static_cast<int>(value);
                    }
                }
                if (module.kind == DsdModuleKind::Main) {
                    error = at_line(where, item.line,
                                    "autoload " + in_quotes(module.name) + ": 'kind' is not Itcm, Dtcm or Unknown(N)");
                    return false;
                }
            }
            out.modules.push_back(std::move(module));
        }
    }
    return true;
}

bool DsdModule::finish(std::string &error)
{
    std::stable_sort(symbols.begin(), symbols.end(),
                     [](const DsdSymbol &a, const DsdSymbol &b) { return a.address < b.address; });
    std::stable_sort(sections.begin(), sections.end(),
                     [](const DsdSection &a, const DsdSection &b) { return a.start < b.start; });
    std::stable_sort(relocations.begin(), relocations.end(),
                     [](const DsdRelocation &a, const DsdRelocation &b) { return a.from < b.from; });
    for (size_t i = 1; i < relocations.size();) {
        if (relocations[i].from != relocations[i - 1].from) {
            i++;
        } else if (same_relocation(relocations[i], relocations[i - 1])) {
            relocations.erase(relocations.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            char text[16];
            std::snprintf(text, sizeof text, "0x%08x", static_cast<unsigned>(relocations[i].from));
            error = std::string("two different relocations from ") + text;
            return false;
        }
    }

    functions.clear();
    data.clear();
    for (size_t i = 0; i < symbols.size(); i++) {
        DsdSymbol &symbol = symbols[i];
        if (symbol.kind == DsdSymbolKind::Function) {
            functions.push_back({symbol.address, symbol.size, symbol.thumb, symbol.name});
            continue;
        }
        if (symbol.kind == DsdSymbolKind::Label) continue;
        if (!symbol.size_stated) {
            // dsd's rule: up to the next symbol or the end of the section.
            uint64_t limit = 0;
            for (size_t next = i + 1; next < symbols.size(); next++) {
                if (symbols[next].address > symbol.address) {
                    limit = symbols[next].address;
                    break;
                }
            }
            const DsdSection *section = section_at(symbol.address);
            if (section && (limit == 0 || section->end < limit)) limit = section->end;
            symbol.size = limit > symbol.address ? static_cast<uint32_t>(limit - symbol.address) : 0;
        }
        data.push_back({symbol.address, symbol.size, symbol.kind == DsdSymbolKind::Bss, symbol.data_type, symbol.name});
    }
    return true;
}

const DsdFunction *DsdModule::function_at(uint32_t address) const
{
    const auto it = std::lower_bound(functions.begin(), functions.end(), address,
                                     [](const DsdFunction &function, uint32_t value) { return function.address < value; });
    return it != functions.end() && it->address == address ? &*it : nullptr;
}

const DsdRelocation *DsdModule::relocation_at(uint32_t from) const
{
    const auto it = std::lower_bound(relocations.begin(), relocations.end(), from,
                                     [](const DsdRelocation &reloc, uint32_t value) { return reloc.from < value; });
    return it != relocations.end() && it->from == from ? &*it : nullptr;
}

const DsdSection *DsdModule::section_at(uint32_t address) const
{
    for (const DsdSection &section : sections) {
        if (address >= section.start && address < section.end) return &section;
    }
    return nullptr;
}

bool DsdConfig::load(const std::string &path, std::string &error)
{
    std::error_code ignored;
    fs::path file(path);
    if (fs::is_directory(file, ignored)) file /= "config.yaml";
    std::string text;
    if (!read_text(file, text, error)) {
        error += " (expected a ds-decomp config.yaml, or the directory that holds one)";
        return false;
    }
    if (!parse_dsd_config(text, file.string(), *this, error)) return false;
    const fs::path base = file.parent_path();
    directory = base.string();

    for (DsdModule &module : modules) {
        fs::path source = base / module.symbols_path;
        if (!read_text(source, text, error) || !parse_dsd_symbols(text, source.string(), module.symbols, error)) {
            return false;
        }
        source = base / module.relocations_path;
        if (!read_text(source, text, error) ||
            !parse_dsd_relocations(text, source.string(), module.relocations, error)) {
            return false;
        }
        // The section list only bounds open-ended data symbols; a config
        // without it is still usable.
        source = base / module.delinks_path;
        if (!module.delinks_path.empty() && fs::is_regular_file(source, ignored)) {
            if (!read_text(source, text, error) || !parse_dsd_sections(text, source.string(), module.sections, error)) {
                return false;
            }
        }
        std::string detail;
        if (!module.finish(detail)) {
            error = (base / module.relocations_path).string() + ": " + detail;
            return false;
        }
    }
    return true;
}

const DsdModule *DsdConfig::find(DsdModuleKind kind, int index) const
{
    for (const DsdModule &module : modules) {
        if (module.kind != kind) continue;
        if (index < 0) return &module;
        if ((kind == DsdModuleKind::Overlay ? module.overlay_id : module.autoload_index) == index) return &module;
    }
    return nullptr;
}

} // namespace ndsrecomp
