// Hitborders' geometry (src/tools/hitborder_geom), outside the game. Run by
// run_tests.bat.
//
// Two kinds of check:
//  - every region against a brute-force copy of the GD 2.2081 test it stands
//    for, written point by point (the player's center at a point, the test
//    run, hit or not), over a grid of thousands of points around the object.
//    A point may only disagree when the test itself flips within a hair of it
//    (it sits on the edge), or, for circles, within the polygon's 0.05-unit
//    chord error;
//  - numbers worked out by hand from the research note (RESEARCH.md §5.4):
//    a cube rides 21.2 above a 45-degree line, gap sizes per mode, the mini
//    cube's inner box.
//
// The brute-force copies are the rules as read from the binary; this checks
// that the drawn polygons say what those rules say. Whether the rules match
// the game is what an in-game probe checks.

#include "tools/hitborder_geom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace gucci::hitborder;

static int fails = 0;
static int checks = 0;
#define CHECK(c)                                             \
    do {                                                     \
        ++checks;                                            \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

namespace {

    constexpr double kPi = 3.14159265358979323846;

    // ---------------------------------------------------------------- GD's tests, point by point

    Rect playerRect(Vec2 c, double hx, double hy) {
        return {c.x - hx, c.y - hy, c.x + hx, c.y + hy};
    }

    // CCRect::intersectsRect: inclusive.
    bool rectsTouch(Rect a, Rect b) {
        return !(a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0);
    }

    // The player's OBB as updateOrientedBox / calculateWithCenter build it.
    std::array<Vec2, 4> playerObb(Vec2 c, double hx, double hy, double rotationDeg) {
        double const a = -rotationDeg * 0.01745329238474369;
        Vec2 const X{std::cos(a) * hx, std::sin(a) * hx};
        Vec2 const Y{-std::sin(a) * hy, std::cos(a) * hy};
        return {{{c.x - X.x - Y.x, c.y - X.y - Y.y},
                 {c.x + X.x - Y.x, c.y + X.y - Y.y},
                 {c.x + X.x + Y.x, c.y + X.y + Y.y},
                 {c.x - X.x + Y.x, c.y - X.y + Y.y}}};
    }

    // An object's OBB: center, size, rotation in degrees (cocos, clockwise).
    std::array<Vec2, 4> objectObb(Vec2 c, double w, double h, double rotationDeg) {
        return playerObb(c, w / 2, h / 2, rotationDeg);
    }

    Rect boundsOf(std::array<Vec2, 4> const& b) {
        Rect r{b[0].x, b[0].y, b[0].x, b[0].y};
        for (auto const& v : b) {
            r.x0 = std::min(r.x0, v.x);
            r.y0 = std::min(r.y0, v.y);
            r.x1 = std::max(r.x1, v.x);
            r.y1 = std::max(r.y1, v.y);
        }
        return r;
    }

    // OBB2D::overlaps1Way 0x6e130: the other box's corners on this box's two
    // edge axes, each scaled so this box spans [origin, origin + 1].
    bool overlaps1Way(std::array<Vec2, 4> const& a, std::array<Vec2, 4> const& b) {
        Vec2 axes[2] = {{a[1].x - a[0].x, a[1].y - a[0].y}, {a[3].x - a[0].x, a[3].y - a[0].y}};
        for (auto& ax : axes) {
            double const l2 = ax.x * ax.x + ax.y * ax.y;
            ax.x /= l2;
            ax.y /= l2;
            double const origin = a[0].x * ax.x + a[0].y * ax.y;
            double tMin = std::numeric_limits<double>::infinity();
            double tMax = -tMin;
            for (auto const& q : b) {
                double const t = q.x * ax.x + q.y * ax.y;
                tMin = std::min(tMin, t);
                tMax = std::max(tMax, t);
            }
            if (tMin > origin + 1 || tMax < origin)
                return false;
        }
        return true;
    }

    bool obbOverlap(std::array<Vec2, 4> const& a, std::array<Vec2, 4> const& b) {
        return overlaps1Way(a, b) && overlaps1Way(b, a);
    }

    // checkCollisions' hazard test (0x2147c0-0x214955), the center at c.
    bool hazardHits(Collider const& h, Player const& p, Options const& o, Vec2 c) {
        Rect const pr = playerRect(c, p.hx, p.hy);
        if (h.radius > 0) {
            if (o.fixRadius) {
                if (!(std::hypot(c.x - h.center.x, c.y - h.center.y) <= h.radius + p.hx))
                    return false;
            } else {
                // playerCircleCollision, legacy: the rect holds the center, or
                // a corner is strictly within r.
                bool hit = h.center.x >= pr.x0 && h.center.x <= pr.x1 && h.center.y >= pr.y0 && h.center.y <= pr.y1;
                for (Vec2 const k : {Vec2{pr.x0, pr.y0}, Vec2{pr.x1, pr.y0}, Vec2{pr.x1, pr.y1}, Vec2{pr.x0, pr.y1}})
                    hit = hit || h.radius > std::hypot(k.x - h.center.x, k.y - h.center.y);
                if (!hit)
                    return false;
            }
        } else if (!rectsTouch(h.rect, pr)) {
            return false;
        }
        if (!h.oriented || (o.fixRadius && h.radius > 0))
            return true;
        return obbOverlap(h.box, playerObb(c, p.hx, p.hy, o.useTilt ? p.rotation : 0.0));
    }

    // A slope, point by point (preSlopeCollision 0x38f2e0 +
    // collidedWithSlopeInternal 0x38f810, as read): 0 nothing, 1 touched, 2 dead.
    int slopeEffect(Slope const& s, Player const& p, Options const& o, Vec2 c) {
        Rect const r = s.rect;
        bool const top = slopeFloorTop(s.direction);
        bool const wallLeft = slopeWallLeft(s.direction);
        Rect const outer = playerRect(c, p.hx, p.hy);
        Rect const inner = playerRect(c, p.ix, p.iy);
        int result = 0;
        auto asBlock = [&](Rect strip) {
            if (!rectsTouch(strip, outer))
                return;
            result = std::max(result, 1);
            if (o.core && !p.platformer && !s.passable && rectsTouch(strip, inner))
                result = 2;
        };
        double const margin = (p.platformer && p.onSlope) ? 5.0 : 0.0;
        bool beyond = false;
        if (wallLeft) {
            if (r.x0 > c.x) {
                beyond = true;
                double const e = margin + ((p.goingLeft && !p.platformer) ? 1.0 : 0.0);
                asBlock({r.x0, r.y0 + e, r.x0 + 1, r.y1 - e});
            }
        } else if (p.platformer || p.goingLeft) {
            if (c.x > r.x1) {
                beyond = true;
                double const e = margin + ((!p.goingLeft && !p.platformer) ? 1.0 : 0.0);
                asBlock({r.x1 - 1, r.y0 + e, r.x1, r.y1 - e});
            }
        }
        if (top) {
            if (c.y >= r.y1) {
                beyond = true;
                asBlock({r.x0, r.y1 - 1, r.x1, r.y1});
            }
        } else if (!(r.y0 + 1 < c.y)) {
            beyond = true;
            asBlock({r.x0, r.y0, r.x1, r.y0 + 1});
        }
        if (beyond || !rectsTouch(r, outer))
            return result;
        if (!rectsTouch({r.x0, r.y0 + 1, r.x1, r.y1 - 1}, outer))  // a fresh contact
            return result;

        double const W = r.x1 - r.x0;
        double const H = r.y1 - r.y0;
        float const theta = std::atan(static_cast<float>(H) / static_cast<float>(W));
        double const off = static_cast<double>(static_cast<float>(2.0 * p.hy) / std::cos(theta) * 0.5f);
        bool down = !s.uphill;
        if (p.goingLeft)
            down = !down;
        if (p.upsideDown)
            down = !down;
        bool const squeeze = p.onSlope && p.ridingSlopeId != s.uniqueId && p.ridingSlopeTop != top;
        double const sq = (squeeze && !p.platformer) ? 20.0 * p.vehicleSize : 0.0;
        double tolRide = 0.0;
        double tolHead = 0.0;
        if (!squeeze) {
            if (down) {
                bool use = p.mode == Mode::Ship
                               ? !p.jumpBuffered
                               : (!p.boosted || p.mode == Mode::Ufo || p.mode == Mode::Wave || p.mode == Mode::Swing);
                if (use && p.mode == Mode::Ufo)
                    use = p.upsideDown ? p.yVelocity >= 0 : p.yVelocity <= 0;
                if (use)
                    tolRide = p.onSlope ? 4.0 : 1.0;
            } else if (p.flying() && p.jumpBuffered) {
                tolHead = p.onSlope ? 2.0 : 1.0;
            }
        }
        bool const riding = top == p.upsideDown;
        double const tol = riding ? tolRide : tolHead;
        double const y = slopeYPos(s, c.x);
        double T = 0.0;
        bool hit = false;
        if (!top) {
            T = std::max(std::min(y + off - sq, r.y1 + p.hy - sq), r.y0);
            hit = c.y < T + tol;
        } else {
            T = std::min(std::max(y - off + sq, r.y0 - p.hy + sq), r.y1);
            hit = c.y > T - tol;
        }
        if (!hit)
            return result;
        bool const cubeHead = !riding && p.cubeLike() && !p.platformer && !p.hitHead;
        if (cubeHead) {
            bool const dead = top ? (c.y - 2.0 > T) : (c.y + 2.0 < T);
            return std::max(result, dead ? 2 : 1);
        }
        bool const classicWave = p.mode == Mode::Wave && !p.dartSlide && !p.platformer && !p.hitHead;
        bool const squeezeKills = squeeze && !p.platformer && !p.hitHead;
        return std::max(result, (s.hazard || classicWave || squeezeKills) ? 2 : 1);
    }

    // ---------------------------------------------------------------- grid compare

    // Every point of `area` on a `step` grid: got(c) must equal want(c),
    // except where want flips within `edge` of the point.
    template <class Got, class Want>
    bool agree(std::string const& name, Rect area, double step, double edge, Got got, Want want) {
        int bad = 0;
        for (double y = area.y0; y <= area.y1; y += step) {
            for (double x = area.x0; x <= area.x1; x += step) {
                Vec2 const c{x, y};
                bool const w = want(c);
                if (got(c) == w)
                    continue;
                bool onEdge = false;
                for (int k = 0; k < 16 && !onEdge; ++k) {
                    double const a = k * kPi / 8;
                    onEdge = want({x + edge * std::cos(a), y + edge * std::sin(a)}) != w;
                }
                if (onEdge)
                    continue;
                if (bad < 4)
                    std::printf("  %s: at (%.3f, %.3f) the test says %s, the drawing %s\n", name.c_str(), x, y,
                                w ? "hit" : "miss", w ? "misses" : "hits");
                ++bad;
            }
        }
        if (bad > 0)
            std::printf("  %s: %d points disagree\n", name.c_str(), bad);
        return bad == 0;
    }

    Player modePlayer(Mode m, bool mini) {
        // getObjectRect(): m_width x vehicle size; getObjectRect(0.3, 0.3):
        // 0.3 x m_width, mini or not (RESEARCH.md §3.1-§3.3).
        double width = 30.0;
        if (m == Mode::Wave)
            width = 10.0;
        else if (m == Mode::Spider)
            width = 27.0;
        Player p;
        p.mode = m;
        p.hx = p.hy = width * (mini ? 0.6 : 1.0) / 2.0;
        p.ix = p.iy = width * 0.3 / 2.0;
        p.vehicleSize = mini ? 0.6 : 1.0;
        return p;
    }

    const char* modeName(Mode m) {
        switch (m) {
        case Mode::Cube: return "cube";
        case Mode::Ship: return "ship";
        case Mode::Ball: return "ball";
        case Mode::Ufo: return "ufo";
        case Mode::Wave: return "wave";
        case Mode::Robot: return "robot";
        case Mode::Spider: return "spider";
        case Mode::Swing: return "swing";
        }
        return "?";
    }

    bool near(double a, double b, double eps = 1e-6) {
        return std::abs(a - b) <= eps;
    }

    // The lowest and highest y in [y0, y1] at x where `inside` is false: the
    // room a column has.
    double firstFree(double x, double y0, double y1, double step, auto inside) {
        for (double y = y0; y <= y1; y += step) {
            if (!inside(Vec2{x, y}))
                return y;
        }
        return y1;
    }

} // namespace

int main() {
    // ---- helpers
    {
        auto hull = convexHull({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 5}, {5, 0}, {2, 3}});
        CHECK(hull.size() == 4);
        CHECK(near(area(hull), 100.0));
        CHECK(area(hull) > 0);  // counter-clockwise

        auto grown = offsetConvex(rectPoly({0, 0, 10, 10}), 2.0);
        CHECK(grown.size() == 4 && near(area(grown), 196.0));
        // In by half a unit and back out: the same octagon.
        auto oct = circlePoly({0, 0}, 20, 8);
        auto back = offsetConvex(offsetConvex(oct, -0.5), 0.5);
        CHECK(back.size() == oct.size() && near(area(back), area(oct), 1e-6));
        // An inset that eats the polygon gives nothing.
        CHECK(offsetConvex(rectPoly({0, 0, 1, 1}), -0.6).empty());

        auto clipped = clipToRect(rectPoly({0, 0, 10, 10}), {5, -5, 20, 5});
        CHECK(near(area(clipped), 25.0));

        // Two upright boxes: the sum is a box of the summed halves.
        std::array<Vec2, 4> box{{{-3, -2}, {3, -2}, {3, 2}, {-3, 2}}};
        auto sum = minkowski(box, playerBox(15, 15, 0));
        CHECK(sum.size() == 4 && near(area(sum), 36.0 * 34.0));
        // A 45-degree diamond with an upright player: an octagon.
        auto diamond = objectObb({0, 0}, 10, 10, 45);
        auto oct2 = minkowski(diamond, playerBox(15, 15, 0));
        CHECK(oct2.size() == 8);
    }

    // ---- the player's OBB turns as GD turns it: a positive (clockwise)
    // rotation of 30 degrees moves the box's +x edge midpoint below the axis.
    {
        auto b = playerBox(15, 15, 30);
        Vec2 const mid{(b[1].x + b[2].x) / 2, (b[1].y + b[2].y) / 2};
        CHECK(mid.x > 0 && mid.y < 0);
        CHECK(near(std::hypot(mid.x, mid.y), 15.0));
    }

    Options o;

    // ---- rect hazards: the rect grown by the outer half, every mode and size
    for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Spider}) {
        for (bool mini : {false, true}) {
            Player const p = modePlayer(m, mini);
            Collider spike;
            spike.rect = {100, 90, 106, 102};  // a spike's 6 x 12 box
            std::vector<Shape> shapes;
            colliderShapes(spike, p, o, Kind::Lethal, shapes);
            CHECK(shapes.size() == 1);
            std::string const name = std::string("rect hazard, ") + modeName(m) + (mini ? " mini" : "");
            CHECK(agree(name, {50, 40, 160, 150}, 0.73, 1e-4,
                        [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                        [&](Vec2 c) { return hazardHits(spike, p, o, c); }));
        }
    }

    // ---- rotated hazards: the octagon, with and without the icon's tilt
    for (double objRot : {30.0, 45.0, 72.5}) {
        for (double tilt : {0.0, 23.0, 90.0, -41.0}) {
            for (bool useTilt : {true, false}) {
                Player p = modePlayer(Mode::Cube, false);
                p.rotation = tilt;
                Options ot = o;
                ot.useTilt = useTilt;
                Collider thorn;
                thorn.oriented = true;
                thorn.box = objectObb({200, 120}, 8, 20, objRot);
                thorn.rect = boundsOf(thorn.box);
                std::vector<Shape> shapes;
                colliderShapes(thorn, p, ot, Kind::Lethal, shapes);
                char name[96];
                std::snprintf(name, sizeof name, "rotated hazard %.1f, tilt %.1f%s", objRot, tilt,
                              useTilt ? "" : " (tilt off)");
                CHECK(agree(name, {150, 70, 250, 170}, 0.61, 1e-4,
                            [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                            [&](Vec2 c) { return hazardHits(thorn, p, ot, c); }));
            }
        }
    }

    // ---- saws: legacy ("hearts") and Fix radius collision, then a legacy saw
    // turned by a rotate trigger (the circle and the rotated test both)
    for (double radius : {15.0, 32.0, 6.0}) {
        for (Mode m : {Mode::Cube, Mode::Wave}) {
            for (bool fix : {false, true}) {
                Player const p = modePlayer(m, false);
                Options of = o;
                of.fixRadius = fix;
                Collider saw;
                saw.radius = radius;
                saw.center = {300, 200};
                saw.rect = {300 - radius, 200 - radius, 300 + radius, 200 + radius};
                std::vector<Shape> shapes;
                colliderShapes(saw, p, of, Kind::Lethal, shapes);
                char name[96];
                std::snprintf(name, sizeof name, "saw r=%.0f, %s, %s", radius, modeName(m), fix ? "fix radius" : "legacy");
                CHECK(agree(name, {220, 120, 380, 280}, 0.67, 0.06,
                            [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                            [&](Vec2 c) { return hazardHits(saw, p, of, c); }));
            }
        }
    }
    {
        Player p = modePlayer(Mode::Cube, false);
        p.rotation = 17.0;
        Collider saw;
        saw.radius = 20.0;
        saw.center = {300, 200};
        saw.oriented = true;
        saw.box = objectObb({300, 200}, 30, 30, 33.0);
        saw.rect = boundsOf(saw.box);
        std::vector<Shape> shapes;
        colliderShapes(saw, p, o, Kind::Lethal, shapes);
        CHECK(agree("legacy saw turned by a trigger", {230, 130, 370, 270}, 0.67, 0.06,
                    [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                    [&](Vec2 c) { return hazardHits(saw, p, o, c); }));
    }
    // The hearts, by the numbers (RESEARCH.md §5.1): r = 15 reaches 15 straight
    // up from a cube's square, not 30; r = 32 reaches 43.3.
    {
        Player const p = modePlayer(Mode::Cube, false);
        for (auto [r, reach] : {std::pair{15.0, 15.0}, std::pair{32.0, 43.3}}) {
            Collider saw;
            saw.radius = r;
            saw.center = {0, 0};
            std::vector<Shape> shapes;
            colliderShapes(saw, p, o, Kind::Lethal, shapes);
            double const top = firstFree(0.0, 0.0, 100.0, 0.01, [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); });
            CHECK(std::abs(top - reach) < 0.1);
        }
    }

    // ---- interactables: the same tests, green
    {
        Player const p = modePlayer(Mode::Ship, false);
        Collider portal;
        portal.rect = {400, 100, 425, 190};
        std::vector<Shape> shapes;
        colliderShapes(portal, p, o, Kind::Special, shapes);
        CHECK(shapes.size() == 1 && shapes[0].kind == Kind::Special);
        CHECK(agree("portal", {350, 50, 480, 240}, 0.73, 1e-4,
                    [&](Vec2 c) { return covered(shapes, Kind::Special, c); },
                    [&](Vec2 c) { return hazardHits(portal, p, o, c); }));
    }

    // ---- blocks: blue where the outer box touches, red where the inner one does
    for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Spider}) {
        for (bool mini : {false, true}) {
            Player const p = modePlayer(m, mini);
            Block block;
            block.rect = {500, 90, 530, 120};
            std::vector<Shape> shapes;
            blockShapes(block, p, o, shapes);
            std::string const name = std::string("block, ") + modeName(m) + (mini ? " mini" : "");
            CHECK(agree(name + " (touch)", {440, 30, 590, 180}, 0.73, 1e-4,
                        [&](Vec2 c) { return covered(shapes, Kind::Touch, c); },
                        [&](Vec2 c) { return rectsTouch(block.rect, playerRect(c, p.hx, p.hy)); }));
            CHECK(agree(name + " (dead)", {440, 30, 590, 180}, 0.73, 1e-4,
                        [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                        [&](Vec2 c) { return rectsTouch(block.rect, playerRect(c, p.ix, p.iy)); }));
        }
    }
    // The inner box ignores mini: a mini cube's core is as wide as a cube's.
    {
        Block block;
        block.rect = {0, 0, 30, 30};
        std::vector<Shape> big, small;
        blockShapes(block, modePlayer(Mode::Cube, false), o, big);
        blockShapes(block, modePlayer(Mode::Cube, true), o, small);
        CHECK(covered(big, Kind::Lethal, {34.4, 15}) && !covered(big, Kind::Lethal, {34.6, 15}));
        CHECK(covered(small, Kind::Lethal, {34.4, 15}) && !covered(small, Kind::Lethal, {34.6, 15}));
        CHECK(covered(small, Kind::Touch, {38.9, 15}) && !covered(small, Kind::Touch, {39.1, 15}));
        // Passable, breakable, platformer, or Simple: no core.
        for (int which = 0; which < 4; ++which) {
            Block b = block;
            Player pp = modePlayer(Mode::Cube, false);
            Options oo = o;
            if (which == 0)
                b.passable = true;
            else if (which == 1)
                b.breakable = true;
            else if (which == 2)
                pp.platformer = true;
            else
                oo.core = false;
            std::vector<Shape> s;
            blockShapes(b, pp, oo, s);
            CHECK(!covered(s, Kind::Lethal, {15, 15}) && covered(s, Kind::Touch, {15, 15}));
        }
    }

    // ---- gaps (RESEARCH.md §5.4)
    {
        // Spike to spike: the hitbox gap less one outer box (cube 30, wave 10).
        for (auto [m, lost] : {std::pair{Mode::Cube, 30.0}, std::pair{Mode::Wave, 10.0}}) {
            Player const p = modePlayer(m, false);
            Collider low, high;
            low.rect = {0, 0, 10, 20};
            high.rect = {0, 80, 10, 100};  // a 60-unit gap
            std::vector<Shape> s;
            colliderShapes(low, p, o, Kind::Lethal, s);
            colliderShapes(high, p, o, Kind::Lethal, s);
            auto in = [&](Vec2 c) { return covered(s, Kind::Lethal, c); };
            double const bottom = firstFree(5, 0, 100, 0.001, in);
            double top = bottom;
            while (top < 100 && !in({5, top + 0.001}))
                top += 0.001;
            CHECK(std::abs((top - bottom) - (60.0 - lost)) < 0.01);
        }
        // Block floor to block ceiling: a cube has G - 19.5 (the floor block's
        // touch zone below, the ceiling's lethal core above), the classic wave
        // G - 3 (the cores both ways, as nothing lands it), a ship G - 30.
        for (auto [m, lost, useTouchAbove] :
             {std::tuple{Mode::Cube, 19.5, false}, std::tuple{Mode::Ship, 30.0, true}, std::tuple{Mode::Wave, 3.0, false}}) {
            Player const p = modePlayer(m, false);
            Block floorBlock, ceilingBlock;
            floorBlock.rect = {0, 0, 30, 30};
            ceilingBlock.rect = {0, 90, 30, 120};  // a 60-unit gap
            std::vector<Shape> s;
            blockShapes(floorBlock, p, o, s);
            blockShapes(ceilingBlock, p, o, s);
            // Room from the floor: the cube and the ship are landed by the touch
            // zone, the wave only stopped by the core.
            Kind const below = m == Mode::Wave ? Kind::Lethal : Kind::Touch;
            Kind const above = useTouchAbove ? Kind::Touch : Kind::Lethal;
            double const from = firstFree(15, 0, 120, 0.001, [&](Vec2 c) { return covered(s, below, c); });
            double to = from;
            while (to < 120 && !covered(s, above, {15, to + 0.001}))
                to += 0.001;
            CHECK(std::abs((to - from) - (60.0 - lost)) < 0.01);
        }
    }

    // ---- slopes, every direction, against the point-by-point rules
    {
        int cases = 0;
        int bad = 0;
        for (int dir = 0; dir < 8; ++dir) {
            bool const top = slopeFloorTop(dir);
            bool const wallLeft = slopeWallLeft(dir);
            // The hypotenuse rises toward the wall on a floor slope and away
            // from it on a ceiling one.
            bool const uphill = top ? wallLeft : !wallLeft;
            for (double w : {30.0, 60.0}) {
                for (bool hazard : {false, true}) {
                    for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Ufo}) {
                        for (bool mini : {false, true}) {
                            for (bool upside : {false, true}) {
                                for (bool left : {false, true}) {
                                    Slope s;
                                    s.rect = {600, 150, 600 + w, 180};
                                    s.direction = dir;
                                    s.uphill = uphill;
                                    s.hazard = hazard;
                                    s.uniqueId = 7;
                                    Player p = modePlayer(m, mini);
                                    p.upsideDown = upside;
                                    p.goingLeft = left;
                                    p.jumpBuffered = (dir + (int)w) % 3 == 0;
                                    p.onSlope = left && !upside;
                                    p.ridingSlopeId = p.onSlope ? 3 : 0;
                                    p.ridingSlopeTop = !top && dir % 2 == 0;
                                    std::vector<Shape> shapes;
                                    slopeShapes(s, p, o, shapes);
                                    char name[128];
                                    std::snprintf(name, sizeof name, "slope dir %d %gx30%s, %s%s%s%s", dir, w,
                                                  hazard ? " hazard" : "", modeName(m), mini ? " mini" : "",
                                                  upside ? " upside down" : "", left ? " going left" : "");
                                    ++cases;
                                    bool const okDead = agree(std::string(name) + " (dead)", {550, 100, 710, 230}, 0.53,
                                                              1e-4,
                                                              [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                                                              [&](Vec2 c) { return slopeEffect(s, p, o, c) == 2; });
                                    bool const okAny = agree(
                                        std::string(name) + " (touched)", {550, 100, 710, 230}, 0.53, 1e-4,
                                        [&](Vec2 c) {
                                            return covered(shapes, Kind::Lethal, c) || covered(shapes, Kind::Touch, c);
                                        },
                                        [&](Vec2 c) { return slopeEffect(s, p, o, c) >= 1; });
                                    if (!okDead || !okAny)
                                        ++bad;
                                }
                            }
                        }
                    }
                }
            }
        }
        std::printf("slopes: %d cases\n", cases);
        CHECK(bad == 0);
    }

    // ---- slopes by the numbers
    {
        // A cube on a 45-degree floor slope rides hy / cos 45 = 21.21 above the
        // line, not 30 (RESEARCH.md §4.4): at the middle, 15 + 21.21.
        Slope s;
        s.rect = {0, 0, 30, 30};
        s.direction = 0;  // floor, wall on the right, rising toward it
        s.uphill = true;
        Player const cube = modePlayer(Mode::Cube, false);
        std::vector<Shape> shapes;
        slopeShapes(s, cube, o, shapes);
        auto touched = [&](Vec2 c) { return covered(shapes, Kind::Touch, c) || covered(shapes, Kind::Lethal, c); };
        CHECK(touched({15, 36.1}) && !touched({15, 36.3}));
        CHECK(!covered(shapes, Kind::Lethal, {15, 20}));  // riding a plain slope is safe

        // A hazard slope: the line 4 units further out, and red.
        s.hazard = true;
        shapes.clear();
        slopeShapes(s, cube, o, shapes);
        CHECK(covered(shapes, Kind::Lethal, {15, 40.1}) && !covered(shapes, Kind::Lethal, {15, 40.3}));

        // The classic wave dies on the line; going down the slope (here,
        // going left up a slope rising right) one unit more.
        s.hazard = false;
        Player wave = modePlayer(Mode::Wave, false);
        double const waveOff = 5.0 / std::cos(kPi / 4);
        shapes.clear();
        slopeShapes(s, wave, o, shapes);
        CHECK(covered(shapes, Kind::Lethal, {15, 15 + waveOff - 0.1}) && !covered(shapes, Kind::Lethal, {15, 15 + waveOff + 0.1}));
        wave.goingLeft = true;
        shapes.clear();
        slopeShapes(s, wave, o, shapes);
        CHECK(covered(shapes, Kind::Lethal, {15, 15 + waveOff + 0.9}) && !covered(shapes, Kind::Lethal, {15, 15 + waveOff + 1.1}));
        // With a D block, the wave slides: blue.
        wave.goingLeft = false;
        wave.dartSlide = true;
        shapes.clear();
        slopeShapes(s, wave, o, shapes);
        CHECK(!covered(shapes, Kind::Lethal, {15, 15 + waveOff - 0.5}) && covered(shapes, Kind::Touch, {15, 15 + waveOff - 0.5}));

        // A ceiling slope over a cube: dead only past 2 units (T' + 2).
        Slope c;
        c.rect = {0, 100, 30, 130};
        c.direction = 1;  // ceiling, wall on the right
        c.uphill = false;
        shapes.clear();
        slopeShapes(c, cube, o, shapes);
        double const Tc = (130.0 - 15.0) - 21.2132;  // the line at the middle less hy / cos 45
        CHECK(covered(shapes, Kind::Touch, {15, Tc + 1.0}) && !covered(shapes, Kind::Lethal, {15, Tc + 1.9}));
        CHECK(covered(shapes, Kind::Lethal, {15, Tc + 2.1}));
        // ... a ship is only snapped there.
        shapes.clear();
        slopeShapes(c, modePlayer(Mode::Ship, false), o, shapes);
        CHECK(!covered(shapes, Kind::Lethal, {15, Tc + 3.0}));

        // A 45-degree floor slope under a 45-degree ceiling slope: a cube has
        // G - 40.4 of room (21.2 below, 21.2 - 2 above), the classic wave
        // G - 14.1 (RESEARCH.md §5.4).
        for (auto [m, lost] : {std::pair{Mode::Cube, 40.43}, std::pair{Mode::Wave, 14.14}}) {
            Slope f;
            f.rect = {0, 0, 30, 30};
            f.direction = 0;
            f.uphill = true;
            Slope g;
            g.rect = {0, 90, 30, 120};  // its line 90 above the floor slope's
            g.direction = 3;            // ceiling, wall on the left, rising
            g.uphill = true;
            Player const pl = modePlayer(m, false);
            std::vector<Shape> s2;
            slopeShapes(f, pl, o, s2);
            slopeShapes(g, pl, o, s2);
            // The room at the middle column, between the floor slope's region
            // (blue for the cube, red for the wave) and the ceiling slope's red.
            auto below = [&](Vec2 v) {
                return covered(s2, Kind::Touch, v) || covered(s2, Kind::Lethal, v);
            };
            double const from = firstFree(15, 15, 120, 0.001, below);
            double to = from;
            while (to < 120 && !covered(s2, Kind::Lethal, {15, to + 0.001}))
                to += 0.001;
            CHECK(std::abs((to - from) - (90.0 - lost)) < 0.02);
        }
    }

    // ---- ground and ceiling
    {
        Ground g;
        g.floorY = 105;
        g.left = 0;
        g.right = 100;
        g.reach = 50;
        Player cube = modePlayer(Mode::Cube, false);
        std::vector<Shape> s;
        groundShapes(g, cube, s);
        CHECK(covered(s, Kind::Touch, {50, 104.9}) && !covered(s, Kind::Touch, {50, 105.1}));
        CHECK(!covered(s, Kind::Lethal, {50, 100}));
        cube.upsideDown = true;
        s.clear();
        groundShapes(g, cube, s);
        CHECK(covered(s, Kind::Lethal, {50, 100}));

        // A corridor: a ship touches both sides, a dual cube dies at the
        // ceiling (or at the floor upside down).
        g.corridor = true;
        g.floorY = 105;
        g.ceilingY = 375;
        Player const ship = modePlayer(Mode::Ship, false);
        s.clear();
        groundShapes(g, ship, s);
        CHECK(covered(s, Kind::Touch, {50, 380}) && covered(s, Kind::Touch, {50, 100}) && !covered(s, Kind::Lethal, {50, 380}));
        Player dualCube = modePlayer(Mode::Cube, false);
        s.clear();
        groundShapes(g, dualCube, s);
        CHECK(covered(s, Kind::Lethal, {50, 380}) && covered(s, Kind::Touch, {50, 100}));
    }

    std::printf("hitborders: %d checks, %s\n", checks, fails == 0 ? "all passed" : "FAILED");
    return fails == 0 ? 0 : 1;
}
