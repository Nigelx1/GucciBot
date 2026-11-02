// SolarBot's scene: "the Orrery on the dark sky", the v2 mockup Nigel picked
// on 2026-11-02. Janet, Solar Flare's smiling sun, looks in from the top-right
// corner at the middle of an orrery: thin orbit rings round her, three dotted
// rings turning, four small planets riding the orbits. Gold constellations
// with four-point sparkles in the header and the sidebar, a cyan-cored
// sunburst medallion, a jeweled kite, warm bokeh and lens-flare twinkles, and
// the end card's scalloped corona bands along the top and bottom, all on the
// level's warm near-black (#1A0800).
//
// Drawn from image layers (resources/solar/*.png, which tools/solar_art.py
// makes along with solar_art.hpp, the tables that place them) and, for the
// rings round her, ImGui's own lines, on the menu window's draw list before
// its children, so the cards stand in front: it shows in the sidebar, the
// page's header, between the cards and below the last one. The sky is fitted
// to the window; Janet and everything round her hold to the top-right corner
// (smaller in a narrow window, so she keeps back from the header's words), the
// bands to the top and bottom edges, and each mark to the edge or the part of
// the page it was placed by. Nothing reaches past the window's rounded
// corners. With Animations on the dotted rings and the medallion's rings turn,
// the planets rock along their orbits, her fire rises and her glow breathes,
// the bands drift, and the sparkles, twinkles, bokeh and the kite's gem pulse;
// off, it holds the first frame, which is the mockup. Written fresh; it
// replaces the 2026-10-31 scene (flares and sun gears on orange).

#include "ui/art.hpp"
#include "ui/look.hpp"
#include "ui/pages.hpp"
#include "ui/pages/solar_art.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>

namespace gucci::ui::pages {

    namespace {

        namespace sa = solar_art;

        constexpr float kPi = 3.14159265f;
        // Not SolarBot any more: its textures go this long after the backdrop
        // was last drawn.
        constexpr double kIdleFree = 5.0;

        ImVec4 const kSky{0.102f, 0.031f, 0.000f, 1.f};         // #1A0800, the level's sky (all of it, if the pictures don't load)
        ImVec4 const kEdge{0.773f, 0.325f, 0.031f, 1.f};        // #C55308, its orange outline: the window's and the cards' edge
        ImVec4 const kEdgeInner{0.659f, 0.518f, 0.059f, 1.f};   // #A8840F, a dimmed gold line inside a card's edge
        ImVec4 const kGold{0.965f, 0.812f, 0.063f, 1.f};        // #F6CF10, its line art: the page you're on, framed
        ImVec4 const kFrameOuter{0.824f, 0.376f, 0.055f, 1.f};  // #D2600E, outside that frame
        ImVec4 const kCyan{0.188f, 1.000f, 0.988f, 1.f};        // #30FFFC, its gems: checks, slider grabs, the frame's gem
        ImVec4 const kScroll{1.000f, 0.941f, 0.784f, 1.f};      // #FFF0C8, the page's scroll grab
        ImVec4 const kBad{0.973f, 0.541f, 0.502f, 1.f};         // #F88A80, a red that reads on her cards

        ImU32 col(ImVec4 const& c, float alpha = 1.f) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(alpha, 0.f, 1.f)));
        }

        ImU32 col(float const (&c)[3], float alpha) {
            return col(ImVec4(c[0], c[1], c[2], 1.f), alpha);
        }

        struct Art {
            art::Texture sky, glow, disc, flames, top, sprites;
        };
        // Never destroyed: at exit cocos may be gone before a static would
        // let its textures go. They go in freeArt.
        Art& artLayers() {
            static Art* art = new Art();
            return *art;
        }
        bool g_ready = false;
        bool g_tried = false;  // a load was tried since the textures last went (once, if it failed)
        double g_lastDrawn = -1.0;

        void freeArt() {
            Art& layers = artLayers();
            for (art::Texture* tex : {&layers.sky, &layers.glow, &layers.disc, &layers.flames, &layers.top, &layers.sprites})
                tex->release();
            g_ready = false;
        }

        // Loads every layer the first time it's drawn. On a failure, the
        // plain sky until the textures are next let go (not a try a frame).
        bool ensureArt() {
            if (g_ready)
                return true;
            if (g_tried)
                return false;
            g_tried = true;
            auto const start = std::chrono::steady_clock::now();
            Art& layers = artLayers();
            struct Want {
                art::Texture* tex;
                sa::File file;
            };
            Want const wants[] = {{&layers.sky, sa::kSky},       {&layers.glow, sa::kGlow}, {&layers.disc, sa::kDisc},
                                  {&layers.flames, sa::kFlames}, {&layers.top, sa::kTop},   {&layers.sprites, sa::kSprites}};
            for (auto const& want : wants) {
                std::string error;
                if (!want.tex->load(want.file.name, error)) {
                    geode::log::error("[GucciBot] SolarBot's art: {}", error);
                    freeArt();
                    return false;
                }
                // The tables were made for this picture: one from another
                // build would put every part in the wrong place.
                if (want.tex->width() != want.file.w || want.tex->height() != want.file.h) {
                    geode::log::error("[GucciBot] SolarBot's art: {} is {}x{}, not the {}x{} its table says",
                                      want.file.name, want.tex->width(), want.tex->height(), want.file.w,
                                      want.file.h);
                    freeArt();
                    return false;
                }
            }
            // A line for a load that worked too, so the log tells "loaded" from
            // "never tried" (the theme not shown yet).
            geode::log::info("[GucciBot] SolarBot's art: loaded {} textures in {} ms", std::size(wants),
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
            g_ready = true;
            return true;
        }

        ImVec2 uvOf(art::Texture const& tex, float u, float v) {
            return ImVec2(u / static_cast<float>(tex.width()), v / static_cast<float>(tex.height()));
        }

        // The part of the picture rect `src` (pixels in `tex`) that, laid over
        // [d0, d1], falls inside the window [a, b]; the window's own corners
        // rounded as the window is.
        void drawInWindow(ImDrawList* dl, art::Texture const& tex, ImVec4 src, ImVec2 d0, ImVec2 d1, ImVec2 a, ImVec2 b,
                          float rounding) {
            ImVec2 const c0(std::max(d0.x, a.x), std::max(d0.y, a.y));
            ImVec2 const c1(std::min(d1.x, b.x), std::min(d1.y, b.y));
            if (c1.x <= c0.x || c1.y <= c0.y || d1.x <= d0.x || d1.y <= d0.y)
                return;
            auto const u = [&](float x) { return src.x + (src.z - src.x) * (x - d0.x) / (d1.x - d0.x); };
            auto const v = [&](float y) { return src.y + (src.w - src.y) * (y - d0.y) / (d1.y - d0.y); };
            ImVec2 const uv0 = uvOf(tex, u(c0.x), v(c0.y));
            ImVec2 const uv1 = uvOf(tex, u(c1.x), v(c1.y));
            constexpr float kNear = 0.5f;
            bool const left = c0.x <= a.x + kNear, right = c1.x >= b.x - kNear;
            bool const top = c0.y <= a.y + kNear, bottom = c1.y >= b.y - kNear;
            ImDrawFlags corners = ImDrawFlags_None;
            if (top && left)
                corners |= ImDrawFlags_RoundCornersTopLeft;
            if (top && right)
                corners |= ImDrawFlags_RoundCornersTopRight;
            if (bottom && left)
                corners |= ImDrawFlags_RoundCornersBottomLeft;
            if (bottom && right)
                corners |= ImDrawFlags_RoundCornersBottomRight;
            // (No corner flags would mean every corner to AddImageRounded.)
            if (corners != ImDrawFlags_None && rounding > 0.f)
                dl->AddImageRounded(tex.id(), c0, c1, uv0, uv1, IM_COL32_WHITE, rounding, corners);
            else
                dl->AddImage(tex.id(), c0, c1, uv0, uv1);
        }

        // Draws with `draw` so that nothing lands in the window's corner
        // squares, which its rounding cuts: when the art's box [p0, p1]
        // reaches one, under three clips that leave them out (the middle top
        // to bottom, then each side between its corners), else as it is. What
        // shows in a corner square (the sky, Janet, the bands) is drawn there
        // rounded, on its own.
        template <class Draw>
        void awayFromCorners(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 a, ImVec2 b, float r, Draw&& draw) {
            bool const reaches = r > 0.f && (p0.x < a.x + r || p1.x > b.x - r) && (p0.y < a.y + r || p1.y > b.y - r);
            if (!reaches) {
                draw();
                return;
            }
            ImVec4 const clips[] = {
                {a.x + r, a.y, b.x - r, b.y},
                {a.x, a.y + r, a.x + r, b.y - r},
                {b.x - r, a.y + r, b.x, b.y - r},
            };
            for (auto const& c : clips) {
                if (c.z <= c.x || c.w <= c.y)
                    continue;
                dl->PushClipRect(ImVec2(c.x, c.y), ImVec2(c.z, c.w), true);
                draw();
                dl->PopClipRect();
            }
        }

        // Sprite `i` of the sheet: its centre at `at`, `scale` times its size,
        // multiplied by `tint`, turned `angle` radians clockwise.
        void sprite(ImDrawList* dl, int i, ImVec2 at, float scale, ImU32 tint, ImVec2 a, ImVec2 b, float rounding,
                    float angle = 0.f) {
            art::Texture const& sheet = artLayers().sprites;
            auto const& s = sa::kSpriteRects[i];
            float const x0 = s.left * scale, y0 = s.top * scale;
            float const x1 = x0 + (s.u1 - s.u0) / sa::kPx * scale, y1 = y0 + (s.v1 - s.v0) / sa::kPx * scale;
            ImVec2 const uv0 = uvOf(sheet, s.u0, s.v0), uv1 = uvOf(sheet, s.u1, s.v1);
            if (angle == 0.f) {
                ImVec2 const p0(at.x + x0, at.y + y0), p1(at.x + x1, at.y + y1);
                awayFromCorners(dl, p0, p1, a, b, rounding, [&] { dl->AddImage(sheet.id(), p0, p1, uv0, uv1, tint); });
                return;
            }
            float const cs = std::cos(angle), sn = std::sin(angle);
            auto const turned = [&](float x, float y) { return ImVec2(at.x + x * cs - y * sn, at.y + x * sn + y * cs); };
            ImVec2 const q1 = turned(x0, y0), q2 = turned(x1, y0), q3 = turned(x1, y1), q4 = turned(x0, y1);
            float const reach = std::max({std::fabs(x0), std::fabs(x1), std::fabs(y0), std::fabs(y1)}) * 1.4143f;
            awayFromCorners(dl, ImVec2(at.x - reach, at.y - reach), ImVec2(at.x + reach, at.y + reach), a, b, rounding, [&] {
                dl->AddImageQuad(sheet.id(), q1, q2, q3, q4, uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), tint);
            });
        }

        // Where a mark placed at (x, y) in the first-size menu goes in the
        // window [a, b]: x from the Left or Right edge or Proportionally across
        // the page right of the sidebar, y from the Top or Bottom.
        ImVec2 place(float x, float y, char ax, char ay, ImVec2 a, ImVec2 b) {
            float const w = b.x - a.x;
            float px = a.x + x;
            if (ax == 'R')
                px = b.x - (sa::kDesignW - x);
            else if (ax == 'P')
                px = a.x + sa::kPageLeft + (x - sa::kPageLeft) * (w - sa::kPageLeft) / (sa::kDesignW - sa::kPageLeft);
            float const py = ay == 'B' ? b.y - (sa::kDesignH - y) : a.y + y;
            return ImVec2(px, py);
        }

        // How much of a ring shows at `p`: all of it, but behind the header's
        // words and the sidebar's (the quiet zones, softened by a Gaussian).
        float quiet(ImVec2 p, ImVec2 a, ImVec2 b) {
            float const k = 1.f / (sa::kQuietBlur * 1.41421356f);
            auto const soft = [k](float v, float lo, float hi) {
                return 0.5f * (std::erf((v - lo) * k) - std::erf((v - hi) * k));
            };
            float cover = 0.f;
            for (auto const& z : sa::kQuietZones) {
                float const y1 = z[3] < 0.f ? b.y + z[3] : a.y + z[3];
                cover += soft(p.x, a.x + z[0], a.x + z[2]) * soft(p.y, a.y + z[1], y1);
            }
            return 1.f - (1.f - sa::kQuietFloor) * std::min(1.f, cover);
        }

        // A dash with round ends from p0 to p1, `r` its half width.
        void capsule(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r, ImU32 color) {
            constexpr int kSteps = 5;  // each round end
            float const along = std::atan2(p1.y - p0.y, p1.x - p0.x);
            ImVec2 pts[2 * (kSteps + 1)];
            int n = 0;
            for (int i = 0; i <= kSteps; ++i) {
                float const turn = along - kPi * 0.5f + kPi * static_cast<float>(i) / kSteps;
                pts[n++] = ImVec2(p1.x + r * std::cos(turn), p1.y + r * std::sin(turn));
            }
            for (int i = 0; i <= kSteps; ++i) {
                float const turn = along + kPi * 0.5f + kPi * static_cast<float>(i) / kSteps;
                pts[n++] = ImVec2(p0.x + r * std::cos(turn), p0.y + r * std::sin(turn));
            }
            dl->AddConvexPolyFilled(pts, n, color);
        }

        // The orbits: thin rings round Janet's centre `c`, quiet behind the
        // words. Only their arcs in the window, in pieces of a few units; the
        // pieces of one alpha make one line.
        void orbits(ImDrawList* dl, ImVec2 c, float k, ImVec2 a, ImVec2 b, float rounding) {
            constexpr int kScan = 180;  // 2-degree steps
            constexpr float kStep = 2.f * kPi / kScan;
            constexpr float kPiece = 4.f;  // units
            constexpr float kMargin = 4.f;
            for (auto const& o : sa::kOrbits) {
                float const r = o.r * k;
                auto const at = [&](float turn) { return ImVec2(c.x + r * std::cos(turn), c.y + r * std::sin(turn)); };
                auto const inView = [&](ImVec2 p) {
                    return p.x >= a.x - kMargin && p.x <= b.x + kMargin && p.y >= a.y - kMargin && p.y <= b.y + kMargin;
                };
                bool in[kScan + 1];
                for (int i = 0; i <= kScan; ++i)
                    in[i] = inView(at(kStep * static_cast<float>(i)));
                int const pieces = std::max(1, static_cast<int>(std::ceil(r * kStep / kPiece)));
                float const half = o.width * 0.5f + 1.f;
                float lineAlpha = -1.f;
                int points = 0;
                auto const flush = [&] {
                    if (points >= 2)
                        dl->PathStroke(col(sa::kOrbitColor, lineAlpha), ImDrawFlags_None, o.width);
                    dl->PathClear();
                    points = 0;
                };
                for (int i = 0; i < kScan; ++i) {
                    if (!in[i] && !in[i + 1]) {
                        flush();
                        continue;
                    }
                    for (int j = 0; j < pieces; ++j) {
                        float const t0 = kStep * (static_cast<float>(i) + static_cast<float>(j) / pieces);
                        float const t1 = kStep * (static_cast<float>(i) + static_cast<float>(j + 1) / pieces);
                        ImVec2 const p0 = at(t0), p1 = at(t1);
                        ImVec2 const lo(std::min(p0.x, p1.x) - half, std::min(p0.y, p1.y) - half);
                        ImVec2 const hi(std::max(p0.x, p1.x) + half, std::max(p0.y, p1.y) + half);
                        float const alpha = std::round(o.alpha * quiet(at(0.5f * (t0 + t1)), a, b) * 64.f) / 64.f;
                        bool const corner = rounding > 0.f && (lo.x < a.x + rounding || hi.x > b.x - rounding) &&
                                            (lo.y < a.y + rounding || hi.y > b.y - rounding);
                        if (corner) {
                            // A piece by a corner, on its own and kept out of it.
                            flush();
                            awayFromCorners(dl, lo, hi, a, b, rounding,
                                            [&] { dl->AddLine(p0, p1, col(sa::kOrbitColor, alpha), o.width); });
                            continue;
                        }
                        if (alpha != lineAlpha || points == 0) {
                            ImVec2 const from = p0;
                            flush();
                            lineAlpha = alpha;
                            dl->PathLineTo(from);
                            points = 1;
                        }
                        dl->PathLineTo(p1);
                        ++points;
                    }
                }
                flush();
            }
        }

        // The dotted rings round Janet's centre `c`, turning: short dashes
        // with round ends, quiet behind the words.
        void dottedRings(ImDrawList* dl, ImVec2 c, float k, double time, ImVec2 a, ImVec2 b, float rounding) {
            float const half = sa::kDotWidth * 0.5f;
            for (auto const& d : sa::kDots) {
                float const r = d.r * k;
                double const spin = std::fmod(static_cast<double>(d.rate) * time, 360.0);
                for (int i = 0; i < d.count; ++i) {
                    double const from = (i * 360.0 / d.count + spin) * kPi / 180.0;
                    float const t0 = static_cast<float>(from);
                    float const t1 = t0 + sa::kDotArc * kPi / 180.f;
                    ImVec2 const p0(c.x + r * std::cos(t0), c.y + r * std::sin(t0));
                    ImVec2 const p1(c.x + r * std::cos(t1), c.y + r * std::sin(t1));
                    ImVec2 const lo(std::min(p0.x, p1.x) - half - 1.f, std::min(p0.y, p1.y) - half - 1.f);
                    ImVec2 const hi(std::max(p0.x, p1.x) + half + 1.f, std::max(p0.y, p1.y) + half + 1.f);
                    if (hi.x < a.x || lo.x > b.x || hi.y < a.y || lo.y > b.y)
                        continue;
                    ImVec2 const mid(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y));
                    ImU32 const color = col(sa::kDotColor, sa::kDotAlpha * quiet(mid, a, b));
                    awayFromCorners(dl, lo, hi, a, b, rounding, [&] { capsule(dl, p0, p1, half, color); });
                }
            }
        }

        // A corona band: its tile repeating along y0..y0 + kCoronaHeight,
        // `scroll` units along (the pattern moves left as it grows), kept out
        // of the window's corner squares; in those, the tile's part there,
        // rounded as the window is.
        void band(ImDrawList* dl, int i, float y0, double scroll, ImVec2 a, ImVec2 b, float rounding) {
            art::Texture const& sheet = artLayers().sprites;
            auto const& s = sa::kSpriteRects[i];
            float const period = sa::kCoronaPeriod, tall = sa::kCoronaHeight;
            float phase = static_cast<float>(std::fmod(scroll, static_cast<double>(period)));
            if (phase < 0.f)
                phase += period;
            ImVec2 const uv0 = uvOf(sheet, s.u0, s.v0), uv1 = uvOf(sheet, s.u1, s.v1);
            awayFromCorners(dl, ImVec2(a.x, y0), ImVec2(b.x, y0 + tall), a, b, rounding, [&] {
                for (float x = a.x - phase; x < b.x; x += period)
                    dl->AddImage(sheet.id(), ImVec2(x, y0), ImVec2(x + period, y0 + tall), uv0, uv1);
            });
            float const r = std::min(rounding, period * 0.5f);
            if (r <= 0.f)
                return;
            for (int end = 0; end < 2; ++end) {
                float const cy = end == 0 ? a.y : b.y - r;
                if (cy < y0 - 0.01f || cy + r > y0 + tall + 0.01f)
                    continue;
                for (int side = 0; side < 2; ++side) {
                    float const cx = side == 0 ? a.x : b.x - r;
                    // The tile's part under the corner square (moved back
                    // inside the tile where the square would wrap round it:
                    // a few units of the band's black edge either way).
                    float u = std::fmod(cx - a.x + phase, period);
                    if (u + r > period)
                        u = period - r;
                    float const v = cy - y0;
                    ImVec2 const t0 = uvOf(sheet, s.u0 + u * sa::kPx, s.v0 + v * sa::kPx);
                    ImVec2 const t1 = uvOf(sheet, s.u0 + (u + r) * sa::kPx, s.v0 + (v + r) * sa::kPx);
                    ImDrawFlags const corner =
                        end == 0 ? (side == 0 ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTopRight)
                                 : (side == 0 ? ImDrawFlags_RoundCornersBottomLeft : ImDrawFlags_RoundCornersBottomRight);
                    // ImGui rounds a corner by at most the rect's side less 1:
                    // the rect reaches a unit past the square, clipped back to it.
                    ImVec2 const q0(cx, cy), q1(cx + r, cy + r);
                    ImVec2 const grow(side == 0 ? 0.f : -1.f, end == 0 ? 0.f : -1.f);
                    ImVec2 const p0(q0.x + grow.x, q0.y + grow.y), p1(q1.x + grow.x + 1.f, q1.y + grow.y + 1.f);
                    ImVec2 const per((t1.x - t0.x) / r, (t1.y - t0.y) / r);
                    ImVec2 const s0(t0.x + per.x * grow.x, t0.y + per.y * grow.y);
                    ImVec2 const s1(t1.x + per.x * (grow.x + 1.f), t1.y + per.y * (grow.y + 1.f));
                    dl->PushClipRect(q0, q1, true);
                    dl->AddImageRounded(sheet.id(), p0, p1, s0, s1, IM_COL32_WHITE, r, corner);
                    dl->PopClipRect();
                }
            }
        }

        // A disc of radius R round `c` filled with `tex`, its texture place at
        // each point given by uvAt: a fan of triangles (ImGui has no circle
        // with a texture).
        template <class Uv>
        void texturedDisc(ImDrawList* dl, ImTextureID tex, ImVec2 c, float R, Uv&& uvAt) {
            constexpr int kRim = 96;
            dl->PushTextureID(tex);
            dl->PrimReserve(kRim * 3, kRim + 1);
            auto const first = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            dl->PrimWriteVtx(c, uvAt(c), IM_COL32_WHITE);
            for (int i = 0; i < kRim; ++i) {
                float const turn = 2.f * kPi * static_cast<float>(i) / kRim;
                ImVec2 const p(c.x + R * std::cos(turn), c.y + R * std::sin(turn));
                dl->PrimWriteVtx(p, uvAt(p), IM_COL32_WHITE);
            }
            for (int i = 0; i < kRim; ++i) {
                dl->PrimWriteIdx(first);
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1 + i));
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1 + (i + 1) % kRim));
            }
            dl->PopTextureID();
        }

        // Her flames: their tile over her disc (radius R round `c`), risen
        // `rise` of its height. It wraps top to bottom, so the disc is drawn
        // twice, each time under a clip that holds one side of the wrap.
        void flames(ImDrawList* dl, art::Texture const& tex, ImVec2 c, float R, float k, double rise, ImVec2 a, ImVec2 b,
                    float rounding) {
            float const left = c.x + sa::kFlameBox[0] * k, wide = (sa::kFlameBox[2] - sa::kFlameBox[0]) * k;
            float const top = c.y + sa::kFlameBox[1] * k, tall = (sa::kFlameBox[3] - sa::kFlameBox[1]) * k;
            float const split = top + static_cast<float>(1.0 - rise) * tall;
            ImTextureID const id = tex.id();
            awayFromCorners(dl, ImVec2(c.x - R, c.y - R), ImVec2(c.x + R, c.y + R), a, b, rounding, [&] {
                for (int pass = 0; pass < 2; ++pass) {
                    float const y0 = pass == 0 ? top : split;
                    float const y1 = pass == 0 ? split : top + tall;
                    if (y1 <= y0)
                        continue;
                    float const shift = static_cast<float>(pass == 0 ? rise : rise - 1.0);
                    dl->PushClipRect(ImVec2(c.x - R - 1.f, y0), ImVec2(c.x + R + 1.f, y1), true);
                    texturedDisc(dl, id, c, R,
                                 [&](ImVec2 p) { return ImVec2((p.x - left) / wide, (p.y - top) / tall + shift); });
                    dl->PopClipRect();
                }
            });
        }

        void drawArt(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
            Art const& layers = artLayers();
            float const w = b.x - a.x, h = b.y - a.y;
            // Frozen at the start with Animations off, not hidden.
            double const time = look().effects ? ImGui::GetTime() : 0.0;
            float const t = static_cast<float>(time);
            auto const turned = [&](float rate) {
                return static_cast<float>(std::fmod(static_cast<double>(rate) * time, 360.0)) * kPi / 180.f;
            };

            // Janet's corner: her full size from the menu's first width up and
            // smaller in a narrower window, so she keeps back from the
            // header's words; everything round her with her.
            float const k = std::clamp((w - 500.f) / 220.f, 0.5f, 1.f);
            ImVec2 const c(b.x - sa::kJanetRight * k, a.y + sa::kJanetDown * k);
            float const R = sa::kJanetR * k;

            // The sky, covering the window with its shape kept, from the
            // top-right corner, where it is warm round her.
            {
                float const s = std::max(w / sa::kDesignW, h / sa::kDesignH);
                drawInWindow(dl, layers.sky, ImVec4(0.f, 0.f, static_cast<float>(layers.sky.width()), static_cast<float>(layers.sky.height())),
                             ImVec2(b.x - sa::kDesignW * s, a.y), ImVec2(b.x, a.y + sa::kDesignH * s), a, b, rounding);
            }

            // The orrery: the orbits, the dotted rings turning, and the
            // planets rocking along their orbits.
            orbits(dl, c, k, a, b, rounding);
            dottedRings(dl, c, k, time, a, b, rounding);
            for (size_t i = 0; i < std::size(sa::kPlanets); ++i) {
                auto const& p = sa::kPlanets[i];
                float const deg = p.base + p.amp * std::sin(sa::kPlanetRockRate * t + sa::kPlanetRockStep * static_cast<float>(i));
                float const turn = deg * kPi / 180.f;
                sprite(dl, p.sprite, ImVec2(c.x + p.r * k * std::cos(turn), c.y + p.r * k * std::sin(turn)), k, IM_COL32_WHITE,
                       a, b, rounding);
            }

            // The constellations, their sparkles pulsing.
            for (size_t i = 0; i < std::size(sa::kConstellations); ++i) {
                auto const& cst = sa::kConstellations[i];
                ImVec2 const at = place(cst.x, cst.y, cst.ax, cst.ay, a, b);
                sprite(dl, cst.sprite, at, 1.f, IM_COL32_WHITE, a, b, rounding);
                for (auto const& sp : sa::kSparkles) {
                    if (sp.constellation != static_cast<int>(i))
                        continue;
                    float const size = sp.r * (0.85f + 0.15f * std::sin(2.f * t + static_cast<float>(sp.k + sp.seed)));
                    sprite(dl, sa::kSparkleSprite, ImVec2(at.x + sp.dx, at.y + sp.dy), size / sa::kSparkleR, IM_COL32_WHITE,
                           a, b, rounding);
                }
            }

            // The medallion, its dashed rings turning two ways; the kite, its
            // gem glinting.
            {
                auto const& m = sa::kMedallion;
                ImVec2 const at = place(m.x, m.y, m.ax, m.ay, a, b);
                sprite(dl, sa::kMedallionBody, at, 1.f, IM_COL32_WHITE, a, b, rounding);
                sprite(dl, sa::kMedallionOuter, at, 1.f, IM_COL32_WHITE, a, b, rounding, turned(sa::kMedallionOuterRate));
                sprite(dl, sa::kMedallionInner, at, 1.f, IM_COL32_WHITE, a, b, rounding, turned(sa::kMedallionInnerRate));
            }
            {
                auto const& kite = sa::kKite;
                ImVec2 const at = place(kite.x, kite.y, kite.ax, kite.ay, a, b);
                sprite(dl, sa::kKiteBody, at, 1.f, IM_COL32_WHITE, a, b, rounding);
                sprite(dl, sa::kKiteGlint, at, 1.f, col(ImVec4(1.f, 1.f, 1.f, 1.f), 0.85f + 0.15f * std::sin(1.4f * t + 2.f)),
                       a, b, rounding);
            }

            // The bokeh lights, pulsing, and the twinkles, growing and shrinking.
            for (size_t i = 0; i < std::size(sa::kBokeh); ++i) {
                auto const& l = sa::kBokeh[i];
                float const glow = 0.85f + 0.15f * std::sin(1.6f * t + 2.3f * static_cast<float>(i));
                sprite(dl, sa::kBokehSprite, place(l.x, l.y, l.ax, l.ay, a, b), l.r / sa::kBokehR,
                       col(ImVec4(1.f, 1.f, 1.f, 1.f), glow), a, b, rounding);
            }
            for (size_t i = 0; i < std::size(sa::kTwinkles); ++i) {
                auto const& l = sa::kTwinkles[i];
                float const size = 0.8f + 0.2f * std::sin(2.1f * t + 1.3f * static_cast<float>(i));
                sprite(dl, sa::kTwinkleSprite, place(l.x, l.y, l.ax, l.ay, a, b), l.r / sa::kTwinkleR * size,
                       IM_COL32_WHITE, a, b, rounding);
            }

            // The corona bands, the top one drifting left and the bottom one right.
            band(dl, sa::kCoronaTop, a.y, sa::kCoronaSpeed * time, a, b, rounding);
            band(dl, sa::kCoronaBottom, b.y - sa::kCoronaHeight, -sa::kCoronaSpeed * time, a, b, rounding);

            // Janet: her light breathing, her disc, her fire rising inside it,
            // and her limb and face over the fire.
            auto const herLayer = [&](art::Texture const& tex, float const (&box)[4], float scale) {
                drawInWindow(dl, tex, ImVec4(0.f, 0.f, static_cast<float>(tex.width()), static_cast<float>(tex.height())),
                             ImVec2(c.x + box[0] * scale, c.y + box[1] * scale), ImVec2(c.x + box[2] * scale, c.y + box[3] * scale),
                             a, b, rounding);
            };
            herLayer(layers.glow, sa::kGlowBox, k * (1.f + sa::kBreathe * std::sin(sa::kBreatheRate * t)));
            herLayer(layers.disc, sa::kDiscBox, k);
            flames(dl, layers.flames, c, R, k, std::fmod(static_cast<double>(sa::kFlameRise) * time, 1.0), a, b, rounding);
            herLayer(layers.top, sa::kTopBox, k);
        }

    } // namespace

    // Her gems' blue in the menu's own widgets and the frame's gem, her
    // orange edge round the cards with a gold line inside it, the page you're
    // on framed in her gold, and an opaque scroll grab that reads over her
    // (after look().refresh(), as JupiterBot's look is). And a lighter red
    // for errors: the dark themes' red reads 5.1:1 on her cards and 4.7:1 as
    // a chip over the sky, this one 7.4:1 and 6.3:1 (a Bad button's label over
    // it still 5.7:1). Good and Warn read as they are.
    void solarLookApply() {
        auto& pal = look().pal;
        pal.bad = kBad;
        pal.cardEdge = {true, kEdge, kEdgeInner};
        pal.pageFrame = {true, kFrameOuter, kGold, kGold, kCyan};
        ImVec4* c = ImGui::GetStyle().Colors;
        c[ImGuiCol_CheckMark] = kCyan;
        c[ImGuiCol_SliderGrab] = kCyan;
        c[ImGuiCol_SliderGrabActive] = shade(kCyan, 0.1f);
        c[ImGuiCol_ScrollbarGrab] = kScroll;
        c[ImGuiCol_ScrollbarGrabHovered] = shade(kScroll, 0.06f);
        c[ImGuiCol_ScrollbarGrabActive] = pal.accent;
    }

    void solarBackdrop() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiStyle const& style = ImGui::GetStyle();
        ImVec2 const a = ImGui::GetWindowPos();
        ImVec2 const size = ImGui::GetWindowSize();
        ImVec2 const b(a.x + size.x, a.y + size.y);
        g_lastDrawn = ImGui::GetTime();
        // Out to the window's border, not just its padding; the border is
        // drawn again over the top at the end, in her orange.
        dl->PushClipRect(a, b, false);
        if (ensureArt())
            drawArt(dl, a, b, style.WindowRounding);
        else
            dl->AddRectFilled(a, b, col(kSky), style.WindowRounding);
        dl->PopClipRect();
        // Where ImGui draws a window's border, under a clip a unit wider all
        // round: the window's own clip leaves the border's pixels out, so the
        // line drawn under it showed on the bottom edge alone (2026-11-02).
        if (style.WindowBorderSize > 0.f) {
            dl->PushClipRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), false);
            dl->AddRect(a, b, col(kEdge), style.WindowRounding, 0, style.WindowBorderSize);
            dl->PopClipRect();
        }
    }

    void solarArtIdle(bool keep) {
        if (keep || (!g_ready && !g_tried))
            return;
        if (ImGui::GetTime() - g_lastDrawn < kIdleFree)
            return;
        freeArt();
        g_tried = false;
    }

} // namespace gucci::ui::pages
