#include "rom.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace ndsrecomp {

uint16_t crc16(const uint8_t *data, size_t size, uint16_t initial)
{
    uint32_t crc = initial;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) ? (crc >> 1) ^ 0xA001u : crc >> 1;
    }
    return static_cast<uint16_t>(crc);
}

std::string hex(uint64_t value, int digits)
{
    char text[32];
    std::snprintf(text, sizeof text, "0x%0*llx", digits, static_cast<unsigned long long>(value));
    return text;
}

const char *secure_area_name(SecureArea state)
{
    switch (state) {
    case SecureArea::None: return "none";
    case SecureArea::Decrypted: return "decrypted";
    case SecureArea::DecryptedId: return "decrypted, ID intact";
    case SecureArea::Blank: return "blank";
    case SecureArea::Encrypted: return "encrypted";
    }
    return "?";
}

namespace {

// Header text: up to the first NUL, anything unprintable shown as '?'.
std::string header_string(const uint8_t *p, size_t length)
{
    std::string out;
    for (size_t i = 0; i < length && p[i] != 0; i++) {
        out += (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '?';
    }
    return out;
}

bool check_program(const Rom &rom, const char *name, const ProgramHeader &program, std::string &error)
{
    if (program.size == 0) return true;
    if (!rom.ptr(program.rom_offset, program.size)) {
        error = std::string("the ") + name + " program (ROM offset " + hex(program.rom_offset) + ", " +
                hex(program.size) + " bytes) runs past the end of the image (" + hex(rom.data.size()) + " bytes)";
        return false;
    }
    if (static_cast<uint64_t>(program.load_address) + program.size > 0x100000000ull) {
        error = std::string("the ") + name + " program (load address " + hex(program.load_address) + ", " +
                hex(program.size) + " bytes) runs past the end of the address space";
        return false;
    }
    return true;
}

void detect_secure_area(Rom &rom)
{
    static const uint8_t destroyed_id[8] = {0xFF, 0xDE, 0xFF, 0xE7, 0xFF, 0xDE, 0xFF, 0xE7};
    static const uint8_t plain_id[8] = {'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'};
    const ProgramHeader &arm9 = rom.header.arm9;
    rom.secure_area = SecureArea::None;
    rom.secure_area_self_check = false;
    if (arm9.rom_offset < 0x4000 || arm9.rom_offset >= 0x8000 || arm9.size < 0x800) return;
    const uint8_t *area = rom.data.data() + arm9.rom_offset; // range checked by check_program
    if (std::memcmp(area, destroyed_id, 8) == 0) {
        rom.secure_area = SecureArea::Decrypted;
    } else if (std::memcmp(area, plain_id, 8) == 0) {
        rom.secure_area = SecureArea::DecryptedId;
    } else {
        // Encrypted data has about one zero byte in 256. Images that were
        // never encrypted leave the area zero-filled or put ordinary code in
        // it; the threshold is the one ds-rom uses for the same decision.
        size_t zeros = 0;
        for (size_t i = 0; i < 0x800; i++) {
            if (area[i] == 0) zeros++;
        }
        rom.secure_area = zeros >= 256 ? SecureArea::Blank : SecureArea::Encrypted;
        return;
    }
    rom.secure_area_self_check = get16(area + 0x0E) == crc16(area + 0x10, 0x7F0);
}

bool parse_fat(Rom &rom, std::string &error)
{
    const Header &h = rom.header;
    if (h.fat_size == 0) return true;
    if (h.fat_size % 8 != 0) {
        error = "the FAT size " + hex(h.fat_size) + " is not a multiple of 8";
        return false;
    }
    const uint8_t *fat = rom.ptr(h.fat_offset, h.fat_size);
    if (!fat) {
        error = "the FAT (ROM offset " + hex(h.fat_offset) + ", " + hex(h.fat_size) +
                " bytes) runs past the end of the image";
        return false;
    }
    const uint32_t count = h.fat_size / 8;
    if (count > kMaxFiles) {
        error = "the FAT has " + std::to_string(count) + " entries; file IDs end at " + std::to_string(kMaxFiles - 1);
        return false;
    }
    rom.files.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t start = get32(fat + i * 8);
        const uint32_t end = get32(fat + i * 8 + 4);
        FileEntry &file = rom.files[i];
        file.id = i;
        if (start <= end && end <= rom.data.size()) {
            file.offset = start;
            file.size = end - start;
            file.valid = true;
        }
    }
    return true;
}

// The FNT is a table of directories (8 bytes each: the offset of the
// directory's list of names, the ID of its first file, and its parent's ID,
// or for the root the number of directories), followed by those lists. A list
// entry is a length byte, the name, and for a sub-directory (bit 7 of the
// length byte) its 16-bit ID; file IDs are not stored, they count up from the
// directory's first.
//
// A hostile table could send this in circles or make it name the same file
// from many directories. Each directory is entered once and each file named
// once, so the work is bounded by the number of directories and files.
bool parse_fnt(Rom &rom, std::string &error)
{
    const Header &h = rom.header;
    rom.directories.clear();
    if (h.fnt_size == 0) return true;
    const uint8_t *fnt = rom.ptr(h.fnt_offset, h.fnt_size);
    if (!fnt) {
        error = "the FNT (ROM offset " + hex(h.fnt_offset) + ", " + hex(h.fnt_size) +
                " bytes) runs past the end of the image";
        return false;
    }
    if (h.fnt_size < 8) {
        error = "the FNT is " + std::to_string(h.fnt_size) + " bytes, too short for the root directory";
        return false;
    }
    const uint32_t directory_count = get16(fnt + 6);
    if (directory_count == 0 || directory_count > kMaxDirectories ||
        static_cast<uint64_t>(directory_count) * 8 > h.fnt_size) {
        error = "the FNT claims " + std::to_string(directory_count) + " directories in " + hex(h.fnt_size) + " bytes";
        return false;
    }
    rom.directories.assign(directory_count, std::string());
    std::vector<bool> entered(directory_count, false);
    std::vector<std::pair<uint32_t, std::string>> pending; // directory index, path prefix ("" or "dir/")
    pending.emplace_back(0u, std::string());
    entered[0] = true;

    while (!pending.empty()) {
        const uint32_t index = pending.back().first;
        const std::string prefix = std::move(pending.back().second);
        pending.pop_back();
        const std::string where = "FNT directory " + hex(0xF000u + index, 4);
        size_t pos = get32(fnt + index * 8);
        uint32_t file_id = get16(fnt + index * 8 + 4);
        for (;;) {
            if (pos >= h.fnt_size) {
                error = where + ": its list of names runs past the end of the FNT";
                return false;
            }
            const uint8_t type = fnt[pos++];
            if (type == 0) break;
            if (type == 0x80) {
                error = where + ": reserved entry type 0x80";
                return false;
            }
            const size_t length = type & 0x7Fu;
            if (length > h.fnt_size - pos) {
                error = where + ": a name runs past the end of the FNT";
                return false;
            }
            std::string name(reinterpret_cast<const char *>(fnt + pos), length);
            pos += length;
            if (type & 0x80u) {
                if (h.fnt_size - pos < 2) {
                    error = where + ": a sub-directory entry runs past the end of the FNT";
                    return false;
                }
                const uint32_t id = get16(fnt + pos);
                pos += 2;
                const uint32_t child = id & 0x0FFFu;
                if ((id & 0xF000u) != 0xF000u || child >= directory_count) {
                    error = where + ": sub-directory ID " + hex(id, 4) + " is not one of the " +
                            std::to_string(directory_count) + " directories";
                    return false;
                }
                if (entered[child]) {
                    error = where + ": directory " + hex(id, 4) + " is listed more than once";
                    return false;
                }
                entered[child] = true;
                rom.directories[child] = prefix + name;
                pending.emplace_back(child, prefix + name + "/");
            } else {
                if (file_id >= rom.files.size()) {
                    error = where + ": names file ID " + std::to_string(file_id) + ", but the FAT has " +
                            std::to_string(rom.files.size()) + " entries";
                    return false;
                }
                FileEntry &file = rom.files[file_id];
                if (!file.path.empty()) {
                    error = where + ": file ID " + std::to_string(file_id) + " is named more than once";
                    return false;
                }
                file.path = prefix + name;
                file_id++;
            }
        }
    }
    return true;
}

bool parse_overlay_table(const Rom &rom, const char *name, uint32_t offset, uint32_t size,
                         std::vector<OverlayEntry> &out, std::string &error)
{
    out.clear();
    if (size == 0) return true;
    if (size % 32 != 0) {
        error = std::string("the ") + name + " overlay table size " + hex(size) + " is not a multiple of 32";
        return false;
    }
    const uint8_t *table = rom.ptr(offset, size);
    if (!table) {
        error = std::string("the ") + name + " overlay table (ROM offset " + hex(offset) + ", " + hex(size) +
                " bytes) runs past the end of the image";
        return false;
    }
    const uint32_t count = size / 32;
    if (count > kMaxFiles) {
        error = std::string("the ") + name + " overlay table has " + std::to_string(count) +
                " entries, more than there can be files";
        return false;
    }
    out.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = table + i * 32;
        OverlayEntry &overlay = out[i];
        overlay.id = get32(e + 0x00);
        overlay.load_address = get32(e + 0x04);
        overlay.size = get32(e + 0x08);
        overlay.bss_size = get32(e + 0x0C);
        overlay.static_init_start = get32(e + 0x10);
        overlay.static_init_end = get32(e + 0x14);
        overlay.file_id = get32(e + 0x18);
        const uint32_t packed = get32(e + 0x1C);
        overlay.compressed_size = packed & 0x00FFFFFFu;
        overlay.flags = static_cast<uint8_t>(packed >> 24);
    }
    return true;
}

} // namespace

bool Rom::load(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::streamoff length = static_cast<std::streamoff>(in.tellg());
    if (length < static_cast<std::streamoff>(kHeaderSize) || static_cast<uint64_t>(length) > kMaxRomSize) {
        error = path + ": not a DS ROM (size " + std::to_string(length) + ")";
        return false;
    }
    data.resize(static_cast<size_t>(length));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(data.data()), length);
    if (!in || in.gcount() != length) {
        error = "cannot read " + path;
        return false;
    }
    if (!parse(error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool Rom::parse(std::string &error)
{
    header = Header();
    files.clear();
    directories.clear();
    arm9_overlays.clear();
    arm7_overlays.clear();
    warnings.clear();
    secure_area = SecureArea::None;
    secure_area_self_check = false;

    if (data.size() < kHeaderSize || data.size() > kMaxRomSize) {
        error = "not a DS ROM (size " + std::to_string(data.size()) + ")";
        return false;
    }
    const uint8_t *h = data.data();
    header.title = header_string(h + 0x000, 12);
    header.game_code = header_string(h + 0x00C, 4);
    header.maker_code = header_string(h + 0x010, 2);
    header.unit_code = h[0x012];
    header.seed_select = h[0x013];
    header.device_capacity = h[0x014];
    header.dsi_flags = h[0x01C];
    header.region = h[0x01D];
    header.rom_version = h[0x01E];
    header.autostart = h[0x01F];
    header.arm9 = {get32(h + 0x020), get32(h + 0x024), get32(h + 0x028), get32(h + 0x02C)};
    header.arm7 = {get32(h + 0x030), get32(h + 0x034), get32(h + 0x038), get32(h + 0x03C)};
    header.fnt_offset = get32(h + 0x040);
    header.fnt_size = get32(h + 0x044);
    header.fat_offset = get32(h + 0x048);
    header.fat_size = get32(h + 0x04C);
    header.arm9_overlay_offset = get32(h + 0x050);
    header.arm9_overlay_size = get32(h + 0x054);
    header.arm7_overlay_offset = get32(h + 0x058);
    header.arm7_overlay_size = get32(h + 0x05C);
    header.card_control_normal = get32(h + 0x060);
    header.card_control_key1 = get32(h + 0x064);
    header.icon_offset = get32(h + 0x068);
    header.secure_area_crc = get16(h + 0x06C);
    header.secure_area_delay = get16(h + 0x06E);
    header.arm9_autoload_hook = get32(h + 0x070);
    header.arm7_autoload_hook = get32(h + 0x074);
    header.secure_area_disable = get32(h + 0x078) | (static_cast<uint64_t>(get32(h + 0x07C)) << 32);
    header.rom_size = get32(h + 0x080);
    header.header_size = get32(h + 0x084);
    header.arm9_params_offset = get32(h + 0x088);
    header.arm7_params_offset = get32(h + 0x08C);
    header.logo_crc = get16(h + 0x15C);
    header.header_crc = get16(h + 0x15E);
    header.debug_rom_offset = get32(h + 0x160);
    header.debug_size = get32(h + 0x164);
    header.debug_load_address = get32(h + 0x168);
    header.logo_crc_computed = crc16(h + 0x0C0, 0x9C);
    header.header_crc_computed = crc16(h, 0x15E);
    if (!header.header_crc_ok()) {
        warnings.push_back("header CRC-16 is " + hex(header.header_crc, 4) + " in the file, " +
                           hex(header.header_crc_computed, 4) + " computed");
    }
    if (!header.logo_crc_ok()) {
        warnings.push_back("logo CRC-16 is " + hex(header.logo_crc, 4) + " in the file, " +
                           hex(header.logo_crc_computed, 4) + " computed");
    }

    if (!check_program(*this, "ARM9", header.arm9, error) || !check_program(*this, "ARM7", header.arm7, error)) {
        return false;
    }
    detect_secure_area(*this);
    if (!parse_fat(*this, error) || !parse_fnt(*this, error)) return false;
    return parse_overlay_table(*this, "ARM9", header.arm9_overlay_offset, header.arm9_overlay_size, arm9_overlays,
                               error) &&
           parse_overlay_table(*this, "ARM7", header.arm7_overlay_offset, header.arm7_overlay_size, arm7_overlays,
                               error);
}

const uint8_t *Rom::file_data(uint32_t id, uint32_t &size, std::string &error) const
{
    if (id >= files.size()) {
        error = "file ID " + std::to_string(id) + " is not in the FAT (" + std::to_string(files.size()) + " entries)";
        return nullptr;
    }
    const FileEntry &file = files[id];
    if (!file.valid) {
        error = "file ID " + std::to_string(id) + " has a FAT range outside the image";
        return nullptr;
    }
    size = file.size;
    return data.data() + file.offset;
}

const FileEntry *Rom::find_file(const std::string &path) const
{
    for (const FileEntry &file : files) {
        if (!file.path.empty() && file.path == path) return &file;
    }
    return nullptr;
}

size_t Rom::named_file_count() const
{
    size_t count = 0;
    for (const FileEntry &file : files) {
        if (!file.path.empty()) count++;
    }
    return count;
}

} // namespace ndsrecomp
