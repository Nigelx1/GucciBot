#pragma once

// Possible positions (Hitborders card; NaN GD's idea, he isn't involved): a
// yellow band ahead of player 1 while a macro plays: where its center could
// go if one click at a time landed anywhere in its frame window, with every
// other click kept where the macro has it.
//
// It's an estimate from the copies of the player (analysis/trajectory.hpp,
// survivesScript, on Absense's simulation), not the real game: for each
// upcoming input within the look-ahead, one run from the real player now per
// timing of that input. The math is tools/positions_core; hitborders.cpp
// draws the result. Nothing runs, and no draw node is made, while the switch
// (HitborderOverlay::Settings::positions) is off.
//
// When it works: the switch on, hitborders shown, a macro playing, player 1
// alive in a started level, not platformer; never while Calculate, a
// pathfinder, the AI pilot or the judge has the run. Once a frame, after the
// frame's ticks (a CCScheduler::update hook, as the Survival Indicator's),
// one run at a time under a time budget.
//
// Main thread only.

#include "tools/positions_core.hpp"

#include <Geode/Geode.hpp>

#include <cstdint>
#include <vector>

namespace gucci::positions {

    // One input's band, worked out (or being worked out) this attempt.
    struct Entry {
        size_t action = 0;  // index in the macro's actions
        size_t input = 0;   // index in the player 1 jump inputs
        uint32_t frame = 0;
        bool press = true;
        Window window;
        std::vector<int> shifts;  // shift 0 first, then the rest
        size_t nextShift = 0;
        uint32_t horizon = 0;
        uint32_t nextFrame = 0;  // the next input's frame (0: none)
        std::vector<Run> runs;   // the counted runs, dropped once the lane is made
        int ran = 0;
        int counted = 0;
        int died = 0;
        int skipped = 0;  // too late to start, or onto a neighbor
        std::vector<int> countedShifts;  // which timings lived (for the readback)
        std::vector<int> diedShifts;
        bool nominalFailed = false;  // the macro as it is died in the copies: no band
        bool done = false;
        Lane lane;
    };

    // The entries of this attempt, in input order. Only done entries with a
    // lane are drawn.
    std::vector<Entry> const& entries();
    // Whether the band should be drawn now (the switch, the borders shown, no
    // Calculate run, player 1 alive and the level not ending).
    bool shouldDraw();
    // Everything dropped (the level going, the switch off).
    void clear();

    // hitborders.cpp, after it emitted the band for a frame (`ms`: the CPU
    // time that took).
    void noteDrawn(int quads, int segments, double ms);

    // Tunables for tests (gucci_test_set; not saved).
    float& budgetMs();
    float& lookaheadSec();
    bool& recordReal();

    // Assistant Access (gucci_positions): read only. `samples`: each lane's
    // samples too; from/to: only frames in that range (0: no bound).
    matjson::Value describe(bool samples, uint32_t from, uint32_t to);

} // namespace gucci::positions
