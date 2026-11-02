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
        // A mark answered among marks in the accent (the Trainer's click
        // bar: a macro press you answered). Good, unless good is too close
        // to the accent to tell apart; then gold, or white (setHit).
        // `hitName` names it for the words that do.
        ImVec4 hit;
        const char* hitName = "green";
        float radius = 6.f;
        float opacity = 0.96f;
        // A look where a see-through accent or tone would go grey (the JMF
        // look: yellow over indigo, complementary colours): chips, notes and
        // a disabled button are filled with `tint`, a solid colour, the
        // selected page in the sidebar is lit in the accent, and the click
        // bar's blocks are solid. refresh() sets it off; the JMF look on.
        bool solidTints = false;
        ImVec4 tint{};
        // A scene's own edge round the cards (SolarBot's: the level's orange
        // outline with a dimmed gold line just inside it), in place of `line`.
        // Off unless `on`: refresh() turns it off, the scene's look on
        // (kit::BeginCard and EndCard).
        struct CardEdge {
            bool on = false;
            ImVec4 outer{}, inner{};
        } cardEdge;
        // ... and its frame for the page you're on in the sidebar (SolarBot's):
        // an outer line, an inner one, the card's fill, the page's name in
        // `ink` and a gem at the frame's end. Off unless `on`, as above
        // (ui/shell.cpp drawMain).
        struct PageFrame {
            bool on = false;
            ImVec4 outer{}, inner{}, ink{}, gem{};
        } pageFrame;
        // The theme is light (RondoBot's white): a hover or press darkens
        // instead of lifting (shade()), and good/warn/bad are deeper ones that
        // read on white. Set with setLight().
        bool light = false;
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

        // Screen pixels per ImGui unit. 1 on Windows and macOS. On a phone or
        // tablet the whole interface (fonts, spacing, every size a page uses)
        // is drawn this many times bigger, worked out from the screen's size
        // and times menuScale; refresh() hands it to imgui-cocos.
        float deviceScale() const;

        // Look options (saved settings, see Settings page).
        float textScale = 1.f;
        bool accentCycle = false;   // slowly cycle the accent's hue
        float accentCycleRate = 0.5f;
        bool effects = true;        // small animations, a theme's pulsing accent (Red Kingdom's, SolarBot's)
                                    // and the moving backdrops (BrrrBot's snow, the JMF sky, SolarBot's Janet)
        float menuScale = 1.f;      // Android and iOS: bigger or smaller than deviceScale picks

    private:
        ImFont* m_fonts[5] = {};
        float m_fontRaster = 1.f;   // the fonts were loaded this many times their size (loadFonts)
    };

    Look& look();

    ImU32 u32(const ImVec4& c, float alphaScale = 1.f);
    ImVec4 withAlpha(const ImVec4& c, float a);
    ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);
    // Relative luminance, for picking readable text on a colour.
    float luminance(const ImVec4& c);
    // How well one colour reads on another, as the WCAG ratio: 1 (the same)
    // to 21 (black on white). Text wants 4.5, a short tag 3.
    float contrast(const ImVec4& a, const ImVec4& b);

    // Lights a palette for a dark or a light background: Palette::light and
    // the good/warn/bad that read on it. refresh() picks from the theme; the
    // JMF page's look, always dark, sets dark back.
    void setLight(Palette& p, bool light);
    // Works out Palette::hit and hitName from the palette's good, accent,
    // surface and raised: the first of good, gold and white that stands
    // apart from the accent and from the click bar's background (a raised
    // tint over a card) by 1.25:1 or more, the WCAG ratio, or, when none
    // does, the one that stands apart the most. By contrast, never by theme
    // name, so a custom theme goes through it too. With `accents`, the
    // accent is judged at each of them (refresh() passes its breath, so the
    // choice holds still while it pulses). refresh() calls it; a look that
    // changes the accent or the cards afterwards (the JMF look) calls it again.
    void setHit(Palette& p, const ImVec4* accents = nullptr, int count = 0);
    // A colour moved by `amount` the way a hover or a press moves it: lighter
    // on a dark palette, darker on a light one (look().pal.light).
    ImVec4 shade(const ImVec4& c, float amount);
    // How opaque a box over the level is (Calculate's, the pathfinders',
    // Video Mode's bar): `dark` on a dark theme, near opaque on a light one,
    // whose dark text would sit on a grey veil over a dark level.
    float overLevelAlpha(float dark);

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
