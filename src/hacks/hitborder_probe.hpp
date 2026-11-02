#pragma once

// gucci_hitborder_probe (Assistant Access): Hitborders checked against GD
// itself, with nobody at the controls (the research note's "Verification
// without Nigel").
//
// For every object near player 1, or in a range of x, the regions the overlay
// draws (hacks/hitborders.cpp, read exactly as paint() reads them: every kind
// on, blocks Exact) are sampled a hair inside and a hair outside every drawn
// edge (tools/hitborder_geom edgeSamples). At each point a hidden copy of the
// player (Absense's, put back to the same state before every point: see
// Trajectory::probeCollisions) stands still and GD's own collision pass runs
// once. What the pass did to the copy because of that object - killed it,
// touched it (a block, a slope), found it touching it (an orb, pad, portal or
// trigger) - is held against what the drawing promises at that point.
//
// A point counts only where GD tested the object with the copy where it was
// put. When the pass moved the copy first (the ground, a landing on another
// block, a slope) and the object gave no sign, the point is inconclusive, not
// a pass or a fail. Orbs, pads, portals and triggers are kept from acting:
// the probe only asks whether they were touched.
//
// Copies only: the real game is put back as it was, and nothing is saved.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>

namespace gucci {

    // The kinds of object, for HitborderProbeArgs::kinds and the report.
    enum class HitborderProbeKind : uint8_t { Spike, Saw, Block, Slope, HazardSlope, Orb, Pad, Portal, Other };
    constexpr int kHitborderProbeKinds = 9;
    const char* hitborderProbeKindName(HitborderProbeKind kind);
    // Words to bits ("spikes, blocks"; "slopes" takes the hazard slopes too):
    // false, and the word it didn't know in `bad`, on an unknown one.
    bool hitborderProbeKinds(std::string const& text, uint32_t& bits, std::string& bad);

    struct HitborderProbeArgs {
        // The player the copy stands in for. -1: player 1's own. The kind in
        // Trajectory::FakeMode numbering (0 cube, 1 ship, 2 ball, 3 ufo,
        // 4 wave, 5 robot, 6 spider, 7 swing).
        int mode = -1;
        int mini = -1;
        int upsideDown = -1;
        float rotation = 0.0f;  // the icon's, in degrees (only the rotated tests use it)
        float dt = -1.0f;       // the pass's dt in GD's sixtieths; below 0, one tick's at the level's rate
        // Which objects: those whose rect (or circle) comes within `radius`
        // of player 1, or, with xFrom < xTo, those reaching into [xFrom, xTo]
        // (and [yFrom, yTo] when yFrom < yTo).
        float radius = 90.0f;
        float xFrom = 0.0f;
        float xTo = 0.0f;
        float yFrom = 0.0f;
        float yTo = 0.0f;
        int uid = 0;            // only this object (the whole level, unless a range is given)
        uint32_t kinds = ~0u;   // a bit per HitborderProbeKind
        double eps = 0.01;      // how far to either side of an edge, in level units
        int perEdge = 3;        // points along each straight edge (a side of a circle gets one, where it touches)
        int maxPoints = 8000;   // collision passes, at most (two a sampled point)
        int maxList = 40;       // mismatches listed
        double maxMs = 4000.0;  // wall clock, at most
    };

    // {"error": why} when it can't run.
    matjson::Value hitborderProbe(HitborderProbeArgs const& args);

} // namespace gucci
