// SNES code image as seen by the recompiler: the ROM mapped by the runtime's
// own cartridge code (so LoROM/HiROM/ExHiROM mirrors match the game exactly),
// plus an optional WRAM image from a runtime trace for code the game runs
// from work RAM.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "snes.h"
}

namespace snesrecomp {

struct MachineDeleter {
    void operator()(SnesMachine *m) const { snes_destroy(m); }
};

class Rom {
public:
    bool load(const std::string &path, std::string &error);
    // WRAM image written next to a trace by the runtime (<trace>.wram).
    bool load_wram_image(const std::string &trace_path, std::string &error);

    // Can code run from this address? ROM, or WRAM when an image is loaded.
    bool is_code(uint32_t pc24) const;
    bool is_wram(uint32_t pc24) const;
    uint8_t read(uint32_t pc24) const { return snes_peek(machine_.get(), pc24 & 0xFFFFFFu); }
    // Byte at pc + i, wrapping within the bank as the program counter does.
    uint8_t read_pc(uint32_t pc24, uint32_t i) const
    {
        return read((pc24 & 0xFF0000u) | ((pc24 + i) & 0xFFFFu));
    }
    uint16_t read16(uint32_t addr) const
    {
        return static_cast<uint16_t>(read(addr) | (read((addr & 0xFF0000u) | ((addr + 1u) & 0xFFFFu)) << 8));
    }

    const SnesMachine *machine() const { return machine_.get(); }

    std::vector<uint8_t> data; // the ROM image as loaded (copier header dropped)
    std::string sha256;
    std::string title;
    bool has_wram = false;

private:
    std::unique_ptr<SnesMachine, MachineDeleter> machine_;
};

} // namespace snesrecomp
