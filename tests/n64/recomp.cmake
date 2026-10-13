# The recompiler, end to end on the repository's own test ROM: interpreted,
# recompiled from the boot image, recompiled with a profile, and in
# --differential lockstep (tests/n64/recompile_fixture.py).
if(TARGET n64recomp AND TARGET n64_run)
    add_test(
        NAME n64_recompile_fixture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/recompile_fixture.py"
                --n64recomp $<TARGET_FILE:n64recomp> --n64run $<TARGET_FILE:n64_run>
                --libdir "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/recompile-fixture"
    )
    set_tests_properties(n64_recompile_fixture PROPERTIES TIMEOUT 600)
endif()
