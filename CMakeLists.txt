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
CPMAddPackage("gh:matcool/gd-imgui-cocos#97643336209b760a3d59d83b770546b1cfef4e7d")
CPMAddPackage("gh:cursey/safetyhook#main")
CPMAddPackage("gh:zyantific/zydis#bffbb61")
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
    safetyhook
    zlibstatic
    libGDR
    ws2_32
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    MOD_VERSION="2.alpha.1"
    NOMINMAX
    _HAS_ITERATOR_DEBUGGING=0
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /utf-8 /wd4201)
endif()

setup_geode_mod(${PROJECT_NAME})
