cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Both Mac architectures, as Geode's mod template does, so an Intel Mac can
# load the macOS binary (CI's was arm64 only). It has to be set before
# project(): the SDK's own default comes after the mod's target exists, too
# late. iOS is arm64. Windows and Android ignore it.
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(GucciBot VERSION 2.0.0 LANGUAGES CXX)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    third_party/ffmpeg/include
)

if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable.")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Geode's bindings, pinned. Without this the SDK clones bindings' main on every
# configure, and main moves under us: on 2026-10-06 a GJGameState rename made
# it uncopyable, which GucciBot's practice checkpoints rely on, and every
# local build stopped compiling. CI pins the same commit (bindings-ref in
# .github/workflows/multi-platform.yml); move both together. A
# GEODE_BINDINGS_REPO_PATH environment variable still wins (CI's action sets
# it), for working on bindings.
#
# ONE codebase, TWO builds (Nigel, 2026-11-16): the same tree builds against
# Geode 5 (GD 2.2081, what 2.0 ships; GEODE_SDK=C:\GeodeSDK) and Geode 6 (GD
# 2.2091; GEODE_SDK pointing at a Geode 6 SDK). A .geode loads on only one
# Geode major, so each SDK gives its own file. The SDK's VERSION file says
# which one this is; the code itself switches on GEODE_COMP_GD_VERSION
# (src/core/geode_compat.hpp).
file(READ "$ENV{GEODE_SDK}/VERSION" GB_GEODE_SDK_VERSION)
string(STRIP "${GB_GEODE_SDK_VERSION}" GB_GEODE_SDK_VERSION)
string(REGEX MATCH "^[0-9]+" GB_GEODE_MAJOR "${GB_GEODE_SDK_VERSION}")
if(GB_GEODE_MAJOR GREATER_EQUAL 6)
    set(GB_GEODE6 ON)
    message(STATUS "GucciBot: Geode ${GB_GEODE_SDK_VERSION} build (GD 2.2091)")
    # Geode 6 builds only Windows so far: iOS's 2.2091 bindings miss 42 of
    # GucciBot's hooks, and the phone ImGui patches aren't redone for the
    # newer gd-imgui-cocos yet (docs/PORT_2209_ASSESSMENT.md).
    if(NOT WIN32)
        message(FATAL_ERROR "GucciBot's Geode 6 (GD 2.2091) build is Windows only for now")
    endif()
    # The bindings commit Geode 6.0.0-beta.1 came out with: the last on main
    # before its release (2026-11-20 23:03 UTC). It was alpha.2's 7a670e9
    # until then; Geode's beta loader refuses a mod built for an alpha.
    set(GB_BINDINGS_COMMIT 78d5bb4eb0732dd636ae16d4dcf6e5ce84ec1221)
    # Its codegen keeps a renamed member's old name as a union with the new
    # one, and a union holding a CCPoint can't be copied, so GJGameState
    # couldn't be either (practice checkpoints and the path preview copy it).
    # The patch keeps old names only for scalars and pointers; same layout.
    set(GB_BINDINGS_PATCH PATCH_COMMAND ${CMAKE_COMMAND}
        "-DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/bindings-g6-copyable-renames.patch"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply_once.cmake")
else()
    set(GB_GEODE6 OFF)
    set(GB_BINDINGS_COMMIT d641858f3e1cd2c6d86013c799f224ebdb8ec1cd)
    set(GB_BINDINGS_PATCH "")
endif()
if(NOT DEFINED ENV{GEODE_BINDINGS_REPO_PATH})
    include(FetchContent)
    FetchContent_Declare(gb_bindings
        GIT_REPOSITORY https://github.com/geode-sdk/bindings.git
        GIT_TAG ${GB_BINDINGS_COMMIT}
        ${GB_BINDINGS_PATCH}
        SOURCE_SUBDIR no-cmake-here  # fetch only: the SDK adds it as a subdirectory itself
    )
    FetchContent_MakeAvailable(gb_bindings)
    set(GEODE_BINDINGS_REPO_PATH ${gb_bindings_SOURCE_DIR})
endif()

# Codegen, built from the pinned bindings above instead of downloaded. The
# download is bindings' rolling "codegen" release, which follows bindings main,
# not the pin: by 2026-10-19 it had moved to the next Geode's layout (headers
# that include Geode/mapping/...), which our SDK doesn't have, and every build,
# local and CI, stopped compiling. Built from the pinned commit it always
# matches the pin. Costs a minute the first time.
set(SKIP_BUILDING_CODEGEN OFF)

add_subdirectory($ENV{GEODE_SDK} $ENV{GEODE_SDK}/build)

set(IMGUI_VERSION "v1.91.4" CACHE STRING "Pin imgui version" FORCE)
# On a phone or tablet the menu is drawn bigger as a whole:
# imgui-cocos-frame-scale.patch lets ImGui lay out a display smaller than the
# GL frame and scales its drawing and touches to match (src/ui/look.cpp sets
# the scale). Android and iOS only; Windows and macOS fetch it as before.
if (GB_GEODE6)
    # The first gd-imgui-cocos for Geode 6 and GD 2.2091 (2026-11-03).
    CPMAddPackage("gh:matcool/gd-imgui-cocos#934958c68b1f050a8976d893584fa71368c1df1f")
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The keyboard modify header doesn't compile for iOS; this version already
    # leaves the keyboard hook out there but still includes the header.
    CPMAddPackage(
        NAME gd-imgui-cocos
        GITHUB_REPOSITORY matcool/gd-imgui-cocos
        GIT_TAG 97643336209b760a3d59d83b770546b1cfef4e7d
        PATCHES
            "${CMAKE_CURRENT_SOURCE_DIR}/patches/imgui-cocos-ios-keyboard.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/patches/imgui-cocos-frame-scale.patch"
    )
elseif (ANDROID)
    CPMAddPackage(
        NAME gd-imgui-cocos
        GITHUB_REPOSITORY matcool/gd-imgui-cocos
        GIT_TAG 97643336209b760a3d59d83b770546b1cfef4e7d
        PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/imgui-cocos-frame-scale.patch"
    )
else()
    CPMAddPackage("gh:matcool/gd-imgui-cocos#97643336209b760a3d59d83b770546b1cfef4e7d")
endif()
# SafetyHook (and Zydis under it) places the engine's midhooks at fixed
# addresses in the Windows build of GD: x86-64 Windows only. No other platform
# builds them (src/core/platform.hpp).
if (WIN32)
    # Pinned to the commit main pointed at on 2026-11-17 (unchanged since
    # 2026-06-20), the one CI and the local builds already used, so an
    # upstream change can't alter the release build.
    CPMAddPackage("gh:cursey/safetyhook#f44cc070a8340f2f26649553c49533475417304d")
    CPMAddPackage("gh:zyantific/zydis#bffbb61")
endif()
CPMAddPackage(
    NAME ZLIB
    GITHUB_REPOSITORY madler/zlib
    VERSION 1.3.1
)
CPMAddPackage("gh:maxnut/GDReplayFormat#9c32b5529067901418ea6e4dfbd36437dc8712bc")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_BINARY_DIR}/_deps/zlib-src
    ${CMAKE_BINARY_DIR}/_deps/zlib-build
)

# The MCP server is the one file that needs winsock2.h before windows.h, and
# Geode's precompiled header force-includes Geode.hpp (and so windows.h, and
# so winsock 1.1) ahead of line 1 of every source file. Opting this one file
# out of the PCH is the only way its own include order gets to matter.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp/mcp_server.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(${PROJECT_NAME}
    imgui-cocos
    zlibstatic
    libGDR
)
if (WIN32)
    # The midhooks, and Winsock for Assistant Access.
    target_link_libraries(${PROJECT_NAME} safetyhook ws2_32)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    MOD_VERSION="2.0.0"
    NOMINMAX
    _HAS_ITERATOR_DEBUGGING=0
    # ImGui's test-engine hooks, for Assistant Access's menu probe
    # (src/ui/probe.hpp, which defines them). imgui-cocos is an INTERFACE
    # library, so ImGui's own sources compile inside this target and see it
    # too. ImGui calls the hooks only while GImGui->TestEngineHookItems is on,
    # which the probe sets for the one frame it captures.
    IMGUI_ENABLE_TEST_ENGINE
)

# TEST ONLY (2026-11-15): -DGB_FORCE_PORTABLE=ON builds Windows with the
# off-Windows engine (GB_NATIVE_ENGINE 0, core/platform.hpp: GJBaseGameLayer::
# processCommands ticking, portable TPS and checkpoint placement, the World
# off), so a hidden Windows test game can exercise what macOS and Android run.
# Off by default; CI, build_win.bat and releases never set it, and without it
# nothing here changes. Build it in its own folder (tools/build_portable.py).
option(GB_FORCE_PORTABLE "TEST ONLY: build Windows with the off-Windows engine" OFF)
if(GB_FORCE_PORTABLE)
    if(NOT WIN32)
        message(FATAL_ERROR "GB_FORCE_PORTABLE is a Windows test build")
    endif()
    message(WARNING "GB_FORCE_PORTABLE: TEST build with the off-Windows engine. Never ship it.")
    target_compile_definitions(${PROJECT_NAME} PRIVATE GB_FORCE_PORTABLE=1)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /utf-8 /wd4201)
endif()

if(GB_GEODE6)
    # Geode checks mod.json's "geode" and "gd" against the SDK, so the Geode 6
    # build packages a copy made here from the one mod.json: the same file
    # with Geode 6's version, GD 2.2091 for the platforms it builds, and the
    # resource paths made absolute (they point back into this tree). The copy
    # and its logo and about.md live in the build folder; the source tree stays
    # the Geode 5 one. setup_geode_mod reads mod.json from
    # CMAKE_CURRENT_SOURCE_DIR, so that points at the copy for the one call.
    set(GB_MOD_ROOT "${CMAKE_BINARY_DIR}/mod-geode6")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" GB_MOD_JSON)
    string(JSON GB_MOD_JSON SET "${GB_MOD_JSON}" geode "\"${GEODE_VERSION_FULL}\"")
    string(JSON GB_MOD_JSON SET "${GB_MOD_JSON}" gd "{\"win\": \"${GEODE_GD_VERSION}\"}")
    string(JSON GB_RES_COUNT LENGTH "${GB_MOD_JSON}" resources files)
    math(EXPR GB_RES_LAST "${GB_RES_COUNT} - 1")
    foreach(i RANGE ${GB_RES_LAST})
        string(JSON GB_RES GET "${GB_MOD_JSON}" resources files ${i})
        string(JSON GB_MOD_JSON SET "${GB_MOD_JSON}" resources files ${i}
            "\"${CMAKE_CURRENT_SOURCE_DIR}/${GB_RES}\"")
    endforeach()
    file(WRITE "${GB_MOD_ROOT}/mod.json" "${GB_MOD_JSON}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/logo.png" "${GB_MOD_ROOT}/logo.png" COPYONLY)
    # Geode shows about.md on the mod's page, so the 2.2091 file's copy says
    # which GD and Geode it's for and what this preview leaves out (no
    # Pathfinder; 240 and 162 TPS and up work, but not under 162, a TPS change
    # inside a macro or High TPS precision). The two lines that name 2.2081 are
    # swapped word for word. If the requirements line was reworded, it's still found by
    # its start ("- Geometry Dash 2.2081") and replaced whole. Any other
    # 2.2081 left after that becomes the 2.2091 version number, with a CMake
    # warning naming the line: a release-day edit to about.md must not stop
    # the 2.2091 build (it runs on every push to master), but the warning says
    # to give that line a proper swap here.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/about.md" GB_ABOUT)
    string(REPLACE
        "- Geometry Dash 2.2081 on Windows, macOS, iOS or Android, Geode 5.10.1+ (macOS, iOS and Android are experimental: see Platforms)"
        "- Geometry Dash ${GEODE_GD_VERSION} on Windows, with Geode ${GEODE_VERSION_FULL}. This file is a preview of GucciBot on GD ${GEODE_GD_VERSION}: everything except the Pathfinder. 240 TPS and any tick rate from 162 TPS up work; tick rates under 162 TPS, a TPS change inside a macro and High TPS precision don't yet. Those, and macOS and Android on GD ${GEODE_GD_VERSION}, come in 2.0.1. For another GD version or platform, use the GucciBot file made for it.\n- Keep only one GucciBot file in your mods folder."
        GB_ABOUT "${GB_ABOUT}")
    string(REPLACE
        "- Silicate's full physics engine — exact GD 2.2081 offsets, proper TPS bypass,"
        "- Silicate's physics engine — GD ${GEODE_GD_VERSION} offsets from Silicate 1.1.2, at 240 TPS or 162 TPS and up in this preview,"
        GB_ABOUT "${GB_ABOUT}")
    # The requirements line, reworded after the version: matched by its start.
    # (A newline goes in front so the first line can match too; it comes off
    # again below.)
    set(GB_ABOUT "\n${GB_ABOUT}")
    string(REGEX REPLACE "\n- Geometry Dash 2[.]2081[^\n]*"
        "\n- Geometry Dash ${GEODE_GD_VERSION} on Windows, with Geode ${GEODE_VERSION_FULL}. This file is a preview of GucciBot on GD ${GEODE_GD_VERSION}: everything except the Pathfinder. 240 TPS and any tick rate from 162 TPS up work; tick rates under 162 TPS, a TPS change inside a macro and High TPS precision don't yet. Those, and macOS and Android on GD ${GEODE_GD_VERSION}, come in 2.0.1. For another GD version or platform, use the GucciBot file made for it.\n- Keep only one GucciBot file in your mods folder."
        GB_ABOUT "${GB_ABOUT}")
    string(SUBSTRING "${GB_ABOUT}" 1 -1 GB_ABOUT)
    # Anything else that still names 2.2081: the version number is swapped and
    # the line is named, so the page never names the wrong GD.
    string(REGEX MATCHALL "[^\n]*2[.]2081[^\n]*" GB_ABOUT_LEFT "${GB_ABOUT}")
    if(GB_ABOUT_LEFT)
        foreach(GB_LINE IN LISTS GB_ABOUT_LEFT)
            message(WARNING "about.md names 2.2081 in a line the Geode 6 build has no swap for; "
                "it now says ${GEODE_GD_VERSION} there, word for word otherwise. Check the line reads right "
                "for the 2.2091 file and give it a swap in CMakeLists.txt: ${GB_LINE}")
        endforeach()
        string(REPLACE "2.2081" "${GEODE_GD_VERSION}" GB_ABOUT "${GB_ABOUT}")
    endif()
    file(WRITE "${GB_MOD_ROOT}/about.md" "${GB_ABOUT}")
    set(GB_REAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CMAKE_CURRENT_SOURCE_DIR "${GB_MOD_ROOT}")
    setup_geode_mod(${PROJECT_NAME})
    set(CMAKE_CURRENT_SOURCE_DIR "${GB_REAL_SOURCE_DIR}")
else()
    setup_geode_mod(${PROJECT_NAME})
endif()
