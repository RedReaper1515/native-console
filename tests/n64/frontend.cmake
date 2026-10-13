# The SDL2 frontend's overlay menu (P5 gate): it opens from the keyboard and
# from a controller alone, pauses the game, rebinds a key and saves the
# bindings back as it found them, and its settings survive a restart. Runs
# n64_play on the repository's fixture ROM with SDL's dummy drivers and
# scratch preferences and save folders (tests/n64/menu_selftest.py).
if(TARGET n64_play)
    add_test(
        NAME n64_menu_selftest
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/menu_selftest.py"
            --play "$<TARGET_FILE:n64_play>"
            --rom-maker "${PROJECT_SOURCE_DIR}/tests/fixtures/make_n64_rom.py"
            --out "${CMAKE_CURRENT_BINARY_DIR}/menu_selftest"
    )
    set_tests_properties(n64_menu_selftest PROPERTIES TIMEOUT 300)
endif()
