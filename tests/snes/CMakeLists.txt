# SNES tests.
#
# External test data comes from tools/fetch_snes_tests.py. Point
# SNES_TEST_DATA_DIR elsewhere to share one download between checkouts.
# Tests that need fetched data are registered only when it is present.

set(SNES_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/snes" CACHE PATH
    "Directory populated by tools/fetch_snes_tests.py")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(sst_65816 sst_65816.c)
target_link_libraries(sst_65816 PRIVATE snescpu)

if(EXISTS "${SNES_TEST_DATA_DIR}/sst65816/v1")
    add_test(
        NAME snes_sst_65816
        COMMAND sst_65816 "${SNES_TEST_DATA_DIR}/sst65816/v1" --report 2
    )
else()
    message(STATUS "SNES: ${SNES_TEST_DATA_DIR}/sst65816/v1 not found; run tools/fetch_snes_tests.py to enable snes_sst_65816")
endif()

# SPC700 vectors (the APU library alone).
add_executable(sst_spc700 sst_spc700.c)
target_link_libraries(sst_spc700 PRIVATE snesapu)
if(EXISTS "${SNES_TEST_DATA_DIR}/sstspc700/v1")
    add_test(NAME snes_sst_spc700 COMMAND sst_spc700 "${SNES_TEST_DATA_DIR}/sstspc700/v1" --report 2)
else()
    message(STATUS "SNES: ${SNES_TEST_DATA_DIR}/sstspc700/v1 not found; run tools/fetch_snes_tests.py to enable snes_sst_spc700")
endif()

# APU unit tests: boot ROM protocol, timers, DSP, handshaking.
add_executable(test_snes_apu test_snes_apu.c)
target_link_libraries(test_snes_apu PRIVATE snesapu)
add_test(NAME snes_apu_units COMMAND test_snes_apu)

# PPU unit tests: VRAM/CGRAM/OAM access, latches, rendering.
add_executable(test_snes_ppu test_snes_ppu.c)
target_link_libraries(test_snes_ppu PRIVATE snesppu)
add_test(NAME snes_ppu_units COMMAND test_snes_ppu)

# The CRT display filter's picture.
add_executable(test_snes_crt test_snes_crt.c)
target_link_libraries(test_snes_crt PRIVATE snesruntime)
add_test(NAME snes_crt_filter COMMAND test_snes_crt)

# SA-1 unit tests on a synthetic cartridge: Super MMC, BW-RAM/I-RAM,
# arithmetic, bit reader, DMA, character conversion, timer, interrupts.
add_executable(test_snes_sa1 test_snes_sa1.c)
target_link_libraries(test_snes_sa1 PRIVATE snesruntime)
add_test(NAME snes_sa1_units COMMAND test_snes_sa1)

# The overlay menu opens from the keyboard and from a controller alone,
# pauses the game, and its settings survive a restart (SDL dummy drivers).
if(TARGET snes_play)
    add_test(
        NAME snes_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/snes/menu_selftest.py"
            --play "$<TARGET_FILE:snes_play>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest"
    )
endif()

# The assembly-to-C checklist ROM (assembled from tools/snes/checklist) in
# the interpreter: every checklist test must pass. It needs no fetched data.
add_test(
    NAME snes_checklist_interpreter
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/snes/run_checklist.py"
        --runner "$<TARGET_FILE:snes_run>"
        --out "${CMAKE_CURRENT_BINARY_DIR}/checklist"
)

# The checklist ROM recompiled by snesrecomp and built against this build's
# runtime: every checklist test must pass natively, and --differential must
# stay clean. The full test-ROM set: tools/snes/recomp_suite.py.
add_test(
    NAME snes_recomp_checklist
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/snes/recomp_suite.py"
        --filter checklist
        --build "${PROJECT_BINARY_DIR}"
        --data "${SNES_TEST_DATA_DIR}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/recomp_checklist"
)

# Test ROMs with a readable verdict, run by the interpreter; fails if any
# ROM in the baseline stops passing. The baseline spans several suites, so
# it is registered only when all of them were fetched (CI fetches gilyon
# alone).
set(_snes_baseline_suites gilyon peterlemon higan absindx)
set(_snes_have_baseline_data TRUE)
foreach(_suite IN LISTS _snes_baseline_suites)
    if(NOT EXISTS "${SNES_TEST_DATA_DIR}/${_suite}")
        set(_snes_have_baseline_data FALSE)
    endif()
endforeach()
if(_snes_have_baseline_data)
    add_test(
        NAME snes_test_roms_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/snes/run_snes_tests.py"
            --runner "$<TARGET_FILE:snes_run>"
            --data "${SNES_TEST_DATA_DIR}"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/rom_baseline.txt"
    )
else()
    message(STATUS "SNES: test-ROM suites (${_snes_baseline_suites}) not all found; run tools/fetch_snes_tests.py to enable snes_test_roms_baseline")
endif()
if(EXISTS "${SNES_TEST_DATA_DIR}/gilyon")
    # gilyon's hardware-verified CPU and SPC700 tests, recompiled.
    add_test(
        NAME snes_recomp_gilyon
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/snes/recomp_suite.py"
            --filter gilyon --no-checklist
            --build "${PROJECT_BINARY_DIR}"
            --data "${SNES_TEST_DATA_DIR}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/recomp_gilyon"
    )
else()
    message(STATUS "SNES: ${SNES_TEST_DATA_DIR}/gilyon not found; run tools/fetch_snes_tests.py to enable the test-ROM suites")
endif()
