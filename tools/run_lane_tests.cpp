// Offline tests for the Trainer lane's rules (src/trainers/lane_rules.hpp):
// a note's tier from its window, the early/late allowance from Calculate's
// edges, the judgment and its words, which note a press answers, and the
// Delay setting (the drawing runs ahead; the judging doesn't move).

#include "trainers/lane_rules.hpp"

#include <cmath>
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

    // The Delay setting: ms to ticks at the lane's rate, never rounded.
    {
        auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
        CHECK(near(delayTicks(100.0, 240.0), 24.0));
        CHECK(near(delayTicks(100.0, 120.0), 12.0));
        CHECK(near(delayTicks(-100.0, 240.0), -24.0));
        CHECK(near(delayTicks(200.0, 360.0), 72.0));
        CHECK(near(delayTicks(1.0, 240.0), 0.24));  // a fraction of a tick stays a fraction
        CHECK(near(delayTicks(0.0, 240.0), 0.0));
        CHECK(near(delayTicks(100.0, 0.0), 24.0));  // no rate yet: 240
        // The drawing's clock is D ms of level time ahead, at any tick rate.
        for (double tps : {30.0, 60.0, 120.0, 240.0, 360.0, 1000.0})
            for (double ms : {-100.0, -7.5, 0.0, 1.0, 16.7, 100.0, 200.0})
                CHECK(near(drawClock(12.345, ms, tps) - 12.345, ms / 1000.0));
        // Every note moves by the same D ms of scroll, and 1 ms (0.24 tick at
        // 240) moves it a little, not a whole tick or nothing.
        double const px = 1200.0 / (300.0 / 240.0);  // 1200 px of lane for a 300-tick lead
        for (double note : {10.0, 10.5, 11.2}) {
            double const x0 = xAt(note, drawClock(10.0, 0.0, 240.0), 100.0, px);
            double const x100 = xAt(note, drawClock(10.0, 100.0, 240.0), 100.0, px);
            CHECK(std::fabs((x0 - x100) - 0.1 * px) < 1e-6);
            double const x1 = xAt(note, drawClock(10.0, 1.0, 240.0), 100.0, px);
            CHECK(x0 - x1 > 0.0 && x0 - x1 < px / 240.0);
        }
        // A note at its own time is on the marker at Delay 0, and Delay D
        // puts it there D ms of level time sooner.
        CHECK(std::fabs(xAt(10.0, drawClock(10.0, 0.0, 240.0), 100.0, px) - 100.0) < 1e-6);
        CHECK(std::fabs(xAt(10.0, drawClock(10.0 - 0.1, 100.0, 240.0), 100.0, px) - 100.0) < 1e-6);
        // The setting's range, and a damaged save.
        CHECK(clampDelayMs(-500.0) == kDelayMinMs && clampDelayMs(500.0) == kDelayMaxMs);
        CHECK(clampDelayMs(37.0) == 37.f);
        CHECK(clampDelayMs(std::nan("")) == 0.f);
    }

    // Verdicts go by the tick the game took the press on: judge() and
    // ticksOff() take no delay at all. This checks what the setting is for: a
    // player whose screen shows the game lagMs late presses when a note looks
    // on the marker, and the game takes the press on the next tick. Delay =
    // lag lands it on the note's tick; Delay 0 with that lag lands it late;
    // one press on one tick gets one verdict whatever the drawing did.
    {
        double const tps = 240.0;
        Allow const a = allowance(3.f, -1, 1, false, false);
        auto pressTick = [&](double noteSec, double delayMs, double lagMs) {
            // The note looks on the marker when drawClock(now) == noteSec, so
            // at game time noteSec - delay, seen lagMs later.
            double const seen = noteSec - delayMs / 1000.0 + lagMs / 1000.0;
            return std::ceil(seen * tps - 1e-6) / tps;  // the tick the game takes it on
        };
        for (double note : {1.0, 2.5, 7.0 + 3.0 / 240.0}) {
            CHECK(judge(ticksOff(pressTick(note, 0.0, 0.0), note, tps), a, 4).verdict == Verdict::Perfect);
            CHECK(judge(ticksOff(pressTick(note, 100.0, 100.0), note, tps), a, 4).verdict == Verdict::Perfect);
            Judgment const late = judge(ticksOff(pressTick(note, 0.0, 100.0), note, tps), a, 4);
            CHECK(late.verdict == Verdict::Outside && late.delta == 24 && label(late) == "LATE 24f");
            Judgment const early = judge(ticksOff(pressTick(note, 100.0, 0.0), note, tps), a, 4);
            CHECK(early.verdict == Verdict::Outside && early.delta == -24);
            double const at = note + 1.0 / tps;
            Judgment const j = judge(ticksOff(at, note, tps), a, 4);
            CHECK(j.verdict == Verdict::Inside && j.delta == 1);
        }
    }

    std::printf("lane tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
