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
        Opening band;       // the gap the route is in at this gate
        double step = 0.0;  // what getting here from the gate before cost
    };

    struct Route {
        std::vector<Waypoint> points;  // from the start to the level's end
        double cost = 0.0;
    };

    struct Plan {
        float fromX = 0.0f;  // where it was planned from
        float fromY = 0.0f;
        std::vector<Route> routes;  // the cheapest first
        int layers = 0;             // gates on the way
        int edges = 0;              // gate-to-gate ways priced
        double ms = 0.0;
    };

    // How many routes are kept: the best and the next ones ready.
    constexpr int kRoutesKept = 3;

    // The plan from (x, y) to the end of the level `pl` is playing.
    Plan plan(const Atlas& atlas, float fromX, float fromY);

    // The plan the overlay shows: made from the player's position the first
    // time it is drawn on a level (cached until the map changes).
    const Plan* shownPlan(GJBaseGameLayer* pl);

    std::string summary(const Plan& p);

    // Pathfinder page > Show the planned route.
    bool routeShown();
    void setRouteShown(bool on);
    // With tickOverlay: puts the route's drawing on the level when wanted.
    void tickRoute(PlayLayer* pl);

    // Step R3: the last leg search drawn over the level, the states it kept
    // as dots and the way it found as a line (replacing the one before).
    void showLeg(PlayLayer* pl, std::vector<std::pair<float, float>> const& dots,
                 std::vector<std::pair<float, float>> const& path);

}  // namespace absense::route
