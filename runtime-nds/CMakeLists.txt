# Nintendo DS runtime.
#
# The two CPU cores (nds9_* for the ARM9, nds7_* for the ARM7) are one set
# of handlers compiled twice, and call nds9_bus_* / nds7_bus_* functions
# that the final program provides. ndscpu is the cores alone (the vector
# harness links it with a replaying bus); ndsruntime is the cores plus the
# machine, which provides the real buses. Both are built from one object
# library so the cores are compiled once. See docs/NDS-PLAN.md.

add_library(ndscpu_objects OBJECT
    src/nds_cpu.c
    src/nds_cpu9.c
    src/nds_cpu7.c
    src/nds_cp15.c
    src/nds_mul_carry.c
)
target_include_directories(ndscpu_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(ndscpu STATIC $<TARGET_OBJECTS:ndscpu_objects>)
target_include_directories(ndscpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(ndsruntime STATIC
    $<TARGET_OBJECTS:ndscpu_objects>
    src/nds.c
    src/nds_2d.c
    src/nds_bios_data.c
    src/nds_boot.c
    src/nds_bus.c
    src/nds_card.c
    src/nds_dma.c
    src/nds_firmware.c
    src/nds_gx.c
    src/nds_gx_raster.c
    src/nds_gx_workers.c
    src/nds_io.c
    src/nds_ipc.c
    src/nds_math.c
    src/nds_prof.c
    src/nds_recomp.c
    src/nds_rtc.c
    src/nds_runner.c
    src/nds_save.c
    src/nds_save_db.c
    src/nds_sound.c
    src/nds_spi.c
    src/nds_spu.c
    src/nds_timer.c
    src/nds_video.c
)
target_include_directories(ndsruntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The larger 3D picture's lines are shared between threads
# (nds_gx_workers.c): Win32 threads, pthreads elsewhere.
find_package(Threads REQUIRED)
target_link_libraries(ndsruntime PUBLIC Threads::Threads)

# Time each part of the machine and report it with the runner's --profile.
# Off by default: the timing calls cost about as much as an instruction.
option(NDS_PROFILE "Build the DS runtime's section profiler" OFF)
if(NDS_PROFILE)
    target_compile_definitions(ndsruntime PRIVATE NDS_PROFILE)
endif()

add_executable(nds_run tools/nds_run.c)
target_link_libraries(nds_run PRIVATE ndsruntime)

# SDL2 frontend (window, the two screens and their layouts, the stylus,
# audio, the settings menu) and nds_play, which plays a ROM with the
# interpreter alone. The menu uses the Dear ImGui copy the Game Boy runtime
# vendors; ndsrecomp copies it into generated projects as
# runtime-nds/vendor/imgui.
if(TARGET SDL2::SDL2)
    set(NDS_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/vendor/imgui")
    add_library(ndsplatform STATIC
        platform/nds_sdl.cpp
        platform/nds_input.cpp
        ${NDS_IMGUI_DIR}/imgui.cpp
        ${NDS_IMGUI_DIR}/imgui_draw.cpp
        ${NDS_IMGUI_DIR}/imgui_tables.cpp
        ${NDS_IMGUI_DIR}/imgui_widgets.cpp
        ${NDS_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${NDS_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(ndsplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform ${NDS_IMGUI_DIR})
    target_compile_features(ndsplatform PUBLIC cxx_std_17)
    target_link_libraries(ndsplatform PUBLIC ndsruntime SDL2::SDL2)

    add_executable(nds_play tools/nds_play.c)
    target_link_libraries(nds_play PRIVATE ndsplatform)
    get_target_property(_nds_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _nds_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET nds_play POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:nds_play>)
    endif()
endif()
