# The RDP: command lists run through n64_rdp_execute and the DPC registers,
# checked byte for byte in RDRAM (n64_rdp_test.c).
if(TARGET n64rdp)
    add_executable(n64_rdp_test ${CMAKE_CURRENT_LIST_DIR}/n64_rdp_test.c)
    target_link_libraries(n64_rdp_test PRIVATE n64rdp)
    add_test(NAME n64_rdp COMMAND n64_rdp_test)
endif()

# Peter Lemon's (krom) RDP test ROMs against their reference screenshots,
# held to tests/n64/rdp_baseline.txt (tools/n64/compare_rdp_tests.py; fails
# closed, needs Pillow). Registered only when the ROMs have been fetched.
if(TARGET n64_run AND EXISTS "${N64_TEST_DATA_DIR}/krom/RDP")
    add_test(NAME n64_rdp_test_roms
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/n64/compare_rdp_tests.py
                     --runner $<TARGET_FILE:n64_run>
                     --data-dir ${N64_TEST_DATA_DIR}
                     --baseline ${CMAKE_CURRENT_LIST_DIR}/rdp_baseline.txt)
    set_tests_properties(n64_rdp_test_roms PROPERTIES TIMEOUT 900)
endif()
