# GameCube runtime.
#
# The Gekko core calls gc_bus_* functions that the final program provides:
# the machine for a game, a flat memory for the tests. gccpu is the core
# alone; the machine, the renderer and the frontend arrive in later phases
# (docs/GC-PLAN.md).

add_library(gccpu STATIC
    src/gc_cpu.c
    src/gc_decode.c
)

# The machine around the core: memory, translation, the register file,
# interrupts and the loaders. The CPU library stays separate because the
# vector harness links it with a flat memory instead.
add_library(gcruntime STATIC
    src/gc.c
    src/gc_bus.c
    src/gc_io.c
    src/gc_efb.c
    src/gc_gx.c
    src/gc_gx_draw.c
    src/gc_gl.c
    src/gc_gl_render.c
    src/gc_memory.c
    src/gc_perf.c
    src/gc_gl_shader.c
    src/gc_gl_programs.c
    src/gc_gx_fog.c
    src/gc_gx_indirect.c
    src/gc_gx_blend.c
    src/gc_gx_raster.c
    src/gc_gx_tev.c
    src/gc_gx_texture.c
    src/gc_gx_workers.c
    src/gc_gx_thread.c
    src/gc_disc.c
    src/wii_disc.c
    src/wii_crypto.c
    src/gc_elf.c
    src/gc_apploader.c
    src/gc_pad.c
    src/gc_card.c
    src/gc_audio.c
    src/gc_ax.c
    src/gc_zelda.c
    src/gc_zelda_mix.c
    src/gc_os.c
    src/gc_coverage.c
    src/gc_differential.c
    src/gc_input_script.c
    src/gc_runner.c
    src/gc_state.c
    src/gc_recompiled_rt.c
    # The Wii's half of the machine (include/wii.h).
    src/wii.c
    src/wii_ios.c
    src/wii_ios_es.c
    src/wii_ios_di.c
    src/wii_ios_fs.c
    src/wii_nand.c
    src/wii_sysconf.c
    src/wii_ios_bt.c
    src/wii_bt.c
    src/wii_remote.c
    src/wii_extension_cipher.c
)
target_link_libraries(gcruntime PUBLIC gccpu)
# The rasteriser draws the rows of a large triangle on helper threads
# (src/gc_gx_workers.c).
find_package(Threads REQUIRED)
target_link_libraries(gcruntime PUBLIC Threads::Threads)

# The pixel path is a chain of small functions across files - the
# rasteriser calls the combiner, which calls the sampler, which calls the
# texel decoder, once a pixel or more - and MSVC inlines across files only
# with whole-program optimisation. Turning it on for these files alone made
# Shrek 2 about 9 % faster (docs/GC-PLAN.md, round three) and did not move a
# generated game's build time; the recompiled blocks are left out of it,
# because link-time code generation over them would. Floating point stays
# strict under /GL. Other compilers are left as they are.
if(MSVC)
    set_source_files_properties(
        src/gc_efb.c src/gc_gx.c src/gc_gx_draw.c src/gc_gx_fog.c src/gc_gx_indirect.c
        src/gc_gx_blend.c src/gc_gx_raster.c src/gc_gx_tev.c src/gc_gx_texture.c
        PROPERTIES COMPILE_OPTIONS /GL)
    target_link_options(gcruntime INTERFACE /LTCG)
endif()

# The test tools. A generated game copies this runtime into its project
# without them and turns them off.
option(GC_RUNTIME_TOOLS "Build gc_run and gc_play" ON)

# gc_run: the headless runner every test program goes through.
if(GC_RUNTIME_TOOLS)
    add_executable(gc_run tools/gc_run.c tools/gc_run_main.c)
    target_link_libraries(gc_run PRIVATE gcruntime)
    # gc_run --gl needs SDL2's library beside it; without it the program
    # cannot even start, which a script sees as a crash before main.
    if(TARGET SDL2::SDL2)
        get_target_property(_gc_run_sdl2_type SDL2::SDL2 TYPE)
        if(WIN32 AND _gc_run_sdl2_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET gc_run POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:gc_run>)
        endif()
    endif()
    # gc_gl_fuzz: the OpenGL path against the reference on random pipelines.
    add_executable(gc_gl_fuzz tools/gc_gl_fuzz.c)
    target_link_libraries(gc_gl_fuzz PRIVATE gcruntime)
endif()
target_include_directories(gccpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Floating point has to be exact and in the order it is written. The Gekko
# FPU model depends on every operation rounding exactly once and on the
# host's exception flags being readable straight afterwards; a compiler that
# is allowed to contract a*b+c into a fused multiply-add, or to move an
# operation across a flag read, changes results. MSVC's default /fp:precise
# does both, and it cost 46 vector cases before this was set. The flag is
# PUBLIC because anything that includes the instruction handlers - the
# tests, and every generated project - needs the same guarantee.
if(MSVC)
    target_compile_options(gccpu PUBLIC /fp:strict)
else()
    target_compile_options(gccpu PUBLIC -ffp-contract=off -frounding-math)
endif()

if(NOT WIN32)
    target_link_libraries(gccpu PUBLIC m)
endif()

# The SDL2 frontend - window, controller, sound and the settings menu - and
# gc_play, which plays a disc with the interpreter alone. A recompiled game
# links the same library and hands it a function that runs a field; that is
# the only thing the two ways of running a game differ in.
if(TARGET SDL2::SDL2)
    # Dear ImGui: a runtime snapshot carries its own; the source tree shares
    # the Game Boy runtime's.
    set(GC_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui")
    if(NOT EXISTS "${GC_IMGUI_DIR}/imgui.h")
        set(GC_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/vendor/imgui")
    endif()
    add_library(gcplatform STATIC
        platform/gc_sdl.cpp
        platform/gc_input.cpp
        platform/gc_audio_out.cpp
        ${GC_IMGUI_DIR}/imgui.cpp
        ${GC_IMGUI_DIR}/imgui_draw.cpp
        ${GC_IMGUI_DIR}/imgui_tables.cpp
        ${GC_IMGUI_DIR}/imgui_widgets.cpp
        ${GC_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${GC_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(gcplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform ${GC_IMGUI_DIR})
    target_link_libraries(gcplatform PUBLIC gcruntime SDL2::SDL2)
    target_compile_features(gcplatform PUBLIC cxx_std_17)

    if(GC_RUNTIME_TOOLS)
        add_executable(gc_play tools/gc_play.c tools/gc_run.c)
        target_link_libraries(gc_play PRIVATE gcplatform)
        get_target_property(_gc_sdl2_type SDL2::SDL2 TYPE)
        if(WIN32 AND _gc_sdl2_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET gc_play POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:gc_play>)
        endif()
    endif()
endif()

# The OpenGL path needs a context, and SDL2 is what makes one. Without it
# gc_gl.c compiles to stubs that report the path unavailable and the
# runtime falls back to the reference rasteriser, which is the correct
# outcome rather than a degraded one - the reference is what defines
# correct in the first place.
if(SDL2_FOUND)
    target_compile_definitions(gcruntime PUBLIC GC_HAVE_SDL2)
    target_include_directories(gcruntime PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gcruntime PUBLIC ${SDL2_LIBRARIES})
endif()
