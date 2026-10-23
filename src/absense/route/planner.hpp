#pragma once

// The route planner: step R2 of AI_PATHFINDER_PLAN.md (design in
// docs/AI_PATHFINDER_RESEARCH.md, section 2, "How it decides"). On the level
// map (atlas.hpp) it picks a route through the whole level: at each gate, the
// gap to be in. Between two gates it follows the map's gaps column by column,
// within how far the mode can climb or fall over a column, and prices the way
// by its length, how tight it is, and how much of it the map cannot vouch for
// (moving parts, things it cannot follow). The cheapest few routes are kept,
// best first.
//
// Step 4 of docs/ROUTE_ENGINE_PLAN.md: what the route does at each gate is one
// of the gate's options (use or skip an orb, touch or avoid a pad or portal,
// fire a toggle or not, or anything, FREE, where the gate offers nothing
// else). Gates in one column are one step of the route, their options
// together. Gravity is carried along: a gravity orb used, a pad or a portal
// changes which way the mode climbs and falls after it. And a pass backward
// over the gates prices the cheapest way from every option to the level's
// end (the cost to go), so the next gate's options can be ranked from any
// state (nextChoices), not only along a route planned once from the start.
//
// It decides; it does not play. Physics (steps R3 and on) turns each stretch
// into exact clicks and says what is impossible, and the planner chooses again.
// A way the map thinks the mode cannot make is priced very high, never ruled
// out: only physics may say that.

#include <string>
#include <vector>

#include "absense/route/atlas.hpp"

class GJBaseGameLayer;
class PlayLayer;

namespace absense::route {

    struct Waypoint {
        int column = 0;
        float x = 0.0f;
        Opening band;       // the gap the route is in at this gate (one of the column's openings)
        double step = 0.0;  // what getting here from the gate before cost
        // Step 4: the option it takes here. The option's own band lies inside
        // `band` (an orb's contact region, the gap above it...); a FREE
        // option's is the whole gap. The first waypoint, where the plan
        // starts, has none (gate -1, a FREE option at the start height).
        int gate = -1;    // index into Atlas::gates
        int choice = -1;  // index into that gate's choices
        Option option;
        int gravity = 0;  // the gravity the route has after it: 0 normal, 1 upside down
    };

    struct Route {
        std::vector<Waypoint> points;  // from the start to the level's end
        double cost = 0.0;
    };

    struct Plan {
        float fromX = 0.0f;  // where it was planned from
        float fromY = 0.0f;
        int fromGravity = 0;        // ... with this gravity (0 normal, 1 upside down)
        std::vector<Route> routes;  // the cheapest first
        int layers = 0;             // gates on the way (one per column with gates)
        int edges = 0;              // gate-to-gate ways priced
        double ms = 0.0;
    };

    // How many routes are kept: the best and the next ones ready.
    constexpr int kRoutesKept = 3;

    // The plan from (x, y), with that gravity, to the end of the level the
    // map is of.
    Plan plan(const Atlas& atlas, float fromX, float fromY, int fromGravity = 0);

    // The plan the overlay shows: made from the player's position the first
    // time it is drawn on a level (cached until the map changes).
    const Plan* shownPlan(GJBaseGameLayer* pl);

    std::string summary(const Plan& p);

    // Step 4: the next gate's options from a state - the first column of
    // gates past the one x is in - each priced as the way there from (x, y)
    // plus the cheapest way from it to the level's end (the cost to go). Best
    // first; ties: use, touch and fire before skip and avoid, then the lower
    // band. FREE comes last (it is what is tried when the named options fail).
    // `gravity`: 0 normal, 1 upside down, -1 player 1's now. Empty past the
    // last gate or outside a level.
    struct Ranked {
        int gate = -1;     // index into Atlas::gates
        int choice = -1;   // index into that gate's choices
        float x = 0.0f;    // the gate's x
        Option option;
        int gravity = 0;   // the gravity after it
        double step = 0.0; // the way there from the state
        double cost = 0.0; // ... and on to the level's end
    };
    std::vector<Ranked> nextChoices(GJBaseGameLayer* pl, float x, float y, int gravity = -1);
    std::vector<Ranked> nextChoices(const Atlas& atlas, float x, float y, int gravity);

    // Pathfinder page > Show the planned route.
    bool routeShown();
    void setRouteShown(bool on);
    // With tickOverlay: puts the route's drawing on the level when wanted.
    void tickRoute(PlayLayer* pl);

    // Step R4: the route the pathfinder follows when guided. Planned from the
    // level's start (so every gate ahead of wherever it starts is on it),
    // cached until the map changes.
    const Plan* guidePlan(GJBaseGameLayer* pl);
    // Pathfinder page > Follow the planned route.
    bool guidanceEnabled();
    void setGuidance(bool on);
    // What the physics map found when it went for the gap (lo) at the gate at x:
    // a gap it keeps failing to reach is priced up and the route planned again.
    void noteLeg(float gateX, float lo, bool found);

    // Step R3: the last leg search drawn over the level, the states it kept
    // as dots and the way it found as a line (replacing the one before).
    void showLeg(PlayLayer* pl, std::vector<std::pair<float, float>> const& dots,
                 std::vector<std::pair<float, float>> const& path);

}  // namespace absense::route
