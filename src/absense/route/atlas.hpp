#pragma once

// The route planner's map of the whole level: step R1 of AI_PATHFINDER_PLAN.md
// (design in docs/AI_PATHFINDER_RESEARCH.md, section 2). Every object is read
// once, the level is cut into one-block columns, and each column gets the gaps
// the player's centre fits through there (its "openings") under the game mode
// in force. Then come the gates, the places a route has a choice or gets
// tight: forks, orbs, pads, portals, narrow gaps, and one at least every
// couple of seconds. Built once per level read, and again once the ghost run
// (what moves, and when) is back; nothing in the game changes.
//
// Step 4 of docs/ROUTE_ENGINE_PLAN.md: each gate also lists what a route can
// DO there, each written as a waypoint (Option): use an orb or skip it, touch
// a pad or portal or go around it, fire a toggle orb or not, and at every gate
// "anything" (FREE), which is tried last. The planner picks one per gate, and
// the route drives (step 5) aim each leg at it.
//
// It is a map, not physics: the planner only uses it to choose, and the exact
// simulation decides what is possible. Where it cannot be sure (something
// moves or toggles there, or a dual, solo or teleport portal changes the game)
// it says so instead of guessing.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class GJBaseGameLayer;
class PlayLayer;
class GameObject;
namespace cocos2d {
    class CCDrawNode;
}

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
        bool timed = false;    // what moves or toggles here was read where it stands while the player goes through (the ghost run)
        float bandLo = 0.0f;   // the room the mode gives the player's centre, floor to ceiling
        float bandHi = 0.0f;
        std::vector<Opening> openings;
    };

    // Waist (step 4): an opening narrows to under three player heights here,
    // the narrowest point of a squeeze like ToE2's slope "diamond". Toggle: a
    // toggle orb, or a touch trigger whose group holds something the player
    // can touch.
    enum class GateKind : uint8_t { Fork, Orb, Pad, Portal, Tight, Spacing, Waist, Toggle };

    const char* gateKindName(GateKind k);

    // What a route does at a gate (step 4). USE / SKIP an orb; TOUCH / AVOID a
    // pad or a gravity, mode or size portal; FIRE / SKIP a toggle orb or a
    // touch trigger; FREE: be anywhere in a gap, nothing asked (every gate has
    // it, one per gap, tried last).
    enum class OptionKind : uint8_t { Free, Use, Skip, Touch, Avoid, Fire };

    const char* optionKindName(OptionKind k);  // "free", "use", "skip", "touch", "avoid", "fire"

    struct Option {
        OptionKind kind = OptionKind::Free;
        // Where the player's centre must be at the gate's x. USE, TOUCH and
        // FIRE: the object's contact region (its box grown by the player's
        // half) clipped to the column's openings; SKIP and AVOID: the
        // openings outside it (a SKIP keeps the whole gap when the orb fills
        // it: the orb is then passed without a press).
        Opening band;
        int objectUid = 0;          // the object it is about (GameObject::m_uniqueID), 0 for none
        bool mustFire = false;      // ... which must fire on the way through (an orb used, a pad or portal touched)
        bool mustNotFire = false;   // ... or must not (skipped, avoided)
        // The gravity it leaves the player with: 0 normal, 1 flipped, -1 as it
        // was. Gravity pads and portals set it outright; an orb that flips it
        // (blue, green, pink dash) and the toggle portal give the other one
        // from before, which only the route knows: flipsGravity.
        int8_t gravityAfter = -1;
        bool flipsGravity = false;
        int opening = -1;  // which of the gate's openings (Gate::options) the band lies in
    };

    // Gravity after taking `o` with `before` (0 normal, 1 flipped) in force.
    int gravityAfter(const Option& o, int before);

    // The same gate on every build of the map for this read of the level
    // (the ghost run coming back rebuilds it): its column, its kind and the
    // object it is at (0 for a gap). Object uids are the game's own for the
    // level as loaded, so a reload of the level gives new ones.
    struct GateKey {
        int column = 0;
        GateKind kind = GateKind::Spacing;
        int objectUid = 0;
        bool operator==(const GateKey& o) const {
            return column == o.column && kind == o.kind && objectUid == o.objectUid;
        }
        bool operator!=(const GateKey& o) const { return !(*this == o); }
        bool operator<(const GateKey& o) const {
            if (column != o.column) return column < o.column;
            if (kind != o.kind) return (int)kind < (int)o.kind;
            return objectUid < o.objectUid;
        }
    };

    struct Gate {
        GateKind kind = GateKind::Spacing;
        int column = 0;
        float x = 0.0f;
        float y = 0.0f;                // an orb's, pad's or portal's centre
        std::vector<Opening> options;  // the bands a route can be in here (the column's openings)
        // Step 4: what a route can do here, the object's own options first
        // (USE/TOUCH/FIRE, then SKIP/AVOID, each lowest band first), then
        // FREE, one per opening.
        std::vector<Option> choices;
        GateKey key;
        const char* what = "";  // the object, "blue orb", "ship portal"...; empty at a gap
    };

    // "use 230-290 at the blue orb, x 21825, flips gravity", for logs and the MCP.
    std::string describe(const Gate& g, const Option& o);

    struct Atlas {
        uint64_t id = 0;  // a new number each build: what the plans built on it are keyed by
        std::string level;
        float length = 0.0f;
        int objects = 0;         // objects the map read
        int movingObjects = 0;   // ... that something moves or toggles (left out of the gaps)
        int unknownObjects = 0;  // ... past which the map cannot follow
        std::vector<Column> columns;
        std::vector<Gate> gates;  // in column order
        std::vector<float> unknownX;
        double buildMs = 0.0;
        // The ghost run (see Trajectory::beginGhost): how many columns it
        // timed, how far it got, what it cost, and why it stopped short or did
        // not run.
        int timedColumns = 0;
        float timedTo = 0.0f;
        double ghostMs = 0.0;
        std::string ghostNote;
        bool ghostOut = false;  // still out when this map was built: it is built again when the ghost is back
        std::vector<std::pair<float, float>> speedPortals;  // (x, speed), in x order: the ghost takes each one
    };

    // The map of the level `pl` is playing, built the first time it is asked
    // for after the level was opened (and again if its objects change count,
    // or the ghost run is back).
    const Atlas* atlasFor(GJBaseGameLayer* pl);

    // The object an option is about (Option::objectUid) in the level `pl` is
    // playing, or nullptr. Remembered for the objects the map's gates are at,
    // looked for otherwise. A ring is a RingObject, the rest GameObjects.
    GameObject* objectOf(GJBaseGameLayer* pl, int uid);

    // From PlayLayer::postUpdate: the ghost run a slice a frame (a share of
    // the frame), so the game does not wait for it.
    void tickGhost(PlayLayer* pl);

    // "Forks 12, orbs 30, ..." for the Pathfinder page and the logs.
    std::string summary(const Atlas& a);

    // The drawing over the level (Pathfinder page > Show the level map).
    bool overlayEnabled();
    void setOverlayEnabled(bool on);
    // From PlayLayer::postUpdate: puts the drawing on the level when it is
    // wanted and missing, and takes it off when it is not.
    void tickOverlay(PlayLayer* pl);

    // The mark an option gets on the overlays (step 4): a letter drawn in
    // lines, U use, S skip, T touch, A avoid, F fire (FREE has none), in its
    // kind's colour, or gold for the one the planned route takes. `y` is its
    // middle; markY puts it in a band (near the bottom of an open-sky gap).
    void drawMark(cocos2d::CCDrawNode* node, OptionKind kind, float x, float y, float size, float alpha,
                  bool taken = false);
    float markY(const Opening& band);

}  // namespace absense::route
