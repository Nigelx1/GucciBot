#pragma once

// The menu's two layouts (Settings > Look > Layout; Nigel, 2026-11-11/12):
//   Pages   - the one window with a sidebar (ui/shell.cpp drawMain), the default.
//   Windows - a bar across the top (the pages, the search box, Close) and each
//             card of every page shown as a window of its own, dragged by its
//             title bar and collapsed with its arrow, in GucciBot's own look.
//
// The cards are the same in both: kit::BeginCard asks cardBegin() whether to
// put the card in a window of its own, so every control is the same code on
// the same values with the same saved settings. What a page draws outside its
// cards goes in the page's own window, which shows only once it has something.
//
// Where each window was dragged to, which are collapsed and which pages are
// showing are saved values ("ui_windows"), so they come back after a restart.
// Phones keep Pages (windows() is false there whatever is saved).
//
// Main thread, inside the ImGui frame, except the test hooks (between frames).

#include <imgui.h>

#include <string>
#include <vector>

namespace gucci::ui::layout {

    // Windows can be picked here (not on Android or iOS).
    bool available();
    // The Windows layout is in use.
    bool windows();
    void setWindows(bool on);
    // From the saved values (the shell's loadSettings).
    void load();

    // The pages whose windows show (by title).
    bool shown(std::string const& page);
    void setShown(std::string const& page, bool on);
    std::vector<std::string> shownPages();

    // ---- the shell, drawing the Windows layout

    // New windows tile from `top` (under the bar) down and across.
    void frameBegin(float top);
    void frameEnd();
    // Around each shown page's draw: its own window (off screen while it has
    // nothing outside its cards) is the current window in between.
    void pageBegin(const char* page);
    void pageEnd();
    // Between pageBegin and pageEnd.
    bool drawingPage();

    // ---- the kit (kit::BeginCard / EndCard)

    // A card is starting: true when it got a window of its own (drawing a
    // page as windows, and no card open around it). Always pair with cardEnd.
    bool cardBegin(const char* title);
    // ... ends it; true when it had its own window.
    bool cardEnd();
    // Drawing a page as windows, outside every card: the kit's gaps and
    // dividers between cards are left out there.
    bool atPageLevel();
    // Drawing a page as windows: the window of one of its cards called
    // `title` (any of them, if the page has more than one) has the keyboard
    // focus.
    bool cardFocused(const char* title);
    // Any of the layout's windows (a card's, a page's, the bar, the search
    // results) has the keyboard focus.
    bool ownsFocus();

    // ---- Assistant Access (gucci_layout, mcp/layout_tool.cpp)

    struct WindowInfo {
        std::string key;    // "Hacks/Gameplay#0" ("Hacks/" for the page's own window)
        std::string title;
        std::string page;
        ImVec4 rect{};      // x, y, w, h as drawn last
        bool collapsed = false;
        bool folded = false;  // shown as its title bar: the screen had no room for it open
        bool placed = false;  // dragged (stays put) rather than tiled
        ImVec2 saved{};       // where it was dragged to
        bool drawn = false;   // drawn in the last frame
        bool pageWindow = false;
    };
    std::vector<WindowInfo> windowList();
    // Done at the window's next draw, as a finished drag (or a click on its
    // arrow) is: false when no window has that key.
    bool requestMove(std::string const& key, ImVec2 pos);
    bool requestCollapse(std::string const& key, bool collapsed);
    // ... given the keyboard focus, as a click in it does.
    bool requestFocus(std::string const& key);
    // The key of the layout's window that has the keyboard focus ("" if none).
    std::string focusedKey();
    // What "ui_windows" holds now (the saved value).
    std::string savedText();
    // Every window back to tiling, none collapsed (saved).
    void resetPlaces();

} // namespace gucci::ui::layout
