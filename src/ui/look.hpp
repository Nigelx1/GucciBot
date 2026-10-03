#pragma once

// The interface's visual system: a palette worked out from the active theme
// every frame, the fonts, and the ImGui style they are applied through.
// Pages never pick raw colours; they ask for a Tone or a palette slot.

#include <imgui.h>

namespace gucci::ui {

    enum class Tone { Plain, Accent, Good, Warn, Bad, Muted };
    enum class Font { Body, Small, Strong, Title, Display };

    struct Palette {
        ImVec4 accent;     // the theme's colour (animated when the theme or a setting asks)
        ImVec4 onAccent;   // text drawn on top of the accent
        ImVec4 backdrop;   // window background
        ImVec4 surface;    // cards
        ImVec4 raised;     // controls sitting on a card
        ImVec4 ink;        // main text
        ImVec4 muted;      // secondary text
        ImVec4 line;       // borders and dividers
        ImVec4 good, warn, bad;
        float radius = 6.f;
        float opacity = 0.96f;
    };

    class Look {
    public:
        Palette pal;

        // Load the fonts into ImGui's atlas. Once, from the ImGui setup call.
        void loadFonts();
        // Recompute the palette from the active theme and push it into
        // ImGui's style. Once per frame, before anything is drawn.
        void refresh();

        ImFont* font(Font f) const;
        ImVec4 tone(Tone t) const;

        // Look options (saved settings, see Settings page).
        float textScale = 1.f;
        bool accentCycle = false;   // slowly cycle the accent's hue
        float accentCycleRate = 0.5f;
        bool effects = true;        // falling-snow backdrop and small animations

    private:
        ImFont* m_fonts[5] = {};
    };

    Look& look();

    ImU32 u32(const ImVec4& c, float alphaScale = 1.f);
    ImVec4 withAlpha(const ImVec4& c, float a);
    ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);
    // Relative luminance, for picking readable text on a colour.
    float luminance(const ImVec4& c);

    // Pushes a font for the lifetime of the scope (no-op when it failed to load).
    class UseFont {
    public:
        explicit UseFont(Font f);
        ~UseFont();
        UseFont(const UseFont&) = delete;
        UseFont& operator=(const UseFont&) = delete;

    private:
        bool m_pushed = false;
    };

} // namespace gucci::ui
