/**
 * @file ir_builder.h
 * @brief Build IR from decoded SM83 instructions
 */

#ifndef RECOMPILER_IR_BUILDER_H
#define RECOMPILER_IR_BUILDER_H

#include "ir.h"
#include "../decoder.h"
#include "../analyzer.h"
#include <vector>

namespace gbrecomp {
namespace ir {

/**
 * @brief Options for IR building
 */
struct BuilderOptions {
    bool emit_source_locations = true;   // Include source address info
    bool emit_comments = true;           // Include disassembly comments
    bool preserve_flags_exactly = true;  // Emit exact flag computations
};

/**
 * @brief Builds IR from analyzed instructions
 */
class IRBuilder {
public:
    explicit IRBuilder(const BuilderOptions& options = {});
    
    /**
     * @brief Build IR program from analysis result
     * 
     * @param analysis Analyzed ROM
     * @param rom_name Name for the program
     * @return IR program
     */
    Program build(const AnalysisResult& analysis, const std::string& rom_name);
    
    /**
     * @brief Lower a single instruction to IR
     * 
     * @param instr Decoded instruction
     * @param block Block to append to
     */
    void lower_instruction(const Instruction& instr, BasicBlock& block);

private:
    BuilderOptions options_;
    
    // Lowering helpers for instruction categories
    void lower_load_r_r(const Instruction& instr, BasicBlock& block);
    void lower_load_r_imm(const Instruction& instr, BasicBlock& block);
    void lower_load_mem(const Instruction& instr, BasicBlock& block);
    void lower_store_mem(const Instruction& instr, BasicBlock& block);
    void lower_alu_r(const Instruction& instr, BasicBlock& block);
    void lower_alu_imm(const Instruction& instr, BasicBlock& block);
    void lower_inc_dec(const Instruction& instr, BasicBlock& block);
    void lower_rotate_shift(const Instruction& instr, BasicBlock& block);
    void lower_bit_op(const Instruction& instr, BasicBlock& block);
    void lower_jump(const Instruction& instr, BasicBlock& block, Program& prog);
    void lower_call(const Instruction& instr, BasicBlock& block, Program& prog);
    void lower_ret(const Instruction& instr, BasicBlock& block);
    void lower_misc(const Instruction& instr, BasicBlock& block);
    void lower_io(const Instruction& instr, BasicBlock& block);
    void lower_16bit_load(const Instruction& instr, BasicBlock& block);
    void lower_16bit_alu(const Instruction& instr, BasicBlock& block);
    
    // Helper to add instruction with source location
    void emit(BasicBlock& block, IRInstruction instr, const Instruction& src);
};

} // namespace ir
} // namespace gbrecomp

#endif // RECOMPILER_IR_BUILDER_H
