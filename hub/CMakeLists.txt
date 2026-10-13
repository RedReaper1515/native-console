# Native Console Hub: library, import/build pipeline, and launcher.
# Built from the root project when SDL2 is available (BUILD_HUB=ON).

set(HUB_IMGUI_DIR "${PROJECT_SOURCE_DIR}/runtime/vendor/imgui")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/HubVoxelProfiles.cmake)
hub_generate_voxel_profiles("${CMAKE_CURRENT_BINARY_DIR}/hub_builtin_voxel_profiles.cpp")

add_executable(native_console_hub WIN32
    src/hub_app.cpp
    src/hub_consoles.cpp
    src/hub_edited_data.cpp
    src/console_gameboy.cpp
    src/console_gba.cpp
    src/console_snes.cpp
    src/console_nes.cpp
    src/console_nds.cpp
    src/console_n64.cpp
    src/console_gc.cpp
    src/hub_export.cpp
    src/hub_job_history.cpp
    src/hub_json.cpp
    src/hub_library.cpp
    src/hub_pipeline.cpp
    src/hub_platform_common.cpp
    src/hub_platform_win.cpp
    src/hub_platform_posix.cpp
    src/hub_settings.cpp
    src/hub_steam.cpp
    src/hub_steam_vdf.cpp
    src/hub_texture_packs.cpp
    src/hub_voxel_profiles.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/hub_builtin_voxel_profiles.cpp
    ${PROJECT_SOURCE_DIR}/runtime/src/gbrt_hash.c
    ${HUB_IMGUI_DIR}/imgui.cpp
    ${HUB_IMGUI_DIR}/imgui_draw.cpp
    ${HUB_IMGUI_DIR}/imgui_tables.cpp
    ${HUB_IMGUI_DIR}/imgui_widgets.cpp
    ${HUB_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${HUB_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

# Families whose code is kept outside this repository (src/hub_extra.h): a
# folder with a hub_extra.cmake that adds their sources to the hub. Without
# one the hub has the families of this repository and no others.
set(NATIVE_CONSOLE_HUB_EXTRA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extra" CACHE PATH
    "Folder of hub console families kept outside the repository")
if(EXISTS "${NATIVE_CONSOLE_HUB_EXTRA_DIR}/hub_extra.cmake")
    message(STATUS "Hub: with the families in ${NATIVE_CONSOLE_HUB_EXTRA_DIR}")
    include("${NATIVE_CONSOLE_HUB_EXTRA_DIR}/hub_extra.cmake")
else()
    target_sources(native_console_hub PRIVATE src/hub_extra_none.cpp)
endif()

target_include_directories(native_console_hub PRIVATE
    src
    ${HUB_IMGUI_DIR}
    ${PROJECT_SOURCE_DIR}/runtime/include
)
target_compile_features(native_console_hub PRIVATE cxx_std_20 c_std_11)
target_link_libraries(native_console_hub PRIVATE SDL2::SDL2)
file(TO_CMAKE_PATH "${SDL2_DIR}" HUB_SDL2_DIR_FORWARD)
target_compile_definitions(native_console_hub PRIVATE
    HUB_VERSION="${PROJECT_VERSION}"
    HUB_SDL2_DIR="${HUB_SDL2_DIR_FORWARD}"
)

if(WIN32)
    # Application icon (Explorer, taskbar, shortcuts); the window icon is set at runtime from it.
    target_sources(native_console_hub PRIVATE assets/hub.rc)
    target_link_libraries(native_console_hub PRIVATE ole32 shell32 windowscodecs advapi32)
    target_compile_definitions(native_console_hub PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(native_console_hub PRIVATE /utf-8)
    endif()
    # Ship SDL2.dll next to the hub so it runs from the build tree.
    if(TARGET SDL2::SDL2)
        get_target_property(_hub_sdl2_type SDL2::SDL2 TYPE)
        if(_hub_sdl2_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET native_console_hub POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:native_console_hub>"
                COMMENT "Copying SDL2.dll next to native_console_hub")
        endif()
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(native_console_hub PRIVATE Threads::Threads)
endif()

install(TARGETS native_console_hub RUNTIME DESTINATION bin)
