#include "blz.h"

#include <cstdio>

namespace ndsrecomp {

namespace {

constexpr size_t kFooterSize = 8;
constexpr unsigned kMinMatch = 3;

std::string hex(uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace

bool blz_read_footer(const uint8_t *data, size_t size, BlzFooter &footer, std::string &error)
{
    if (size < kFooterSize) {
        error = "BLZ: " + std::to_string(size) + " bytes is too short to hold the 8-byte footer";
        return false;
    }
    const uint8_t *f = data + size - kFooterSize;
    footer.compressed_span = f[0] | (static_cast<uint32_t>(f[1]) << 8) | (static_cast<uint32_t>(f[2]) << 16);
    footer.footer_size = f[3];
    footer.extra_size = f[4] | (static_cast<uint32_t>(f[5]) << 8) | (static_cast<uint32_t>(f[6]) << 16) |
                        (static_cast<uint32_t>(f[7]) << 24);
    if (footer.footer_size < kFooterSize) {
        error = "BLZ: footer size " + std::to_string(footer.footer_size) + " is smaller than the footer itself";
        return false;
    }
    if (footer.footer_size > footer.compressed_span) {
        error = "BLZ: footer size " + std::to_string(footer.footer_size) + " exceeds the compressed region (" +
                hex(footer.compressed_span) + " bytes)";
        return false;
    }
    if (footer.compressed_span > size) {
        error = "BLZ: the compressed region (" + hex(footer.compressed_span) + " bytes) is larger than the file (" +
                hex(size) + " bytes)";
        return false;
    }
    // Seventeen stream bytes (a flag byte and eight pairs) produce at most
    // 144 bytes, so the footer cannot honestly claim more than this. Checked
    // before anything is allocated: extra_size is 32 bits of untrusted input.
    const uint64_t stream = footer.compressed_span - footer.footer_size;
    const uint64_t most = (stream / 17 + 1) * 144;
    if (static_cast<uint64_t>(footer.compressed_span) + footer.extra_size > most) {
        error = "BLZ: the footer claims " + hex(footer.extra_size) + " extra bytes, more than " + hex(stream) +
                " bytes of stream can produce";
        return false;
    }
    return true;
}

bool blz_decompress(const uint8_t *data, size_t size, std::vector<uint8_t> &out, std::string &error)
{
    BlzFooter footer;
    if (!blz_read_footer(data, size, footer, error)) return false;

    const size_t stop = size - footer.compressed_span; // first byte of the compressed region
    const size_t out_size = size + footer.extra_size;
    size_t src = size - footer.footer_size; // one past the next byte to read
    size_t dst = out_size;                  // one past the next byte to write

    // The console works in one buffer; here the input stays untouched and the
    // output starts as a copy of it, which is the same thing as long as no
    // write lands on a byte that is still to be read. That is checked below.
    out.assign(data, data + size);
    out.resize(out_size);

    while (src > stop) {
        unsigned flags = data[--src];
        for (int bit = 0; bit < 8; bit++, flags <<= 1) {
            if (src == stop) {
                if (bit == 0) {
                    error = "BLZ: the stream ends on a flag byte at " + hex(src);
                    return false;
                }
                break;
            }
            size_t count = 1;
            size_t distance = 0;
            if (flags & 0x80u) {
                if (src - stop < 2) {
                    error = "BLZ: the stream ends inside a length-distance pair at " + hex(src - 1);
                    return false;
                }
                const unsigned high = data[--src];
                const unsigned low = data[--src];
                count = (high >> 4) + kMinMatch;
                distance = (((high & 0x0Fu) << 8) | low) + kMinMatch;
                if (distance > out_size - dst) {
                    error = "BLZ: the pair at " + hex(src) + " copies from " + std::to_string(distance) +
                            " bytes above the write position, past the end of the output";
                    return false;
                }
            } else {
                src--;
            }
            if (count > dst - src) {
                error = "BLZ: the output overtakes the input at " + hex(src) +
                        " (the stream cannot be decompressed in place)";
                return false;
            }
            if (distance) {
                for (size_t i = 0; i < count; i++) {
                    dst--;
                    out[dst] = out[dst + distance];
                }
            } else {
                out[--dst] = data[src];
            }
        }
    }
    if (dst != stop) {
        error = "BLZ: the stream produced " + hex(out_size - dst) + " bytes, the footer promised " +
                hex(out_size - stop);
        return false;
    }
    return true;
}

} // namespace ndsrecomp
