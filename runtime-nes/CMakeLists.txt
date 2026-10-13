# NES runtime.
#
# The 6502 core calls nes_bus_* functions that the final program provides.
# nescpu is the core alone (the SingleStepTests harness links it with a
# replaying bus); nesruntime is the core plus the machine, which provides
# the real bus. Both are built from one object library so the core is
# compiled once and there is no circular static-library dependency.
#
# The PPU and the APU are separate libraries with their own unit tests; they
# do not depend on the machine. The PPU reaches the cartridge through
# nes_ppu_bus_* functions the final program provides, and the APU's DMC
# reads its samples through nes_apu_dmc_read, the same way.

add_library(nescpu_objects OBJECT
    src/nes_cpu.c
)
target_include_directories(nescpu_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(nescpu STATIC $<TARGET_OBJECTS:nescpu_objects>)
target_include_directories(nescpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB NES_APU_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/nes_apu*.c)
add_library(nesapu STATIC ${NES_APU_SOURCES})
target_include_directories(nesapu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT WIN32)
    target_link_libraries(nesapu PUBLIC m)
endif()

file(GLOB NES_PPU_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/nes_ppu*.c)
add_library(nesppu STATIC ${NES_PPU_SOURCES})
target_include_directories(nesppu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(nesruntime STATIC
    $<TARGET_OBJECTS:nescpu_objects>
    src/nes.c
    src/nes_bus.c
    src/nes_cart.c
    src/nes_runner.c
    src/nes_recomp.c
    src/nes_coverage.c
    src/nes_crt.c
    src/nes_sha256.c
    src/nes_state.c
)
target_include_directories(nesruntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nesruntime PUBLIC nesapu nesppu)
if(NOT WIN32)
    target_link_libraries(nesruntime PUBLIC m)
endif()

add_executable(nes_run tools/nes_run.c)
target_link_libraries(nes_run PRIVATE nesruntime)

# SDL2 frontend (window, input, audio, the settings menu with Dear ImGui)
# and nes_play, which plays a ROM with the interpreter alone.
if(TARGET SDL2::SDL2)
    set(NES_IMGUI_DIR ${PROJECT_SOURCE_DIR}/runtime/vendor/imgui)
    add_library(nesplatform STATIC
        platform/nes_sdl.cpp
        platform/nes_input.cpp
        ${NES_IMGUI_DIR}/imgui.cpp
        ${NES_IMGUI_DIR}/imgui_draw.cpp
        ${NES_IMGUI_DIR}/imgui_tables.cpp
        ${NES_IMGUI_DIR}/imgui_widgets.cpp
        ${NES_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${NES_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(nesplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform ${NES_IMGUI_DIR})
    target_link_libraries(nesplatform PUBLIC nesruntime SDL2::SDL2)
    target_compile_features(nesplatform PUBLIC cxx_std_17)

    add_executable(nes_play tools/nes_play.c)
    target_link_libraries(nes_play PRIVATE nesplatform)
    get_target_property(_nes_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _nes_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET nes_play POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:nes_play>)
    endif()
endif()
