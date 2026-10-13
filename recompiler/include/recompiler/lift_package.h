/**
 * @file lift_package.h
 * @brief Lifted source to build into a generated game (LIFTING.md)
 *
 * A lift package is a manifest naming C sources written against gblift.h and
 * the ROM subroutine each of their functions stands for. Generated with
 * `--lift`, a game calls the C where the recompiled code would have run that
 * subroutine. tools/gb_lift.py writes the manifest.
 */

#ifndef RECOMPILER_LIFT_PACKAGE_H
#define RECOMPILER_LIFT_PACKAGE_H

#include "ir/ir.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gbrecomp {

struct LiftSource {
    std::string relative_path;
    std::string content;
    bool is_translation_unit = true;
};

struct LiftBinding {
    BankId bank = 0;
    uint16_t address = 0;
    std::string function_id;  // gbfn:v1:BBBB:AAAA
    std::string symbol;       // the C function
};

struct LiftPackage {
    bool enabled = false;
    std::string rom_sha256;
    size_t rom_size = 0;
    std::vector<LiftSource> sources;
    std::vector<LiftBinding> bindings;
};

/**
 * @brief Read a `gbrecomp.lift-package` manifest and the sources it names
 *
 * Fails, with a message in @p error, on a malformed manifest, a source
 * outside the manifest's directory, or a ROM other than the one the package
 * was made for.
 */
bool load_lift_package(const std::filesystem::path& manifest_path,
                       const uint8_t* rom_data,
                       size_t rom_size,
                       LiftPackage& package,
                       std::string& error);

/**
 * @brief Check that every bound subroutine starts a block of recompiled code
 *
 * That is where the call to the lifted function is emitted, so a binding
 * anywhere else would silently never run.
 */
bool validate_lift_bindings(const LiftPackage& package,
                            const ir::Program& program,
                            std::string& error);

} // namespace gbrecomp

#endif // RECOMPILER_LIFT_PACKAGE_H
