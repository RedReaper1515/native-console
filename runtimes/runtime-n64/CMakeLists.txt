# Nintendo 64 runtime.
#
# The parts are libraries that meet at link-time seams (docs/N64-PLAN.md):
#
#   n64cpu  the VR4300 core; it calls n64_bus_* functions the final program
#           provides (the machine, or the vector harness's flat memory)
#   n64rsp  the RSP: scalar and vector units, DMEM/IMEM, SP DMA
#   n64rdp  the RDP: command interface and the reference rasteriser
#   n64runtime  the machine around them: the bus, MI, VI, AI, PI, SI, the
#           PIF and the cartridge, the run loop, the recompiled-code
#           dispatcher and the headless runner
#
# The floating-point unit depends on each host operation rounding once and
# on reading the host's exception flags straight afterwards, so nothing may
# contract a*b+c into a fused multiply-add or move operations across a flag
# read (the GameCube lesson, docs/GC-PLAN.md "The vector pipeline").

if(MSVC)
    set(N64_STRICT_FP /fp:strict CACHE INTERNAL "strict floating point for the N64 FPU")
else()
    set(N64_STRICT_FP -ffp-contract=off -frounding-math CACHE INTERNAL "strict floating point for the N64 FPU")
endif()

file(GLOB N64_CPU_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/n64_cpu*.c)
if(N64_CPU_SOURCES)
add_library(n64cpu_objects OBJECT ${N64_CPU_SOURCES})
target_include_directories(n64cpu_objects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(n64cpu_objects PRIVATE ${N64_STRICT_FP})

add_library(n64cpu STATIC $<TARGET_OBJECTS:n64cpu_objects>)
target_include_directories(n64cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT WIN32)
    target_link_libraries(n64cpu PUBLIC m)
endif()
endif()

file(GLOB N64_RSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/n64_rsp*.c)
if(N64_RSP_SOURCES)
    add_library(n64rsp STATIC ${N64_RSP_SOURCES})
    target_include_directories(n64rsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()

file(GLOB N64_RDP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/n64_rdp*.c)
if(N64_RDP_SOURCES)
    add_library(n64rdp STATIC ${N64_RDP_SOURCES})
    target_include_directories(n64rdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # The rasteriser shares a primitive's scanlines with helper threads
    # (n64_rdp_workers.c): Win32 threads, pthreads elsewhere.
    find_package(Threads REQUIRED)
    target_link_libraries(n64rdp PUBLIC Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(n64rdp PUBLIC m)
    endif()
endif()

# The machine: every other source in src/.
file(GLOB N64_ALL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
set(N64_MACHINE_SOURCES ${N64_ALL_SOURCES})
list(REMOVE_ITEM N64_MACHINE_SOURCES ${N64_CPU_SOURCES} ${N64_RSP_SOURCES} ${N64_RDP_SOURCES})
if(N64_MACHINE_SOURCES AND TARGET n64cpu AND TARGET n64rsp AND TARGET n64rdp)
    add_library(n64runtime STATIC $<TARGET_OBJECTS:n64cpu_objects> ${N64_MACHINE_SOURCES})
    target_include_directories(n64runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(n64runtime PRIVATE ${N64_STRICT_FP})
    target_link_libraries(n64runtime PUBLIC n64rsp n64rdp)
    if(NOT WIN32)
        target_link_libraries(n64runtime PUBLIC m)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/n64_run.c)
        add_executable(n64_run tools/n64_run.c)
        target_link_libraries(n64_run PRIVATE n64runtime)
    endif()
endif()

# SDL2 frontend (window, input, audio, the settings menu with Dear ImGui)
# and n64_play, which plays a ROM with the interpreter alone.
if(TARGET SDL2::SDL2 AND TARGET n64runtime)
    set(N64_IMGUI_DIR ${PROJECT_SOURCE_DIR}/runtimes/runtime-gb/vendor/imgui)
    add_library(n64platform STATIC
        platform/n64_sdl.cpp
        platform/n64_input.cpp
        ${N64_IMGUI_DIR}/imgui.cpp
        ${N64_IMGUI_DIR}/imgui_draw.cpp
        ${N64_IMGUI_DIR}/imgui_tables.cpp
        ${N64_IMGUI_DIR}/imgui_widgets.cpp
        ${N64_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${N64_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(n64platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform ${N64_IMGUI_DIR})
    target_link_libraries(n64platform PUBLIC n64runtime SDL2::SDL2)
    target_compile_features(n64platform PUBLIC cxx_std_17)

    add_executable(n64_play tools/n64_play.c)
    target_link_libraries(n64_play PRIVATE n64platform)
    get_target_property(_n64_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _n64_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET n64_play POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:n64_play>)
    endif()
endif()
