/*
 * What the recompiler is given: an executable image and where its pieces
 * belong in the guest's address space.
 *
 * Three shapes reach this. A `.dol` is the format the console itself
 * loads. An ELF is what devkitPro produces and what every test program in
 * tests/gc/programs is, so it is worth reading directly - the phase gate
 * is that those programs give the same verdicts recompiled as
 * interpreted, and requiring a disc image to get there would put the gate
 * behind a commercial game. A disc image contains a DOL, and the runtime
 * already knows how to find it.
 */
#ifndef GCRECOMP_IMAGE_H
#define GCRECOMP_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp {

/* One loadable run of bytes, at the address the guest will see it. */
struct Segment {
  uint32_t address = 0;
  bool executable = false;
  std::vector<uint8_t> bytes;
};

struct Image {
  std::string name;          /* what it was read from */
  uint32_t entry = 0;
  std::vector<Segment> segments;
  /* The executable's own bytes, as they will be embedded. For a disc this
   * is the DOL that was pulled out of it, not the disc - the code is
   * embedded and the content is not. */
  std::vector<uint8_t> raw;
  /* The six-character id, when the image came from a disc that has one. */
  std::string game_code;
  /* A Wii program rather than a GameCube one: read off a disc that says
   * so, or said on the command line for an ELF or a DOL, which do not. */
  bool wii = false;
  /* A Wii disc's title key: sixteen bytes, or empty. The program that is
   * written carries it, so that it can read the disc it was built from
   * without the common key that was needed to find it (gc_disc.h). */
  std::vector<uint8_t> title_key;

  /* The word at a guest address, big-endian, or false when no segment
   * covers it. */
  bool word(uint32_t address, uint32_t *out) const;
  bool executable_at(uint32_t address) const;
};

/* Read a disc image, a .dol or an ELF. Returns false with `error` set.
 * `key_file`, when not empty, is where to find the common key for a Wii
 * disc that the usual places (gc_disc.h) did not open. */
bool read_image(const std::string &path, Image *out, std::string *error, const std::string &key_file = "");

}  // namespace gcrecomp

#endif
