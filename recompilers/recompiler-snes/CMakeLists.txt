# snesrecomp: Super Nintendo static recompiler (ROM -> C project).
#
# Shares instruction classification (snes_cpu_decode.h), the cartridge
# memory map and SHA-256 with the runtime so the analyzer, the emitter and
# the interpreter agree, and links the runtime itself for --profile (a
# short interpreted run that finds code reached only at run time).

add_library(snesrecomp_core STATIC
    src/rom.cpp
    src/analyzer.cpp
    src/emitter.cpp
)
target_include_directories(snesrecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(snesrecomp_core PUBLIC snesruntime)

add_executable(snesrecomp src/main.cpp)
target_link_libraries(snesrecomp PRIVATE snesrecomp_core)
target_compile_definitions(snesrecomp PRIVATE
    SNESRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtimes/runtime-snes"
    SNESRECOMP_VERSION="${PROJECT_VERSION}"
)
