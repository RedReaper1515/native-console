# NES tests.
#
# External test data comes from tools/fetch_nes_tests.py. Point
# NES_TEST_DATA_DIR elsewhere to share one download between checkouts.
# Tests that need fetched data are registered only when it is present.

set(NES_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/nes" CACHE PATH
    "Directory populated by tools/fetch_nes_tests.py")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 6502 vectors: sst_6502 links the core alone and provides the bus itself.
# It shares the JSON reader of the SNES vector harnesses.
add_executable(sst_6502 sst_6502.c)
target_link_libraries(sst_6502 PRIVATE nescpu)
target_include_directories(sst_6502 PRIVATE ${PROJECT_SOURCE_DIR}/tests/snes)

# Reset, interrupt entry, NMI hijacking, interrupt poll timing, JAM and the
# decode table: what the vectors do not cover. Needs no test data.
add_test(NAME nes_cpu_units COMMAND sst_6502 --units)

if(EXISTS "${NES_TEST_DATA_DIR}/sst6502/v1")
    add_test(
        NAME nes_sst_6502
        COMMAND sst_6502 "${NES_TEST_DATA_DIR}/sst6502/v1" --report 2
    )
else()
    message(STATUS "NES: ${NES_TEST_DATA_DIR}/sst6502/v1 not found; run tools/fetch_nes_tests.py to enable nes_sst_6502")
endif()

# PPU unit tests: registers, VBlank/NMI timing, the bus accesses of a
# scanline, background and sprite rendering against a test cartridge, and
# the RGB palette. The test program provides the nes_ppu_bus_* functions.
add_executable(test_nes_ppu test_nes_ppu.c)
target_link_libraries(test_nes_ppu PRIVATE nesppu)
add_test(NAME nes_ppu_units COMMAND test_nes_ppu)

# APU unit tests: channels, frame counter and IRQ timing, DMC, mixer tables,
# the output stage, chunked runs and the event horizon. The test program
# provides the DMC's sample fetch.
add_executable(test_nes_apu test_nes_apu.c)
target_link_libraries(test_nes_apu PRIVATE nesapu)
add_test(NAME nes_apu_units COMMAND test_nes_apu)

# Cartridge header and mapper boards on synthetic images.
add_executable(test_nes_cart test_nes_cart.c)
target_link_libraries(test_nes_cart PRIVATE nesruntime)
add_test(NAME nes_cart_units COMMAND test_nes_cart)

# The CRT display filter's picture. The filter needs nothing of the machine,
# so the test compiles it directly.
add_executable(test_nes_crt
    test_nes_crt.c
    ${PROJECT_SOURCE_DIR}/runtimes/runtime-nes/src/nes_crt.c
)
target_include_directories(test_nes_crt PRIVATE ${PROJECT_SOURCE_DIR}/runtimes/runtime-nes/include)
if(NOT WIN32)
    target_link_libraries(test_nes_crt PRIVATE m)
endif()
add_test(NAME nes_crt_filter COMMAND test_nes_crt)

# The overlay menu opens from the keyboard and from a controller alone,
# pauses the game, its settings survive a restart and a rebinding is saved
# and put back; the battery save and the overscan setting are checked on the
# way (SDL dummy drivers, a ROM made by the script).
if(TARGET nes_play)
    add_test(
        NAME nes_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/menu_selftest.py"
            --play "$<TARGET_FILE:nes_play>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest"
    )
endif()

# The project's own assembler: every encoding against the decode header and
# a hand-written reference table.
add_test(NAME nes_asm6502_selftest
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/asm6502.py" --selftest)

# The battery-save fixture (also the release smoke ROM) writes A5 5A to PRG
# RAM and idles.
add_test(NAME nes_sram_fixture_build
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/fixtures/make_nes_sram_rom.py"
        --output "${CMAKE_CURRENT_BINARY_DIR}/nes_sram.nes")
set_tests_properties(nes_sram_fixture_build PROPERTIES FIXTURES_SETUP nes_sram_rom)
add_test(NAME nes_sram_fixture
    COMMAND nes_run "${CMAKE_CURRENT_BINARY_DIR}/nes_sram.nes" --until-idle --frames 30
        --expect-ram 6000=0xA5 --expect-ram 6001=0x5A)
set_tests_properties(nes_sram_fixture PROPERTIES FIXTURES_REQUIRED nes_sram_rom)

# The checklist ROM (tools/nes/checklist): assembled from source, so it needs
# no fetched data. Interpreted, then recompiled: built, run natively and in
# --differential lockstep.
add_test(NAME nes_checklist_interpreter
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/run_checklist.py"
        --runner "$<TARGET_FILE:nes_run>" --out "${CMAKE_CURRENT_BINARY_DIR}/checklist")
add_test(NAME nes_recomp_checklist
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/run_checklist.py"
        --runner "$<TARGET_FILE:nes_run>" --recompiler "$<TARGET_FILE:nesrecomp>"
        --build "${PROJECT_BINARY_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/recomp_checklist" --trace)
set_tests_properties(nes_recomp_checklist PROPERTIES TIMEOUT 600)

# Tests on fetched ROMs.
if(EXISTS "${NES_TEST_DATA_DIR}/nes-test-roms/other/nestest.nes")
    # kevtris's nestest against its reference trace: every instruction's
    # registers and cycle count.
    add_test(
        NAME nes_nestest_trace
        COMMAND "${CMAKE_COMMAND}"
            -DNES_RUN=$<TARGET_FILE:nes_run>
            -DROM=${NES_TEST_DATA_DIR}/nes-test-roms/other/nestest.nes
            -DREFERENCE=${NES_TEST_DATA_DIR}/nes-test-roms/other/nestest.log
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/nestest.log
            -DPYTHON=${Python3_EXECUTABLE}
            -DCOMPARE=${PROJECT_SOURCE_DIR}/tools/nes/nestest_compare.py
            -P "${CMAKE_CURRENT_SOURCE_DIR}/nestest.cmake"
    )
    # Every catalogued test ROM in the interpreter, each judged the way its
    # suite reports (tools/nes/run_nes_tests.py); fails if a ROM of the
    # baseline stops passing, is missing, or a fetched ROM is not catalogued.
    add_test(
        NAME nes_test_roms_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/run_nes_tests.py"
            --runner "$<TARGET_FILE:nes_run>"
            --data "${NES_TEST_DATA_DIR}"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/rom_baseline.txt"
    )
    # The P4 gate on a short representative subset: each ROM recompiled,
    # built against this build's runtime, the same verdict as interpreted,
    # and --differential clean. The whole catalogue:
    # tools/nes/recomp_suite.py --build <build dir>.
    add_test(
        NAME nes_recomp_suite
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/recomp_suite.py"
            --subset
            --build "${PROJECT_BINARY_DIR}"
            --runner "$<TARGET_FILE:nes_run>"
            --recompiler "$<TARGET_FILE:nesrecomp>"
            --data "${NES_TEST_DATA_DIR}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rs"
    )
    set_tests_properties(nes_recomp_suite PROPERTIES TIMEOUT 900)
else()
    message(STATUS "NES: ${NES_TEST_DATA_DIR}/nes-test-roms not found; run tools/fetch_nes_tests.py to enable the ROM tests")
endif()

# AccuracyCoin (MIT): its 146 tests run headless; fails if a test of the
# baseline stops passing or the run does not reach the result screen.
if(EXISTS "${NES_TEST_DATA_DIR}/accuracycoin/AccuracyCoin.nes")
    add_test(
        NAME nes_accuracycoin_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/accuracycoin.py"
            --runner "$<TARGET_FILE:nes_run>"
            --rom "${NES_TEST_DATA_DIR}/accuracycoin/AccuracyCoin.nes"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/accuracycoin_baseline.txt"
            --failures
    )
endif()

# A save state resumes exactly: a run that saves at frame 60 and a run that
# loads that state end in the same machine state and picture as one straight
# run (the checklist ROM, interpreted; tools/nes/state_resume.py crosses it
# with recompiled code when given a generated game).
add_test(NAME nes_state_resume
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nes/state_resume.py"
        --exe "$<TARGET_FILE:nes_run>" --out "${CMAKE_CURRENT_BINARY_DIR}/state_resume")
