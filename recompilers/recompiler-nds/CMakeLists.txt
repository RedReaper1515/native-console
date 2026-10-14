# ndsrecomp: Nintendo DS static recompiler (ROM -> C project).
#
# The ROM front end (header and file system, BLZ decompression, the code
# modules, ds-decomp symbols and relocations), then the analyzer and the
# emitter. The last two read the runtime's instruction classifier
# (nds_cpu_decode.h) so that they and the interpreter agree, and compile the
# replacement BIOS images the runtime boots with like any other code. The
# runtime itself is linked for those images and for --profile (a short
# interpreted run that finds code the analysis cannot reach).

add_library(ndsrecomp_core STATIC
    src/blz.cpp
    src/rom.cpp
    src/modules.cpp
    src/symbols.cpp
    src/check.cpp
    src/analyzer.cpp
    src/analysis.cpp
    src/emitter.cpp
    src/recompile.cpp
)
target_include_directories(ndsrecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ndsrecomp_core PUBLIC ndsruntime)

add_executable(ndsrecomp src/main.cpp)
target_link_libraries(ndsrecomp PRIVATE ndsrecomp_core)
target_compile_definitions(ndsrecomp PRIVATE
    NDSRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtimes/runtime-nds"
    NDSRECOMP_VERSION="${PROJECT_VERSION}"
)
