#pragma once

// Hitborders' geometry (Nigel, 2026-11-02; NaN GD's "accurate gaps" idea):
// every object's collision shape grown by the player's own box, a Minkowski
// sum, so a region drawn is exactly where the player's CENTER may not go (or,
// for the touch and special kinds, where its box touches something). The
// rules are GD 2.2081's own collision tests, read from the Windows binary:
//
//   checkCollisions 0x2137f0            ground, ceiling, the order of the pass
//   collisionCheckObjects 0x214960      orbs, pads, portals (and what is skipped)
//   playerCircleCollision 0x211df0      saws, both "Fix radius collision" ways
//   OBB2D::overlaps1Way 0x6e130         rotated objects (two-way SAT)
//   collidedWithObjectInternal 0x391a70 blocks: only the inner box kills
//   preSlopeCollision 0x38f2e0          a slope's legs, 1-unit blocks
//   collidedWithSlopeInternal 0x38f810  a slope's hypotenuse
//
// The full evidence is the research note behind this feature (RESEARCH.md
// §3-§5, plus the slope tolerances and the corridor rules read for the build).
//
// Pure: no GD, no cocos. hacks/hitborders.cpp reads the level into these
// structs and draws what comes back; tools/run_hitborder_tests.cpp checks the
// shapes against brute-force copies of GD's tests, offline.

#include <array>
#include <cstdint>
#include <vector>

namespace gucci::hitborder {

    struct Vec2 {
        double x = 0.0;
        double y = 0.0;
    };

    // An axis-aligned rect by its edges (a CCRect's getMinX .. getMaxY).
    struct Rect {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    };

    // What a region means for the player's center, and so its color.
    enum class Kind : uint8_t {
        Lethal,   // red: the center dies here
        Touch,    // blue: the box touches something solid (lands, slides, snaps; no death)
        Special,  // green: orbs, pads, portals, touch triggers
    };

    // One region: a convex polygon, counter-clockwise. Circles come out as
    // polygons too (circlePoly), so a drawer has one kind of thing to draw.
    struct Shape {
        Kind kind = Kind::Lethal;
        std::vector<Vec2> poly;
    };

    enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

    // The player, as the next collision pass will test it.
    struct Player {
        Vec2 pos;                   // center: getPosition() (getRealPosition, box offset 0)
        double hx = 15.0, hy = 15.0;  // outer half size: getObjectRect()
        double ix = 4.5, iy = 4.5;    // inner half size: getObjectRect(0.3, 0.3)
        double rotation = 0.0;      // the icon's rotation in degrees (CCNode::getRotation)
        Mode mode = Mode::Cube;
        bool upsideDown = false;
        bool platformer = false;
        bool sideways = false;      // rotated gameplay: its slope rules are not decoded
        bool goingLeft = false;
        bool dartSlide = false;     // m_stateDartSlide > 0: touching a D block (1755)
        bool hitHead = false;       // m_stateHitHead > 0: touching an H block (1859)
        // The slope tests' tolerances read these (collidedWithSlopeInternal).
        // checkCollisions copies m_isOnSlope into m_wasOnSlope as it starts,
        // so "on a slope now" is "was on a slope" for the next pass.
        bool onSlope = false;
        bool jumpBuffered = false;  // m_jumpBuffered (+0x985)
        bool boosted = false;       // m_maybeIsBoosted (+0xa1c)
        double yVelocity = 0.0;
        int ridingSlopeId = 0;      // m_collidingWithSlopeId
        bool ridingSlopeTop = false;  // m_isCurrentSlopeTop
        double vehicleSize = 1.0;

        bool flying() const { return mode == Mode::Ship || mode == Mode::Ufo || mode == Mode::Wave || mode == Mode::Swing; }
        // Cube, robot and spider: no head snap, a slope over the head kills
        // past 2 units, the corridor's far side kills.
        bool cubeLike() const { return mode == Mode::Cube || mode == Mode::Robot || mode == Mode::Spider; }
    };

    // ---------------------------------------------------------------- objects

    // A hazard, or an orb / pad / portal / touch trigger: the same three tests
    // against the player's outer box (checkCollisions 0x2147c0-0x214955,
    // collisionCheckObjects 0x214ae4-0x214bb7).
    struct Collider {
        Rect rect;                    // getObjectRect(): for an oriented object, its box's bounding rect
        bool oriented = false;        // m_shouldUseOuterOb
        std::array<Vec2, 4> box{};    // its OBB2D's corners, when oriented
        double radius = 0.0;          // > 0: the circle test (m_objectRadius, scaled)
        Vec2 center;                  // the circle's center: getRealPosition()
    };

    // A block (Solid, Breakable): touched by the outer box, killed by the inner.
    struct Block {
        Rect rect;                    // getObjectRect(): blocks have no rotated test
        bool passable = false;        // m_isPassable: no inner-box death
        bool breakable = false;       // breaks instead of killing
    };

    // A slope (type 25).
    struct Slope {
        Rect rect;                    // getObjectRect()
        int direction = 0;            // m_slopeDirection
        bool uphill = false;          // m_slopeUphill
        bool hazard = false;          // m_slopeIsHazard: IDs 363, 364, 366, 367, 1717, 1718, 1723, 1724
        bool passable = false;
        int uniqueId = 0;             // m_uniqueID
    };

    // What to draw. `core` is the blocks' red inner-box core ("Exact"); off,
    // only the blue touch zone ("Simple"). Circles get as many sides as keep
    // a chord within 0.05 units of the true edge (circleSides), at any zoom.
    struct Options {
        bool fixRadius = false;       // the level's "Fix radius collision" (LevelSettingsObject)
        bool useTilt = true;          // rotated objects against the icon's tilt; off: an upright icon
        bool core = true;
    };

    // Orbs, pads and portals pass Kind::Special; hazards Kind::Lethal.
    void colliderShapes(Collider const& c, Player const& p, Options const& o, Kind kind, std::vector<Shape>& out);
    void blockShapes(Block const& b, Player const& p, Options const& o, std::vector<Shape>& out);
    void slopeShapes(Slope const& s, Player const& p, Options const& o, std::vector<Shape>& out);

    // The ground and the ceiling (checkCollisions 0x21395f-0x214162). The
    // floor is at 90 + g out of a corridor, minPortalY + g in one, the ceiling
    // at maxPortalY - g; g = m_unkAngle1 * m_vehicleSize / 2. A corridor is
    // ship, UFO, wave, swing, ball, spider or dual, unless the camera is in
    // free mode. Regions run from the line out to `reach` beyond it.
    struct Ground {
        bool corridor = false;
        double floorY = 105.0;
        double ceilingY = 0.0;        // a corridor's only
        double left = 0.0, right = 0.0, reach = 1000.0;
    };
    void groundShapes(Ground const& g, Player const& p, std::vector<Shape>& out);

    // ---------------------------------------------------------------- slopes

    inline bool slopeFloorTop(int direction) {
        return direction == 1 || direction == 3 || direction == 5 || direction == 6;
    }
    inline bool slopeWallLeft(int direction) {
        return direction == 2 || direction == 3 || direction == 4 || direction == 6;
    }
    // GameObject::slopeYPos(float) 0x1a13b0: the rect's diagonal extended past
    // its ends; a hazard slope's line moved 4 units toward its open side.
    double slopeYPos(Slope const& s, double x);

    // ---------------------------------------------------------------- helpers

    std::vector<Vec2> rectPoly(Rect r);
    // Counter-clockwise hull, no repeated or collinear points.
    std::vector<Vec2> convexHull(std::vector<Vec2> pts);
    // The player's box at the origin, turned as GD turns it: OBB2D corners for
    // the angle -rotation (updateOrientedBox 0x1a1570, calculateWithCenter 0x6da80).
    std::array<Vec2, 4> playerBox(double hx, double hy, double rotationDeg);
    // `box` grown by a box at the origin: the hull of the 16 corner sums.
    std::vector<Vec2> minkowski(std::array<Vec2, 4> const& box, std::array<Vec2, 4> const& player);
    // A convex polygon clipped to a rect (Sutherland-Hodgman).
    std::vector<Vec2> clipToRect(std::vector<Vec2> const& poly, Rect r);
    // ... and to another convex polygon.
    std::vector<Vec2> clipConvex(std::vector<Vec2> const& poly, std::vector<Vec2> const& clip);
    // A circle as a polygon with `segments` corners on it.
    std::vector<Vec2> circlePoly(Vec2 c, double r, int segments);
    // Every edge of a convex counter-clockwise polygon moved out by w (in by
    // -w), corners mitered. The drawer uses it for the outline band and to
    // take back the half unit cocos' fill adds. Empty when an inset eats it.
    std::vector<Vec2> offsetConvex(std::vector<Vec2> const& poly, double w);

    double area(std::vector<Vec2> const& poly);
    // Inside or on the edge of a convex counter-clockwise polygon, within eps.
    bool contains(std::vector<Vec2> const& poly, Vec2 p, double eps = 1e-9);
    // How far p is from the polygon's edge, inside or out.
    double distanceToEdge(std::vector<Vec2> const& poly, Vec2 p);
    // In any of these shapes of this kind.
    bool covered(std::vector<Shape> const& shapes, Kind kind, Vec2 p, double eps = 1e-9);

} // namespace gucci::hitborder
