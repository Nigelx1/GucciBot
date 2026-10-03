#include "ui/look.hpp"
#include "ui/themes.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace gucci::ui {

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
        auto regular = (dir / "Roboto-Regular.ttf").string();
        auto bold = (dir / "Roboto-Bold.ttf").string();
        std::error_code ec;
        bool const haveRegular = std::filesystem::exists(regular, ec);
        bool const haveBold = std::filesystem::exists(bold, ec);

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
        for (auto const& w : wants) {
            std::string const& path = (w.boldFace && haveBold) ? bold : regular;
            if (!haveRegular && !(w.boldFace && haveBold))
                continue;
            m_fonts[static_cast<int>(w.slot)] = io.Fonts->AddFontFromFileTTF(path.c_str(), w.px);
        }
        if (!m_fonts[static_cast<int>(Font::Body)])
            m_fonts[static_cast<int>(Font::Body)] = io.Fonts->AddFontDefault();
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
    } // namespace

    void Look::refresh() {
        auto const& theme = themes::active();
        float const t = static_cast<float>(ImGui::GetTime());

        ImVec4 accent = theme.accent;
        if (theme.pulse) {
            // A slow breath between the colour and a brighter version of it.
            float const breath = 0.5f + 0.5f * std::sin(t * 2.2f);
            accent = mix(accent, lift(accent, 0.25f), breath);
        }
        if (accentCycle)
            accent = rotateHue(accent, t * 0.08f * std::clamp(accentCycleRate, 0.05f, 4.f));
        accent.w = 1.f;

        pal.accent = accent;
        pal.onAccent = luminance(accent) > 0.55f ? ImVec4(0.06f, 0.06f, 0.06f, 1.f) : ImVec4(0.97f, 0.97f, 0.97f, 1.f);
        pal.opacity = std::clamp(theme.opacity, 0.3f, 1.f);
        pal.backdrop = withAlpha(theme.background, pal.opacity);
        pal.surface = withAlpha(theme.card, 1.f);
        pal.raised = withAlpha(lift(theme.card, 0.055f), 1.f);
        pal.ink = withAlpha(theme.text, 1.f);
        pal.muted = withAlpha(theme.textMuted, 1.f);
        pal.line = withAlpha(mix(theme.card, theme.text, 0.14f), 1.f);
        pal.good = ImVec4(0.36f, 0.80f, 0.47f, 1.f);
        pal.warn = ImVec4(0.96f, 0.72f, 0.27f, 1.f);
        pal.bad = ImVec4(0.92f, 0.36f, 0.36f, 1.f);
        pal.radius = std::clamp(theme.radius, 0.f, 14.f);

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
        c[ImGuiCol_PopupBg] = withAlpha(lift(theme.background, 0.03f), 0.98f);
        c[ImGuiCol_Border] = pal.line;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = pal.raised;
        c[ImGuiCol_FrameBgHovered] = lift(pal.raised, 0.04f);
        c[ImGuiCol_FrameBgActive] = lift(pal.raised, 0.07f);
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
        c[ImGuiCol_SliderGrabActive] = lift(pal.accent, 0.1f);
        c[ImGuiCol_Button] = pal.raised;
        c[ImGuiCol_ButtonHovered] = lift(pal.raised, 0.05f);
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

        ImGui::GetIO().FontGlobalScale = std::clamp(textScale, 0.7f, 1.6f);
    }

} // namespace gucci::ui
