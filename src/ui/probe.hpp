#pragma once

// The menu probe (Assistant Access, 2026-11-01): what one frame of
// GucciBot's ImGui interface shows, as data, so a session reads the menu as
// text instead of off a screenshot. gucci_ui, gucci_ui_view and
// gucci_overlays ask for it (mcp/mcp_tools.cpp).
//
// A tool asks between frames (request()); the next frame the shell draws
// (ui/shell.cpp's frame()) is the capture frame, and three layers record it:
//   - the kit (ui/kit.cpp): every card, row and control the menu is built
//     from, with its value, enabled or not and whether it's in view. Each kit
//     function calls in here behind `if (probe::on())`.
//   - ImGui's test-engine hooks (IMGUI_ENABLE_TEST_ENGINE): every item with
//     an ID, the raw ImGui ones too (the macro list's rows, a row's "...",
//     the popups' items, tabs), with its label and rect.
//   - ImGui's text log: the text as drawn, raw ImGui text and tables too.
//     Logging also submits what's scrolled out of view, so the text is the
//     whole page, not the part in sight; the picture doesn't change, the
//     scissor still cuts it.
//
// Costs nothing when no tool is asking: the kit's checks read one bool, and
// ImGui calls its hooks only while GImGui->TestEngineHookItems is on (the
// capture frame alone). All of it runs inside the ImGui frame on the main
// thread; nothing is saved.

#include "ui/look.hpp"

#include <imgui.h>
#include <matjson.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gucci::ui::probe {

    namespace detail {
        extern bool g_on;
    }

    // This frame is being captured.
    inline bool on() {
        return detail::g_on;
    }

    // ---- asked between frames (the tools' pump, main thread)

    // Capture the next frame the shell draws. A capture already in is dropped.
    void request();
    // The capture asked for has been taken.
    bool ready();

    // What to return, and how.
    struct Query {
        std::string window;     // "" or "all": every window; "menu", "compact", "overlays", "popups", or a name
        std::string card;       // only this card's elements and text (its title, exactly, then ignoring case)
        // find: elements matching all that are set (exact, then ignoring
        // case, then a part), the label without ##...
        std::string page;       // checked against the menu's page
        std::string findWindow, findCard, findRow, findLabel, findKind, findValue, findContext;
        std::optional<uint32_t> findId;
        std::optional<int> findIndex;  // the n-th match only
        bool finding = false;
        bool visibleOnly = false;
        bool lint = false;
        int max = 800;           // elements
        int maxLines = 400;      // text lines a window
        bool text = true;        // the text outline (else JSON)
    };
    // The capture, as gucci_ui answers: JSON {frame, imgui_frame, display,
    // framebuffer, menu, windows, elements, text, lint, truncated}, or with
    // q.text {outline, ...} where outline is the menu as lines.
    matjson::Value result(Query const& q);

    // A window that was drawn in the last capture, for scrolling and crops:
    // its ImGui name, rect (x, y, w, h in ImGui's pixels), scroll and the
    // height in view. Found by key ("page", "menu", "compact", "macros") or by
    // name (exactly, then a part).
    struct WindowInfo {
        std::string name;
        std::string key;
        ImVec4 rect{};
        float scrollY = 0.f;
        float scrollMaxY = 0.f;
        float viewH = 0.f;
    };
    std::optional<WindowInfo> window(std::string const& key);
    // A card's rect (x, y, w, h) in the last capture, by its title.
    std::optional<ImVec4> cardRect(std::string const& title);
    // The elements matching a query's find, with their rects, in the last capture.
    struct Hit {
        int n = 0;
        std::string path;
        ImVec4 rect{};
        std::string window;
    };
    std::vector<Hit> find(Query const& q);
    // The titles of the cards in view in the last capture, in order.
    std::vector<std::string> visibleCards();

    // Scroll an ImGui window (by its name) between frames: it moves at the
    // window's next Begin. False when no window has that name.
    bool scrollTo(std::string const& windowName, float y);

    // ---- the shell (ui/shell.cpp)

    // Around the shell's frame(): the capture starts and ends with it, so
    // every early return is covered.
    struct FrameGuard {
        FrameGuard();
        ~FrameGuard();
        FrameGuard(FrameGuard const&) = delete;
        FrameGuard& operator=(FrameGuard const&) = delete;
    };

    // Everything recorded inside carries ctx {key: value} (a saved macro's
    // row and its "..." menu: {macro: name}).
    struct Scope {
        Scope(const char* key, std::string value);
        ~Scope();
        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;
        bool active = false;
    };
    // A row of the Saved macros list (its Selectable is the last item).
    void macroRow(std::string const& name, std::string const& ext, std::string const& tag, bool selected);

    // ---- the kit (ui/kit.cpp), each behind `if (probe::on())`

    void card(const char* title, const char* caption);  // inside the card, before its title
    void endCard();                                      // after its EndChild
    void row(const char* label, const char* hint);       // before the label is drawn
    void endRow();
    void feature(bool open);                             // a feature's body begins (true) or ends

    enum class TextKind { Section, Label, Paragraph, Hint };
    // Around a text kit function: the text is recorded once it's drawn, and
    // logged with its mark ("~ " for a hint, "! " for a warning).
    struct Text {
        Text(TextKind kind, const char* text, Tone tone);
        ~Text();
        Text(Text const&) = delete;
        Text& operator=(Text const&) = delete;
        bool counted = false;  // inside a capture (nested ones count, only the outer one records)
        bool active = false;
        TextKind kind;
        const char* text;
        Tone tone;
    };
    // Text the kit draws straight onto the draw list (Note, Chip), so ImGui's
    // log never sees it: logged here, at its place.
    void note(const char* text, Tone tone, ImVec2 a, ImVec2 b);
    void chip(const char* text, Tone tone, ImVec2 a, ImVec2 b);
    void progress(float fraction, const char* overlay);
    // A hint drawn inside a row's label column: kept out of the log there
    // and said after the row's controls instead (endRow), so a row reads as
    // one line. RAII, a no-op unless capturing.
    struct Quiet {
        Quiet();
        ~Quiet();
        Quiet(Quiet const&) = delete;
        Quiet& operator=(Quiet const&) = delete;
        bool active = false;
    };

    // Controls, after they're drawn; `widget` is the ImGui label the kit gave
    // it inside its own PushID (its ID is worked out from it).
    void toggle(bool value);
    void choice(int index, const char* const* options, int count);
    void dropdown(int index, const char* const* options, int count);
    void dropdown(int index, std::vector<std::string> const& options, const char* placeholder);
    void slider(const char* widget, double value, double lo, double hi);
    void input(const char* widget, matjson::Value value, const char* placeholder = nullptr);
    void button(const char* label, Tone tone);
    void color(ImVec4 const& c);
    void key(std::string const& name, bool waiting);

} // namespace gucci::ui::probe
