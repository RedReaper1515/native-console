# Game Boy Advance tests.
#
# External test data comes from tools/fetch_gba_tests.py. Point
# GBA_TEST_DATA_DIR elsewhere to share one download between checkouts.

set(GBA_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/gba" CACHE PATH
    "Directory populated by tools/fetch_gba_tests.py")

add_executable(sst_arm7tdmi sst_arm7tdmi.c)
target_link_libraries(sst_arm7tdmi PRIVATE gbacpu)

add_executable(test_gba_eeprom test_gba_eeprom.c)
target_link_libraries(test_gba_eeprom PRIVATE gbaruntime)
add_test(NAME gba_eeprom_protocol COMMAND test_gba_eeprom)

add_executable(test_gba_apu test_gba_apu.c)
target_link_libraries(test_gba_apu PRIVATE gbaruntime)
add_test(NAME gba_apu_mixer COMMAND test_gba_apu)

add_executable(test_gba_rtc test_gba_rtc.c)
target_link_libraries(test_gba_rtc PRIVATE gbaruntime)
add_test(NAME gba_rtc_protocol COMMAND test_gba_rtc)

add_executable(test_gba_link test_gba_link.c)
target_link_libraries(test_gba_link PRIVATE gbaruntime)
add_test(NAME gba_link_cable COMMAND test_gba_link)

add_executable(test_gba_state test_gba_state.c)
target_link_libraries(test_gba_state PRIVATE gbaruntime)
add_test(NAME gba_save_state COMMAND test_gba_state)
set_tests_properties(gba_save_state PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(test_gba_ppu_large test_gba_ppu_large.c)
target_link_libraries(test_gba_ppu_large PRIVATE gbaruntime)
add_test(NAME gba_ppu_large COMMAND test_gba_ppu_large)

add_executable(test_gba_upscale test_gba_upscale.c)
target_link_libraries(test_gba_upscale PRIVATE gbaruntime)
add_test(NAME gba_mmpx_upscale COMMAND test_gba_upscale)

add_executable(test_gba_crt test_gba_crt.c)
target_link_libraries(test_gba_crt PRIVATE gbaruntime)
add_test(NAME gba_crt_filter COMMAND test_gba_crt)

add_executable(test_gba_texture_pack test_gba_texture_pack.c)
target_link_libraries(test_gba_texture_pack PRIVATE gbaruntime)
add_test(NAME gba_texture_pack COMMAND test_gba_texture_pack)

if(TARGET gbaplatform)
    add_executable(test_gba_input test_gba_input.cpp)
    target_link_libraries(test_gba_input PRIVATE gbaplatform)
    target_compile_features(test_gba_input PRIVATE cxx_std_17)
    add_test(NAME gba_input_bindings COMMAND test_gba_input)
    get_target_property(_gba_test_sdl2_type SDL2::SDL2 TYPE)
    if(WIN32 AND _gba_test_sdl2_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET test_gba_input POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:test_gba_input>)
    endif()
endif()

if(EXISTS "${GBA_TEST_DATA_DIR}/sst/v1")
    add_test(
        NAME gba_sst_arm7tdmi
        COMMAND sst_arm7tdmi "${GBA_TEST_DATA_DIR}/sst/v1" --report 2
    )
else()
    message(STATUS "GBA: ${GBA_TEST_DATA_DIR}/sst/v1 not found; run tools/fetch_gba_tests.py to enable gba_sst_arm7tdmi")
endif()

# Test ROMs that report through R12 (jsmolka, Alyosha), run by the
# interpreter; fails if any ROM in the baseline stops passing.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The overlay menu opens from the keyboard and from a controller alone,
# pauses the game, its settings survive a restart, and --mute stays out of
# them (SDL dummy drivers).
if(TARGET gba_play)
    add_test(
        NAME gba_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gba/menu_selftest.py"
            --play "$<TARGET_FILE:gba_play>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest"
    )
endif()

if(EXISTS "${GBA_TEST_DATA_DIR}/jsmolka" AND EXISTS "${GBA_TEST_DATA_DIR}/alyosha")
    add_test(
        NAME gba_test_roms_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gba/run_gba_tests.py"
            --runner "$<TARGET_FILE:gba_run>"
            --data "${GBA_TEST_DATA_DIR}"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/r12_baseline.txt"
    )
endif()

# Recompile the jsmolka ROMs with gbarecomp, build them against this build's
# runtime, and require the same verdicts as the interpreter plus a clean
# --differential run. The full 138-ROM set: tools/gba/recomp_suite.py.
if(EXISTS "${GBA_TEST_DATA_DIR}/jsmolka")
    add_test(
        NAME gba_recomp_jsmolka
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gba/recomp_suite.py"
            --filter jsmolka
            --build "${PROJECT_BINARY_DIR}"
            --data "${GBA_TEST_DATA_DIR}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/recomp_jsmolka"
    )
endif()
