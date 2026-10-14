/*
 * The DS ROM front end of ndsrecomp (recompilers/recompiler-nds/src): BLZ decompression,
 * CRC-16 and FxHash, the cartridge header and file system, the code modules,
 * and the reader and checker for ds-decomp symbols and relocations.
 *
 * Nothing here needs a commercial ROM. The ROM these tests parse is built in
 * memory: a compressed ARM9 program with module params and two autoload
 * blocks, an ARM7 program with one, a compressed and a plain overlay, and a
 * small file system.
 *
 * Where the expected values come from:
 *
 *   BLZ      Two streams assembled by hand from the format, and round trips
 *            through the compressor below, which is written from the format
 *            alone and shares no code with blz.cpp. So the decompressor is
 *            never checked against data it produced itself.
 *   CRC-16   The published check values of CRC-16/MODBUS and CRC-16/ARC.
 *   FxHash   Values printed by the Rust fxhash crate 0.2.1 itself
 *            (fxhash::hash64 of a byte slice), which is what ds-decomp runs.
 *   dsd      Lines taken from ds-decomp's docs and from the Phantom
 *            Hourglass decompilation's configuration (zeldaret/ph, CC0).
 *
 * The mutation loops at the end of the BLZ and ROM sections feed damaged
 * input to the parsers; under AddressSanitizer they are the memory-safety
 * test. A parser may refuse such input or accept it, but may not read or
 * write out of bounds, and what it accepts must be self-consistent.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "blz.h"
#include "check.h"
#include "modules.h"
#include "rom.h"
#include "symbols.h"

using namespace ndsrecomp;

static int g_checks;
static int g_failures;

static void check(bool ok, const char *what, int line)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL line %d: %s\n", line, what);
}

static void check_eq(uint64_t got, uint64_t want, const char *what, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    std::printf("FAIL line %d: %s is 0x%llx, expected 0x%llx\n", line, what, static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
}

static void check_str(const std::string &got, const std::string &want, const char *what, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    std::printf("FAIL line %d: %s is \"%s\", expected \"%s\"\n", line, what, got.c_str(), want.c_str());
}

// A call that must fail, with a message that mentions `part`.
static void check_error(bool result, const std::string &error, const char *part, const char *what, int line)
{
    g_checks++;
    if (!result && error.find(part) != std::string::npos) return;
    g_failures++;
    if (result) std::printf("FAIL line %d: %s succeeded, expected an error about \"%s\"\n", line, what, part);
    else std::printf("FAIL line %d: %s failed with \"%s\", expected an error about \"%s\"\n", line, what, error.c_str(), part);
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(got, want) check_eq(static_cast<uint64_t>(got), static_cast<uint64_t>(want), #got, __LINE__)
#define CHECK_STR(got, want) check_str((got), (want), #got, __LINE__)
#define CHECK_ERROR(result, error, part) check_error((result), (error), (part), #result, __LINE__)

// Deterministic pseudo-random numbers (a linear congruential generator).
static uint32_t g_random = 1;
static uint32_t random_below(uint32_t limit)
{
    g_random = g_random * 1664525u + 1013904223u;
    return (g_random >> 8) % limit;
}

static void put16(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

static void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
    put16(bytes, at, value & 0xFFFFu);
    put16(bytes, at + 2, value >> 16);
}

static void append(std::vector<uint8_t> &to, const std::vector<uint8_t> &bytes)
{
    to.insert(to.end(), bytes.begin(), bytes.end());
}

// Bytes that compress the way code and tables do: runs, repeated words and
// phrases, with enough noise that literals and pairs both occur.
static std::vector<uint8_t> sample_data(size_t size, uint32_t seed)
{
    static const char *const phrases[] = {"overlay", "\xE5\x9F\x10\x04", "\x00\x00\x00\x00\x00\x00\x00\x00", "Nitro",
                                          "\xE1\x2F\xFF\x1E", "abcabcabcabc"};
    g_random = seed;
    std::vector<uint8_t> out;
    while (out.size() < size) {
        const uint32_t choice = random_below(10);
        if (choice < 6) {
            const char *phrase = phrases[choice];
            const size_t length = choice == 2 ? 8 : std::strlen(phrase);
            for (size_t i = 0; i < length; i++) out.push_back(static_cast<uint8_t>(phrase[i]));
        } else if (choice < 8) {
            const uint8_t value = static_cast<uint8_t>(random_below(256));
            for (uint32_t i = random_below(40) + 1; i > 0; i--) out.push_back(value);
        } else {
            for (uint32_t i = random_below(6) + 1; i > 0; i--) out.push_back(static_cast<uint8_t>(random_below(256)));
        }
    }
    out.resize(size);
    return out;
}

static std::vector<uint8_t> random_bytes(size_t size, uint32_t seed)
{
    g_random = seed;
    std::vector<uint8_t> out(size);
    for (uint8_t &byte : out) byte = static_cast<uint8_t>(random_below(256));
    return out;
}

/* ------------------------------------------------------------------ */
/* A BLZ compressor, for the tests only                                */
/* ------------------------------------------------------------------ */

// Compresses plain[start..] and stores plain[..start] as it is. Greedy: at
// each position, working down from the end, the longest match among the bytes
// above it (3 to 18 bytes, 3 to 0x1002 bytes away) or a literal. The stream
// is then cut where it has saved the most, which is also what keeps the
// output from overtaking the input when the console decompresses in place;
// the bytes below the cut are stored. Returns nothing if compression does not
// even pay for the footer.
static std::vector<uint8_t> blz_compress(const std::vector<uint8_t> &plain, size_t start)
{
    struct Token {
        bool pair;
        uint8_t literal;
        unsigned length;
        unsigned distance;
    };
    const size_t size = plain.size();
    std::vector<Token> tokens;
    size_t at = size; // plain[at..] is encoded so far
    while (at > start) {
        unsigned best_length = 0;
        unsigned best_distance = 0;
        for (unsigned distance = 3; distance <= 0x1002 && at - 1 + distance < size && best_length < 18; distance++) {
            unsigned length = 0;
            while (length < 18 && at - length > start && plain[at - 1 - length] == plain[at - 1 - length + distance]) {
                length++;
            }
            if (length > best_length) {
                best_length = length;
                best_distance = distance;
            }
        }
        if (best_length >= 3) {
            tokens.push_back({true, 0, best_length, best_distance});
            at -= best_length;
        } else {
            tokens.push_back({false, plain[at - 1], 0, 0});
            at--;
        }
    }

    long saved = 0;
    long best_saved = 0;
    size_t count = 0; // tokens kept
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i % 8 == 0) saved -= 1; // the flag byte
        if (tokens[i].pair) saved += static_cast<long>(tokens[i].length) - 2;
        if (saved > best_saved) {
            best_saved = saved;
            count = i + 1;
        }
    }

    std::vector<uint8_t> stream; // in the order the decompressor reads it
    size_t covered = 0;
    for (size_t group = 0; group < count; group += 8) {
        uint8_t flags = 0;
        for (size_t i = group; i < group + 8 && i < count; i++) {
            if (tokens[i].pair) flags = static_cast<uint8_t>(flags | (0x80u >> (i - group)));
        }
        stream.push_back(flags);
        for (size_t i = group; i < group + 8 && i < count; i++) {
            const Token &token = tokens[i];
            if (token.pair) {
                const unsigned offset = token.distance - 3;
                stream.push_back(static_cast<uint8_t>(((token.length - 3) << 4) | (offset >> 8)));
                stream.push_back(static_cast<uint8_t>(offset));
                covered += token.length;
            } else {
                stream.push_back(token.literal);
                covered += 1;
            }
        }
    }

    const size_t stored = size - covered;
    std::vector<uint8_t> out(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(stored));
    out.insert(out.end(), stream.rbegin(), stream.rend());
    size_t footer_size = 8;
    while (out.size() % 4 != 0) {
        out.push_back(0xFF);
        footer_size++;
    }
    const size_t total = out.size() + 8;
    if (total > size) return {};
    const size_t span = total - stored;
    out.push_back(static_cast<uint8_t>(span));
    out.push_back(static_cast<uint8_t>(span >> 8));
    out.push_back(static_cast<uint8_t>(span >> 16));
    out.push_back(static_cast<uint8_t>(footer_size));
    out.resize(total);
    put32(out, total - 4, static_cast<uint32_t>(size - total));
    return out;
}

static bool decompress(const std::vector<uint8_t> &compressed, std::vector<uint8_t> &out, std::string &error)
{
    return blz_decompress(compressed.data(), compressed.size(), out, error);
}

static void test_blz()
{
    std::vector<uint8_t> out;
    std::string error;

    // "ABCDABCDABCDABCD": four literals (D C B A, read downwards) and a pair
    // that copies 12 bytes from 4 above, in one group with flags 00001000.
    // Stream as read: 08 44 43 42 41 90 01; stored reversed, then one byte
    // of padding and the footer (region 16 bytes, footer 9, nothing extra).
    const std::vector<uint8_t> hand1 = {0x01, 0x90, 0x41, 0x42, 0x43, 0x44, 0x08, 0xFF,
                                        0x10, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
    CHECK(decompress(hand1, out, error));
    CHECK_STR(std::string(out.begin(), out.end()), "ABCDABCDABCDABCD");

    // "XY" stored, then twenty 'a': three literals and a pair of 17 bytes
    // from 3 above (it reads its own output). Region 14 bytes, 6 extra.
    const std::vector<uint8_t> hand2 = {0x58, 0x59, 0x00, 0xE0, 0x61, 0x61, 0x61, 0x10,
                                        0x0E, 0x00, 0x00, 0x08, 0x06, 0x00, 0x00, 0x00};
    CHECK(decompress(hand2, out, error));
    CHECK_STR(std::string(out.begin(), out.end()), "XYaaaaaaaaaaaaaaaaaaaa");

    BlzFooter footer;
    CHECK(blz_read_footer(hand2.data(), hand2.size(), footer, error));
    CHECK_EQ(footer.compressed_span, 14);
    CHECK_EQ(footer.footer_size, 8);
    CHECK_EQ(footer.extra_size, 6);

    // Round trips through the independent compressor: several kinds of
    // data, sizes that leave every amount of padding, with and without a
    // stored prefix.
    struct Case {
        size_t size;
        size_t start;
        uint32_t seed;
    };
    const Case cases[] = {{64, 0, 1},    {65, 0, 2},     {66, 0, 3},      {67, 0, 4},     {300, 0, 5},
                          {1000, 0, 6},  {1000, 257, 7}, {4096, 0, 8},    {5000, 2048, 9}, {9001, 1, 10},
                          {9000, 0, 11}, {777, 700, 12}, {2500, 0, 13}};
    size_t round_trips = 0;
    size_t paddings_seen = 0;
    for (const Case &c : cases) {
        const std::vector<uint8_t> plain = sample_data(c.size, c.seed);
        const std::vector<uint8_t> compressed = blz_compress(plain, c.start);
        if (compressed.empty() || !decompress(compressed, out, error) || out != plain) {
            std::printf("FAIL: BLZ round trip of %zu bytes (start %zu): %s\n", c.size, c.start,
                        compressed.empty() ? "did not compress" : error.c_str());
            g_failures++;
            continue;
        }
        round_trips++;
        if (blz_read_footer(compressed.data(), compressed.size(), footer, error)) {
            paddings_seen |= static_cast<size_t>(1) << (footer.footer_size - 8);
            if (compressed.size() - footer.compressed_span < c.start) {
                std::printf("FAIL: the compressed region reaches below the stored prefix\n");
                g_failures++;
            }
        }
    }
    CHECK_EQ(round_trips, sizeof cases / sizeof cases[0]);
    CHECK_EQ(paddings_seen, 0xF); // footers of 8, 9, 10 and 11 bytes all occurred

    // A run of one byte is the densest stream there is (pairs of 18 from 3
    // above); the output-size limit in the footer check must still admit it.
    const std::vector<uint8_t> zeros(20000, 0);
    const std::vector<uint8_t> zeros_compressed = blz_compress(zeros, 0);
    CHECK(!zeros_compressed.empty() && zeros_compressed.size() < 2400);
    CHECK(decompress(zeros_compressed, out, error) && out == zeros);

    // Data that does not compress at all is not a BLZ stream (the tools
    // store it plain); the test compressor says so.
    CHECK(blz_compress(random_bytes(512, 99), 0).empty());

    // Compressible data above incompressible data: the cut leaves the
    // incompressible part stored.
    std::vector<uint8_t> mixed = random_bytes(600, 21);
    append(mixed, sample_data(1500, 22));
    const std::vector<uint8_t> mixed_compressed = blz_compress(mixed, 0);
    CHECK(!mixed_compressed.empty());
    CHECK(decompress(mixed_compressed, out, error) && out == mixed);
    CHECK(blz_read_footer(mixed_compressed.data(), mixed_compressed.size(), footer, error) &&
          mixed_compressed.size() - footer.compressed_span >= 500);
    // The other way round: the stream opens with a long run of literals.
    mixed = sample_data(3000, 23);
    append(mixed, random_bytes(300, 24));
    const std::vector<uint8_t> literals_first = blz_compress(mixed, 0);
    CHECK(!literals_first.empty());
    CHECK(decompress(literals_first, out, error) && out == mixed);

    // Damaged streams, each refused for its own reason.
    CHECK_ERROR(blz_decompress(hand1.data(), 7, out, error), error, "too short");
    std::vector<uint8_t> bad = hand1;
    bad[11] = 7; // footer smaller than itself
    CHECK_ERROR(decompress(bad, out, error), error, "smaller than the footer");
    bad = hand1;
    bad[11] = 17; // footer larger than the region
    CHECK_ERROR(decompress(bad, out, error), error, "exceeds the compressed region");
    bad = hand1;
    bad[8] = 0x11; // region larger than the file
    CHECK_ERROR(decompress(bad, out, error), error, "larger than the file");
    bad = hand1;
    put32(bad, 12, 0x7FFFFFFF); // more output than the stream can hold: refused before allocating
    CHECK_ERROR(decompress(bad, out, error), error, "more than");
    bad = hand1;
    put32(bad, 12, 4); // four bytes more than the stream produces
    CHECK_ERROR(decompress(bad, out, error), error, "promised");
    bad = hand1;
    bad[6] = 0x88; // the first token is now a pair: nothing above it to copy from
    CHECK_ERROR(decompress(bad, out, error), error, "past the end of the output");
    bad = hand2;
    bad[2] = 0x01; // the pair now copies from 4 above with 3 bytes written (hand2 itself is the limit: 3 from 3)
    CHECK_ERROR(decompress(bad, out, error), error, "past the end of the output");
    bad = hand1;
    bad[6] = 0x0C; // a flag bit set beyond the last token is never looked at
    CHECK(decompress(bad, out, error) && std::string(out.begin(), out.end()) == "ABCDABCDABCDABCD");
    // A literal, then a pair with one byte left for it.
    const std::vector<uint8_t> cut_pair = {0x00, 0x41, 0x40, 0xFF, 0x0C, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00};
    CHECK_ERROR(decompress(cut_pair, out, error), error, "inside a length-distance pair");
    // Output that would overwrite input not yet read. The top of the data
    // (93 zero bytes) takes 14 bytes of stream, and below it come 16
    // literals: the fifth pair would write over where those literals still
    // sit. Every length in it is right, so only the in-place rule refuses it
    // (a compressor avoids this by storing the 16 bytes instead).
    std::vector<uint8_t> as_read = {0x1F, 0, 0, 0};
    for (int i = 0; i < 5; i++) {
        as_read.push_back(0xF0);
        as_read.push_back(0x00);
    }
    for (int group = 0; group < 2; group++) {
        as_read.push_back(0x00);
        for (int i = 0; i < 8; i++) as_read.push_back(static_cast<uint8_t>('A' + i));
    }
    std::vector<uint8_t> overtaking(as_read.rbegin(), as_read.rend());
    append(overtaking, {40, 0, 0, 8, 69, 0, 0, 0});
    CHECK_ERROR(decompress(overtaking, out, error), error, "overtakes");
    // A flag byte with nothing after it.
    const std::vector<uint8_t> dangling = {0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00};
    CHECK_ERROR(decompress(dangling, out, error), error, "ends on a flag byte");

    // Mutations of a valid stream: any single byte changed, any length cut.
    // Nothing may be read or written out of bounds, and whatever is accepted
    // has the size the footer gives.
    const std::vector<uint8_t> plain = sample_data(600, 31);
    const std::vector<uint8_t> valid = blz_compress(plain, 16);
    CHECK(!valid.empty());
    size_t inconsistent = 0;
    size_t accepted = 0;
    g_random = 77;
    for (int i = 0; i < 30000; i++) {
        std::vector<uint8_t> mutated = valid;
        const uint32_t kind = random_below(8);
        if (kind == 0) {
            mutated.resize(random_below(static_cast<uint32_t>(valid.size()) + 1));
        } else if (kind == 1) {
            // the footer, which holds every length
            mutated[mutated.size() - 1 - random_below(8)] = static_cast<uint8_t>(random_below(256));
        } else {
            for (uint32_t n = random_below(3) + 1; n > 0; n--) {
                mutated[random_below(static_cast<uint32_t>(mutated.size()))] = static_cast<uint8_t>(random_below(256));
            }
        }
        if (!blz_decompress(mutated.data(), mutated.size(), out, error)) continue;
        accepted++;
        if (!blz_read_footer(mutated.data(), mutated.size(), footer, error) ||
            out.size() != mutated.size() + footer.extra_size) {
            inconsistent++;
        }
    }
    std::printf("BLZ: %zu of 30000 damaged streams still decompress\n", accepted);
    CHECK_EQ(inconsistent, 0);
    CHECK(accepted > 100); // mutations that only change literals: the loop does reach the decoder's end
}

/* ------------------------------------------------------------------ */
/* CRC-16 and FxHash                                                   */
/* ------------------------------------------------------------------ */

static void test_hashes()
{
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc16(digits, sizeof digits), 0x4B37);    // CRC-16/MODBUS check value
    CHECK_EQ(crc16(digits, sizeof digits, 0), 0xBB3D); // CRC-16/ARC check value
    CHECK_EQ(crc16(digits, 0), 0xFFFF);
    // Running it in two parts is the same as in one.
    CHECK_EQ(crc16(digits + 4, 5, crc16(digits, 4)), 0x4B37);

    // fxhash::hash64(&b"abcdefghijklmnopqrstuvwxyz"[..n]), fxhash 0.2.1.
    const uint8_t *letters = reinterpret_cast<const uint8_t *>("abcdefghijklmnopqrstuvwxyz");
    static const struct {
        size_t size;
        uint64_t hash;
    } expected[] = {
        {0, 0x0000000000000000ull},  {1, 0x8e5a628a53371e27ull},  {2, 0xce29b1e157f003d9ull},
        {3, 0xa6a91e60c835047eull},  {4, 0x45b7a40fc000e99dull},  {5, 0xf2b970df090d6f8bull},
        {6, 0xbb976f4a1627a799ull},  {7, 0x0f8826edb75baf28ull},  {8, 0xf05732280c376430ull},
        {9, 0xe8bdb72b5bb693caull},  {15, 0xdd069e8260a74fc3ull}, {16, 0xe5c63c461418eba6ull},
        {26, 0x8b0433b124b693c8ull},
    };
    for (const auto &e : expected) CHECK_EQ(fxhash64(letters, e.size), e.hash);
    const uint8_t zeros[16] = {};
    CHECK_EQ(fxhash64(zeros, sizeof zeros), 0x82b113a2a42ca225ull);

    CHECK_STR(hex(0x2000800), "0x02000800");
    CHECK_STR(hex(0xCF56, 4), "0xcf56");
    CHECK_STR(hex(0x15560e62cc17dc7aull, 16), "0x15560e62cc17dc7a");
}

/* ------------------------------------------------------------------ */
/* A ROM built in memory                                               */
/* ------------------------------------------------------------------ */

constexpr uint32_t kArm9Load = 0x02000000u;
constexpr uint32_t kArm9RomOffset = 0x4000u;
constexpr uint32_t kArm9Params = 0x848u; // offset of the module params in the ARM9 program
constexpr uint32_t kArm9MainSize = 0x1400u;
constexpr uint32_t kArm7Load = 0x02380000u;
constexpr uint32_t kOverlayLoad = 0x02002000u;

struct TestRom {
    std::vector<uint8_t> rom;
    // What the modules must come out as.
    std::vector<uint8_t> arm9_image; // decompressed, with compressed_end as it is in RAM
    std::vector<uint8_t> itcm;
    std::vector<uint8_t> dtcm;
    std::vector<uint8_t> overlay0;
    std::vector<uint8_t> overlay1;
    std::vector<uint8_t> arm7;
    // Where things are, for the tests that damage them.
    size_t arm9_size = 0;
    size_t arm7_offset = 0;
    size_t overlay_table = 0;
    size_t fnt_offset = 0;
    size_t fnt_size = 0;
    size_t fat_offset = 0;
};

static void pad_to(std::vector<uint8_t> &rom, size_t alignment)
{
    while (rom.size() % alignment != 0) rom.push_back(0xFF);
}

static void finish_header(std::vector<uint8_t> &rom)
{
    put16(rom, 0x15C, crc16(rom.data() + 0xC0, 0x9C));
    put16(rom, 0x15E, crc16(rom.data(), 0x15E));
}

static TestRom build_test_rom()
{
    TestRom t;

    // The ARM9 program as it is in RAM once started: a secure area, start-up
    // code with its module params, code, then (where the BSS will be) the
    // two autoload blocks and the list that describes them.
    std::vector<uint8_t> image = sample_data(kArm9MainSize, 41);
    const uint8_t secure_id[14] = {0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE};
    const std::vector<uint8_t> secure_noise = random_bytes(0x7F0, 42);
    std::memcpy(image.data(), secure_id, sizeof secure_id);
    std::memcpy(image.data() + 0x10, secure_noise.data(), secure_noise.size());
    put16(image, 0x0E, crc16(image.data() + 0x10, 0x7F0));
    t.itcm = sample_data(0x180, 43);
    t.dtcm = sample_data(0x40, 44);
    const uint32_t bss_start = kArm9Load + kArm9MainSize;
    const uint32_t list_start = bss_start + 0x180 + 0x40;
    put32(image, 0x840, kArm9Load + kArm9Params); // the autoload routine's literal
    put32(image, 0x844, 0xE12FFF1E);              // bx lr: the autoload hook
    put32(image, kArm9Params + 0x00, list_start);
    put32(image, kArm9Params + 0x04, list_start + 24);
    put32(image, kArm9Params + 0x08, bss_start);
    put32(image, kArm9Params + 0x0C, bss_start);
    put32(image, kArm9Params + 0x10, bss_start + 0x300);
    put32(image, kArm9Params + 0x14, 0); // compressed_end, known once compressed
    put32(image, kArm9Params + 0x18, 0x04007530);
    put32(image, kArm9Params + 0x1C, 0xDEC00621);
    put32(image, kArm9Params + 0x20, 0x2106C0DE);
    append(image, t.itcm);
    append(image, t.dtcm);
    image.resize(image.size() + 24);
    put32(image, image.size() - 24, 0x01FF8000);
    put32(image, image.size() - 20, 0x180);
    put32(image, image.size() - 16, 0);
    put32(image, image.size() - 12, 0x027E0000);
    put32(image, image.size() - 8, 0x40);
    put32(image, image.size() - 4, 0x100);

    std::vector<uint8_t> arm9 = blz_compress(image, 0x900);
    const uint32_t compressed_end = kArm9Load + static_cast<uint32_t>(arm9.size());
    put32(arm9, kArm9Params + 0x14, compressed_end);
    put32(image, kArm9Params + 0x14, compressed_end);
    t.arm9_image = image;
    t.arm9_size = arm9.size();

    // The ARM7 program: start-up code, the literal and hook that locate its
    // params (which have no marker), one autoload block and its list.
    t.arm7 = sample_data(0x30, 45);
    const std::vector<uint8_t> arm7_block = sample_data(0x60, 46);
    put32(t.arm7, 0x10, kArm7Load + 0x18);
    put32(t.arm7, 0x14, 0xE12FFF1E);
    put32(t.arm7, 0x18, kArm7Load + 0x90);
    put32(t.arm7, 0x1C, kArm7Load + 0x9C);
    put32(t.arm7, 0x20, kArm7Load + 0x30);
    put32(t.arm7, 0x24, kArm7Load + 0x30);
    put32(t.arm7, 0x28, kArm7Load + 0x30);
    put32(t.arm7, 0x2C, 0);
    append(t.arm7, arm7_block);
    t.arm7.resize(0x9C);
    put32(t.arm7, 0x90, 0x037F8000);
    put32(t.arm7, 0x94, 0x60);
    put32(t.arm7, 0x98, 0x20);

    t.overlay0 = sample_data(0x500, 47);
    t.overlay1 = sample_data(0x123, 48);
    const std::vector<uint8_t> overlay0_file = blz_compress(t.overlay0, 0);
    // The file is four bytes longer than the compressed data in it: the
    // overlay table says how much to read, not the FAT.
    std::vector<uint8_t> overlay0_stored = overlay0_file;
    append(overlay0_stored, {0xDE, 0xAD, 0xBE, 0xEF});
    const std::vector<uint8_t> text = {'h', 'e', 'l', 'l', 'o'};

    std::vector<uint8_t> &rom = t.rom;
    rom.assign(kArm9RomOffset, 0);
    append(rom, arm9);
    // The twelve bytes after the ARM9 program: marker, params, signatures.
    rom.resize(rom.size() + 12);
    put32(rom, rom.size() - 12, 0xDEC00621);
    put32(rom, rom.size() - 8, kArm9Params);
    pad_to(rom, 0x200);

    t.overlay_table = rom.size();
    rom.resize(rom.size() + 64);
    pad_to(rom, 0x200);
    t.arm7_offset = rom.size();
    append(rom, t.arm7);
    pad_to(rom, 0x200);

    // Root: a.txt and data/; data: b.bin and sub/; sub: c.bin and d. Files 0
    // and 1 are the overlays, which have no names.
    t.fnt_offset = rom.size();
    const uint8_t fnt[] = {
        24, 0, 0, 0, 2, 0, 3, 0,       // 0: root, names at 24, first file 2, 3 directories in all
        38, 0, 0, 0, 3, 0, 0x00, 0xF0, // 8: data, names at 38, first file 3, parent root
        51, 0, 0, 0, 4, 0, 0x01, 0xF0, // 16: sub, names at 51, first file 4, parent data
        5, 'a', '.', 't', 'x', 't', 0x84, 'd', 'a', 't', 'a', 0x01, 0xF0, 0, // 24: a.txt, data/ (ID at 35)
        5, 'b', '.', 'b', 'i', 'n', 0x83, 's', 'u', 'b', 0x02, 0xF0, 0,      // 38: b.bin, sub/ (ID at 48)
        5, 'c', '.', 'b', 'i', 'n', 1, 'd', 0,                               // 51: c.bin, d
    };
    t.fnt_size = sizeof fnt;
    rom.insert(rom.end(), fnt, fnt + sizeof fnt);
    pad_to(rom, 0x200);
    t.fat_offset = rom.size();
    rom.resize(rom.size() + 6 * 8);
    pad_to(rom, 0x200);

    const std::vector<uint8_t> *contents[5] = {&overlay0_stored, &t.overlay1, &text, &t.dtcm, &t.itcm};
    for (size_t i = 0; i < 5; i++) {
        put32(rom, t.fat_offset + i * 8, static_cast<uint32_t>(rom.size()));
        append(rom, *contents[i]);
        put32(rom, t.fat_offset + i * 8 + 4, static_cast<uint32_t>(rom.size()));
        pad_to(rom, 4);
    } // file 5 ("d") is an empty entry: start and end 0

    // Overlay 0: compressed and signed. Overlay 1: stored plain, at the
    // same address.
    const size_t ov = t.overlay_table;
    put32(rom, ov + 0x00, 0);
    put32(rom, ov + 0x04, kOverlayLoad);
    put32(rom, ov + 0x08, 0x500);
    put32(rom, ov + 0x0C, 0x40);
    put32(rom, ov + 0x10, kOverlayLoad + 0x4F0);
    put32(rom, ov + 0x14, kOverlayLoad + 0x500);
    put32(rom, ov + 0x18, 0);
    put32(rom, ov + 0x1C, static_cast<uint32_t>(overlay0_file.size()) | 0x03000000u);
    put32(rom, ov + 0x20, 1);
    put32(rom, ov + 0x24, kOverlayLoad);
    put32(rom, ov + 0x28, 0x123);
    put32(rom, ov + 0x2C, 0x10);
    put32(rom, ov + 0x30, kOverlayLoad + 0x100);
    put32(rom, ov + 0x34, kOverlayLoad + 0x108);
    put32(rom, ov + 0x38, 1);
    put32(rom, ov + 0x3C, 0);

    std::memcpy(rom.data(), "TESTROM", 7);
    std::memcpy(rom.data() + 0x0C, "TEST", 4);
    std::memcpy(rom.data() + 0x10, "ZZ", 2);
    rom[0x12] = 0;
    rom[0x14] = 1;
    rom[0x1E] = 3;
    put32(rom, 0x20, kArm9RomOffset);
    put32(rom, 0x24, kArm9Load + 0x800);
    put32(rom, 0x28, kArm9Load);
    put32(rom, 0x2C, static_cast<uint32_t>(arm9.size()));
    put32(rom, 0x30, static_cast<uint32_t>(t.arm7_offset));
    put32(rom, 0x34, kArm7Load + 8);
    put32(rom, 0x38, kArm7Load);
    put32(rom, 0x3C, static_cast<uint32_t>(t.arm7.size()));
    put32(rom, 0x40, static_cast<uint32_t>(t.fnt_offset));
    put32(rom, 0x44, sizeof fnt);
    put32(rom, 0x48, static_cast<uint32_t>(t.fat_offset));
    put32(rom, 0x4C, 6 * 8);
    put32(rom, 0x50, static_cast<uint32_t>(t.overlay_table));
    put32(rom, 0x54, 64);
    put32(rom, 0x68, 0);
    put16(rom, 0x6C, 0x1234);
    put32(rom, 0x70, kArm9Load + 0x844);
    put32(rom, 0x74, kArm7Load + 0x14);
    put32(rom, 0x80, static_cast<uint32_t>(rom.size()));
    put32(rom, 0x84, 0x4000);
    put32(rom, 0x88, kArm9RomOffset + kArm9Params);
    for (size_t i = 0; i < 0x9C; i++) rom[0xC0 + i] = static_cast<uint8_t>(i * 7 + 1);
    finish_header(rom);
    return t;
}

static bool parse(const std::vector<uint8_t> &bytes, Rom &rom, std::string &error)
{
    rom.data = bytes;
    return rom.parse(error);
}

// Parses and builds the modules of a ROM image.
static bool modules_of(const std::vector<uint8_t> &bytes, ModuleSet &set, std::string &error)
{
    Rom rom;
    return parse(bytes, rom, error) && build_modules(rom, set, error);
}

// How many different names the modules of a set have: all of them, if the
// set is to be keyed by name.
static size_t distinct_names(const ModuleSet &set)
{
    std::set<std::string> names;
    for (const Module &module : set.modules) names.insert(module.name);
    return names.size();
}

static void test_rom()
{
    const TestRom t = build_test_rom();
    Rom rom;
    std::string error;
    CHECK(!t.rom.empty() && t.arm9_size < t.arm9_image.size()); // the ARM9 program did compress
    CHECK(parse(t.rom, rom, error));
    if (g_failures) std::printf("  (%s)\n", error.c_str());

    const Header &h = rom.header;
    CHECK_STR(h.title, "TESTROM");
    CHECK_STR(h.game_code, "TEST");
    CHECK_STR(h.maker_code, "ZZ");
    CHECK_EQ(h.unit_code, 0);
    CHECK_EQ(h.device_capacity, 1);
    CHECK_EQ(h.rom_version, 3);
    CHECK_EQ(h.arm9.rom_offset, kArm9RomOffset);
    CHECK_EQ(h.arm9.entry, kArm9Load + 0x800);
    CHECK_EQ(h.arm9.load_address, kArm9Load);
    CHECK_EQ(h.arm9.size, t.arm9_size);
    CHECK_EQ(h.arm7.rom_offset, t.arm7_offset);
    CHECK_EQ(h.arm7.entry, kArm7Load + 8);
    CHECK_EQ(h.arm7.load_address, kArm7Load);
    CHECK_EQ(h.arm7.size, 0x9C);
    CHECK_EQ(h.fnt_offset, t.fnt_offset);
    CHECK_EQ(h.fat_offset, t.fat_offset);
    CHECK_EQ(h.fat_size, 48);
    CHECK_EQ(h.arm9_overlay_offset, t.overlay_table);
    CHECK_EQ(h.arm9_overlay_size, 64);
    CHECK_EQ(h.arm7_overlay_size, 0);
    CHECK_EQ(h.icon_offset, 0);
    CHECK_EQ(h.secure_area_crc, 0x1234);
    CHECK_EQ(h.arm9_autoload_hook, kArm9Load + 0x844);
    CHECK_EQ(h.arm7_autoload_hook, kArm7Load + 0x14);
    CHECK_EQ(h.rom_size, t.rom.size());
    CHECK_EQ(h.header_size, 0x4000);
    CHECK_EQ(h.arm9_params_offset, kArm9RomOffset + kArm9Params);
    CHECK(h.header_crc_ok() && h.logo_crc_ok());
    CHECK(rom.warnings.empty());
    CHECK(rom.secure_area == SecureArea::Decrypted && rom.secure_area_self_check);

    // The file system.
    CHECK_EQ(rom.files.size(), 6);
    CHECK_EQ(rom.named_file_count(), 4);
    CHECK_EQ(rom.directories.size(), 3);
    if (rom.files.size() == 6 && rom.directories.size() == 3) {
        CHECK_STR(rom.files[0].path, "");
        CHECK_STR(rom.files[2].path, "a.txt");
        CHECK_STR(rom.files[3].path, "data/b.bin");
        CHECK_STR(rom.files[4].path, "data/sub/c.bin");
        CHECK_STR(rom.files[5].path, "data/sub/d"); // IDs count up within a directory
        CHECK_STR(rom.directories[0], "");
        CHECK_STR(rom.directories[1], "data");
        CHECK_STR(rom.directories[2], "data/sub");
        CHECK_EQ(rom.files[2].size, 5);
        CHECK(rom.files[5].valid && rom.files[5].size == 0);
        uint32_t size = 0;
        const uint8_t *data = rom.file_data(2, size, error);
        CHECK(data && size == 5 && std::memcmp(data, "hello", 5) == 0);
        const FileEntry *found = rom.find_file("data/sub/c.bin");
        CHECK(found && found->id == 4 && found->size == t.itcm.size());
        CHECK(rom.find_file("data/missing") == nullptr);
        CHECK_ERROR(rom.file_data(6, size, error) != nullptr, error, "not in the FAT");
    }
    CHECK(rom.ptr(rom.data.size() - 4, 4) != nullptr);
    CHECK(rom.ptr(rom.data.size() - 4, 5) == nullptr);
    CHECK(rom.ptr(0xFFFFFFFFFFFFFFF0ull, 0x20) == nullptr);

    CHECK_EQ(rom.arm9_overlays.size(), 2);
    CHECK_EQ(rom.arm7_overlays.size(), 0);
    if (rom.arm9_overlays.size() == 2) {
        const OverlayEntry &a = rom.arm9_overlays[0];
        CHECK(a.compressed() && a.authenticated());
        CHECK_EQ(a.load_address, kOverlayLoad);
        CHECK_EQ(a.size, 0x500);
        CHECK_EQ(a.bss_size, 0x40);
        CHECK_EQ(a.compressed_size + 4, rom.files[0].size);
        const OverlayEntry &b = rom.arm9_overlays[1];
        CHECK(!b.compressed() && !b.authenticated());
        CHECK_EQ(b.id, 1);
        CHECK_EQ(b.file_id, 1);
    }

    // The modules.
    ModuleSet set;
    CHECK(build_modules(rom, set, error));
    CHECK_EQ(set.modules.size(), 7);
    CHECK_EQ(distinct_names(set), 7);
    CHECK(set.arm9.compressed && set.arm9.params.present && set.arm9.params.has_nitrocode);
    CHECK_EQ(set.arm9.params.offset, kArm9Params);
    CHECK_EQ(set.arm9.params.sdk_version, 0x04007530);
    CHECK_EQ(set.arm9.params.compressed_end, kArm9Load + t.arm9_size);
    CHECK_EQ(set.arm9.image_size, t.arm9_image.size());
    CHECK(set.arm9.stored_prefix >= 0x900 && set.arm9.stored_prefix < t.arm9_size);
    CHECK_EQ(set.arm9.autoload_count, 2);
    CHECK(set.arm7.params.present && !set.arm7.params.has_nitrocode && !set.arm7.compressed);
    CHECK_EQ(set.arm7.params.offset, 0x18);
    CHECK_EQ(set.arm7.autoload_count, 1);

    const Module *main_module = set.find("main");
    CHECK(main_module && main_module == set.find(ModuleKind::Arm9Main) && main_module == &set.modules[0]);
    if (main_module) {
        const std::vector<uint8_t> expected(t.arm9_image.begin(), t.arm9_image.begin() + kArm9MainSize);
        CHECK(main_module->bytes == expected);
        CHECK_EQ(main_module->load_address, kArm9Load);
        CHECK_EQ(main_module->bss_size, 0x300);
        CHECK(main_module->compressed);
        CHECK_EQ(main_module->hash, fxhash64(expected.data(), expected.size()));
        // What ds-rom would write: the same with compressed_end cleared.
        std::vector<uint8_t> object = expected;
        put32(object, kArm9Params + 0x14, 0);
        CHECK_EQ(main_module->dsd_hash, fxhash64(object.data(), object.size()));
        CHECK(main_module->dsd_hash != main_module->hash);
        CHECK_EQ(main_module->end(), kArm9Load + kArm9MainSize);
        CHECK_EQ(main_module->bss_end(), kArm9Load + kArm9MainSize + 0x300);
        CHECK(main_module->contains(kArm9Load) && main_module->contains(kArm9Load + kArm9MainSize - 4, 4));
        CHECK(!main_module->contains(kArm9Load + kArm9MainSize - 3, 4) && !main_module->contains(kArm9Load - 1));
        CHECK(!main_module->contains(0xFFFFFFFFu, 4));
        CHECK(main_module->ptr(kArm9Load + kArm9MainSize, 1) == nullptr);
        CHECK_EQ(main_module->read32(kArm9Load + kArm9Params + 0x1C), 0xDEC00621);
        CHECK_EQ(main_module->read16(kArm9Load + 0x0E), get16(expected.data() + 0x0E));
    }
    const Module *itcm = set.find(ModuleKind::Itcm);
    CHECK(itcm && itcm->name == "itcm" && itcm->bytes == t.itcm);
    if (itcm) {
        CHECK_EQ(itcm->load_address, 0x01FF8000);
        CHECK_EQ(itcm->bss_size, 0);
        CHECK_EQ(itcm->autoload_index, 0);
        CHECK(itcm->compressed); // it came out of the compressed program
        CHECK_EQ(itcm->dsd_hash, itcm->hash);
        CHECK_EQ(itcm->hash, fxhash64(t.itcm.data(), t.itcm.size()));
    }
    const Module *dtcm = set.find("dtcm");
    CHECK(dtcm && dtcm->kind == ModuleKind::Dtcm && dtcm->bytes == t.dtcm);
    if (dtcm) {
        CHECK_EQ(dtcm->load_address, 0x027E0000);
        CHECK_EQ(dtcm->bss_size, 0x100);
        CHECK_EQ(dtcm->autoload_index, 1);
    }
    const Module *ov0 = set.find(ModuleKind::Arm9Overlay, 0);
    CHECK(ov0 && ov0->name == "ov000" && ov0->bytes == t.overlay0);
    if (ov0) {
        CHECK(ov0->compressed);
        CHECK_EQ(ov0->overlay_id, 0);
        CHECK_EQ(ov0->load_address, kOverlayLoad);
        CHECK_EQ(ov0->bss_size, 0x40);
        CHECK_EQ(ov0->static_init_start, kOverlayLoad + 0x4F0);
        CHECK_EQ(ov0->static_init_end, kOverlayLoad + 0x500);
        CHECK_EQ(ov0->file_id, 0);
        CHECK_EQ(ov0->hash, fxhash64(t.overlay0.data(), t.overlay0.size()));
    }
    const Module *ov1 = set.find("ov001");
    CHECK(ov1 && ov1 == set.find(ModuleKind::Arm9Overlay, 1) && ov1->bytes == t.overlay1 && !ov1->compressed);
    CHECK(set.find(ModuleKind::Arm9Overlay, 2) == nullptr);
    CHECK(set.find(ModuleKind::Arm9Autoload) == nullptr && set.find(ModuleKind::Arm7Overlay) == nullptr);
    const Module *arm7 = set.find(ModuleKind::Arm7Main);
    CHECK(arm7 && arm7->name == "arm7" && arm7->size() == 0x30);
    if (arm7) {
        CHECK(std::memcmp(arm7->bytes.data(), t.arm7.data(), 0x30) == 0);
        CHECK_EQ(arm7->load_address, kArm7Load);
        CHECK_EQ(arm7->bss_size, 0);
    }
    const Module *wram = set.find(ModuleKind::Arm7Autoload, 0);
    CHECK(wram && wram->name == "arm7_autoload_0" && wram->size() == 0x60);
    if (wram) {
        CHECK(std::memcmp(wram->bytes.data(), t.arm7.data() + 0x30, 0x60) == 0);
        CHECK_EQ(wram->load_address, 0x037F8000);
        CHECK_EQ(wram->bss_size, 0x20);
        CHECK(!wram->compressed);
    }
    CHECK_EQ(set.total_bytes(), kArm9MainSize + 0x180 + 0x40 + 0x500 + 0x123 + 0x30 + 0x60);
    CHECK(set.notes.empty());
    CHECK_STR(module_kind_name(ModuleKind::Arm7Autoload), "arm7_autoload");
    CHECK(module_is_arm7(ModuleKind::Arm7Overlay) && !module_is_arm7(ModuleKind::Dtcm));
    CHECK(module_is_overlay(ModuleKind::Arm9Overlay) && !module_is_overlay(ModuleKind::Arm9Main));

    // The four ways the ARM9's module params are found, each on its own:
    // the header's pointer, the words after the program, the literal before
    // the autoload hook, and the marker itself.
    {
        const size_t footer = kArm9RomOffset + t.arm9_size;
        std::vector<uint8_t> bytes = t.rom;
        put32(bytes, 0x88, 0);
        CHECK(modules_of(bytes, set, error) && set.arm9.params.offset == kArm9Params && set.modules.size() == 7);
        put32(bytes, footer, 0);
        CHECK(modules_of(bytes, set, error) && set.arm9.params.offset == kArm9Params && set.modules.size() == 7);
        put32(bytes, 0x70, 0);
        CHECK(modules_of(bytes, set, error) && set.arm9.params.offset == kArm9Params && set.modules.size() == 7);
        // A header pointer that is relative to the program, as DSi-era headers have it.
        bytes = t.rom;
        put32(bytes, 0x88, kArm9Params);
        put32(bytes, footer, 0);
        put32(bytes, 0x70, 0);
        CHECK(modules_of(bytes, set, error) && set.arm9.params.offset == kArm9Params && set.modules.size() == 7);
    }
    // The main module ends where the BSS starts, which need not be where the
    // autoload blocks are stored (ds-rom cuts arm9.bin the same way).
    {
        std::vector<uint8_t> bytes = t.rom;
        put32(bytes, kArm9RomOffset + kArm9Params + 0x0C, kArm9Load + kArm9MainSize - 0x40);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 7);
        const Module *shorter = set.find("main");
        CHECK(shorter && shorter->size() == kArm9MainSize - 0x40 && shorter->bss_size == 0x340);
        CHECK(set.find("itcm") && set.find("itcm")->bytes == t.itcm && set.find("dtcm") && set.find("dtcm")->bytes == t.dtcm);
    }
    // Without the hook the ARM7's params cannot be told from data: the
    // program is one module.
    {
        std::vector<uint8_t> bytes = t.rom;
        put32(bytes, 0x74, 0);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 6 && !set.arm7.params.present);
        const Module *whole = set.find("arm7");
        CHECK(whole && whole->bytes == t.arm7 && set.find(ModuleKind::Arm7Autoload) == nullptr);
        // A hook whose literal leads to words that are not params (the list
        // would lie outside the program) is ignored the same way.
        bytes = t.rom;
        put32(bytes, t.arm7_offset + 0x1C, kArm7Load + 0x1000);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 6 && !set.arm7.params.present);
        // So is one whose "compressed end" is set: the ARM7 has no decompressor.
        bytes = t.rom;
        put32(bytes, t.arm7_offset + 0x2C, kArm7Load + 0x9C);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 6 && !set.arm7.params.present);
    }
    // A program with no SDK start-up code at all (homebrew): one module each.
    {
        std::vector<uint8_t> bytes = t.rom;
        put32(bytes, 0x70, 0);
        put32(bytes, 0x74, 0);
        put32(bytes, 0x88, 0);
        put32(bytes, kArm9RomOffset + t.arm9_size, 0);
        put32(bytes, kArm9RomOffset + kArm9Params + 0x1C, 0); // no marker
        CHECK(modules_of(bytes, set, error) && !set.arm9.params.present && !set.arm9.compressed);
        const Module *whole = set.find("main");
        CHECK(whole && whole->size() == t.arm9_size && whole->dsd_hash == whole->hash && whole->bss_size == 0);
        CHECK_EQ(set.modules.size(), 4); // main, two overlays, arm7
    }

    // The secure area's states.
    {
        std::vector<uint8_t> bytes = t.rom;
        std::memcpy(bytes.data() + kArm9RomOffset, "encryObj", 8);
        CHECK(parse(bytes, rom, error) && rom.secure_area == SecureArea::DecryptedId && rom.secure_area_self_check);
        CHECK(build_modules(rom, set, error) && set.notes.size() == 1);
        const Module *with_id = set.find("main");
        const uint8_t destroyed[8] = {0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7};
        CHECK(with_id && std::memcmp(with_id->bytes.data(), destroyed, 8) == 0);
        CHECK(with_id && main_module && with_id->hash == fxhash64(t.arm9_image.data(), kArm9MainSize));

        bytes = t.rom;
        const std::vector<uint8_t> noise = random_bytes(0x800, 50);
        std::memcpy(bytes.data() + kArm9RomOffset, noise.data(), noise.size());
        CHECK(parse(bytes, rom, error) && rom.secure_area == SecureArea::Encrypted);
        CHECK_ERROR(build_modules(rom, set, error), error, "encrypted");
        CHECK(set.modules.empty());

        bytes = t.rom;
        std::memset(bytes.data() + kArm9RomOffset, 0, 0x800);
        CHECK(parse(bytes, rom, error) && rom.secure_area == SecureArea::Blank && !rom.secure_area_self_check);

        bytes = t.rom;
        bytes[kArm9RomOffset + 0x20] ^= 0xFF; // damage inside the checked range
        CHECK(parse(bytes, rom, error) && rom.secure_area == SecureArea::Decrypted && !rom.secure_area_self_check);
        CHECK_STR(secure_area_name(SecureArea::Encrypted), "encrypted");
    }

    // Stale checksums are reported and nothing more.
    {
        std::vector<uint8_t> bytes = t.rom;
        bytes[0] = 'B';
        bytes[0xC0] ^= 1;
        CHECK(parse(bytes, rom, error));
        CHECK(!rom.header.header_crc_ok() && !rom.header.logo_crc_ok() && rom.warnings.size() == 2);
        CHECK_STR(rom.header.title, "BESTROM");
        CHECK(build_modules(rom, set, error) && set.modules.size() == 7);
        bytes[1] = 0x07; // an unprintable byte in the title
        CHECK(parse(bytes, rom, error));
        CHECK_STR(rom.header.title, "B?STROM");
    }

    // Tables that point outside the image, each refused with its reason.
    {
        std::vector<uint8_t> bytes(t.rom.begin(), t.rom.begin() + 0x1FF);
        CHECK_ERROR(parse(bytes, rom, error), error, "not a DS ROM");
        bytes = t.rom;
        put32(bytes, 0x2C, static_cast<uint32_t>(t.rom.size()));
        CHECK_ERROR(parse(bytes, rom, error), error, "ARM9 program");
        bytes = t.rom;
        put32(bytes, 0x30, 0xFFFFFFF0u);
        CHECK_ERROR(parse(bytes, rom, error), error, "ARM7 program");
        bytes = t.rom;
        put32(bytes, 0x38, 0xFFFFFF80u); // load address + size wraps
        CHECK_ERROR(parse(bytes, rom, error), error, "address space");
        bytes = t.rom;
        put32(bytes, 0x4C, 47);
        CHECK_ERROR(parse(bytes, rom, error), error, "multiple of 8");
        bytes = t.rom;
        put32(bytes, 0x48, static_cast<uint32_t>(t.rom.size()) - 8);
        CHECK_ERROR(parse(bytes, rom, error), error, "FAT");
        bytes = t.rom;
        put32(bytes, 0x54, 63);
        CHECK_ERROR(parse(bytes, rom, error), error, "multiple of 32");
        bytes = t.rom;
        put32(bytes, 0x50, 0xFFFFFFFFu);
        CHECK_ERROR(parse(bytes, rom, error), error, "overlay table");
        bytes = t.rom;
        put32(bytes, 0x40, static_cast<uint32_t>(t.rom.size()));
        CHECK_ERROR(parse(bytes, rom, error), error, "FNT");
        bytes = t.rom;
        put32(bytes, 0x44, 7);
        CHECK_ERROR(parse(bytes, rom, error), error, "too short");
        bytes = t.rom;
        put32(bytes, 0x44, 30); // cut right after the root's first name
        CHECK_ERROR(parse(bytes, rom, error), error, "runs past the end of the FNT");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 6, 0); // no directories
        CHECK_ERROR(parse(bytes, rom, error), error, "directories");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 6, 4097);
        CHECK_ERROR(parse(bytes, rom, error), error, "directories");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 48, 0xF001); // sub/ replaced by a second data/: a cycle
        CHECK_ERROR(parse(bytes, rom, error), error, "more than once");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 48, 0xF000); // ... or by the root
        CHECK_ERROR(parse(bytes, rom, error), error, "more than once");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 48, 0xF003); // a directory that does not exist
        CHECK_ERROR(parse(bytes, rom, error), error, "is not one of");
        bytes = t.rom;
        put16(bytes, t.fnt_offset + 48, 0x0002); // not a directory ID at all
        CHECK_ERROR(parse(bytes, rom, error), error, "is not one of");
        bytes = t.rom;
        bytes[t.fnt_offset + 12] = 2; // data/ starts at file 2, which a.txt already is
        CHECK_ERROR(parse(bytes, rom, error), error, "named more than once");
        bytes = t.rom;
        bytes[t.fnt_offset + 20] = 6; // sub/ starts at file 6; the FAT ends at 5
        CHECK_ERROR(parse(bytes, rom, error), error, "FAT has 6 entries");
        bytes = t.rom;
        bytes[t.fnt_offset + 24] = 0x80;
        CHECK_ERROR(parse(bytes, rom, error), error, "reserved");
        bytes = t.rom;
        bytes[t.fnt_offset + 16] = 200; // sub/'s names start outside the table
        CHECK_ERROR(parse(bytes, rom, error), error, "runs past the end of the FNT");
        // No FNT at all is fine: files without names.
        bytes = t.rom;
        put32(bytes, 0x44, 0);
        CHECK(parse(bytes, rom, error) && rom.files.size() == 6 && rom.named_file_count() == 0);

        // A file whose range leaves the image is only an error when used.
        bytes = t.rom;
        put32(bytes, t.fat_offset + 2 * 8 + 4, static_cast<uint32_t>(t.rom.size()) + 1);
        CHECK(parse(bytes, rom, error) && !rom.files[2].valid && rom.files[2].size == 0);
        CHECK(build_modules(rom, set, error));
        uint32_t size = 0;
        CHECK_ERROR(rom.file_data(2, size, error) != nullptr, error, "outside the image");
        bytes = t.rom;
        put32(bytes, t.fat_offset + 4, 0); // overlay 0's file ends before it starts
        CHECK(parse(bytes, rom, error) && !rom.files[0].valid);
        CHECK_ERROR(build_modules(rom, set, error), error, "ARM9 overlay 0");
    }

    // Damage to what the modules are made from.
    {
        const size_t params = kArm9RomOffset + kArm9Params;
        const size_t ov = t.overlay_table;
        std::vector<uint8_t> bytes = t.rom;
        put32(bytes, params + 0x14, kArm9Load + static_cast<uint32_t>(t.arm9_size) + 4);
        CHECK_ERROR(modules_of(bytes, set, error), error, "outside the program");
        bytes = t.rom;
        put32(bytes, params + 0x14, kArm9Load);
        CHECK_ERROR(modules_of(bytes, set, error), error, "outside the program");
        bytes = t.rom;
        put32(bytes, params + 0x14, kArm9Load + static_cast<uint32_t>(t.arm9_size) - 4); // not where the footer is
        CHECK_ERROR(modules_of(bytes, set, error), error, "ARM9 program: BLZ");
        bytes = t.rom;
        put32(bytes, kArm9RomOffset + t.arm9_size - 4, 0x00F00000); // the footer's extra size
        CHECK_ERROR(modules_of(bytes, set, error), error, "ARM9 program: BLZ");
        bytes = t.rom;
        put32(bytes, kArm9RomOffset + t.arm9_size - 4, 0x02000000); // 32 MiB: more than the console has
        CHECK_ERROR(modules_of(bytes, set, error), error, "ARM9 program: BLZ");
        bytes = t.rom;
        put32(bytes, params + 0x04, kArm9Load + 0x100000); // autoload list far outside
        CHECK_ERROR(modules_of(bytes, set, error), error, "autoload list");
        bytes = t.rom;
        put32(bytes, params + 0x04, get32(bytes.data() + params) + 23); // not whole entries
        CHECK_ERROR(modules_of(bytes, set, error), error, "autoload list");
        bytes = t.rom;
        put32(bytes, params + 0x08, kArm9Load - 4);
        CHECK_ERROR(modules_of(bytes, set, error), error, "autoload blocks start");
        bytes = t.rom;
        put32(bytes, params + 0x0C, kArm9Load + 0x100000);
        CHECK_ERROR(modules_of(bytes, set, error), error, "BSS");
        bytes = t.rom;
        put32(bytes, params + 0x10, kArm9Load);
        CHECK_ERROR(modules_of(bytes, set, error), error, "BSS");
        bytes = t.rom;
        put32(bytes, params + 0x08, kArm9Load + static_cast<uint32_t>(t.arm9_image.size()) - 8); // blocks start too late
        CHECK_ERROR(modules_of(bytes, set, error), error, "runs past the end of the program");

        // Module params in the compressed part of the program cannot be what
        // the start-up code reads before it decompresses. Here a second copy
        // sits in the footer's padding, which counts as compressed data, the
        // header points at it, and everything else about the stream is valid.
        std::vector<uint8_t> arm9(t.rom.begin() + kArm9RomOffset,
                                  t.rom.begin() + kArm9RomOffset + static_cast<std::ptrdiff_t>(t.arm9_size));
        std::vector<uint8_t> copy(arm9.begin() + kArm9Params, arm9.begin() + kArm9Params + 0x24);
        put32(copy, 0x14, kArm9Load + static_cast<uint32_t>(t.arm9_size) + 0x24);
        BlzFooter footer;
        CHECK(blz_read_footer(arm9.data(), arm9.size(), footer, error) && footer.extra_size >= 0x24);
        arm9.insert(arm9.end() - 8, copy.begin(), copy.end());
        put32(arm9, arm9.size() - 8, (footer.compressed_span + 0x24) | ((footer.footer_size + 0x24) << 24));
        put32(arm9, arm9.size() - 4, footer.extra_size - 0x24);
        bytes = t.rom;
        const uint32_t moved = static_cast<uint32_t>(bytes.size()); // the program now sits at the end of the image
        append(bytes, arm9);
        put32(bytes, 0x20, moved);
        put32(bytes, 0x2C, static_cast<uint32_t>(arm9.size()));
        put32(bytes, 0x88, moved + static_cast<uint32_t>(t.arm9_size) - 8);
        CHECK_ERROR(modules_of(bytes, set, error), error, "inside the compressed data");
        // The same image with the header pointing at the params in the
        // stored part is fine, wherever the program sits in the ROM.
        put32(bytes, moved + kArm9Params + 0x14, kArm9Load + static_cast<uint32_t>(arm9.size()));
        put32(bytes, 0x88, moved + kArm9Params);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 7 && set.find("main") &&
              set.find("main")->size() == kArm9MainSize);

        bytes = t.rom;
        put32(bytes, ov + 0x18, 6);
        CHECK_ERROR(modules_of(bytes, set, error), error, "not in the FAT");
        bytes = t.rom;
        put32(bytes, ov + 0x1C, 0x01000000); // compressed, size 0
        CHECK_ERROR(modules_of(bytes, set, error), error, "compressed size");
        bytes = t.rom;
        put32(bytes, ov + 0x1C, 0x01FFFFFF); // compressed size beyond the file
        CHECK_ERROR(modules_of(bytes, set, error), error, "compressed size");
        bytes = t.rom;
        put32(bytes, ov + 0x1C, get32(bytes.data() + ov + 0x1C) - 4); // the footer is not there
        CHECK_ERROR(modules_of(bytes, set, error), error, "ARM9 overlay 0: BLZ");
        bytes = t.rom;
        put32(bytes, ov + 0x08, 0x504); // more than the file decompresses to
        CHECK_ERROR(modules_of(bytes, set, error), error, "its table entry says");
        bytes = t.rom;
        put32(bytes, ov + 0x28, 0x124); // more than the plain file holds
        CHECK_ERROR(modules_of(bytes, set, error), error, "its table entry says");
        bytes = t.rom;
        put32(bytes, ov + 0x20, 0); // two overlays with ID 0
        CHECK_ERROR(modules_of(bytes, set, error), error, "appears twice");
        bytes = t.rom;
        put32(bytes, ov + 0x20, 0x10000);
        CHECK_ERROR(modules_of(bytes, set, error), error, "16-bit");
        bytes = t.rom;
        put32(bytes, ov + 0x24, 0xFFFFFF00u); // runs off the end of the address space
        CHECK_ERROR(modules_of(bytes, set, error), error, "address space");
        bytes = t.rom;
        put32(bytes, ov + 0x2C, 0xFFFFFFFFu); // so does this BSS
        CHECK_ERROR(modules_of(bytes, set, error), error, "address space");

        // Fewer bytes asked for than the file holds: the rest is not loaded.
        bytes = t.rom;
        put32(bytes, ov + 0x28, 0x100);
        CHECK(modules_of(bytes, set, error) && set.find("ov001") && set.find("ov001")->size() == 0x100);
        bytes = t.rom;
        put32(bytes, ov + 0x08, 0x4F0); // ... and of a compressed one, with a note
        CHECK(modules_of(bytes, set, error) && set.find("ov000") && set.find("ov000")->size() == 0x4F0 &&
              set.notes.size() == 1);
        // An overlay table for the ARM7 is read the same way.
        bytes = t.rom;
        put32(bytes, 0x58, static_cast<uint32_t>(t.overlay_table));
        put32(bytes, 0x5C, 64);
        CHECK(modules_of(bytes, set, error) && set.modules.size() == 9 && distinct_names(set) == 9);
        const Module *arm7_overlay = set.find(ModuleKind::Arm7Overlay, 1);
        CHECK(arm7_overlay && arm7_overlay->name == "arm7_ov001" && arm7_overlay->bytes == t.overlay1);
    }

    // Mutations of the header and of every table: nothing may be read or
    // written out of bounds, and modules that come out must be consistent.
    {
        struct Range {
            size_t start;
            size_t size;
        };
        const Range ranges[] = {{0x00, 0x180},
                                {kArm9RomOffset + kArm9Params, 0x24},
                                {kArm9RomOffset + t.arm9_size - 8, 20},
                                {t.overlay_table, 64},
                                {t.arm7_offset, 0x9C},
                                {t.fnt_offset, t.fnt_size},
                                {t.fat_offset, 48}};
        size_t inconsistent = 0;
        size_t parsed = 0;
        size_t built = 0;
        g_random = 123;
        Rom mutated;
        for (int i = 0; i < 20000; i++) {
            mutated.data = t.rom;
            for (uint32_t n = random_below(4) + 1; n > 0; n--) {
                const Range &range = ranges[random_below(sizeof ranges / sizeof ranges[0])];
                const size_t at = range.start + random_below(static_cast<uint32_t>(range.size));
                // Small changes and extreme values both: tables break in different ways.
                const uint32_t how = random_below(4);
                if (how == 0) mutated.data[at] = static_cast<uint8_t>(random_below(256));
                else if (how == 1) mutated.data[at] = static_cast<uint8_t>(mutated.data[at] + 1);
                else if (how == 2) mutated.data[at] = 0xFF;
                else mutated.data[at] = 0;
            }
            if (random_below(16) == 0) mutated.data.resize(0x200 + random_below(static_cast<uint32_t>(t.rom.size()) - 0x200));
            if (!mutated.parse(error)) continue;
            parsed++;
            for (const FileEntry &file : mutated.files) {
                if (static_cast<uint64_t>(file.offset) + file.size > mutated.data.size()) inconsistent++;
            }
            ModuleSet modules;
            if (!build_modules(mutated, modules, error)) continue;
            built++;
            for (const Module &module : modules.modules) {
                if (module.hash != fxhash64(module.bytes.data(), module.bytes.size()) ||
                    module.bss_end() > 0x100000000ull || module.bytes.size() > 0x1000000) {
                    inconsistent++;
                }
            }
        }
        std::printf("ROM: of 20000 damaged images, %zu parse and %zu of those yield modules\n", parsed, built);
        CHECK_EQ(inconsistent, 0);
        CHECK(parsed > 1000 && built > 100);
    }
}

/* ------------------------------------------------------------------ */
/* ds-decomp symbols, relocations, sections and config                  */
/* ------------------------------------------------------------------ */

static void test_symbols()
{
    std::vector<DsdSymbol> symbols;
    std::string error;

    // Lines from the Phantom Hourglass configuration and from symbols.md.
    const std::string text =
        "IsDebugger kind:function(thumb,size=0x4) addr:0x02000076\n"
        "Entry kind:function(arm,size=0x13c) addr:0x02000800\n"
        "BuildInfo kind:data(any) addr:0x02000b64\n"
        "\n"
        "// a comment line\n"
        "data_02057ae0 kind:data(word) addr:0x02057ae0 // trailing comment\n"
        "_02003154 kind:data(byte[256]) addr:0x02003154\n"
        "table kind:data(short[]) addr:0x02003300\r\n"
        "words kind:data(word[0x10]) addr:0x02003400\n"
        "sActorTypeList kind:bss(size=0x8) addr:0x02069120\n"
        "data_02058e22 kind:bss addr:0x02058e22\n"
        ".L_0210eb10 kind:label(thumb) addr:0x0210ea90\n"
        ".L_02002e28 kind:label(arm) addr:0x02002e28\n"
        "@4 kind:bss addr:0x027e0600 local\n"
        "sQuiverSizes kind:data(any) addr:0x020dc664 local\n"
        "WeakAmbiguousSymbol kind:bss addr:0x02009abc weak ambiguous\n"
        "_Z14GetEquipHammerv kind:function(arm,size=0x10) addr:0x020b84cc local\n"
        "func_unknown kind:function(thumb,size=0x0,unknown) addr:0x02001000\n"
        "func_protected kind:function(arm,size=0x40,dsprot=(0x38,0x1234),dsprot_range=(0x02000000..0x02000010,0x5)) "
        "addr:0x02004000\n"
        "decimal kind:data(byte) addr:33554432\n";
    CHECK(parse_dsd_symbols(text, "symbols.txt", symbols, error));
    CHECK_EQ(symbols.size(), 18);
    if (symbols.size() == 18) {
        CHECK_STR(symbols[0].name, "IsDebugger");
        CHECK(symbols[0].kind == DsdSymbolKind::Function && symbols[0].thumb && symbols[0].size_stated);
        CHECK_EQ(symbols[0].size, 4);
        CHECK_EQ(symbols[0].address, 0x02000076);
        CHECK(symbols[1].kind == DsdSymbolKind::Function && !symbols[1].thumb);
        CHECK_EQ(symbols[1].size, 0x13C);
        CHECK(symbols[2].kind == DsdSymbolKind::Data && symbols[2].data_type == DsdDataType::Any &&
              !symbols[2].size_stated);
        CHECK(symbols[3].data_type == DsdDataType::Word && symbols[3].size_stated && symbols[3].size == 4);
        CHECK(symbols[4].data_type == DsdDataType::Byte && symbols[4].size == 256);
        CHECK(symbols[5].data_type == DsdDataType::Short && !symbols[5].size_stated);
        CHECK_EQ(symbols[5].address, 0x02003300); // the \r is not part of the address
        CHECK(symbols[6].data_type == DsdDataType::Word && symbols[6].size == 0x40);
        CHECK(symbols[7].kind == DsdSymbolKind::Bss && symbols[7].size_stated && symbols[7].size == 8);
        CHECK(symbols[8].kind == DsdSymbolKind::Bss && !symbols[8].size_stated);
        CHECK(symbols[9].kind == DsdSymbolKind::Label && symbols[9].thumb);
        CHECK_STR(symbols[9].name, ".L_0210eb10");
        CHECK(symbols[10].kind == DsdSymbolKind::Label && !symbols[10].thumb);
        CHECK(symbols[11].local && !symbols[11].weak && !symbols[11].ambiguous);
        CHECK_STR(symbols[11].name, "@4");
        CHECK(symbols[12].local && symbols[12].kind == DsdSymbolKind::Data);
        CHECK(symbols[13].weak && symbols[13].ambiguous && !symbols[13].local);
        CHECK(symbols[14].local && symbols[14].kind == DsdSymbolKind::Function);
        CHECK(symbols[15].unknown && symbols[15].thumb);
        CHECK(symbols[16].kind == DsdSymbolKind::Function && symbols[16].size == 0x40 && !symbols[16].unknown);
        CHECK_EQ(symbols[16].address, 0x02004000);
        CHECK_EQ(symbols[17].address, 0x02000000);
    }

    // As strict as dsd: what it would refuse is refused, with the line.
    CHECK_ERROR(parse_dsd_symbols("a kind:function(arm,size=0x4) addr:0x0\nb kind:function(arm) addr:0x4\n", "s.txt",
                                  symbols, error),
                error, "s.txt:2: a function needs a size");
    CHECK_ERROR(parse_dsd_symbols("f kind:function(size=0x4) addr:0x0\n", "s", symbols, error), error, "instruction set");
    CHECK_ERROR(parse_dsd_symbols("f kind:function(mips,size=0x4) addr:0x0\n", "s", symbols, error), error, "'mips'");
    CHECK_ERROR(parse_dsd_symbols("f kind:function(arm,size=0x4,colour=1) addr:0x0\n", "s", symbols, error), error,
                "unknown function attribute 'colour'");
    CHECK_ERROR(parse_dsd_symbols("f kind:function(arm,size=4g) addr:0x0\n", "s", symbols, error), error, "function size");
    CHECK_ERROR(parse_dsd_symbols("f kind:code addr:0x0\n", "s", symbols, error), error, "unknown symbol kind 'code'");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any)\n", "s", symbols, error), error, "no 'addr'");
    CHECK_ERROR(parse_dsd_symbols("f addr:0x0\n", "s", symbols, error), error, "no 'kind'");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any) addr:0x100000000\n", "s", symbols, error), error, "address");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any) addr:0x\n", "s", symbols, error), error, "address");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any) addr:0x0 global\n", "s", symbols, error), error,
                "unknown symbol attribute 'global'");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any) addr:0x0 local weak\n", "s", symbols, error), error, "local or weak");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(any[4]) addr:0x0\n", "s", symbols, error), error, "cannot be an array");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(long) addr:0x0\n", "s", symbols, error), error, "'long'");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(word[4]x) addr:0x0\n", "s", symbols, error), error, "malformed array");
    CHECK_ERROR(parse_dsd_symbols("f kind:data(word[0x40000000]) addr:0x0\n", "s", symbols, error), error, "larger than");
    CHECK_ERROR(parse_dsd_symbols("f kind:data() addr:0x0\n", "s", symbols, error), error, "data type");
    CHECK_ERROR(parse_dsd_symbols("f kind:bss(align=4) addr:0x0\n", "s", symbols, error), error, "unknown bss attribute");
    CHECK_ERROR(parse_dsd_symbols("f kind:label(x86) addr:0x0\n", "s", symbols, error), error, "'x86'");
    CHECK(parse_dsd_symbols("", "s", symbols, error) && symbols.empty());
    CHECK(parse_dsd_symbols("\xEF\xBB\xBF// only a comment\n\n   \n", "s", symbols, error) && symbols.empty());
}

static void test_relocations()
{
    std::vector<DsdRelocation> relocs;
    std::string error;
    const std::string text =
        "from:0x02000814 kind:arm_call to:0x02000a78 module:main\n"
        "from:0x020008fc kind:arm_call_thumb to:0x0202ae54 module:main\n"
        "from:0x02000918 kind:load to:0x027e0000 module:dtcm\n"
        "from:0x02000930 kind:load to:0x01ffa48c module:itcm\n"
        "from:0x02000c38 kind:arm_call to:0x0211f9a0 module:overlay(16)\n"
        "from:0x0201834c kind:load to:0x027e037c add:0x4c module:dtcm\n"
        "from:0x0207bc4c kind:arm_call to:0x0215ce54 module:overlays(17,18)\n"
        "from:0x02091a18 kind:arm_call to:0x02175c44 module:overlays(15,27,61) // why\n"
        "from:0x020ad870 kind:overlay_id to:29\n"
        "from:0x0204f470 kind:link_time_const(ARM9_CTOR_START)\n"
        "from:0x02001000 kind:thumb_call to:0x02001100 module:main\n"
        "from:0x02001004 kind:thumb_call_arm to:0x02001200 module:main\n"
        "from:0x02001008 kind:arm_branch to:0x02001300 module:main\n"
        "from:0x0200100c kind:load to:0x02001400 add:-0x10 module:none\n"
        "from:0x02001010 kind:load to:0x02400000 module:autoload(2)\n"
        "module:main to:0x02001500 kind:load from:0x02001014\n"
        "from:0x02001018 kind:link_time_const(__OVERLAY_COUNT) to:0x5 module:main\n";
    CHECK(parse_dsd_relocations(text, "relocs.txt", relocs, error));
    CHECK_EQ(relocs.size(), 17);
    if (relocs.size() == 17) {
        CHECK(relocs[0].kind == DsdRelocKind::ArmCall && relocs[0].target == DsdRelocTarget::Main);
        CHECK_EQ(relocs[0].from, 0x02000814);
        CHECK_EQ(relocs[0].to, 0x02000A78);
        CHECK_EQ(relocs[0].addend, 0);
        CHECK(relocs[1].kind == DsdRelocKind::ArmCallThumb);
        CHECK(relocs[2].kind == DsdRelocKind::Load && relocs[2].target == DsdRelocTarget::Dtcm);
        CHECK(relocs[3].target == DsdRelocTarget::Itcm);
        CHECK(relocs[4].target == DsdRelocTarget::Overlay && relocs[4].overlays == std::vector<uint16_t>{16});
        CHECK_EQ(relocs[5].addend, 0x4C);
        CHECK(relocs[6].target == DsdRelocTarget::Overlays && relocs[6].overlays == (std::vector<uint16_t>{17, 18}));
        CHECK(relocs[7].overlays == (std::vector<uint16_t>{15, 27, 61}));
        CHECK(relocs[8].kind == DsdRelocKind::OverlayId && relocs[8].target == DsdRelocTarget::None);
        CHECK_EQ(relocs[8].to, 29); // decimal: an ID, not an address
        CHECK(relocs[9].kind == DsdRelocKind::LinkTimeConst && relocs[9].constant == DsdLinkTimeConst::Arm9CtorStart);
        CHECK(relocs[9].to == 0 && relocs[9].target == DsdRelocTarget::None);
        CHECK(relocs[10].kind == DsdRelocKind::ThumbCall && relocs[11].kind == DsdRelocKind::ThumbCallArm);
        CHECK(relocs[12].kind == DsdRelocKind::ArmBranch);
        CHECK(relocs[13].target == DsdRelocTarget::None && relocs[13].addend == -0x10);
        CHECK(relocs[14].target == DsdRelocTarget::Autoload && relocs[14].autoload_index == 2);
        CHECK(relocs[15].kind == DsdRelocKind::Load && relocs[15].from == 0x02001014 && relocs[15].to == 0x02001500);
        // A link-time constant has no address and no module, whatever the line says.
        CHECK(relocs[16].constant == DsdLinkTimeConst::OverlayCount && relocs[16].to == 0 &&
              relocs[16].target == DsdRelocTarget::None);
    }
    CHECK_STR(dsd_reloc_kind_name(DsdRelocKind::ThumbCallArm), "thumb_call_arm");

    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4\n", "r.txt", relocs, error), error,
                "r.txt:1: the relocation has no 'module'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load module:main\n", "r", relocs, error), error, "no 'to'");
    CHECK_ERROR(parse_dsd_relocations("kind:load to:0x4 module:main\n", "r", relocs, error), error, "no 'from'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 to:0x4 module:main\n", "r", relocs, error), error, "no 'kind'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:overlay_id\n", "r", relocs, error), error, "no 'to'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:jump to:0x4 module:main\n", "r", relocs, error), error,
                "unknown relocation kind 'jump'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:link_time_const(__HEAP) to:0x4\n", "r", relocs, error), error,
                "unknown link-time constant '__HEAP'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:overlays(3)\n", "r", relocs, error), error,
                "two or more");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:overlay(x)\n", "r", relocs, error), error,
                "overlay ID");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:overlay(65536)\n", "r", relocs, error), error,
                "overlay ID");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:overlays(1,,2)\n", "r", relocs, error), error,
                "overlay ID");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:main(1)\n", "r", relocs, error), error,
                "takes no options");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 module:arm7\n", "r", relocs, error), error,
                "unknown module 'arm7'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 size:4 module:main\n", "r", relocs, error), error,
                "unknown relocation attribute 'size'");
    CHECK_ERROR(parse_dsd_relocations("from:0x0 kind:load to:0x4 add:1.5 module:main\n", "r", relocs, error), error,
                "addend");
    CHECK_ERROR(parse_dsd_relocations("from:zero kind:load to:0x4 module:main\n", "r", relocs, error), error, "'from'");

    // finish(): sorted by address, a repeated line dropped, two different
    // relocations at one address refused.
    DsdModule module;
    CHECK(parse_dsd_relocations("from:0x20 kind:load to:0x4 module:main\nfrom:0x10 kind:load to:0x8 module:main\n"
                                "from:0x20 kind:load to:0x4 module:main\n",
                                "r", module.relocations, error));
    CHECK(module.finish(error) && module.relocations.size() == 2 && module.relocations[0].from == 0x10);
    CHECK(module.relocation_at(0x20) && module.relocation_at(0x20)->to == 4 && !module.relocation_at(0x18));
    CHECK(parse_dsd_relocations("from:0x20 kind:load to:0x4 module:main\nfrom:0x20 kind:load to:0x4 module:itcm\n",
                                "r", module.relocations, error));
    CHECK_ERROR(module.finish(error), error, "two different relocations from 0x00000020");
}

static void test_sections_and_sizes()
{
    std::string error;
    DsdModule module;
    // The top of a delinks.txt: options, the module's sections, then files.
    const std::string delinks =
        "    categories: Core engine\n"
        "    .text       start:0x02000000 end:0x02000100 kind:code align:32\n"
        "    .rodata     start:0x02000100 end:0x02000180 kind:rodata align:4\n"
        "    .data       start:0x02000180 end:0x02000200 kind:data align:32\n"
        "    .bss        start:0x02000200 end:0x02000300 kind:bss align:32\n"
        "\n"
        "src/Main/Actor/ActorType.cpp:\n"
        "    complete\n"
        "    .text       start:0x02000040 end:0x02000080\n"
        "    .bss        start:0x02000200 end:0x02000208\n";
    CHECK(parse_dsd_sections(delinks, "delinks.txt", module.sections, error));
    CHECK_EQ(module.sections.size(), 4);
    if (module.sections.size() == 4) {
        CHECK_STR(module.sections[0].name, ".text");
        CHECK(module.sections[0].kind == DsdSectionKind::Code && module.sections[1].kind == DsdSectionKind::Rodata);
        CHECK(module.sections[2].kind == DsdSectionKind::Data && module.sections[3].kind == DsdSectionKind::Bss);
        CHECK_EQ(module.sections[1].start, 0x02000100);
        CHECK_EQ(module.sections[1].end, 0x02000180);
        CHECK_EQ(module.sections[0].alignment, 32);
    }
    std::vector<DsdSection> sections;
    CHECK_ERROR(parse_dsd_sections("    .text start:0x0 end:0x4 kind:code\n", "d", sections, error), error, "needs start");
    CHECK_ERROR(parse_dsd_sections("    .text start:0x8 end:0x4 kind:code align:4\n", "d", sections, error), error,
                "ends before");
    CHECK_ERROR(parse_dsd_sections("    .text start:0x0 end:0x4 kind:text align:4\n", "d", sections, error), error,
                "'kind'");
    CHECK_ERROR(parse_dsd_sections("    .text start:0x0 end:0x4 kind:code align:4 flags:x\n", "d", sections, error), error,
                "unknown section attribute");
    CHECK(parse_dsd_sections("file.cpp:\n    .text start:0x0 end:0x4\n", "d", sections, error) && sections.empty());

    // Open-ended data runs to the next symbol or the end of its section,
    // whichever comes first.
    CHECK(parse_dsd_symbols("f2 kind:function(thumb,size=0x6) addr:0x02000020\n"
                            "f1 kind:function(arm,size=0x20) addr:0x02000000\n"
                            "label kind:label(arm) addr:0x02000010\n"
                            "ro1 kind:data(any) addr:0x02000100\n"
                            "ro2 kind:data(word) addr:0x02000140\n"
                            "ro3 kind:data(any) addr:0x02000150\n"
                            "d1 kind:data(short[]) addr:0x02000180\n"
                            "b1 kind:bss addr:0x02000200\n"
                            "b2 kind:bss(size=0x2) addr:0x02000280\n"
                            "b3 kind:bss addr:0x02000284\n"
                            "stray kind:data(any) addr:0x02000400\n",
                            "s", module.symbols, error));
    CHECK(module.finish(error));
    CHECK_EQ(module.functions.size(), 2);
    CHECK_EQ(module.data.size(), 8);
    if (module.functions.size() == 2 && module.data.size() == 8) {
        CHECK_STR(module.functions[0].name, "f1"); // sorted by address
        CHECK(!module.functions[0].thumb && module.functions[0].size == 0x20);
        CHECK(module.functions[1].thumb && module.functions[1].address == 0x02000020);
        CHECK(module.function_at(0x02000020) == &module.functions[1] && module.function_at(0x02000022) == nullptr);
        CHECK_EQ(module.data[0].size, 0x40); // ro1: to ro2
        CHECK_EQ(module.data[1].size, 4);    // ro2: stated
        CHECK_EQ(module.data[2].size, 0x30); // ro3: to the end of .rodata, not to d1
        CHECK_EQ(module.data[3].size, 0x80); // d1: to the end of .data
        CHECK(module.data[3].type == DsdDataType::Short && !module.data[3].bss);
        CHECK(module.data[4].bss && module.data[4].size == 0x80); // b1: to b2
        CHECK_EQ(module.data[5].size, 2);
        CHECK_EQ(module.data[6].size, 0x7C); // b3: to the end of .bss
        CHECK_EQ(module.data[7].size, 0);    // outside every section, nothing after it: unknown
        CHECK_STR(module.data[7].name, "stray");
        CHECK(module.section_at(0x020001FF) == &module.sections[2] && module.section_at(0x02000300) == nullptr);
    }
}

static void test_config()
{
    DsdConfig config;
    std::string error;
    const std::string yaml =
        "# what `dsd init` writes, plus the forms an autoload of unknown kind takes\n"
        "rom_config: '../../../extract/usa/config.yaml'\n"
        "build_path: '../../../build/usa'\n"
        "delinks_path: \"../../../build/usa/delinks\"\n"
        "enable_dsprot: true\n"
        "main_module:\n"
        "  name: main\n"
        "  object: '../../../build/usa/build/arm9.bin'\n"
        "  hash: '15560e62cc17dc7a'\n"
        "  delinks: delinks.txt\n"
        "  symbols: symbols.txt\n"
        "  relocations: relocs.txt\n"
        "autoloads:\n"
        "- kind: Itcm\n"
        "  name: itcm\n"
        "  object: '../../../build/usa/build/itcm.bin'\n"
        "  hash: deae433dc838b912\n"
        "  delinks: itcm/delinks.txt\n"
        "  symbols: itcm/symbols.txt\n"
        "  relocations: itcm/relocs.txt\n"
        "- kind: Dtcm\n"
        "  name: dtcm\n"
        "  hash: c6da5bb10005c16c   # a comment\n"
        "  symbols: dtcm/symbols.txt\n"
        "  relocations: dtcm/relocs.txt\n"
        "- kind: !Unknown 2\n"
        "  name: autoload_2\n"
        "  hash: '1'\n"
        "  symbols: autoload_2/symbols.txt\n"
        "  relocations: autoload_2/relocs.txt\n"
        "- kind:\n"
        "    Unknown: 3\n"
        "  name: autoload_3\n"
        "  hash: 'ABCDEF0123456789'\n"
        "  symbols: autoload_3/symbols.txt\n"
        "  relocations: autoload_3/relocs.txt\n"
        "overlays:\n"
        "- id: 0\n"
        "  name: ov000\n"
        "  object: '../../../build/usa/build/arm9_ov000.bin'\n"
        "  hash: '42f62ed35683d09b'\n"
        "  delinks: overlays/ov000/delinks.txt\n"
        "  symbols: overlays/ov000/symbols.txt\n"
        "  relocations: overlays/ov000/relocs.txt\n"
        "- id: 61\n"
        "  signed: true\n"
        "  name: ov061\n"
        "  hash: \"44139084df396b2b\"\n"
        "  symbols: 'it''s/symbols.txt'\n"
        "  relocations: overlays/ov061/relocs.txt\n";
    CHECK(parse_dsd_config(yaml, "config.yaml", config, error));
    CHECK_EQ(config.modules.size(), 7);
    CHECK_STR(config.rom_config, "../../../extract/usa/config.yaml");
    CHECK_STR(config.delinks_dir, "../../../build/usa/delinks");
    CHECK(config.enable_dsprot);
    if (config.modules.size() == 7) {
        const DsdModule &main_module = config.modules[0];
        CHECK(main_module.kind == DsdModuleKind::Main && main_module.name == "main");
        CHECK_EQ(main_module.hash, 0x15560e62cc17dc7aull);
        CHECK_STR(main_module.symbols_path, "symbols.txt");
        CHECK_STR(main_module.relocations_path, "relocs.txt");
        CHECK_STR(main_module.delinks_path, "delinks.txt");
        CHECK_STR(main_module.object_path, "../../../build/usa/build/arm9.bin");
        CHECK(config.modules[1].kind == DsdModuleKind::Itcm && config.modules[1].hash == 0xdeae433dc838b912ull);
        CHECK(config.modules[2].kind == DsdModuleKind::Dtcm && config.modules[2].hash == 0xc6da5bb10005c16cull);
        CHECK(config.modules[2].delinks_path.empty());
        CHECK(config.modules[3].kind == DsdModuleKind::Autoload && config.modules[3].autoload_index == 2);
        CHECK_EQ(config.modules[3].hash, 1);
        CHECK(config.modules[4].kind == DsdModuleKind::Autoload && config.modules[4].autoload_index == 3);
        CHECK_EQ(config.modules[4].hash, 0xABCDEF0123456789ull);
        CHECK(config.modules[5].kind == DsdModuleKind::Overlay && config.modules[5].overlay_id == 0);
        CHECK(!config.modules[5].overlay_signed);
        CHECK_STR(config.modules[5].symbols_path, "overlays/ov000/symbols.txt");
        CHECK(config.modules[6].overlay_id == 61 && config.modules[6].overlay_signed);
        CHECK_EQ(config.modules[6].hash, 0x44139084df396b2bull);
        CHECK_STR(config.modules[6].symbols_path, "it's/symbols.txt");
        CHECK(config.find(DsdModuleKind::Overlay, 61) == &config.modules[6]);
        CHECK(config.find(DsdModuleKind::Autoload, 3) == &config.modules[4]);
        CHECK(config.find(DsdModuleKind::Itcm) == &config.modules[1] && config.find(DsdModuleKind::Overlay, 5) == nullptr);
    }

    // A config without autoloads or overlays, as a game without them has.
    CHECK(parse_dsd_config("main_module:\n  name: main\n  hash: 0\n  symbols: s\n  relocations: r\n"
                           "autoloads: []\noverlays: []\n",
                           "c", config, error) &&
          config.modules.size() == 1 && !config.enable_dsprot);

    const std::string module = "  name: main\n  hash: 12\n  symbols: s\n  relocations: r\n";
    CHECK_ERROR(parse_dsd_config("overlays: []\n", "c.yaml", config, error), error, "no 'main_module'");
    CHECK_ERROR(parse_dsd_config("- a\n- b\n", "c", config, error), error, "no top-level mapping");
    CHECK_ERROR(parse_dsd_config("main_module:\n  name: main\n  symbols: s\n  relocations: r\n", "c", config, error), error,
                "'hash' is missing");
    CHECK_ERROR(parse_dsd_config("main_module:\n  name: main\n  hash: xyz\n  symbols: s\n  relocations: r\n", "c", config,
                                 error),
                error, "not a 64-bit hexadecimal number");
    CHECK_ERROR(parse_dsd_config("main_module:\n  name: main\n  hash: 11112222333344445\n  symbols: s\n  relocations: r\n",
                                 "c", config, error),
                error, "not a 64-bit hexadecimal number");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "overlays:\n- name: ov000\n  hash: 1\n  symbols: s\n"
                                                             "  relocations: r\n",
                                 "c", config, error),
                error, "no 16-bit 'id'");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "autoloads:\n- kind: Wram\n  name: x\n  hash: 1\n"
                                                             "  symbols: s\n  relocations: r\n",
                                 "c", config, error),
                error, "'kind' is not");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "  name: again\n", "c", config, error), error,
                "c:6: the key 'name' appears twice");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "overlays: [1, 2]\n", "c", config, error), error,
                "flow collections");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "note: |\n  text\n", "c", config, error), error,
                "block scalars");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "note: *anchor\n", "c", config, error), error, "anchors");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "note: 'open\n", "c", config, error), error, "not closed");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "note: 'a' b\n", "c", config, error), error,
                "after the closing quote");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "    deeper: 1\n", "c", config, error), error,
                "unexpected indentation");
    CHECK_ERROR(parse_dsd_config("main_module:\n" + module + "just text\n", "c", config, error), error,
                "expected 'key: value'");
    CHECK_ERROR(parse_dsd_config("main_module:\n\tname: main\n", "c", config, error), error, "tab");
    CHECK_ERROR(parse_dsd_config("main_module: main\n", "c", config, error), error, "expected a module");
    // Nesting has a limit instead of a stack overflow.
    std::string deep;
    for (int i = 0; i < 200; i++) deep += std::string(static_cast<size_t>(i), ' ') + "k:\n";
    CHECK_ERROR(parse_dsd_config(deep, "c", config, error), error, "nested too deeply");
    CHECK_ERROR(config.load("no/such/directory/anywhere", error), error, "cannot open");
}

/* ------------------------------------------------------------------ */
/* Relocations against the bytes they describe                         */
/* ------------------------------------------------------------------ */

static Module make_module(ModuleKind kind, const char *name, uint32_t address, size_t size, uint32_t bss_size, int index)
{
    Module module;
    module.kind = kind;
    module.name = name;
    module.load_address = address;
    module.bytes.assign(size, 0);
    module.bss_size = bss_size;
    if (module_is_overlay(kind)) module.overlay_id = index;
    else module.autoload_index = index;
    return module;
}

static void finish_module(Module &module)
{
    module.hash = fxhash64(module.bytes.data(), module.bytes.size());
    module.dsd_hash = module.hash;
}

// The verdict on one relocation line.
static RelocCheck verdict(const ModuleSet &set, const Module &module, const char *line, std::string &detail)
{
    std::vector<DsdRelocation> relocs;
    std::string error;
    if (!parse_dsd_relocations(line, "line", relocs, error) || relocs.size() != 1) {
        std::printf("FAIL: cannot parse \"%s\": %s\n", line, error.c_str());
        g_failures++;
        return RelocCheck::Bad;
    }
    detail.clear();
    return check_relocation(set, module, relocs[0], detail);
}

static void test_check()
{
    ModuleSet set;
    set.modules.push_back(make_module(ModuleKind::Arm9Main, "main", 0x02000000, 0x200, 0x100, -1));
    set.modules.push_back(make_module(ModuleKind::Itcm, "itcm", 0x01FF8000, 0x40, 0, 0));
    set.modules.push_back(make_module(ModuleKind::Dtcm, "dtcm", 0x027E0000, 0x10, 0x30, 1));
    set.modules.push_back(make_module(ModuleKind::Arm9Overlay, "ov007", 0x02100000, 0x20, 0x20, 7));
    set.modules.push_back(make_module(ModuleKind::Arm9Overlay, "ov008", 0x02100000, 0x30, 0, 8));
    set.modules.push_back(make_module(ModuleKind::Arm7Main, "arm7", 0x02380000, 0x10, 0, -1));
    set.modules.push_back(make_module(ModuleKind::Arm9Autoload, "autoload_2", 0x02400000, 0x10, 0, 2));
    std::vector<uint8_t> &code = set.modules[0].bytes;
    // Instructions and words written by hand from the ARM encodings.
    put32(code, 0x00, 0xEB00000E); // bl   0x02000040   (0x00 + 8 + 0x0E * 4)
    put32(code, 0x04, 0xFB000011); // blx  0x02000052   (0x04 + 8 + 0x11 * 4 + 2: bit 24 is the half word)
    put32(code, 0x08, 0x02000100); // .word main + 0x100
    put32(code, 0x0C, 0x01FF8010); // .word itcm + 0x10
    put16(code, 0x10, 0xF000);     // bl   0x02000060   (0x10 + 4 + 0x26 * 2), Thumb
    put16(code, 0x12, 0xF826);
    put16(code, 0x14, 0xF000);     // blx  0x02000040   (0x14 + 4 + 0x14 * 2), Thumb to ARM
    put16(code, 0x16, 0xE814);
    put32(code, 0x18, 0xEBFFFFF8); // bl   0x02000000   (backwards)
    put32(code, 0x1C, 7);          // overlay ID 7
    put32(code, 0x20, 0xEA000006); // b    0x02000040
    put32(code, 0x24, 0x02000108); // .word main + 0x100 + 8
    put32(code, 0x28, 0x12345678); // a link-time constant
    put32(code, 0x2C, 0x020000F0); // .word main + 0x100 - 0x10
    put32(code, 0x30, 0x1B000003); // blne 0x02000044
    put32(code, 0x34, 0x02100034); // .word into overlay 7's BSS, past the end of overlay 8
    put32(code, 0x38, 0xEB03FFF0); // bl   0x02100000   (0x38 + 8 + 0x3FFF0 * 4): into the overlays
    put32(code, 0x3C, 0x02000300); // .word one past main's BSS
    put16(code, 0x42, 0xF7FF);     // bl   0x02000002   (0x42 + 4 - 0x1000 + 0x7DE * 2), Thumb, backwards
    put16(code, 0x44, 0xFFDE);
    put16(code, 0x46, 0xF000);     // blx  from 0x46: 0x46 + 4 + 0x1A * 2 = 0x7E, which lands on 0x7C
    put16(code, 0x48, 0xE81A);
    put32(code, 0x4C, 0x02100010); // .word into both overlays
    put32(code, 0x50, 0x027E0020); // .word into dtcm's BSS
    put32(code, 0x54, 0x02100030); // .word one past overlay 8
    put32(code, 0x58, 0x02400008); // .word into the autoload block with index 2
    put32(code, 0x5C, 0xFFFFFFFF); // not an overlay ID
    for (Module &module : set.modules) finish_module(module);
    const Module &main_module = set.modules[0];
    std::string detail;

    CHECK(verdict(set, main_module, "from:0x02000000 kind:arm_call to:0x02000040 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000004 kind:arm_call_thumb to:0x02000052 module:main", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000008 kind:load to:0x02000100 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x0200000c kind:load to:0x01ff8010 module:itcm", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000010 kind:thumb_call to:0x02000060 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000014 kind:thumb_call_arm to:0x02000040 module:main", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000018 kind:arm_call to:0x02000000 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x0200001c kind:overlay_id to:7", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000020 kind:arm_branch to:0x02000040 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000024 kind:load to:0x02000100 add:0x8 module:main", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000028 kind:link_time_const(__CODE_HI)", detail) == RelocCheck::Unverifiable);
    CHECK(verdict(set, main_module, "from:0x0200002c kind:load to:0x02000100 add:-0x10 module:main", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000030 kind:arm_call to:0x02000044 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000034 kind:load to:0x02100034 module:overlay(7)", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000038 kind:arm_call to:0x02100000 module:overlays(7,8)", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x0200003c kind:load to:0x02000300 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000042 kind:thumb_call to:0x02000002 module:main", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000046 kind:thumb_call_arm to:0x0200007c module:main", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x0200004c kind:load to:0x02100010 module:overlays(7,8)", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000050 kind:load to:0x027e0020 module:dtcm", detail) == RelocCheck::Verified);
    // A pointer to one past a module's last byte still belongs to it.
    CHECK(verdict(set, main_module, "from:0x02000054 kind:load to:0x02100030 module:overlay(8)", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000008 kind:load to:0x02000100 module:none", detail) == RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000058 kind:load to:0x02400008 module:autoload(2)", detail) ==
          RelocCheck::Verified);
    // An index too large to be one does not mean "any module of that kind".
    CHECK(verdict(set, main_module, "from:0x02000058 kind:load to:0x02400008 module:autoload(4294967295)", detail) ==
          RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x0200005c kind:overlay_id to:4294967295", detail) == RelocCheck::Bad);
    CHECK(detail.find("does not have") != std::string::npos);

    // Each way a relocation can disagree with the bytes.
    CHECK(verdict(set, main_module, "from:0x02000000 kind:arm_call to:0x02000044 module:main", detail) == RelocCheck::Bad);
    CHECK(detail.find("leads to 0x02000040") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x02000000 kind:arm_call_thumb to:0x02000040 module:main", detail) == RelocCheck::Bad);
    CHECK(detail.find("is not the branch") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x02000004 kind:arm_call to:0x02000052 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000020 kind:arm_call to:0x02000040 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000000 kind:arm_branch to:0x02000040 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000008 kind:arm_call to:0x02000100 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000010 kind:thumb_call_arm to:0x02000060 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000014 kind:thumb_call to:0x02000040 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000046 kind:thumb_call_arm to:0x0200007e module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000008 kind:load to:0x02000104 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000024 kind:load to:0x02000100 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x0200001c kind:overlay_id to:8", detail) == RelocCheck::Bad);
    CHECK(detail.find("not the overlay ID 8") != std::string::npos);
    put32(set.modules[0].bytes, 0x1C, 9);
    CHECK(verdict(set, main_module, "from:0x0200001c kind:overlay_id to:9", detail) == RelocCheck::Bad);
    CHECK(detail.find("does not have") != std::string::npos);
    put32(set.modules[0].bytes, 0x1C, 7);
    // The right bytes, the wrong module.
    CHECK(verdict(set, main_module, "from:0x02000008 kind:load to:0x02000100 module:itcm", detail) == RelocCheck::Bad);
    CHECK(detail.find("outside itcm") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x0200000c kind:load to:0x01ff8010 module:main", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x02000034 kind:load to:0x02100034 module:overlays(7,8)", detail) == RelocCheck::Bad);
    CHECK(detail.find("outside overlay 8") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x02000034 kind:load to:0x02100034 module:overlay(9)", detail) == RelocCheck::Bad);
    CHECK(detail.find("overlay 9, which the ROM does not have") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x02000034 kind:load to:0x02100034 module:autoload(2)", detail) == RelocCheck::Bad);
    // A call must land in initialised bytes; overlay 7 has 0x20 of them.
    put32(set.modules[0].bytes, 0x38, 0xEB03FFFA); // bl 0x02100028
    CHECK(verdict(set, main_module, "from:0x02000038 kind:arm_call to:0x02100028 module:overlay(8)", detail) ==
          RelocCheck::Verified);
    CHECK(verdict(set, main_module, "from:0x02000038 kind:arm_call to:0x02100028 module:overlay(7)", detail) == RelocCheck::Bad);
    put32(set.modules[0].bytes, 0x38, 0xEB03FFF0);
    // Outside the module's bytes, including by one byte at its end.
    CHECK(verdict(set, main_module, "from:0x020001fd kind:load to:0x0 module:none", detail) == RelocCheck::Bad);
    CHECK(detail.find("outside the module's bytes") != std::string::npos);
    CHECK(verdict(set, main_module, "from:0x01fffffc kind:load to:0x0 module:none", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0xfffffffe kind:thumb_call to:0x0 module:none", detail) == RelocCheck::Bad);
    CHECK(verdict(set, main_module, "from:0x020001fc kind:load to:0x0 module:none", detail) == RelocCheck::Verified);

    // A whole configuration against the set.
    DsdConfig config;
    std::string error;
    auto add = [&](DsdModuleKind kind, const char *name, int index, uint64_t hash, const char *symbols,
                   const char *relocs) {
        DsdModule module;
        module.kind = kind;
        module.name = name;
        if (kind == DsdModuleKind::Overlay) module.overlay_id = index;
        if (kind == DsdModuleKind::Autoload) module.autoload_index = index;
        module.hash = hash;
        if (!parse_dsd_symbols(symbols, name, module.symbols, error) ||
            !parse_dsd_relocations(relocs, name, module.relocations, error) || !module.finish(error)) {
            std::printf("FAIL: test configuration for %s: %s\n", name, error.c_str());
            g_failures++;
        }
        config.modules.push_back(std::move(module));
    };
    add(DsdModuleKind::Main, "main", -1, set.modules[0].dsd_hash,
        "start kind:function(arm,size=0x10) addr:0x02000000\n"
        "thumb_fn kind:function(thumb,size=0x8) addr:0x02000010\n"
        "tail kind:function(arm,size=0x4) addr:0x0200000c\n"
        "table kind:data(word[4]) addr:0x02000100\n"
        "state kind:bss addr:0x02000200\n"
        "here kind:label(arm) addr:0x02000040\n",
        "from:0x02000000 kind:arm_call to:0x02000040 module:main\n"
        "from:0x02000008 kind:load to:0x02000100 module:main\n"
        "from:0x02000010 kind:thumb_call to:0x02000060 module:main\n"
        "from:0x0200001c kind:overlay_id to:7\n"
        "from:0x02000028 kind:link_time_const(__CODE_HI)\n");
    add(DsdModuleKind::Itcm, "itcm", -1, set.modules[1].dsd_hash, "", "");
    add(DsdModuleKind::Dtcm, "dtcm", -1, set.modules[2].dsd_hash, "dtcm_bss kind:bss addr:0x027e0010\n", "");
    add(DsdModuleKind::Autoload, "autoload_2", 2, set.modules[6].dsd_hash, "", "");
    add(DsdModuleKind::Overlay, "ov007", 7, set.modules[3].dsd_hash, "", "");
    add(DsdModuleKind::Overlay, "ov008", 8, set.modules[4].dsd_hash, "", "");
    DsdCheck result = check_dsd_config(set, config);
    CHECK(result.ok());
    CHECK_EQ(result.hashes_matched(), 6);
    CHECK_EQ(result.modules.size(), 6);
    CHECK(result.not_in_config.empty()); // the ARM7 module needs no entry: dsd describes the ARM9 only
    if (result.modules.size() == 6) {
        const DsdModuleCheck &row = result.modules[0];
        CHECK(row.module == &set.modules[0] && row.config == &config.modules[0] && row.hash_ok && row.ok());
        CHECK_EQ(row.arm_functions, 2);
        CHECK_EQ(row.thumb_functions, 1);
        CHECK_EQ(row.labels, 1);
        CHECK_EQ(row.data_symbols, 1);
        CHECK_EQ(row.bss_symbols, 1);
        CHECK_EQ(row.calls, 2);
        CHECK_EQ(row.loads, 1);
        CHECK_EQ(row.other_relocations, 2);
        CHECK_EQ(row.relocations_verified, 4);
        CHECK_EQ(row.relocations_unverifiable, 1);
        CHECK_EQ(row.relocations_bad, 0);
        CHECK_EQ(row.symbols_bad, 0);
        CHECK_EQ(row.overlapping_functions, 1); // start runs into tail, which is allowed
        CHECK(row.problems.empty());
    }

    // The hash a configuration is held to is that of ds-rom's file, which for
    // a compressed ARM9 program is not the hash of what is in RAM.
    set.modules[0].dsd_hash ^= 0x10;
    CHECK(!check_dsd_config(set, config).ok());
    config.modules[0].hash = set.modules[0].dsd_hash;
    CHECK(check_dsd_config(set, config).ok() && set.modules[0].dsd_hash != set.modules[0].hash);

    // One wrong hash, one relocation that does not match, symbols outside
    // their module: each makes the check fail and says where.
    config.modules[1].hash ^= 1;
    result = check_dsd_config(set, config);
    CHECK(!result.ok() && result.hashes_matched() == 5 && !result.modules[1].hash_ok && result.modules[0].ok());
    CHECK(result.modules[1].problems.size() == 1 && result.modules[1].problems[0].find("hash") != std::string::npos);
    config.modules[1].hash ^= 1;

    CHECK(parse_dsd_relocations("from:0x02000008 kind:load to:0x02000104 module:main\n", "r", config.modules[0].relocations,
                                error));
    result = check_dsd_config(set, config);
    CHECK(!result.ok() && result.modules[0].relocations_bad == 1 && result.hashes_matched() == 6);
    config.modules[0].relocations.clear();

    const char *misplaced[] = {
        "f kind:function(arm,size=0x10) addr:0x020001f8\n",  // runs past the end of the bytes
        "f kind:function(arm,size=0x4) addr:0x02000002\n",   // ARM code on a half word
        "f kind:function(thumb,size=0x4) addr:0x02000001\n", // Thumb code on an odd address
        "d kind:data(any) addr:0x02000204\n",                // data in the BSS
        "b kind:bss addr:0x020001fc\n",                      // BSS in the data
        "b kind:bss addr:0x02000304\n",                      // BSS past the BSS
        "l kind:label(arm) addr:0x02000200\n",               // a label outside the bytes
    };
    size_t refused = 0;
    for (const char *line : misplaced) {
        std::vector<DsdSymbol> kept = config.modules[0].symbols;
        std::vector<DsdSymbol> extra;
        if (!parse_dsd_symbols(line, "s", extra, error)) continue;
        config.modules[0].symbols.push_back(extra[0]);
        result = check_dsd_config(set, config);
        if (!result.ok() && result.modules[0].symbols_bad == 1 && result.modules[0].problems.size() == 1) refused++;
        config.modules[0].symbols = kept;
    }
    CHECK_EQ(refused, sizeof misplaced / sizeof misplaced[0]);
    CHECK(check_dsd_config(set, config).ok());

    // A module the ROM does not have, and one the configuration lacks.
    add(DsdModuleKind::Overlay, "ov009", 9, 0, "", "");
    result = check_dsd_config(set, config);
    CHECK(!result.ok() && result.modules.back().module == nullptr && !result.modules.back().ok());
    config.modules.pop_back();
    config.modules.pop_back(); // ov008 is now undescribed
    result = check_dsd_config(set, config);
    CHECK(!result.ok() && result.not_in_config.size() == 1 && result.not_in_config[0] == &set.modules[4]);
    config.modules.clear();
    CHECK(!check_dsd_config(set, config).ok()); // an empty configuration proves nothing
}

int main()
{
    test_blz();
    test_hashes();
    test_rom();
    test_symbols();
    test_relocations();
    test_sections_and_sizes();
    test_config();
    test_check();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        std::printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
