# Wii tests.
#
# The Wii is the GameCube's machine grown, and shares its runtime
# (runtime-gc). These are the tests of what is Wii-only. None of them needs
# a game, a key, devkitPPC or anything fetched: the disc they read is built
# here, around keys made up for it.

# AES-128 and SHA-1 against the vectors in FIPS-197, SP 800-38A and
# RFC 3174. The file is compiled in rather than linked from the runtime so
# that this test is of those two hundred lines and nothing else.
add_executable(wii_crypto_test
    wii_crypto_test.c
    "${PROJECT_SOURCE_DIR}/runtime-gc/src/wii_crypto.c"
)
target_include_directories(wii_crypto_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime-gc/include")
add_test(NAME wii_crypto COMMAND wii_crypto_test)

# The test disc's builder (wii_test_disc.h): a library, because the disc
# test builds its discs itself, and a small program for anyone else who
# wants one written to a file.
add_library(wii_test_disc STATIC wii_test_disc.c)
target_include_directories(wii_test_disc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(wii_test_disc PUBLIC gcruntime)

add_executable(wii_make_test_disc wii_make_test_disc.c)
target_link_libraries(wii_make_test_disc PRIVATE wii_test_disc)

# The disc reader on Wii discs: every container, every way of supplying a
# key, and the formats that are turned away. It writes about twenty
# megabytes of images into its own directory under the build tree.
set(WII_TEST_DISC_DIR "${CMAKE_CURRENT_BINARY_DIR}/discs")
file(MAKE_DIRECTORY "${WII_TEST_DISC_DIR}")
add_executable(wii_disc_test wii_disc_test.c)
target_link_libraries(wii_disc_test PRIVATE wii_test_disc)
add_test(NAME wii_disc COMMAND wii_disc_test "${WII_TEST_DISC_DIR}")

# The builder's command line, which nothing above goes through.
add_test(
    NAME wii_make_test_disc
    COMMAND wii_make_test_disc "${WII_TEST_DISC_DIR}/made-by-tool.wbfs" --wbfs 21 --far
)

# ---------------------------------------------------------------------------
# The machine's Wii half (runtime-gc/include/wii.h).

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Memory, Broadway's extra BATs, Hollywood's registers and the mailbox's
# handshake, driven from outside with no guest code.
add_executable(wii_machine_test wii_machine_test.c)
target_link_libraries(wii_machine_test PRIVATE gcruntime)
add_test(NAME wii_machine COMMAND wii_machine_test)

# ---------------------------------------------------------------------------
# The flash filesystem (wii_nand.c, wii_sysconf.c, wii_ios_fs.c).

# The tree and its one host file, with no machine: every operation and
# every error ISFS has, its limits, the round trip through the file and
# what happens to a damaged one, the default console, and SYSCONF and
# setting.txt read back by a reader that shares nothing with the writer.
add_executable(wii_nand_test wii_nand_test.c)
target_link_libraries(wii_nand_test PRIVATE gcruntime)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/nand_test")
add_test(NAME wii_nand COMMAND wii_nand_test "${CMAKE_CURRENT_BINARY_DIR}/nand_test")

# /dev/fs and the files, through the mailbox: each request's buffer layout
# as libogc and the SDK send it, the descriptor limit, how long a read
# takes, and the host file being written a second after a game stops.
# (The same from a guest's side is wii_fstest, one of the programs
# wii_programs_baseline runs.)
add_executable(wii_fs_device_test wii_fs_device_test.c)
target_link_libraries(wii_fs_device_test PRIVATE gcruntime)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fs_device_test")
add_test(NAME wii_fs_device COMMAND wii_fs_device_test "${CMAKE_CURRENT_BINARY_DIR}/fs_device_test")

# The guest programs: tests/wii/programs, and tests/gc/programs built for the
# Wii, against their baseline. They need devkitPPC to build
# (tools/wii/build_wii_programs.py), so the tests register only when the
# programs are there.
set(WII_PROGRAMS_DIR "${PROJECT_SOURCE_DIR}/build/wii-programs" CACHE PATH
    "Directory populated by tools/wii/build_wii_programs.py")
if(EXISTS "${WII_PROGRAMS_DIR}/wii_hello.elf" AND TARGET gc_run)
    add_test(
        NAME wii_programs_baseline
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/gc/run_gc_tests.py"
            --gc-run $<TARGET_FILE:gc_run>
            --tests "${WII_PROGRAMS_DIR}"
            --runner-arg=--wii
            --disc "${WII_PROGRAMS_DIR}/test.gcm"
            --card "${CMAKE_CURRENT_BINARY_DIR}/programs-card.raw"
            --baseline "${CMAKE_CURRENT_SOURCE_DIR}/programs_baseline.txt"
    )
    # The same drawing programs leave the same frame on both consoles.
    add_test(
        NAME wii_frames_match_gamecube
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/wii/compare_consoles.py"
            --gc-run $<TARGET_FILE:gc_run>
            --wii "${WII_PROGRAMS_DIR}"
    )
    # A disc that boots: an encrypted partition, an apploader, a DOL, and the
    # drive and title devices behind IOS, interpreted and then recompiled
    # from the disc with nothing but the key the recompiler embedded.
    if(TARGET gcrecomp)
        add_test(
            NAME wii_boot_disc
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/wii/check_boot_disc.py"
                --gc-run $<TARGET_FILE:gc_run>
                --gcrecomp $<TARGET_FILE:gcrecomp>
                --tool $<TARGET_FILE:wii_make_test_disc>
                --programs "${WII_PROGRAMS_DIR}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/boot-disc"
        )
    endif()
else()
    message(STATUS "Wii: ${WII_PROGRAMS_DIR} not found; build the guest programs with tools/wii/build_wii_programs.py (needs devkitPPC) to enable wii_programs_baseline, wii_frames_match_gamecube and wii_boot_disc")
endif()

# dolphin-emu/hwtests built for the console it was written for
# (tools/gc/build_hwtests.py --wii), held to the *GameCube's* baseline: the
# processor and the graphics chip are the same on both, so the Wii machine
# has to get the same numbers, test for test.
set(WII_HWTESTS_DIR "${PROJECT_SOURCE_DIR}/build/wii-hwtests" CACHE PATH
    "Directory populated by tools/gc/build_hwtests.py --wii")
if(EXISTS "${WII_HWTESTS_DIR}/cputest" AND TARGET gc_run)
    add_test(
        NAME wii_hwtests_baseline
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/gc/run_gc_tests.py"
            --gc-run $<TARGET_FILE:gc_run>
            --tests "${WII_HWTESTS_DIR}"
            --runner-arg=--wii
            --baseline "${PROJECT_SOURCE_DIR}/tests/gc/hwtest_baseline.txt"
    )
else()
    message(STATUS "Wii: ${WII_HWTESTS_DIR} not found; build it with tools/gc/build_hwtests.py --wii (needs devkitPPC) to enable wii_hwtests_baseline")
endif()

# ---------------------------------------------------------------------------
# Bluetooth and the remotes (runtime-gc/include/wii_bt.h).

# The console's Bluetooth module and the remotes behind it, with the test
# playing a game's Bluetooth stack: start-up as lwbt and as Broadcom's stack
# do it, a remote's connection and channels, every report, the camera, both
# extensions, service discovery, flow control, four remotes at once, and the
# same run twice giving the same bytes at the same cycles.
add_executable(wii_bt_test wii_bt_test.c)
target_link_libraries(wii_bt_test PRIVATE gcruntime)
add_test(NAME wii_bt COMMAND wii_bt_test)

# The same from a guest's side: libogc's own stack and its remote library
# read a remote the runner holds a known way (gc_run --pad-test), once with
# a Nunchuk plugged in and once with a Classic Controller. (The Nunchuk run
# is also one of the programs wii_programs_baseline runs.)
if(TARGET gc_run AND EXISTS "${WII_PROGRAMS_DIR}/wii_padtest.elf")
    add_test(
        NAME wii_padtest_nunchuk
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/wii/run_wii_program.py"
            --gc-run $<TARGET_FILE:gc_run>
            --program "${WII_PROGRAMS_DIR}/wii_padtest.elf" --tests 5
            -- --wii --pad-test --frames 1200
    )
    add_test(
        NAME wii_padtest_classic
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/wii/run_wii_program.py"
            --gc-run $<TARGET_FILE:gc_run>
            --program "${WII_PROGRAMS_DIR}/wii_padtest.elf" --tests 5
            -- --wii --pad-test=classic --frames 1200
    )
    set_tests_properties(wii_padtest_nunchuk wii_padtest_classic PROPERTIES TIMEOUT 900)
else()
    message(STATUS "Wii: ${WII_PROGRAMS_DIR}/wii_padtest.elf not found; run tools/wii/build_wii_programs.py to enable wii_padtest_*")
endif()
