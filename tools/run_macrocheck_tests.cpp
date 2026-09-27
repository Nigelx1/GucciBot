// Macro problem check (src/tools/macro_check.hpp), outside the game. Run by
// run_tests.bat.
#include "tools/macro_check.hpp"

#include <cstdio>

using namespace gucci;
using namespace gucci::macrocheck;
using T = gb::ActionType;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

static gb::Action in(uint32_t f, bool press, bool p2 = false, T t = T::Jump) {
    gb::Action a;
    a.m_frame = f;
    a.m_type = t;
    a.m_holding = press;
    a.m_player2 = p2;
    return a;
}

static gb::Action ev(uint32_t f, T t, double tps = 0.0) {
    gb::Action a;
    a.m_frame = f;
    a.m_type = t;
    a.m_tps = tps;
    return a;
}

int main() {
    int cases = 0;

    // a clean macro: two clicks, one per player, nothing to report
    {
        cases++;
        auto r = check({in(10, true), in(20, false), in(15, true, true), in(25, false, true)});
        // 15 < 20 is out of order -- keep it sorted for the clean case
        auto r2 = check({in(10, true), in(15, true, true), in(20, false), in(25, false, true)});
        CHECK(!r.ok());
        CHECK(r2.ok() && r2.clicks == 2 && r2.heldToEnd == 0);
    }
    // a press while already held in that lane
    {
        cases++;
        auto r = check({in(10, true), in(12, true), in(20, false)});
        CHECK(r.findings.size() == 1 && r.findings[0].problem == Problem::DoublePress &&
              r.findings[0].frame == 12 && r.findings[0].lane == 0);
    }
    // a release with nothing held
    {
        cases++;
        auto r = check({in(10, false)});
        CHECK(r.findings.size() == 1 && r.findings[0].problem == Problem::OrphanRelease);
    }
    // lanes are separate: p1 jump held does not make a p2 press a double
    {
        cases++;
        auto r = check({in(10, true), in(11, true, true), in(20, false), in(21, false, true)});
        CHECK(r.ok() && r.clicks == 2);
    }
    // left/right are their own lanes
    {
        cases++;
        auto r = check({in(10, true, false, T::Left), in(11, true, false, T::Right),
                        in(20, false, false, T::Left), in(21, false, false, T::Right)});
        CHECK(r.ok() && r.clicks == 2);
    }
    // a death cuts the hold; the release after it is noted, not a problem
    {
        cases++;
        auto r = check({in(10, true), ev(15, T::Death), in(16, false)});
        CHECK(r.ok() && r.cutByReset == 1 && r.releaseAfterReset == 1 && r.clicks == 0);
    }
    // a hold still open at the end is noted, not a problem
    {
        cases++;
        auto r = check({in(10, true)});
        CHECK(r.ok() && r.heldToEnd == 1);
    }
    // out of order
    {
        cases++;
        auto r = check({in(20, true), in(10, false)});
        bool sawUnsorted = false;
        for (auto const& f : r.findings)
            sawUnsorted |= f.problem == Problem::Unsorted && f.index == 1;
        CHECK(sawUnsorted);
    }
    // bad TPS values
    {
        cases++;
        auto r = check({ev(0, T::TPS, 240.0), ev(5, T::TPS, 0.0), ev(6, T::TPS, -1.0)});
        CHECK(r.findings.size() == 2 && r.findings[0].problem == Problem::BadValue);
    }
    // an unknown type
    {
        cases++;
        gb::Action bad;
        bad.m_frame = 3;
        bad.m_type = static_cast<T>(99);
        auto r = check({bad});
        CHECK(r.findings.size() == 1 && r.findings[0].problem == Problem::BadValue);
    }
    // a restart also cuts holds, in every lane
    {
        cases++;
        auto r = check({in(10, true), in(10, true, true), ev(12, T::RestartFull)});
        CHECK(r.ok() && r.cutByReset == 2 && r.heldToEnd == 0);
    }
    // an empty macro is fine
    {
        cases++;
        auto r = check({});
        CHECK(r.ok() && r.clicks == 0);
    }

    std::printf(fails ? "macro check: %d case(s), %d FAILED\n" : "macro check: %d cases, all passed\n",
                cases, fails);
    return fails ? 1 : 0;
}
