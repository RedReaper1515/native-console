/**
 * @file ir_optimizer.h
 * @brief Optional optimization passes for IR
 */

#ifndef RECOMPILER_IR_OPTIMIZER_H
#define RECOMPILER_IR_OPTIMIZER_H

#include "ir.h"

namespace gbrecomp {
namespace ir {

/**
 * @brief Optimization level
 */
enum class OptLevel {
    O0,  // No optimization
    O1,  // Basic optimizations (constant propagation, dead code)
    O2,  // More aggressive (may affect debugging)
};

/**
 * @brief Optimization pass interface
 */
class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;
    
    virtual const char* name() const = 0;
    virtual bool run(Program& program) = 0;
};

/**
 * @brief Constant propagation pass
 * 
 * Propagates known constant values through registers.
 * Example: LD A, 5; LD B, A  →  LD A, 5; LD B, 5
 */
class ConstantPropagation : public OptimizationPass {
public:
    const char* name() const override { return "ConstantPropagation"; }
    bool run(Program& program) override;
};

/**
 * @brief Dead code elimination pass
 * 
 * Removes instructions whose results are never used.
 */
class DeadCodeElimination : public OptimizationPass {
public:
    const char* name() const override { return "DeadCodeElimination"; }
    bool run(Program& program) override;
};

/**
 * @brief Unreachable block elimination
 * 
 * Removes blocks that cannot be reached from entry points.
 */
class UnreachableBlockElimination : public OptimizationPass {
public:
    const char* name() const override { return "UnreachableBlockElimination"; }
    bool run(Program& program) override;
};

/**
 * @brief Flag computation elimination
 * 
 * Removes flag computations that are immediately overwritten.
 * Must be used carefully - some games may read flags unexpectedly.
 */
class FlagElimination : public OptimizationPass {
public:
    const char* name() const override { return "FlagElimination"; }
    bool run(Program& program) override;
};

/**
 * @brief Run optimization passes at the specified level
 * 
 * @param program IR program to optimize
 * @param level Optimization level
 * @return Number of passes that made changes
 */
int optimize(Program& program, OptLevel level);

/**
 * @brief Run a single optimization pass
 * 
 * @param program IR program
 * @param pass Pass to run
 * @return true if pass made changes
 */
bool run_pass(Program& program, OptimizationPass& pass);

} // namespace ir
} // namespace gbrecomp

#endif // RECOMPILER_IR_OPTIMIZER_H
