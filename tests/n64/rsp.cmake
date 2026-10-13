# The RSP on its own: scalar and vector instructions, SP registers and DMA,
# run as small programs in IMEM (tests/n64/n64_rsp_test.c).
if(TARGET n64rsp)
    add_executable(n64_rsp_test ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_test.c)
    target_link_libraries(n64_rsp_test PRIVATE n64rsp)
    add_test(NAME n64_rsp COMMAND n64_rsp_test)

    # The interpreter's fast paths (SSE2 vector unit, whole-lane loads and
    # stores, the decode cache) against the reference handlers and a plain
    # decode-and-step interpreter, over edge cases and random state.
    add_executable(n64_rsp_fast_test ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_fast_test.c)
    target_link_libraries(n64_rsp_fast_test PRIVATE n64rsp)
    add_test(NAME n64_rsp_fast COMMAND n64_rsp_fast_test)

    # Both again with the RSP built from the reference handlers alone, which
    # is what a host without SSE2 runs, so that path is tested on every host.
    get_target_property(_n64_rsp_sources n64rsp SOURCES)
    get_target_property(_n64_rsp_includes n64rsp INTERFACE_INCLUDE_DIRECTORIES)
    add_library(n64rsp_reference STATIC ${_n64_rsp_sources})
    target_include_directories(n64rsp_reference PUBLIC ${_n64_rsp_includes})
    target_compile_definitions(n64rsp_reference PUBLIC N64_RSP_REFERENCE_ONLY)
    add_executable(n64_rsp_reference_test ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_test.c)
    target_link_libraries(n64_rsp_reference_test PRIVATE n64rsp_reference)
    add_test(NAME n64_rsp_reference COMMAND n64_rsp_reference_test)
    add_executable(n64_rsp_fast_reference_test ${CMAKE_CURRENT_SOURCE_DIR}/n64_rsp_fast_test.c)
    target_link_libraries(n64_rsp_fast_reference_test PRIVATE n64rsp_reference)
    add_test(NAME n64_rsp_fast_reference COMMAND n64_rsp_fast_reference_test)
endif()
