# gcrecomp: a GameCube executable into a C project that is that program.
#
# It links the runtime, and not for convenience: the analyzer and the
# emitter both read gc_cpu_decode.h and gc_cpu_ops.h, which are the same
# decode classifier and the same opcode list the interpreter dispatches
# from. Three readers, one table - which is how an emitter and an
# interpreter are kept from drifting apart.

add_library(gcrecomp_core STATIC
    src/image.cpp
    src/analyzer.cpp
    src/emitter.cpp
    src/block.cpp
)
target_include_directories(gcrecomp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(gcrecomp_core PUBLIC gcruntime)

add_executable(gcrecomp src/main.cpp)
target_link_libraries(gcrecomp PRIVATE gcrecomp_core)
target_compile_definitions(gcrecomp PRIVATE
    GCRECOMP_DEFAULT_RUNTIME_DIR="${PROJECT_SOURCE_DIR}/runtimes/runtime-gc"
    GCRECOMP_VERSION="${PROJECT_VERSION}"
)
