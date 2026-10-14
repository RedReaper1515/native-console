// A Nintendo DS cartridge image as the recompiler reads it: the header, the
// NitroROM file system (FAT and FNT) and the two overlay tables.
//
// Layout reference: GBATEK, "DS Cartridge Header", "DS Cartridge Secure Area"
// and "DS Cartridge NitroROM and NitroARC File Systems".
//
// Every offset and size in the file is untrusted. Parsing checks each one
// against the file and fails with a message; nothing here indexes the image
// with a value it has not checked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndsrecomp {

// CRC-16 as the DS BIOS computes it (GetCRC16): reflected polynomial 0xA001.
// Every checksum in the cartridge starts from 0xFFFF (CRC-16/MODBUS).
uint16_t crc16(const uint8_t *data, size_t size, uint16_t initial = 0xFFFFu);

inline uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p)
{
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// "0x" and at least `digits` lower-case hex digits.
std::string hex(uint64_t value, int digits = 8);

constexpr size_t kHeaderSize = 0x200;        // the part of the header this reads
constexpr size_t kMaxRomSize = 0x20000000;   // 512 MiB, the largest cartridge made
constexpr uint32_t kMaxFiles = 0xF000;       // file IDs 0x0000-0xEFFF; 0xF000 and up are directories
constexpr uint32_t kMaxDirectories = 0x1000; // directory IDs 0xF000-0xFFFF

// One CPU's boot program: header 0x20 (ARM9) and 0x30 (ARM7).
struct ProgramHeader {
    uint32_t rom_offset = 0;
    uint32_t entry = 0;
    uint32_t load_address = 0;
    uint32_t size = 0;
};

struct Header {
    std::string title;      // 0x000, 12 bytes
    std::string game_code;  // 0x00C, 4 bytes
    std::string maker_code; // 0x010, 2 bytes
    uint8_t unit_code = 0;       // 0x012: 0 DS, 2 DS and DSi, 3 DSi only
    uint8_t seed_select = 0;     // 0x013
    uint8_t device_capacity = 0; // 0x014: the chip holds 128 KiB << n
    uint8_t dsi_flags = 0;       // 0x01C
    uint8_t region = 0;          // 0x01D
    uint8_t rom_version = 0;     // 0x01E
    uint8_t autostart = 0;       // 0x01F
    ProgramHeader arm9;          // 0x020
    ProgramHeader arm7;          // 0x030
    uint32_t fnt_offset = 0;     // 0x040
    uint32_t fnt_size = 0;
    uint32_t fat_offset = 0;     // 0x048
    uint32_t fat_size = 0;
    uint32_t arm9_overlay_offset = 0; // 0x050
    uint32_t arm9_overlay_size = 0;
    uint32_t arm7_overlay_offset = 0; // 0x058
    uint32_t arm7_overlay_size = 0;
    uint32_t card_control_normal = 0; // 0x060, the ROMCTRL value for normal commands
    uint32_t card_control_key1 = 0;   // 0x064
    uint32_t icon_offset = 0;         // 0x068, 0 when there is no icon and title
    uint16_t secure_area_crc = 0;     // 0x06C, of the secure area as the card sends it (encrypted)
    uint16_t secure_area_delay = 0;   // 0x06E
    // The address the SDK's start-up code returns through once the autoload
    // blocks are in place; the word before it is the address of that CPU's
    // module params. Zero in programs not built with the SDK.
    uint32_t arm9_autoload_hook = 0;  // 0x070
    uint32_t arm7_autoload_hook = 0;  // 0x074
    uint64_t secure_area_disable = 0; // 0x078
    uint32_t rom_size = 0;            // 0x080, bytes in use
    uint32_t header_size = 0;         // 0x084
    // ROM offsets of the module params (GBATEK: "unknown, some rom_offset").
    // DSi-era headers store the ARM9 one relative to the program instead.
    uint32_t arm9_params_offset = 0;  // 0x088
    uint32_t arm7_params_offset = 0;  // 0x08C
    uint16_t logo_crc = 0;            // 0x15C, of 0x0C0-0x15B; 0xCF56 for Nintendo's logo
    uint16_t header_crc = 0;          // 0x15E, of 0x000-0x15D
    uint32_t debug_rom_offset = 0;    // 0x160
    uint32_t debug_size = 0;          // 0x164
    uint32_t debug_load_address = 0;  // 0x168

    // Computed over the file. A mismatch is reported, never a reason to
    // refuse the ROM: homebrew and patched images often carry stale values.
    uint16_t logo_crc_computed = 0;
    uint16_t header_crc_computed = 0;
    bool logo_crc_ok() const { return logo_crc == logo_crc_computed; }
    bool header_crc_ok() const { return header_crc == header_crc_computed; }
};

// One FAT entry, with the name the FNT gives it.
struct FileEntry {
    uint32_t id = 0;     // index in the FAT
    uint32_t offset = 0; // ROM offset of the first byte
    uint32_t size = 0;
    // False for an entry whose range is inverted or runs past the end of the
    // image (offset and size are then zero). Using such a file is an error;
    // its presence alone is not.
    bool valid = false;
    // "dir/sub/name" from the FNT, with no leading slash. Empty for files the
    // FNT does not name, which is how overlays are stored. The bytes are the
    // ROM's own (ASCII by the specification, Shift-JIS in some games): they
    // are data, not a host path.
    std::string path;
};

// One 32-byte entry of an overlay table.
struct OverlayEntry {
    uint32_t id = 0;                // 0x00
    uint32_t load_address = 0;      // 0x04
    uint32_t size = 0;              // 0x08, initialised bytes once loaded
    uint32_t bss_size = 0;          // 0x0C, zero-filled bytes after them
    uint32_t static_init_start = 0; // 0x10, table of static initialiser pointers
    uint32_t static_init_end = 0;   // 0x14
    uint32_t file_id = 0;           // 0x18
    uint32_t compressed_size = 0;   // 0x1C bits 0-23: bytes to read when compressed
    uint8_t flags = 0;              // 0x1C bits 24-31
    bool compressed() const { return (flags & 0x01u) != 0; }
    bool authenticated() const { return (flags & 0x02u) != 0; } // HMAC-SHA1 in the ARM9 program
};

// What the first 2 KiB of the ARM9 program look like. The card sends them
// KEY1-encrypted; the BIOS decrypts them, checks the ID "encryObj" and
// overwrites it with 0xE7FFDEFF twice. Dumps normally hold that last state.
enum class SecureArea {
    None,        // the ARM9 program is not at ROM 0x4000-0x7FFF, or is shorter than 2 KiB
    Decrypted,   // starts with 0xE7FFDEFF 0xE7FFDEFF
    DecryptedId, // starts with "encryObj": decrypted, ID not yet destroyed
    Blank,       // mostly zero bytes: never encrypted (development and homebrew images)
    Encrypted,   // none of the above
};
const char *secure_area_name(SecureArea state);

struct Rom {
    std::vector<uint8_t> data;
    Header header;
    std::vector<FileEntry> files;         // one per FAT entry, index = file ID
    std::vector<std::string> directories; // index = directory ID & 0xFFF; [0] is the root, ""
    std::vector<OverlayEntry> arm9_overlays;
    std::vector<OverlayEntry> arm7_overlays;
    SecureArea secure_area = SecureArea::None;
    // For a decrypted secure area: bytes 0x0E-0x0F hold the CRC-16 of bytes
    // 0x10-0x7FF, which checks the decryption without any key.
    bool secure_area_self_check = false;
    // Findings that are reported but do not stop the load.
    std::vector<std::string> warnings;

    // Reads `path` and parses it.
    bool load(const std::string &path, std::string &error);
    // Parses `data`, which the caller has filled.
    bool parse(std::string &error);

    // `size` bytes at a ROM offset, or nullptr if any of them is outside the image.
    const uint8_t *ptr(uint64_t offset, uint64_t size) const
    {
        return offset <= data.size() && size <= data.size() - offset ? data.data() + offset : nullptr;
    }
    // The bytes of a file, or nullptr with `error` set.
    const uint8_t *file_data(uint32_t id, uint32_t &size, std::string &error) const;
    const FileEntry *find_file(const std::string &path) const;
    size_t named_file_count() const;
};

} // namespace ndsrecomp
