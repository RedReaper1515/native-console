# nesrecomp: NES static recompiler (ROM -> C project).
#
# Shares instruction classification (nes_cpu_decode.h), the cartridge header
# parser and SHA-256 with the runtime so the analyzer, the emitter and the
# interpreter agree, and links the runtime itself for --profile (a short
# interpreted run that finds code reached only at run time, and which bank
# was mapped when it was).

add_library(nesrecomp_core STATIC
    src/rom.cpp
    src/analyzer.cpp
    src/emitter.cpp
)
target_include_directories(nesrecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nesrecomp_core PUBLIC nesruntime)

add_executable(nesrecomp src/main.cpp)
target_link_libraries(nesrecomp PRIVATE nesrecomp_core)
target_compile_definitions(nesrecomp PRIVATE
    NESRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtimes/runtime-nes"
    NESRECOMP_VERSION="${PROJECT_VERSION}"
)

# rom.h pulls in the instruction handlers (nes_recomp.h), which the
# recompiler never calls: MSVC would warn once per opcode in every file.
if(MSVC)
    target_compile_options(nesrecomp_core PRIVATE /wd4505)
    target_compile_options(nesrecomp PRIVATE /wd4505)
endif()
