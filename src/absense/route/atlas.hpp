#pragma once

// The route planner's map of the whole level: step R1 of AI_PATHFINDER_PLAN.md
// (design in docs/AI_PATHFINDER_RESEARCH.md, section 2). Every object is read
// once, the level is cut into one-block columns, and each column gets the gaps
// the player's centre fits through there (its "openings") under the game mode
// in force. Then come the gates, the places a route has a choice or gets
// tight: forks, orbs, pads, portals, narrow gaps, and one at least every
// couple of seconds. Built once per level read; nothing in the game changes.
//
// It is a map, not physics: the planner only uses it to choose, and the exact
// simulation decides what is possible. Where it cannot be sure (something
// moves or toggles there, or a dual, solo or teleport portal changes the game)
// it says so instead of guessing.

#include <cstdint>
#include <string>
#include <vector>

class GJBaseGameLayer;
class PlayLayer;

namespace absense::route {

    constexpr float kColumnWidth = 30.0f;  // one block

    struct Opening {
        float lo = 0.0f;  // where the player's centre fits, bottom to top
        float hi = 0.0f;
    };

    struct Column {
        float x0 = 0.0f;       // left edge
        uint8_t mode = 0;      // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
        bool mini = false;
        bool dynamic = false;  // something here moves or toggles: the map makes no claim about it
        bool unknown = false;  // a dual, solo or teleport portal, teleport orb, custom ring or trigger the map cannot follow
        float bandLo = 0.0f;   // the room the mode gives the player's centre, floor to ceiling
        float bandHi = 0.0f;
        std::vector<Opening> openings;
    };

    enum class GateKind : uint8_t { Fork, Orb, Pad, Portal, Tight, Spacing };

    const char* gateKindName(GateKind k);

    struct Gate {
        GateKind kind = GateKind::Spacing;
        int column = 0;
        float x = 0.0f;
        float y = 0.0f;                // an orb's, pad's or portal's centre
        std::vector<Opening> options;  // the bands a route can be in here
    };

    struct Atlas {
        std::string level;
        float length = 0.0f;
        int objects = 0;         // objects the map read
        int movingObjects = 0;   // ... that something moves or toggles (left out of the gaps)
        int unknownObjects = 0;  // ... past which the map cannot follow
        std::vector<Column> columns;
        std::vector<Gate> gates;
        std::vector<float> unknownX;
        double buildMs = 0.0;
    };

    // The map of the level `pl` is playing, built the first time it is asked
    // for after the level was opened (and again if its objects change count).
    const Atlas* atlasFor(GJBaseGameLayer* pl);

    // "Forks 12, orbs 30, ..." for the Pathfinder page and the logs.
    std::string summary(const Atlas& a);

    // The drawing over the level (Pathfinder page > Show the level map).
    bool overlayEnabled();
    void setOverlayEnabled(bool on);
    // From PlayLayer::postUpdate: puts the drawing on the level when it is
    // wanted and missing, and takes it off when it is not.
    void tickOverlay(PlayLayer* pl);

}  // namespace absense::route
