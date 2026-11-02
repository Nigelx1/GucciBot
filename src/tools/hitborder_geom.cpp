#include "tools/hitborder_geom.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gucci::hitborder {

    namespace {

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kInf = std::numeric_limits<double>::infinity();

        double cross(Vec2 o, Vec2 a, Vec2 b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        Rect inflate(Rect r, double dx, double dy) {
            return {r.x0 - dx, r.y0 - dy, r.x1 + dx, r.y1 + dy};
        }

        Rect intersect(Rect a, Rect b) {
            return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
        }

        // Drops repeated and collinear corners (clipping leaves both behind).
        std::vector<Vec2> cleanup(std::vector<Vec2> poly) {
            constexpr double kEps = 1e-9;
            bool changed = true;
            while (changed && poly.size() >= 3) {
                changed = false;
                size_t const n = poly.size();
                for (size_t i = 0; i < n; ++i) {
                    Vec2 const prev = poly[(i + n - 1) % n];
                    Vec2 const cur = poly[i];
                    Vec2 const next = poly[(i + 1) % n];
                    bool const same = std::abs(cur.x - prev.x) < kEps && std::abs(cur.y - prev.y) < kEps;
                    double const len = std::hypot(next.x - prev.x, next.y - prev.y);
                    bool const straight = len > 0 && std::abs(cross(prev, cur, next)) / len < kEps;
                    if (same || straight) {
                        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(i));
                        changed = true;
                        break;
                    }
                }
            }
            if (poly.size() < 3)
                poly.clear();
            return poly;
        }

        bool convexCCW(std::vector<Vec2> const& poly) {
            size_t const n = poly.size();
            if (n < 3)
                return false;
            for (size_t i = 0; i < n; ++i) {
                if (cross(poly[i], poly[(i + 1) % n], poly[(i + 2) % n]) < -1e-9)
                    return false;
            }
            return area(poly) > 0;
        }

        void push(std::vector<Shape>& out, Kind kind, std::vector<Vec2> poly) {
            poly = cleanup(std::move(poly));
            if (poly.size() < 3 || !(area(poly) > 1e-9))
                return;
            out.push_back(Shape{kind, std::move(poly)});
        }

        void pushRect(std::vector<Shape>& out, Kind kind, Rect r) {
            if (!(r.x1 > r.x0) || !(r.y1 > r.y0))
                return;
            out.push_back(Shape{kind, rectPoly(r)});
        }

        // Keeps the part of `poly` where nx*x + ny*y <= d.
        std::vector<Vec2> clipHalf(std::vector<Vec2> const& poly, double nx, double ny, double d) {
            std::vector<Vec2> out;
            size_t const n = poly.size();
            if (n == 0)
                return out;
            out.reserve(n + 2);
            for (size_t i = 0; i < n; ++i) {
                Vec2 const a = poly[i];
                Vec2 const b = poly[(i + 1) % n];
                double const da = nx * a.x + ny * a.y - d;
                double const db = nx * b.x + ny * b.y - d;
                if (da <= 0)
                    out.push_back(a);
                if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                    double const t = da / (da - db);
                    out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
                }
            }
            return out;
        }

        // The cube's 2-unit band under a slope (Shape::quiet): the shapes
        // band() just added from `from` on.
        void markQuiet(std::vector<Shape>& out, size_t from) {
            for (size_t i = from; i < out.size(); ++i)
                out[i].quiet = true;
        }

        // Sides for a circle of radius r, so a corner of the polygon around
        // it strays under 0.05 units from it.
        int circleSides(double r) {
            if (!(r > 0))
                return 16;
            double const n = kPi * std::sqrt(r / (2.0 * 0.05));
            return static_cast<int>(std::clamp(std::ceil(n), 16.0, 160.0));
        }

        // The region between two piecewise-linear bounds over [xa, xb]:
        // lo(x) < y < hi(x). `kinks` are the x where either bound may bend
        // (more is harmless). Each run where the band has room becomes one
        // convex polygon, or one quad per stretch when it is not convex.
        template <class Lo, class Hi>
        void band(std::vector<Shape>& out, Kind kind, double xa, double xb, std::vector<double> kinks, Lo lo, Hi hi) {
            if (!(xb > xa))
                return;
            std::vector<double> xs{xa, xb};
            for (double x : kinks) {
                if (std::isfinite(x) && x > xa && x < xb)
                    xs.push_back(x);
            }
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end(), [](double u, double v) { return std::abs(u - v) < 1e-9; }),
                     xs.end());
            // Split where the band closes or opens.
            std::vector<double> all;
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                all.push_back(xs[i]);
                double const d0 = hi(xs[i]) - lo(xs[i]);
                double const d1 = hi(xs[i + 1]) - lo(xs[i + 1]);
                if ((d0 > 0) != (d1 > 0) && d0 != d1) {
                    double const t = d0 / (d0 - d1);
                    if (t > 0 && t < 1)
                        all.push_back(xs[i] + (xs[i + 1] - xs[i]) * t);
                }
            }
            all.push_back(xs.back());

            auto open = [&](size_t i) {
                double const mid = (all[i] + all[i + 1]) / 2;
                return hi(mid) - lo(mid) > 1e-9;
            };
            size_t i = 0;
            while (i + 1 < all.size()) {
                if (!open(i)) {
                    ++i;
                    continue;
                }
                size_t j = i + 1;
                while (j + 1 < all.size() && open(j))
                    ++j;
                std::vector<Vec2> poly;
                for (size_t q = i; q <= j; ++q)
                    poly.push_back({all[q], lo(all[q])});
                for (size_t q = j + 1; q-- > i;)
                    poly.push_back({all[q], hi(all[q])});
                poly = cleanup(std::move(poly));
                if (convexCCW(poly)) {
                    out.push_back(Shape{kind, std::move(poly)});
                } else {
                    for (size_t q = i; q < j; ++q)
                        push(out, kind,
                             {{all[q], lo(all[q])},
                              {all[q + 1], lo(all[q + 1])},
                              {all[q + 1], hi(all[q + 1])},
                              {all[q], hi(all[q])}});
                }
                i = j;
            }
        }

    } // namespace

    // ---------------------------------------------------------------- helpers

    std::vector<Vec2> rectPoly(Rect r) {
        return {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
    }

    std::vector<Vec2> convexHull(std::vector<Vec2> pts) {
        std::sort(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        pts.erase(std::unique(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }),
                  pts.end());
        size_t const n = pts.size();
        if (n < 3)
            return {};
        std::vector<Vec2> h(2 * n);
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0)
                --k;
            h[k++] = pts[i];
        }
        for (size_t i = n - 1, t = k + 1; i-- > 0;) {
            while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0)
                --k;
            h[k++] = pts[i];
        }
        h.resize(k - 1);
        return h;
    }

    std::array<Vec2, 4> playerBox(double hx, double hy, double rotationDeg) {
        double const a = -rotationDeg * kPi / 180.0;
        double const c = std::cos(a);
        double const s = std::sin(a);
        Vec2 const X{c * hx, s * hx};
        Vec2 const Y{-s * hy, c * hy};
        return {{{-X.x - Y.x, -X.y - Y.y}, {X.x - Y.x, X.y - Y.y}, {X.x + Y.x, X.y + Y.y}, {-X.x + Y.x, -X.y + Y.y}}};
    }

    std::vector<Vec2> minkowski(std::array<Vec2, 4> const& box, std::array<Vec2, 4> const& player) {
        std::vector<Vec2> pts;
        pts.reserve(16);
        for (auto const& q : box) {
            for (auto const& p : player)
                pts.push_back({q.x + p.x, q.y + p.y});
        }
        return convexHull(std::move(pts));
    }

    std::vector<Vec2> clipToRect(std::vector<Vec2> const& poly, Rect r) {
        auto p = clipHalf(poly, -1.0, 0.0, -r.x0);
        p = clipHalf(p, 1.0, 0.0, r.x1);
        p = clipHalf(p, 0.0, -1.0, -r.y0);
        p = clipHalf(p, 0.0, 1.0, r.y1);
        return cleanup(std::move(p));
    }

    std::vector<Vec2> clipConvex(std::vector<Vec2> const& poly, std::vector<Vec2> const& clip) {
        std::vector<Vec2> p = poly;
        size_t const n = clip.size();
        for (size_t i = 0; i < n && !p.empty(); ++i) {
            Vec2 const a = clip[i];
            Vec2 const b = clip[(i + 1) % n];
            // Outward normal of a counter-clockwise edge.
            double const nx = b.y - a.y;
            double const ny = -(b.x - a.x);
            p = clipHalf(p, nx, ny, nx * a.x + ny * a.y);
        }
        return cleanup(std::move(p));
    }

    std::vector<Vec2> circlePoly(Vec2 c, double r, int segments) {
        std::vector<Vec2> out;
        if (!(r > 0))
            return out;
        segments = std::max(segments, 8);
        out.reserve(static_cast<size_t>(segments));
        // Corners at r / cos(pi / n): every side touches the circle at its
        // middle, so nothing GD tests is left out of the drawing.
        double const corner = r / std::cos(kPi / segments);
        for (int i = 0; i < segments; ++i) {
            double const a = 2.0 * kPi * i / segments;
            out.push_back({c.x + corner * std::cos(a), c.y + corner * std::sin(a)});
        }
        return out;
    }

    std::vector<Vec2> offsetConvex(std::vector<Vec2> const& poly, double w) {
        size_t const n = poly.size();
        if (n < 3 || w == 0.0)
            return poly;
        auto normal = [](Vec2 a, Vec2 b) {
            double const dx = b.x - a.x;
            double const dy = b.y - a.y;
            double const len = std::hypot(dx, dy);
            return len > 0 ? Vec2{dy / len, -dx / len} : Vec2{0.0, 0.0};
        };
        std::vector<Vec2> out;
        out.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            Vec2 const prev = poly[(i + n - 1) % n];
            Vec2 const cur = poly[i];
            Vec2 const next = poly[(i + 1) % n];
            Vec2 const n1 = normal(prev, cur);
            Vec2 const n2 = normal(cur, next);
            double const dot = n1.x * n2.x + n1.y * n2.y;
            // Growing past a corner sharper than 60 degrees: a bevel, not a
            // long spike. Shrinking keeps the miter (it is what cocos adds).
            if (w > 0 && dot < -0.5) {
                out.push_back({cur.x + n1.x * w, cur.y + n1.y * w});
                out.push_back({cur.x + n2.x * w, cur.y + n2.y * w});
                continue;
            }
            double const k = w / std::max(1.0 + dot, 1e-6);
            out.push_back({cur.x + (n1.x + n2.x) * k, cur.y + (n1.y + n2.y) * k});
        }
        if (w < 0) {
            // An inset that eats the polygon turns it through its middle; a
            // square comes back the right way round, so check that every edge
            // still runs the way it did.
            for (size_t i = 0; i < n; ++i) {
                Vec2 const a = poly[i];
                Vec2 const b = poly[(i + 1) % n];
                Vec2 const c = out[i];
                Vec2 const d = out[(i + 1) % n];
                if ((d.x - c.x) * (b.x - a.x) + (d.y - c.y) * (b.y - a.y) <= 0)
                    return {};
            }
            if (!convexCCW(out))
                return {};
        }
        return out;
    }

    double area(std::vector<Vec2> const& poly) {
        double a = 0.0;
        size_t const n = poly.size();
        for (size_t i = 0; i < n; ++i) {
            Vec2 const p = poly[i];
            Vec2 const q = poly[(i + 1) % n];
            a += p.x * q.y - q.x * p.y;
        }
        return a / 2.0;
    }

    bool contains(std::vector<Vec2> const& poly, Vec2 p, double eps) {
        size_t const n = poly.size();
        if (n < 3)
            return false;
        for (size_t i = 0; i < n; ++i) {
            Vec2 const a = poly[i];
            Vec2 const b = poly[(i + 1) % n];
            double const len = std::hypot(b.x - a.x, b.y - a.y);
            if (len <= 0)
                continue;
            if (cross(a, b, p) / len < -eps)
                return false;
        }
        return true;
    }

    double distanceToEdge(std::vector<Vec2> const& poly, Vec2 p) {
        double best = std::numeric_limits<double>::infinity();
        size_t const n = poly.size();
        for (size_t i = 0; i < n; ++i) {
            Vec2 const a = poly[i];
            Vec2 const b = poly[(i + 1) % n];
            double const dx = b.x - a.x;
            double const dy = b.y - a.y;
            double const len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            best = std::min(best, std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t)));
        }
        return best;
    }

    bool covered(std::vector<Shape> const& shapes, Kind kind, Vec2 p, double eps) {
        for (auto const& s : shapes) {
            if (s.kind == kind && contains(s.poly, p, eps))
                return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- objects

    void colliderShapes(Collider const& c, Player const& p, Options const& o, Kind kind, std::vector<Shape>& out) {
        double const hx = p.hx;
        double const hy = p.hy;
        // The rotated test runs on top of the others for an oriented object,
        // except after a circle that passed with "Fix radius collision" on
        // (0x214865-0x2148ca).
        bool const rotated = c.oriented && !(o.fixRadius && c.radius > 0);
        std::vector<Vec2> both;
        if (rotated)
            both = minkowski(c.box, playerBox(hx, hy, o.useTilt ? p.rotation : 0.0));

        if (c.radius > 0) {
            if (o.fixRadius) {
                // The player as a circle of half its width (0x211e22-0x211ec0).
                push(out, kind, circlePoly(c.center, c.radius + hx, circleSides(c.radius + hx)));
                return;
            }
            // Legacy (0x211ec5-0x212068): the player's rect holds the center,
            // or one of its corners is strictly within r. NaN's "hearts".
            std::vector<std::vector<Vec2>> parts;
            parts.push_back(rectPoly({c.center.x - hx, c.center.y - hy, c.center.x + hx, c.center.y + hy}));
            for (int sx = -1; sx <= 1; sx += 2) {
                for (int sy = -1; sy <= 1; sy += 2)
                    parts.push_back(circlePoly({c.center.x + sx * hx, c.center.y + sy * hy}, c.radius,
                                               circleSides(c.radius)));
            }
            for (auto& part : parts)
                push(out, kind, rotated ? clipConvex(part, both) : std::move(part));
            return;
        }

        // An inclusive rect test against the outer box (0x214813-0x214863).
        Rect const grown = inflate(c.rect, hx, hy);
        if (!rotated) {
            pushRect(out, kind, grown);
            return;
        }
        push(out, kind, clipToRect(both, grown));
    }

    Rect blockCore(Rect const& r, Player const& p) {
        Rect core = inflate(r, p.ix, p.iy);
        // The classic wave goes straight to the inner box (0x392013-0x392023).
        if (p.mode == Mode::Wave && !p.dartSlide)
            return core;
        bool const six = p.mode == Mode::Ship || p.mode == Mode::Ufo || p.mode == Mode::Wave || p.mode == Mode::Swing;
        double const thr = six ? 6.0 : 10.0;
        // A ship, UFO, wave (sliding), swing or ball is snapped on its head
        // side too; a cube, robot or spider only with an H block or block 2866
        // touched (0x391e3e-0x391e62).
        bool const headSnaps = six || p.mode == Mode::Ball || p.hitHead || p.flipGravity;
        if (!p.upsideDown || headSnaps)
            core.y1 = std::min(core.y1, r.y1 + p.hy - thr);
        if (p.upsideDown || headSnaps)
            core.y0 = std::max(core.y0, r.y0 - p.hy + thr);
        return core;
    }

    void blockShapes(Block const& b, Player const& p, Options const& o, std::vector<Shape>& out) {
        // The outer box touches (lands, snaps, slides); only the inner box
        // kills (0x39361b-0x3937f8), outside platformer, on a block that
        // isn't passable or breakable, and not where a snap comes first.
        pushRect(out, Kind::Touch, inflate(b.rect, p.hx, p.hy));
        if (o.core && !p.platformer && !b.passable && !b.breakable)
            pushRect(out, Kind::Lethal, blockCore(b.rect, p));
    }

    double slopeYPos(Slope const& s, double x) {
        Rect const& r = s.rect;
        float const w = static_cast<float>(r.x1 - r.x0);
        float const h = static_cast<float>(r.y1 - r.y0);
        double const k = w != 0.f ? static_cast<double>(h / w) : 0.0;  // GD divides in float
        double y = s.uphill ? r.y0 + (x - r.x0) * k : r.y1 - (x - r.x0) * k;
        if (s.hazard)
            y += slopeFloorTop(s.direction) ? -4.0 : 4.0;
        return y;
    }

    void slopeShapes(Slope const& s, Player const& p, Options const& o, std::vector<Shape>& out) {
        Rect const r = s.rect;
        double const W = r.x1 - r.x0;
        double const H = r.y1 - r.y0;
        if (!(W > 0) || !(H > 0))
            return;
        bool const top = slopeFloorTop(s.direction);  // a ceiling slope: the flat leg on top
        bool const wallLeft = slopeWallLeft(s.direction);
        double const hx = p.hx;
        double const hy = p.hy;

        if (p.sideways) {
            // Rotated gameplay goes through handleRotatedCollisionInternal
            // (0x38f140), which isn't decoded: the triangle grown by the box,
            // red for a hazard slope and blue otherwise.
            double const wx = wallLeft ? r.x0 : r.x1;
            double const ox = wallLeft ? r.x1 : r.x0;
            double const fy = top ? r.y1 : r.y0;
            double const oy = top ? r.y0 : r.y1;
            std::vector<Vec2> pts;
            for (Vec2 const v : {Vec2{wx, fy}, Vec2{wx, oy}, Vec2{ox, fy}}) {
                for (int sx = -1; sx <= 1; sx += 2) {
                    for (int sy = -1; sy <= 1; sy += 2)
                        pts.push_back({v.x + sx * hx, v.y + sy * hy});
                }
            }
            push(out, s.hazard ? Kind::Lethal : Kind::Touch, convexHull(std::move(pts)));
            return;
        }

        // ---- the legs (preSlopeCollision 0x38f2e0): 1-unit blocks, each
        // only while the center is past it, tested like any block, snaps
        // included.
        bool const legsKill = o.core && !p.platformer && !s.passable;
        double const margin = (p.platformer && p.onSlope) ? 5.0 : 0.0;
        auto leg = [&](Rect strip, Rect beyond) {
            pushRect(out, Kind::Touch, intersect(inflate(strip, hx, hy), beyond));
            if (legsKill)
                pushRect(out, Kind::Lethal, intersect(blockCore(strip, p), beyond));
        };
        // A left wall always counts (cx < x0); a right one, in classic mode,
        // only going left (or when the slope outruns you, not drawn).
        bool const rightWallCounts = !wallLeft && (p.platformer || p.goingLeft);
        if (wallLeft) {
            double const e = margin + ((p.goingLeft && !p.platformer) ? 1.0 : 0.0);
            leg({r.x0, r.y0 + e, r.x0 + 1.0, r.y1 - e}, {-kInf, -kInf, r.x0, kInf});
        } else if (rightWallCounts) {
            double const e = margin + ((!p.goingLeft && !p.platformer) ? 1.0 : 0.0);
            leg({r.x1 - 1.0, r.y0 + e, r.x1, r.y1 - e}, {r.x1, -kInf, kInf, kInf});
        }
        if (top)
            leg({r.x0, r.y1 - 1.0, r.x1, r.y1}, {-kInf, r.y1, kInf, kInf});
        else
            leg({r.x0, r.y0, r.x1, r.y0 + 1.0}, {-kInf, -kInf, kInf, r.y0 + 1.0});

        // ---- the hypotenuse (collidedWithSlopeInternal 0x38f810)
        // Does the slope run downhill the way the player goes (r15b,
        // 0x38f8d1-0x38f9f8)? It decides the tolerances below.
        bool down = !s.uphill;
        if (p.goingLeft)
            down = !down;
        if (p.upsideDown)
            down = !down;
        // Riding one slope into one of the other kind (0x38fd03-0x38fd3d).
        bool const squeeze = p.onSlope && p.ridingSlopeId != s.uniqueId && p.ridingSlopeTop != top;
        double const sq = (squeeze && !p.platformer) ? 20.0 * p.vehicleSize : 0.0;
        // The center rides hy / cos(theta) off the line: the box lying flat
        // on it (0x38fcd2-0x38fcfa, in float as GD has it).
        float const theta = std::atan(static_cast<float>(H) / static_cast<float>(W));
        double const off = static_cast<double>(static_cast<float>(2.0 * hy) / std::cos(theta) * 0.5f);
        // The tolerances (0x38fab3-0x38fb94, 0x38fe2b-0x38fefb, 0x38ff71-0x38ffbd).
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

        bool const riding = top == p.upsideDown;  // normal gravity rides floor slopes
        bool const classicWave = p.mode == Mode::Wave && !p.dartSlide && !p.platformer && !p.hitHead;
        bool const squeezeKills = squeeze && !p.platformer && !p.hitHead;
        // What a hit does (0x3900e9-0x3902e1): a hazard slope, the classic
        // wave and a squeeze kill; a cube, robot or spider under a slope dies
        // only past 2 units, whatever the slope; anything else is snapped.
        bool const cubeHead = !riding && p.cubeLike() && !p.platformer && !p.hitHead;
        Kind const hitKind = (s.hazard || classicWave || squeezeKills) ? Kind::Lethal : Kind::Touch;
        double const tol = riding ? tolRide : tolHead;

        // Where the center is tested at all: its box over the slope's rect,
        // not past a leg, and (a fresh contact) over [x0,x1] x [y0+1,y1-1].
        double xa = r.x0 - hx;
        double xb = r.x1 + hx;
        if (wallLeft)
            xa = std::max(xa, r.x0);
        else if (rightWallCounts)
            xb = std::min(xb, r.x1);

        double const kLine = static_cast<double>(static_cast<float>(H) / static_cast<float>(W));
        double const lineB = s.uphill ? kLine : -kLine;
        double const lineA = slopeYPos(s, 0.0);  // y = lineA + lineB * x
        auto xWhereLine = [&](double y) { return (y - lineA) / lineB; };

        if (!top) {
            // A floor slope: T(x) = max(min(L + off, y1 + hy), y0), the
            // center hit below it (0x38fd73-0x38fdaf).
            double const cap = r.y1 + hy - sq;
            double const freshTop = r.y1 - 1.0 + hy;
            auto T = [&](double x) {
                return std::max(std::min(slopeYPos(s, x) + off - sq, cap), r.y0);
            };
            std::vector<double> kinks;
            for (double t : {cap, r.y0, freshTop - tol, freshTop + 2.0, freshTop, r.y0 + 1.0 - tol, r.y0 + 3.0})
                kinks.push_back(xWhereLine(t - off + sq));
            // (cap / y0: T's own bends; freshTop - tol: T + tol meets the
            // fresh-contact top; freshTop + 2 and y0 + 3: the cube's T - 2.)
            auto lo = [&](double) { return r.y0 + 1.0; };
            if (cubeHead) {
                // Upside down under a floor slope: dead below T - 2.
                band(out, Kind::Lethal, xa, xb, kinks, lo, [&](double x) { return std::min(T(x) - 2.0, freshTop); });
                size_t const quietFrom = out.size();
                band(out, Kind::Touch, xa, xb, kinks, [&](double x) { return std::max(r.y0 + 1.0, T(x) - 2.0); },
                     [&](double x) { return std::min(T(x) + tol, freshTop); });
                markQuiet(out, quietFrom);
            } else {
                band(out, hitKind, xa, xb, kinks, lo, [&](double x) { return std::min(T(x) + tol, freshTop); });
            }
        } else {
            // A ceiling slope: T'(x) = min(max(L - off, y0 - hy), y1), the
            // center hit above it (0x38fdb1-0x38fdf0).
            double const floorCap = r.y0 - hy + sq;
            double const freshBottom = r.y0 + 1.0 - hy;
            auto T = [&](double x) {
                return std::min(std::max(slopeYPos(s, x) - off + sq, floorCap), r.y1);
            };
            std::vector<double> kinks;
            for (double t : {floorCap, r.y1, freshBottom + tol, freshBottom - 2.0, freshBottom, r.y1 + tol, r.y1 - 2.0})
                kinks.push_back(xWhereLine(t + off - sq));
            auto hi = [&](double) { return r.y1; };
            if (cubeHead) {
                // A cube, robot or spider with a slope over its head: dead
                // past T' + 2 (0x390194-0x390224).
                band(out, Kind::Lethal, xa, xb, kinks, [&](double x) { return std::max(T(x) + 2.0, freshBottom); }, hi);
                size_t const quietFrom = out.size();
                band(out, Kind::Touch, xa, xb, kinks, [&](double x) { return std::max(T(x) - tol, freshBottom); },
                     [&](double x) { return std::min(r.y1, T(x) + 2.0); });
                markQuiet(out, quietFrom);
            } else {
                band(out, hitKind, xa, xb, kinks, [&](double x) { return std::max(T(x) - tol, freshBottom); }, hi);
            }
        }
    }

    void groundShapes(Ground const& g, Player const& p, std::vector<Shape>& out) {
        if (p.sideways || !(g.right > g.left))
            return;
        // Out of a corridor, an upside-down player under the floor dies (two
        // steps running, 0x213b1b-0x213ba9 and 0x214168). In one, a cube or
        // robot dies against the far side: the ceiling, or the floor upside
        // down (0x213cd0-0x213d8d, 0x213f2d-0x213fee).
        bool const cubeOrRobot = (p.mode == Mode::Cube || p.mode == Mode::Robot) && !p.hitHead;
        Kind floorKind = Kind::Touch;
        Kind ceilingKind = Kind::Touch;
        if (!g.corridor) {
            if (p.upsideDown)
                floorKind = Kind::Lethal;
        } else if (cubeOrRobot) {
            if (p.upsideDown)
                floorKind = Kind::Lethal;
            else
                ceilingKind = Kind::Lethal;
        }
        pushRect(out, floorKind, {g.left, g.floorY - g.reach, g.right, g.floorY});
        if (g.corridor)
            pushRect(out, ceilingKind, {g.left, g.ceilingY, g.right, g.ceilingY + g.reach});
    }

    // ---------------------------------------------------------------- the accuracy probe

    Region regionAt(std::vector<Shape> const& shapes, Vec2 p, bool* quiet) {
        bool lethal = false;
        bool touch = false;
        bool loud = false;  // a touch region that isn't quiet holds it
        bool special = false;
        for (auto const& s : shapes) {
            if (!contains(s.poly, p))
                continue;
            switch (s.kind) {
            case Kind::Lethal: lethal = true; break;
            case Kind::Touch:
                touch = true;
                loud = loud || !s.quiet;
                break;
            case Kind::Special: special = true; break;
            }
        }
        if (quiet)
            *quiet = !lethal && touch && !loud;
        if (lethal)
            return Region::Lethal;
        if (touch)
            return Region::Touch;
        return special ? Region::Special : Region::None;
    }

    std::vector<Circle> colliderCircles(Collider const& c, Player const& p, Options const& o) {
        std::vector<Circle> out;
        if (!(c.radius > 0))
            return out;
        if (o.fixRadius) {
            out.push_back({c.center, c.radius + p.hx});
            return out;
        }
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2)
                out.push_back({{c.center.x + sx * p.hx, c.center.y + sy * p.hy}, c.radius});
        }
        return out;
    }

    std::vector<EdgeSample> edgeSamples(std::vector<Shape> const& shapes, std::vector<Circle> const& circles,
                                        double eps, int perEdge, size_t* skipped) {
        std::vector<EdgeSample> out;
        size_t dropped = 0;
        perEdge = std::max(perEdge, 1);
        auto tolerance = [](Circle const& c) { return 1e-7 * std::max(1.0, c.r); };
        // The circle a side touches: circlePoly's sides all do, at their
        // middles, and so does what clipping leaves of one. Its line is r
        // from the center, with the center on its inner side, and its ends
        // are no further out than a corner of the coarsest polygon circlePoly
        // makes (8 sides).
        double const kFurthest = 1.0 / std::cos(kPi / 8.0);
        auto touching = [&](Vec2 a, Vec2 b, double len) -> Circle const* {
            for (auto const& c : circles) {
                double const tol = tolerance(c);
                if (std::abs(cross(a, b, c.c) / len - c.r) > tol)
                    continue;
                double const reach = c.r * kFurthest + tol;
                if (std::hypot(a.x - c.c.x, a.y - c.c.y) <= reach && std::hypot(b.x - c.c.x, b.y - c.c.y) <= reach)
                    return &c;
            }
            return nullptr;
        };
        auto add = [&](Vec2 on, Vec2 n, size_t si, size_t ei, bool arc) {
            EdgeSample s;
            s.on = on;
            s.normal = n;
            s.in = {on.x - eps * n.x, on.y - eps * n.y};
            s.out = {on.x + eps * n.x, on.y + eps * n.y};
            s.inside = regionAt(shapes, s.in, &s.quietIn);
            s.outside = regionAt(shapes, s.out);
            s.shape = si;
            s.edge = ei;
            s.arc = arc;
            if (s.inside == s.outside) {
                ++dropped;
                return;
            }
            out.push_back(s);
        };
        for (size_t si = 0; si < shapes.size(); ++si) {
            auto const& poly = shapes[si].poly;
            size_t const n = poly.size();
            if (n < 3)
                continue;
            for (size_t ei = 0; ei < n; ++ei) {
                Vec2 const a = poly[ei];
                Vec2 const b = poly[(ei + 1) % n];
                double const dx = b.x - a.x;
                double const dy = b.y - a.y;
                double const len = std::hypot(dx, dy);
                if (!(len > 1e-9))
                    continue;
                Vec2 const normal{dy / len, -dx / len};  // outward of a counter-clockwise edge
                // A side of a circle: sampled where it touches, which is on
                // the circle, along the radius (the side's own normal there).
                // A piece clipping left of one without that point lies
                // outside the circle all along, by up to 0.05: left out.
                if (Circle const* c = touching(a, b, len)) {
                    double const t = ((c->c.x - a.x) * dx + (c->c.y - a.y) * dy) / (len * len);
                    if (t >= -1e-9 && t <= 1.0 + 1e-9)
                        add({a.x + dx * t, a.y + dy * t}, normal, si, ei, true);
                    continue;
                }
                for (int k = 1; k <= perEdge; ++k) {
                    double const t = static_cast<double>(k) / (perEdge + 1);
                    add({a.x + dx * t, a.y + dy * t}, normal, si, ei, false);
                }
            }
        }
        if (skipped)
            *skipped = dropped;
        return out;
    }

} // namespace gucci::hitborder
