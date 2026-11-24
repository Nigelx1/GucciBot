// Which Calculate band a window goes in (src/analysis/ac/band_pick.hpp),
// outside the game. Run by run_tests.bat. The cases are the Bands card's own
// rules (a whole band takes a sub-tick 1.3 as 1, a decimal edge holds the
// value as printed) and Nigel's D13: a sub-tick value in a gap between two
// bands counts in the band below.
#include "analysis/ac/band_pick.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace gucci::bandpick;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

// The band index for a value: `whole` as the markers pass it (the floored
// count, at least 1), `shown` already rounded to the printed places.
static int bandOf(std::vector<Edges> const& bands, double shown, int whole = -1) {
    if (whole < 0) {
        whole = static_cast<int>(std::floor(shown));
        if (whole < 1)
            whole = 1;
    }
    return pick(bands.size(), [&](size_t i) { return bands[i]; }, whole, shown);
}

int main() {
    int cases = 0;

    // GucciBot's default bands (shim.hpp): all whole, so a sub-tick value is
    // floored and nothing falls between them; past 10, none.
    {
        cases++;
        std::vector<Edges> const def = {{0, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 6}, {7, 8}, {9, 10}};
        CHECK(bandOf(def, 0.44) == 0);
        CHECK(bandOf(def, 1.0) == 0);
        CHECK(bandOf(def, 1.3) == 0);   // the Bands card's "a sub-tick 1.3 counts as 1"
        CHECK(bandOf(def, 2.0) == 1);
        CHECK(bandOf(def, 2.38) == 1);
        CHECK(bandOf(def, 2.99) == 1);
        CHECK(bandOf(def, 5.5) == 4);
        CHECK(bandOf(def, 6.7) == 4);
        CHECK(bandOf(def, 10.0) == 6);
        CHECK(bandOf(def, 10.4) == 6);  // floored to 10
        CHECK(bandOf(def, 11.0) == -1);
        CHECK(bandOf(def, 11.6) == -1);  // above the top band: still none
    }
    // The 11-18 test pass's bands (an edge at 1.5): 0-1.5 and 1.5-2, then the
    // whole 3. 2.02, 2.29 and 2.38 fell in no band; they count in 1.5-2 now.
    {
        cases++;
        std::vector<Edges> const b = {{0, 1.5f}, {1.5f, 2}, {3, 3}, {4, 4}, {5, 6}, {7, 8}, {9, 10}};
        CHECK(bandOf(b, 1.38) == 0);
        CHECK(bandOf(b, 1.5) == 0);  // the first band holding it, in the list's order
        CHECK(bandOf(b, 1.6) == 1);
        CHECK(bandOf(b, 2.0) == 1);
        CHECK(bandOf(b, 2.02) == 1);  // D13
        CHECK(bandOf(b, 2.29) == 1);  // D13
        CHECK(bandOf(b, 2.38) == 1);  // D13
        CHECK(bandOf(b, 2.99) == 1);  // D13
        CHECK(bandOf(b, 3.0) == 2);
        CHECK(bandOf(b, 3.7) == 2);   // a whole band floors it, as before
    }
    // The order of the list doesn't change which band is below.
    {
        cases++;
        std::vector<Edges> const b = {{3, 3}, {9, 10}, {1.5f, 2}, {0, 1.5f}};
        CHECK(bandOf(b, 2.38) == 2);
        CHECK(bandOf(b, 1.2) == 3);
    }
    // Sub-frame bands (0.3-0.5, 0.5-1) under whole ones: 1.3 counts in 0.5-1.
    {
        cases++;
        std::vector<Edges> const b = {{0.3f, 0.5f}, {0.5f, 1}, {2, 2}, {3, 3}};
        CHECK(bandOf(b, 0.3) == 0);
        CHECK(bandOf(b, 0.4) == 0);
        CHECK(bandOf(b, 0.5) == 0);
        CHECK(bandOf(b, 0.7) == 1);
        CHECK(bandOf(b, 1.0) == 1);
        CHECK(bandOf(b, 1.3) == 1);   // D13: between 0.5-1 and 2
        CHECK(bandOf(b, 1.99) == 1);  // D13
        CHECK(bandOf(b, 2.5) == 2);   // the whole 2 floors it
        CHECK(bandOf(b, 0.2) == -1);  // below the lowest band: still none
    }
    // A decimal top band: a value past it isn't between two bands. And the
    // first band holding a value wins, as before: the whole 2 would take 2.7
    // too (floored), but 2.5-3 comes first in this list.
    {
        cases++;
        std::vector<Edges> const b = {{2.5f, 3}, {2, 2}};
        CHECK(bandOf(b, 2.7) == 0);
        CHECK(bandOf(b, 2.2) == 1);   // the whole 2 floors it
        CHECK(bandOf(b, 3.0) == 0);
        CHECK(bandOf(b, 3.2) == -1);
    }
    // A whole window in a gap still lands in none: D13 is about sub-tick values.
    {
        cases++;
        std::vector<Edges> const b = {{1.5f, 1.9f}, {3, 4}};
        CHECK(bandOf(b, 2.0) == -1);
        CHECK(bandOf(b, 2.38) == 0);  // a sub-tick one counts in the band below
    }
    // Ties on the top: the first band in the list with that top.
    {
        cases++;
        std::vector<Edges> const b = {{1.2f, 2}, {1.5f, 2}, {3, 3}};
        CHECK(bandOf(b, 2.4) == 0);
        std::vector<Edges> const c = {{1.5f, 2}, {1.2f, 2}, {3, 3}};
        CHECK(bandOf(c, 2.4) == 0);
    }
    // An edge is a float: 0.3 is a hair off 0.3, and the slack lets it in.
    {
        cases++;
        std::vector<Edges> const b = {{0.3f, 0.5f}, {0.7f, 1}};
        CHECK(bandOf(b, 0.3) == 0);
        CHECK(bandOf(b, 0.5) == 0);
        CHECK(bandOf(b, 0.6) == 0);   // D13: between 0.3-0.5 and 0.7-1
        CHECK(bandOf(b, 0.7) == 1);
        CHECK(holds({0.3f, 0.5f}, 1, 0.3));
        CHECK(!holds({0.3f, 0.5f}, 1, 0.29));
    }
    // No bands at all.
    {
        cases++;
        std::vector<Edges> const none;
        CHECK(bandOf(none, 2.38) == -1);
    }

    if (fails) {
        std::printf("bandpick: %d check(s) failed in %d cases\n", fails, cases);
        return 1;
    }
    std::printf("bandpick: all %d cases pass\n", cases);
    return 0;
}
