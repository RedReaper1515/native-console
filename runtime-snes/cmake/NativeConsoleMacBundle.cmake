# macOS application bundles for recompiled games.
#
# Generated projects include this file and call native_console_macos_bundle()
# after every other post-build step. With -DNATIVE_CONSOLE_MACOS_BUNDLE=ON the
# game becomes <target>.app on macOS: Info.plist, SDL2.framework copied into
# Contents/Frameworks, and an ad-hoc code signature (required on Apple Silicon
# once the bundle is modified). It is off by default because the hub and the
# tests run build/<target> directly; tools/macos/build_macos_app.sh turns it on.
#
# Configure with SDL2_DIR pointing into the official SDL2 release framework
# (SDL2.framework/Resources/CMake from SDL2-<version>.dmg). It is universal,
# so CMAKE_OSX_ARCHITECTURES="arm64;x86_64" builds a universal app. With any
# other SDL2 (Homebrew) the app links that SDL2 where it is installed and only
# runs on Macs that have it.
#
# runtime/cmake and runtime-gba/cmake hold identical copies of this file.

option(NATIVE_CONSOLE_MACOS_BUNDLE "Build macOS games as .app bundles" OFF)
set(NATIVE_CONSOLE_APP_NAME "" CACHE STRING "macOS app display name (default: the target name)")
set(NATIVE_CONSOLE_BUNDLE_ID "" CACHE STRING "macOS bundle identifier (default: io.nativeconsole.<target>)")

set(_native_console_bundle_dir "${CMAKE_CURRENT_LIST_DIR}")

# Finds SDL2.framework behind the SDL2::SDL2 target, or returns "".
function(_native_console_sdl2_framework out_var)
    set(candidates "${SDL2_DIR}")
    if(TARGET SDL2::SDL2)
        foreach(prop IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE INTERFACE_INCLUDE_DIRECTORIES INTERFACE_LINK_OPTIONS)
            get_target_property(value SDL2::SDL2 ${prop})
            if(value)
                list(APPEND candidates "${value}")
            endif()
        endforeach()
    endif()
    foreach(candidate IN LISTS candidates)
        string(REGEX MATCH "[^\";]*/SDL2\\.framework" framework "${candidate}")
        if(framework AND IS_DIRECTORY "${framework}")
            get_filename_component(framework "${framework}" REALPATH)
            set(${out_var} "${framework}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(native_console_macos_bundle target)
    if(NOT APPLE OR NOT NATIVE_CONSOLE_MACOS_BUNDLE)
        return()
    endif()
    set(app_name "${NATIVE_CONSOLE_APP_NAME}")
    if(NOT app_name)
        set(app_name "${target}")
    endif()
    set(bundle_id "${NATIVE_CONSOLE_BUNDLE_ID}")
    if(NOT bundle_id)
        string(REPLACE "_" "-" bundle_id "io.nativeconsole.${target}")
    endif()
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${_native_console_bundle_dir}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${app_name}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        BUILD_RPATH "@executable_path/../Frameworks")

    _native_console_sdl2_framework(sdl2_framework)
    set(frameworks "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
    if(sdl2_framework)
        message(STATUS "Native Console: bundling ${sdl2_framework} into ${target}.app")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND rm -rf "${frameworks}/SDL2.framework"
            COMMAND mkdir -p "${frameworks}"
            COMMAND ditto "${sdl2_framework}" "${frameworks}/SDL2.framework"
            VERBATIM)
    else()
        message(WARNING "Native Console: SDL2 is not the SDL2.framework from the official SDL2 .dmg; "
                        "${target}.app will use the SDL2 installed on this Mac and will not run on Macs without it.")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:${target}>"
        VERBATIM)
endfunction()
