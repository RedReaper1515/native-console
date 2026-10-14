#include "recompiler/native_patch.h"
#include "recompiler/lift_package.h"
#include "recompiler/lift_export.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace gbrecomp {
namespace {

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t size) {
    static constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const uint64_t bit_size = static_cast<uint64_t>(size) * 8u;
    const size_t padded_size = ((size + 9u + 63u) / 64u) * 64u;
    std::vector<uint8_t> padded(padded_size, 0);
    if (size > 0) {
        std::copy(data, data + size, padded.begin());
    }
    padded[size] = 0x80u;
    for (size_t i = 0; i < 8; ++i) {
        padded[padded_size - 1u - i] = static_cast<uint8_t>(bit_size >> (i * 8u));
    }

    for (size_t offset = 0; offset < padded.size(); offset += 64u) {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = offset + i * 4u;
            w[i] = (static_cast<uint32_t>(padded[p]) << 24u) |
                   (static_cast<uint32_t>(padded[p + 1]) << 16u) |
                   (static_cast<uint32_t>(padded[p + 2]) << 8u) |
                   static_cast<uint32_t>(padded[p + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate_right(w[i - 15], 7) ^
                                rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3u);
            const uint32_t s1 = rotate_right(w[i - 2], 17) ^
                                rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10u);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::array<uint8_t, 32> digest{};
    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4u] = static_cast<uint8_t>(h[i] >> 24u);
        digest[i * 4u + 1u] = static_cast<uint8_t>(h[i] >> 16u);
        digest[i * 4u + 2u] = static_cast<uint8_t>(h[i] >> 8u);
        digest[i * 4u + 3u] = static_cast<uint8_t>(h[i]);
    }
    return digest;
}

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    uint64_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        JsonValue value = parse_value();
        skip_ws();
        if (position_ != input_.size()) {
            fail("unexpected trailing content");
        }
        return value;
    }

private:
    std::string_view input_;
    size_t position_ = 0;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message + " at byte " + std::to_string(position_));
    }

    void skip_ws() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    char take() {
        if (position_ >= input_.size()) fail("unexpected end of input");
        return input_[position_++];
    }

    bool consume(char expected) {
        skip_ws();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        skip_ws();
        if (position_ >= input_.size()) fail("expected a JSON value");
        switch (input_[position_]) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.string = parse_string();
                return value;
            }
            case 't': return parse_keyword("true", true);
            case 'f': return parse_keyword("false", false);
            case 'n': return parse_null();
            default:
                if (std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                    return parse_number();
                }
                fail("invalid JSON value");
        }
    }

    JsonValue parse_object() {
        JsonValue value;
        value.type = JsonValue::Type::Object;
        take();
        if (consume('}')) return value;
        for (;;) {
            skip_ws();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("expected an object key");
            }
            std::string key = parse_string();
            if (!consume(':')) fail("expected ':' after object key");
            JsonValue child = parse_value();
            if (!value.object.emplace(key, std::move(child)).second) {
                fail("duplicate object key '" + key + "'");
            }
            if (consume('}')) break;
            if (!consume(',')) fail("expected ',' or '}' in object");
        }
        return value;
    }

    JsonValue parse_array() {
        JsonValue value;
        value.type = JsonValue::Type::Array;
        take();
        if (consume(']')) return value;
        for (;;) {
            value.array.push_back(parse_value());
            if (consume(']')) break;
            if (!consume(',')) fail("expected ',' or ']' in array");
        }
        return value;
    }

    static void append_utf8(std::string& output, uint32_t codepoint) {
        if (codepoint <= 0x7fu) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    std::string parse_string() {
        if (take() != '"') fail("expected string");
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(take());
            if (ch == '"') return result;
            if (ch < 0x20u) fail("control character in string");
            if (ch != '\\') {
                result.push_back(static_cast<char>(ch));
                continue;
            }
            const char escaped = take();
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4u > input_.size()) fail("short Unicode escape");
                    uint32_t codepoint = 0;
                    for (size_t i = 0; i < 4; ++i) {
                        const char digit = take();
                        codepoint <<= 4u;
                        if (digit >= '0' && digit <= '9') codepoint |= digit - '0';
                        else if (digit >= 'a' && digit <= 'f') codepoint |= digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') codepoint |= digit - 'A' + 10;
                        else fail("invalid Unicode escape");
                    }
                    if (codepoint >= 0xd800u && codepoint <= 0xdfffu) {
                        fail("surrogate Unicode escapes are not supported");
                    }
                    append_utf8(result, codepoint);
                    break;
                }
                default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    JsonValue parse_number() {
        const size_t start = position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ - start > 1u && input_[start] == '0') {
            fail("leading zero in JSON number");
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
            fail("manifest numbers must be non-negative integers");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        const std::string_view digits = input_.substr(start, position_ - start);
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value.number);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
            fail("integer is out of range");
        }
        return value;
    }

    JsonValue parse_keyword(std::string_view keyword, bool boolean) {
        if (input_.substr(position_, keyword.size()) != keyword) fail("invalid keyword");
        position_ += keyword.size();
        JsonValue value;
        value.type = JsonValue::Type::Bool;
        value.boolean = boolean;
        return value;
    }

    JsonValue parse_null() {
        if (input_.substr(position_, 4) != "null") fail("invalid keyword");
        position_ += 4;
        return JsonValue{};
    }
};

static const JsonValue& required_member(const JsonValue& object,
                                        const std::string& name,
                                        JsonValue::Type type) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error("expected JSON object");
    }
    auto it = object.object.find(name);
    if (it == object.object.end()) {
        throw std::runtime_error("missing required field '" + name + "'");
    }
    if (it->second.type != type) {
        throw std::runtime_error("field '" + name + "' has the wrong type");
    }
    return it->second;
}

static const JsonValue* optional_member(const JsonValue& object,
                                        const std::string& name,
                                        JsonValue::Type type) {
    auto it = object.object.find(name);
    if (it == object.object.end()) return nullptr;
    if (it->second.type != type) {
        throw std::runtime_error("field '" + name + "' has the wrong type");
    }
    return &it->second;
}

static void require_only_fields(const JsonValue& object,
                                std::initializer_list<const char*> fields,
                                const char* context) {
    std::set<std::string> allowed;
    for (const char* field : fields) allowed.emplace(field);
    for (const auto& [name, value] : object.object) {
        (void)value;
        if (!allowed.count(name)) {
            throw std::runtime_error(std::string("unknown ") + context + " field '" + name + "'");
        }
    }
}

static bool is_c_identifier(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_';
    });
}

static bool parse_hex(std::string_view text, uint32_t& value) {
    value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

static NativePatchBinding parse_function_id(const std::string& id) {
    static constexpr std::string_view prefix = "gbfn:v1:";
    if (id.size() != prefix.size() + 4u + 1u + 4u ||
        id.compare(0, prefix.size(), prefix) != 0 || id[prefix.size() + 4u] != ':') {
        throw std::runtime_error("invalid function ID '" + id +
                                 "' (expected gbfn:v1:BBBB:AAAA)");
    }
    uint32_t bank = 0;
    uint32_t address = 0;
    if (!parse_hex(std::string_view(id).substr(prefix.size(), 4), bank) ||
        !parse_hex(std::string_view(id).substr(prefix.size() + 5u, 4), address) ||
        bank > 0xffffu || address > 0xffffu) {
        throw std::runtime_error("invalid hexadecimal function ID '" + id + "'");
    }
    NativePatchBinding binding;
    binding.bank = static_cast<BankId>(bank);
    binding.address = static_cast<uint16_t>(address);
    binding.function_id = native_function_id(binding.bank, binding.address);
    if (binding.function_id != id) {
        throw std::runtime_error("function ID must use canonical lowercase hexadecimal: '" +
                                 binding.function_id + "'");
    }
    return binding;
}

static std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20u) {
                    output << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                           << static_cast<unsigned>(ch) << std::dec;
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

static std::string normalized_manifest(const NativePatchPackage& package) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schema\": \"gbrecomp.native-patch\",\n"
           << "  \"version\": 1,\n"
           << "  \"patch_id\": \"" << json_escape(package.patch_id) << "\",\n"
           << "  \"rom\": {\n"
           << "    \"sha256\": \"" << package.rom_sha256 << "\",\n"
           << "    \"size\": " << package.rom_size << "\n"
           << "  },\n"
           << "  \"sources\": [\n";
    for (size_t i = 0; i < package.sources.size(); ++i) {
        output << "    \"" << json_escape(package.sources[i].relative_path) << "\""
               << (i + 1u < package.sources.size() ? "," : "") << "\n";
    }
    output << "  ],\n  \"bindings\": [\n";
    for (size_t i = 0; i < package.bindings.size(); ++i) {
        const NativePatchBinding& binding = package.bindings[i];
        output << "    {\n      \"function\": \"" << binding.function_id << "\"";
        if (!binding.pre_callback.empty()) {
            output << ",\n      \"pre\": \"" << binding.pre_callback << "\"";
        }
        if (!binding.replace_callback.empty()) {
            output << ",\n      \"replace\": \"" << binding.replace_callback << "\"";
        }
        if (!binding.post_callback.empty()) {
            output << ",\n      \"post\": \"" << binding.post_callback << "\"";
        }
        if (binding.allow_return_stack_entry) {
            output << ",\n      \"entry_contract\": \"return-stack\"";
        }
        output << "\n    }" << (i + 1u < package.bindings.size() ? "," : "") << "\n";
    }
    output << "  ]\n}\n";
    return output.str();
}

static bool path_is_safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return true;
}

static bool path_has_portable_source_name(const std::filesystem::path& path) {
    const std::string normalized = path.generic_string();
    return !normalized.empty() &&
           std::all_of(normalized.begin(), normalized.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                      ch == '.' || ch == '/';
           });
}

static bool path_is_within(const std::filesystem::path& directory,
                           const std::filesystem::path& candidate) {
    auto directory_it = directory.begin();
    auto candidate_it = candidate.begin();
    for (; directory_it != directory.end() && candidate_it != candidate.end();
         ++directory_it, ++candidate_it) {
        if (*directory_it != *candidate_it) return false;
    }
    return directory_it == directory.end() && candidate_it != candidate.end();
}

static std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open '" + path.string() + "'");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("could not read '" + path.string() + "'");
    }
    return contents.str();
}

} // namespace

std::string sha256_hex(const uint8_t* data, size_t size) {
    const std::array<uint8_t, 32> digest = sha256_bytes(data, size);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

std::string native_function_id(BankId bank, uint16_t address) {
    std::ostringstream output;
    output << "gbfn:v1:" << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(4) << static_cast<unsigned>(bank) << ':'
           << std::setw(4) << static_cast<unsigned>(address);
    return output.str();
}

bool is_native_patchable_function(const ir::Program& program,
                                  const ir::Function& function,
                                  size_t rom_size) {
    if (function.is_interrupt_handler) return false;

    size_t rom_offset = 0;
    if (function.bank == 0 && function.entry_address < 0x4000u) {
        rom_offset = function.entry_address;
    } else if (function.bank > 0 && function.entry_address >= 0x4000u &&
               function.entry_address < 0x8000u) {
        rom_offset = static_cast<size_t>(function.bank) * 0x4000u +
                     (function.entry_address - 0x4000u);
    } else {
        return false;
    }
    if (rom_offset >= rom_size) return false;

    const uint32_t entry =
        (static_cast<uint32_t>(function.bank) << 16u) | function.entry_address;

    /*
     * Aggressive analysis can create additional strong entry wrappers inside
     * one connected generated body. In that case function.block_ids contains
     * only the entry fragment even though the emitted body reaches its RET.
     *
     * Metadata queries this predicate for every discovered wrapper, so build
     * one reverse-reachability index per IR program instead of traversing the
     * graph per function. Thread-local storage keeps parallel multi-ROM
     * generation isolated.
     */
    struct ReturnReachability {
        const ir::Program* program = nullptr;
        size_t block_count = 0;
        std::set<uint32_t> reaches_ret;
        std::set<uint32_t> reaches_reti;
    };
    static thread_local ReturnReachability cache;
    if (cache.program != &program || cache.block_count != program.blocks.size()) {
        cache = ReturnReachability{};
        cache.program = &program;
        cache.block_count = program.blocks.size();

        std::set<uint32_t> addresses;
        std::map<uint32_t, std::vector<uint32_t>> predecessors;
        std::vector<uint32_t> ret_blocks;
        std::vector<uint32_t> reti_blocks;
        for (const auto& [block_id, block] : program.blocks) {
            (void)block_id;
            const uint32_t address =
                (static_cast<uint32_t>(block.bank) << 16u) |
                block.start_address;
            addresses.insert(address);
            for (const ir::IRInstruction& instruction : block.instructions) {
                if (instruction.opcode == ir::Opcode::RET ||
                    instruction.opcode == ir::Opcode::RET_CC) {
                    ret_blocks.push_back(address);
                } else if (instruction.opcode == ir::Opcode::RETI) {
                    reti_blocks.push_back(address);
                }
            }
        }
        for (const auto& [block_id, block] : program.blocks) {
            (void)block_id;
            const uint32_t address =
                (static_cast<uint32_t>(block.bank) << 16u) |
                block.start_address;
            for (uint32_t successor : block.successors) {
                if ((successor >> 16u) == block.bank &&
                    addresses.count(successor) != 0u) {
                    predecessors[successor].push_back(address);
                }
            }
        }

        auto mark_reachable = [&predecessors](
                                  std::vector<uint32_t> pending,
                                  std::set<uint32_t>& reachable) {
            while (!pending.empty()) {
                const uint32_t address = pending.back();
                pending.pop_back();
                if (!reachable.insert(address).second) continue;
                auto predecessor_it = predecessors.find(address);
                if (predecessor_it == predecessors.end()) continue;
                pending.insert(
                    pending.end(),
                    predecessor_it->second.begin(),
                    predecessor_it->second.end());
            }
        };
        mark_reachable(std::move(ret_blocks), cache.reaches_ret);
        mark_reachable(std::move(reti_blocks), cache.reaches_reti);
    }
    return cache.reaches_ret.count(entry) != 0u &&
           cache.reaches_reti.count(entry) == 0u;
}

bool load_native_patch_manifest(const std::filesystem::path& manifest_path,
                                const uint8_t* rom_data,
                                size_t rom_size,
                                NativePatchPackage& package,
                                std::string& error) {
    try {
        package = NativePatchPackage{};
        const std::string text = read_text_file(manifest_path);
        const JsonValue root = JsonParser(text).parse();
        if (root.type != JsonValue::Type::Object) {
            throw std::runtime_error("manifest root must be an object");
        }
        require_only_fields(root,
                            {"schema", "version", "patch_id", "rom", "sources", "bindings",
                             "host_configuration"},
                            "manifest");
        if (required_member(root, "schema", JsonValue::Type::String).string !=
            "gbrecomp.native-patch") {
            throw std::runtime_error("unsupported manifest schema");
        }
        if (required_member(root, "version", JsonValue::Type::Number).number != 1u) {
            throw std::runtime_error("unsupported native patch manifest version");
        }

        package.patch_id = required_member(root, "patch_id", JsonValue::Type::String).string;
        if (package.patch_id.empty() || package.patch_id.size() > 128u ||
            !std::all_of(package.patch_id.begin(), package.patch_id.end(), [](unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
            })) {
            throw std::runtime_error(
                "patch_id must contain 1-128 ASCII letters, digits, '.', '_', or '-'");
        }

        const JsonValue& rom = required_member(root, "rom", JsonValue::Type::Object);
        require_only_fields(rom, {"sha256", "size"}, "rom");
        package.rom_sha256 = required_member(rom, "sha256", JsonValue::Type::String).string;
        if (package.rom_sha256.size() != 64u ||
            !std::all_of(package.rom_sha256.begin(), package.rom_sha256.end(), [](unsigned char ch) {
                return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
            })) {
            throw std::runtime_error("rom.sha256 must be 64 lowercase hexadecimal digits");
        }
        const uint64_t declared_size = required_member(rom, "size", JsonValue::Type::Number).number;
        if (declared_size != rom_size) {
            throw std::runtime_error("ROM size mismatch: manifest declares " +
                                     std::to_string(declared_size) + ", input has " +
                                     std::to_string(rom_size));
        }
        package.rom_size = rom_size;
        const std::string actual_sha256 = sha256_hex(rom_data, rom_size);
        if (package.rom_sha256 != actual_sha256) {
            throw std::runtime_error("ROM SHA-256 mismatch: manifest declares " +
                                     package.rom_sha256 + ", input is " + actual_sha256);
        }

        const JsonValue* host_configuration =
            optional_member(root, "host_configuration", JsonValue::Type::Object);
        if (host_configuration != nullptr) {
            require_only_fields(
                *host_configuration,
                {"schema", "version", "policy_id", "offset_limit",
                 "value_minimum", "value_maximum"},
                "host_configuration");
            package.host_configuration_schema = required_member(
                *host_configuration, "schema", JsonValue::Type::String).string;
            const uint64_t host_version = required_member(
                *host_configuration, "version", JsonValue::Type::Number).number;
            package.host_configuration_policy_id = required_member(
                *host_configuration, "policy_id", JsonValue::Type::String).string;
            const uint64_t offset_limit = required_member(
                *host_configuration, "offset_limit", JsonValue::Type::Number).number;
            const uint64_t value_minimum = required_member(
                *host_configuration, "value_minimum", JsonValue::Type::Number).number;
            const uint64_t value_maximum = required_member(
                *host_configuration, "value_maximum", JsonValue::Type::Number).number;
            const auto portable_id = [](const std::string& value) {
                return !value.empty() && value.size() < 64u &&
                    std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                        return (ch >= 'a' && ch <= 'z') ||
                               (ch >= 'A' && ch <= 'Z') ||
                               (ch >= '0' && ch <= '9') || ch == '.' ||
                               ch == '_' || ch == '-';
                    });
            };
            if (host_version > UINT32_MAX || offset_limit > INT32_MAX ||
                value_minimum > UINT32_MAX || value_maximum > UINT32_MAX ||
                !portable_id(package.host_configuration_schema) ||
                !portable_id(package.host_configuration_policy_id) ||
                host_version == 0u || value_minimum == 0u ||
                value_minimum > value_maximum) {
                throw std::runtime_error("invalid host_configuration contract");
            }
            package.host_configuration_version = static_cast<uint32_t>(host_version);
            package.host_configuration_offset_limit =
                static_cast<uint32_t>(offset_limit);
            package.host_configuration_value_minimum =
                static_cast<uint32_t>(value_minimum);
            package.host_configuration_value_maximum =
                static_cast<uint32_t>(value_maximum);
            package.host_configuration_enabled = true;
        }

        const JsonValue& sources = required_member(root, "sources", JsonValue::Type::Array);
        if (sources.array.empty()) throw std::runtime_error("sources must not be empty");
        const std::filesystem::path manifest_dir =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(manifest_path).parent_path());
        std::set<std::string> source_paths;
        for (const JsonValue& source : sources.array) {
            if (source.type != JsonValue::Type::String) {
                throw std::runtime_error("every sources entry must be a string");
            }
            const std::filesystem::path relative(source.string);
            if (!path_is_safe_relative(relative)) {
                throw std::runtime_error("native patch source must be a contained relative path: '" +
                                         source.string + "'");
            }
            if (!path_has_portable_source_name(relative)) {
                throw std::runtime_error("native patch source must use a portable path name: '" +
                                         source.string + "'");
            }
            std::string extension = relative.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const bool is_translation_unit =
                extension == ".c" || extension == ".cc" ||
                extension == ".cpp" || extension == ".cxx";
            const bool is_header =
                extension == ".h" || extension == ".hh" ||
                extension == ".hpp" || extension == ".hxx";
            if (!is_translation_unit && !is_header) {
                throw std::runtime_error("unsupported native patch source extension: '" +
                                         source.string + "'");
            }
            const std::string normalized = relative.generic_string();
            if (!source_paths.insert(normalized).second) {
                throw std::runtime_error("duplicate native patch source '" + normalized + "'");
            }
            const std::filesystem::path source_path = manifest_dir / relative;
            if (!std::filesystem::is_regular_file(source_path)) {
                throw std::runtime_error("native patch source does not exist: '" +
                                         source_path.string() + "'");
            }
            const std::filesystem::path canonical_source =
                std::filesystem::canonical(source_path);
            if (!path_is_within(manifest_dir, canonical_source)) {
                throw std::runtime_error("native patch source resolves outside its package: '" +
                                         source.string + "'");
            }
            package.sources.push_back(
                {normalized, read_text_file(canonical_source), is_translation_unit});
        }

        const JsonValue& bindings = required_member(root, "bindings", JsonValue::Type::Array);
        if (bindings.array.empty()) throw std::runtime_error("bindings must not be empty");
        std::set<std::string> function_ids;
        for (const JsonValue& item : bindings.array) {
            if (item.type != JsonValue::Type::Object) {
                throw std::runtime_error("every bindings entry must be an object");
            }
            require_only_fields(
                item,
                {"function", "pre", "replace", "post", "entry_contract"},
                "binding");
            NativePatchBinding binding = parse_function_id(
                required_member(item, "function", JsonValue::Type::String).string);
            const JsonValue* pre = optional_member(item, "pre", JsonValue::Type::String);
            const JsonValue* replace = optional_member(item, "replace", JsonValue::Type::String);
            const JsonValue* post = optional_member(item, "post", JsonValue::Type::String);
            const JsonValue* entry_contract =
                optional_member(item, "entry_contract", JsonValue::Type::String);
            binding.pre_callback = pre ? pre->string : "";
            binding.replace_callback = replace ? replace->string : "";
            binding.post_callback = post ? post->string : "";
            if (entry_contract != nullptr) {
                if (entry_contract->string != "return-stack") {
                    throw std::runtime_error(
                        "binding entry_contract must be 'return-stack'");
                }
                binding.allow_return_stack_entry = true;
            }
            if (binding.pre_callback.empty() && binding.replace_callback.empty() &&
                binding.post_callback.empty()) {
                throw std::runtime_error("binding '" + binding.function_id +
                                         "' must declare pre, replace, or post");
            }
            for (const auto& [role, callback] :
                 std::array<std::pair<const char*, const std::string*>, 3>{
                     std::pair{"pre", &binding.pre_callback},
                     std::pair{"replace", &binding.replace_callback},
                     std::pair{"post", &binding.post_callback}}) {
                if (!callback->empty() && !is_c_identifier(*callback)) {
                    throw std::runtime_error(std::string("invalid ") + role +
                                             " callback identifier '" + *callback + "'");
                }
            }
            if (!function_ids.insert(binding.function_id).second) {
                throw std::runtime_error("duplicate binding for '" + binding.function_id + "'");
            }
            package.bindings.push_back(std::move(binding));
        }

        package.enabled = true;
        package.manifest_json = normalized_manifest(package);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        package = NativePatchPackage{};
        error = exception.what();
        return false;
    }
}

bool validate_native_patch_bindings(const NativePatchPackage& package,
                                    const ir::Program& program,
                                    std::string& error) {
    if (!package.enabled) {
        error.clear();
        return true;
    }
    for (const NativePatchBinding& binding : package.bindings) {
        const ir::Function* match = nullptr;
        for (const auto& [name, function] : program.functions) {
            (void)name;
            if (function.bank == binding.bank && function.entry_address == binding.address) {
                if (match != nullptr) {
                    error = "native patch function ID is ambiguous: '" + binding.function_id + "'";
                    return false;
                }
                match = &function;
            }
        }
        if (match == nullptr) {
            error = "native patch function was not discovered: '" + binding.function_id + "'";
            return false;
        }
        if (!is_native_patchable_function(program, *match, package.rom_size)) {
            error = "native patch function is not a ROM-backed returning function: '" +
                    binding.function_id + "'";
            return false;
        }
    }
    error.clear();
    return true;
}

/* ---------------------------------------------------------------------------
 * Lift packages (lift_package.h). They live in this file because they are
 * read the same way as native patch packages: the same JSON reader and the
 * same rules for what a package may name as a source.
 * ------------------------------------------------------------------------- */

bool load_lift_package(const std::filesystem::path& manifest_path,
                       const uint8_t* rom_data,
                       size_t rom_size,
                       LiftPackage& package,
                       std::string& error) {
    try {
        package = LiftPackage{};
        const std::string text = read_text_file(manifest_path);
        const JsonValue root = JsonParser(text).parse();
        if (root.type != JsonValue::Type::Object) {
            throw std::runtime_error("manifest root must be an object");
        }
        require_only_fields(root, {"schema", "version", "rom", "sources", "functions"},
                            "manifest");
        if (required_member(root, "schema", JsonValue::Type::String).string !=
            "gbrecomp.lift-package") {
            throw std::runtime_error("unsupported manifest schema");
        }
        if (required_member(root, "version", JsonValue::Type::Number).number != 1u) {
            throw std::runtime_error("unsupported lift package version");
        }

        const JsonValue& rom = required_member(root, "rom", JsonValue::Type::Object);
        require_only_fields(rom, {"sha256", "size"}, "rom");
        package.rom_sha256 = required_member(rom, "sha256", JsonValue::Type::String).string;
        const uint64_t declared_size = required_member(rom, "size", JsonValue::Type::Number).number;
        if (declared_size != rom_size) {
            throw std::runtime_error("ROM size mismatch: package is for " +
                                     std::to_string(declared_size) + " bytes, input has " +
                                     std::to_string(rom_size));
        }
        package.rom_size = rom_size;
        const std::string actual_sha256 = sha256_hex(rom_data, rom_size);
        if (package.rom_sha256 != actual_sha256) {
            throw std::runtime_error("ROM SHA-256 mismatch: package is for " +
                                     package.rom_sha256 + ", input is " + actual_sha256);
        }

        const std::filesystem::path manifest_dir =
            std::filesystem::canonical(manifest_path).parent_path();
        const JsonValue& sources = required_member(root, "sources", JsonValue::Type::Array);
        if (sources.array.empty()) throw std::runtime_error("sources must not be empty");
        std::set<std::string> source_paths;
        for (const JsonValue& source : sources.array) {
            if (source.type != JsonValue::Type::String) {
                throw std::runtime_error("every sources entry must be a string");
            }
            const std::filesystem::path relative(source.string);
            if (!path_is_safe_relative(relative) || !path_has_portable_source_name(relative)) {
                throw std::runtime_error(
                    "lift source must be a contained relative path with a portable name: '" +
                    source.string + "'");
            }
            std::string extension = relative.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const bool is_translation_unit = extension == ".c";
            if (!is_translation_unit && extension != ".h") {
                throw std::runtime_error("lift sources are .c and .h files: '" + source.string + "'");
            }
            const std::string normalized = relative.generic_string();
            if (!source_paths.insert(normalized).second) {
                throw std::runtime_error("duplicate lift source '" + normalized + "'");
            }
            const std::filesystem::path source_path = manifest_dir / relative;
            if (!std::filesystem::is_regular_file(source_path)) {
                throw std::runtime_error("lift source does not exist: '" + source_path.string() + "'");
            }
            const std::filesystem::path canonical_source = std::filesystem::canonical(source_path);
            if (!path_is_within(manifest_dir, canonical_source)) {
                throw std::runtime_error("lift source resolves outside its package: '" +
                                         source.string + "'");
            }
            package.sources.push_back(
                {normalized, read_text_file(canonical_source), is_translation_unit});
        }

        const JsonValue& functions = required_member(root, "functions", JsonValue::Type::Array);
        if (functions.array.empty()) throw std::runtime_error("functions must not be empty");
        std::set<std::string> function_ids;
        std::set<std::string> symbols;
        for (const JsonValue& item : functions.array) {
            if (item.type != JsonValue::Type::Object) {
                throw std::runtime_error("every functions entry must be an object");
            }
            require_only_fields(item, {"id", "symbol"}, "function");
            const NativePatchBinding where = parse_function_id(
                required_member(item, "id", JsonValue::Type::String).string);
            LiftBinding binding;
            binding.bank = where.bank;
            binding.address = where.address;
            binding.function_id = where.function_id;
            binding.symbol = required_member(item, "symbol", JsonValue::Type::String).string;
            if (!is_c_identifier(binding.symbol)) {
                throw std::runtime_error("invalid C function name '" + binding.symbol + "'");
            }
            if (binding.address >= 0x8000u) {
                throw std::runtime_error("lifted function is not in the ROM: '" +
                                         binding.function_id + "'");
            }
            if (!function_ids.insert(binding.function_id).second) {
                throw std::runtime_error("'" + binding.function_id + "' is lifted twice");
            }
            if (!symbols.insert(binding.symbol).second) {
                throw std::runtime_error("C function '" + binding.symbol +
                                         "' is bound to two subroutines");
            }
            package.bindings.push_back(std::move(binding));
        }

        package.enabled = true;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        package = LiftPackage{};
        error = exception.what();
        return false;
    }
}

bool load_lift_edges(const std::filesystem::path& path,
                     const uint8_t* rom_data,
                     size_t rom_size,
                     std::vector<LiftFarEdge>& edges,
                     std::vector<uint32_t>& entered,
                     std::vector<LiftTableEntry>& table_entries,
                     std::string& error) {
    try {
        edges.clear();
        entered.clear();
        table_entries.clear();
        const JsonValue root = JsonParser(read_text_file(path)).parse();
        if (root.type != JsonValue::Type::Object) {
            throw std::runtime_error("the root must be an object");
        }
        require_only_fields(root, {"schema", "version", "rom", "edges", "entered", "declared_tables"},
                            "lift edges");
        if (required_member(root, "schema", JsonValue::Type::String).string != "gbrecomp.lift-edges") {
            throw std::runtime_error("not a gbrecomp.lift-edges file");
        }
        if (required_member(root, "version", JsonValue::Type::Number).number != 1u) {
            throw std::runtime_error("unsupported lift edges version");
        }
        const JsonValue& rom = required_member(root, "rom", JsonValue::Type::Object);
        require_only_fields(rom, {"sha256", "size"}, "rom");
        if (required_member(rom, "size", JsonValue::Type::Number).number != rom_size ||
            required_member(rom, "sha256", JsonValue::Type::String).string !=
                sha256_hex(rom_data, rom_size)) {
            throw std::runtime_error("these edges were found in another ROM");
        }
        const uint64_t bank_count = (rom_size + 0x3FFFu) / 0x4000u;
        for (const JsonValue& item : required_member(root, "edges", JsonValue::Type::Array).array) {
            if (item.type != JsonValue::Type::Object) {
                throw std::runtime_error("every edges entry must be an object");
            }
            require_only_fields(item, {"from", "site", "kind", "helper", "known", "bank", "address",
                                       "returns_to", "left_at", "table", "seen"}, "edge");
            const std::string& kind = required_member(item, "kind", JsonValue::Type::String).string;
            if (kind != "call" && kind != "jump" && kind != "computed") {
                throw std::runtime_error("an edge is a call, a jump or a computed jump, not '" + kind + "'");
            }
            // A computed jump has no helper, and may be in code the analysis
            // does not have, in which case it is in no function either.
            NativePatchBinding from;
            NativePatchBinding helper;
            const auto from_value = item.object.find("from");
            const bool has_from = from_value != item.object.end() &&
                                  from_value->second.type == JsonValue::Type::String;
            if (has_from) {
                from = parse_function_id(from_value->second.string);
            } else if (kind != "computed" || from_value == item.object.end() ||
                       from_value->second.type != JsonValue::Type::Null) {
                throw std::runtime_error("an edge needs the function it is made from");
            }
            if (kind != "computed") {
                helper = parse_function_id(required_member(item, "helper", JsonValue::Type::String).string);
            }
            const uint64_t site = required_member(item, "site", JsonValue::Type::Number).number;
            const uint64_t bank = required_member(item, "bank", JsonValue::Type::Number).number;
            const uint64_t address = required_member(item, "address", JsonValue::Type::Number).number;
            const uint64_t returns_to =
                required_member(item, "returns_to", JsonValue::Type::Number).number;
            // A target is code in the ROM: the home bank below 0x4000, a
            // bank the cartridge has above it.
            if (site > 0xFFFFu || returns_to > 0xFFFFu || address >= 0x8000u || bank >= bank_count ||
                (address < 0x4000u) != (bank == 0)) {
                throw std::runtime_error("an edge from '" + from.function_id +
                                         "' goes somewhere that is not in the ROM");
            }
            LiftFarEdge edge;
            edge.from = has_from ? AnalysisResult::make_addr(from.bank, from.address) : 0;
            edge.site = static_cast<uint16_t>(site);
            edge.is_jump = kind == "jump";
            edge.is_computed = kind == "computed";
            edge.helper = edge.is_computed ? 0 : AnalysisResult::make_addr(helper.bank, helper.address);
            if (const JsonValue* seen = optional_member(item, "seen", JsonValue::Type::Bool)) {
                edge.seen = seen->boolean;
            }
            if (edge.is_computed && !edge.seen) {
                throw std::runtime_error("a computed jump's target is only known from a run");
            }
            if (!has_from) edge.site = 0;
            edge.target = AnalysisResult::make_addr(static_cast<BankId>(bank),
                                                    static_cast<uint16_t>(address));
            edge.returns_to = static_cast<uint16_t>(returns_to);
            if (const JsonValue* left_at = optional_member(item, "left_at", JsonValue::Type::Number)) {
                if (left_at->number > 0xFFFFu) {
                    throw std::runtime_error("an edge from '" + from.function_id +
                                             "' leaves its helper somewhere that is not an address");
                }
                edge.left_at = static_cast<uint16_t>(left_at->number);
            }
            if (const JsonValue* table = optional_member(item, "table", JsonValue::Type::Object)) {
                require_only_fields(*table, {"bank", "address", "index"}, "table");
                const uint64_t table_bank = required_member(*table, "bank", JsonValue::Type::Number).number;
                const uint64_t table_address =
                    required_member(*table, "address", JsonValue::Type::Number).number;
                const uint64_t index = required_member(*table, "index", JsonValue::Type::Number).number;
                if (table_address >= 0x8000u || table_bank >= bank_count || index > 0xFFFFu ||
                    (table_address < 0x4000u) != (table_bank == 0)) {
                    throw std::runtime_error("an edge from '" + from.function_id +
                                             "' goes through a table that is not in the ROM");
                }
                edge.from_table = true;
                edge.table = AnalysisResult::make_addr(static_cast<BankId>(table_bank),
                                                       static_cast<uint16_t>(table_address));
                edge.table_index = static_cast<uint16_t>(index);
            }
            edges.push_back(edge);
        }
        if (const JsonValue* list = optional_member(root, "entered", JsonValue::Type::Array)) {
            for (const JsonValue& item : list->array) {
                if (item.type != JsonValue::Type::Array || item.array.size() != 2 ||
                    item.array[0].type != JsonValue::Type::Number ||
                    item.array[1].type != JsonValue::Type::Number ||
                    item.array[0].number >= bank_count || item.array[1].number >= 0x8000u) {
                    throw std::runtime_error("every 'entered' entry is a [bank, address] in the ROM");
                }
                entered.push_back(AnalysisResult::make_addr(static_cast<BankId>(item.array[0].number),
                                                            static_cast<uint16_t>(item.array[1].number)));
            }
        }
        // Tables somebody declared, each with the functions it names.
        if (const JsonValue* list = optional_member(root, "declared_tables", JsonValue::Type::Array)) {
            for (const JsonValue& table : list->array) {
                if (table.type != JsonValue::Type::Object) {
                    throw std::runtime_error("every declared table must be an object");
                }
                require_only_fields(table, {"name", "bank", "address", "entries"}, "declared table");
                const std::string& name = required_member(table, "name", JsonValue::Type::String).string;
                const uint64_t table_bank = required_member(table, "bank", JsonValue::Type::Number).number;
                const uint64_t table_address =
                    required_member(table, "address", JsonValue::Type::Number).number;
                if (table_address >= 0x8000u || table_bank >= bank_count ||
                    (table_address < 0x4000u) != (table_bank == 0)) {
                    throw std::runtime_error("the declared table '" + name + "' is not in the ROM");
                }
                for (const JsonValue& item : required_member(table, "entries", JsonValue::Type::Array).array) {
                    if (item.type != JsonValue::Type::Object) {
                        throw std::runtime_error("every entry of a declared table must be an object");
                    }
                    require_only_fields(item, {"index", "bank", "address"}, "entry of a declared table");
                    const uint64_t index = required_member(item, "index", JsonValue::Type::Number).number;
                    const uint64_t bank = required_member(item, "bank", JsonValue::Type::Number).number;
                    const uint64_t address = required_member(item, "address", JsonValue::Type::Number).number;
                    if (index > 0xFFFFu || address >= 0x8000u || bank >= bank_count ||
                        (address < 0x4000u) != (bank == 0)) {
                        throw std::runtime_error("an entry of the declared table '" + name +
                                                 "' is not a function in the ROM");
                    }
                    LiftTableEntry entry;
                    entry.target = AnalysisResult::make_addr(static_cast<BankId>(bank),
                                                             static_cast<uint16_t>(address));
                    entry.table = AnalysisResult::make_addr(static_cast<BankId>(table_bank),
                                                            static_cast<uint16_t>(table_address));
                    entry.index = static_cast<uint16_t>(index);
                    entry.name = name;
                    table_entries.push_back(entry);
                }
            }
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        edges.clear();
        entered.clear();
        table_entries.clear();
        error = exception.what();
        return false;
    }
}

bool validate_lift_bindings(const LiftPackage& package,
                            const ir::Program& program,
                            std::string& error) {
    if (!package.enabled) {
        error.clear();
        return true;
    }
    std::set<uint32_t> block_starts;
    for (const auto& [block_id, block] : program.blocks) {
        (void)block_id;
        block_starts.insert((static_cast<uint32_t>(block.bank) << 16u) | block.start_address);
    }
    for (const LiftBinding& binding : package.bindings) {
        const uint32_t address = (static_cast<uint32_t>(binding.bank) << 16u) | binding.address;
        if (block_starts.count(address) == 0u) {
            error = "no recompiled block starts at lifted function '" + binding.function_id +
                    "' (" + binding.symbol + "); it has to be code this analysis found";
            return false;
        }
    }
    error.clear();
    return true;
}

} // namespace gbrecomp
