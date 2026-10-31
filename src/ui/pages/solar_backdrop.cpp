// SolarBot's scene (Nigel, 2026-10-31): Solar Flare behind the menu. Janet,
// the level's smiling sun, rises from the bottom-right corner with her face
// turned into the menu; thin gold flares fan out from her with sparks along
// them, a few gold sun gears turn, and the level's blue shows here and there
// as cyan gems in gold frames, and in the menu's checks and slider grabs.
//
// Drawn the way the JMF sky is (trainer_pages.cpp jupiterBackdrop): on the
// menu window's draw list before its children, so the cards stand in front
// and it shows in the sidebar, the page's header, between the cards and
// below the last one; marks placed in a fixed design space stretched to the
// window; colours with their own alpha, so BIG BRRRR's flicker doesn't dim
// it; nothing kept between frames, and with Animations off it holds still
// rather than going away. Written fresh.

#include "ui/look.hpp"
#include "ui/pages.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace gucci::ui::pages {

    namespace {

        constexpr float kPi = 3.14159265f;

        ImVec4 const kSky{0.604f, 0.239f, 0.000f, 1.f};       // #9A3D00, SolarBot's background
        ImVec4 const kGold{1.000f, 0.769f, 0.000f, 1.f};      // #FFC400, its accent
        ImVec4 const kSpark{1.000f, 0.957f, 0.690f, 1.f};     // #FFF4B0, the flares' sparks
        ImVec4 const kCyan{0.212f, 0.784f, 0.957f, 1.f};      // #36C8F4, the level's gems
        ImVec4 const kSunEdge{0.878f, 0.353f, 0.000f, 1.f};   // #E05A00, Janet's edge
        ImVec4 const kSunCore{1.000f, 0.882f, 0.302f, 1.f};   // #FFE14D, her middle
        ImVec4 const kSunGlow{1.000f, 0.851f, 0.239f, 1.f};   // #FFD93D, the light round her
        ImVec4 const kFace{0.239f, 0.098f, 0.000f, 1.f};      // #3D1900, her eyes and smile

        ImU32 col(ImVec4 const& c, float alpha) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(alpha, 0.f, 1.f)));
        }

        // 0..1, fixed for a mark's number and a salt (the snow's hash).
        float hashed(int i, uint32_t salt) {
            uint32_t h = static_cast<uint32_t>(i + 1) * 2654435761u ^ salt * 0x9E3779B9u;
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            return static_cast<float>(h & 0xFFFFu) / 65535.f;
        }

        ImVec2 along(ImVec2 c, float angle, float dist) {
            return ImVec2(c.x + std::cos(angle) * dist, c.y + std::sin(angle) * dist);
        }

        // A diamond, taller than wide, as the level's gems are.
        void diamond(ImDrawList* dl, ImVec2 c, float halfH, ImU32 fill) {
            float const halfW = halfH * 0.68f;
            dl->AddQuadFilled({c.x, c.y - halfH}, {c.x + halfW, c.y}, {c.x, c.y + halfH}, {c.x - halfW, c.y}, fill);
        }

        void diamondFrame(ImDrawList* dl, ImVec2 c, float halfH, ImU32 line, float width) {
            float const halfW = halfH * 0.68f;
            dl->AddQuad({c.x, c.y - halfH}, {c.x + halfW, c.y}, {c.x, c.y + halfH}, {c.x - halfW, c.y}, line, width);
        }

        // A sun gear: a ring of square teeth round a hub, outlined.
        void gear(ImDrawList* dl, ImVec2 c, float r, float turn, ImU32 line, float width) {
            constexpr int kTeeth = 10;
            ImVec2 pts[kTeeth * 4];
            float const step = 2.f * kPi / static_cast<float>(kTeeth);
            for (int k = 0; k < kTeeth; ++k) {
                float const a0 = turn + step * static_cast<float>(k);
                pts[k * 4 + 0] = along(c, a0, r * 0.78f);
                pts[k * 4 + 1] = along(c, a0 + step * 0.12f, r);
                pts[k * 4 + 2] = along(c, a0 + step * 0.48f, r);
                pts[k * 4 + 3] = along(c, a0 + step * 0.60f, r * 0.78f);
            }
            dl->AddPolyline(pts, kTeeth * 4, line, ImDrawFlags_Closed, width);
            dl->AddCircle(c, r * 0.46f, line, 0, width);
        }

        // Janet's face: two closed, smiling eyes and a small smile under them,
        // `u` across, leaning by `tilt` (radians) so she looks up into the menu.
        void face(ImDrawList* dl, ImVec2 at, float u, float tilt, ImU32 ink, float width) {
            ImVec2 const ex(std::cos(tilt), std::sin(tilt));   // her right
            ImVec2 const ey(-std::sin(tilt), std::cos(tilt));  // her down
            auto const p = [&](float x, float y) {
                return ImVec2(at.x + ex.x * x + ey.x * y, at.y + ex.y * x + ey.y * y);
            };
            for (float side : {-1.f, 1.f}) {
                dl->PathArcTo(p(side * 0.62f * u, -0.30f * u), 0.26f * u, kPi + tilt + 0.35f,
                              2.f * kPi + tilt - 0.35f, 12);
                dl->PathStroke(ink, 0, width);
            }
            dl->PathArcTo(p(0.f, 0.02f * u), 0.50f * u, tilt + 0.62f, kPi + tilt - 0.62f, 16);
            dl->PathStroke(ink, 0, width);
        }

        // Where the marks go, in a 720 x 500 menu (its first size), stretched
        // with the window. Kept off Janet's corner, which covers it.
        struct Mark {
            float x, y, size;
        };
        constexpr Mark kGems[] = {
            {34, 98, 1.0f},  {146, 236, 0.85f}, {106, 452, 0.9f}, {268, 108, 0.8f},
            {432, 24, 1.1f}, {630, 54, 0.9f},   {692, 178, 1.0f}, {452, 484, 0.85f},
        };
        constexpr Mark kGears[] = {
            {72, 170, 17}, {128, 354, 13}, {318, 36, 15}, {560, 98, 19}, {24, 300, 12},
        };

    } // namespace

    // Her gems' blue in the menu's own widgets (after look().refresh(), as
    // JupiterBot's look is).
    void solarLookApply() {
        ImVec4* c = ImGui::GetStyle().Colors;
        c[ImGuiCol_CheckMark] = kCyan;
        c[ImGuiCol_SliderGrab] = kCyan;
        c[ImGuiCol_SliderGrabActive] = shade(kCyan, 0.1f);
    }

    void solarBackdrop() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiStyle const& style = ImGui::GetStyle();
        ImVec2 const a = ImGui::GetWindowPos();
        ImVec2 const size = ImGui::GetWindowSize();
        ImVec2 const b(a.x + size.x, a.y + size.y);
        // Out to the window's border, not just its padding, so Janet meets the
        // edges; the border is drawn again over the top at the end.
        dl->PushClipRect(ImVec2(a.x + 1.f, a.y + 1.f), ImVec2(b.x - 1.f, b.y - 1.f), false);
        dl->AddRectFilled(a, b, col(kSky, 1.f), style.WindowRounding);

        // Frozen at the start with Animations off, not hidden.
        float const t = look().effects ? static_cast<float>(ImGui::GetTime()) : 0.f;
        auto const at = [&](Mark const& m) { return ImVec2(a.x + m.x / 720.f * size.x, a.y + m.y / 500.f * size.y); };

        // Janet: her centre just inside the bottom-right corner, so a little
        // more than her top-left quarter shows.
        float const R = std::max(70.f, std::min(size.x, size.y) * 0.42f);
        ImVec2 const c(b.x - R * 0.10f, b.y - R * 0.06f);
        float const breath = 0.5f + 0.5f * std::sin(t * 0.8f);  // her glow, slowly

        // Her warmth over the window: wide, faint rings, many enough that
        // their steps don't show.
        for (int k = 0; k < 8; ++k)
            dl->AddCircleFilled(c, R * (3.2f - 0.27f * static_cast<float>(k)), col(kSunGlow, 0.018f), 96);

        // The flares: thin gold needles all the way round her, turning very
        // slowly; those pointing into the window show. Each has a faint body,
        // gold edges and sparks along its middle.
        float const reach = std::sqrt(size.x * size.x + size.y * size.y);
        constexpr int kFlares = 18;
        for (int i = 0; i < kFlares; ++i) {
            float const angle = 0.13f + static_cast<float>(i) * (2.f * kPi / kFlares) + t * 0.02f;
            float const length = reach * (0.60f + 0.40f * hashed(i, 1));
            float const halfBase = R * (0.010f + 0.010f * hashed(i, 2));
            float const alpha = 0.20f + 0.20f * hashed(i, 3);
            ImVec2 const root = along(c, angle, R * 1.03f);
            ImVec2 const side(-std::sin(angle) * halfBase, std::cos(angle) * halfBase);
            ImVec2 const left(root.x + side.x, root.y + side.y);
            ImVec2 const right(root.x - side.x, root.y - side.y);
            ImVec2 const tip = along(c, angle, R * 1.03f + length);
            dl->AddTriangleFilled(left, right, tip, col(kGold, alpha * 0.35f));
            dl->AddLine(left, tip, col(kGold, alpha), 1.f);
            dl->AddLine(right, tip, col(kGold, alpha), 1.f);
            for (int s = 0; s < 3; ++s) {
                float const f = 0.22f + 0.24f * static_cast<float>(s) + 0.10f * hashed(i * 3 + s, 4);
                ImVec2 const spark = along(c, angle, R * 1.03f + length * f);
                dl->AddCircleFilled(spark, 4.f, col(kGold, alpha * 0.55f), 10);
                dl->AddCircleFilled(spark, 1.7f, col(kSpark, std::min(1.f, alpha * 2.2f)), 8);
            }
        }

        // Sun gears, turning slowly, every other one the other way; some with
        // a blue hub.
        for (size_t i = 0; i < std::size(kGears); ++i) {
            Mark const& m = kGears[i];
            float const turn = (i % 2 ? -1.f : 1.f) * t * 0.10f + static_cast<float>(i);
            ImVec2 const p = at(m);
            gear(dl, p, m.size, turn, col(kGold, 0.30f), 1.4f);
            if (i % 2 == 0)
                dl->AddCircleFilled(p, m.size * 0.2f, col(kCyan, 0.45f), 12);
        }

        // The blue here and there: cyan gems in thin gold frames, glinting.
        for (size_t i = 0; i < std::size(kGems); ++i) {
            Mark const& m = kGems[i];
            ImVec2 const p = at(m);
            float const glint = 0.80f + 0.20f * std::sin(t * 1.3f + static_cast<float>(i) * 1.7f);
            float const h = 7.f * m.size;
            diamond(dl, p, h * 1.45f, col(kCyan, 0.10f * glint));
            diamondFrame(dl, p, h * 1.55f, col(kGold, 0.45f), 1.2f);
            diamond(dl, p, h, col(kCyan, 0.70f * glint));
            diamond(dl, ImVec2(p.x - h * 0.15f, p.y - h * 0.35f), h * 0.22f, col(kSpark, 0.55f * glint));
        }

        // Janet's own glow, breathing: soft rings just outside her rim.
        for (int k = 5; k >= 1; --k) {
            float const ring = static_cast<float>(k);
            float const spread = R * (1.f + (0.032f + 0.012f * breath) * ring);
            dl->AddCircleFilled(c, spread, col(kSunGlow, (0.05f + 0.035f * breath) * (1.f - 0.12f * ring)), 96);
        }
        // Her body: layered discs, deep orange at the edge to bright yellow
        // inside, close enough that they blend.
        constexpr int kLayers = 24;
        for (int i = 0; i < kLayers; ++i) {
            float const f = static_cast<float>(i) / static_cast<float>(kLayers - 1);
            dl->AddCircleFilled(c, R * (1.f - 0.64f * f), col(mix(kSunEdge, kSunCore, f), 1.f), 96);
        }
        dl->AddCircle(c, R, col(kGold, 0.95f), 128, std::max(2.f, R * 0.012f));
        // Her face, looking up into the menu.
        ImVec2 const faceAt = along(c, kPi + 0.68f, R * 0.50f);
        face(dl, faceAt, R * 0.18f, -0.30f, col(kFace, 0.92f), std::max(2.f, R * 0.018f));

        dl->PopClipRect();
        if (style.WindowBorderSize > 0.f)
            dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), style.WindowRounding, 0, style.WindowBorderSize);
    }

} // namespace gucci::ui::pages
