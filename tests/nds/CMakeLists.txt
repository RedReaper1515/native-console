# Nintendo DS tests.
#
# External test data comes from tools/fetch_nds_tests.py, and the ARM7's
# per-instruction vectors from tools/fetch_gba_tests.py (the ARM7 is an
# ARM7TDMI). Point NDS_TEST_DATA_DIR or NDS_ARM7_VECTOR_DIR elsewhere to
# share one download between checkouts.

set(NDS_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/nds" CACHE PATH
    "Directory populated by tools/fetch_nds_tests.py")
set(NDS_ARM7_VECTOR_DIR "${PROJECT_SOURCE_DIR}/roms/gba/sst/v1" CACHE PATH
    "SingleStepTests ARM7TDMI vectors, from tools/fetch_gba_tests.py --suite sst")

add_executable(sst_nds_arm sst_arm.c)
target_link_libraries(sst_nds_arm PRIVATE ndscpu)

add_executable(test_nds_cpu test_nds_cpu.c)
target_link_libraries(test_nds_cpu PRIVATE ndscpu)
add_test(NAME nds_cpu_arm9_behaviour COMMAND test_nds_cpu)

add_executable(test_nds_machine test_nds_machine.c)
target_link_libraries(test_nds_machine PRIVATE ndsruntime)
add_test(NAME nds_machine COMMAND test_nds_machine)

# The recompiler's ROM front end: header and file system, BLZ, the code
# modules, the ds-decomp reader.
add_executable(test_nds_rom test_nds_rom.cpp)
target_link_libraries(test_nds_rom PRIVATE ndsrecomp_core)
add_test(NAME nds_rom_front_end COMMAND test_nds_rom)

# The analyzer's rules for code nothing calls directly: leaf functions that
# are only pointed at from data, and the instructions a run reports.
add_executable(test_nds_analysis test_nds_analysis.cpp)
target_link_libraries(test_nds_analysis PRIVATE ndsrecomp_core)
add_test(NAME nds_analysis COMMAND test_nds_analysis)

# The same against a real game and its decompilation's configuration: every
# module hash, symbol and relocation. The ROM is the user's own and is never
# committed, so this runs only where it is pointed at one.
set(NDS_PH_ROM "" CACHE FILEPATH
    "The Legend of Zelda: Phantom Hourglass (USA), the user's own, for the ds-decomp module check; never committed")
if(NDS_PH_ROM AND EXISTS "${NDS_PH_ROM}" AND EXISTS "${NDS_TEST_DATA_DIR}/ph-decomp/config/usa/arm9/config.yaml")
    add_test(NAME nds_rom_ph_modules
        COMMAND ndsrecomp "${NDS_PH_ROM}" --symbols "${NDS_TEST_DATA_DIR}/ph-decomp/config/usa/arm9" --check)
endif()

# One 2D engine on its own: every value of every field that selects
# something, on both engines.
add_executable(test_nds_2d test_nds_2d.c)
target_link_libraries(test_nds_2d PRIVATE ndsruntime)
add_test(NAME nds_2d COMMAND test_nds_2d)

# The 3D engine on its own. The geometry engine against GBATEK and
# independent arithmetic; the rasteriser against two sets of hardware
# recordings: polyrastertest's 77 scenes (compiled in from
# tests/nds/gx/polyrastertest_data.h) and nds-interp's edge captures, which
# tools/nds/gx_unpack_interp.py unpacks from the fetched archive.
add_executable(test_nds_gx test_nds_gx.c)
target_link_libraries(test_nds_gx PRIVATE ndsruntime)
add_test(NAME nds_gx COMMAND test_nds_gx)

add_executable(test_nds_gx_raster test_nds_gx_raster.c)
target_link_libraries(test_nds_gx_raster PRIVATE ndsruntime)
if(EXISTS "${NDS_TEST_DATA_DIR}/nds-interp-data/TL.bin")
    add_test(NAME nds_gx_raster COMMAND test_nds_gx_raster --interp "${NDS_TEST_DATA_DIR}/nds-interp-data")
    add_test(NAME nds_gx_raster_full COMMAND test_nds_gx_raster --interp "${NDS_TEST_DATA_DIR}/nds-interp-data" --full)
else()
    message(STATUS "NDS: ${NDS_TEST_DATA_DIR}/nds-interp-data not found; run tools/nds/gx_unpack_interp.py to enable the nds-interp gate")
    add_test(NAME nds_gx_raster COMMAND test_nds_gx_raster --skip-interp)
endif()

# The sound unit on its own: every channel format, the mixer's output stage,
# the capture units.
add_executable(test_nds_spu test_nds_spu.c)
target_link_libraries(test_nds_spu PRIVATE ndsruntime)
add_test(NAME nds_spu COMMAND test_nds_spu)

# The ARM7's serial devices and the card's save chip, each on its own.
add_executable(test_nds_firmware test_nds_firmware.c)
target_link_libraries(test_nds_firmware PRIVATE ndsruntime)
add_test(NAME nds_firmware_image COMMAND test_nds_firmware)

add_executable(test_nds_spi test_nds_spi.c)
target_link_libraries(test_nds_spi PRIVATE ndsruntime)
add_test(NAME nds_spi_protocol COMMAND test_nds_spi)

add_executable(test_nds_save test_nds_save.c)
target_link_libraries(test_nds_save PRIVATE ndsruntime)
add_test(NAME nds_save_protocol COMMAND test_nds_save)

add_executable(test_nds_rtc test_nds_rtc.c)
target_link_libraries(test_nds_rtc PRIVATE ndsruntime)
add_test(NAME nds_rtc_protocol COMMAND test_nds_rtc)

if(EXISTS "${NDS_ARM7_VECTOR_DIR}")
    add_test(
        NAME nds_sst_arm7
        COMMAND sst_nds_arm --cpu 7 "${NDS_ARM7_VECTOR_DIR}" --report 2
    )
else()
    message(STATUS "NDS: ${NDS_ARM7_VECTOR_DIR} not found; run tools/fetch_gba_tests.py --suite sst to enable nds_sst_arm7")
endif()

if(EXISTS "${NDS_TEST_DATA_DIR}/armv5-vectors/index.txt")
    add_test(
        NAME nds_sst_arm9
        COMMAND sst_nds_arm --cpu 9 "${NDS_TEST_DATA_DIR}/armv5-vectors" --report 2
    )
else()
    message(STATUS "NDS: ${NDS_TEST_DATA_DIR}/armv5-vectors not found; run tools/nds/gen_armv5_vectors.py to enable nds_sst_arm9")
endif()

# The wrestler ROMs, read from the picture: each test's verdict rectangle
# against the pixels a pass shows (tools/nds/oracle_make_wrestlers.py writes
# the data from a DeSmuME run; tools/nds/oracle_check.py reads it).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The save chip table is what tools/nds/gen_save_db.py writes from the pinned
# database. Only where a copy is on disk: no network in tests.
if(EXISTS "${NDS_TEST_DATA_DIR}/skyemu/src/nds_rom_database.h")
    add_test(
        NAME nds_save_db_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/gen_save_db.py" --check
                --source "${NDS_TEST_DATA_DIR}/skyemu/src/nds_rom_database.h"
    )
endif()
if(EXISTS "${NDS_TEST_DATA_DIR}/oracle/armwrestler/tests.json" AND
   EXISTS "${NDS_TEST_DATA_DIR}/oracle/rockwrestler/tests.json")
    add_test(
        NAME nds_wrestlers
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/oracle_check.py"
                $<TARGET_FILE:nds_run> --data "${NDS_TEST_DATA_DIR}" --suite armwrestler --suite rockwrestler
    )
else()
    message(STATUS "NDS: ${NDS_TEST_DATA_DIR}/oracle/*/tests.json not found; run tools/nds/oracle_make_wrestlers.py to enable nds_wrestlers")
endif()

# polyrastertest as a ROM, through the whole machine: its own verdict lines
# on the bottom screen ("Testing complete!", "Tests Passed: 77/77").
if(EXISTS "${NDS_TEST_DATA_DIR}/oracle/polyrastertest/tests.json")
    add_test(
        NAME nds_polyrastertest_rom
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/oracle_check.py"
                $<TARGET_FILE:nds_run> --data "${NDS_TEST_DATA_DIR}" --suite polyrastertest
    )
endif()

# The frontend's input layer: bindings, where each layout puts the screens on
# the displays it is used on, which display pixel touches which DS pixel, and
# which options send a command line to the headless runner (read from the
# runner's source too, so one added there and forgotten here fails).
if(TARGET ndsplatform)
    add_executable(test_nds_input test_nds_input.cpp)
    target_link_libraries(test_nds_input PRIVATE ndsplatform)
    add_test(NAME nds_input_layouts_and_bindings
             COMMAND test_nds_input "${PROJECT_SOURCE_DIR}/runtimes/runtime-nds/src/nds_runner.c")
    get_target_property(_nds_test_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _nds_test_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET test_nds_input POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_nds_input>)
    endif()
endif()

# The overlay menu, driven with real SDL events under SDL's dummy drivers:
# it opens from the keyboard and from a controller alone and pauses the game,
# bindings are changed, saved and put back, the stylus, microphone and lid
# reach the game, and settings and the save survive a restart.
if(TARGET nds_play)
    add_test(NAME nds_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nds/menu_selftest.py"
                --play "$<TARGET_FILE:nds_play>" --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest")
endif()

# The built test ROMs, each read the way it reports (tools/nds/run_nds_tests.py);
# fails when an entry of the baseline stops passing or an unknown one fails.
if(EXISTS "${NDS_TEST_DATA_DIR}/built/MANIFEST.json")
    add_test(
        NAME nds_test_roms_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/run_nds_tests.py"
            --nds-run "$<TARGET_FILE:nds_run>"
            --roms "${NDS_TEST_DATA_DIR}/built"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/blocksds_tests.json"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/blocksds_baseline.json"
            --json "${CMAKE_CURRENT_BINARY_DIR}/nds_test_roms.json"
    )
    # Profile, recompile and build each ROM the way the hub does, then
    # require the interpreter's verdicts and final state and a clean
    # --differential. These need the compiler's environment. The full set:
    # tools/nds/recomp_suite.py.
    add_test(
        NAME nds_recomp_blocksds_system
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/recomp_suite.py"
            --filter system_ --no-oracle --profile
            --ndsrecomp "$<TARGET_FILE:ndsrecomp>"
            --roms "${NDS_TEST_DATA_DIR}/built"
            --work "${CMAKE_CURRENT_BINARY_DIR}/recomp_system"
            --json "${CMAKE_CURRENT_BINARY_DIR}/recomp_system.json"
    )
    set_tests_properties(nds_recomp_blocksds_system PROPERTIES TIMEOUT 900)
    if(EXISTS "${NDS_TEST_DATA_DIR}/oracle/armwrestler/tests.json" AND
       EXISTS "${NDS_TEST_DATA_DIR}/oracle/rockwrestler/tests.json")
        add_test(
            NAME nds_recomp_wrestlers
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/nds/recomp_suite.py"
                --filter wrestler --profile
                --ndsrecomp "$<TARGET_FILE:ndsrecomp>"
                --roms "${NDS_TEST_DATA_DIR}/built" --data "${NDS_TEST_DATA_DIR}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/recomp_wrestlers"
                --json "${CMAKE_CURRENT_BINARY_DIR}/recomp_wrestlers.json"
        )
        set_tests_properties(nds_recomp_wrestlers PROPERTIES TIMEOUT 900)
    endif()
else()
    message(STATUS "NDS: ${NDS_TEST_DATA_DIR}/built/MANIFEST.json not found; run tools/nds/build_test_roms.py to enable nds_test_roms_baseline and nds_recomp_*")
endif()

# ndsrecomp on the repository's own ROMs (tests/fixtures/make_nds_save_rom.py):
# the profile and its trace, the project it writes, and code a program runs
# from another address than it was loaded at.
add_test(
    NAME nds_recompile_fixture
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/recompile_fixture.py"
            --ndsrecomp $<TARGET_FILE:ndsrecomp> --work "${CMAKE_CURRENT_BINARY_DIR}/recompile-fixture"
)
