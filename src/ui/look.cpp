#include "ui/look.hpp"
#include "ui/themes.hpp"

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_MOBILE)
#include <imgui-cocos.hpp>
#endif

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace gucci::ui {

#if defined(GEODE_IS_MOBILE)
    namespace {
        // The menu is laid out for a desktop window about 500 units tall, a
        // unit being a screen pixel. On a phone it was a pixel too (imgui-cocos
        // gives ImGui the GL frame), so 15-pixel text came out about a
        // millimetre tall. Instead the screen's short side is this many units:
        // about 2.7 pixels a unit on a 1080-pixel phone, text near 2.5 mm. GD's
        // own interface scales the same way (its screen is always 320 points
        // tall), so a tablet gets it proportionally bigger, as GD's buttons are.
        constexpr float kMobileUnitsTall = 400.f;

        float screenScale() {
            auto* view = CCDirector::sharedDirector()->getOpenGLView();
            if (!view)
                return 1.f;
            CCSize const frame = view->getFrameSize();
            float const shortSide = std::min(frame.width, frame.height);
            return shortSide > 0.f ? std::max(1.f, shortSide / kMobileUnitsTall) : 1.f;
        }
    } // namespace
#endif

    Look& look() {
        static Look s_look;
        return s_look;
    }

    ImU32 u32(const ImVec4& c, float alphaScale) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alphaScale, 0.f, 1.f)));
    }

    ImVec4 withAlpha(const ImVec4& c, float a) {
        return ImVec4(c.x, c.y, c.z, a);
    }

    ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    float luminance(const ImVec4& c) {
        return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    }

    float contrast(const ImVec4& a, const ImVec4& b) {
        // WCAG's luminance is of linear light, not of the stored (gamma
        // encoded) values luminance() above weighs.
        auto const lum = [](const ImVec4& c) {
            auto const lin = [](float v) {
                v = std::clamp(v, 0.f, 1.f);
                return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            };
            return 0.2126f * lin(c.x) + 0.7152f * lin(c.y) + 0.0722f * lin(c.z);
        };
        float const la = lum(a);
        float const lb = lum(b);
        return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
    }

    void setLight(Palette& p, bool light) {
        p.light = light;
        if (light) {
            // At least 4.5:1 on white, on the controls over it and in a
            // chip's tint of themselves (4.9:1 and up).
            p.good = ImVec4(0.086f, 0.416f, 0.180f, 1.f);  // #166A2E
            p.warn = ImVec4(0.549f, 0.310f, 0.000f, 1.f);  // #8C4F00
            p.bad = ImVec4(0.706f, 0.137f, 0.094f, 1.f);   // #B42318
        } else {
            p.good = ImVec4(0.36f, 0.80f, 0.47f, 1.f);
            p.warn = ImVec4(0.96f, 0.72f, 0.27f, 1.f);
            p.bad = ImVec4(0.92f, 0.36f, 0.36f, 1.f);
        }
    }

    void setHit(Palette& p, const ImVec4* accents, int count) {
        // Nigel's pick (2026-11-01): an answered press turned from the accent
        // to good, and on RondoBot the two greens are 1.22:1 (WakaBot's
        // 1.10:1), so a hit could not be seen. Two colors under 1.25:1 can
        // only be told apart by hue: that is the step the 2026-10-31 theme
        // review found too faint for a card's edge to show (refresh(),
        // pal.line), and hue fails where the two are both green, and for a
        // color-blind player. WCAG's own 3:1 for marks can't be the bar: no
        // built-in theme's good is 3:1 from its accent (2.76:1 at most). On
        // RondoBot nothing lighter than its green is 3:1 from both that green
        // and its white bar (2.27:1 at best); only near-black is (black,
        // 3.83:1), and of the three picks below gold stands apart most
        // (2.14:1). A bar above about 1.27:1 would switch themes whose green
        // already stands apart by hue: SolarBot first (1.27:1), then BrrrBot
        // (1.31:1), LSU (1.37:1) and LemonadeBot (1.45:1).
        constexpr float kTellApart = 1.25f;
        ImVec4 const gold{0.788f, 0.659f, 0.298f, 1.f};  // #C9A84C, GucciBot's own
        ImVec4 const white{1.f, 1.f, 1.f, 1.f};
        if (!accents || count <= 0) {
            accents = &p.accent;
            count = 1;
        }
        // The click bar's own background: its raised tint at 55% over a card
        // (pages/trainer_pages.cpp drawBar).
        ImVec4 const behind = mix(p.surface, p.raised, 0.55f);
        auto const apart = [&](ImVec4 const& c) {
            float worst = contrast(c, behind);
            for (int i = 0; i < count; ++i)
                worst = std::min(worst, contrast(c, accents[i]));
            return worst;
        };
        struct Pick {
            ImVec4 color;
            const char* name;
        };
        Pick const picks[] = {{p.good, "green"}, {gold, "gold"}, {white, "white"}};
        int chosen = -1;
        int most = 0;
        float mostApart = -1.f;
        for (int i = 0; i < 3 && chosen < 0; ++i) {
            float const a = apart(picks[i].color);
            if (a >= kTellApart)
                chosen = i;
            else if (a > mostApart) {
                most = i;
                mostApart = a;
            }
        }
        if (chosen < 0)
            chosen = most;
        p.hit = picks[chosen].color;
        p.hitName = picks[chosen].name;
    }

    UseFont::UseFont(Font f) {
        if (ImFont* font = look().font(f)) {
            ImGui::PushFont(font);
            m_pushed = true;
        }
    }

    UseFont::~UseFont() {
        if (m_pushed)
            ImGui::PopFont();
    }

    void Look::loadFonts() {
        ImGuiIO& io = ImGui::GetIO();
        auto dir = Mod::get()->getResourcesDir();
        auto const regularFile = dir / "Roboto-Regular.ttf";
        auto const boldFile = dir / "Roboto-Bold.ttf";
        std::error_code ec;
        bool const haveRegular = std::filesystem::exists(regularFile, ec);
        bool const haveBold = std::filesystem::exists(boldFile, ec);
        // ImGui opens font files by UTF-8 name. path.string() is the ANSI
        // code page on Windows (and throws for a folder name it can't spell).
        std::string const regular = geode::utils::string::pathToString(regularFile);
        std::string const bold = geode::utils::string::pathToString(boldFile);

        // Body first: the first font loaded is ImGui's default.
        struct Want {
            Font slot;
            bool boldFace;
            float px;
        };
        const Want wants[] = {
            {Font::Body, false, 15.f},
            {Font::Small, false, 12.5f},
            {Font::Strong, true, 15.f},
            {Font::Title, true, 19.f},
            {Font::Display, true, 27.f},
        };
#if defined(GEODE_IS_MOBILE)
        // The glyphs are made as big as the screen shows them and shrunk back
        // by FontGlobalScale (refresh), so text is drawn from glyphs at the
        // screen's size rather than 15-pixel ones stretched. Set once: a later
        // Menu size change resamples them. Glyphs this big need no sideways
        // oversampling, and leaving it out halves the atlas.
        m_fontRaster = std::clamp(screenScale(), 1.f, 4.f);
        ImFontConfig cfg;
        cfg.OversampleH = 1;
#endif
        for (auto const& w : wants) {
            std::string const& path = (w.boldFace && haveBold) ? bold : regular;
            if (!haveRegular && !(w.boldFace && haveBold))
                continue;
#if defined(GEODE_IS_MOBILE)
            m_fonts[static_cast<int>(w.slot)] = io.Fonts->AddFontFromFileTTF(path.c_str(), w.px * m_fontRaster, &cfg);
#else
            m_fonts[static_cast<int>(w.slot)] = io.Fonts->AddFontFromFileTTF(path.c_str(), w.px);
#endif
        }
        if (!m_fonts[static_cast<int>(Font::Body)]) {
#if defined(GEODE_IS_MOBILE)
            ImFontConfig fallback;  // ImGui's own settings for its pixel font, made as big
            fallback.SizePixels = 13.f * m_fontRaster;
            fallback.OversampleH = fallback.OversampleV = 1;
            fallback.PixelSnapH = true;
            m_fonts[static_cast<int>(Font::Body)] = io.Fonts->AddFontDefault(&fallback);
#else
            m_fonts[static_cast<int>(Font::Body)] = io.Fonts->AddFontDefault();
#endif
        }
    }

    float Look::deviceScale() const {
#if defined(GEODE_IS_MOBILE)
        return std::clamp(screenScale() * std::clamp(menuScale, 0.6f, 1.6f), 1.f, 6.f);
#else
        return 1.f;
#endif
    }

    ImFont* Look::font(Font f) const {
        ImFont* chosen = m_fonts[static_cast<int>(f)];
        return chosen ? chosen : m_fonts[static_cast<int>(Font::Body)];
    }

    ImVec4 Look::tone(Tone t) const {
        switch (t) {
            case Tone::Accent: return pal.accent;
            case Tone::Good: return pal.good;
            case Tone::Warn: return pal.warn;
            case Tone::Bad: return pal.bad;
            case Tone::Muted: return pal.muted;
            case Tone::Plain: break;
        }
        return pal.ink;
    }

    namespace {
        // Hue rotation of a colour by `turns` of the colour wheel.
        ImVec4 rotateHue(const ImVec4& c, float turns) {
            float h, s, v;
            ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
            h = std::fmod(h + turns, 1.f);
            if (h < 0.f)
                h += 1.f;
            ImVec4 out = c;
            ImGui::ColorConvertHSVtoRGB(h, s, v, out.x, out.y, out.z);
            return out;
        }

        ImVec4 lift(const ImVec4& c, float amount) {
            return ImVec4(std::min(1.f, c.x + amount), std::min(1.f, c.y + amount), std::min(1.f, c.z + amount), c.w);
        }

        ImVec4 darken(const ImVec4& c, float amount) {
            return ImVec4(std::max(0.f, c.x - amount), std::max(0.f, c.y - amount), std::max(0.f, c.z - amount), c.w);
        }
    } // namespace

    ImVec4 shade(const ImVec4& c, float amount) {
        return look().pal.light ? darken(c, amount) : lift(c, amount);
    }

    float overLevelAlpha(float dark) {
        // 0.92 white over black keeps RondoBot's ink and muted text near
        // 6:1; at 0.6 they were under 2.6:1.
        return look().pal.light ? 0.92f : dark;
    }

    void Look::refresh() {
        auto const& theme = themes::active();
        float const t = static_cast<float>(ImGui::GetTime());

        // A light theme (RondoBot's white) is lit the other way: what lifts on
        // a dark theme (the raised controls, a hover, a press, the pulse)
        // darkens on a light one, which near white would otherwise clamp to
        // nothing. Dark themes come out as before, bar Calculate's table
        // (below), which every theme colours now. Light takes dark text too:
        // a custom theme's mid grey under white text stays lit dark.
        setLight(pal, luminance(theme.background) > 0.5f && luminance(theme.text) < luminance(theme.background));

        ImVec4 accent = theme.accent;
        bool const breathes = theme.pulse && effects;
        float const turns = t * 0.08f * std::clamp(accentCycleRate, 0.05f, 4.f);  // the cycle's
        // With Settings > Look > Animations off it holds the theme's own
        // color, as the backdrops hold still (Nigel, 2026-11-01: "make it
        // stop"). A test's still shots (gucci_menu, effects false) turn the
        // switch off for the test, so they hold it too.
        if (breathes) {
            // A slow breath between the colour and a brighter version of it
            // (a deeper one on a light theme).
            float const breath = 0.5f + 0.5f * std::sin(t * 2.2f);
            accent = mix(accent, shade(accent, 0.25f), breath);
        }
        if (accentCycle)
            accent = rotateHue(accent, turns);
        accent.w = 1.f;

        pal.accent = accent;
        pal.onAccent = luminance(accent) > 0.55f ? ImVec4(0.06f, 0.06f, 0.06f, 1.f) : ImVec4(0.97f, 0.97f, 0.97f, 1.f);
        pal.opacity = std::clamp(theme.opacity, 0.3f, 1.f);
        pal.backdrop = withAlpha(theme.background, pal.opacity);
        pal.surface = withAlpha(theme.card, 1.f);
        pal.raised = withAlpha(shade(theme.card, 0.055f), 1.f);
        pal.ink = withAlpha(theme.text, 1.f);
        pal.muted = withAlpha(theme.textMuted, 1.f);
        // Borders and dividers: a step from the card towards the text, 14%
        // of the way on a dark theme (GucciBot's line is 1.43:1 on its
        // card). The same step on a white card is 1.25:1, too faint to keep
        // a white card's edge on a near-white window, so a light theme's
        // take 24% (1.48:1, RondoBot's light green-grey).
        pal.line = withAlpha(mix(theme.card, theme.text, pal.light ? 0.24f : 0.14f), 1.f);
        pal.radius = std::clamp(theme.radius, 0.f, 14.f);
        pal.solidTints = false;  // the JMF look turns it on after this (pages/trainer_pages.cpp jmfLook)
        pal.cardEdge.on = false;   // SolarBot's look turns these on after this (pages/solar_backdrop.cpp)
        pal.pageFrame.on = false;
        // An answered mark's color (setHit), judged against the accent at
        // every quarter of its breath, so it doesn't flip as the accent pulses.
        if (breathes) {
            ImVec4 breath[5];
            for (int i = 0; i < 5; ++i) {
                ImVec4 a = mix(theme.accent, shade(theme.accent, 0.25f), 0.25f * (float)i);
                if (accentCycle)
                    a = rotateHue(a, turns);
                a.w = 1.f;
                breath[i] = a;
            }
            setHit(pal, breath, 5);
        } else {
            setHit(pal);
        }

        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowPadding = ImVec2(14.f, 12.f);
        s.FramePadding = ImVec2(9.f, 5.f);
        s.ItemSpacing = ImVec2(8.f, 7.f);
        s.ItemInnerSpacing = ImVec2(6.f, 5.f);
        s.ScrollbarSize = 10.f;
        s.GrabMinSize = 10.f;
        s.WindowBorderSize = 1.f;
        s.ChildBorderSize = 1.f;
        s.PopupBorderSize = 1.f;
        s.FrameBorderSize = 0.f;
        s.WindowRounding = pal.radius + 4.f;
        s.ChildRounding = pal.radius + 2.f;
        s.FrameRounding = pal.radius;
        s.PopupRounding = pal.radius;
        s.GrabRounding = pal.radius;
        s.ScrollbarRounding = pal.radius + 2.f;
        s.TabRounding = pal.radius;
        s.WindowTitleAlign = ImVec2(0.f, 0.5f);
        s.SelectableTextAlign = ImVec2(0.f, 0.5f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = pal.ink;
        c[ImGuiCol_TextDisabled] = pal.muted;
        c[ImGuiCol_WindowBg] = pal.backdrop;
        c[ImGuiCol_ChildBg] = withAlpha(pal.surface, 0.f);
        c[ImGuiCol_PopupBg] = withAlpha(shade(theme.background, 0.03f), 0.98f);
        c[ImGuiCol_Border] = pal.line;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = pal.raised;
        c[ImGuiCol_FrameBgHovered] = shade(pal.raised, 0.04f);
        c[ImGuiCol_FrameBgActive] = shade(pal.raised, 0.07f);
        c[ImGuiCol_TitleBg] = pal.backdrop;
        c[ImGuiCol_TitleBgActive] = pal.backdrop;
        c[ImGuiCol_TitleBgCollapsed] = pal.backdrop;
        c[ImGuiCol_MenuBarBg] = pal.surface;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = withAlpha(pal.muted, 0.35f);
        c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(pal.muted, 0.55f);
        c[ImGuiCol_ScrollbarGrabActive] = withAlpha(pal.accent, 0.8f);
        c[ImGuiCol_CheckMark] = pal.accent;
        c[ImGuiCol_SliderGrab] = pal.accent;
        c[ImGuiCol_SliderGrabActive] = shade(pal.accent, 0.1f);
        c[ImGuiCol_Button] = pal.raised;
        c[ImGuiCol_ButtonHovered] = shade(pal.raised, 0.05f);
        c[ImGuiCol_ButtonActive] = withAlpha(pal.accent, 0.55f);
        c[ImGuiCol_Header] = withAlpha(pal.accent, 0.22f);
        c[ImGuiCol_HeaderHovered] = withAlpha(pal.accent, 0.30f);
        c[ImGuiCol_HeaderActive] = withAlpha(pal.accent, 0.40f);
        c[ImGuiCol_Separator] = pal.line;
        c[ImGuiCol_SeparatorHovered] = withAlpha(pal.accent, 0.6f);
        c[ImGuiCol_SeparatorActive] = pal.accent;
        c[ImGuiCol_ResizeGrip] = withAlpha(pal.accent, 0.18f);
        c[ImGuiCol_ResizeGripHovered] = withAlpha(pal.accent, 0.45f);
        c[ImGuiCol_ResizeGripActive] = withAlpha(pal.accent, 0.8f);
        c[ImGuiCol_Tab] = pal.raised;
        c[ImGuiCol_TabHovered] = withAlpha(pal.accent, 0.35f);
        c[ImGuiCol_TabSelected] = withAlpha(pal.accent, 0.28f);
        c[ImGuiCol_PlotLines] = pal.accent;
        c[ImGuiCol_PlotHistogram] = pal.accent;
        c[ImGuiCol_TextSelectedBg] = withAlpha(pal.accent, 0.35f);
        c[ImGuiCol_NavHighlight] = pal.accent;
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.45f);
        // Tables (Calculate's) in the theme's colours rather than ImGui's dark
        // grey, which a light theme can't read: the header a step towards the
        // text, the stripes a breath of it. On every theme (the 2026-10-31
        // spec), so a dark theme's header is a tint of it now, not grey.
        c[ImGuiCol_TableHeaderBg] = withAlpha(mix(pal.surface, pal.ink, 0.10f), 1.f);
        c[ImGuiCol_TableBorderStrong] = pal.line;
        c[ImGuiCol_TableBorderLight] = withAlpha(pal.line, 0.6f);
        c[ImGuiCol_TableRowBgAlt] = withAlpha(pal.ink, 0.05f);

#if defined(GEODE_IS_MOBILE)
        // Fingers, not a pointer: taller controls, more room between rows, a
        // bigger slider grab, and hit boxes a little past what is drawn. The
        // scrollbars only show where a page is: pages scroll by dragging
        // (ui/touch.cpp). In ImGui units, grown with everything else below.
        s.FramePadding = ImVec2(10.f, 9.f);
        s.ItemSpacing = ImVec2(8.f, 9.f);
        s.ScrollbarSize = 6.f;
        s.GrabMinSize = 18.f;
        s.TouchExtraPadding = ImVec2(2.f, 3.f);
        // imgui-cocos sized this frame's display before it started, so a
        // change shows in full from the next frame.
        ImGuiCocos::setFrameScale(deviceScale());
        ImGui::GetIO().FontGlobalScale = std::clamp(textScale, 0.7f, 1.6f) / m_fontRaster;
#else
        ImGui::GetIO().FontGlobalScale = std::clamp(textScale, 0.7f, 1.6f);
#endif
    }

} // namespace gucci::ui
