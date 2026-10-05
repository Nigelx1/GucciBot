cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

add_subdirectory($ENV{GEODE_SDK} $ENV{GEODE_SDK}/build)

set(IMGUI_VERSION "v1.91.4" CACHE STRING "Pin imgui version" FORCE)
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The keyboard modify header doesn't compile for iOS; this version already
    # leaves the keyboard hook out there but still includes the header.
    CPMAddPackage(
        NAME gd-imgui-cocos
        GITHUB_REPOSITORY matcool/gd-imgui-cocos
        GIT_TAG 97643336209b760a3d59d83b770546b1cfef4e7d
        PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/imgui-cocos-ios-keyboard.patch"
    )
else()
    CPMAddPackage("gh:matcool/gd-imgui-cocos#97643336209b760a3d59d83b770546b1cfef4e7d")
endif()
# SafetyHook (and Zydis under it) places the engine's midhooks at fixed
# addresses in the Windows build of GD: x86-64 Windows only. No other platform
# builds them (src/core/platform.hpp).
if (WIN32)
    CPMAddPackage("gh:cursey/safetyhook#main")
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
    MOD_VERSION="2.0.0-beta.3"
    NOMINMAX
    _HAS_ITERATOR_DEBUGGING=0
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /utf-8 /wd4201)
endif()

setup_geode_mod(${PROJECT_NAME})
