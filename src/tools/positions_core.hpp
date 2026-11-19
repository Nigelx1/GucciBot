#pragma once

// Possible positions (hitborders' yellow band; NaN GD's idea, he isn't
// involved): the math, with nothing of the game in it, so the offline suite
// (tools/run_positions_tests.cpp) can check it. hacks/positions.cpp runs the
// copies and hands their paths here; hacks/hitborders.cpp draws what comes
// back.
//
// The idea: for one input of the macro, the copies of player 1 are run with
// that input moved to each timing in its window and every other input where
// the macro has it. Where the player's center can be at each tick, over every
// timing that lives, is the band. It widens after the input, stays wide while
// the paths run apart (through later clicks too, since those are kept), and
// pinches to a line where the paths meet again on a surface.
//
// Frames are absolute, as everywhere in GucciBot: a copy started at frame
// `now` is at frame now + k + 1 after its k-th tick (index k of its path).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace gucci::positions {

    // One player 1 jump input of the macro: its frame and whether it presses.
    struct Input {
        uint32_t frame = 0;
        bool press = true;
    };

    // ---------------------------------------------------------------- the window

    // A window in ticks, relative to the input's frame. 0 is always in it.
    struct Window {
        int low = -4;
        int high = 4;
        bool fromMark = false;  // Calculate's counted window; else the default
    };

    inline constexpr int kDefaultReach = 4;
    inline constexpr int kMaxShifts = 13;

    // Calculate's mark as a window in whole ticks. `cbf`: low and high are in
    // sub-tick slots, `slotsPerTick` of them a tick; they go to ticks toward
    // zero (the copies only play inputs on tick edges). A mark that can't be
    // used (`usable` false: none, typed by hand, desynced or hidden) gives
    // the default -4..+4.
    Window windowFor(bool usable, int64_t low, int64_t high, bool cbf, double slotsPerTick);

    // The shifts to run: every tick in the window, or at most `maxShifts` of
    // them spread evenly, low, high and 0 always among them. Sorted, no
    // repeats.
    std::vector<int> pickShifts(Window const& w, int maxShifts = kMaxShifts);

    // Whether input `i` can be moved by `shift` with the run starting at
    // frame `now`: it still lands after now (offset 1 or more), and stays
    // strictly between the inputs either side of it.
    bool shiftAllowed(std::vector<Input> const& inputs, size_t i, int shift, uint32_t now);

    // ---------------------------------------------------------------- the run

    // The last frame a run for input `i` goes to: the next input plus
    // `pastNext` ticks, never more than `cap` ticks after the input itself
    // (with no next input, the input plus `pastNext`, capped the same).
    uint32_t horizonFor(std::vector<Input> const& inputs, size_t i, int pastNext, int cap);

    // The script for one run from frame `now` to `horizon`: (tick offset,
    // hold) for every input after now up to the horizon, input `i` moved by
    // `shift`, sorted by offset (stable, so two inputs on one tick keep the
    // macro's order). Offsets count as GucciBot's frames do: frame now + k is
    // offset k.
    std::vector<std::pair<int, bool>> scriptFor(std::vector<Input> const& inputs, size_t i, int shift,
                                                uint32_t now, uint32_t horizon);

    // How much of a run counts: how many of its path points (from the start)
    // are possible positions, or -1 when none are. `lived` is how many ticks
    // the copy lived (survivesScript's answer, `ticks` when it lived through),
    // `nextFrame` the next input's frame (0: none). A run that lives to the
    // horizon counts whole. One that dies after the next input counts up to
    // the next input: past it, that timing leads to a death. One that dies
    // before then doesn't count.
    int countedPoints(int lived, int ticks, uint32_t now, uint32_t nextFrame);

    // ---------------------------------------------------------------- the envelope

    struct Point {
        float x = 0.f;
        float y = 0.f;
    };

    // One run's path: the copy's center after each tick, the first at frame
    // `firstFrame`.
    struct Run {
        int shift = 0;
        uint32_t firstFrame = 0;
        std::vector<Point> points;
    };

    struct Sample {
        float x = 0.f;
        float yMin = 0.f;
        float yMax = 0.f;
        float width() const { return yMax - yMin; }
    };

    // Where the center can be, frame by frame: samples[k] is frame
    // startFrame + k.
    struct Lane {
        uint32_t startFrame = 0;
        std::vector<Sample> samples;
        float x0 = 0.f, x1 = 0.f, y0 = 0.f, y1 = 0.f;  // bounds, for culling
        int runs = 0;      // runs it was made from, the nominal among them
        int xBreaks = 0;   // runs that stopped counting where their x left the nominal's
        float maxWidth = 0.f;
        bool empty() const { return samples.empty(); }
        uint32_t endFrame() const { return startFrame + static_cast<uint32_t>(samples.size()); }  // one past
    };

    struct EnvelopeOptions {
        float minWidth = 0.05f;  // narrower than this is no band at the ends
        float maxXGap = 1.f;     // a run further than this from the nominal's x stops counting
    };

    // The envelope over `runs`, along the nominal's frames (runs[nominal] is
    // shift 0, the macro as it is). Each other run counts at the frames it
    // has, until its x leaves the nominal's. Leading and trailing frames
    // narrower than minWidth are cut, one frame kept each side; a narrow
    // stretch between two wide ones stays (the pinch). Empty when nothing is
    // wider than minWidth anywhere.
    Lane envelope(std::vector<Run> const& runs, size_t nominal, EnvelopeOptions const& opt = {});

    // ---------------------------------------------------------------- drawing

    // The samples of [from, to) closer together in x than `minStep`, folded
    // into the last one kept: each kept sample's yMin and yMax hold the ones
    // it stands for, so thinning never narrows the band. The first and last
    // are always kept.
    std::vector<Sample> thin(std::vector<Sample> const& s, size_t from, size_t to, float minStep);

    // The samples whose x is within [x0, x1], with one more each side so the
    // edges reach off screen: [from, to). from == to when none.
    std::pair<size_t, size_t> visibleRange(std::vector<Sample> const& s, float x0, float x1);

    // One trapezoid of fill between two samples: two vertical sides, so always
    // convex. a, b on the bottom (left, right), c, d on the top (right, left).
    struct Quad {
        Point a, b, c, d;
    };

    // The fill between consecutive samples, each side inset by `inset` in y
    // (cocos grows a borderless polygon by half a unit), skipped where both
    // sides are narrower than `minFill`; a side that the inset would turn
    // inside out goes to its middle.
    std::vector<Quad> fillQuads(std::vector<Sample> const& s, float inset, float minFill);

} // namespace gucci::positions
