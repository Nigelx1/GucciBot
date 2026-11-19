// Offline tests for Possible positions' math (src/tools/positions_core.hpp):
// the window from Calculate's mark, the shifts picked from it, which shifts
// are allowed, the horizon, the script a run gets, which part of a run counts,
// the envelope (apex cap, pinch on a landing, wide through a later click,
// trimmed ends, x breaks), thinning that never narrows, culling and the fill.

#include "tools/positions_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace gucci::positions;

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

static bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// A cube-ish jump: flat at y0 until `jumpAt`, then a parabola back down to y0,
// where it stays (a landing). Frames from `first`, x = 10 per frame.
static Run jumpRun(int shift, uint32_t first, int count, int jumpAt, float y0 = 100.f) {
    Run r;
    r.shift = shift;
    r.firstFrame = first;
    for (int k = 0; k < count; ++k) {
        float const x = 10.f * static_cast<float>(first + static_cast<uint32_t>(k));
        int const t = static_cast<int>(first) + k - (jumpAt + shift);
        float y = y0;
        if (t > 0 && t < 20)
            y = y0 + static_cast<float>(t * (20 - t));  // up and back down in 20 frames
        r.points.push_back({x, y});
    }
    return r;
}

// A wave-ish path: a straight line, the slope flipping at each click.
static Run waveRun(int shift, uint32_t first, int count, std::vector<int> clicks) {
    Run r;
    r.shift = shift;
    r.firstFrame = first;
    float y = 200.f;
    float dy = 2.f;
    size_t next = 0;
    for (int k = 0; k < count; ++k) {
        int const f = static_cast<int>(first) + k;
        while (next < clicks.size() && clicks[next] <= f) {
            dy = -dy;
            next++;
        }
        y += dy;
        r.points.push_back({10.f * static_cast<float>(f), y});
    }
    return r;
}

int main() {
    // ---- the window
    {
        Window w = windowFor(false, -9, 9, false, 0);
        CHECK(w.low == -kDefaultReach && w.high == kDefaultReach && !w.fromMark);
        w = windowFor(true, -2, 3, false, 0);
        CHECK(w.low == -2 && w.high == 3 && w.fromMark);
        // Sub-tick slots, 100 a tick, toward zero.
        w = windowFor(true, -250, 180, true, 100.0);
        CHECK(w.low == -2 && w.high == 1 && w.fromMark);
        // A window that doesn't hold 0 still runs the macro as it is.
        w = windowFor(true, 1, 3, false, 0);
        CHECK(w.low == 0 && w.high == 3);
        // A sub-tick mark with no rate is no mark.
        w = windowFor(true, -250, 180, true, 0.0);
        CHECK(!w.fromMark && w.low == -4);
        // Inside out: no mark.
        w = windowFor(true, 3, -3, false, 0);
        CHECK(!w.fromMark);
        // Far: clamped.
        w = windowFor(true, -100000, 100000, false, 0);
        CHECK(w.low == -240 && w.high == 240);
    }

    // ---- the shifts
    {
        auto s = pickShifts({-4, 4, false});
        CHECK(s.size() == 9 && s.front() == -4 && s.back() == 4);
        s = pickShifts({-2, 0, true});
        CHECK(s.size() == 3 && s[0] == -2 && s[2] == 0);
        s = pickShifts({-30, 10, true}, 13);
        CHECK(s.size() <= 13 && s.front() == -30 && s.back() == 10);
        CHECK(std::binary_search(s.begin(), s.end(), 0));
        CHECK(std::is_sorted(s.begin(), s.end()));
        CHECK(std::adjacent_find(s.begin(), s.end()) == s.end());
        s = pickShifts({-7, 50, true}, 5);
        CHECK(s.size() <= 5 && s.front() == -7 && s.back() == 50 && std::binary_search(s.begin(), s.end(), 0));
    }

    // ---- allowed shifts
    std::vector<Input> in = {{100, true}, {110, false}, {130, true}, {131, false}};
    {
        CHECK(shiftAllowed(in, 1, 0, 50));
        CHECK(shiftAllowed(in, 1, -9, 50));
        CHECK(!shiftAllowed(in, 1, -10, 50));  // onto the press before it
        CHECK(shiftAllowed(in, 1, 19, 50));
        CHECK(!shiftAllowed(in, 1, 20, 50));   // onto the press after it
        CHECK(!shiftAllowed(in, 1, -5, 105));  // already past
        CHECK(!shiftAllowed(in, 1, -4, 106));  // lands on now: no tick left for it
        CHECK(shiftAllowed(in, 1, -3, 106));
        CHECK(shiftAllowed(in, 0, -60, 30));   // the first input has nothing before it
        CHECK(!shiftAllowed(in, 2, 1, 50));    // 131 is the release after it
        CHECK(shiftAllowed(in, 3, 40, 50));    // the last has nothing after it
        CHECK(!shiftAllowed(in, 9, 0, 50));
    }

    // ---- the horizon
    {
        CHECK(horizonFor(in, 0, 240, 480) == 110 + 240);
        CHECK(horizonFor(in, 1, 240, 480) == 130 + 240);
        CHECK(horizonFor(in, 1, 240, 100) == 110 + 100);  // capped from the input
        CHECK(horizonFor(in, 3, 240, 480) == 131 + 240);  // no next: the input plus
        CHECK(horizonFor(in, 4, 240, 480) == 0);
    }

    // ---- the script
    {
        auto sc = scriptFor(in, 1, 0, 90, 200);
        CHECK(sc.size() == 4);
        CHECK(sc[0].first == 10 && sc[0].second);
        CHECK(sc[1].first == 20 && !sc[1].second);
        CHECK(sc[3].first == 41 && !sc[3].second);
        sc = scriptFor(in, 1, -3, 90, 200);
        CHECK(sc[1].first == 17 && !sc[1].second);
        // Moved past the press after it would reorder: the script sorts.
        sc = scriptFor(in, 0, 15, 90, 200);
        CHECK(sc[0].first == 20 && !sc[0].second);
        CHECK(sc[1].first == 25 && sc[1].second);
        // Only what's after now and up to the horizon.
        sc = scriptFor(in, 2, 0, 105, 130);
        CHECK(sc.size() == 2 && sc[0].first == 5 && sc[1].first == 25);
    }

    // ---- what counts
    {
        CHECK(countedPoints(300, 300, 100, 150) == 300);  // lived through
        CHECK(countedPoints(300, 300, 100, 0) == 300);
        CHECK(countedPoints(120, 300, 100, 150) == 50);   // died after the next input: up to it
        CHECK(countedPoints(50, 300, 100, 150) == 50);    // lived exactly to it
        CHECK(countedPoints(49, 300, 100, 150) == -1);    // died before it
        CHECK(countedPoints(120, 300, 100, 0) == -1);     // no next input: the horizon or nothing
        CHECK(countedPoints(120, 300, 100, 500) == -1);   // the next input past the run's end
        CHECK(countedPoints(-1, 300, 100, 150) == -1);    // no copy ran
        CHECK(countedPoints(0, 0, 100, 150) == -1);
    }

    // ---- the envelope: a cube jump, timings -3..+3
    {
        std::vector<Run> runs;
        for (int s = -3; s <= 3; ++s)
            runs.push_back(jumpRun(s, 1000, 80, 1020));
        size_t const nominal = 3;
        CHECK(runs[nominal].shift == 0);
        Lane lane = envelope(runs, nominal);
        CHECK(!lane.empty());
        CHECK(lane.runs == 7 && lane.xBreaks == 0);
        // It starts one frame before the first wide frame: the earliest timing
        // leaves the ground at 1017 + 1.
        CHECK(lane.startFrame == 1017);
        CHECK(near(lane.samples.front().width(), 0.f));
        // The nominal lies inside everywhere.
        bool inside = true;
        for (size_t k = 0; k < lane.samples.size(); ++k) {
            uint32_t const f = lane.startFrame + static_cast<uint32_t>(k);
            float const y = runs[nominal].points[f - 1000].y;
            inside &= y >= lane.samples[k].yMin - 1e-4f && y <= lane.samples[k].yMax + 1e-4f;
            inside &= near(lane.samples[k].x, 10.f * static_cast<float>(f));
        }
        CHECK(inside);
        // The cap: at the nominal's apex (frame 1030), the top is the apex (100), above
        // both edge timings (-3 and +3, at t = 13 and 7: 91 each).
        size_t const apex = 1030 - lane.startFrame;
        CHECK(near(lane.samples[apex].yMax, 200.f));
        CHECK(runs[0].points[30].y < 200.f - 1.f && runs[6].points[30].y < 200.f - 1.f);
        // The last timing lands at 1043: the lane ends one frame after that.
        CHECK(lane.endFrame() == 1044);
        CHECK(near(lane.samples.back().width(), 0.f));
        CHECK(lane.maxWidth > 50.f);
        CHECK(lane.x0 <= lane.x1 && lane.y0 <= lane.y1);
    }

    // ---- a pinch between two wide stretches stays
    {
        // Two jumps, timings -2..+2: every timing has landed by 1032, then
        // they part again at the second jump (moved with the first here).
        std::vector<Run> runs;
        for (int s = -2; s <= 2; ++s) {
            Run a = jumpRun(s, 1000, 100, 1010);
            Run b = jumpRun(s, 1000, 100, 1050);
            for (size_t k = 0; k < a.points.size(); ++k)
                a.points[k].y = std::max(a.points[k].y, b.points[k].y);
            runs.push_back(a);
        }
        Lane lane = envelope(runs, 2);
        CHECK(!lane.empty());
        // Between the two jumps (all landed by 1032, the next leaves at 1048+1)
        // the width is 0 and the lane goes on through it.
        size_t const mid = 1040 - lane.startFrame;
        CHECK(mid < lane.samples.size());
        CHECK(near(lane.samples[mid].width(), 0.f));
        CHECK(lane.endFrame() > 1060);
    }

    // ---- wide through a later click (a wave: the click after it is kept on its tick)
    {
        std::vector<Run> runs;
        for (int s = -2; s <= 2; ++s)
            runs.push_back(waveRun(s, 1000, 60, {1010 + s, 1030}));
        Lane lane = envelope(runs, 2);
        CHECK(!lane.empty());
        // After 1012 every timing has turned; the width (2 ticks either way, 4 a
        // tick of slope... 8 units of y either side) stays through 1030's click.
        size_t const before = 1029 - lane.startFrame;
        size_t const after = 1035 - lane.startFrame;
        CHECK(after < lane.samples.size());
        CHECK(lane.samples[before].width() > 10.f);
        CHECK(near(lane.samples[before].width(), lane.samples[after].width(), 1e-3f));
        CHECK(lane.endFrame() == 1060);  // the nominal's end: never narrows again
    }

    // ---- runs that stop early count where they have samples
    {
        std::vector<Run> runs;
        runs.push_back(jumpRun(-2, 1000, 30, 1010));  // cut at the next input (1029)
        runs.push_back(jumpRun(0, 1000, 80, 1010));
        runs.push_back(jumpRun(2, 1005, 75, 1010));   // started later (a later frame's run)
        Lane lane = envelope(runs, 1);
        CHECK(!lane.empty());
        CHECK(lane.runs == 3);
        // At 1030 only the nominal (landed, 100) and +2 (136) are there.
        size_t const k = 1030 - lane.startFrame;
        CHECK(k < lane.samples.size());
        CHECK(near(lane.samples[k].yMin, 100.f) && near(lane.samples[k].yMax, 136.f));
    }

    // ---- an x break: that run stops counting there
    {
        std::vector<Run> runs;
        runs.push_back(jumpRun(0, 1000, 60, 1010));
        Run off = jumpRun(3, 1000, 60, 1010);
        for (size_t k = 25; k < off.points.size(); ++k)
            off.points[k].x += 30.f;  // missed a portal
        runs.push_back(off);
        Lane lane = envelope(runs, 0);
        CHECK(lane.xBreaks == 1);
        CHECK(!lane.empty());
        CHECK(lane.endFrame() <= 1026);
    }

    // ---- nothing wide: no lane; no nominal: no lane
    {
        std::vector<Run> runs;
        runs.push_back(jumpRun(0, 1000, 60, 1010));
        runs.push_back(jumpRun(0, 1000, 60, 1010));
        CHECK(envelope(runs, 0).empty());
        CHECK(envelope(runs, 5).empty());
        std::vector<Run> none;
        CHECK(envelope(none, 0).empty());
    }

    // ---- thinning never narrows
    {
        std::vector<Sample> s;
        for (int k = 0; k < 100; ++k)
            s.push_back({0.25f * static_cast<float>(k), -static_cast<float>(k % 7), static_cast<float>(k % 5)});
        auto t = thin(s, 0, s.size(), 1.f);
        CHECK(t.size() < s.size() && t.size() >= 24);
        CHECK(near(t.front().x, 0.f) && near(t.back().x, s.back().x));
        float lo = 0.f, hi = 0.f, tlo = 0.f, thi = 0.f;
        for (auto const& v : s) {
            lo = std::min(lo, v.yMin);
            hi = std::max(hi, v.yMax);
        }
        for (auto const& v : t) {
            tlo = std::min(tlo, v.yMin);
            thi = std::max(thi, v.yMax);
        }
        CHECK(near(lo, tlo) && near(hi, thi));
        auto all = thin(s, 0, s.size(), 0.f);
        CHECK(all.size() == s.size());
        CHECK(thin(s, 50, 40, 1.f).empty());
        CHECK(thin(s, 98, 500, 1.f).size() == 2);
    }

    // ---- culling
    {
        std::vector<Sample> s;
        for (int k = 0; k < 10; ++k)
            s.push_back({10.f * static_cast<float>(k), 0.f, 1.f});
        auto r = visibleRange(s, 25.f, 55.f);
        CHECK(r.first == 2 && r.second == 7);  // 30..50 and one each side
        r = visibleRange(s, -100.f, 1000.f);
        CHECK(r.first == 0 && r.second == 10);
        r = visibleRange(s, 200.f, 300.f);
        CHECK(r.first == r.second);
    }

    // ---- fill
    {
        std::vector<Sample> s = {{0.f, 0.f, 0.f}, {1.f, 0.f, 10.f}, {2.f, 5.f, 5.2f}, {3.f, 5.f, 5.f}};
        auto q = fillQuads(s, 0.5f, 1.f);
        CHECK(q.size() == 2);  // the last pair is narrow on both sides
        CHECK(near(q[0].a.y, 0.f) && near(q[0].d.y, 0.f));  // the zero side goes to its middle
        CHECK(near(q[0].b.y, 0.5f) && near(q[0].c.y, 9.5f));
        CHECK(near(q[1].b.y, 5.1f) && near(q[1].c.y, 5.1f));  // inside out at 0.2 wide: the middle
        CHECK(near(q[0].a.x, 0.f) && near(q[0].b.x, 1.f) && near(q[0].c.x, 1.f) && near(q[0].d.x, 0.f));
        CHECK(fillQuads({}, 0.5f, 1.f).empty());
    }

    std::printf("positions tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
