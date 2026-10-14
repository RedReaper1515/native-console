find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Generated projects call find_package(SDL2) themselves; forward the SDL2 the
# tree was configured with so relocated builds resolve it the same way.
set(GBRECOMP_GENERATED_CMAKE_ARGS)
if(DEFINED SDL2_DIR AND NOT SDL2_DIR STREQUAL "SDL2_DIR-NOTFOUND")
    # One token: argparse reads a separate "-D..." value as an option unless it contains a space.
    list(APPEND GBRECOMP_GENERATED_CMAKE_ARGS "--cmake-arg=-DSDL2_DIR=${SDL2_DIR}")
endif()

# Windows releases ship SDL2.dll and its license next to the recompilers and
# the hub, and SDL2's development files in SDL2/ where SDL2_DIR is the cmake
# folder of an unpacked SDL2-devel-*-VC package.
set(GBRECOMP_SDL2_RELEASE_ARGS)
if(WIN32 AND TARGET SDL2::SDL2 AND DEFINED SDL2_DIR)
    get_target_property(_gbrecomp_sdl2_type SDL2::SDL2 TYPE)
    get_filename_component(_gbrecomp_sdl2_root "${SDL2_DIR}" DIRECTORY)
    set(_gbrecomp_sdl2_license "")
    foreach(_candidate LICENSE.txt COPYING.txt)
        if(EXISTS "${_gbrecomp_sdl2_root}/${_candidate}")
            set(_gbrecomp_sdl2_license "${_gbrecomp_sdl2_root}/${_candidate}")
            break()
        endif()
    endforeach()
    if(_gbrecomp_sdl2_type STREQUAL "SHARED_LIBRARY" AND _gbrecomp_sdl2_license)
        list(APPEND GBRECOMP_SDL2_RELEASE_ARGS
            "--extra-file=$<TARGET_FILE:SDL2::SDL2>=SDL2.dll"
            "--extra-file=${_gbrecomp_sdl2_license}=THIRD_PARTY/SDL2-LICENSE.txt")
        if(EXISTS "${SDL2_DIR}/sdl2-config.cmake" AND EXISTS "${_gbrecomp_sdl2_root}/lib/x64/SDL2.lib")
            list(APPEND GBRECOMP_SDL2_RELEASE_ARGS "--sdl2-dev=${_gbrecomp_sdl2_root}")
        endif()
    endif()
endif()

add_test(
    NAME accuracy_runner_rejects_empty_selection
    COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/run_tests.py"
        --filter __gbrecomp_no_matching_test__
)
set_tests_properties(accuracy_runner_rejects_empty_selection PROPERTIES
    WILL_FAIL TRUE
)

add_test(
    NAME accuracy_runner_exit_policy
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_accuracy_runner_policy.py"
)

add_test(
    NAME gb_discover_tool
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_gb_discover.py"
)

add_test(
    NAME gb_texture_pack_tool
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_gb_texture_pack.py"
)

add_test(
    NAME accuracy_runner_cache_provenance
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_accuracy_runner_cache.py"
)

add_test(
    NAME nl0_profile_summary_estimates_dynamic_coverage
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_nl0_profile_summary.py"
)

add_test(
    NAME nl0_profile_runner_requires_cycle_anchored_input
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_nl0_profile_runner.py"
)

add_test(
    NAME multi_rom_analysis_isolation
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_multi_rom_analysis_isolation.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        --fixture-generator
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
)

# Packages this build like a release (every console's recompiler and
# runtime, the hub) and proves the relocated copy builds and runs a project
# per console. The GBA project needs jsmolka's arm.gba from
# tools/fetch_gba_tests.py; without it only the GBA files are checked.
set(_release_gba_rom "${PROJECT_SOURCE_DIR}/roms/gba/jsmolka/arm/arm.gba")
if(EXISTS "${_release_gba_rom}")
    set(_release_gba_args --gba-rom "${_release_gba_rom}")
else()
    set(_release_gba_args --skip gba)
endif()
add_test(
    NAME release_bundle_is_self_contained
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_release_bundle.py"
        --bin-dir "$<TARGET_FILE_DIR:gbrecomp>"
        ${_release_gba_args}
        ${GBRECOMP_SDL2_RELEASE_ARGS}
        ${GBRECOMP_GENERATED_CMAKE_ARGS}
)
set_tests_properties(release_bundle_is_self_contained PROPERTIES TIMEOUT 900)

add_test(
    NAME generation_progress_is_machine_readable_and_private
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_generation_progress.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        --runtime-dir "${PROJECT_SOURCE_DIR}/runtime"
        --fixture-generator
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
)
set_tests_properties(
    generation_progress_is_machine_readable_and_private
    PROPERTIES TIMEOUT 180
)

add_test(
    NAME native_patch_sdk_end_to_end
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_native_patch_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        --fixture-generator
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
        ${GBRECOMP_GENERATED_CMAKE_ARGS}
)
set_tests_properties(native_patch_sdk_end_to_end PROPERTIES TIMEOUT 360)

add_executable(test_mbc5_rom_bank_id
    test_mbc5_rom_bank_id.cpp
)
target_link_libraries(test_mbc5_rom_bank_id PRIVATE gbrecomp_core)
add_test(NAME mbc5_rom_bank_id COMMAND test_mbc5_rom_bank_id)

add_executable(test_mbc5_analysis test_mbc5_analysis.cpp)
target_link_libraries(test_mbc5_analysis PRIVATE gbrecomp_core)
add_test(NAME mbc5_analysis_uses_9_bit_banks COMMAND test_mbc5_analysis)
set_tests_properties(mbc5_analysis_uses_9_bit_banks PROPERTIES TIMEOUT 5)

add_executable(test_mbc5_codegen test_mbc5_codegen.cpp)
target_link_libraries(test_mbc5_codegen PRIVATE gbrecomp_core)
add_test(NAME mbc5_codegen_uses_9_bit_banks COMMAND test_mbc5_codegen)

add_executable(test_codegen_bus_phase test_codegen_bus_phase.cpp)
target_link_libraries(test_codegen_bus_phase PRIVATE gbrecomp_core)
add_test(NAME codegen_places_memory_access_in_final_mcycle COMMAND test_codegen_bus_phase)

add_executable(test_analyzer_call_clobber test_analyzer_call_clobber.cpp)
target_link_libraries(test_analyzer_call_clobber PRIVATE gbrecomp_core)
add_test(NAME analyzer_call_clobbers_constants COMMAND test_analyzer_call_clobber)

add_executable(test_analyzer_register_effects test_analyzer_register_effects.cpp)
target_link_libraries(test_analyzer_register_effects PRIVATE gbrecomp_core)
add_test(NAME analyzer_invalidates_written_registers COMMAND test_analyzer_register_effects)

add_executable(test_analyzer_state_join test_analyzer_state_join.cpp)
target_link_libraries(test_analyzer_state_join PRIVATE gbrecomp_core)
add_test(NAME analyzer_joins_control_flow_state COMMAND test_analyzer_state_join)

add_executable(test_analyzer_mapper_state test_analyzer_mapper_state.cpp)
target_link_libraries(test_analyzer_mapper_state PRIVATE gbrecomp_core)
add_test(NAME analyzer_models_mapper_registers COMMAND test_analyzer_mapper_state)

add_executable(test_analyzer_home_bank_calls test_analyzer_home_bank_calls.cpp)
target_link_libraries(test_analyzer_home_bank_calls PRIVATE gbrecomp_core)
add_test(NAME analyzer_resolves_home_bank_calls COMMAND test_analyzer_home_bank_calls)

add_executable(test_analyzer_overlap_resolution test_analyzer_overlap_resolution.cpp)
target_link_libraries(test_analyzer_overlap_resolution PRIVATE gbrecomp_core)
add_test(NAME analyzer_excludes_overlapping_decodes COMMAND test_analyzer_overlap_resolution)

add_test(NAME generated_bank_guards_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_bank_guard_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_bank_guards_end_to_end PROPERTIES TIMEOUT 240)

add_test(NAME generated_dynamic_code_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_dynamic_code_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_dynamic_code_end_to_end PROPERTIES TIMEOUT 240)

add_test(NAME generated_jump_safepoints_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_jump_safepoint_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_jump_safepoints_end_to_end PROPERTIES TIMEOUT 240)

# Lifting to source (docs/LIFTING.md): the export, the mechanical lift and the
# checker, on a ROM with a function for every instruction. Needs a C compiler
# on PATH, like the generated-project tests.
add_test(NAME lift_export_baseline_and_checker_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(lift_export_baseline_and_checker_end_to_end PROPERTIES TIMEOUT 240)

# Calls made through a bank-switching helper: found by running the helper
# (gb_lift.py edges), handed back with --lift-edges, and the functions they
# reach exported, lifted, checked and compiled.
add_test(NAME lift_far_call_edges_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_edges.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(lift_far_call_edges_end_to_end PROPERTIES TIMEOUT 240)

# Lifted source built into a generated game: the game has to end in the same
# state with it as without, resume from a save state made inside it, and
# change when a lifted function is edited.
add_test(NAME generated_lifted_source_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_runtime_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lifted_source_end_to_end PROPERTIES TIMEOUT 300)

# Evidence from running the game: a generated game run with --evidence has
# to record the jumps its code does not spell out and nothing else, end in
# the same state, and give the export the functions only those jumps reach.
add_test(NAME generated_evidence_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_evidence.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_evidence_end_to_end PROPERTIES TIMEOUT 300)

# The states a game entered its functions in: a game run with --entry-states
# has to end as it does without and keep the registers and memory of the
# arrivals the rule names, and `gb_lift.py check --entries` has to get a
# function through, exactly as the game ran it, that no made-up state does.
add_test(NAME generated_lift_entries_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_entries.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_entries_end_to_end PROPERTIES TIMEOUT 600)

# Which bytes of the ROM a game executed: a game run with --executed has to
# end as it does without and note exactly the bytes of the instructions it
# ran, and `gb_lift.py executed` has to sort the export's functions by it and
# by what was read as data: ran, read and never executed, or neither.
add_test(NAME generated_executed_map_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_executed.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_executed_map_end_to_end PROPERTIES TIMEOUT 300)

# The ROM's data as files: taken apart with `gb_data.py split`, put together
# again byte for byte, and an edited data file showing in a game given the
# rebuilt image with --rom-image, which refuses one that changes its code.
add_test(NAME generated_data_rebuild_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_rebuild.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_rebuild_end_to_end PROPERTIES TIMEOUT 300)

# A data file's tiles as a picture: `gb_data.py tiles` draws them as a PNG,
# and `build` reads the picture back into the bytes once it has been edited,
# however it was saved. Unedited, the image is still the ROM. Compiles no game.
add_test(NAME data_tiles_as_png
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_tiles.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(data_tiles_as_png PROPERTIES TIMEOUT 120)

# What the game does with its data: a game run with --data-use follows every
# byte it reads from the ROM to video memory, the sprite table, the sound
# registers or a jump, ends as it does without, and `gb_data.py uses` writes
# what was seen into the data tree.
add_test(NAME generated_data_use_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_use.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_use_end_to_end PROPERTIES TIMEOUT 300)

# The other way round: a game run with --trace-back says which instruction
# wrote a watched byte of RAM and what the value was made of, back through
# RAM to ROM bytes, constants and hardware; it ends as it does without, and
# leaves the other recorders' files as they are. `gb_trace_back.py` prints it.
add_test(NAME generated_trace_back_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_trace_back.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_trace_back_end_to_end PROPERTIES TIMEOUT 300)

# A game's text as text: the instruction that prints is found from what the
# game was seen to read, `gb_data.py glyphs` draws its letters out of a save
# state, `text` writes its strings to a file, and a string edited there is in
# the image `build` makes and on the screen of the game run with it.
add_test(NAME generated_data_text_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_text.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_text_end_to_end PROPERTIES TIMEOUT 300)

# What a game shows to be data, told to the analysis: `gb_data.py annotations`
# writes the bytes a played game used as they are, an export and a game made
# with --annotations that file have them as data, whole, and a picture of
# them edited shows in that game, where the game made without refuses it.
add_test(NAME generated_data_annotations_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_annotations.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_annotations_end_to_end PROPERTIES TIMEOUT 300)

# What a generated game holds an image to (--rom-image) is what its data tree
# calls code: the two come from one analysis and have to be one answer, also
# where an entry point lies at an instruction that is not decoded. Compiles
# no game.
add_test(NAME data_pinned_is_the_tree_code
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_pinned.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(data_pinned_is_the_tree_code PROPERTIES TIMEOUT 120)

# An address the analysis looked at and decoded nothing at (no instruction
# has that opcode, or it would run on into declared data) is no block: the
# game holds no image to it, takes every data file changed, and where it does
# go there the interpreter runs it to the same end.
add_test(NAME generated_data_undecoded_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_undecoded.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_undecoded_end_to_end PROPERTIES TIMEOUT 300)

# Packed data edited as what the game's own routine unpacks it to: a game
# given the pack `gb_data.py build` writes keeps what the routine makes
# (--unpacked-harvest), `unpacked` puts that into the tree as pictures, and an
# edited picture is in the game, recompiled and lifted, with the image still
# the ROM.
add_test(NAME generated_data_unpacked_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_unpacked.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_unpacked_end_to_end PROPERTIES TIMEOUT 600)

# Finding the routine that unpacks: a game run with --data-use-follow on the
# instruction that reads its packed bytes ends as it does without, records
# which functions were running while it read and what each read and stored,
# and `gb_data.py unpackers` proposes the tightest that reads a stream whole.
add_test(NAME generated_data_unpackers_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_unpackers.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_unpackers_end_to_end PROPERTIES TIMEOUT 300)

# The game unpacking streams nobody played as far as: a game given
# --unpacked-call enters a function and stops as the routine returns, and
# `gb_data.py unpack` has it do so for the streams of a list, of the tables
# declared, and of a walk on from where known streams end, which tells the
# guesses that were borne out from the ones that were not.
add_test(NAME generated_data_unpack_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_unpack.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_unpack_end_to_end PROPERTIES TIMEOUT 600)

# The files of a data tree called by name: `gb_data.py names` brings the
# labels of a symbol file into names.sym, renames regions, views and unpacked
# things by it, and the tree builds to the same image and pack as before;
# a name taken away gives the file its old name back. Compiles no game.
add_test(NAME data_names_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_names.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(data_names_end_to_end PROPERTIES TIMEOUT 300)

# Tile pictures in the game's colours: a game run with --data-use notes, once
# a frame, which layer drew each ROM tile and through which palette, on a
# Game Boy and on a Game Boy Color, and ends as it does without; `gb_data.py
# tiles` draws the picture in the palette most seen, colour 0 clear for a
# sprite, and an edit saved indexed, RGB or RGBA is read back right where two
# colours share a shade, or refused where nothing says which is meant.
# And tile maps: the record says how the bytes in a tile map lay on the
# screen and which tiles they drew, `gb_data.py maps` writes them as grids of
# tile numbers that `build` reads back, with a picture drawn from the tree.
add_test(NAME generated_data_colours_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_colours.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_colours_end_to_end PROPERTIES TIMEOUT 600)

# Pictures of tiles nobody said the place of: a game run with --data-use-tiles
# ends as it does without and records where in a tile each byte it copied
# landed, and `gb_data.py tiles` with no file named draws the runs of whole
# tiles that makes, at 1 or 2 bits a pixel, and leaves alone what somebody
# told it of or edited.
add_test(NAME generated_data_tiles_found_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_tiles_found.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_tiles_found_end_to_end PROPERTIES TIMEOUT 300)

# A played run kept and played again: two sittings recorded with
# `--record-run` replay to every checkpoint and to the end, recompiled and
# interpreted, from the record's own save and clock; what is not the run is
# told from it; `run_record.py` replays it in pieces from save states; a
# run a script played (`--input`) has every press in its record and replays;
# and a machine without its sound hardware (`--no-audio`) is refused.
add_test(NAME generated_run_record_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_run_record.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_run_record_end_to_end PROPERTIES TIMEOUT 300)

# Generated code does what the interpreter does, to the cycle: a game that
# waits for a line of the screen in a loop (`ldh a,[rLY]` / `cp l` / `jr nz`)
# has to be the same machine recompiled and interpreted after any number of
# frames, and a run of it recorded recompiled has to replay both ways.
add_test(NAME generated_ly_wait_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_ly_wait.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_ly_wait_end_to_end PROPERTIES TIMEOUT 300)

# A game played by a program: with `--control` the game stops at the start of
# every frame for `tools/gb_control.py`, which reads its memory and says what
# is held. What the program holds has to reach the game as presses, a run
# played that way has to be in its record and replay with no program there,
# and what a record would not hold has to be refused while one is recorded.
add_test(NAME generated_control_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_gb_control.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_control_end_to_end PROPERTIES TIMEOUT 300)

# The game exploring itself: `gb_lift.py explore` has to cross a room that
# buttons at random from power-on do not, by keeping the moments that show a
# new screen, and write evidence that makes what is behind it a function.
add_test(NAME generated_explore_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_explore.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_explore_end_to_end PROPERTIES TIMEOUT 600)

# A save state is of the game, not of the ROM image the game was given: one
# made with the ROM loads with an edited image (--rom-image) and the other way
# about, what is read again is the image's, a state from before goes with what
# it went with, another game's is refused, and a replayed run still takes only
# a state of its own image.
add_test(NAME generated_state_image_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_state_image.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_state_image_end_to_end PROPERTIES TIMEOUT 600)

# A data tree split again: `gb_data.py split --keep OLD` writes the tree of a
# new export with the old tree's edits, pictures, text and unpacked things
# taken over, names what cannot go with its bytes, stops at an edit that is
# code now, and builds the image and the pack the old tree builds; and `split
# --force` does not write over edits unless told to. Compiles no game.
add_test(NAME data_keep_split_again
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_keep.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(data_keep_split_again PROPERTIES TIMEOUT 300)

add_executable(test_rgbds_symbol_parser test_rgbds_symbol_parser.cpp)
target_link_libraries(test_rgbds_symbol_parser PRIVATE gbrecomp_core)
add_test(NAME rgbds_symbol_parser_handles_constants_and_aliases
         COMMAND test_rgbds_symbol_parser)

add_executable(test_analysis_diagnostics test_analysis_diagnostics.cpp)
target_link_libraries(test_analysis_diagnostics PRIVATE gbrecomp_core)
add_test(NAME analyzer_exports_actionable_diagnostics
         COMMAND test_analysis_diagnostics)

add_test(
    NAME generated_port_extension_end_to_end
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_port_extension_e2e.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        --fixture-generator
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
        ${GBRECOMP_GENERATED_CMAKE_ARGS}
)
set_tests_properties(generated_port_extension_end_to_end PROPERTIES TIMEOUT 180)

add_executable(test_mbc1_lower_window_analysis test_mbc1_lower_window_analysis.cpp)
target_link_libraries(test_mbc1_lower_window_analysis PRIVATE gbrecomp_core)
add_test(NAME analyzer_compiles_mbc1_lower_window COMMAND test_mbc1_lower_window_analysis)

set(GBRECOMP_TEST_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mbc1_mode1_lower_window")
set(GBRECOMP_TEST_ROM
    "${CMAKE_CURRENT_BINARY_DIR}/fixtures/mbc1_mode1_lower_window.gb")
set(GBRECOMP_TEST_INTERNAL_HEADER
    "${GBRECOMP_TEST_GENERATED_DIR}/mbc1_mode1_lower_window_internal.h")

add_custom_command(
    OUTPUT
        "${GBRECOMP_TEST_INTERNAL_HEADER}"
        "${GBRECOMP_TEST_GENERATED_DIR}/mbc1_mode1_lower_window.h"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
        --mapper mbc1
        --output "${GBRECOMP_TEST_ROM}"
    COMMAND
        "$<TARGET_FILE:gbrecomp>"
        "${GBRECOMP_TEST_ROM}"
        --no-scan
        -o "${GBRECOMP_TEST_GENERATED_DIR}"
    DEPENDS
        gbrecomp
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py"
    COMMENT "Generating the synthetic MBC1 test project"
    VERBATIM
)

if(TARGET gbrt)
    # The lift checker's own interpreter against the runtime's, per opcode.
    add_executable(test_lift_cpu
        test_lift_cpu.c
        "${PROJECT_SOURCE_DIR}/lift/checker/lift_cpu.c"
        "${PROJECT_SOURCE_DIR}/lift/checker/lift_machine.c")
    target_include_directories(test_lift_cpu PRIVATE
        "${PROJECT_SOURCE_DIR}/lift/checker")
    target_compile_definitions(test_lift_cpu PRIVATE GBLIFT_USE_RUNTIME_CONTEXT)
    target_link_libraries(test_lift_cpu PRIVATE gbrt)
    add_test(NAME lift_checker_cpu_matches_runtime_interpreter COMMAND test_lift_cpu)

    add_executable(test_mbc5_runtime_bank test_mbc5_runtime_bank.c)
    target_link_libraries(test_mbc5_runtime_bank PRIVATE gbrt)
    add_test(NAME mbc5_runtime_uses_9_bit_banks COMMAND test_mbc5_runtime_bank)

    add_executable(test_runtime_rom_mapping test_runtime_rom_mapping.c)
    target_link_libraries(test_runtime_rom_mapping PRIVATE gbrt)
    add_test(NAME runtime_uses_one_rom_mapping COMMAND test_runtime_rom_mapping)

    add_executable(test_eram_mirroring test_eram_mirroring.c)
    target_link_libraries(test_eram_mirroring PRIVATE gbrt)
    add_test(NAME cartridge_ram_mirrors_chip_address_lines COMMAND test_eram_mirroring)

    add_executable(test_hdma_timing test_hdma_timing.c)
    target_link_libraries(test_hdma_timing PRIVATE gbrt)
    add_test(NAME hdma_charges_hardware_cycles COMMAND test_hdma_timing)

    add_executable(test_oam_dma_bus test_oam_dma_bus.c)
    target_link_libraries(test_oam_dma_bus PRIVATE gbrt)
    add_test(NAME oam_dma_uses_model_specific_buses COMMAND test_oam_dma_bus)

    add_executable(test_oam_corruption test_oam_corruption.c)
    target_link_libraries(test_oam_corruption PRIVATE gbrt)
    add_test(NAME dmg_inc16_corrupts_current_oam_scan_row COMMAND test_oam_corruption)

    add_executable(test_timer_reload test_timer_reload.c)
    target_link_libraries(test_timer_reload PRIVATE gbrt)
    add_test(NAME timer_preserves_overflow_reload_edges COMMAND test_timer_reload)

    add_executable(test_timer_arithmetic test_timer_arithmetic.c)
    target_link_libraries(test_timer_arithmetic PRIVATE gbrt)
    add_test(NAME arithmetic_timer_matches_scalar_oracle COMMAND test_timer_arithmetic)

    add_executable(test_interrupt_entry test_interrupt_entry.c)
    target_link_libraries(test_interrupt_entry PRIVATE gbrt)
    add_test(NAME interrupt_entry_rechecks_ie_if_during_push COMMAND test_interrupt_entry)

    add_executable(test_halt_bug test_halt_bug.c)
    target_link_libraries(test_halt_bug PRIVATE gbrt)
    add_test(NAME halt_bug_suppresses_one_opcode_fetch_increment COMMAND test_halt_bug)

    add_executable(test_cgb_post_boot test_cgb_post_boot.c)
    target_link_libraries(test_cgb_post_boot PRIVATE gbrt)
    add_test(NAME cgb_post_boot_state_follows_header_and_mode COMMAND test_cgb_post_boot)

    add_executable(test_audio_chunk_invariance test_audio_chunk_invariance.c)
    target_link_libraries(test_audio_chunk_invariance PRIVATE gbrt)
    add_test(NAME audio_pcm_is_invariant_to_scheduler_batch_size COMMAND test_audio_chunk_invariance)

    if(SDL2_FOUND)
        add_executable(test_display_palette test_display_palette.cpp)
        target_link_libraries(test_display_palette PRIVATE gbrt)
        add_test(NAME display_palette_maps_completed_ppu_frames COMMAND test_display_palette)

        add_executable(test_audio_callback_concurrency test_audio_callback_concurrency.cpp)
        target_link_libraries(test_audio_callback_concurrency PRIVATE gbrt)
        add_test(NAME audio_callback_uses_race_free_batched_ring COMMAND test_audio_callback_concurrency)

        add_executable(test_input_script test_input_script.cpp)
        target_link_libraries(test_input_script PRIVATE gbrt)
        add_test(NAME input_script_rejects_malformed_tokens COMMAND test_input_script)

        add_executable(test_persistence_directory test_persistence_directory.cpp)
        target_link_libraries(test_persistence_directory PRIVATE gbrt)
        add_test(NAME persistence_directory_isolates_battery_save COMMAND test_persistence_directory)

        add_executable(test_screenshot_prefix test_screenshot_prefix.cpp)
        target_link_libraries(test_screenshot_prefix PRIVATE gbrt)
        add_test(NAME screenshot_prefix_preserves_long_evidence_path COMMAND test_screenshot_prefix)

        # SDL2 ships as a DLL on Windows. Without it beside the test binaries
        # the loader raises a modal "SDL2.dll was not found" dialog, which
        # makes CTest hang instead of fail.
        if(WIN32 AND TARGET SDL2::SDL2)
            get_target_property(_gbrecomp_sdl2_type SDL2::SDL2 TYPE)
            if(_gbrecomp_sdl2_type STREQUAL "SHARED_LIBRARY")
                foreach(_gbrecomp_sdl2_test
                        test_audio_callback_concurrency
                        test_input_script
                        test_persistence_directory
                        test_screenshot_prefix)
                    add_custom_command(TARGET ${_gbrecomp_sdl2_test} POST_BUILD
                        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "$<TARGET_FILE:SDL2::SDL2>"
                            "$<TARGET_FILE_DIR:${_gbrecomp_sdl2_test}>"
                        VERBATIM)
                endforeach()
            endif()
        endif()
    endif()

    add_executable(test_ppu_dot_timing test_ppu_dot_timing.c)
    target_link_libraries(test_ppu_dot_timing PRIVATE gbrt)
    add_test(NAME ppu_uses_dot_aware_pixel_transfer COMMAND test_ppu_dot_timing)

    add_executable(test_runtime_bus_phase test_runtime_bus_phase.c)
    target_link_libraries(test_runtime_bus_phase PRIVATE gbrt)
    add_test(NAME ram_stub_places_memory_access_in_final_mcycle COMMAND test_runtime_bus_phase)

    add_executable(test_state_json test_state_json.c)
    target_link_libraries(test_state_json PRIVATE gbrt)
    add_test(NAME state_json_includes_full_hram COMMAND test_state_json)

    # Link cable: two machines run a hand-assembled ROM that exchanges bytes.
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/linktest.gb"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_link_rom.py"
                "${CMAKE_CURRENT_BINARY_DIR}/linktest.gb"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_link_rom.py"
        VERBATIM)
    add_executable(test_gb_link test_gb_link.c "${CMAKE_CURRENT_BINARY_DIR}/linktest.gb")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/linktest.gb" PROPERTIES HEADER_FILE_ONLY TRUE)
    target_link_libraries(test_gb_link PRIVATE gbrt)
    add_test(NAME gb_link_cable_exchanges_bytes COMMAND test_gb_link "${CMAKE_CURRENT_BINARY_DIR}/linktest.gb")

    add_executable(test_runtime_test_breakpoint test_runtime_test_breakpoint.c)
    target_link_libraries(test_runtime_test_breakpoint PRIVATE gbrt)
    add_test(NAME runtime_stops_on_opt_in_test_breakpoint COMMAND test_runtime_test_breakpoint)

    add_executable(test_interpreter_diagnostics test_interpreter_diagnostics.c)
    target_link_libraries(test_interpreter_diagnostics PRIVATE gbrt)
    add_test(NAME interpreter_diagnostics_record_control_flow_exit COMMAND test_interpreter_diagnostics)

    add_executable(test_performance_counters test_performance_counters.c)
    target_link_libraries(test_performance_counters PRIVATE gbrt)
    add_test(NAME performance_counters_preserve_stop_semantics COMMAND test_performance_counters)

    add_executable(test_event_deadline test_event_deadline.c)
    target_link_libraries(test_event_deadline PRIVATE gbrt)
    add_test(NAME runtime_reports_conservative_next_event_deadline COMMAND test_event_deadline)

    add_executable(test_semantic_reader test_semantic_reader.c)
    target_link_libraries(test_semantic_reader PRIVATE gbrt)
    add_test(NAME semantic_reader_is_bounded_and_exact_rom
             COMMAND test_semantic_reader)

    # The mutation API is compiled into gbrt only with native patches on. That
    # option takes BUILD_TESTS as its default on the *first* configure only,
    # so a tree first configured without tests keeps it off - and a test
    # registered regardless fails to link rather than saying why.
    if(GBRECOMP_ENABLE_NATIVE_PATCHES)
        add_executable(test_gameplay_mutation test_gameplay_mutation.c)
        target_link_libraries(test_gameplay_mutation PRIVATE gbrt)
        add_test(NAME gameplay_mutation_is_atomic_and_fail_closed
                 COMMAND test_gameplay_mutation)
    else()
        message(STATUS "GB: GBRECOMP_ENABLE_NATIVE_PATCHES is OFF, so test_gameplay_mutation is not built (configure with -DGBRECOMP_ENABLE_NATIVE_PATCHES=ON)")
    endif()

    add_executable(test_host_configuration test_host_configuration.c)
    target_link_libraries(test_host_configuration PRIVATE gbrt)
    add_test(NAME host_configuration_is_canonical_and_fail_closed
             COMMAND test_host_configuration)

    add_executable(test_data_mod_overlay test_data_mod_overlay.c)
    target_link_libraries(test_data_mod_overlay PRIVATE gbrt)
    add_test(NAME data_mod_overlay_preserves_original_rom
             COMMAND test_data_mod_overlay)

    add_executable(test_port_extension test_port_extension.c)
    target_link_libraries(test_port_extension PRIVATE gbrt)
    add_test(NAME port_extension_is_versioned_headless_and_exact_rom
             COMMAND test_port_extension)

    add_executable(test_texture_pack test_texture_pack.c)
    target_link_libraries(test_texture_pack PRIVATE gbrt)
    add_test(NAME texture_pack_replaces_tiles_without_touching_guest_state
             COMMAND test_texture_pack)

    add_executable(test_voxel_view test_voxel_view.c)
    target_link_libraries(test_voxel_view PRIVATE gbrt)
    add_test(NAME voxel_view_tracks_rooms_and_falls_back_to_2d
             COMMAND test_voxel_view)

    add_executable(test_presentation_contract test_presentation_contract.c)
    target_link_libraries(test_presentation_contract PRIVATE gbrt)
    add_test(NAME native_presentation_is_exact_rom_and_gpu_independent
             COMMAND test_presentation_contract)

    add_executable(test_widescreen_compositor test_widescreen_compositor.c)
    target_link_libraries(test_widescreen_compositor PRIVATE gbrt)
    add_test(NAME widescreen_compositor_is_bounded_and_renderer_independent
             COMMAND test_widescreen_compositor)

    add_executable(test_presentation_replacements
        test_presentation_replacements.c)
    target_link_libraries(test_presentation_replacements PRIVATE gbrt)
    add_test(NAME presentation_replacements_are_hashed_and_provenanced
             COMMAND test_presentation_replacements)

    add_executable(test_generated_mbc1_fast_read
        test_generated_mbc1_fast_read.c
        "${GBRECOMP_TEST_INTERNAL_HEADER}"
    )
    target_include_directories(test_generated_mbc1_fast_read PRIVATE
        "${GBRECOMP_TEST_GENERATED_DIR}"
    )
    target_link_libraries(test_generated_mbc1_fast_read PRIVATE gbrt)

    add_test(
        NAME generated_mbc1_mode1_lower_window
        COMMAND test_generated_mbc1_fast_read
    )
else()
    message(WARNING "Skipping generated runtime tests because gbrt is unavailable")
endif()

# Native Console Hub: Steam shortcut files, account parsing, and artwork.
# Pure logic from hub/src, so it builds without SDL or the hub itself.
add_executable(test_hub_steam
    ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_steam.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_steam_vdf.cpp)
target_include_directories(test_hub_steam PRIVATE ${PROJECT_SOURCE_DIR}/hub/src)
target_compile_features(test_hub_steam PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_hub_steam PRIVATE /utf-8)
endif()
add_test(NAME hub_steam_shortcuts_and_artwork COMMAND test_hub_steam)

# Native Console Hub: the finished jobs kept on disk for the Jobs view, and
# what Clear history deletes.
add_executable(test_hub_job_history
    ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_job_history.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_job_history.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_json.cpp)
target_include_directories(test_hub_job_history PRIVATE ${PROJECT_SOURCE_DIR}/hub/src)
target_compile_features(test_hub_job_history PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_hub_job_history PRIVATE /utf-8)
endif()
add_test(NAME hub_job_history COMMAND test_hub_job_history)

# Native Console Hub: cancelling a job ends every process the job started.
# The hub's process runner on Windows; the POSIX one is not held to this yet.
if(WIN32)
    add_executable(test_hub_run_cancel
        ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_run_cancel.cpp
        ${PROJECT_SOURCE_DIR}/hub/src/hub_platform_common.cpp
        ${PROJECT_SOURCE_DIR}/hub/src/hub_platform_win.cpp)
    target_include_directories(test_hub_run_cancel PRIVATE ${PROJECT_SOURCE_DIR}/hub/src)
    target_compile_features(test_hub_run_cancel PRIVATE cxx_std_20)
    target_compile_definitions(test_hub_run_cancel PRIVATE UNICODE _UNICODE)
    target_link_libraries(test_hub_run_cancel PRIVATE ole32 shell32 windowscodecs advapi32)
    if(MSVC)
        target_compile_options(test_hub_run_cancel PRIVATE /utf-8)
    endif()
    add_test(NAME hub_run_cancel_ends_process_tree COMMAND test_hub_run_cancel "${Python3_EXECUTABLE}")
endif()

# Native Console Hub: texture pack listing, import, and launch arguments.
add_executable(test_hub_texture_packs
    ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_texture_packs.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_texture_packs.cpp)
target_include_directories(test_hub_texture_packs PRIVATE ${PROJECT_SOURCE_DIR}/hub/src)
target_compile_features(test_hub_texture_packs PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_hub_texture_packs PRIVATE /utf-8)
endif()
add_test(NAME hub_texture_packs_install_and_launch COMMAND test_hub_texture_packs)

# Native Console Hub: 3D view profiles (built-in, import, launch arguments).
include(${PROJECT_SOURCE_DIR}/hub/cmake/HubVoxelProfiles.cmake)
hub_generate_voxel_profiles("${CMAKE_CURRENT_BINARY_DIR}/test_hub_builtin_voxel_profiles.cpp")
add_executable(test_hub_voxel_profiles
    ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_voxel_profiles.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_voxel_profiles.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/test_hub_builtin_voxel_profiles.cpp)
target_include_directories(test_hub_voxel_profiles PRIVATE ${PROJECT_SOURCE_DIR}/hub/src)
target_compile_features(test_hub_voxel_profiles PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_hub_voxel_profiles PRIVATE /utf-8)
endif()
add_test(NAME hub_voxel_profiles_install_and_launch COMMAND test_hub_voxel_profiles)

# The Game Boy, GBA, SNES, NES, DS and N64 runtimes each carry the macOS app files; keep them identical.
foreach(bundle_file NativeConsoleMacBundle.cmake Info.plist.in make_macos_app.sh)
    add_test(NAME macos_bundle_${bundle_file}_copies_match
             COMMAND ${CMAKE_COMMAND} -E compare_files
                     ${PROJECT_SOURCE_DIR}/runtime/cmake/${bundle_file}
                     ${PROJECT_SOURCE_DIR}/runtime-gba/cmake/${bundle_file})
    add_test(NAME macos_bundle_${bundle_file}_snes_copy_matches
             COMMAND ${CMAKE_COMMAND} -E compare_files
                     ${PROJECT_SOURCE_DIR}/runtime/cmake/${bundle_file}
                     ${PROJECT_SOURCE_DIR}/runtime-snes/cmake/${bundle_file})
    add_test(NAME macos_bundle_${bundle_file}_nes_copy_matches
             COMMAND ${CMAKE_COMMAND} -E compare_files
                     ${PROJECT_SOURCE_DIR}/runtime/cmake/${bundle_file}
                     ${PROJECT_SOURCE_DIR}/runtime-nes/cmake/${bundle_file})
    add_test(NAME macos_bundle_${bundle_file}_nds_copy_matches
             COMMAND ${CMAKE_COMMAND} -E compare_files
                     ${PROJECT_SOURCE_DIR}/runtime/cmake/${bundle_file}
                     ${PROJECT_SOURCE_DIR}/runtime-nds/cmake/${bundle_file})
    add_test(NAME macos_bundle_${bundle_file}_n64_copy_matches
             COMMAND ${CMAKE_COMMAND} -E compare_files
                     ${PROJECT_SOURCE_DIR}/runtime/cmake/${bundle_file}
                     ${PROJECT_SOURCE_DIR}/runtime-n64/cmake/${bundle_file})
endforeach()

# A frontend that calls SDL_Quit() with text still in stdout's buffer now and
# then loses it on Windows (a menu self-test's verdict, say), so each one
# quits through a quit_sdl() that flushes first. The loss is too rare for a
# test that runs a frontend; this one reads the sources.
add_test(
    NAME frontends_flush_before_sdl_quit
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_frontends_flush_before_quit.py"
        --root "${PROJECT_SOURCE_DIR}"
)

# Hub: a Game Boy game started with edited data, a data tree or a built image
# (hub_edited_data.h). Pure logic from hub/src; with Python it also builds a
# small tree with the real tools/gb_data.py.
add_executable(test_hub_edited_data
    ${PROJECT_SOURCE_DIR}/hub/tests/test_hub_edited_data.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_edited_data.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_library.cpp
    ${PROJECT_SOURCE_DIR}/hub/src/hub_json.cpp
    ${PROJECT_SOURCE_DIR}/runtime/src/gbrt_hash.c)
target_include_directories(test_hub_edited_data PRIVATE
    ${PROJECT_SOURCE_DIR}/hub/src
    ${PROJECT_SOURCE_DIR}/runtime/include)
target_compile_features(test_hub_edited_data PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_hub_edited_data PRIVATE /utf-8)
endif()
add_test(NAME hub_edited_data_build_and_launch
         COMMAND test_hub_edited_data "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gb_data.py"
                 "$<TARGET_FILE:gbrecomp>" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_mapper_rom.py")

# Data that grows by moving: a game run with --data-use-pointers ends as it
# does without and records which two bytes made each address it read its ROM
# through, `gb_data.py pointers` makes of that the things that can move,
# `free` proposes the empty ends of banks, and a string made longer is in the
# image `build` makes, in declared free space with its pointers turned, and
# on the screen of the game given it. `build --move-all` proves the pointers.
add_test(NAME generated_data_pointers_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_pointers.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_data_pointers_end_to_end PROPERTIES TIMEOUT 300)

# From a ROM to a folder to mod it in: `tools/gb_workspace.py` generates and
# builds the game, records it played, tells the analysis what that showed and
# makes the game and the tree again, and leaves every view the data tools make
# with a README. Run again it does nothing, an edit survives every run and
# both splits, a run that stopped goes on from its step, and a charmap, save
# states, an unpackers declaration, a symbol file, a recorded run and lines
# for the analysis each turn their own steps on.
add_test(NAME generated_workspace_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_workspace.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_workspace_end_to_end PROPERTIES TIMEOUT 900)

# Lifted C as a source folder (docs/LIFTING.md, "Working in the source"): made by
# `gb_lift.py source`, edited, and built into the game by `gb_lift.py build`.
# A comment and a tidied body have to be kept and still match the ROM's code,
# a function changed on purpose has to stop the build until changes.txt says
# it is meant, and an update from another export has to keep what somebody
# changed and name what it found no place for.
add_test(NAME generated_lift_source_folder_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_source.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_source_folder_end_to_end PROPERTIES TIMEOUT 600)

# Functions of a game written by hand as ordinary C, with a binding in the
# place of each lifted function: the C is compiled into the checker and the
# game from where it is under src/, a byte of scratch RAM and an undefined
# register are let off only where the folder says so, an edit to the C has
# its bindings checked again, and the game still ends as the original does.
add_test(NAME generated_lift_by_hand_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_by_hand.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_by_hand_end_to_end PROPERTIES TIMEOUT 600)

# What each function takes, gives and keeps, worked out from the original
# and from who reads what after each call: a saved register is kept and not
# taken, a carry the next call adds in is a result, a function with a way in
# nobody can list gives everything it changes, and a flag that decides only
# how long a function takes is taken, found by the watching or, where that
# did not see it, by the function taking another time held to its line.
# `build` holds each function to its line, and the game run with
# --lift-signatures shows a line that calls a result left over, which the
# checker cannot.
add_test(NAME generated_lift_signatures_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_signatures.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_signatures_end_to_end PROPERTIES TIMEOUT 600)

# Data the source names: an operand that leads to something in the ROM goes
# by a name in a source folder made with --data-tree, and what it leads to
# can then be moved, with a header saying where to. A table another operand
# spells, one whose operand the game compares and one it jumps on from stay.
# The game built with the header plays the moved image as the original, and
# the game built without it does not.
add_test(NAME generated_lift_named_data_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_named_data.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_named_data_end_to_end PROPERTIES TIMEOUT 600)

# Lifted functions written as C with arguments by `gb_lift.py convert`, from
# what signatures.txt says each takes, gives and keeps: a function with two
# arguments and a result, calls written with arguments, a pair that is only
# saved, a jump into another function, a callee that returns past its
# caller, a jump through a register that is left alone. Every one is held
# to the ROM's code, and a callee whose line does not hold is not trusted.
add_test(NAME generated_lift_convert_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lift_convert.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>"
        ${GBRECOMP_GENERATED_CMAKE_ARGS})
set_tests_properties(generated_lift_convert_end_to_end PROPERTIES TIMEOUT 600)

# The commands of music and sound as text: `gb_data.py streams` follows the
# starts streams.json declares through commands in two grammars, a call, a
# jump, a loop and an end, and writes each region's passages as text. The
# tree unedited builds to the ROM, an edited note to the bytes meant, a
# longer passage is refused, and a byte no command has stops a stream with
# its address. Compiles no game.
add_test(NAME data_streams_as_text
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_data_streams.py"
        --gbrecomp "$<TARGET_FILE:gbrecomp>")
set_tests_properties(data_streams_as_text PROPERTIES TIMEOUT 300)
