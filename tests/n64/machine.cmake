# The machine and the test ROMs.

# Peter Lemon's CPU, FPU and RSP tests, held to tests/n64/rom_baseline.txt.
if(TARGET n64_run AND EXISTS "${N64_TEST_DATA_DIR}/krom/CPUTest")
    add_test(
        NAME n64_test_roms_baseline
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/n64/run_n64_tests.py"
                --runner $<TARGET_FILE:n64_run> --data "${N64_TEST_DATA_DIR}"
                --baseline "${CMAKE_CURRENT_SOURCE_DIR}/rom_baseline.txt" -j 4
    )
    set_tests_properties(n64_test_roms_baseline PROPERTIES TIMEOUT 900)
else()
    message(STATUS "N64: ${N64_TEST_DATA_DIR}/krom not found; run tools/fetch_n64_tests.py --suite krom for n64_test_roms_baseline")
endif()
