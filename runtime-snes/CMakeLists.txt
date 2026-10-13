# SNES runtime.
#
# The 65C816 core calls snes_bus_* functions that the final program
# provides. snescpu is the core alone (the SingleStepTests harness links it
# with a replaying bus); snesruntime is the core plus the machine, which
# provides the real bus. Both are built from one object library so the core
# is compiled once and there is no circular static-library dependency.
#
# The APU (SPC700, S-DSP) and the PPU are separate libraries with their own
# unit tests; they do not depend on the machine.

add_library(snescpu_objects OBJECT
    src/snes_cpu.c
)
target_include_directories(snescpu_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(snescpu STATIC $<TARGET_OBJECTS:snescpu_objects>)
target_include_directories(snescpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB SNES_APU_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_apu*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_spc700*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_dsp*.c
)
add_library(snesapu STATIC ${SNES_APU_SOURCES})
target_include_directories(snesapu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT WIN32)
    target_link_libraries(snesapu PUBLIC m)
endif()

file(GLOB SNES_PPU_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/snes_ppu*.c)
add_library(snesppu STATIC ${SNES_PPU_SOURCES})
target_include_directories(snesppu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(snesruntime STATIC
    $<TARGET_OBJECTS:snescpu_objects>
    src/snes.c
    src/snes_bus.c
    src/snes_cart.c
    src/snes_dma.c
    src/snes_io.c
    src/snes_runner.c
    src/snes_recomp.c
    src/snes_sa1.c
    src/snes_coverage.c
    src/snes_crt.c
    src/snes_sha256.c
    src/snes_state.c
)
target_include_directories(snesruntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(snesruntime PUBLIC snesapu snesppu)
if(NOT WIN32)
    target_link_libraries(snesruntime PUBLIC m)
endif()

add_executable(snes_run tools/snes_run.c)
target_link_libraries(snes_run PRIVATE snesruntime)

# SDL2 frontend (window, input, audio, the settings menu with Dear ImGui)
# and snes_play, which plays a ROM with the interpreter alone.
if(TARGET SDL2::SDL2)
    set(SNES_IMGUI_DIR ${PROJECT_SOURCE_DIR}/runtime/vendor/imgui)
    add_library(snesplatform STATIC
        platform/snes_sdl.cpp
        platform/snes_input.cpp
        ${SNES_IMGUI_DIR}/imgui.cpp
        ${SNES_IMGUI_DIR}/imgui_draw.cpp
        ${SNES_IMGUI_DIR}/imgui_tables.cpp
        ${SNES_IMGUI_DIR}/imgui_widgets.cpp
        ${SNES_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${SNES_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(snesplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform ${SNES_IMGUI_DIR})
    target_link_libraries(snesplatform PUBLIC snesruntime SDL2::SDL2)
    target_compile_features(snesplatform PUBLIC cxx_std_17)

    add_executable(snes_play tools/snes_play.c)
    target_link_libraries(snes_play PRIVATE snesplatform)
    get_target_property(_snes_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _snes_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET snes_play POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:snes_play>)
    endif()
endif()
