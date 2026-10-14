#include "image.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "gc_disc.h"
}

namespace gcrecomp {
namespace {

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint32_t)p[0] << 8) | p[1]); }

bool read_file(const std::string &path, std::vector<uint8_t> *out, std::string *error) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    *error = "cannot open " + path;
    return false;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    fclose(file);
    *error = path + " is empty";
    return false;
  }
  out->resize((size_t)size);
  const size_t got = fread(out->data(), 1, out->size(), file);
  fclose(file);
  if (got != out->size()) {
    *error = "short read on " + path;
    return false;
  }
  return true;
}

/* A DOL is a table of up to seven text sections and eleven data sections,
 * each with a file offset, a load address and a length, and then the BSS
 * and the entry point. There is no header magic at all: the format is
 * recognised by its own consistency, which is why this checks that the
 * entry point lands inside a section it just loaded. */
bool read_dol(const std::vector<uint8_t> &bytes, Image *out, std::string *error) {
  if (bytes.size() < 0x100) {
    *error = "too small to be a DOL";
    return false;
  }
  for (unsigned i = 0; i < 18u; i++) {
    const uint32_t offset = be32(&bytes[i * 4]);
    const uint32_t address = be32(&bytes[0x48 + i * 4]);
    const uint32_t length = be32(&bytes[0x90 + i * 4]);
    if (!length || !address) continue;
    if ((size_t)offset + length > bytes.size()) {
      *error = "a DOL section runs past the end of the file";
      return false;
    }
    Segment segment;
    segment.address = address;
    segment.executable = i < 7u;
    segment.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + length);
    out->segments.push_back(std::move(segment));
  }
  out->entry = be32(&bytes[0xe0]);
  if (out->segments.empty()) {
    *error = "no sections";
    return false;
  }
  return true;
}

/* The program headers of a big-endian 32-bit PowerPC ELF. The section
 * table is not needed: what is loaded is what the program headers say. */
bool read_elf(const std::vector<uint8_t> &bytes, Image *out, std::string *error) {
  if (bytes.size() < 52 || bytes[4] != 1 || bytes[5] != 2) {
    *error = "not a 32-bit big-endian ELF";
    return false;
  }
  out->entry = be32(&bytes[24]);
  {
    const uint32_t table = be32(&bytes[28]);
    const unsigned entry_size = be16(&bytes[42]);
    const unsigned count = be16(&bytes[44]);
    unsigned i;
    for (i = 0; i < count; i++) {
      const uint8_t *header = &bytes[table + (size_t)i * entry_size];
      if ((size_t)table + (size_t)(i + 1) * entry_size > bytes.size()) break;
      if (be32(header) != 1u) continue; /* PT_LOAD */
      {
        const uint32_t offset = be32(header + 4);
        const uint32_t address = be32(header + 8);
        const uint32_t file_size = be32(header + 16);
        const uint32_t flags = be32(header + 24);
        if (!file_size) continue;
        if ((size_t)offset + file_size > bytes.size()) {
          *error = "an ELF segment runs past the end of the file";
          return false;
        }
        Segment segment;
        segment.address = address;
        segment.executable = (flags & 1u) != 0;
        segment.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + file_size);
        out->segments.push_back(std::move(segment));
      }
    }
  }
  if (out->segments.empty()) {
    *error = "no loadable segments";
    return false;
  }
  return true;
}

}  // namespace

bool Image::word(uint32_t address, uint32_t *out) const {
  for (const Segment &segment : segments) {
    if (address < segment.address) continue;
    const uint32_t offset = address - segment.address;
    if (offset + 4u > segment.bytes.size()) continue;
    *out = be32(&segment.bytes[offset]);
    return true;
  }
  return false;
}

bool Image::executable_at(uint32_t address) const {
  for (const Segment &segment : segments) {
    if (!segment.executable) continue;
    if (address < segment.address) continue;
    if (address - segment.address + 4u <= segment.bytes.size()) return true;
  }
  return false;
}

/* A disc is not an executable; it contains one. The runtime already knows
 * how to open an image and where its DOL sits, including the compressed
 * form, so this asks it rather than learning the format twice.
 *
 * How long the DOL is, is not written down anywhere: it is however far the
 * last of its eighteen sections reaches. */
bool read_disc(const std::string &path, Image *out, std::string *error, const std::string &key_file) {
  GcDisc *disc = gc_disc_open(path.c_str(), nullptr);
  uint8_t header[0x100];
  uint32_t length = 0x100;
  bool ok;

  if (!disc) {
    *error = path + " is not an ELF, a DOL, or a disc this can read";
    return false;
  }
  /* A Wii disc opens without its key, and then nothing inside it reads.
   * That is worth saying in so many words rather than as a missing DOL. */
  if (gc_disc_is_locked(disc) && !key_file.empty()) gc_disc_load_key_file(disc, key_file.c_str(), stderr);
  if (gc_disc_is_locked(disc)) {
    *error = path + ": " + gc_disc_key_help();
    gc_disc_close(disc);
    return false;
  }
  if (gc_disc_read(disc, gc_disc_dol_offset(disc), header, sizeof(header)) != 0) {
    *error = "cannot read the DOL out of " + path;
    gc_disc_close(disc);
    return false;
  }
  for (unsigned i = 0; i < 18u; i++) {
    const uint32_t offset = be32(&header[i * 4]);
    const uint32_t size = be32(&header[0x90 + i * 4]);
    if (size && offset + size > length) length = offset + size;
  }
  out->raw.resize(length);
  ok = gc_disc_read(disc, gc_disc_dol_offset(disc), out->raw.data(), length) == 0;
  out->game_code = disc->game_code;
  out->wii = disc->is_wii != 0;
  {
    uint8_t key[16];
    if (gc_disc_title_key(disc, key) == 0) out->title_key.assign(key, key + 16);
  }
  gc_disc_close(disc);
  if (!ok) {
    *error = "the DOL in " + path + " runs past the end of the disc";
    return false;
  }
  return read_dol(out->raw, out, error);
}

bool read_image(const std::string &path, Image *out, std::string *error, const std::string &key_file) {
  uint8_t peek[0x20];
  std::vector<uint8_t> bytes;
  size_t got = 0;

  out->name = path;
  {
    /* Enough to tell the shapes apart, and no more: a disc is over a
     * gigabyte and reading all of it to look at four bytes would be a
     * memorable way to start. */
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
      *error = "cannot open " + path;
      return false;
    }
    got = fread(peek, 1, sizeof(peek), file);
    fclose(file);
  }
  if (got < 4) {
    *error = path + " is too short to be anything";
    return false;
  }

  /* A CISO says so in its first four bytes; an uncompressed disc has its
   * own magic a little further in. Either way it is a disc and contains an
   * executable rather than being one. */
  if ((peek[0] == 'C' && peek[1] == 'I' && peek[2] == 'S' && peek[3] == 'O') ||
      (got >= 0x20 && be32(&peek[0x1c]) == 0xc2339f3du)) {
    return read_disc(path, out, error, key_file);
  }
  /* And a Wii's, in any container its reader knows (it is asked, rather
   * than the list of containers being kept twice). */
  if (gc_disc_console(path.c_str()) != 0) return read_disc(path, out, error, key_file);

  if (!read_file(path, &bytes, error)) return false;
  out->raw = bytes;
  if (peek[0] == 0x7f && peek[1] == 'E' && peek[2] == 'L' && peek[3] == 'F') {
    return read_elf(bytes, out, error);
  }
  return read_dol(bytes, out, error);
}

}  // namespace gcrecomp
