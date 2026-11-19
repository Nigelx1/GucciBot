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

    // ---- the Delay setting (both styles)
    //
    // The drawing runs Delay ms ahead of the game so the notes arrive that
    // much early, for a screen or setup that lags. The lane's clock is the
    // level's ticks over its tick rate, so D ms are D x tps / 1000 ticks
    // (fractional: the drawing moves smoothly, it never snaps to a tick),
    // which is D / 1000 s on that clock at any tick rate. Judging takes no
    // delay: a press is judged by the tick the game took it on (judge,
    // ticksOff), whatever the drawing shows.
    constexpr float kDelayMinMs = -100.f;
    constexpr float kDelayMaxMs = 200.f;

    inline float clampDelayMs(double ms) {
        if (!(ms == ms))  // NaN from a damaged save
            return 0.f;
        return (float)std::clamp(ms, (double)kDelayMinMs, (double)kDelayMaxMs);
    }

    // The delay in ticks at `tps`, not rounded.
    inline double delayTicks(double ms, double tps) {
        return ms * (tps > 0.0 ? tps : 240.0) / 1000.0;
    }

    // The clock the drawing uses: the game's clock `nowSec` plus the delay.
    inline double drawClock(double nowSec, double ms, double tps) {
        double const rate = tps > 0.0 ? tps : 240.0;
        return nowSec + delayTicks(ms, rate) / rate;
    }

    // Where a moment `t` sits on a lane scrolling at pxPerSec, its marker at
    // markX, drawn at `drawSec` (drawClock).
    inline double xAt(double t, double drawSec, double markX, double pxPerSec) {
        return markX + (t - drawSec) * pxPerSec;
    }

    // The note a press at `t` answers: the nearest one in `times` (sorted)
    // that `open(i)` allows and within `reach` seconds. -1: none (a stray
    // press, which the lane leaves unjudged).
    template <class Open>
    inline int matchIf(std::vector<double> const& times, double t, double reach, Open&& open) {
        auto const it = std::lower_bound(times.begin(), times.end(), t);
        int const n = (int)times.size();
        int best = -1;
        double bestD = reach;
        for (int i = (int)(it - times.begin()); i < n; ++i) {
            double const d = times[(size_t)i] - t;
            if (d > bestD)
                break;
            if (open((size_t)i)) {
                best = i;
                bestD = d;
                break;
            }
        }
        for (int i = (int)(it - times.begin()) - 1; i >= 0; --i) {
            double const d = t - times[(size_t)i];
            if (d > bestD)
                break;
            if (open((size_t)i)) {
                if (d < bestD || best < 0) {
                    best = i;
                    bestD = d;
                }
                break;
            }
        }
        return best;
    }

    // Any note not answered yet.
    inline int match(std::vector<double> const& times, std::vector<uint8_t> const& answered, double t, double reach) {
        return matchIf(times, t, reach, [&](size_t i) { return !answered[i]; });
    }

    // In a 2-player level a press answers only its own player's notes:
    // those whose player flag (`flag2[i]`) is `want2`, the flag of the
    // player that pressed. Elsewhere any note: in a 1-player level a click
    // moves player 1, whatever flag its notes carry.
    inline int matchPlayer(std::vector<double> const& times, std::vector<uint8_t> const& answered,
                           std::vector<uint8_t> const& flag2, bool twoPlayer, bool want2, double t,
                           double reach) {
        if (!twoPlayer || flag2.size() != times.size())
            return match(times, answered, t, reach);
        return matchIf(times, t, reach, [&](size_t i) { return !answered[i] && (flag2[i] != 0) == want2; });
    }

    // Which counted window a macro click's press (or release) takes. The
    // windows file keys a window by frame, press or release, and player, not
    // by button. `ownButtons`: how many different buttons of this player
    // press (or release) on that frame (two clicks of one button there, a
    // tap on another's release tick, share its window). `otherClicks`: how
    // many of the other player's clicks do, leaving out this click's exact
    // twin (same press, release and button: a 1-player macro's double-flag
    // copy). `ownWindow` / `otherWindow`: whether a window is counted there
    // under each player. Its own player's window first. The other player's
    // only when that player has no click of its own there (a 1-player
    // level's dual fixes count under the other flag); in a 2-player level it
    // belongs to that player's click. A jump and a Left or Right of one
    // player on one frame take none: the file doesn't say whose it is.
    enum class WindowPick : uint8_t { None, Own, Other };

    inline WindowPick pickWindow(int ownButtons, int otherClicks, bool ownWindow, bool otherWindow) {
        if (ownButtons != 1)
            return WindowPick::None;
        if (ownWindow)
            return WindowPick::Own;
        if (otherWindow && otherClicks == 0)
            return WindowPick::Other;
        return WindowPick::None;
    }

} // namespace gucci::lanerules
