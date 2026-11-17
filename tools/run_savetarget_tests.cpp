// Save's rule (src/tools/save_target.hpp), outside the game: when a Save
// writes at once and when it asks, and the name "Save as" offers. Run by
// run_tests.bat.
#include "tools/save_target.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace gucci::savetarget;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

int main() {
    int cases = 0;

    // A name no file has: written at once, whatever the macro came from.
    cases++;
    CHECK(decide(false, false) == Decision::Write);
    CHECK(decide(false, true) == Decision::Write);

    // The macro's own file (carried on recording from it, edited, saved
    // again): written over, no question.
    cases++;
    CHECK(decide(true, true) == Decision::Write);

    // Another macro's file (a fresh recording under the old name, or a typed
    // name that's taken): asks.
    cases++;
    CHECK(decide(true, false) == Decision::Ask);

    // Save as: the next free number, checked against every extension (the
    // set stands in for "any theme's file has this name").
    {
        cases++;
        std::set<std::string> files = {"zz"};
        auto taken = [&](std::string const& n) { return files.count(n) > 0; };
        CHECK(nextFreeName("zz", taken) == "zz 2");
        files.insert("zz 2");
        CHECK(nextFreeName("zz", taken) == "zz 3");
        files.insert("zz 4");  // a gap is used
        CHECK(nextFreeName("zz", taken) == "zz 3");
        files.insert("zz 3");
        CHECK(nextFreeName("zz", taken) == "zz 5");
    }
    {
        cases++;
        // A name that ends in a number gets one more on the end, not a
        // changed number: "Level 2" is a level's name, not a copy.
        std::set<std::string> files = {"Level 2"};
        auto taken = [&](std::string const& n) { return files.count(n) > 0; };
        CHECK(nextFreeName("Level 2", taken) == "Level 2 2");
    }

    if (fails) {
        std::printf("save target tests: %d failure(s) in %d case(s)\n", fails, cases);
        return 1;
    }
    std::printf("save target tests: all %d case(s) passed\n", cases);
    return 0;
}
