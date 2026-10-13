/**
 * @file lift_export.cpp
 * @brief Machine-readable description of a ROM for lifting it to source
 */

#include "recompiler/lift_export.h"

#include "recompiler/native_patch.h"

#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace gbrecomp {

namespace {

constexpr uint16_t kHL = LIFT_REG_H | LIFT_REG_L;
constexpr uint16_t kFlags = LIFT_FLAG_Z | LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C;
// SP moves on every call and return, so it says nothing about a function's
// inputs. It is tracked as a hazard instead (see "sp_manipulation").
constexpr uint16_t kReported = LIFT_REG_ALL & ~LIFT_REG_SP;

uint16_t reg8_bits(uint8_t index) {
    switch (index & 7u) {
        case 0: return LIFT_REG_B;
        case 1: return LIFT_REG_C;
        case 2: return LIFT_REG_D;
        case 3: return LIFT_REG_E;
        case 4: return LIFT_REG_H;
        case 5: return LIFT_REG_L;
        case 6: return 0;  // (HL): a memory operand, addressed through H and L
        default: return LIFT_REG_A;
    }
}

uint16_t operand8_reads(uint8_t index) {
    return (index & 7u) == 6u ? kHL : reg8_bits(index);
}

uint16_t pair_bits(uint8_t index) {
    switch (index & 3u) {
        case 0: return LIFT_REG_B | LIFT_REG_C;
        case 1: return LIFT_REG_D | LIFT_REG_E;
        case 2: return kHL;
        default: return LIFT_REG_SP;
    }
}

uint16_t condition_flag(uint8_t condition) {
    return (condition & 2u) ? LIFT_FLAG_C : LIFT_FLAG_Z;
}

}  // namespace

LiftInstructionEffect lift_instruction_effect(const Instruction& instr) {
    LiftInstructionEffect e;

    if (instr.is_cb_prefixed) {
        const uint8_t op = instr.cb_opcode;
        const uint8_t reg = op & 7u;
        const uint8_t group = op >> 6;
        const uint8_t kind = (op >> 3) & 7u;
        e.reads |= operand8_reads(reg);
        if (group == 0) {
            e.writes |= reg8_bits(reg) | kFlags;
            if (kind == 2 || kind == 3) e.reads |= LIFT_FLAG_C;  // RL, RR
        } else if (group == 1) {
            e.writes |= LIFT_FLAG_Z | LIFT_FLAG_N | LIFT_FLAG_H;  // BIT
        } else {
            e.writes |= reg8_bits(reg);  // RES, SET
        }
        return e;
    }

    const uint8_t op = instr.opcode;

    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) return e;  // HALT
        const uint8_t dst = (op >> 3) & 7u;
        const uint8_t src = op & 7u;
        e.reads |= operand8_reads(src);
        if (dst == 6) e.reads |= kHL;
        e.writes |= reg8_bits(dst);
        return e;
    }

    if (op >= 0x80 && op <= 0xBF) {
        const uint8_t kind = (op >> 3) & 7u;
        const uint8_t src = op & 7u;
        // SUB A, SBC A,A, XOR A and CP A give a result that does not depend
        // on what A held, so they are not a use of A.
        const bool cancels = src == 7 && (kind == 2 || kind == 3 || kind == 5 || kind == 7);
        if (!cancels) e.reads |= LIFT_REG_A | operand8_reads(src);
        if (kind == 1 || kind == 3) e.reads |= LIFT_FLAG_C;
        if (kind != 7) e.writes |= LIFT_REG_A;
        e.writes |= kFlags;
        return e;
    }

    if (op < 0x40) {
        const uint8_t reg = (op >> 3) & 7u;
        const uint8_t pair = (op >> 4) & 3u;
        switch (op & 0xC7u) {
            case 0x04:  // INC r
            case 0x05:  // DEC r
                e.reads |= operand8_reads(reg);
                e.writes |= reg8_bits(reg) | LIFT_FLAG_Z | LIFT_FLAG_N | LIFT_FLAG_H;
                return e;
            case 0x06:  // LD r,n
                if (reg == 6) e.reads |= kHL;
                e.writes |= reg8_bits(reg);
                return e;
            default:
                break;
        }
        switch (op & 0xCFu) {
            case 0x01:  // LD rr,nn
                e.writes |= pair_bits(pair);
                return e;
            case 0x03:  // INC rr
            case 0x0B:  // DEC rr
                e.reads |= pair_bits(pair);
                e.writes |= pair_bits(pair);
                return e;
            case 0x09:  // ADD HL,rr
                e.reads |= kHL | pair_bits(pair);
                e.writes |= kHL | LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C;
                return e;
            default:
                break;
        }
        switch (op) {
            case 0x02: e.reads |= LIFT_REG_B | LIFT_REG_C | LIFT_REG_A; break;
            case 0x12: e.reads |= LIFT_REG_D | LIFT_REG_E | LIFT_REG_A; break;
            case 0x0A: e.reads |= LIFT_REG_B | LIFT_REG_C; e.writes |= LIFT_REG_A; break;
            case 0x1A: e.reads |= LIFT_REG_D | LIFT_REG_E; e.writes |= LIFT_REG_A; break;
            case 0x22: case 0x32: e.reads |= kHL | LIFT_REG_A; e.writes |= kHL; break;
            case 0x2A: case 0x3A: e.reads |= kHL; e.writes |= kHL | LIFT_REG_A; break;
            case 0x07: case 0x0F:  // RLCA, RRCA
                e.reads |= LIFT_REG_A;
                e.writes |= LIFT_REG_A | kFlags;
                break;
            case 0x17: case 0x1F:  // RLA, RRA
                e.reads |= LIFT_REG_A | LIFT_FLAG_C;
                e.writes |= LIFT_REG_A | kFlags;
                break;
            case 0x08: e.reads |= LIFT_REG_SP; break;  // LD (nn),SP
            case 0x20: case 0x28: e.reads |= LIFT_FLAG_Z; break;
            case 0x30: case 0x38: e.reads |= LIFT_FLAG_C; break;
            case 0x27:  // DAA
                e.reads |= LIFT_REG_A | LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C;
                e.writes |= LIFT_REG_A | LIFT_FLAG_Z | LIFT_FLAG_H | LIFT_FLAG_C;
                break;
            case 0x2F:  // CPL
                e.reads |= LIFT_REG_A;
                e.writes |= LIFT_REG_A | LIFT_FLAG_N | LIFT_FLAG_H;
                break;
            case 0x37: e.writes |= LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C; break;
            case 0x3F:
                e.reads |= LIFT_FLAG_C;
                e.writes |= LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C;
                break;
            default:
                break;  // NOP, STOP, JR
        }
        return e;
    }

    // 0xC0 - 0xFF
    const uint8_t condition = (op >> 3) & 3u;
    const uint8_t pair = (op >> 4) & 3u;
    if ((op & 0xE7u) == 0xC0u) {  // RET cc
        e.reads |= condition_flag(condition) | LIFT_REG_SP;
        e.writes |= LIFT_REG_SP;
        return e;
    }
    if ((op & 0xE7u) == 0xC2u) {  // JP cc,nn
        e.reads |= condition_flag(condition);
        return e;
    }
    if ((op & 0xE7u) == 0xC4u) {  // CALL cc,nn
        e.reads |= condition_flag(condition) | LIFT_REG_SP;
        e.writes |= LIFT_REG_SP;
        return e;
    }
    if ((op & 0xCFu) == 0xC1u) {  // POP rr
        e.reads |= LIFT_REG_SP;
        e.writes |= LIFT_REG_SP | (pair == 3 ? (LIFT_REG_A | kFlags) : pair_bits(pair));
        return e;
    }
    if ((op & 0xCFu) == 0xC5u) {  // PUSH rr
        e.reads |= LIFT_REG_SP | (pair == 3 ? (LIFT_REG_A | kFlags) : pair_bits(pair));
        e.writes |= LIFT_REG_SP;
        return e;
    }
    if ((op & 0xC7u) == 0xC6u) {  // ALU A,n
        const uint8_t kind = (op >> 3) & 7u;
        e.reads |= LIFT_REG_A;
        if (kind == 1 || kind == 3) e.reads |= LIFT_FLAG_C;
        if (kind != 7) e.writes |= LIFT_REG_A;
        e.writes |= kFlags;
        return e;
    }
    if ((op & 0xC7u) == 0xC7u) {  // RST
        e.reads |= LIFT_REG_SP;
        e.writes |= LIFT_REG_SP;
        return e;
    }
    switch (op) {
        case 0xC9: case 0xD9: case 0xCD:
            e.reads |= LIFT_REG_SP;
            e.writes |= LIFT_REG_SP;
            break;
        case 0xE0: case 0xEA: e.reads |= LIFT_REG_A; break;
        case 0xF0: case 0xFA: e.writes |= LIFT_REG_A; break;
        case 0xE2: e.reads |= LIFT_REG_C | LIFT_REG_A; break;
        case 0xF2: e.reads |= LIFT_REG_C; e.writes |= LIFT_REG_A; break;
        case 0xE8: e.reads |= LIFT_REG_SP; e.writes |= LIFT_REG_SP | kFlags; break;
        case 0xF8: e.reads |= LIFT_REG_SP; e.writes |= kHL | kFlags; break;
        case 0xF9: e.reads |= kHL; e.writes |= LIFT_REG_SP; break;
        case 0xE9: e.reads |= kHL; break;
        default:
            break;  // JP nn, DI, EI, undefined opcodes
    }
    return e;
}

namespace {

/* ---------------------------------------------------------------------------
 * Small JSON helpers
 * ------------------------------------------------------------------------- */

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20 || ch >= 0x7F) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    out += '"';
    return out;
}

std::string function_id(BankId bank, uint16_t address) {
    return native_function_id(bank, address);
}

std::string id_list(const std::set<uint32_t>& ids) {
    std::string out = "[";
    bool first = true;
    for (const uint32_t id : ids) {
        if (!first) out += ", ";
        first = false;
        out += json_string(function_id(static_cast<BankId>(id >> 16),
                                       static_cast<uint16_t>(id & 0xFFFFu)));
    }
    out += "]";
    return out;
}

std::string register_list(uint16_t bits) {
    static const std::array<std::pair<uint16_t, const char*>, 7> kNames = {{
        {LIFT_REG_A, "a"}, {LIFT_REG_B, "b"}, {LIFT_REG_C, "c"}, {LIFT_REG_D, "d"},
        {LIFT_REG_E, "e"}, {LIFT_REG_H, "h"}, {LIFT_REG_L, "l"},
    }};
    std::string out = "[";
    bool first = true;
    for (const auto& [bit, name] : kNames) {
        if (!(bits & bit)) continue;
        if (!first) out += ", ";
        first = false;
        out += std::string("\"") + name + "\"";
    }
    out += "]";
    return out;
}

std::string flag_list(uint16_t bits) {
    static const std::array<std::pair<uint16_t, const char*>, 4> kNames = {{
        {LIFT_FLAG_Z, "z"}, {LIFT_FLAG_N, "n"}, {LIFT_FLAG_H, "h"}, {LIFT_FLAG_C, "c"},
    }};
    std::string out = "[";
    bool first = true;
    for (const auto& [bit, name] : kNames) {
        if (!(bits & bit)) continue;
        if (!first) out += ", ";
        first = false;
        out += std::string("\"") + name + "\"";
    }
    out += "]";
    return out;
}

const char* memory_space(uint16_t address) {
    if (address < 0x4000) return "rom0";
    if (address < 0x8000) return "romx";
    if (address < 0xA000) return "vram";
    if (address < 0xC000) return "eram";
    if (address < 0xE000) return "wram";
    if (address < 0xFE00) return "echo";
    if (address < 0xFEA0) return "oam";
    if (address < 0xFF00) return "unusable";
    if (address < 0xFF80) return "io";
    if (address < 0xFFFF) return "hram";
    return "ie";
}

// Hardware register names as hardware.inc spells them.
const char* io_register_name(uint16_t address) {
    switch (address) {
        case 0xFF00: return "rP1";
        case 0xFF01: return "rSB";
        case 0xFF02: return "rSC";
        case 0xFF04: return "rDIV";
        case 0xFF05: return "rTIMA";
        case 0xFF06: return "rTMA";
        case 0xFF07: return "rTAC";
        case 0xFF0F: return "rIF";
        case 0xFF10: return "rNR10";
        case 0xFF11: return "rNR11";
        case 0xFF12: return "rNR12";
        case 0xFF13: return "rNR13";
        case 0xFF14: return "rNR14";
        case 0xFF16: return "rNR21";
        case 0xFF17: return "rNR22";
        case 0xFF18: return "rNR23";
        case 0xFF19: return "rNR24";
        case 0xFF1A: return "rNR30";
        case 0xFF1B: return "rNR31";
        case 0xFF1C: return "rNR32";
        case 0xFF1D: return "rNR33";
        case 0xFF1E: return "rNR34";
        case 0xFF20: return "rNR41";
        case 0xFF21: return "rNR42";
        case 0xFF22: return "rNR43";
        case 0xFF23: return "rNR44";
        case 0xFF24: return "rNR50";
        case 0xFF25: return "rNR51";
        case 0xFF26: return "rNR52";
        case 0xFF40: return "rLCDC";
        case 0xFF41: return "rSTAT";
        case 0xFF42: return "rSCY";
        case 0xFF43: return "rSCX";
        case 0xFF44: return "rLY";
        case 0xFF45: return "rLYC";
        case 0xFF46: return "rDMA";
        case 0xFF47: return "rBGP";
        case 0xFF48: return "rOBP0";
        case 0xFF49: return "rOBP1";
        case 0xFF4A: return "rWY";
        case 0xFF4B: return "rWX";
        case 0xFF4D: return "rKEY1";
        case 0xFF4F: return "rVBK";
        case 0xFF51: return "rHDMA1";
        case 0xFF52: return "rHDMA2";
        case 0xFF53: return "rHDMA3";
        case 0xFF54: return "rHDMA4";
        case 0xFF55: return "rHDMA5";
        case 0xFF56: return "rRP";
        case 0xFF68: return "rBCPS";
        case 0xFF69: return "rBCPD";
        case 0xFF6A: return "rOCPS";
        case 0xFF6B: return "rOCPD";
        case 0xFF70: return "rSVBK";
        case 0xFFFF: return "rIE";
        default: return nullptr;
    }
}

// Registers whose value the hardware changes on its own. Code that reads one
// depends on when it runs, so it cannot be lifted without keeping its timing.
bool is_volatile_io(uint16_t address) {
    switch (address) {
        case 0xFF00: case 0xFF01: case 0xFF02: case 0xFF04: case 0xFF05:
        case 0xFF0F: case 0xFF26: case 0xFF41: case 0xFF44: case 0xFF4D:
        case 0xFF55: case 0xFF56:
            return true;
        default:
            return false;
    }
}

/* ---------------------------------------------------------------------------
 * Disassembly in RGBDS syntax, with jump targets as addresses
 * ------------------------------------------------------------------------- */

std::string hex_word(unsigned value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "$%04X", value & 0xFFFFu);
    return buffer;
}

std::string hex_byte(unsigned value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "$%02X", value & 0xFFu);
    return buffer;
}

std::string lift_disassemble(const Instruction& instr, uint8_t imm8, uint16_t imm16) {
    static const char* const kReg8[8] = {"b", "c", "d", "e", "h", "l", "[hl]", "a"};
    static const char* const kReg16[4] = {"bc", "de", "hl", "sp"};
    static const char* const kStack16[4] = {"bc", "de", "hl", "af"};
    static const char* const kCondition[4] = {"nz", "z", "nc", "c"};
    static const char* const kAlu[8] = {"add a, ", "adc a, ", "sub ", "sbc a, ",
                                        "and ", "xor ", "or ", "cp "};
    static const char* const kShift[8] = {"rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl"};

    if (instr.is_cb_prefixed) {
        const uint8_t op = instr.cb_opcode;
        const char* reg = kReg8[op & 7u];
        const std::string bit = std::to_string((op >> 3) & 7u);
        switch (op >> 6) {
            case 0: return std::string(kShift[(op >> 3) & 7u]) + " " + reg;
            case 1: return "bit " + bit + ", " + reg;
            case 2: return "res " + bit + ", " + reg;
            default: return "set " + bit + ", " + reg;
        }
    }

    const uint8_t op = instr.opcode;
    const std::string n8 = hex_byte(imm8);
    const std::string n16 = hex_word(imm16);
    const std::string high = "[" + hex_word(0xFF00u + imm8) + "]";
    const std::string jr_target =
        hex_word(static_cast<unsigned>(instr.address + 2 + static_cast<int8_t>(imm8)));

    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) return "halt";
        return std::string("ld ") + kReg8[(op >> 3) & 7u] + ", " + kReg8[op & 7u];
    }
    if (op >= 0x80 && op <= 0xBF) {
        return std::string(kAlu[(op >> 3) & 7u]) + kReg8[op & 7u];
    }
    if (op < 0x40) {
        const char* reg = kReg8[(op >> 3) & 7u];
        const char* pair = kReg16[(op >> 4) & 3u];
        switch (op & 0x0Fu) {
            case 0x1: return std::string("ld ") + pair + ", " + n16;
            case 0x3: return std::string("inc ") + pair;
            case 0xB: return std::string("dec ") + pair;
            case 0x9: return std::string("add hl, ") + pair;
            default: break;
        }
        switch (op & 7u) {
            case 4: return std::string("inc ") + reg;
            case 5: return std::string("dec ") + reg;
            case 6: return std::string("ld ") + reg + ", " + n8;
            default: break;
        }
        switch (op) {
            case 0x00: return "nop";
            case 0x02: return "ld [bc], a";
            case 0x12: return "ld [de], a";
            case 0x0A: return "ld a, [bc]";
            case 0x1A: return "ld a, [de]";
            case 0x22: return "ld [hli], a";
            case 0x32: return "ld [hld], a";
            case 0x2A: return "ld a, [hli]";
            case 0x3A: return "ld a, [hld]";
            case 0x07: return "rlca";
            case 0x0F: return "rrca";
            case 0x17: return "rla";
            case 0x1F: return "rra";
            case 0x08: return "ld [" + n16 + "], sp";
            case 0x10: return "stop";
            case 0x18: return "jr " + jr_target;
            case 0x20: case 0x28: case 0x30: case 0x38:
                return std::string("jr ") + kCondition[(op >> 3) & 3u] + ", " + jr_target;
            case 0x27: return "daa";
            case 0x2F: return "cpl";
            case 0x37: return "scf";
            case 0x3F: return "ccf";
            default: return "db " + hex_byte(op);
        }
    }

    const char* condition = kCondition[(op >> 3) & 3u];
    const char* stack = kStack16[(op >> 4) & 3u];
    if ((op & 0xE7u) == 0xC0u) return std::string("ret ") + condition;
    if ((op & 0xE7u) == 0xC2u) return std::string("jp ") + condition + ", " + n16;
    if ((op & 0xE7u) == 0xC4u) return std::string("call ") + condition + ", " + n16;
    if ((op & 0xCFu) == 0xC1u) return std::string("pop ") + stack;
    if ((op & 0xCFu) == 0xC5u) return std::string("push ") + stack;
    if ((op & 0xC7u) == 0xC6u) return std::string(kAlu[(op >> 3) & 7u]) + n8;
    if ((op & 0xC7u) == 0xC7u) return "rst " + hex_byte(op & 0x38u);
    switch (op) {
        case 0xC3: return "jp " + n16;
        case 0xC9: return "ret";
        case 0xD9: return "reti";
        case 0xCD: return "call " + n16;
        case 0xE0: return "ldh " + high + ", a";
        case 0xF0: return "ldh a, " + high;
        case 0xE2: return "ldh [c], a";
        case 0xF2: return "ldh a, [c]";
        case 0xEA: return "ld [" + n16 + "], a";
        case 0xFA: return "ld a, [" + n16 + "]";
        case 0xE8: return "add sp, " + std::to_string(static_cast<int>(static_cast<int8_t>(imm8)));
        case 0xF8: return "ld hl, sp" + std::string(static_cast<int8_t>(imm8) < 0 ? "" : "+") +
                          std::to_string(static_cast<int>(static_cast<int8_t>(imm8)));
        case 0xF9: return "ld sp, hl";
        case 0xE9: return "jp hl";
        case 0xF3: return "di";
        case 0xFB: return "ei";
        default: return "db " + hex_byte(op);
    }
}

/* ---------------------------------------------------------------------------
 * Function model
 * ------------------------------------------------------------------------- */

struct Target {
    bool known = false;       // the bank of the target is known
    bool is_ram = false;      // the target is outside the ROM
    BankId bank = 0;
    uint16_t address = 0;
    uint32_t full() const { return AnalysisResult::make_addr(bank, address); }
};

struct LocalBlock {
    uint16_t start = 0;
    uint16_t end = 0;                     // the address after its last instruction
    std::vector<const Instruction*> instructions;
    std::vector<size_t> successors;       // blocks of the same function
    // Those of them a JP HL comes back to: the function pushed their address
    // and what it jumps to returns there, so other code runs on the way.
    std::vector<size_t> back_successors;
    bool back_certain = false;            // the push is in the block, of a constant
    std::vector<uint32_t> tail_functions; // functions control leaves into
    bool returns = false;                 // ends in RET or RETI
    bool unknown_exit = false;            // leaves to code that is no function
};

struct MemoryAccess {
    bool read = false;
    bool write = false;
    bool pointer = false;  // the address is loaded into a register pair
    std::set<uint16_t> sites;
};

struct CallSite {
    uint16_t site = 0;
    Target target;
    bool conditional = false;
    bool is_rst = false;
    bool target_is_function = false;
    bool is_fallthrough = false;  // jumps_out only: control runs into the target
    bool is_computed = false;     // jumps_out only: a resolved JP HL
    bool reaches_known = false;   // the target is a helper, and where it goes from here is known
    uint32_t reaches = 0;         // that place, bank << 16 | address
    // Or the helper picks from a table: its entries, in order.
    bool reaches_table = false;
    uint32_t table = 0;
    std::vector<uint32_t> table_entries;
    // Or a run of the game went on from here to these.
    std::vector<uint32_t> seen;
};

struct BankWrite {
    uint16_t site = 0;
    uint16_t address = 0;
    int value = -1;
};

struct RomReference {
    uint16_t site = 0;
    uint16_t address = 0;
    bool bank_known = false;
    BankId bank = 0;
    bool is_code = false;
};

struct Summary {
    uint16_t live_in = 0;                // may be read before it is written
    uint16_t need_in = 0;                // the same, counting code that cannot be followed
                                         // as reading everything
    uint16_t may_def = 0;                // may be written
    uint16_t must_def = LIFT_REG_ALL;    // written on every path out
    bool incomplete = false;             // an unknown callee or exit is involved
};

/* What one block does when it ends, before blocks are grouped into functions. */
/* A function that is where another function goes on: the tail two of them
 * share, or a tail of one that lies apart from it. It is entered by a jump,
 * with whatever the function jumping in has pushed. */
bool is_tail_kind(const char* kind) {
    const std::string name = kind;
    return name == "shared_tail" || name == "detached_tail";
}

struct BlockExit {
    Target target;
    uint16_t site = 0;
    bool conditional = false;
    bool is_jump = false;      // a JP or JR, as opposed to running into the next block
    bool is_computed = false;  // one resolved target of a JP HL
    bool comes_back = false;   // where what a JP HL jumps to returns: a pushed address
    bool to_block = false;     // the target is a decoded block
};

struct BlockInfo {
    uint16_t start = 0;
    uint16_t end = 0;                   // the address after its last instruction
    std::vector<uint32_t> computed;     // where the analysis has its JP HL go
    std::vector<const Instruction*> instructions;
    std::vector<BlockExit> exits;
    bool returns = false;
    bool unknown_exit = false;  // a computed jump with no known target, or bad code
    bool back_certain = false;  // its JP HL has the return address the block itself pushed
};

struct FunctionModel {
    uint32_t full_address = 0;
    BankId bank = 0;
    uint16_t entry_address = 0;
    std::string name;
    const char* kind = "call";
    bool recompiler_function = false;  // the recompiler emits a function here too
    std::vector<uint32_t> block_addresses;  // full addresses; the entry comes first
    std::vector<LocalBlock> blocks;  // blocks[0] is the entry
    std::vector<CallSite> calls;
    std::vector<CallSite> jumps_out;
    std::vector<uint16_t> computed_jumps;
    std::vector<std::pair<uint16_t, const char*>> returns;
    std::map<uint16_t, MemoryAccess> memory;
    std::vector<BankWrite> bank_writes;
    std::vector<RomReference> rom_references;
    std::set<std::string> hazards;
    std::set<std::string> pushes;
    std::set<std::string> pops;
    std::set<uint32_t> callers;
    size_t instruction_count = 0;
    size_t byte_count = 0;
    Summary summary;

    // What the function owes whoever called it.
    int entry_depth = 0;                 // words pushed since the call when control arrives
    bool balanced = true;                // every return pops the address the call pushed
    std::set<std::string> entered_by;    // ways in that have no call site; empty: all are known
    std::map<uint16_t, std::array<int, 8>> site_known;  // constants at each call and jump
    std::map<uint16_t, std::vector<uint32_t>> seen_jumps;  // where a run saw each computed jump go
    bool ran = false;                    // a run of the game entered it
    const LiftTableEntry* in_table = nullptr;  // the first declared table that names it
    uint16_t read_after = 0;             // what a caller may read after it returns
};

/* Stack depths, in words above the return address. */
constexpr int kDepthUnset = INT_MIN;
constexpr int kDepthUnknown = INT_MAX;

Target resolve_target(const ROM& rom, const Instruction& instr, uint16_t address) {
    Target target;
    target.address = address;
    if (address >= 0x8000) {
        target.is_ram = true;
        return target;
    }
    if (address < 0x4000) {
        target.known = true;
        target.bank = 0;
        return target;
    }
    if (rom.bank_count() <= 2) {
        target.known = true;
        target.bank = 1;
    } else if (instr.address >= 0x4000 && instr.address < 0x8000 && instr.bank != 0) {
        target.known = true;
        target.bank = instr.bank;
    } else if (instr.resolved_target_bank != UNKNOWN_BANK) {
        target.known = true;
        target.bank = instr.resolved_target_bank;
    }
    return target;
}

uint16_t direct_target_address(const Instruction& instr) {
    switch (instr.type) {
        case InstructionType::JR_N:
        case InstructionType::JR_CC_N:
            return static_cast<uint16_t>(instr.address + 2 + instr.offset);
        case InstructionType::RST:
            return instr.rst_vector;
        default:
            return instr.imm16;
    }
}

bool is_direct_jump(const Instruction& instr) {
    return instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN ||
           instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N;
}

bool is_call(const Instruction& instr) {
    return instr.type == InstructionType::CALL_NN || instr.type == InstructionType::CALL_CC_NN ||
           instr.type == InstructionType::RST;
}

bool is_return(const Instruction& instr) {
    return instr.type == InstructionType::RET || instr.type == InstructionType::RETI ||
           instr.type == InstructionType::RET_CC;
}

bool is_conditional(const Instruction& instr) {
    return instr.type == InstructionType::JP_CC_NN || instr.type == InstructionType::JR_CC_N ||
           instr.type == InstructionType::CALL_CC_NN || instr.type == InstructionType::RET_CC;
}

const char* pair_name(uint8_t index, bool af) {
    switch (index & 3u) {
        case 0: return "bc";
        case 1: return "de";
        case 2: return "hl";
        default: return af ? "af" : "sp";
    }
}

/* Block-local constant tracking, enough to name the fixed addresses that
 * `ld hl,$C200 / ld a,[hl]` style code touches. */
struct KnownRegisters {
    std::array<int, 8> value;  // indexed like the opcode's register field; 6 unused
    KnownRegisters() { value.fill(-1); }
    int pair(uint8_t high, uint8_t low) const {
        if (value[high] < 0 || value[low] < 0) return -1;
        return (value[high] << 8) | value[low];
    }
    void set_pair(uint8_t high, uint8_t low, int v) {
        if (v < 0) {
            value[high] = value[low] = -1;
        } else {
            value[high] = (v >> 8) & 0xFF;
            value[low] = v & 0xFF;
        }
    }
    void forget(uint16_t bits) {
        static const std::array<uint16_t, 8> kBits = {
            LIFT_REG_B, LIFT_REG_C, LIFT_REG_D, LIFT_REG_E, LIFT_REG_H, LIFT_REG_L, 0, LIFT_REG_A};
        for (size_t i = 0; i < kBits.size(); ++i) {
            if (bits & kBits[i]) value[i] = -1;
        }
    }
};

class Exporter {
public:
    Exporter(const ROM& rom, const AnalysisResult& analysis, const LiftExportOptions& options)
        : rom_(rom), analysis_(analysis), options_(options) {}

    std::string build() {
        build_coverage();
        build_block_infos();
        partition_functions();
        build_models();
        solve_stack();
        solve_summaries();
        link_callers();
        find_unlisted_entries();
        solve_read_after();
        return emit();
    }

private:
    const ROM& rom_;
    const AnalysisResult& analysis_;
    const LiftExportOptions& options_;
    std::vector<FunctionModel> models_;
    std::map<uint32_t, size_t> model_index_;
    std::vector<uint8_t> covered_;           // one flag per ROM byte
    std::map<uint32_t, BlockInfo> block_infos_;
    std::map<uint32_t, const char*> entries_;  // function entry -> why it is one
    std::map<uint32_t, std::set<uint16_t>> jump_returns_;  // JP HL -> where a run or a probe saw it come back
    std::set<uint32_t> back_points_;           // blocks a JP HL comes back to
    std::map<uint32_t, uint32_t> owner_;       // block -> the function it belongs to

    // As ROM::read_banked() has it: a bank above 0 at an address below
    // 0x4000 is that bank mapped into the lower window (MBC1 mode 1).
    size_t rom_offset(BankId bank, uint16_t address) const {
        return static_cast<size_t>(bank) * 0x4000u + (address & 0x3FFFu);
    }

    // Operand bytes straight from the ROM; the decoder only fills the fields
    // its own users need.
    uint8_t operand8(const Instruction& instr) const {
        if (instr.address >= 0x8000) return instr.imm8;
        return rom_.read_banked(instr.bank, static_cast<uint16_t>(instr.address + 1));
    }

    uint16_t operand16(const Instruction& instr) const {
        if (instr.address >= 0x8000) return instr.imm16;
        return static_cast<uint16_t>(
            rom_.read_banked(instr.bank, static_cast<uint16_t>(instr.address + 1)) |
            (rom_.read_banked(instr.bank, static_cast<uint16_t>(instr.address + 2)) << 8));
    }

    bool is_function(const Target& target) const {
        return target.known && model_index_.count(target.full()) != 0;
    }

    void build_coverage() {
        covered_.assign(rom_.size(), 0);
        for (const Instruction& instr : analysis_.instructions) {
            if (instr.address >= 0x8000) continue;  // code copied to RAM
            const size_t offset = rom_offset(instr.bank, instr.address);
            for (size_t i = 0; i < instr.length && offset + i < covered_.size(); ++i) {
                covered_[offset + i] = 1;
            }
        }
    }

    /* The block that code in `bank` at `site` finds at `address`: the same
     * window of the ROM is the same bank, the home bank is always there, and
     * from the home bank the switchable area is whichever bank is mapped. */
    bool place_seen_from(BankId bank, uint16_t site, uint16_t address, uint32_t& full) const {
        if (address >= 0x8000) return false;
        BankId target_bank = 0;
        if (address < 0x4000) {
            target_bank = site < 0x4000 ? bank : 0;
        } else if (site >= 0x4000) {
            target_bank = bank;
        } else if (rom_.bank_count() <= 2) {
            target_bank = 1;
        } else {
            return false;
        }
        full = AnalysisResult::make_addr(target_bank, address);
        return true;
    }

    /* What a block has pushed and not popped where it ends: for each word
     * the constant its register pair held, or -1. Nothing once a call or a
     * write of SP has come after the pushes, since what is on top then is no
     * longer this block's to say. */
    std::vector<int> pushed_at_end(const BlockInfo& info) const {
        enum { B = 0, C = 1, D = 2, E = 3, H = 4, L = 5 };
        KnownRegisters known;
        std::vector<int> pushed;
        for (const Instruction* pointer : info.instructions) {
            const Instruction& instr = *pointer;
            if (instr.type == InstructionType::PUSH) {
                switch ((instr.opcode >> 4) & 3u) {
                    case 0: pushed.push_back(known.pair(B, C)); break;
                    case 1: pushed.push_back(known.pair(D, E)); break;
                    case 2: pushed.push_back(known.pair(H, L)); break;
                    default: pushed.push_back(-1); break;
                }
            } else if (instr.type == InstructionType::POP) {
                if (!pushed.empty()) pushed.pop_back();
            } else if (is_call(instr) || writes_sp(instr)) {
                pushed.clear();
            }
            follow_constants(known, instr);
        }
        return pushed;
    }

    /* `ld de, back / push de / jp hl` is a call: what HL points at returns to
     * `back`, and the function goes on from there. For that the block the
     * jump ends has to have pushed the word on top itself, so that it is
     * this function's and not its caller's. The address is the constant the
     * pair held, or, where it held none, what a run of the game or a probe
     * of the helper found there (`returns_to` in the edges). Only the first
     * is certain: the others are places the jump has been seen to come back
     * to, which the lifted source compares with what is on the stack. */
    std::set<uint32_t> jump_return_places(BankId bank, const Instruction& jump, const BlockInfo& info,
                                          bool& certain) const {
        std::set<uint32_t> places;
        certain = false;
        const std::vector<int> pushed = pushed_at_end(info);
        if (pushed.empty()) return places;
        uint32_t full = 0;
        if (pushed.back() >= 0) {
            if (place_seen_from(bank, jump.address, static_cast<uint16_t>(pushed.back()), full)) {
                places.insert(full);
                certain = true;
            }
            return places;
        }
        const auto seen = jump_returns_.find(AnalysisResult::make_addr(bank, jump.address));
        if (seen == jump_returns_.end()) return places;
        for (const uint16_t address : seen->second) {
            if (place_seen_from(bank, jump.address, address, full)) places.insert(full);
        }
        return places;
    }

    void find_jump_returns(BankId bank, const Instruction& jump, BlockInfo& info) {
        bool certain = false;
        for (const uint32_t address : jump_return_places(bank, jump, info, certain)) {
            if (!block_infos_.count(address)) continue;  // not decoded
            BlockExit exit;
            exit.target.known = true;
            exit.target.bank = static_cast<BankId>(address >> 16);
            exit.target.address = static_cast<uint16_t>(address & 0xFFFFu);
            exit.site = jump.address;
            exit.conditional = true;
            exit.comes_back = true;
            exit.to_block = true;
            info.exits.push_back(exit);
            info.back_certain = certain;
            back_points_.insert(address);
        }
    }

    /* The instruction an edge says control left at. It is in the helper the
     * edge was found through, or is the site. */
    bool left_at_place(const LiftFarEdge& edge, uint32_t& full) const {
        if (edge.left_at == 0) return false;
        const uint32_t owner = edge.is_computed ? edge.from : edge.helper;
        if (owner == 0) return false;
        const BankId owner_bank = static_cast<BankId>(owner >> 16);
        const uint16_t owner_address = static_cast<uint16_t>(owner & 0xFFFFu);
        BankId bank = 0;
        if (edge.left_at >= 0x4000) {
            // A helper in the home bank that left from the switchable
            // area did so in a bank the edge does not name.
            if (owner_address >= 0x4000) {
                bank = owner_bank;
            } else if (rom_.bank_count() <= 2) {
                bank = 1;
            } else {
                return false;
            }
        }
        full = AnalysisResult::make_addr(bank, edge.left_at);
        return true;
    }

    /* Where each computed jump was found to come back, from the edges. */
    void collect_jump_returns() {
        for (const LiftFarEdge& edge : options_.far_edges) {
            uint32_t left = 0;
            if (edge.returns_to == 0 || !left_at_place(edge, left)) continue;
            jump_returns_[left].insert(edge.returns_to);
        }
    }

    /* A run that saw a RET go where a JP HL comes back saw that jump's
     * callee return, and not a jump to a function that starts there. A
     * recorder writes such a RET down when it did not see the push: a
     * recording that starts from a save state made inside the function has
     * nothing on its shadow of the call stack, so the JP HL is to it a jump
     * with nothing to come back to, and the callee's RET one to an address no
     * CALL pushed. Another JP HL seen to go there is a jump like any other. */
    bool came_back(const LiftFarEdge& edge) const {
        if (!edge.seen || !back_points_.count(edge.target)) return false;
        uint32_t left = 0;
        if (!left_at_place(edge, left)) return false;
        const auto found = analysis_.addr_to_index.find(left);
        return found != analysis_.addr_to_index.end() &&
               is_return(analysis_.instructions[found->second]);
    }

    /* Nothing in the code jumps to where a JP HL comes back, so the address
     * may be in the middle of a block. The block is cut in two there: the
     * function can only go on from the start of one. */
    void split_at_jump_returns() {
        std::vector<uint32_t> wanted;
        for (const auto& [address, info] : block_infos_) {
            const Instruction& last = *info.instructions.back();
            if (last.type != InstructionType::JP_HL) continue;
            bool certain = false;
            for (const uint32_t place :
                 jump_return_places(static_cast<BankId>(address >> 16), last, info, certain)) {
                wanted.push_back(place);
            }
        }
        for (const uint32_t place : wanted) {
            auto after = block_infos_.upper_bound(place);
            if (after == block_infos_.begin()) continue;
            auto found = std::prev(after);
            if (found->first == place || (found->first >> 16) != (place >> 16)) continue;
            BlockInfo& head = found->second;
            const uint16_t address = static_cast<uint16_t>(place & 0xFFFFu);
            size_t cut = 0;
            for (size_t k = 1; k < head.instructions.size(); ++k) {
                if (head.instructions[k]->address == address) cut = k;
            }
            if (cut == 0) continue;  // not in this block, or not where an instruction starts
            BlockInfo tail;
            tail.start = address;
            tail.end = head.end;
            tail.computed = std::move(head.computed);
            tail.instructions.assign(head.instructions.begin() + static_cast<std::ptrdiff_t>(cut),
                                     head.instructions.end());
            head.computed.clear();
            head.instructions.resize(cut);
            head.end = address;
            block_infos_[place] = std::move(tail);
        }
    }

    void build_block_infos() {
        collect_jump_returns();
        for (const auto& [full_address, block] : analysis_.blocks) {
            BlockInfo info;
            info.start = block.start_address;
            info.end = block.end_address;
            info.computed = block.successors;
            for (const size_t index : block.instruction_indices) {
                if (index < analysis_.instructions.size()) {
                    info.instructions.push_back(&analysis_.instructions[index]);
                }
            }
            if (!info.instructions.empty()) block_infos_[full_address] = std::move(info);
        }
        split_at_jump_returns();

        for (auto& [full_address, info] : block_infos_) {
            const BankId bank = static_cast<BankId>(full_address >> 16);
            const Instruction& last = *info.instructions.back();
            const uint16_t next = static_cast<uint16_t>(last.address + last.length);

            auto leave = [&](Target target, bool conditional, bool is_jump) {
                BlockExit exit;
                exit.target = target;
                exit.site = last.address;
                exit.conditional = conditional;
                exit.is_jump = is_jump;
                exit.to_block = target.known && !target.is_ram &&
                                block_infos_.count(target.full()) != 0;
                info.exits.push_back(exit);
            };
            auto fall = [&]() {
                Target target;
                target.address = next;
                target.bank = bank;
                // Code does not run off the end of the ROM window into RAM.
                target.known = !(next >= 0x8000 && last.address < 0x8000);
                leave(target, false, false);
            };

            switch (last.type) {
                case InstructionType::RET:
                case InstructionType::RETI:
                    info.returns = true;
                    break;
                case InstructionType::RET_CC:
                    info.returns = true;
                    fall();
                    break;
                case InstructionType::JP_HL: {
                    bool any = false;
                    for (const uint32_t successor : info.computed) {
                        if (!block_infos_.count(successor)) continue;
                        Target target;
                        target.known = true;
                        target.bank = static_cast<BankId>(successor >> 16);
                        target.address = static_cast<uint16_t>(successor & 0xFFFFu);
                        leave(target, true, false);
                        info.exits.back().is_computed = true;
                        any = true;
                    }
                    if (!any) info.unknown_exit = true;
                    find_jump_returns(bank, last, info);
                    break;
                }
                case InstructionType::UNDEFINED:
                case InstructionType::INVALID:
                    info.unknown_exit = true;
                    break;
                default:
                    if (is_direct_jump(last)) {
                        leave(resolve_target(rom_, last, direct_target_address(last)),
                              is_conditional(last), true);
                        if (is_conditional(last)) fall();
                    } else {
                        fall();
                    }
                    break;
            }
        }
    }

    /* Group blocks into subroutines. The recompiler's own functions are
     * units of code generation (a call's return point starts a new one), so
     * they are not used here. A function starts where something calls, and
     * holds the blocks that jumps and fall-through reach from there without
     * entering another function. A block that two functions would share
     * becomes a function of its own, so every block has exactly one owner. */
    void partition_functions() {
        auto seed = [&](uint32_t full_address, const char* kind) {
            if (!block_infos_.count(full_address)) return;
            entries_.emplace(full_address, kind);
        };

        seed(AnalysisResult::make_addr(0, analysis_.entry_point), "entry");
        for (const uint16_t vector : analysis_.interrupt_vectors) {
            seed(AnalysisResult::make_addr(0, vector), "interrupt");
        }
        for (const Instruction& instr : analysis_.instructions) {
            if (!is_call(instr)) continue;
            const Target target = resolve_target(rom_, instr, direct_target_address(instr));
            if (target.known && !target.is_ram) seed(target.full(), "call");
        }
        // Reached through a helper that takes its target in registers
        // (`--lift-edges`): as much a call as a CALL is.
        for (const LiftFarEdge& edge : options_.far_edges) {
            if (!edge.from_table && !edge.seen) seed(edge.target, "far_call");
        }
        // Entries of a table of functions a helper calls through.
        for (const LiftFarEdge& edge : options_.far_edges) {
            if (edge.from_table) seed(edge.target, "table_call");
        }
        // Entries of a table of functions somebody declared: who calls
        // through it is not known, where its entries are is.
        for (const LiftTableEntry& entry : options_.table_entries) seed(entry.target, "declared_table");
        // Where a run of the game was seen to go by a jump the code does not
        // spell out. Not where what it saw was a function's own JP HL coming
        // back: that address is where the function goes on.
        for (const LiftFarEdge& edge : options_.far_edges) {
            if (edge.seen && !came_back(edge)) seed(edge.target, "seen");
        }
        // And where one arrived by a call nothing above accounts for: a CALL
        // whose bank the code around it does not show. The list does not say
        // how a place was arrived at, and where a JP HL comes back is arrived
        // at every time its callee returns.
        for (const uint32_t address : options_.entered) {
            if (!back_points_.count(address)) seed(address, "seen");
        }
        for (const uint32_t target : analysis_.computed_jump_targets) {
            // The analysis makes an entry of the address pushed before a
            // JP HL so that the game can arrive there. Here it is where the
            // function that pushed it goes on.
            if (back_points_.count(target) && analysis_.synthetic_entry_targets.count(target)) continue;
            seed(target, "jump_table");
        }
        for (const uint32_t target : options_.declared_functions) seed(target, "declared");

        std::map<uint32_t, std::vector<uint32_t>> predecessors;
        for (const auto& [full_address, info] : block_infos_) {
            for (const BlockExit& exit : info.exits) {
                if (exit.to_block) predecessors[exit.target.full()].push_back(full_address);
            }
        }

        for (;;) {
            // Flood from every entry, stopping at other entries.
            std::map<uint32_t, std::set<uint32_t>> owners;
            for (const auto& [entry, kind] : entries_) {
                std::vector<uint32_t> stack = {entry};
                while (!stack.empty()) {
                    const uint32_t current = stack.back();
                    stack.pop_back();
                    if (current != entry && entries_.count(current)) continue;
                    if (!owners[current].insert(entry).second) continue;
                    for (const BlockExit& exit : block_infos_.at(current).exits) {
                        if (exit.to_block) stack.push_back(exit.target.full());
                    }
                }
            }

            bool changed = false;
            // A block nothing reaches is code the scans found. Its roots
            // become functions; the next round floods from them.
            std::vector<uint32_t> unowned;
            for (const auto& [full_address, info] : block_infos_) {
                if (!owners.count(full_address)) unowned.push_back(full_address);
            }
            if (!unowned.empty()) {
                bool any_root = false;
                for (const uint32_t full_address : unowned) {
                    bool has_unowned_predecessor = false;
                    const auto it = predecessors.find(full_address);
                    if (it != predecessors.end()) {
                        for (const uint32_t predecessor : it->second) {
                            if (predecessor != full_address && !owners.count(predecessor)) {
                                has_unowned_predecessor = true;
                                break;
                            }
                        }
                    }
                    if (!has_unowned_predecessor) {
                        entries_.emplace(full_address, "unreferenced");
                        any_root = true;
                    }
                }
                if (!any_root) entries_.emplace(unowned.front(), "unreferenced");
                changed = true;
            }

            // The first block of each shared stretch becomes a function.
            std::vector<uint32_t> shared_heads;
            uint32_t first_shared = 0;
            bool any_shared = false;
            for (const auto& [full_address, set] : owners) {
                if (set.size() < 2) continue;
                if (!any_shared) {
                    first_shared = full_address;
                    any_shared = true;
                }
                bool is_head = false;
                const auto it = predecessors.find(full_address);
                if (it != predecessors.end()) {
                    for (const uint32_t predecessor : it->second) {
                        const auto owned = owners.find(predecessor);
                        if (owned == owners.end() || owned->second != set ||
                            entries_.count(predecessor)) {
                            is_head = true;
                            break;
                        }
                    }
                }
                if (is_head) shared_heads.push_back(full_address);
            }
            if (any_shared) {
                if (shared_heads.empty()) shared_heads.push_back(first_shared);
                for (const uint32_t head : shared_heads) entries_.emplace(head, "shared_tail");
                changed = true;
            }

            // A stretch of a function that does not follow on from any of
            // its blocks in the ROM, and that the function gets to only by
            // JP, is another routine it ends by jumping to: hand-written
            // code keeps a routine in one piece, and a JP where a JR would
            // not reach. Scored against a disassembly of Pokémon Red, four
            // in five of these are functions there, and most of the rest
            // are stretches as long as one. (Reached by JR, one in ten;
            // following on from the function, a JP's target is its own
            // local label more often than not.) Not in code only the scans
            // found, where a stretch apart is usually data.
            if (!changed) {
                std::map<uint32_t, std::vector<uint32_t>> blocks_of;
                for (const auto& [full_address, set] : owners) {
                    blocks_of[*set.begin()].push_back(full_address);
                }
                for (const auto& [entry, blocks] : blocks_of) {
                    if (std::string(entries_.at(entry)) == "unreferenced") continue;
                    std::set<uint32_t> follows;  // where each of its blocks ends
                    for (const uint32_t block : blocks) {
                        follows.insert((block & 0xFFFF0000u) | block_infos_.at(block).end);
                    }
                    for (const uint32_t block : blocks) {
                        if (block == entry || follows.count(block)) continue;
                        const auto ways = predecessors.find(block);
                        if (ways == predecessors.end()) continue;
                        bool only_by_jp = true;
                        for (const uint32_t from : ways->second) {
                            const BlockInfo& info = block_infos_.at(from);
                            const uint8_t opcode = info.instructions.back()->opcode;
                            const bool is_jp = opcode == 0xC3 || opcode == 0xC2 || opcode == 0xCA ||
                                               opcode == 0xD2 || opcode == 0xDA;
                            bool by_jump = false;
                            for (const BlockExit& exit : info.exits) {
                                if (!exit.to_block || exit.target.full() != block) continue;
                                by_jump = exit.is_jump && !exit.is_computed && !exit.comes_back;
                                if (!by_jump) break;
                            }
                            if (!is_jp || !by_jump) {
                                only_by_jp = false;
                                break;
                            }
                        }
                        if (only_by_jp && entries_.emplace(block, "detached_tail").second) changed = true;
                    }
                }
            }

            if (!changed) {
                for (const auto& [full_address, set] : owners) {
                    owner_[full_address] = *set.begin();
                }
                break;
            }
        }
    }

    void build_models() {
        models_.reserve(entries_.size());
        for (const auto& [full_address, kind] : entries_) {
            model_index_[full_address] = models_.size();
            FunctionModel model;
            model.full_address = full_address;
            model.bank = static_cast<BankId>(full_address >> 16);
            model.entry_address = static_cast<uint16_t>(full_address & 0xFFFFu);
            model.kind = kind;
            const auto function = analysis_.functions.find(full_address);
            model.recompiler_function = function != analysis_.functions.end();
            model.name = model.recompiler_function
                             ? function->second.name
                             : generate_function_name(model.bank, model.entry_address);
            model.block_addresses.push_back(full_address);
            models_.push_back(std::move(model));
        }
        for (const auto& [block, entry] : owner_) {
            if (block != entry) models_[model_index_.at(entry)].block_addresses.push_back(block);
        }
        for (FunctionModel& model : models_) {
            build_model(model);
            solve_site_constants(model);
        }
        attach_far_edges();
    }

    /* Put each call made through a helper on the CALL or jump that makes it.
     * The edges name the function the site was in when they were found; the
     * functions may be cut differently now, so the site is looked up by where
     * it is. */
    void attach_far_edges() {
        if (options_.far_edges.empty() && options_.entered.empty()) return;
        std::map<std::pair<uint32_t, bool>, uint32_t> where;  // (site, is a jump) -> target
        struct Table {
            uint32_t address = 0;
            std::map<uint16_t, uint32_t> entries;  // index -> target
        };
        std::map<std::pair<uint32_t, bool>, Table> tables;
        std::map<uint32_t, std::set<uint32_t>> seen_from_call;  // CALL site -> targets
        std::map<uint32_t, std::set<uint32_t>> seen_at_jump;    // computed jump -> targets
        for (const LiftFarEdge& edge : options_.far_edges) {
            const BankId bank = edge.site >= 0x4000 ? static_cast<BankId>(edge.from >> 16) : 0;
            const std::pair<uint32_t, bool> key{AnalysisResult::make_addr(bank, edge.site), edge.is_jump};
            if (edge.seen) {
                if (edge.is_computed) {
                    if (edge.from != 0 || edge.site != 0) seen_at_jump[key.first].insert(edge.target);
                } else {
                    seen_from_call[key.first].insert(edge.target);
                }
                continue;
            }
            if (edge.from_table) {
                tables[key].address = edge.table;
                tables[key].entries[edge.table_index] = edge.target;
            } else {
                where[key] = edge.target;
            }
        }
        for (FunctionModel& model : models_) {
            auto attach = [&](CallSite& site, bool is_jump) {
                const BankId bank = site.site >= 0x4000 ? model.bank : 0;
                const std::pair<uint32_t, bool> key{AnalysisResult::make_addr(bank, site.site), is_jump};
                const auto table = tables.find(key);
                if (table != tables.end()) {
                    site.reaches_table = true;
                    site.table = table->second.address;
                    for (const auto& [index, target] : table->second.entries) {
                        if (model_index_.count(target)) site.table_entries.push_back(target);
                    }
                    return;
                }
                const auto found = where.find(key);
                if (found == where.end() || !model_index_.count(found->second)) return;
                site.reaches_known = true;
                site.reaches = found->second;
            };
            for (CallSite& call : model.calls) attach(call, false);
            for (CallSite& jump : model.jumps_out) {
                if (!jump.is_fallthrough && !jump.is_computed) attach(jump, true);
            }
            for (CallSite& call : model.calls) {
                const BankId bank = call.site >= 0x4000 ? model.bank : 0;
                const auto found = seen_from_call.find(AnalysisResult::make_addr(bank, call.site));
                if (found == seen_from_call.end()) continue;
                for (const uint32_t target : found->second) {
                    if (model_index_.count(target)) call.seen.push_back(target);
                }
            }
            // A computed jump is a JP HL, or a RET the run saw used as one.
            for (const LocalBlock& block : model.blocks) {
                for (const Instruction* instr : block.instructions) {
                    if (instr->type != InstructionType::JP_HL && !is_return(*instr)) continue;
                    const BankId bank = instr->address >= 0x4000 ? model.bank : 0;
                    const auto found = seen_at_jump.find(AnalysisResult::make_addr(bank, instr->address));
                    if (found == seen_at_jump.end()) continue;
                    for (const uint32_t target : found->second) {
                        if (model_index_.count(target)) model.seen_jumps[instr->address].push_back(target);
                    }
                }
            }
        }
        for (const uint32_t address : options_.entered) {
            const auto found = model_index_.find(address);
            if (found != model_index_.end()) models_[found->second].ran = true;
        }
    }

    void build_model(FunctionModel& model) {
        std::map<uint32_t, size_t> local_index;
        for (const uint32_t address : model.block_addresses) {
            const BlockInfo& info = block_infos_.at(address);
            LocalBlock local;
            local.start = info.start;
            local.end = info.end;
            local.instructions = info.instructions;
            local_index[address] = model.blocks.size();
            model.blocks.push_back(std::move(local));
        }

        for (size_t i = 0; i < model.blocks.size(); ++i) {
            LocalBlock& block = model.blocks[i];
            const BlockInfo& info = block_infos_.at(model.block_addresses[i]);
            model.instruction_count += block.instructions.size();
            for (const Instruction* instr : block.instructions) {
                model.byte_count += instr->length;
                note_instruction(model, *instr);
            }
            scan_memory(model, block);

            block.returns = info.returns;
            block.unknown_exit = info.unknown_exit;
            for (const BlockExit& exit : info.exits) {
                const auto local = exit.to_block ? local_index.find(exit.target.full())
                                                 : local_index.end();
                if (exit.comes_back) {
                    // Back into this function, or the jump is one that
                    // leaves like any other: the code it goes to returns
                    // into whichever function the address is in.
                    if (local != local_index.end()) {
                        block.successors.push_back(local->second);
                        block.back_successors.push_back(local->second);
                        block.back_certain = info.back_certain;
                    }
                    continue;
                }
                if (local != local_index.end() && local->second != 0) {
                    block.successors.push_back(local->second);
                    continue;
                }
                if (local != local_index.end()) {
                    // Back to the function's own entry: a loop, not a call.
                    block.successors.push_back(0);
                    continue;
                }
                const bool to_function = exit.to_block && entries_.count(exit.target.full()) != 0;
                if (to_function) {
                    block.tail_functions.push_back(exit.target.full());
                } else {
                    block.unknown_exit = true;
                }
                if (exit.target.is_ram) model.hazards.insert("calls_ram");
                if (exit.is_jump || to_function) {
                    CallSite jump;
                    jump.site = exit.site;
                    jump.target = exit.target;
                    jump.conditional = exit.conditional;
                    jump.target_is_function = to_function;
                    jump.is_computed = exit.is_computed;
                    jump.is_fallthrough = !exit.is_jump && !exit.is_computed;
                    model.jumps_out.push_back(jump);
                }
            }
            if (block.unknown_exit) model.hazards.insert("unknown_exit");
        }

        const auto function = analysis_.functions.find(model.full_address);
        const bool may_switch =
            function != analysis_.functions.end() && function->second.may_switch_rom_bank;
        if (may_switch || !model.bank_writes.empty()) model.hazards.insert("bank_switch");
        if (std::string(model.kind) == "interrupt") model.hazards.insert("interrupt_handler");
        if (model.entry_address >= 0x8000) model.hazards.insert("ram_code");
        if (model.entry_address < 0x4000 && model.bank != 0) model.hazards.insert("lower_window_bank");
    }

    void note_instruction(FunctionModel& model, const Instruction& instr) {
        switch (instr.type) {
            case InstructionType::HALT: model.hazards.insert("halt"); break;
            case InstructionType::STOP: model.hazards.insert("stop"); break;
            case InstructionType::DI:
            case InstructionType::EI:
            case InstructionType::RETI:
                model.hazards.insert("interrupt_control");
                break;
            case InstructionType::UNDEFINED:
            case InstructionType::INVALID:
                model.hazards.insert("undefined_opcode");
                break;
            case InstructionType::JP_HL:
                model.hazards.insert("computed_jump");
                model.computed_jumps.push_back(instr.address);
                break;
            case InstructionType::LD_SP_HL:
            case InstructionType::LD_NN_SP:
            case InstructionType::LD_HL_SP_N:
            case InstructionType::ADD_SP_N:
                model.hazards.insert("sp_manipulation");
                break;
            case InstructionType::PUSH:
                model.pushes.insert(pair_name((instr.opcode >> 4) & 3u, true));
                break;
            case InstructionType::POP:
                model.pops.insert(pair_name((instr.opcode >> 4) & 3u, true));
                break;
            default:
                break;
        }
        if (!instr.is_cb_prefixed) {
            switch (instr.opcode) {
                case 0x31: case 0x33: case 0x3B: case 0x39:  // LD SP,nn / INC SP / DEC SP / ADD HL,SP
                    model.hazards.insert("sp_manipulation");
                    break;
                default:
                    break;
            }
        }
        if (is_return(instr)) {
            const char* kind = instr.type == InstructionType::RETI
                                   ? "reti"
                                   : (instr.type == InstructionType::RET_CC ? "ret_cc" : "ret");
            model.returns.emplace_back(instr.address, kind);
        }
        if (is_call(instr)) {
            CallSite call;
            call.site = instr.address;
            call.target = resolve_target(rom_, instr, direct_target_address(instr));
            call.conditional = is_conditional(instr);
            call.is_rst = instr.type == InstructionType::RST;
            call.target_is_function = is_function(call.target);
            if (call.target.is_ram) {
                model.hazards.insert("calls_ram");
            } else if (!call.target_is_function) {
                model.hazards.insert("unresolved_call");
            }
            model.calls.push_back(call);
        }
    }

    void record_access(FunctionModel& model, int address, uint16_t site, bool read, bool write) {
        if (address < 0) return;
        const uint16_t addr = static_cast<uint16_t>(address);
        MemoryAccess& access = model.memory[addr];
        access.read = access.read || read;
        access.write = access.write || write;
        access.sites.insert(site);
        if (read && is_volatile_io(addr)) model.hazards.insert("reads_volatile_io");
    }

    void scan_memory(FunctionModel& model, const LocalBlock& block) {
        KnownRegisters known;
        enum { B = 0, C = 1, D = 2, E = 3, H = 4, L = 5, A = 7 };
        for (const Instruction* pointer : block.instructions) {
            const Instruction& instr = *pointer;
            const uint16_t site = instr.address;
            const int hl = known.pair(H, L);
            const uint8_t imm8 = operand8(instr);
            const uint16_t imm16 = operand16(instr);

            if (instr.is_cb_prefixed) {
                const uint8_t reg = instr.cb_opcode & 7u;
                if (reg == 6) {
                    const bool is_bit = (instr.cb_opcode >> 6) == 1;
                    record_access(model, hl, site, true, !is_bit);
                }
                follow_constants(known, instr);
                continue;
            }

            const uint8_t op = instr.opcode;
            // Fixed-address memory operands.
            switch (op) {
                case 0xFA: record_access(model, imm16, site, true, false); break;
                case 0xEA:
                    if (imm16 < 0x8000) {
                        model.bank_writes.push_back({site, imm16, known.value[A]});
                    } else {
                        record_access(model, imm16, site, false, true);
                    }
                    break;
                case 0x08:
                    record_access(model, imm16, site, false, true);
                    record_access(model, (imm16 + 1) & 0xFFFF, site, false, true);
                    break;
                case 0xF0: record_access(model, 0xFF00 + imm8, site, true, false); break;
                case 0xE0: record_access(model, 0xFF00 + imm8, site, false, true); break;
                case 0xF2:
                    if (known.value[C] >= 0) record_access(model, 0xFF00 + known.value[C], site, true, false);
                    break;
                case 0xE2:
                    if (known.value[C] >= 0) record_access(model, 0xFF00 + known.value[C], site, false, true);
                    break;
                case 0x0A: record_access(model, known.pair(B, C), site, true, false); break;
                case 0x1A: record_access(model, known.pair(D, E), site, true, false); break;
                case 0x02: record_access(model, known.pair(B, C), site, false, true); break;
                case 0x12: record_access(model, known.pair(D, E), site, false, true); break;
                case 0x2A: case 0x3A: record_access(model, hl, site, true, false); break;
                case 0x22: case 0x32: case 0x36:
                    if (hl >= 0 && hl < 0x8000) {
                        model.bank_writes.push_back(
                            {site, static_cast<uint16_t>(hl), op == 0x36 ? imm8 : known.value[A]});
                    } else {
                        record_access(model, hl, site, false, true);
                    }
                    break;
                case 0x34: case 0x35: record_access(model, hl, site, true, true); break;
                default:
                    if (op >= 0x40 && op <= 0x7F && op != 0x76) {
                        const uint8_t dst = (op >> 3) & 7u;
                        const uint8_t src = op & 7u;
                        if (src == 6) record_access(model, hl, site, true, false);
                        if (dst == 6) {
                            if (hl >= 0 && hl < 0x8000) {
                                model.bank_writes.push_back(
                                    {site, static_cast<uint16_t>(hl), known.value[src]});
                            } else {
                                record_access(model, hl, site, false, true);
                            }
                        }
                    } else if (op >= 0x80 && op <= 0xBF && (op & 7u) == 6) {
                        record_access(model, hl, site, true, false);
                    }
                    break;
            }

            // A 16-bit immediate that is an address.
            if ((op & 0xCFu) == 0x01u && ((op >> 4) & 3u) != 3u) {
                if (imm16 >= 0x8000) {
                    MemoryAccess& access = model.memory[imm16];
                    access.pointer = true;
                    access.sites.insert(site);
                } else if (imm16 >= 0x0150) {
                    RomReference reference;
                    reference.site = site;
                    reference.address = imm16;
                    const Target target = resolve_target(rom_, instr, imm16);
                    reference.bank_known = target.known;
                    reference.bank = target.bank;
                    if (target.known) {
                        const size_t offset = rom_offset(target.bank, imm16);
                        reference.is_code = offset < covered_.size() && covered_[offset];
                    }
                    model.rom_references.push_back(reference);
                }
            }

            follow_constants(known, instr);
        }
    }

    /* What one instruction does to the constants known to be in the registers. */
    void follow_constants(KnownRegisters& known, const Instruction& instr) const {
        enum { B = 0, C = 1, D = 2, E = 3, H = 4, L = 5, A = 7 };
        if (is_call(instr)) {
            known = KnownRegisters();
            return;
        }
        if (instr.is_cb_prefixed) {
            known.forget(lift_instruction_effect(instr).writes);
            return;
        }
        const uint8_t op = instr.opcode;
        const int hl = known.pair(H, L);
        if ((op & 0xC7u) == 0x06u && ((op >> 3) & 7u) != 6u) {
            known.value[(op >> 3) & 7u] = operand8(instr);
        } else if ((op & 0xCFu) == 0x01u) {
            const uint16_t imm16 = operand16(instr);
            switch ((op >> 4) & 3u) {
                case 0: known.set_pair(B, C, imm16); break;
                case 1: known.set_pair(D, E, imm16); break;
                case 2: known.set_pair(H, L, imm16); break;
                default: break;
            }
        } else if (op >= 0x40 && op <= 0x7F && op != 0x76 && (op & 7u) != 6u &&
                   ((op >> 3) & 7u) != 6u) {
            known.value[(op >> 3) & 7u] = known.value[op & 7u];
        } else if (op == 0xAF) {
            known.value[A] = 0;
        } else if ((op & 0xCFu) == 0x03u || (op & 0xCFu) == 0x0Bu) {
            const int delta = (op & 0xCFu) == 0x03u ? 1 : -1;
            auto step = [&](uint8_t high, uint8_t low) {
                const int value = known.pair(high, low);
                known.set_pair(high, low, value < 0 ? -1 : ((value + delta) & 0xFFFF));
            };
            switch ((op >> 4) & 3u) {
                case 0: step(B, C); break;
                case 1: step(D, E); break;
                case 2: step(H, L); break;
                default: break;
            }
        } else if (op == 0x22 || op == 0x2A || op == 0x32 || op == 0x3A) {
            const int delta = (op == 0x22 || op == 0x2A) ? 1 : -1;
            known.set_pair(H, L, hl < 0 ? -1 : ((hl + delta) & 0xFFFF));
            if (op == 0x2A || op == 0x3A) known.value[A] = -1;
        } else {
            known.forget(lift_instruction_effect(instr).writes);
        }
    }

    /* The constants in the registers where each call and each jump is made:
     * what a helper that takes its target in registers was handed. Followed
     * through the whole function, so the loads need not be in the block the
     * call is in; a register two ways in disagree about is not known. */
    void solve_site_constants(FunctionModel& model) const {
        const size_t count = model.blocks.size();
        std::vector<KnownRegisters> in(count);
        std::vector<uint8_t> reached(count, 0);
        if (count == 0) return;
        reached[0] = 1;  // entered with nothing known
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < count; ++i) {
                if (!reached[i]) continue;
                KnownRegisters known = in[i];
                for (const Instruction* instr : model.blocks[i].instructions) {
                    follow_constants(known, *instr);
                }
                // Other code has run before a JP HL comes back.
                if (!model.blocks[i].back_successors.empty()) known = KnownRegisters();
                for (const size_t successor : model.blocks[i].successors) {
                    if (!reached[successor]) {
                        reached[successor] = 1;
                        in[successor] = successor == 0 ? KnownRegisters() : known;
                        changed = true;
                        continue;
                    }
                    for (size_t r = 0; r < in[successor].value.size(); ++r) {
                        if (in[successor].value[r] >= 0 && in[successor].value[r] != known.value[r]) {
                            in[successor].value[r] = -1;
                            changed = true;
                        }
                    }
                }
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if (!reached[i]) continue;
            KnownRegisters known = in[i];
            for (const Instruction* instr : model.blocks[i].instructions) {
                if (is_call(*instr) || is_direct_jump(*instr)) {
                    model.site_known[instr->address] = known.value;
                }
                follow_constants(known, *instr);
            }
        }
    }

    /* Registers a function may read before writing them, may write, and
     * always writes. Callees contribute through their own summaries, so the
     * whole program is iterated to a fixed point. */
    bool solve_function(FunctionModel& model) {
        Summary next;
        next.may_def = 0;
        next.incomplete = false;

        auto callee_summary = [&](const Target& target, bool& known) -> const Summary* {
            known = is_function(target);
            if (!known) return nullptr;
            return &models_[model_index_.at(target.full())].summary;
        };

        const size_t count = model.blocks.size();
        // Backward: live registers.
        std::vector<uint16_t> live_in(count, 0);
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = count; i-- > 0;) {
                const LocalBlock& block = model.blocks[i];
                uint16_t live = 0;
                for (const size_t successor : block.successors) live |= live_in[successor];
                for (const uint32_t tail : block.tail_functions) {
                    live |= models_[model_index_.at(tail)].summary.live_in;
                }
                for (size_t k = block.instructions.size(); k-- > 0;) {
                    const Instruction& instr = *block.instructions[k];
                    const LiftInstructionEffect effect = lift_instruction_effect(instr);
                    if (is_call(instr)) {
                        bool known = false;
                        const Summary* callee = callee_summary(
                            resolve_target(rom_, instr, direct_target_address(instr)), known);
                        if (known) {
                            const uint16_t through =
                                static_cast<uint16_t>(callee->live_in | (live & ~callee->must_def));
                            live = is_conditional(instr) ? static_cast<uint16_t>(through | live) : through;
                        }
                        live |= effect.reads;
                    } else if (is_return(instr) && !is_conditional(instr)) {
                        live = effect.reads;
                    } else {
                        live = static_cast<uint16_t>((live & ~effect.writes) | effect.reads);
                    }
                }
                if (live != live_in[i]) {
                    live_in[i] = live;
                    changed = true;
                }
            }
        }
        next.live_in = count ? live_in[0] : 0;

        // The same again with everything the first pass leaves out counted
        // as reading every register: a callee that is not a known function,
        // a way out that is not a return, and a return that does not go back
        // to the caller. Unlike live_in, this holds for an incomplete
        // function too.
        std::vector<uint16_t> need_in(count, 0);
        changed = true;
        while (changed) {
            changed = false;
            for (size_t i = count; i-- > 0;) {
                const LocalBlock& block = model.blocks[i];
                uint16_t need = block.unknown_exit ? LIFT_REG_ALL : 0;
                for (const size_t successor : block.successors) need |= need_in[successor];
                for (const uint32_t tail : block.tail_functions) {
                    need |= model.balanced ? models_[model_index_.at(tail)].summary.need_in
                                           : LIFT_REG_ALL;
                }
                for (size_t k = block.instructions.size(); k-- > 0;) {
                    const Instruction& instr = *block.instructions[k];
                    const LiftInstructionEffect effect = lift_instruction_effect(instr);
                    if (is_call(instr)) {
                        bool known = false;
                        const Summary* callee = callee_summary(
                            resolve_target(rom_, instr, direct_target_address(instr)), known);
                        if (known) {
                            const uint16_t through =
                                static_cast<uint16_t>(callee->need_in | (need & ~callee->must_def));
                            need = is_conditional(instr) ? static_cast<uint16_t>(through | need) : through;
                        } else {
                            need = LIFT_REG_ALL;
                        }
                        need |= effect.reads;
                    } else if (is_return(instr)) {
                        need = static_cast<uint16_t>((is_conditional(instr) ? need : 0) |
                                                     (model.balanced ? 0 : LIFT_REG_ALL) |
                                                     effect.reads);
                    } else {
                        need = static_cast<uint16_t>((need & ~effect.writes) | effect.reads);
                    }
                }
                if (need != need_in[i]) {
                    need_in[i] = need;
                    changed = true;
                }
            }
        }
        next.need_in = count ? need_in[0] : 0;

        // Forward: registers written on every path, and anything written at all.
        std::vector<std::vector<size_t>> predecessors(count);
        for (size_t i = 0; i < count; ++i) {
            for (const size_t successor : model.blocks[i].successors) {
                predecessors[successor].push_back(i);
            }
        }
        std::vector<uint16_t> def_out(count, LIFT_REG_ALL);
        uint16_t must = LIFT_REG_ALL;
        bool has_exit = false;
        changed = true;
        while (changed) {
            changed = false;
            must = LIFT_REG_ALL;
            has_exit = false;
            for (size_t i = 0; i < count; ++i) {
                const LocalBlock& block = model.blocks[i];
                uint16_t defined = LIFT_REG_ALL;
                if (i == 0) {
                    defined = 0;
                } else {
                    for (const size_t predecessor : predecessors[i]) defined &= def_out[predecessor];
                }
                for (const Instruction* pointer : block.instructions) {
                    const Instruction& instr = *pointer;
                    const LiftInstructionEffect effect = lift_instruction_effect(instr);
                    next.may_def |= effect.writes;
                    if (is_call(instr)) {
                        bool known = false;
                        const Summary* callee = callee_summary(
                            resolve_target(rom_, instr, direct_target_address(instr)), known);
                        if (!known || callee->incomplete) {
                            // It may never come back here: a way out, with
                            // what is written so far.
                            next.incomplete = true;
                            must &= defined;
                            has_exit = true;
                        }
                        if (known) {
                            next.may_def |= callee->may_def;
                            if (!is_conditional(instr)) defined |= callee->must_def;
                        }
                    } else if (is_return(instr)) {
                        must &= defined;
                        has_exit = true;
                    } else {
                        defined |= effect.writes;
                    }
                }
                for (const uint32_t tail : block.tail_functions) {
                    const Summary& callee = models_[model_index_.at(tail)].summary;
                    next.may_def |= callee.may_def;
                    if (callee.incomplete) next.incomplete = true;
                    must &= static_cast<uint16_t>(defined | callee.must_def);
                    has_exit = true;
                }
                if (block.unknown_exit) {
                    next.incomplete = true;
                    must &= defined;
                    has_exit = true;
                }
                if (defined != def_out[i]) {
                    def_out[i] = defined;
                    changed = true;
                }
            }
        }
        next.must_def = has_exit ? must : LIFT_REG_ALL;
        // A summary only ever grows less certain; never let an iteration undo that.
        next.live_in |= model.summary.live_in;
        next.need_in |= model.summary.need_in;
        next.may_def |= model.summary.may_def;
        next.must_def &= model.summary.must_def;
        next.incomplete = next.incomplete || model.summary.incomplete;

        const bool differs = next.live_in != model.summary.live_in ||
                             next.need_in != model.summary.need_in ||
                             next.may_def != model.summary.may_def ||
                             next.must_def != model.summary.must_def ||
                             next.incomplete != model.summary.incomplete;
        model.summary = next;
        return differs;
    }

    void solve_summaries() {
        for (int round = 0; round < 64; ++round) {
            bool changed = false;
            for (FunctionModel& model : models_) {
                changed = solve_function(model) || changed;
            }
            if (!changed) break;
        }
    }

    void link_callers() {
        for (const FunctionModel& model : models_) {
            auto add = [&](const CallSite& site) {
                if (site.reaches_known) {
                    models_[model_index_.at(site.reaches)].callers.insert(model.full_address);
                }
                for (const uint32_t entry : site.table_entries) {
                    models_[model_index_.at(entry)].callers.insert(model.full_address);
                }
                for (const uint32_t target : site.seen) {
                    models_[model_index_.at(target)].callers.insert(model.full_address);
                }
                if (!site.target_is_function) return;
                models_[model_index_.at(site.target.full())].callers.insert(model.full_address);
            };
            for (const CallSite& call : model.calls) add(call);
            for (const CallSite& jump : model.jumps_out) add(jump);
            for (const auto& [site, targets] : model.seen_jumps) {
                for (const uint32_t target : targets) {
                    models_[model_index_.at(target)].callers.insert(model.full_address);
                }
            }
        }
    }

    /* ------------------------------------------------------------------- */
    /* What a function owes its callers.                                    */

    static bool writes_sp(const Instruction& instr) {
        switch (instr.type) {
            case InstructionType::LD_SP_HL:
            case InstructionType::ADD_SP_N:
                return true;
            default:
                break;
        }
        // LD SP,nn / INC SP / DEC SP
        return !instr.is_cb_prefixed &&
               (instr.opcode == 0x31 || instr.opcode == 0x33 || instr.opcode == 0x3B);
    }

    /* One function's stack depths. Returns whether every RET in it pops the
     * address its call pushed, and tells the shared tails it runs into how
     * deep the stack is when it does. */
    bool stack_pass(const FunctionModel& model, bool& entries_changed) {
        const size_t count = model.blocks.size();
        std::vector<int> depth_in(count, kDepthUnset);
        depth_in[0] = model.entry_depth;
        bool balanced = model.entry_depth != kDepthUnknown;
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 0; i < count; ++i) {
                if (depth_in[i] == kDepthUnset) continue;
                int depth = depth_in[i];
                const LocalBlock& block = model.blocks[i];
                for (const Instruction* pointer : block.instructions) {
                    const Instruction& instr = *pointer;
                    if (is_return(instr)) {
                        if (depth != 0) balanced = false;
                        continue;
                    }
                    if (depth == kDepthUnknown) continue;
                    if (instr.type == InstructionType::PUSH) {
                        ++depth;
                    } else if (instr.type == InstructionType::POP) {
                        // Below zero it is the return address, or the caller's own.
                        if (--depth < 0) balanced = false;
                    } else if (writes_sp(instr)) {
                        depth = kDepthUnknown;
                        balanced = false;
                    }
                }
                // What a JP HL jumps to pops the address it comes back to.
                if (!block.back_successors.empty() && depth != kDepthUnknown) {
                    if (depth > 0) {
                        --depth;
                    } else {
                        depth = kDepthUnknown;
                        balanced = false;
                    }
                }
                for (const size_t successor : block.successors) {
                    int& target = depth_in[successor];
                    if (target == kDepthUnset) {
                        target = depth;
                        changed = true;
                    } else if (target != depth) {
                        balanced = false;
                        if (target != kDepthUnknown) {
                            target = kDepthUnknown;
                            changed = true;
                        }
                    }
                }
                for (const uint32_t tail : block.tail_functions) {
                    FunctionModel& target = models_[model_index_.at(tail)];
                    if (!is_tail_kind(target.kind)) {
                        // Jumping into a function with something pushed
                        // makes its RET a jump to what was pushed.
                        if (depth != 0) balanced = false;
                    } else if (target.entry_depth == kDepthUnset) {
                        target.entry_depth = depth;
                        entries_changed = true;
                    } else if (target.entry_depth != depth) {
                        balanced = false;
                        if (target.entry_depth != kDepthUnknown) {
                            target.entry_depth = kDepthUnknown;
                            entries_changed = true;
                        }
                    }
                }
            }
        }
        return balanced;
    }

    /* A function is balanced when each of its returns goes back to its
     * caller. One that pops its return address, pushes an address and
     * returns to it, or moves SP, leaves somewhere this analysis cannot
     * follow: its summary is incomplete and it owes its callers everything.
     * A shared tail starts with what the code jumping into it has pushed. */
    void solve_stack() {
        for (FunctionModel& model : models_) {
            model.entry_depth = is_tail_kind(model.kind) ? kDepthUnset : 0;
        }
        for (int round = 0; round < 64; ++round) {
            bool changed = false;
            for (FunctionModel& model : models_) {
                if (model.entry_depth == kDepthUnset) continue;
                const bool balanced = stack_pass(model, changed);
                if (balanced != model.balanced) {
                    model.balanced = balanced;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        for (FunctionModel& model : models_) {
            if (model.entry_depth == kDepthUnset) {
                model.entry_depth = kDepthUnknown;
                model.balanced = false;
            }
            if (!model.balanced) model.summary.incomplete = true;
        }
    }

    /* Ways into a function that no call or jump in the decoded code accounts
     * for. With none, the callers the export lists are all there are. None of
     * this is proof: a table this does not recognise still calls what it
     * points to. It is evidence, erring towards "something else may". */
    void find_unlisted_entries() {
        std::map<uint16_t, std::vector<size_t>> by_address;
        for (size_t i = 0; i < models_.size(); ++i) {
            FunctionModel& model = models_[i];
            by_address[model.entry_address].push_back(i);
            const std::string kind = model.kind;
            if (kind != "call" && !is_tail_kind(model.kind)) model.entered_by.insert(kind);
            if (analysis_.computed_jump_targets.count(model.full_address) ||
                analysis_.branch_entry_targets.count(model.full_address) ||
                analysis_.synthetic_entry_targets.count(model.full_address)) {
                model.entered_by.insert("jump_table");
            }
        }
        // Its callers are listed, but it returns into the helper, and what
        // is read after that is the helper's and its caller's together.
        for (const LiftFarEdge& edge : options_.far_edges) {
            const auto found = model_index_.find(edge.target);
            if (found != model_index_.end()) {
                models_[found->second].entered_by.insert(
                    edge.seen ? "seen" : (edge.from_table ? "table_call" : "far_call"));
            }
        }

        // In a declared table: whatever reads the table can call it, and
        // nothing lists that.
        for (const LiftTableEntry& entry : options_.table_entries) {
            const auto found = model_index_.find(entry.target);
            if (found == model_index_.end()) continue;
            FunctionModel& model = models_[found->second];
            model.entered_by.insert("declared_table");
            if (!model.in_table) model.in_table = &entry;
        }

        // An address loaded into a register pair can be jumped to, or pushed
        // for a RET to take.
        for (const FunctionModel& model : models_) {
            // A load that is the address half of a far call found from here
            // is accounted for.
            std::set<uint32_t> far_called;
            for (const CallSite& call : model.calls) {
                if (call.reaches_known) far_called.insert(call.reaches);
            }
            for (const CallSite& jump : model.jumps_out) {
                if (jump.reaches_known) far_called.insert(jump.reaches);
            }
            for (const RomReference& reference : model.rom_references) {
                const auto found = by_address.find(reference.address);
                if (found == by_address.end()) continue;
                for (const size_t index : found->second) {
                    FunctionModel& target = models_[index];
                    if (reference.bank_known && reference.bank != target.bank) continue;
                    if (far_called.count(target.full_address)) continue;
                    target.entered_by.insert("address_loaded");
                }
            }
        }

        // The address as a word in data: a table of pointers, perhaps. Data
        // is every byte that is not an instruction of a function something
        // reaches. Looked for where such a table would be: the function's
        // own bank, the home bank, and anywhere next to its bank number.
        std::vector<uint8_t> code(rom_.size(), 0);
        for (const FunctionModel& model : models_) {
            if (std::string(model.kind) == "unreferenced") continue;
            for (const LocalBlock& block : model.blocks) {
                for (const Instruction* instr : block.instructions) {
                    if (instr->address >= 0x8000) continue;
                    const size_t offset = rom_offset(instr->bank, instr->address);
                    for (size_t i = 0; i < instr->length && offset + i < code.size(); ++i) {
                        code[offset + i] = 1;
                    }
                }
            }
        }
        const uint8_t* bytes = rom_.data();
        for (size_t offset = 0; offset + 1 < rom_.size(); ++offset) {
            if (code[offset] || code[offset + 1]) continue;
            if ((offset & 0x3FFFu) == 0x3FFFu) continue;  // a word does not span two banks
            const uint16_t word = static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
            const auto found = by_address.find(word);
            if (found == by_address.end()) continue;
            const size_t bank = offset / 0x4000u;
            for (const size_t index : found->second) {
                FunctionModel& target = models_[index];
                bool points = false;
                if (target.entry_address < 0x4000) {
                    points = bank == 0;
                } else {
                    points = bank == 0 || bank == target.bank ||
                             (offset > 0 && !code[offset - 1] && bytes[offset - 1] == target.bank) ||
                             (offset + 2 < rom_.size() && !code[offset + 2] &&
                              bytes[offset + 2] == target.bank);
                }
                if (points) target.entered_by.insert("data_pointer");
            }
        }
    }

    /* Liveness through one function, given what is read after each function
     * returns; adds what this one's calls and jumps out ask of their targets. */
    bool read_after_pass(const FunctionModel& model) {
        const uint16_t at_return = model.read_after;
        bool grew = false;
        auto ask = [&](uint32_t target, uint16_t bits) {
            FunctionModel& callee = models_[model_index_.at(target)];
            const uint16_t merged = static_cast<uint16_t>(callee.read_after | bits);
            if (merged != callee.read_after) {
                callee.read_after = merged;
                grew = true;
            }
        };
        auto before = [&](const Summary& callee, uint16_t after) -> uint16_t {
            return static_cast<uint16_t>(callee.need_in | (after & ~callee.must_def));
        };

        const size_t count = model.blocks.size();
        std::vector<uint16_t> live_in(count, 0);
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = count; i-- > 0;) {
                const LocalBlock& block = model.blocks[i];
                uint16_t live = 0;
                for (const size_t successor : block.successors) live |= live_in[successor];
                for (const uint32_t tail : block.tail_functions) {
                    // The target returns to our caller, or to the address
                    // this function pushed for it.
                    const uint16_t after = block.back_successors.empty() ? at_return : live;
                    ask(tail, after);
                    live |= before(models_[model_index_.at(tail)].summary, after);
                }
                if (block.unknown_exit) live = LIFT_REG_ALL;
                for (size_t k = block.instructions.size(); k-- > 0;) {
                    const Instruction& instr = *block.instructions[k];
                    const LiftInstructionEffect effect = lift_instruction_effect(instr);
                    if (is_call(instr)) {
                        const Target target =
                            resolve_target(rom_, instr, direct_target_address(instr));
                        if (is_function(target)) {
                            ask(target.full(), live);
                            const uint16_t through =
                                before(models_[model_index_.at(target.full())].summary, live);
                            live = is_conditional(instr) ? static_cast<uint16_t>(through | live)
                                                         : through;
                        } else {
                            live = LIFT_REG_ALL;
                        }
                        live |= effect.reads;
                    } else if (is_return(instr)) {
                        live = static_cast<uint16_t>(
                            (is_conditional(instr) ? live : 0) | at_return | effect.reads);
                    } else {
                        live = static_cast<uint16_t>((live & ~effect.writes) | effect.reads);
                    }
                }
                if (live != live_in[i]) {
                    live_in[i] = live;
                    changed = true;
                }
            }
        }
        return grew;
    }

    /* What may be read after each function returns: everything, unless all
     * its callers are known and it returns to them; then what those callers
     * read after the call, and what the callers of a function that jumps
     * into it read. */
    void solve_read_after() {
        for (FunctionModel& model : models_) {
            const bool everything = !model.entered_by.empty() || !model.balanced;
            model.read_after = everything ? LIFT_REG_ALL : 0;
        }
        for (int round = 0; round < 256; ++round) {
            bool grew = false;
            for (const FunctionModel& model : models_) {
                grew = read_after_pass(model) || grew;
            }
            if (!grew) return;
        }
        // Not settled: say nothing rather than something wrong.
        for (FunctionModel& model : models_) model.read_after = LIFT_REG_ALL;
    }

    /* ------------------------------------------------------------------- */

    void emit_target(std::ostringstream& out, const CallSite& site) const {
        out << "\"site\": " << site.site << ", \"target\": " << site.target.address
            << ", \"target_bank\": ";
        if (site.target.known) {
            out << site.target.bank;
        } else {
            out << "null";
        }
        out << ", \"conditional\": " << (site.conditional ? "true" : "false") << ", \"function\": ";
        if (site.target_is_function) {
            out << json_string(function_id(site.target.bank, site.target.address));
        } else {
            out << "null";
        }
    }

    /* `, "reaches": {...}`: where the helper this site calls takes it. */
    void emit_places(std::ostringstream& out, const std::vector<uint32_t>& places) const {
        out << "[";
        for (size_t i = 0; i < places.size(); ++i) {
            const BankId bank = static_cast<BankId>(places[i] >> 16);
            const uint16_t address = static_cast<uint16_t>(places[i] & 0xFFFFu);
            out << (i ? ", " : "") << "{\"bank\": " << bank << ", \"address\": " << address
                << ", \"function\": " << json_string(function_id(bank, address)) << "}";
        }
        out << "]";
    }

    void emit_reaches(std::ostringstream& out, const CallSite& site) const {
        if (!site.seen.empty()) {
            out << ", \"reaches_seen\": ";
            emit_places(out, site.seen);
        }
        if (site.reaches_table) {
            out << ", \"reaches_table\": {\"bank\": " << (site.table >> 16) << ", \"address\": "
                << (site.table & 0xFFFFu) << ", \"entries\": [";
            for (size_t i = 0; i < site.table_entries.size(); ++i) {
                const BankId bank = static_cast<BankId>(site.table_entries[i] >> 16);
                const uint16_t address = static_cast<uint16_t>(site.table_entries[i] & 0xFFFFu);
                out << (i ? ", " : "") << "{\"bank\": " << bank << ", \"address\": " << address
                    << ", \"function\": " << json_string(function_id(bank, address)) << "}";
            }
            out << "]}";
            return;
        }
        if (!site.reaches_known) return;
        const BankId bank = static_cast<BankId>(site.reaches >> 16);
        const uint16_t address = static_cast<uint16_t>(site.reaches & 0xFFFFu);
        out << ", \"reaches\": {\"bank\": " << bank << ", \"address\": " << address
            << ", \"function\": " << json_string(function_id(bank, address)) << "}";
    }

    /* `, "known": {"b": 3, "h": 120, "l": 80}`: register constants at a site, if any. */
    void emit_known(std::ostringstream& out, const FunctionModel& model, uint16_t site) const {
        const auto found = model.site_known.find(site);
        if (found == model.site_known.end()) return;
        static const char* const kNames[8] = {"b", "c", "d", "e", "h", "l", nullptr, "a"};
        bool any = false;
        for (size_t r = 0; r < 8; ++r) {
            if (!kNames[r] || found->second[r] < 0) continue;
            out << (any ? ", " : ", \"known\": {") << "\"" << kNames[r] << "\": " << found->second[r];
            any = true;
        }
        if (any) out << "}";
    }

    std::string symbol_name(BankId bank, uint16_t address) const {
        auto it = analysis_.symbol_metadata.find(AnalysisResult::make_addr(bank, address));
        if (it == analysis_.symbol_metadata.end() && address >= 0x8000) {
            it = analysis_.symbol_metadata.find(AnalysisResult::make_addr(0, address));
        }
        if (it == analysis_.symbol_metadata.end()) return {};
        // Only a name the user's symbols gave: emitted names are the recompiler's own.
        return it->second.source_name;
    }

    std::string memory_name(uint16_t address) const {
        const std::string symbol = symbol_name(0, address);
        if (!symbol.empty()) return symbol;
        const char* io = io_register_name(address);
        return io ? io : "";
    }

    void emit_function(std::ostringstream& out, const FunctionModel& model) const {
        out << "    {\n";
        out << "      \"id\": " << json_string(function_id(model.bank, model.entry_address))
            << ",\n";
        out << "      \"name\": " << json_string(model.name) << ",\n";
        const std::string symbol = symbol_name(model.bank, model.entry_address);
        if (!symbol.empty()) out << "      \"symbol\": " << json_string(symbol) << ",\n";
        out << "      \"bank\": " << model.bank << ",\n";
        out << "      \"address\": " << model.entry_address << ",\n";
        out << "      \"kind\": \"" << model.kind << "\",\n";
        out << "      \"recompiler_function\": " << (model.recompiler_function ? "true" : "false")
            << ",\n";
        out << "      \"instruction_count\": " << model.instruction_count << ",\n";
        out << "      \"byte_count\": " << model.byte_count << ",\n";

        out << "      \"hazards\": [";
        {
            bool first = true;
            for (const std::string& hazard : model.hazards) {
                if (!first) out << ", ";
                first = false;
                out << json_string(hazard);
            }
        }
        out << "],\n";

        out << "      \"registers\": {\"reads\": " << register_list(model.summary.live_in & kReported)
            << ", \"needs\": " << register_list(model.summary.need_in & kReported)
            << ", \"writes\": " << register_list(model.summary.may_def & kReported)
            << ", \"always_writes\": " << register_list(model.summary.must_def & kReported)
            << ", \"read_after\": " << register_list(model.read_after & kReported) << "},\n";
        out << "      \"flags\": {\"reads\": " << flag_list(model.summary.live_in)
            << ", \"needs\": " << flag_list(model.summary.need_in)
            << ", \"writes\": " << flag_list(model.summary.may_def)
            << ", \"always_writes\": " << flag_list(model.summary.must_def)
            << ", \"read_after\": " << flag_list(model.read_after) << "},\n";
        out << "      \"summary_incomplete\": " << (model.summary.incomplete ? "true" : "false")
            << ",\n";
        out << "      \"stack_balanced\": " << (model.balanced ? "true" : "false") << ",\n";
        out << "      \"entered_by\": [";
        {
            bool first = true;
            for (const std::string& way : model.entered_by) {
                if (!first) out << ", ";
                first = false;
                out << json_string(way);
            }
        }
        out << "],\n";

        auto emit_names = [&](const char* key, const std::set<std::string>& names) {
            out << "      \"" << key << "\": [";
            bool first = true;
            for (const std::string& name : names) {
                if (!first) out << ", ";
                first = false;
                out << json_string(name);
            }
            out << "],\n";
        };
        emit_names("pushes", model.pushes);
        emit_names("pops", model.pops);

        out << "      \"calls\": [";
        for (size_t i = 0; i < model.calls.size(); ++i) {
            out << (i ? ", " : "") << "{";
            emit_target(out, model.calls[i]);
            out << ", \"kind\": " << (model.calls[i].is_rst ? "\"rst\"" : "\"call\"");
            emit_known(out, model, model.calls[i].site);
            emit_reaches(out, model.calls[i]);
            out << "}";
        }
        out << "],\n";
        out << "      \"jumps_out\": [";
        for (size_t i = 0; i < model.jumps_out.size(); ++i) {
            const CallSite& jump = model.jumps_out[i];
            out << (i ? ", " : "") << "{";
            emit_target(out, jump);
            out << ", \"kind\": \""
                << (jump.is_computed ? "computed" : (jump.is_fallthrough ? "fallthrough" : "jump"))
                << "\"";
            if (!jump.is_computed && !jump.is_fallthrough) {
                emit_known(out, model, jump.site);
                emit_reaches(out, jump);
            }
            out << "}";
        }
        out << "],\n";
        out << "      \"computed_jumps\": [";
        for (size_t i = 0; i < model.computed_jumps.size(); ++i) {
            out << (i ? ", " : "") << model.computed_jumps[i];
        }
        out << "],\n";
        if (!model.seen_jumps.empty()) {
            out << "      \"seen_jumps\": [";
            bool first = true;
            for (const auto& [site, targets] : model.seen_jumps) {
                out << (first ? "" : ", ") << "{\"site\": " << site << ", \"targets\": ";
                emit_places(out, targets);
                out << "}";
                first = false;
            }
            out << "],\n";
        }
        if (!options_.entered.empty()) {
            out << "      \"ran\": " << (model.ran ? "true" : "false") << ",\n";
        }
        if (model.in_table) {
            out << "      \"declared_table\": {\"name\": " << json_string(model.in_table->name)
                << ", \"bank\": " << (model.in_table->table >> 16)
                << ", \"address\": " << (model.in_table->table & 0xFFFFu)
                << ", \"index\": " << model.in_table->index << "},\n";
        }
        out << "      \"returns\": [";
        for (size_t i = 0; i < model.returns.size(); ++i) {
            out << (i ? ", " : "") << "{\"site\": " << model.returns[i].first << ", \"kind\": \""
                << model.returns[i].second << "\"}";
        }
        out << "],\n";
        out << "      \"callers\": " << id_list(model.callers) << ",\n";

        out << "      \"memory\": [";
        {
            bool first = true;
            for (const auto& [address, access] : model.memory) {
                if (!first) out << ", ";
                first = false;
                std::string mode;
                if (access.read) mode += "r";
                if (access.write) mode += "w";
                if (access.pointer) mode += "p";
                out << "{\"address\": " << address << ", \"space\": \"" << memory_space(address)
                    << "\", \"access\": \"" << mode << "\"";
                const std::string name = memory_name(address);
                if (!name.empty()) out << ", \"name\": " << json_string(name);
                out << ", \"sites\": [";
                bool first_site = true;
                for (const uint16_t site : access.sites) {
                    if (!first_site) out << ", ";
                    first_site = false;
                    out << site;
                }
                out << "]}";
            }
        }
        out << "],\n";

        out << "      \"bank_writes\": [";
        for (size_t i = 0; i < model.bank_writes.size(); ++i) {
            const BankWrite& write = model.bank_writes[i];
            out << (i ? ", " : "") << "{\"site\": " << write.site << ", \"address\": "
                << write.address << ", \"value\": ";
            if (write.value >= 0) {
                out << write.value;
            } else {
                out << "null";
            }
            out << "}";
        }
        out << "],\n";

        out << "      \"rom_references\": [";
        for (size_t i = 0; i < model.rom_references.size(); ++i) {
            const RomReference& reference = model.rom_references[i];
            out << (i ? ", " : "") << "{\"site\": " << reference.site << ", \"address\": "
                << reference.address << ", \"bank\": ";
            if (reference.bank_known) {
                out << reference.bank;
            } else {
                out << "null";
            }
            out << ", \"points_at\": \""
                << (!reference.bank_known ? "unknown" : (reference.is_code ? "code" : "data"))
                << "\"}";
        }
        out << "],\n";

        out << "      \"blocks\": [\n";
        for (size_t i = 0; i < model.blocks.size(); ++i) {
            const LocalBlock& block = model.blocks[i];
            out << "        {\"address\": " << block.start << ", \"end\": "
                << block.end << ", \"successors\": [";
            for (size_t k = 0; k < block.successors.size(); ++k) {
                out << (k ? ", " : "")
                    << model.blocks[block.successors[k]].start;
            }
            out << "], \"leaves_to\": [";
            for (size_t k = 0; k < block.tail_functions.size(); ++k) {
                out << (k ? ", " : "")
                    << json_string(function_id(static_cast<BankId>(block.tail_functions[k] >> 16),
                                               static_cast<uint16_t>(block.tail_functions[k] & 0xFFFFu)));
            }
            out << "], \"returns\": " << (block.returns ? "true" : "false")
                << ", \"unknown_exit\": " << (block.unknown_exit ? "true" : "false");
            if (!block.back_successors.empty()) {
                out << ", \"jump_returns_to\": [";
                for (size_t k = 0; k < block.back_successors.size(); ++k) {
                    out << (k ? ", " : "")
                        << model.blocks[block.back_successors[k]].start;
                }
                out << "], \"jump_returns_certain\": " << (block.back_certain ? "true" : "false");
            }
            out << ", \"instructions\": [\n";
            for (size_t k = 0; k < block.instructions.size(); ++k) {
                const Instruction& instr = *block.instructions[k];
                out << "          {\"address\": " << instr.address << ", \"bytes\": \"";
                for (uint8_t b = 0; b < instr.length; ++b) {
                    const uint8_t byte = instr.address < 0x8000
                                             ? rom_.read_banked(instr.bank,
                                                                static_cast<uint16_t>(instr.address + b))
                                             : 0;
                    out << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                        << static_cast<unsigned>(byte) << std::dec << std::nouppercase;
                }
                out << "\", \"asm\": " << json_string(lift_disassemble(instr, operand8(instr), operand16(instr))) << ", \"cycles\": "
                    << static_cast<unsigned>(instr.cycles);
                if (is_conditional(instr)) {
                    out << ", \"cycles_taken\": " << static_cast<unsigned>(instr.cycles_branch);
                }
                if (is_direct_jump(instr) || is_call(instr)) {
                    const Target target = resolve_target(rom_, instr, direct_target_address(instr));
                    out << ", \"target\": " << target.address << ", \"target_bank\": ";
                    if (target.known) {
                        out << target.bank;
                    } else {
                        out << "null";
                    }
                }
                out << "}" << (k + 1 < block.instructions.size() ? "," : "") << "\n";
            }
            out << "        ]}" << (i + 1 < model.blocks.size() ? "," : "") << "\n";
        }
        out << "      ]\n";
        out << "    }";
    }

    struct Region {
        BankId bank;
        uint16_t address;
        size_t size;
        size_t offset;
    };

    std::string guess_region(const Region& region) const {
        const uint8_t* bytes = rom_.data() + region.offset;
        const size_t n = region.size;
        if (region.bank == 0 && region.address >= 0x0104 && region.address + n <= 0x0150) {
            return "header";
        }
        bool uniform = true;
        for (size_t i = 1; i < n; ++i) {
            if (bytes[i] != bytes[0]) {
                uniform = false;
                break;
            }
        }
        if (uniform) return "padding";
        if (n >= 8) {
            size_t printable = 0, letters = 0;
            for (size_t i = 0; i < n; ++i) {
                const uint8_t b = bytes[i];
                if (b >= 0x20 && b <= 0x7E) ++printable;
                if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) ++letters;
            }
            if (printable * 100 >= n * 85 && letters * 100 >= n * 40) return "ascii_text";
        }
        if (n >= 8 && n % 2 == 0) {
            size_t plausible = 0;
            std::set<uint16_t> distinct;
            for (size_t i = 0; i + 1 < n; i += 2) {
                const uint16_t word = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
                distinct.insert(word);
                const bool in_bank = region.bank == 0 ? (word >= 0x0150 && word < 0x8000)
                                                      : (word >= 0x4000 && word < 0x8000);
                if (in_bank) ++plausible;
            }
            if (plausible * 100 >= (n / 2) * 90 && distinct.size() >= 3) return "pointer_table";
        }
        return "unknown";
    }

    void emit_data_regions(std::ostringstream& out) const {
        // Where the analyzer's byte-statistics classifier found data.
        std::set<uint32_t> classified;
        for (const auto& [id, diagnostic] : analysis_.analysis_diagnostics) {
            if (diagnostic.kind == "data_region_classified") {
                classified.insert(AnalysisResult::make_addr(diagnostic.bank, diagnostic.address));
            }
        }
        // Which functions load a pointer into each ROM offset.
        std::map<size_t, std::set<uint32_t>> referenced;
        for (const FunctionModel& model : models_) {
            for (const RomReference& reference : model.rom_references) {
                if (!reference.bank_known || reference.is_code) continue;
                referenced[rom_offset(reference.bank, reference.address)].insert(model.full_address);
            }
        }

        std::vector<Region> regions;
        const size_t banks = (rom_.size() + 0x3FFF) / 0x4000;
        for (size_t bank = 0; bank < banks; ++bank) {
            const size_t base = bank * 0x4000;
            const size_t limit = std::min(rom_.size(), base + 0x4000);
            size_t i = base;
            while (i < limit) {
                if (covered_[i]) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < limit && !covered_[j]) {
                    // The cartridge header is its own region.
                    if (bank == 0 && (j == 0x0104 || j == 0x0150) && j != i) break;
                    ++j;
                }
                const uint16_t address =
                    static_cast<uint16_t>(bank == 0 ? i : 0x4000 + (i - base));
                regions.push_back({static_cast<BankId>(bank), address, j - i, i});
                i = j;
            }
        }

        out << "  \"data_regions\": [\n";
        for (size_t r = 0; r < regions.size(); ++r) {
            const Region& region = regions[r];
            bool was_classified = false;
            for (auto it = classified.lower_bound(AnalysisResult::make_addr(region.bank, region.address));
                 it != classified.end(); ++it) {
                if (static_cast<BankId>(*it >> 16) != region.bank) break;
                if ((*it & 0xFFFFu) >= region.address + region.size) break;
                was_classified = true;
                break;
            }
            std::set<uint32_t> users;
            size_t pointer_count = 0;
            for (auto it = referenced.lower_bound(region.offset);
                 it != referenced.end() && it->first < region.offset + region.size; ++it) {
                ++pointer_count;
                for (const uint32_t user : it->second) {
                    if (users.size() < 16) users.insert(user);
                }
            }
            out << "    {\"bank\": " << region.bank << ", \"address\": " << region.address
                << ", \"size\": " << region.size << ", \"rom_offset\": " << region.offset
                << ", \"guess\": \"" << guess_region(region) << "\", \"classified_by_analyzer\": "
                << (was_classified ? "true" : "false") << ", \"pointer_targets\": " << pointer_count
                << ", \"referenced_by\": " << id_list(users) << "}"
                << (r + 1 < regions.size() ? "," : "") << "\n";
        }
        out << "  ],\n";
    }

    void emit_memory_map(std::ostringstream& out) const {
        struct Entry {
            std::set<uint32_t> readers;
            std::set<uint32_t> writers;
            std::set<uint32_t> pointers;
        };
        std::map<uint16_t, Entry> map;
        for (const FunctionModel& model : models_) {
            for (const auto& [address, access] : model.memory) {
                Entry& entry = map[address];
                if (access.read) entry.readers.insert(model.full_address);
                if (access.write) entry.writers.insert(model.full_address);
                if (access.pointer) entry.pointers.insert(model.full_address);
            }
        }
        out << "  \"memory\": [\n";
        size_t index = 0;
        for (const auto& [address, entry] : map) {
            out << "    {\"address\": " << address << ", \"space\": \"" << memory_space(address)
                << "\"";
            const std::string name = memory_name(address);
            if (!name.empty()) out << ", \"name\": " << json_string(name);
            out << ", \"readers\": " << id_list(entry.readers) << ", \"writers\": "
                << id_list(entry.writers) << ", \"pointer_loads\": " << id_list(entry.pointers)
                << "}" << (++index < map.size() ? "," : "") << "\n";
        }
        out << "  ]\n";
    }

    std::string emit() const {
        const ROMHeader& header = rom_.header();
        std::ostringstream out;
        out << "{\n";
        out << "  \"schema\": \"gbrecomp.lift-export\",\n";
        out << "  \"version\": 1,\n";
        out << "  \"rom\": {\n";
        out << "    \"title\": " << json_string(header.title) << ",\n";
        out << "    \"sha256\": \"" << sha256_hex(rom_.data(), rom_.size()) << "\",\n";
        out << "    \"size\": " << rom_.size() << ",\n";
        out << "    \"mapper\": " << json_string(mbc_type_name(header.mbc_type)) << ",\n";
        out << "    \"cartridge_type\": " << static_cast<unsigned>(rom_.data()[0x147]) << ",\n";
        out << "    \"rom_banks\": " << rom_.bank_count() << ",\n";
        out << "    \"ram_size\": " << header.ram_size_bytes << ",\n";
        out << "    \"cgb\": " << (header.is_cgb ? "true" : "false") << ",\n";
        out << "    \"cgb_only\": " << (header.is_cgb_only ? "true" : "false") << "\n";
        out << "  },\n";
        out << "  \"analysis_profile\": " << json_string(options_.analysis_profile) << ",\n";

        size_t covered = 0;
        for (const uint8_t flag : covered_) covered += flag;
        out << "  \"stats\": {\"functions\": " << models_.size() << ", \"instructions\": "
            << analysis_.instructions.size() << ", \"code_bytes\": " << covered
            << ", \"other_bytes\": " << (covered_.size() - covered) << "},\n";

        out << "  \"functions\": [\n";
        for (size_t i = 0; i < models_.size(); ++i) {
            emit_function(out, models_[i]);
            out << (i + 1 < models_.size() ? ",\n" : "\n");
        }
        out << "  ],\n";
        emit_data_regions(out);
        emit_memory_map(out);
        out << "}\n";
        return out.str();
    }
};

}  // namespace

std::string build_lift_export_json(const ROM& rom,
                                   const AnalysisResult& analysis,
                                   const LiftExportOptions& options) {
    return Exporter(rom, analysis, options).build();
}

bool write_lift_export(const std::filesystem::path& path,
                       const ROM& rom,
                       const AnalysisResult& analysis,
                       const LiftExportOptions& options,
                       std::string* error) {
    const std::string json = build_lift_export_json(rom, analysis, options);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) *error = "cannot open " + path.string() + " for writing";
        return false;
    }
    file << json;
    file.close();
    if (!file) {
        if (error) *error = "failed to write " + path.string();
        return false;
    }
    return true;
}

} // namespace gbrecomp
