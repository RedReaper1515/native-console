# n64recomp: Nintendo 64 static recompiler (ROM -> C project).
#
# Shares the decode classifier (n64_cpu_decode.h), the cartridge parser and
# SHA-256 with the runtime, so the analyzer, the emitter and the interpreter
# agree, and links the runtime itself for --profile (an interpreted run that
# records the code games load into RDRAM at run time).

add_library(n64recomp_core STATIC
    src/image.cpp
    src/analyzer.cpp
    src/emitter.cpp
)
target_include_directories(n64recomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(n64recomp_core PUBLIC n64runtime)

add_executable(n64recomp src/main.cpp)
target_link_libraries(n64recomp PRIVATE n64recomp_core)
target_compile_definitions(n64recomp PRIVATE
    N64RECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtime-n64"
    N64RECOMP_VERSION="${PROJECT_VERSION}"
)
