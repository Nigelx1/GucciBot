// Offline tests for the Trainer lane's rules (src/trainers/lane_rules.hpp):
// a note's tier from its window, the early/late allowance from Calculate's
// edges, the judgment and its words, and which note a press answers.

#include "trainers/lane_rules.hpp"

#include <cstdio>

using namespace gucci::lanerules;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (cond) {                                                    \
            ++g_pass;                                                  \
        } else {                                                       \
            ++g_fail;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

int main() {
    // Tiers: Juice's 1 / 2 / 3 frames, then wide; nothing counted is unknown.
    CHECK(tierFor(0.f, false) == Tier::Unknown);
    CHECK(tierFor(1.f, false) == Tier::One);
    CHECK(tierFor(0.4f, false) == Tier::One);  // a CBF count under a tick
    CHECK(tierFor(2.f, false) == Tier::Two);
    CHECK(tierFor(3.f, false) == Tier::Three);
    CHECK(tierFor(4.f, false) == Tier::Wide);
    CHECK(tierFor(9.f, false) == Tier::Wide);
    CHECK(tierFor(0.f, true) == Tier::Wide);

    // Allowance from Calculate's edges.
    {
        Allow a = allowance(3.f, -1, 1, false, false);
        CHECK(a.known && a.early == 1 && a.late == 1);
        a = allowance(2.f, 0, 1, false, false);
        CHECK(a.known && a.early == 0 && a.late == 1);
        a = allowance(2.f, -1, 0, false, false);
        CHECK(a.early == 1 && a.late == 0);
        a = allowance(1.f, 0, 0, false, false);
        CHECK(a.early == 0 && a.late == 0);
        // Edges that don't hold the macro's frame: centered, extra tick late.
        a = allowance(4.f, 2, 5, false, false);
        CHECK(a.early == 1 && a.late == 2);
        // A hand-typed window with no edges.
        a = allowance(3.f, 0, 0, false, false);
        CHECK(a.early == 1 && a.late == 1);  // 0..0 can't hold 3 ticks: centered
        a = allowance(5.f, 3, 3, false, false);
        CHECK(a.early == 2 && a.late == 2);
        // CBF: only the tick itself.
        a = allowance(1.6f, -4, 3, false, true);
        CHECK(a.known && a.early == 0 && a.late == 0);
        // Nothing counted.
        a = allowance(0.f, 0, 0, false, false);
        CHECK(!a.known);
        // Free: the swept edges, however wide.
        a = allowance(0.f, -20, 20, true, false);
        CHECK(a.known && a.early == 20 && a.late == 20);
    }

    // Judgments.
    {
        Allow const a = allowance(3.f, -1, 1, false, false);
        CHECK(judge(0, a, 4).verdict == Verdict::Perfect);
        CHECK(judge(-1, a, 4).verdict == Verdict::Inside);
        CHECK(judge(1, a, 4).verdict == Verdict::Inside);
        CHECK(judge(2, a, 4).verdict == Verdict::Outside);
        CHECK(judge(-2, a, 4).verdict == Verdict::Outside);
        Allow const none;
        CHECK(judge(3, none, 4).verdict == Verdict::Inside);
        CHECK(judge(5, none, 4).verdict == Verdict::Outside);
        CHECK(label(judge(0, a, 4)) == "PERFECT");
        CHECK(label(judge(-1, a, 4)) == "EARLY 1f");
        CHECK(label(judge(2, a, 4)) == "LATE 2f");
        Judgment m;
        m.verdict = Verdict::Missed;
        CHECK(label(m) == "MISS");
    }

    // Ticks off.
    CHECK(ticksOff(1.0 + 1.0 / 240.0, 1.0, 240.0) == 1);
    CHECK(ticksOff(1.0 - 2.0 / 240.0, 1.0, 240.0) == -2);
    CHECK(ticksOff(1.0 + 0.4 / 240.0, 1.0, 240.0) == 0);

    // Matching.
    {
        std::vector<double> t{1.0, 1.1, 1.2, 2.0};
        std::vector<uint8_t> ans(4, 0);
        CHECK(match(t, ans, 1.01, 0.2) == 0);
        CHECK(match(t, ans, 1.08, 0.2) == 1);
        CHECK(match(t, ans, 1.5, 0.2) == -1);  // stray: nothing within reach
        ans[1] = 1;
        CHECK(match(t, ans, 1.1, 0.2) == 0 || match(t, ans, 1.1, 0.2) == 2);
        CHECK(match(t, ans, 1.12, 0.2) == 2);
        ans[0] = ans[2] = 1;
        CHECK(match(t, ans, 1.1, 0.2) == -1);
        CHECK(match(t, ans, 1.9, 0.2) == 3);
        CHECK(match(t, ans, 2.3, 0.2) == -1);
        std::vector<double> empty;
        std::vector<uint8_t> none;
        CHECK(match(empty, none, 1.0, 0.2) == -1);
    }

    std::printf("lane tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
