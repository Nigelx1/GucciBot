#pragma once

// The rules of the Trainer's lane (trainers/lane.*), kept free of the game so
// the offline tests can check them (tools/run_lane_tests.cpp).
//
// The lane is Juice's design (2026-11-17, "basically like osu!taiko"): notes
// scroll into a fixed hit marker, and each note's look says how tight its
// timing window is. A note's tier comes from Calculate's count for that
// press (or release); a press is judged by how many ticks off the macro's
// frame it landed, against the shifts Calculate found still live.
// Written fresh for GucciBot.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace gucci::lanerules {

    // How a note looks. Unknown: no window counted for it.
    enum class Tier : uint8_t { Unknown, One, Two, Three, Wide };

    // A window in ticks (whole, or a CBF count under or between ticks) to its
    // tier. `isFree`: every shift Calculate tried lived.
    inline Tier tierFor(float windowTicks, bool isFree) {
        if (isFree)
            return Tier::Wide;
        if (!(windowTicks > 0.f))
            return Tier::Unknown;
        if (windowTicks < 1.5f)
            return Tier::One;
        if (windowTicks < 2.5f)
            return Tier::Two;
        if (windowTicks < 3.5f)
            return Tier::Three;
        return Tier::Wide;
    }

    // How many ticks early and late still land inside the window.
    struct Allow {
        int early = 0;
        int late = 0;
        bool known = false;  // a counted window; otherwise judged by the OK range
    };

    // low..high: the shifts Calculate found still live. Used when they hold
    // the macro's own frame and span the window; a window typed by hand, or
    // edges that don't, is taken as centered on the macro's frame, the extra
    // tick of an even window going late. A CBF count (subTick) under or
    // between ticks: only the macro's own tick.
    inline Allow allowance(float windowTicks, int low, int high, bool isFree, bool subTick) {
        Allow a;
        if (!(windowTicks > 0.f) && !isFree)
            return a;
        a.known = true;
        if (subTick && !isFree)
            return a;
        int const w = std::max(1, (int)std::lround(windowTicks));
        // The edges span every live shift, so never fewer ticks than the
        // count (more with holes in it).
        if (low <= 0 && high >= 0 && (isFree || high - low + 1 >= w)) {
            a.early = -low;
            a.late = high;
            return a;
        }
        a.early = (w - 1) / 2;
        a.late = w - 1 - a.early;
        return a;
    }

    enum class Verdict : uint8_t { Perfect, Inside, Outside, Missed };

    struct Judgment {
        Verdict verdict = Verdict::Missed;
        int delta = 0;  // ticks; negative early
    };

    // `okTicks`: how far off a note with no counted window may land and
    // still count (the Trainer's OK range, in ticks).
    inline Judgment judge(int delta, Allow const& a, int okTicks) {
        Judgment j;
        j.delta = delta;
        if (delta == 0)
            j.verdict = Verdict::Perfect;
        else if (a.known)
            j.verdict = (delta < 0 ? -delta <= a.early : delta <= a.late) ? Verdict::Inside : Verdict::Outside;
        else
            j.verdict = std::abs(delta) <= okTicks ? Verdict::Inside : Verdict::Outside;
        return j;
    }

    // "PERFECT", "EARLY 1f", "LATE 2f", "MISS".
    inline std::string label(Judgment const& j) {
        if (j.verdict == Verdict::Missed)
            return "MISS";
        if (j.delta == 0)
            return "PERFECT";
        return std::string(j.delta < 0 ? "EARLY " : "LATE ") + std::to_string(std::abs(j.delta)) + "f";
    }

    // Seconds off a target to whole ticks, rounded to the nearest.
    inline int ticksOff(double atSec, double targetSec, double tps) {
        return (int)std::lround((atSec - targetSec) * tps);
    }

    // The note a press at `t` answers: the nearest one in `times` (sorted)
    // not answered yet and within `reach` seconds. -1: none (a stray press,
    // which the lane leaves unjudged).
    inline int match(std::vector<double> const& times, std::vector<uint8_t> const& answered, double t, double reach) {
        auto const it = std::lower_bound(times.begin(), times.end(), t);
        int const n = (int)times.size();
        int best = -1;
        double bestD = reach;
        for (int i = (int)(it - times.begin()); i < n; ++i) {
            double const d = times[(size_t)i] - t;
            if (d > bestD)
                break;
            if (!answered[(size_t)i]) {
                best = i;
                bestD = d;
                break;
            }
        }
        for (int i = (int)(it - times.begin()) - 1; i >= 0; --i) {
            double const d = t - times[(size_t)i];
            if (d > bestD)
                break;
            if (!answered[(size_t)i]) {
                if (d < bestD || best < 0) {
                    best = i;
                    bestD = d;
                }
                break;
            }
        }
        return best;
    }

} // namespace gucci::lanerules
