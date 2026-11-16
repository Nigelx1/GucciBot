#pragma once

// GucciBot (2026-11-15, the vsearch lane): the Pathfinder's "Show the search".
// While the game waits for a decision of the Search (Absense) engine, the
// paths of the copies it is trying are drawn faintly from where they start,
// its best idea so far brighter, and the corner box gets one line in plain
// words ("Deciding at 43.2%: 120 paths tried, the best lasts 1.4 s").
//
// It only reads: the positions of copies the search is stepping anyway
// (PathTap, filled by Trajectory::run) and the decision in progress
// (AbsensePathfinder::viewInfo). Nothing here is read back by the search, so
// it cannot change what is decided. Fixed sizes throughout: the last 32 paths
// of the decision in progress, at most 33 points each, and the best at most
// 96, so the drawing never grows.
//
// The switch and the line for the rest of the mod are in absense/glue.hpp
// (searchShown, setSearchShown, searchLine, searchViewJson, searchViewFrame).

#include <cstdint>

struct PathTap;

namespace absense::view {
    // The tap evaluate() hands its run when the view is on (null when it is
    // off), reset for this run. `decisionKey` names the decision in progress:
    // a new one empties the ring.
    PathTap* tapFor(uint64_t decisionKey);
    // That run is over: its path goes into the ring. `candidate` is the idea's
    // index in the decision (to find the best's path when the decision has no
    // samples of its own for it), `survived` its ticks.
    void keep(int candidate, int survived, bool complete);
}  // namespace absense::view
