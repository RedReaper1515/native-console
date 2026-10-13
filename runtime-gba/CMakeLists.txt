# Game Boy Advance runtime.
#
# The ARM7TDMI core calls gba_bus_* functions that the final program
# provides. gbacpu is the core alone (the SingleStepTests harness links it
# with a replaying bus); gbaruntime is the core plus the machine (memory
# map, I/O, timers, DMA, video timing), which provides the real bus. Both
# are built from one object library so the core is compiled once and there
# is no circular static-library dependency.

add_library(gbacpu_objects OBJECT
    src/gba_cpu.c
    src/gba_cpu_arm.c
    src/gba_cpu_thumb.c
    src/gba_mul_carry.c
)
target_include_directories(gbacpu_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(gbacpu STATIC $<TARGET_OBJECTS:gbacpu_objects>)
target_include_directories(gbacpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(gbaruntime STATIC
    $<TARGET_OBJECTS:gbacpu_objects>
    src/gba.c
    src/gba_apu.c
    src/gba_bios.c
    src/gba_bus.c
    src/gba_coverage.c
    src/gba_io.c
    src/gba_link.c
    src/gba_net.c
    src/gba_timer.c
    src/gba_dma.c
    src/gba_eeprom.c
    src/gba_rtc.c
    src/gba_ppu.c
    src/gba_recomp.c
    src/gba_runner.c
    src/gba_sha256.c
    src/gba_state.c
    src/gba_texture_pack.c
    src/gba_crt.c
    src/gba_upscale.c
)
target_include_directories(gbaruntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The APU uses libm (sin, pow, lrint), which is separate from libc outside Windows.
if(NOT WIN32)
    target_link_libraries(gbaruntime PUBLIC m)
endif()
# The link cable's network transport (gba_net.c).
if(WIN32)
    target_link_libraries(gbaruntime PUBLIC ws2_32)
endif()

add_executable(gba_run tools/gba_run.c)
target_link_libraries(gba_run PRIVATE gbaruntime)

# SDL2 frontend (window, input, audio, settings menu) and gba_play, which
# plays a ROM with the interpreter alone. The menu uses the Dear ImGui copy
# the Game Boy runtime vendors; gbarecomp copies it into generated projects
# as runtime-gba/vendor/imgui.
if(TARGET SDL2::SDL2)
    set(GBA_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/vendor/imgui")
    add_library(gbaplatform STATIC
        platform/gba_sdl.cpp
        platform/gba_input.cpp
        ${GBA_IMGUI_DIR}/imgui.cpp
        ${GBA_IMGUI_DIR}/imgui_draw.cpp
        ${GBA_IMGUI_DIR}/imgui_tables.cpp
        ${GBA_IMGUI_DIR}/imgui_widgets.cpp
        ${GBA_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${GBA_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(gbaplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform)
    target_include_directories(gbaplatform PRIVATE ${GBA_IMGUI_DIR})
    target_compile_features(gbaplatform PRIVATE cxx_std_17)
    target_link_libraries(gbaplatform PUBLIC gbaruntime SDL2::SDL2)

    add_executable(gba_play tools/gba_play.c)
    target_link_libraries(gba_play PRIVATE gbaplatform)
    get_target_property(_gba_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _gba_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET gba_play POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:gba_play>)
    endif()
endif()
