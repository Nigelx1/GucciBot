// Runs the frame editor's tests outside Geometry Dash.
//
// The same cases run at startup inside the game (see src/tools/selfcheck.hpp)
// and land in the Diagnostics panel. This file exists so they can also be run
// on a change, in a second, without launching GD -- which is the difference
// between tests that get checked and tests that get remembered about.
//
// Run it with .\run_tests.bat -- that sets up the MSVC environment and does
// the same thing as:
//
//   cl /std:c++20 /EHsc /I src /DGB_EDIT_CORE_TEST_STANDALONE ^
//      tools\run_edit_tests.cpp /Fe:build\run_edit_tests.exe /Fo:build\
//   build\run_edit_tests.exe
//
// Exits non-zero if anything fails, so it can gate a commit.

#define GB_EDIT_CORE_TEST_STANDALONE 1
#include "tools/edit_core_test.hpp"

#include <cstdio>

int main() {
    int passed = 0;
    int failed = 0;

    gucci::editcoretest::runCases([&](char const* name, bool ok) {
        if (ok) {
            passed++;
        } else {
            failed++;
            std::printf("FAIL  %s\n", name);
        }
    });

    if (failed == 0)
        std::printf("frame editor rules: %d cases, all passed\n", passed);
    else
        std::printf("frame editor rules: %d of %d cases FAILED\n", failed, passed + failed);
    return failed == 0 ? 0 : 1;
}
