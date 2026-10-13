# Nintendo 64 tests. Every test is named n64_* so `ctest -R n64` selects them.
#
# External test data comes from tools/fetch_n64_tests.py. Point
# N64_TEST_DATA_DIR elsewhere to share one download between checkouts.
# Tests that need fetched data are registered only when it is present.

set(N64_TEST_DATA_DIR "${PROJECT_SOURCE_DIR}/roms/n64" CACHE PATH
    "Directory populated by tools/fetch_n64_tests.py")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Each part's tests live in their own file, so the parts can grow apart.
foreach(_part cpu rsp rdp vi machine recomp frontend)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_part}.cmake")
        include("${CMAKE_CURRENT_SOURCE_DIR}/${_part}.cmake")
    endif()
endforeach()
