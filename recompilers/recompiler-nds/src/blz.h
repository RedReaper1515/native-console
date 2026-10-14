// BLZ: the Nintendo DS SDK's backwards LZ ("MI_UncompressBackward"), which
// compresses the ARM9 program and overlays.
//
// A BLZ file is decompressed in place by the game's own start-up code, from
// its last byte towards its first, so that the output can overwrite the input
// it has already consumed. The file ends in an 8-byte footer:
//
//   -8  u24  size of the compressed region, counted back from the end of the
//            file and including the footer and its padding; the bytes before
//            that region are stored as they are and stay where they are
//   -5  u8   size of the footer plus padding (8 to 11)
//   -4  u32  how many bytes the decompressed data is longer than the file
//
// The stream is read downwards from the padding. A flag byte gives the kind of
// the next eight tokens, most significant bit first: 0 is a literal byte, 1 is
// two bytes hhhh dddd dddddddd (the high byte is read first) that copy h + 3
// bytes from d + 3 bytes above the write position. Decompression ends when
// the read position reaches the start of the compressed region.
//
// Written from that format, which is taken from the SDK routine itself (the
// start-up code of any SDK game carries it) and from the description in
// AetiasHax/ds-rom, lib/src/compress/lz77.rs (MIT, Copyright (c) 2025 Aetias).
// No code is copied.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndsrecomp {

struct BlzFooter {
    uint32_t compressed_span = 0; // bytes at the end of the file that hold the stream, footer included
    uint32_t footer_size = 0;     // footer plus padding
    uint32_t extra_size = 0;      // decompressed size minus file size
};

// Reads and checks the footer of the `size` bytes at `data`.
bool blz_read_footer(const uint8_t *data, size_t size, BlzFooter &footer, std::string &error);

// Decompresses the `size` bytes at `data` into `out`, which receives the whole
// image: the stored prefix followed by the decompressed bytes
// (size + extra_size bytes). Every length in the stream is checked. A stream
// that the console could not decompress in place (its output would overwrite
// input not yet read) is refused, as is one that leaves a gap between the
// stored prefix and the output.
bool blz_decompress(const uint8_t *data, size_t size, std::vector<uint8_t> &out, std::string &error);

} // namespace ndsrecomp
