// Hitborders' geometry (src/tools/hitborder_geom), outside the game. Run by
// run_tests.bat.
//
// Two kinds of check:
//  - every region against a brute-force copy of the GD 2.2081 test it stands
//    for, written point by point (the player's center at a point, the test
//    run, hit or not), over a grid of thousands of points around the object.
//    A point may only disagree when the test itself flips within a hair of it
//    (it sits on the edge), or, for circles, within the 0.05 units the
//    polygon drawn around one strays outside it;
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

    // collidedWithObjectInternal's two snaps, the center at c (0x392013-
    // 0x392b6e): whether the block's top or bottom side takes the player
    // before its inner box is tested. A snap clears that block's death.
    bool snapTakes(Rect const& b, Player const& p, Vec2 c) {
        // The classic wave (no D block) skips both (0x392013-0x392023).
        if (p.mode == Mode::Wave && !p.dartSlide)
            return false;
        // thr: 10, or 6 for a ship, UFO, wave or swing (0x391b90-0x391c50).
        bool const six = p.mode == Mode::Ship || p.mode == Mode::Ufo || p.mode == Mode::Wave || p.mode == Mode::Swing;
        double const thr = six ? 6.0 : 10.0;
        bool const flyer = six || p.mode == Mode::Ball;
        bool const forced = p.hitHead || p.flipGravity;  // [rsp+0x35] (0x391e3e-0x391e62)
        // The top side runs for dil (normal gravity; upside down, a flyer) or
        // [rsp+0x35] (0x392351-0x392479), and snaps when the box's bottom edge
        // plus thr reaches the block's top (0x39247f-0x3924a9).
        if ((!p.upsideDown || flyer || forced) && (c.y - p.hy) + thr >= b.y1)
            return true;
        // The bottom side runs upside down, for a flyer, or for [rsp+0x35]
        // (0x39289d-0x3928f2), and snaps when the box's top edge less thr
        // reaches the block's bottom (0x392a35-0x392a45).
        if ((p.upsideDown || flyer || forced) && (c.y + p.hy) - thr <= b.y0)
            return true;
        return false;
    }

    // A block's inner-box death (0x39361b-0x3937f8), the center at c: the
    // inner box meets the rect and no snap came first.
    bool blockKills(Rect const& b, Player const& p, Vec2 c) {
        return rectsTouch(b, playerRect(c, p.ix, p.iy)) && !snapTakes(b, p, c);
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
        int result = 0;
        auto asBlock = [&](Rect strip) {
            if (!rectsTouch(strip, outer))
                return;
            result = std::max(result, 1);
            if (o.core && !p.platformer && !s.passable && blockKills(strip, p, c))
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

        // Distance to the edge, from inside and out (the audit's "missed by").
        auto square = rectPoly({0, 0, 10, 10});
        CHECK(near(distanceToEdge(square, {5, 4}), 4.0));
        CHECK(near(distanceToEdge(square, {13, 14}), 5.0));
        CHECK(near(distanceToEdge(square, {-0.5, 5}), 0.5));

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
    // A saw turned by a trigger: legacy, the circle and the rotated test both;
    // with Fix radius collision, the circle alone (0x214879-0x21488a skip the
    // rotated test for an object with a radius).
    for (bool fix : {false, true}) {
        Player p = modePlayer(Mode::Cube, false);
        p.rotation = 17.0;
        Options of = o;
        of.fixRadius = fix;
        Collider saw;
        saw.radius = 20.0;
        saw.center = {300, 200};
        saw.oriented = true;
        saw.box = objectObb({300, 200}, 30, 30, 33.0);
        saw.rect = boundsOf(saw.box);
        std::vector<Shape> shapes;
        colliderShapes(saw, p, of, Kind::Lethal, shapes);
        CHECK(agree(fix ? "fix radius saw turned by a trigger" : "legacy saw turned by a trigger",
                    {230, 130, 370, 270}, 0.67, 0.06, [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                    [&](Vec2 c) { return hazardHits(saw, p, of, c); }));
    }
    // A circle is drawn around the true one: every point of the circle is in
    // the polygon, and no corner strays more than 0.05 out (circleSides).
    for (double r : {6.0, 15.0, 21.6, 32.0, 47.0, 120.0}) {
        Player const p = modePlayer(Mode::Cube, false);
        Options of = o;
        of.fixRadius = true;
        Collider saw;
        saw.radius = r;
        saw.center = {10, 20};
        std::vector<Shape> shapes;
        colliderShapes(saw, p, of, Kind::Lethal, shapes);
        double const grown = r + p.hx;
        bool holds = shapes.size() == 1;
        for (int k = 0; k < 3600 && holds; ++k) {
            double const a = k * kPi / 1800.0;
            holds = contains(shapes[0].poly, {10 + grown * std::cos(a), 20 + grown * std::sin(a)}, 1e-9);
        }
        double furthest = 0.0;
        for (auto const& v : shapes.empty() ? std::vector<Vec2>{} : shapes[0].poly)
            furthest = std::max(furthest, std::hypot(v.x - 10, v.y - 20) - grown);
        CHECK(holds && furthest > 0.0 && furthest <= 0.05);
    }
    // The hearts, by the numbers (RESEARCH.md §5.1): r = 15 reaches 15 straight
    // up from a cube's square, not 30; r = 32 reaches 43.3. With r = 15 the
    // two top lobes only touch, right on that line, where a side of the
    // polygon drawn around one lies along it for about 1.2 units either way
    // (all of it under 0.05 outside the lobe's circle): the line reaches that
    // much further, never short.
    {
        Player const p = modePlayer(Mode::Cube, false);
        for (auto [r, reach, sliver] : {std::tuple{15.0, 15.0, 1.5}, std::tuple{32.0, 43.27, 0.1}}) {
            Collider saw;
            saw.radius = r;
            saw.center = {0, 0};
            std::vector<Shape> shapes;
            colliderShapes(saw, p, o, Kind::Lethal, shapes);
            double const top = firstFree(0.0, 0.0, 100.0, 0.01, [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); });
            CHECK(top >= reach - 0.01 && top - reach < sliver);
            // ... and how far that is from GD's own lobe: the 0.05 a corner
            // may stray, and the scan's 0.01 step.
            double const out = std::hypot(0.0 - (-p.hx), top - p.hy) - r;
            CHECK(out < 0.06);
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

    // ---- blocks: blue where the outer box touches, red where the inner one
    // does and no snap takes the player first; every mode, size and gravity,
    // and the D block (a sliding wave), the H block and block 2866
    {
        int cases = 0;
        int bad = 0;
        for (Mode m : {Mode::Cube, Mode::Ship, Mode::Ball, Mode::Ufo, Mode::Wave, Mode::Robot, Mode::Spider,
                       Mode::Swing}) {
            for (bool mini : {false, true}) {
                for (bool upside : {false, true}) {
                    for (int extra = 0; extra < 3; ++extra) {
                        Player p = modePlayer(m, mini);
                        p.upsideDown = upside;
                        p.dartSlide = extra == 1;
                        p.hitHead = extra == 2 && !upside;
                        p.flipGravity = extra == 2 && upside;
                        Block block;
                        block.rect = {500, 90, 530, 120};
                        std::vector<Shape> shapes;
                        blockShapes(block, p, o, shapes);
                        char name[96];
                        std::snprintf(name, sizeof name, "block, %s%s%s%s", modeName(m), mini ? " mini" : "",
                                      upside ? " upside down" : "",
                                      extra == 1 ? ", D block" : extra == 2 ? (upside ? ", block 2866" : ", H block") : "");
                        ++cases;
                        bool const touch = agree(std::string(name) + " (touch)", {440, 30, 590, 180}, 0.73, 1e-4,
                                                 [&](Vec2 c) { return covered(shapes, Kind::Touch, c); },
                                                 [&](Vec2 c) { return rectsTouch(block.rect, playerRect(c, p.hx, p.hy)); });
                        bool const dead = agree(std::string(name) + " (dead)", {440, 30, 590, 180}, 0.73, 1e-4,
                                                [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                                                [&](Vec2 c) { return blockKills(block.rect, p, c); });
                        if (!touch || !dead)
                            ++bad;
                    }
                }
            }
        }
        std::printf("blocks: %d cases\n", cases);
        CHECK(bad == 0);
    }
    // The snaps by the numbers (the probe's findings in the game, 2026-11-02):
    // the core's landing side for a block at [0,30] x [0,30], top 30.
    {
        Block block;
        block.rect = {0, 0, 30, 30};
        auto coreTop = [&](Player const& p) {
            std::vector<Shape> s;
            blockShapes(block, p, o, s);
            return firstFree(15.0, 15.0, 60.0, 0.001, [&](Vec2 c) { return covered(s, Kind::Lethal, c); });
        };
        auto coreBottom = [&](Player const& p) {
            std::vector<Shape> s;
            blockShapes(block, p, o, s);
            double y = 15.0;
            while (y > -30.0 && covered(s, Kind::Lethal, {15.0, y - 0.001}))
                y -= 0.001;
            return y;
        };
        // A cube's inner box decides (15 - 10 = 5 > 4.5): 34.5 and -4.5.
        CHECK(std::abs(coreTop(modePlayer(Mode::Cube, false)) - 34.5) < 0.01);
        CHECK(std::abs(coreBottom(modePlayer(Mode::Cube, false)) + 4.5) < 0.01);
        // A mini cube lands from 9 - 10 = 1 below the top: the core loses 5.5
        // units there (Jumper's wall: the probe's mini cube at y 154.49 over a
        // top of 150 was landed at 159).
        CHECK(std::abs(coreTop(modePlayer(Mode::Cube, true)) - 29.0) < 0.01);
        CHECK(std::abs(coreBottom(modePlayer(Mode::Cube, true)) + 4.5) < 0.01);
        // A spider: 13.5 - 10 = 3.5 above the top, 0.55 off the core.
        CHECK(std::abs(coreTop(modePlayer(Mode::Spider, false)) - 33.5) < 0.01);
        // A mini ship: 9 - 6 = 3 on both sides (landed, and snapped under).
        CHECK(std::abs(coreTop(modePlayer(Mode::Ship, true)) - 33.0) < 0.01);
        CHECK(std::abs(coreBottom(modePlayer(Mode::Ship, true)) + 3.0) < 0.01);
        // A mini ball: thr 10 both sides, 1 past each edge.
        CHECK(std::abs(coreTop(modePlayer(Mode::Ball, true)) - 29.0) < 0.01);
        CHECK(std::abs(coreBottom(modePlayer(Mode::Ball, true)) - 1.0) < 0.01);
        // The classic wave is never snapped; a mini cube upside down lands
        // under the block instead.
        CHECK(std::abs(coreTop(modePlayer(Mode::Wave, true)) - 31.5) < 0.01);
        Player flipped = modePlayer(Mode::Cube, true);
        flipped.upsideDown = true;
        CHECK(std::abs(coreTop(flipped) - 34.5) < 0.01);
        CHECK(std::abs(coreBottom(flipped) - 1.0) < 0.01);
        // Clubstep 2's false alarm in real play: a mini cube's center 0.70
        // under the top of the old core at a block's top-left corner, landed.
        Block corner;
        corner.rect = {11600, 180, 11630, 210};
        std::vector<Shape> s;
        blockShapes(corner, modePlayer(Mode::Cube, true), o, s);
        CHECK(!covered(s, Kind::Lethal, {11596.33, 213.80}) && covered(s, Kind::Touch, {11596.33, 213.80}));
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
                    for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Ufo, Mode::Ball, Mode::Robot,
                                   Mode::Spider, Mode::Swing}) {
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

    // ---- slopes in a platformer level: the legs never kill and their ends
    // draw in 5 while on a slope, a right wall counts going right too, no
    // squeeze, and nothing but a hazard slope kills
    {
        int cases = 0;
        int bad = 0;
        for (int dir = 0; dir < 8; ++dir) {
            bool const top = slopeFloorTop(dir);
            bool const wallLeft = slopeWallLeft(dir);
            bool const uphill = top ? wallLeft : !wallLeft;
            for (bool hazard : {false, true}) {
                for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Ball}) {
                    for (bool upside : {false, true}) {
                        for (bool onSlope : {false, true}) {
                            Slope s;
                            s.rect = {600, 150, 660, 180};
                            s.direction = dir;
                            s.uphill = uphill;
                            s.hazard = hazard;
                            s.uniqueId = 7;
                            Player p = modePlayer(m, false);
                            p.platformer = true;
                            p.upsideDown = upside;
                            p.onSlope = onSlope;
                            p.ridingSlopeId = onSlope ? 3 : 0;
                            p.ridingSlopeTop = !top;  // a squeeze, were it not a platformer
                            std::vector<Shape> shapes;
                            slopeShapes(s, p, o, shapes);
                            char name[128];
                            std::snprintf(name, sizeof name, "platformer slope dir %d%s, %s%s%s", dir,
                                          hazard ? " hazard" : "", modeName(m), upside ? " upside down" : "",
                                          onSlope ? " on a slope" : "");
                            ++cases;
                            bool const okDead = agree(std::string(name) + " (dead)", {550, 100, 710, 230}, 0.53, 1e-4,
                                                      [&](Vec2 c) { return covered(shapes, Kind::Lethal, c); },
                                                      [&](Vec2 c) { return slopeEffect(s, p, o, c) == 2; });
                            bool const okAny = agree(
                                std::string(name) + " (touched)", {550, 100, 710, 230}, 0.53, 1e-4,
                                [&](Vec2 c) { return covered(shapes, Kind::Lethal, c) || covered(shapes, Kind::Touch, c); },
                                [&](Vec2 c) { return slopeEffect(s, p, o, c) >= 1; });
                            if (!okDead || !okAny)
                                ++bad;
                            // Only a hazard slope has any red.
                            bool anyRed = false;
                            for (auto const& sh : shapes)
                                anyRed = anyRed || sh.kind == Kind::Lethal;
                            if (anyRed != hazard)
                                ++bad;
                        }
                    }
                }
            }
        }
        std::printf("platformer slopes: %d cases\n", cases);
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

    // ---- the accuracy probe's points (edgeSamples): a hair either side of
    // every drawn edge, each point says what the point-by-point rules say
    // there - the question gucci_hitborder_probe asks GD itself in the game
    {
        int points = 0;
        int bad = 0;
        auto regionFromEffect = [](int effect) {
            return effect == 2 ? Region::Lethal : effect == 1 ? Region::Touch : Region::None;
        };
        auto sampleAgainst = [&](std::string const& name, std::vector<Shape> const& shapes,
                                 std::vector<Circle> const& circles, auto rule) {
            size_t skipped = 0;
            auto const samples = edgeSamples(shapes, circles, 0.01, 3, &skipped);
            int wrong = 0;
            if (samples.empty()) {
                std::printf("  %s: no points\n", name.c_str());
                wrong = 1;
            }
            for (auto const& s : samples) {
                ++points;
                Region const in = rule(s.in);
                Region const out = rule(s.out);
                bool ok = in == s.inside && out == s.outside && s.inside != s.outside &&
                          near(std::hypot(s.normal.x, s.normal.y), 1.0, 1e-9);
                if (!ok && wrong < 3)
                    std::printf("  %s: at (%.3f, %.3f) the rules say %d inside / %d outside, the drawing %d / %d\n",
                                name.c_str(), s.on.x, s.on.y, (int)in, (int)out, (int)s.inside, (int)s.outside);
                wrong += ok ? 0 : 1;
            }
            if (wrong > 0) {
                std::printf("  %s: %d of %zu points disagree\n", name.c_str(), wrong, samples.size());
                ++bad;
            }
            return samples;
        };

        // A spike: four edges, three points each, red inside and nothing out.
        {
            Player const p = modePlayer(Mode::Cube, false);
            Collider spike;
            spike.rect = {100, 90, 106, 102};
            std::vector<Shape> shapes;
            colliderShapes(spike, p, o, Kind::Lethal, shapes);
            size_t skipped = 99;
            auto const samples = edgeSamples(shapes, {}, 0.01, 3, &skipped);
            CHECK(samples.size() == 12 && skipped == 0);
            bool onEdge = true;
            for (auto const& s : samples) {
                onEdge = onEdge && s.inside == Region::Lethal && s.outside == Region::None && !s.arc;
                onEdge = onEdge && (near(s.on.x, 85) || near(s.on.x, 121) || near(s.on.y, 75) || near(s.on.y, 117));
                onEdge = onEdge && near(std::hypot(s.out.x - s.in.x, s.out.y - s.in.y), 0.02, 1e-9);
            }
            CHECK(onEdge);
        }
        // A block: the touch zone's edges (blue in, nothing out) and the
        // core's (red in, blue out).
        {
            Block block;
            block.rect = {500, 90, 530, 120};
            std::vector<Shape> shapes;
            blockShapes(block, modePlayer(Mode::Cube, false), o, shapes);
            auto const samples = edgeSamples(shapes, {}, 0.01, 3);
            int zone = 0;
            int core = 0;
            for (auto const& s : samples) {
                zone += s.inside == Region::Touch && s.outside == Region::None;
                core += s.inside == Region::Lethal && s.outside == Region::Touch;
            }
            CHECK(samples.size() == 24 && zone == 12 && core == 12);
        }
        // Two spikes side by side in one list: where they overlap, the edges
        // inside the other one are no border, and are left out.
        {
            Player const p = modePlayer(Mode::Cube, false);
            Collider a, b;
            a.rect = {0, 0, 10, 20};
            b.rect = {20, 0, 30, 20};  // grown by 15, the two overlap from 5 to 25
            std::vector<Shape> shapes;
            colliderShapes(a, p, o, Kind::Lethal, shapes);
            colliderShapes(b, p, o, Kind::Lethal, shapes);
            size_t skipped = 0;
            auto const samples = edgeSamples(shapes, {}, 0.01, 3, &skipped);
            CHECK(skipped > 0 && samples.size() + skipped == 24);
        }
        // Saws: every side of a circle sampled where it touches it, on the
        // circle.
        for (bool fix : {false, true}) {
            Player const p = modePlayer(Mode::Cube, false);
            Options of = o;
            of.fixRadius = fix;
            Collider saw;
            saw.radius = 32.0;
            saw.center = {300, 200};
            saw.rect = {268, 168, 332, 232};
            std::vector<Shape> shapes;
            colliderShapes(saw, p, of, Kind::Lethal, shapes);
            auto const circles = colliderCircles(saw, p, of);
            CHECK(circles.size() == (fix ? 1u : 4u));
            auto const samples = edgeSamples(shapes, circles, 0.01, 3);
            bool onCircle = true;
            size_t arcs = 0;
            for (auto const& s : samples) {
                if (!s.arc)
                    continue;
                ++arcs;
                bool found = false;
                for (auto const& c : circles)
                    found = found || near(std::hypot(s.on.x - c.c.x, s.on.y - c.c.y), c.r, 1e-6);
                onCircle = onCircle && found;
            }
            CHECK(onCircle && arcs > 0);
            // With Fix radius collision the disk is the whole region: every
            // side of it is a point.
            if (fix)
                CHECK(arcs == samples.size() && samples.size() == shapes[0].poly.size());
        }
        // Where two lobes of a legacy saw's hearts meet (the probe's 2026-11-02
        // run: a point 0.01 outside a cleft was drawn empty, though 0.014-0.022
        // inside the next lobe's circle, when the lobes were drawn inside their
        // circles): the radii of Bloodbath's and Clubstep's legacy saws, every
        // size of player. No point of the probe's may disagree with GD's test.
        for (double radius : {21.6, 25.0, 28.5, 40.0, 61.2}) {
            for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Spider}) {
                for (bool mini : {false, true}) {
                    Player const p = modePlayer(m, mini);
                    Collider saw;
                    saw.radius = radius;
                    saw.center = {1185, 345};
                    saw.rect = {1185 - radius, 345 - radius, 1185 + radius, 345 + radius};
                    std::vector<Shape> shapes;
                    colliderShapes(saw, p, o, Kind::Lethal, shapes);
                    char name[96];
                    std::snprintf(name, sizeof name, "probe: hearts r=%.1f, %s%s", radius, modeName(m), mini ? " mini" : "");
                    for (double eps : {0.01, 0.002}) {
                        auto const samples = edgeSamples(shapes, colliderCircles(saw, p, o), eps, 3);
                        int wrong = 0;
                        for (auto const& s : samples) {
                            Region const in = hazardHits(saw, p, o, s.in) ? Region::Lethal : Region::None;
                            Region const out = hazardHits(saw, p, o, s.out) ? Region::Lethal : Region::None;
                            ++points;
                            if (in != s.inside || out != s.outside) {
                                if (wrong < 2)
                                    std::printf("  %s, eps %.3f: at (%.4f, %.4f) GD's test says %d / %d, the drawing %d / %d\n",
                                                name, eps, s.on.x, s.on.y, (int)in, (int)out, (int)s.inside, (int)s.outside);
                                ++wrong;
                            }
                        }
                        if (wrong > 0)
                            ++bad;
                    }
                }
            }
        }
        // The cube's 2-unit band under a ceiling slope is quiet; a ship's is not.
        {
            Slope c;
            c.rect = {0, 100, 30, 130};
            c.direction = 1;
            c.uphill = false;
            for (Mode m : {Mode::Cube, Mode::Ship}) {
                std::vector<Shape> shapes;
                slopeShapes(c, modePlayer(m, false), o, shapes);
                int quiet = 0;
                for (auto const& s : edgeSamples(shapes, {}, 0.01, 3))
                    quiet += s.quietIn;
                CHECK(m == Mode::Cube ? quiet > 0 : quiet == 0);
            }
            std::vector<Shape> shapes;
            slopeShapes(c, modePlayer(Mode::Cube, false), o, shapes);
            double const Tc = (130.0 - 15.0) - 21.2132;
            bool quiet = false;
            CHECK(regionAt(shapes, {15, Tc + 1.0}, &quiet) == Region::Touch && quiet);
            CHECK(regionAt(shapes, {15, Tc + 2.5}, &quiet) == Region::Lethal && !quiet);
        }

        // ... and against the point-by-point rules, object by object.
        for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Spider, Mode::Ufo, Mode::Robot, Mode::Ball,
                       Mode::Swing}) {
            for (bool mini : {false, true}) {
                Player const p = modePlayer(m, mini);
                std::string const who = std::string(modeName(m)) + (mini ? " mini" : "");
                Collider spike;
                spike.rect = {100, 90, 106, 102};
                std::vector<Shape> shapes;
                colliderShapes(spike, p, o, Kind::Lethal, shapes);
                sampleAgainst("probe: spike, " + who, shapes, {},
                              [&](Vec2 c) { return hazardHits(spike, p, o, c) ? Region::Lethal : Region::None; });
                for (bool upside : {false, true}) {
                    Player q = p;
                    q.upsideDown = upside;
                    Block block;
                    block.rect = {500, 90, 530, 120};
                    shapes.clear();
                    blockShapes(block, q, o, shapes);
                    sampleAgainst("probe: block, " + who + (upside ? " upside down" : ""), shapes, {}, [&](Vec2 c) {
                        if (blockKills(block.rect, q, c))
                            return Region::Lethal;
                        return rectsTouch(block.rect, playerRect(c, q.hx, q.hy)) ? Region::Touch : Region::None;
                    });
                }
                Collider portal;
                portal.rect = {400, 100, 425, 190};
                shapes.clear();
                colliderShapes(portal, p, o, Kind::Special, shapes);
                sampleAgainst("probe: portal, " + who, shapes, {},
                              [&](Vec2 c) { return hazardHits(portal, p, o, c) ? Region::Special : Region::None; });
            }
        }
        for (double objRot : {30.0, 45.0, 72.5}) {
            for (double tilt : {0.0, 23.0, -41.0}) {
                Player p = modePlayer(Mode::Cube, false);
                p.rotation = tilt;
                Collider thorn;
                thorn.oriented = true;
                thorn.box = objectObb({200, 120}, 8, 20, objRot);
                thorn.rect = boundsOf(thorn.box);
                std::vector<Shape> shapes;
                colliderShapes(thorn, p, o, Kind::Lethal, shapes);
                char name[96];
                std::snprintf(name, sizeof name, "probe: rotated hazard %.1f, tilt %.1f", objRot, tilt);
                sampleAgainst(name, shapes, {},
                              [&](Vec2 c) { return hazardHits(thorn, p, o, c) ? Region::Lethal : Region::None; });
            }
        }
        for (double radius : {15.0, 32.0, 6.0}) {
            for (Mode m : {Mode::Cube, Mode::Wave}) {
                for (bool fix : {false, true}) {
                    for (bool turned : {false, true}) {
                        Player p = modePlayer(m, false);
                        p.rotation = turned ? 17.0 : 0.0;
                        Options of = o;
                        of.fixRadius = fix;
                        Collider saw;
                        saw.radius = radius;
                        saw.center = {300, 200};
                        saw.rect = {300 - radius, 200 - radius, 300 + radius, 200 + radius};
                        if (turned) {
                            saw.oriented = true;
                            saw.box = objectObb({300, 200}, radius * 1.5, radius * 1.5, 33.0);
                            saw.rect = boundsOf(saw.box);
                        }
                        std::vector<Shape> shapes;
                        colliderShapes(saw, p, of, Kind::Lethal, shapes);
                        char name[96];
                        std::snprintf(name, sizeof name, "probe: saw r=%.0f, %s, %s%s", radius, modeName(m),
                                      fix ? "fix radius" : "legacy", turned ? ", turned" : "");
                        sampleAgainst(name, shapes, colliderCircles(saw, p, of),
                                      [&](Vec2 c) { return hazardHits(saw, p, of, c) ? Region::Lethal : Region::None; });
                    }
                }
            }
        }
        for (int dir = 0; dir < 8; ++dir) {
            bool const top = slopeFloorTop(dir);
            bool const wallLeft = slopeWallLeft(dir);
            bool const uphill = top ? wallLeft : !wallLeft;
            for (double w : {30.0, 60.0}) {
                for (bool hazard : {false, true}) {
                    for (Mode m : {Mode::Cube, Mode::Ship, Mode::Wave, Mode::Ufo, Mode::Ball, Mode::Robot, Mode::Spider,
                                   Mode::Swing}) {
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
                                    std::vector<Shape> shapes;
                                    slopeShapes(s, p, o, shapes);
                                    char name[128];
                                    std::snprintf(name, sizeof name, "probe: slope dir %d %gx30%s, %s%s%s%s", dir, w,
                                                  hazard ? " hazard" : "", modeName(m), mini ? " mini" : "",
                                                  upside ? " upside down" : "", left ? " going left" : "");
                                    sampleAgainst(name, shapes, {},
                                                  [&](Vec2 c) { return regionFromEffect(slopeEffect(s, p, o, c)); });
                                }
                            }
                        }
                    }
                }
            }
        }
        std::printf("probe points: %d\n", points);
        CHECK(bad == 0);
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
