# GameCube tests.
#
# External test data comes from tools/fetch_gc_tests.py. Point GC_TEST_DATA_DIR
# elsewhere to share one download between checkouts. Tests that need fetched
# data are registered only when it is present.

set(GC_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/gc" CACHE PATH
    "Directory populated by tools/fetch_gc_tests.py")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The decode classifier against the table it was generated from: every
# dispatch position, every opcode's encoding, and the field accessors.
add_executable(gc_decode_test gc_decode_test.c)
target_link_libraries(gc_decode_test PRIVATE gccpu)
add_test(NAME gc_decode COMMAND gc_decode_test)

# The interpreter on a flat big-endian memory.
add_executable(gc_cpu_test gc_cpu_test.c gc_test_bus.c)
target_link_libraries(gc_cpu_test PRIVATE gccpu)
add_test(NAME gc_cpu_units COMMAND gc_cpu_test)

# The floating-point fast path held to the full one (gc_fpu_impl.h).
add_executable(gc_fpu_fast_test gc_fpu_fast_test.c gc_test_bus.c)
target_link_libraries(gc_fpu_fast_test PRIVATE gccpu)
add_test(NAME gc_fpu_fast COMMAND gc_fpu_fast_test)

# Recompiled blocks held to the interpreter on random machine states: the
# generator writes random blocks through gcrecomp's block emitter, and the
# driver runs each against the interpreter (gc_recompiled_fuzz.c).
if(TARGET gcrecomp_core)
    add_executable(gc_recompiled_fuzz_gen gc_recompiled_fuzz_gen.cpp)
    target_link_libraries(gc_recompiled_fuzz_gen PRIVATE gcrecomp_core)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gc_recompiled_fuzz_blocks.c"
        COMMAND gc_recompiled_fuzz_gen "${CMAKE_CURRENT_BINARY_DIR}/gc_recompiled_fuzz_blocks.c" 2000
        DEPENDS gc_recompiled_fuzz_gen
        COMMENT "Writing random recompiled blocks for gc_recompiled_fuzz")
    add_executable(gc_recompiled_fuzz gc_recompiled_fuzz.c "${CMAKE_CURRENT_BINARY_DIR}/gc_recompiled_fuzz_blocks.c")
    target_link_libraries(gc_recompiled_fuzz PRIVATE gcruntime)
    if(MSVC)
        target_compile_options(gc_recompiled_fuzz PRIVATE /bigobj)
    endif()
    add_test(NAME gc_recompiled_fuzz COMMAND gc_recompiled_fuzz)
endif()
# The bus's shortcut to main memory held to its full translation (gc_bus.c).
add_executable(gc_bus_fast_test gc_bus_fast_test.c)
target_link_libraries(gc_bus_fast_test PRIVATE gcruntime)
add_test(NAME gc_bus_fast COMMAND gc_bus_fast_test)

# Held pages: which pending copy a byte belongs to, how many bytes a host
# instruction moves, and that an access beside a copy's bytes runs at once
# while one into them waits for the copy (gc_memory.h).
add_executable(gc_memory_test gc_memory_test.c)
target_link_libraries(gc_memory_test PRIVATE gcruntime)
target_include_directories(gc_memory_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime-gc/src")
add_test(NAME gc_memory COMMAND gc_memory_test)

# What the graphics thread keeps about EFB copies: when the card's texture of
# one stands for a texture a draw names, and when a copy whose place has been
# written since is found again by its bytes (gc_gx_thread.h). The test plays
# the card's part itself. The thread exists on Windows only.
if(WIN32)
    add_executable(gc_gx_copies_test gc_gx_copies_test.c)
    target_link_libraries(gc_gx_copies_test PRIVATE gcruntime)
    target_include_directories(gc_gx_copies_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime-gc/src")
    add_test(NAME gc_gx_copies COMMAND gc_gx_copies_test)
endif()

# The checked-in decode header must match what the generator produces, so
# that a change to the ISA table cannot land without the header following.
add_test(
    NAME gc_decode_header_current
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gc/gen_decode.py" --check
)

# What a trace keeps: entries, not returns from interrupts, merged with the
# file already there.
add_executable(gc_coverage_test gc_coverage_test.c)
target_link_libraries(gc_coverage_test PRIVATE gcruntime)
add_test(NAME gc_coverage COMMAND gc_coverage_test)

# The graphics FIFO and the vertex decoder: how big a vertex is, and what
# happens when a command arrives in pieces.
add_executable(gc_gx_test gc_gx_test.c)
target_link_libraries(gc_gx_test PRIVATE gcruntime)
add_test(NAME gc_gx COMMAND gc_gx_test)

# The sound microcode: the command list walker, a voice through the decoder
# and the mixer, and the output format. No hardware oracle exists for any of
# it, so these check the code against docs/gc-dsp-ax.md rather than against
# a console - on cases chosen so the ways it goes wrong are distinguishable.
add_executable(gc_ax_test gc_ax_test.c)
target_link_libraries(gc_ax_test PRIVATE gcruntime)
add_test(NAME gc_ax COMMAND gc_ax_test)

# The other sound microcode, the one Nintendo's own games upload: recognised
# by its bytes, and its conversation through the mailbox held word for word
# to docs/gc-dsp-zelda.md (gc_zelda.h).
add_executable(gc_zelda_test gc_zelda_test.c)
target_link_libraries(gc_zelda_test PRIVATE gcruntime)
add_test(NAME gc_zelda COMMAND gc_zelda_test)

# And what its frames hold: voices small enough to work out on paper, mixed
# and compared sample for sample with the document's arithmetic.
add_executable(gc_zelda_mix_test gc_zelda_mix_test.c)
target_link_libraries(gc_zelda_mix_test PRIVATE gcruntime)
add_test(NAME gc_zelda_mix COMMAND gc_zelda_mix_test)

# And what that mix sounds like: three seconds of a looping stream and a sound
# effect, held to the properties tools/gc/audio_check.py holds a game's output
# to - no clipping or clicks, a tonal spectrum, the level the volumes give.
add_executable(gc_ax_sound_test gc_ax_sound_test.c)
target_link_libraries(gc_ax_sound_test PRIVATE gcruntime)
if(NOT WIN32)
    target_link_libraries(gc_ax_sound_test PRIVATE m)
endif()
add_test(NAME gc_ax_sound COMMAND gc_ax_sound_test)

# The disc reader, against an image built here rather than committed: a
# checked-in binary is a thing nobody can review, and the maker script is
# forty lines.
set(GC_TEST_DISC "${CMAKE_CURRENT_BINARY_DIR}/test.gcm")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gc/make_test_disc.py"
            --out "${GC_TEST_DISC}"
    RESULT_VARIABLE gc_disc_made
    OUTPUT_QUIET
)
if(gc_disc_made EQUAL 0)
    add_executable(gc_disc_test gc_disc_test.c)
    target_link_libraries(gc_disc_test PRIVATE gcruntime)
    add_test(NAME gc_disc COMMAND gc_disc_test "${GC_TEST_DISC}")
else()
    message(FATAL_ERROR "GC: tools/gc/make_test_disc.py failed (${gc_disc_made})")
endif()

# The Gekko vectors, measured on an oracle by tools/gc/capture_vectors.py.
# They are checked in, so this needs no devkitPPC and no Dolphin.
add_executable(gc_vectors gc_vectors.c gc_test_bus.c)
target_link_libraries(gc_vectors PRIVATE gccpu)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/gekko.vectors")
    add_test(
        NAME gc_vectors
        COMMAND gc_vectors "${CMAKE_CURRENT_SOURCE_DIR}/vectors/gekko.vectors" --report 6
    )
else()
    message(STATUS "GC: tests/gc/vectors/gekko.vectors not found; run tools/gc/capture_vectors.py to enable gc_vectors")
endif()

# The checked-in case table must match what the generator produces, for the
# same reason the decode header does.
add_test(
    NAME gc_vector_cases_current
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gc/gen_vector_cases.py" --check
)

# A wait loop that a trace's entry points cut into blocks is still emitted as
# one, so a game rebuilt with its cover trace keeps the skip.
if(TARGET gcrecomp)
    add_test(
        NAME gc_split_wait_loop
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gc_split_wait_loop.py"
            --gcrecomp "$<TARGET_FILE:gcrecomp>"
    )
endif()

# The overlay menu opens from the keyboard and from a controller alone,
# pauses the game, and its settings survive a restart (SDL dummy drivers).
if(TARGET gc_play)
    add_test(
        NAME gc_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gc/menu_selftest.py"
            --play "$<TARGET_FILE:gc_play>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest"
    )
endif()

# Save states resume exactly: the test programs run straight through, against
# the same programs saved part way (inside fields and at a field's end) and
# resumed from the file - in the same process, and then in a fresh one. A
# controller that changes every field and a memory card ride along. The
# programs need devkitPPC (tools/gc/build_gc_programs.py); a program that has
# not been built is skipped with a note, as the other program tests do.
set(GC_STATE_PROGRAMS gc_spin gc_drawtest gc_audiotest gc_cardtest gc_padtest gc_disctest gc_tevtest
    gc_textest gc_fogtest gc_memtest)
set(gc_state_args --out "${CMAKE_CURRENT_BINARY_DIR}/state_roundtrip" --disc "${GC_TEST_DISC}" --card)
foreach(program IN LISTS GC_STATE_PROGRAMS)
    list(APPEND gc_state_args "${PROJECT_SOURCE_DIR}/build/gc-programs/${program}.elf")
endforeach()
add_executable(gc_state_test gc_state_test.c)
target_link_libraries(gc_state_test PRIVATE gcruntime)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/state_roundtrip")
add_test(NAME gc_state_roundtrip COMMAND gc_state_test ${gc_state_args})
set_tests_properties(gc_state_roundtrip PROPERTIES FIXTURES_SETUP gc_state_files)
add_test(NAME gc_state_roundtrip_fresh_process COMMAND gc_state_test --verify ${gc_state_args})
set_tests_properties(gc_state_roundtrip_fresh_process PROPERTIES FIXTURES_REQUIRED gc_state_files)

# The memory card across a restart: the same program run twice against one
# image, which is the only way to check that what a game wrote reached the
# file the next run will open. Registers unconditionally; the script says so
# and passes when the program has not been built.
add_test(
    NAME gc_card_restart
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/gc_card_restart.py"
        --gc-run $<TARGET_FILE:gc_run>
        --program "${PROJECT_SOURCE_DIR}/build/gc-programs/gc_cardtest.elf"
)

# The memory card as a commercial game's save library sees it: driven through
# the expansion-bus registers, mounted under the retail library's checks
# (which libogc, and so gc_cardtest, does not make - the header serial against
# the console's flash ID above all), saved to and remounted after a restart,
# at two card sizes. Needs nothing built outside this tree.
add_executable(gc_card_test gc_card_test.c)
target_link_libraries(gc_card_test PRIVATE gcruntime)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/card_test")
add_test(NAME gc_card_retail_mount COMMAND gc_card_test "${CMAKE_CURRENT_BINARY_DIR}/card_test")

# The test programs, against their baseline: dolphin-emu/hwtests for the
# hardware layer and tests/gc/programs for the machine around the CPU. Both
# need devkitPPC to build (tools/gc/build_hwtests.py,
# tools/gc/build_gc_programs.py), so the test registers only when at least
# one of them is there - and the runner exits zero, saying so, if they
# disappear.
set(GC_HWTESTS_DIR "${PROJECT_SOURCE_DIR}/build/hwtests" CACHE PATH
    "Directory populated by tools/gc/build_hwtests.py")
set(GC_PROGRAMS_DIR "${PROJECT_SOURCE_DIR}/build/gc-programs" CACHE PATH
    "Directory populated by tools/gc/build_gc_programs.py")
set(gc_test_trees)
if(EXISTS "${GC_HWTESTS_DIR}/cputest")
    list(APPEND gc_test_trees --tests "${GC_HWTESTS_DIR}")
endif()
if(EXISTS "${GC_PROGRAMS_DIR}")
    list(APPEND gc_test_trees --tests "${GC_PROGRAMS_DIR}")
endif()
if(gc_test_trees)
    add_test(
        NAME gc_hwtests_baseline
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/gc/run_gc_tests.py"
            --gc-run $<TARGET_FILE:gc_run>
            ${gc_test_trees}
            --disc "${GC_TEST_DISC}"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/hwtest_baseline.txt"
    )
else()
    message(STATUS "GC: neither ${GC_HWTESTS_DIR} nor ${GC_PROGRAMS_DIR} found; build them with tools/gc/build_hwtests.py and tools/gc/build_gc_programs.py to enable gc_hwtests_baseline")
endif()
