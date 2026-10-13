# GameBoy Recompiled - Static Recompiler for Game Boy ROMs
# Top-level CMake configuration

cmake_minimum_required(VERSION 3.20)

# Release unless told otherwise. Before project(): with MSVC and a
# single-configuration generator, project() itself makes it Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(gameboy-recompiled 
    VERSION 0.1.1
    DESCRIPTION "Static recompiler for Game Boy (DMG/CGB) ROMs"
    LANGUAGES C CXX
)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C Standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O3)
    endif()
elseif(MSVC)
    add_compile_options(/W4)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find SDL2 (for runtime)
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    message(STATUS "Found SDL2: ${SDL2_INCLUDE_DIRS}")
else()
    message(STATUS "SDL2 not found - runtime will not be built")
endif()

# Subdirectories
add_subdirectory(recompiler)

# Game Boy Advance runtime (the CPU core needs no SDL) and recompiler.
add_subdirectory(runtime-gba)
add_subdirectory(recompiler-gba)

# Super Nintendo runtime (the CPU cores need no SDL) and recompiler.
add_subdirectory(runtime-snes)
add_subdirectory(recompiler-snes)

# NES runtime (the CPU core needs no SDL) and recompiler.
add_subdirectory(runtime-nes)
add_subdirectory(recompiler-nes)

# GameCube runtime. The recompiler and the rest of the machine arrive in
# later phases; see docs/GC-PLAN.md.
add_subdirectory(runtime-gc)
add_subdirectory(recompiler-gc)

# Nintendo DS runtime. The CPU cores so far; see docs/NDS-PLAN.md.
add_subdirectory(runtime-nds)
add_subdirectory(recompiler-nds)

# Nintendo 64 runtime and recompiler; see docs/N64-PLAN.md.
add_subdirectory(runtime-n64)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/recompiler-n64/CMakeLists.txt)
    add_subdirectory(recompiler-n64)
endif()

if(SDL2_FOUND)
    add_subdirectory(runtime)
endif()

# Optional: Native Console Hub (library, import/build pipeline, launcher)
option(BUILD_HUB "Build the Native Console Hub desktop application" ON)
if(BUILD_HUB AND SDL2_FOUND)
    add_subdirectory(hub)
endif()

# Optional: Build tests
option(BUILD_TESTS "Build test suite" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tests/gba)
    add_subdirectory(tests/snes)
    add_subdirectory(tests/nes)
    add_subdirectory(tests/gc)
    add_subdirectory(tests/wii)
    add_subdirectory(tests/nds)
    add_subdirectory(tests/n64)
endif()

# Install rules
install(TARGETS gbrecomp
    RUNTIME DESTINATION bin
)

# Summary
message(STATUS "")
message(STATUS "GameBoy Recompiled Configuration:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  SDL2:         ${SDL2_FOUND}")
message(STATUS "  Tests:        ${BUILD_TESTS}")
message(STATUS "  Hub:          ${BUILD_HUB}")
message(STATUS "")
