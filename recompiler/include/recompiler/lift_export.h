/**
 * @file lift_export.h
 * @brief Machine-readable description of a ROM for lifting it to source
 *
 * The export is what a person or a model needs to turn recompiled functions
 * into readable C: every function with its blocks and disassembly, who calls
 * whom, which registers a function reads before it writes them, which fixed
 * memory addresses it touches, and which ROM bytes are not code. See
 * docs/LIFTING.md for the schema.
 */

#ifndef RECOMPILER_LIFT_EXPORT_H
#define RECOMPILER_LIFT_EXPORT_H

#include "analyzer.h"
#include "rom.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gbrecomp {

/**
 * @brief A call that reaches a function through a helper
 *
 * The code loads constants into registers and calls or jumps to a helper
 * that takes its target from them (it switches bank and jumps to HL, it
 * looks a number up in a table). `tools/gb_lift.py edges` finds where each
 * such call goes by running the helper; `--lift-edges` hands the result back.
 */
struct LiftFarEdge {
    uint32_t from = 0;        // the function the call is in, bank << 16 | address
    uint16_t site = 0;        // address of the CALL or jump to the helper
    bool is_jump = false;
    uint32_t helper = 0;      // the helper, bank << 16 | address
    uint32_t target = 0;      // where control ends up, bank << 16 | address
    uint16_t returns_to = 0;  // where the target returns: in the helper, or 0 for its caller
    uint16_t left_at = 0;     // the computed jump that went to the target, or 0 if not said
    // The helper took the target from a table: this is one of its entries.
    bool from_table = false;
    uint32_t table = 0;       // the table, bank << 16 | address
    uint16_t table_index = 0;
    // Seen in a run of the game (`--evidence`), not worked out from the ROM:
    // true of that run, and the same call may go elsewhere in another.
    bool seen = false;
    // The site is the computed jump itself, not a call to a helper that
    // makes it. `from` is 0 when the jump is in code the analysis lacks.
    bool is_computed = false;
};

// A function a table names, in a table somebody declared (`gb_lift.py edges
// --tables`): where the table is and which of its entries this is. Nothing
// says who calls through the table.
struct LiftTableEntry {
    uint32_t target = 0;      // the function, bank << 16 | address
    uint32_t table = 0;       // the table, bank << 16 | address
    uint16_t index = 0;
    std::string name;         // what whoever declared the table called it
};

struct LiftExportOptions {
    // How the analysis was configured, recorded verbatim so that an export
    // can be reproduced (for example "pointer-scan=strict").
    std::string analysis_profile;

    // Function entries the user declared (a .sym file or --annotations), as
    // (bank << 16 | address). The export treats each as a function start.
    std::vector<uint32_t> declared_functions;

    // Calls made through helpers (`--lift-edges`). Each target is a function
    // start, with the call that reaches it listed.
    std::vector<LiftFarEdge> far_edges;

    // Addresses a run of the game arrived at by a call or a computed jump,
    // as bank << 16 | address. With any, each function says whether it ran.
    std::vector<uint32_t> entered;

    // Entries of declared tables (`--lift-edges`). Each target is a function
    // start, and says which entry of which table it is.
    std::vector<LiftTableEntry> table_entries;
};

/**
 * @brief Read a `gbrecomp.lift-edges` file
 *
 * Fails, with a message in @p error, on a malformed file or one made for
 * another ROM.
 */
bool load_lift_edges(const std::filesystem::path& path,
                     const uint8_t* rom_data,
                     size_t rom_size,
                     std::vector<LiftFarEdge>& edges,
                     std::vector<uint32_t>& entered,
                     std::vector<LiftTableEntry>& table_entries,
                     std::string& error);

/**
 * @brief Register and flag sets used by the export, one bit each
 */
enum LiftRegisterBit : uint16_t {
    LIFT_REG_B = 1u << 0,
    LIFT_REG_C = 1u << 1,
    LIFT_REG_D = 1u << 2,
    LIFT_REG_E = 1u << 3,
    LIFT_REG_H = 1u << 4,
    LIFT_REG_L = 1u << 5,
    LIFT_REG_A = 1u << 6,
    LIFT_REG_SP = 1u << 7,
    LIFT_FLAG_Z = 1u << 8,
    LIFT_FLAG_N = 1u << 9,
    LIFT_FLAG_H = 1u << 10,
    LIFT_FLAG_C = 1u << 11,
    LIFT_REG_ALL = 0x0fffu,
};

struct LiftInstructionEffect {
    uint16_t reads = 0;   // LiftRegisterBit set read by the instruction
    uint16_t writes = 0;  // LiftRegisterBit set written by the instruction
};

/**
 * @brief Registers and flags one instruction reads and writes
 *
 * Control transfers report only what the instruction itself touches (a
 * conditional jump reads its flag, a call writes SP); the callee's effect is
 * the caller's business.
 */
LiftInstructionEffect lift_instruction_effect(const Instruction& instr);

/**
 * @brief Build the export as a JSON document
 */
std::string build_lift_export_json(const ROM& rom,
                                   const AnalysisResult& analysis,
                                   const LiftExportOptions& options = {});

/**
 * @brief Write the export to a file
 * @return false, with a message in @p error, if the file cannot be written
 */
bool write_lift_export(const std::filesystem::path& path,
                       const ROM& rom,
                       const AnalysisResult& analysis,
                       const LiftExportOptions& options,
                       std::string* error);

} // namespace gbrecomp

#endif // RECOMPILER_LIFT_EXPORT_H
