// JupiterBot's scene and the JMF Trainer's sky: "Star chart at night", the B
// direction of the 2026-11-01 mockups (Nigel: "B for SURE"). The level's dials
// and its ending: a hand-inked crescent-moon dial looking in from the top-right
// corner, scribbled one-stroke stars, twinkles and blue specks over an
// ultramarine night, and a skyline in the sidebar's foot with lit windows over
// a thin yellow ground line.
//
// Drawn from image layers (resources/jupiter/*.png, which tools/jupiter_art.py
// makes along with jupiter_art.hpp, the tables that place them) on the menu
// window's draw list before its children, so the cards stand in front: it
// shows in the sidebar, the page's header, between the cards and below the
// last one. The sky is fitted to the window; the dial holds to the top-right
// corner, the skyline to the bottom, and each star to the edge or the part of
// the page it was placed by. Colours with their own alpha, so BIG BRRRR's
// flicker doesn't dim it. With Animations on the dial's rings turn (two ways),
// the halos and twinkles pulse, the specks rise and a lit window or two goes
// dark now and then; off, it holds the first frame, which is the mockup.

#include "ui/art.hpp"
#include "ui/look.hpp"
#include "ui/pages.hpp"
#include "ui/pages/jupiter_art.hpp"

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

        namespace ja = jupiter_art;

        constexpr float kPi = 3.14159265f;
        // The dial's turning rings, in degrees a second clockwise: the dots
        // and the bezel one way, the dashed rings the other.
        constexpr double kCwRate = 3.0;
        constexpr double kCcwRate = -2.0;
        constexpr double kSpeckRise = 4.0;  // units a second
        // A theme without this art lets its textures go this long after the
        // backdrop was last drawn (the JMF page left behind).
        constexpr double kIdleFree = 5.0;

        ImVec4 const kSky{0.027f, 0.016f, 0.247f, 1.f};     // #07043F, the menu's sky if the pictures don't load
        ImVec4 const kHalo{0.290f, 0.255f, 0.722f, 1.f};    // #4A41B8, the light round a scribbled star
        ImVec4 const kYellow{0.976f, 0.961f, 0.345f, 1.f};  // #F9F558, the level's line art
        ImVec4 const kGround{0.008f, 0.004f, 0.078f, 1.f};  // #020114, under the ground line
        ImVec4 const kBlue{0.055f, 0.647f, 0.976f, 1.f};    // #0EA5F9, a speck
        ImVec4 const kTeal{0.110f, 0.639f, 0.776f, 1.f};    // #1CA3C6, a speck

        ImU32 col(ImVec4 const& c, float alpha = 1.f) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(alpha, 0.f, 1.f)));
        }

        struct Art {
            art::Texture sky, dial, dialCw, dialCcw, city, sprites;
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
            for (art::Texture* tex :
                 {&layers.sky, &layers.dial, &layers.dialCw, &layers.dialCcw, &layers.city, &layers.sprites})
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
                ja::File file;
            };
            Want const wants[] = {{&layers.sky, ja::kSky},         {&layers.dial, ja::kDial},
                                  {&layers.dialCw, ja::kDialCw},   {&layers.dialCcw, ja::kDialCcw},
                                  {&layers.city, ja::kCity},       {&layers.sprites, ja::kSprites}};
            for (auto const& want : wants) {
                std::string error;
                if (!want.tex->load(want.file.name, error)) {
                    geode::log::error("[GucciBot] JupiterBot's art: {}", error);
                    freeArt();
                    return false;
                }
                // The tables were made for this picture: one from another
                // build would put every part in the wrong place.
                if (want.tex->width() != want.file.w || want.tex->height() != want.file.h) {
                    geode::log::error("[GucciBot] JupiterBot's art: {} is {}x{}, not the {}x{} its table says",
                                      want.file.name, want.tex->width(), want.tex->height(), want.file.w,
                                      want.file.h);
                    freeArt();
                    return false;
                }
            }
            // A line for a load that worked too, so the log tells "loaded" from
            // "never tried" (the theme not shown yet).
            geode::log::info("[GucciBot] JupiterBot's art: loaded {} textures in {} ms", std::size(wants),
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
        void drawInWindow(ImDrawList* dl, art::Texture const& tex, ImVec4 src, ImVec2 d0, ImVec2 d1, ImVec2 a,
                          ImVec2 b, float rounding) {
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

        // A turning ring: a square texture reaching `r` from its centre `c`,
        // turned `angle` radians clockwise. Drawn under two clips that leave
        // out the window's top-right corner square, which the window's
        // rounding cuts and a turned quad can't be.
        void ring(ImDrawList* dl, art::Texture const& tex, ImVec2 c, float r, float angle, ImVec2 a, ImVec2 b,
                  float rounding) {
            float const cs = std::cos(angle), sn = std::sin(angle);
            auto const at = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
            ImVec2 const p1 = at(-r, -r), p2 = at(r, -r), p3 = at(r, r), p4 = at(-r, r);
            auto const quad = [&] {
                dl->AddImageQuad(tex.id(), p1, p2, p3, p4, ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f),
                                 ImVec2(0.f, 1.f));
            };
            float const corner = std::max(0.f, rounding);
            dl->PushClipRect(ImVec2(a.x, a.y + corner), b, true);
            quad();
            dl->PopClipRect();
            if (corner > 0.f) {
                dl->PushClipRect(a, ImVec2(b.x - corner, a.y + corner), true);
                quad();
                dl->PopClipRect();
            }
        }

        // Sprite `i` of the sprite sheet, its centre at `at`, `scale` times
        // its size, multiplied by `tint`.
        void sprite(ImDrawList* dl, int i, ImVec2 at, float scale, ImU32 tint) {
            art::Texture const& sheet = artLayers().sprites;
            auto const& s = ja::kSpriteRects[i];
            ImVec2 const p0(at.x + s.left * scale, at.y + s.top * scale);
            ImVec2 const p1(p0.x + (s.u1 - s.u0) / ja::kPx * scale, p0.y + (s.v1 - s.v0) / ja::kPx * scale);
            dl->AddImage(sheet.id(), p0, p1, uvOf(sheet, s.u0, s.v0), uvOf(sheet, s.u1, s.v1), tint);
        }

        // The part of the window's rounded rect within `height` of its bottom
        // edge, filled: the ground keeps to the window's rounded corners.
        void bottomBand(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float height, ImU32 color) {
            float const r = std::clamp(rounding, 0.f, std::min(b.x - a.x, b.y - a.y) * 0.5f);
            if (r < 0.5f) {
                dl->AddRectFilled(ImVec2(a.x, b.y - height), b, color);
                return;
            }
            // Round each corner from where the band's top meets it (0 is the
            // window's side, a quarter turn its bottom), clockwise.
            float const from = height >= r ? 0.f : std::asin(std::clamp((r - height) / r, 0.f, 1.f));
            constexpr int kSteps = 8;
            ImVec2 pts[2 + 2 * (kSteps + 1)];
            int n = 0;
            if (height > r) {
                pts[n++] = ImVec2(a.x, b.y - height);
                pts[n++] = ImVec2(b.x, b.y - height);
            }
            ImVec2 const right(b.x - r, b.y - r), left(a.x + r, b.y - r);
            for (int i = 0; i <= kSteps; ++i) {
                float const turn = from + (kPi * 0.5f - from) * static_cast<float>(i) / kSteps;
                pts[n++] = ImVec2(right.x + r * std::cos(turn), right.y + r * std::sin(turn));
            }
            for (int i = 0; i <= kSteps; ++i) {
                float const turn = kPi * 0.5f + (kPi * 0.5f - from) * static_cast<float>(i) / kSteps;
                pts[n++] = ImVec2(left.x + r * std::cos(turn), left.y + r * std::sin(turn));
            }
            dl->AddConvexPolyFilled(pts, n, color);
        }

        void drawArt(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
            Art const& layers = artLayers();
            float const w = b.x - a.x, h = b.y - a.y;
            // Frozen at the start with Animations off, not hidden.
            double const time = look().effects ? ImGui::GetTime() : 0.0;
            float const t = static_cast<float>(time);

            // The sky, covering the window with its shape kept: as much of it
            // as fits from its bottom, where the horizon glows behind the
            // skyline.
            {
                float const s = std::max(w / ja::kDesignW, h / ja::kDesignH);
                ImVec2 const d0(a.x + (w - ja::kDesignW * s) * 0.5f, b.y - ja::kDesignH * s);
                ImVec2 const d1(d0.x + ja::kDesignW * s, b.y);
                drawInWindow(dl, layers.sky, ImVec4(0.f, 0.f, (float)layers.sky.width(), (float)layers.sky.height()), d0, d1, a,
                             b, rounding);
            }

            // The dial in the top-right corner, its full size from the menu's
            // first width up and smaller in a narrower window, so it keeps
            // back from the header's words as SolarBot's Janet does.
            float const k = std::clamp((w - 500.f) / 220.f, 0.5f, 1.f);
            ImVec2 const c(b.x - ja::kDialRight * k, a.y + ja::kDialTop * k);
            {
                ImVec2 const d0(c.x + ja::kDialStill[0] * k, c.y + ja::kDialStill[1] * k);
                ImVec2 const d1(d0.x + ja::kDialStill[2] * k, d0.y + ja::kDialStill[3] * k);
                drawInWindow(dl, layers.dial, ImVec4(0.f, 0.f, (float)layers.dial.width(), (float)layers.dial.height()), d0,
                             d1, a, b, rounding);
            }
            auto const turned = [&](double rate) {
                return static_cast<float>(std::fmod(rate * time, 360.0)) * kPi / 180.f;
            };
            ring(dl, layers.dialCcw, c, ja::kDialCcwR * k, turned(kCcwRate), a, b, rounding);
            ring(dl, layers.dialCw, c, ja::kDialCwR * k, turned(kCwRate), a, b, rounding);

            // The stars, each where it was placed: from the left or right
            // edge, or across the page right of the sidebar, as far down as
            // at the first size (or as far up from the bottom).
            auto const place = [&](ja::Star const& s) {
                float x = a.x + s.x;
                if (s.ax == 'R')
                    x = b.x - (ja::kDesignW - s.x);
                else if (s.ax == 'P')
                    x = a.x + ja::kPageLeft + (s.x - ja::kPageLeft) * (w - ja::kPageLeft) / (ja::kDesignW - ja::kPageLeft);
                float const y = s.ay == 'B' ? b.y - (ja::kDesignH - s.y) : a.y + s.y;
                return ImVec2(x, y);
            };
            // Each star's own beat: its halo's strength, a twinkle's size.
            auto const beat = [&](int i) { return 0.75f + 0.25f * std::sin(t * 1.9f + static_cast<float>(i) * 1.7f); };
            int halos = 0;
            for (auto const& s : ja::kStars) {
                if (s.kind != ja::Kind::Scribble)
                    continue;
                sprite(dl, ja::kHaloSprite, place(s), ja::kHaloReach * s.r / ja::kHaloR, col(kHalo, 0.40f * beat(halos)));
                ++halos;
            }
            int twinkles = 0;
            for (auto const& s : ja::kStars) {
                float scale = 1.f;
                if (s.kind == ja::Kind::Twinkle)
                    scale = 0.85f + 0.25f * beat(3 + twinkles++);
                sprite(dl, s.sprite, place(s), scale, IM_COL32_WHITE);
            }

            // The specks, rising and coming round again, spread over the
            // window as they were over the first size.
            {
                double const span = ja::kSpeckSpan / ja::kDesignH * h;
                for (size_t i = 0; i < std::size(ja::kSpecks); ++i) {
                    auto const& sp = ja::kSpecks[i];
                    double y = std::fmod(sp.y / ja::kDesignH * h - kSpeckRise * time, span);
                    if (y < 0.0)
                        y += span;
                    ImVec2 const p(a.x + sp.x / ja::kDesignW * w, a.y + static_cast<float>(y));
                    // Not in the window's rounded top corners.
                    if (p.y < a.y + rounding && (p.x < a.x + rounding || p.x + sp.size > b.x - rounding))
                        continue;
                    dl->AddRectFilled(p, ImVec2(p.x + sp.size, p.y + sp.size),
                                      i % 2 ? col(kBlue, 0.85f) : col(kTeal, 0.75f));
                }
            }

            // The skyline: the strip repeating along the bottom, then the
            // sidebar's foot and the tower on the right edge over it, then the
            // lit windows, one or two of them dark at a time.
            {
                auto const& m = ja::kCityMiddle;
                float const from = a.x + m.x, to = b.x + ja::kCityMiddleEnd;
                if (to > from) {
                    dl->PushClipRect(ImVec2(from, a.y), ImVec2(to, b.y), true);
                    for (float x = from; x < to; x += m.w)
                        dl->AddImage(layers.city.id(), ImVec2(x, b.y + m.y), ImVec2(x + m.w, b.y + m.y + m.h),
                                     uvOf(layers.city, m.u0, m.v0), uvOf(layers.city, m.u1, m.v1));
                    dl->PopClipRect();
                }
                auto const& l = ja::kCityLeft;
                ImVec2 const l0(a.x + l.x, b.y + l.y);
                drawInWindow(dl, layers.city, ImVec4(l.u0, l.v0, l.u1, l.v1), l0, ImVec2(l0.x + l.w, l0.y + l.h), a, b,
                             rounding);
                auto const& r = ja::kCityRight;
                ImVec2 const r0(b.x + r.x, b.y + r.y);
                drawInWindow(dl, layers.city, ImVec4(r.u0, r.v0, r.u1, r.v1), r0, ImVec2(r0.x + r.w, r0.y + r.h), a, b,
                             rounding);
            }
            {
                auto const flick = static_cast<int64_t>(time * 0.7);
                for (size_t i = 0; i < std::size(ja::kLitWindows); ++i) {
                    if ((static_cast<int64_t>(i) + flick) % 7 == 3)
                        continue;
                    auto const& s = ja::kLitWindows[i];
                    sprite(dl, ja::kWindowSprite, ImVec2(s.x >= 0.f ? a.x + s.x : b.x + s.x, b.y + s.y), 1.f,
                           IM_COL32_WHITE);
                }
            }

            // The ground: a thin yellow line over near-black along the bottom.
            bottomBand(dl, a, b, rounding, 6.8f, col(kYellow));
            bottomBand(dl, a, b, rounding, 5.4f, col(kGround));
        }

    } // namespace

    void jupiterBackdrop() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiStyle const& style = ImGui::GetStyle();
        ImVec2 const a = ImGui::GetWindowPos();
        ImVec2 const size = ImGui::GetWindowSize();
        ImVec2 const b(a.x + size.x, a.y + size.y);
        g_lastDrawn = ImGui::GetTime();
        // Out to the window's border, not just its padding; the border is
        // drawn again over the top at the end.
        dl->PushClipRect(a, b, false);
        if (ensureArt())
            drawArt(dl, a, b, style.WindowRounding);
        else
            dl->AddRectFilled(a, b, col(kSky), style.WindowRounding);
        dl->PopClipRect();
        if (style.WindowBorderSize > 0.f)
            dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding, 0, style.WindowBorderSize);
    }

    void jupiterArtIdle(bool keep) {
        if (keep || (!g_ready && !g_tried))
            return;
        if (ImGui::GetTime() - g_lastDrawn < kIdleFree)
            return;
        freeArt();
        g_tried = false;
    }

} // namespace gucci::ui::pages
