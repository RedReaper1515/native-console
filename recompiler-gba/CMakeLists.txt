# gbarecomp: Game Boy Advance static recompiler (ROM -> C project).
#
# Shares instruction classification (gba_cpu_decode.h) and SHA-256 with the
# runtime so the analyzer, the emitter and the interpreter agree, and links
# the runtime itself for --profile (a short interpreted run that finds code
# reached only at run time).

add_library(gbarecomp_core STATIC
    src/rom.cpp
    src/analyzer.cpp
    src/emitter.cpp
)
target_include_directories(gbarecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gbarecomp_core PUBLIC gbaruntime)

add_executable(gbarecomp src/main.cpp)
target_link_libraries(gbarecomp PRIVATE gbarecomp_core)
target_compile_definitions(gbarecomp PRIVATE
    GBARECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime-gba"
    GBARECOMP_VERSION="${PROJECT_VERSION}"
)
