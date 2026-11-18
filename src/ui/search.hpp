#pragma once

// The menu's search box (Nigel, 2026-11-11: "A search box for the whole
// menu. Type noclip or hitborders and it jumps straight to that switch.").
//
// The index is built from the kit itself, at the same calls the menu probe
// (ui/probe.hpp) records: every card, row, feature switch and button the
// pages draw, so a new control is searchable without anything added for it.
// The shell (ui/shell.cpp) draws every page the sidebar lists, once, into a
// hidden window off screen with the recorder on (indexing()); a feature's
// options are drawn too while that runs (kit::FeatureBegin opens its body),
// so a switch under one that is off is found as well. Rows an `if` hides
// right now (another Pathfinder engine's card) are not.
//
// A pick opens the page, scrolls to the control and highlights it for a
// moment: the kit reports where it drew the control (and the card and the
// switches it sits under) while the live page draws, and the deepest one
// drawn is the one shown, so a control under a switch that's off shows its
// switch.
//
// Everything here runs on the main thread, inside the ImGui frame.

#include <imgui.h>

#include <optional>
#include <string>
#include <vector>

namespace gucci::ui::search {

    namespace detail {
        extern bool g_rec;
        extern bool g_indexing;
    }

    // The kit should report to the recorder (an index pass, or a jump being
    // found or highlighted on the live page). One bool read otherwise.
    inline bool recording() {
        return detail::g_rec;
    }
    // The hidden index pass is drawing: pages skip their side effects (a
    // trainer counting as open, the theme editor's preview, a captured key).
    inline bool indexing() {
        return detail::g_indexing;
    }

    // ---- the kit (ui/kit.cpp), each behind `if (search::recording())`

    void card(const char* title, const char* caption);  // inside the card's child, before its title
    void endCard();                                      // inside it, before EndChild
    void row(const char* label, const char* hint);       // RowBegin, before the label is drawn
    void endRow(ImVec2 start, float bottom);             // RowEnd: where the row began and its bottom
    void featureBody(bool open);                         // a feature's options begin (true) or end
    void button(const char* label);                      // after a kit::Button is drawn
    void hotkey();                                       // a kit::Key in the row: it's a hotkey's row
    // A page's own tab bar (Macro Tools): what's drawn between these is on
    // that tab. The index pass draws every tab's contents (an ImGui tab bar
    // shows only one); a jump to a control on a tab not showing selects it
    // (selectTab true once: pass ImGuiTabItemFlags_SetSelected).
    void tabBegin(const char* label);
    void tabEnd();
    bool selectTab(const char* label);
    // A depth around a control's own buttons (kit::Key's), so they aren't
    // listed as buttons of their own.
    struct Mute {
        Mute();
        ~Mute();
        Mute(Mute const&) = delete;
        Mute& operator=(Mute const&) = delete;
    };

    // ---- the shell

    // Once a frame, before anything is drawn.
    void frameBegin();
    // The shell should draw the index pass this frame (asked for, and no menu
    // probe capture running).
    bool wantIndex();
    void indexBegin();                    // the hidden pass starts
    void indexPage(const char* title);    // ... each page, before it draws
    void indexEnd();
    // Around the live page's draw (the full menu's ##page child).
    void livePageBegin(const char* title);
    void livePageEnd();
    // The Windows layout (ui/layout.hpp): the shell draws several pages, so
    // it calls livePageBegin/End around the picked one only (jumpPage, "" for
    // no jump going), and "in view" is the screen inside the card's own
    // window (setFreeWindows, around the pages). seekingCard: a card on the
    // jump's way (its window opens and comes to the front). menuFocused: for
    // the next field() this frame, whether any of the layout's windows has the
    // keyboard (Ctrl+F's check), in place of the window the box is in.
    std::string jumpPage();
    void setFreeWindows(bool on);
    bool seekingCard(std::string const& page, std::string const& title);
    void menuFocused(bool focused);
    // A page to switch to after a pick (its title), once.
    std::optional<std::string> takePick();

    // The search box, on the full menu's pages only (Compact Mode has none:
    // Nigel, 2026-11-15). `width` > 0 exact, else the rest of the line.
    void field(float width);
    // The box has text: the results go where the page would be.
    bool showing();
    // The results, as a card in place of the page.
    void results();

    // ---- Assistant Access (gucci_search, mcp/search_tool.cpp)

    struct Result {
        std::string label;
        std::string where;  // "Macro > Hitboxes"
        std::string kind;   // card, row, feature, hotkey, button
        std::string hint;
        int score = 0;
    };
    std::string query();
    std::vector<Result> currentResults();
    // What the results say when nothing matches ("" when something does).
    std::string emptyMessage();
    // The box's ImGui ID (0 when it wasn't drawn last frame: the menu is
    // closed or in Compact Mode) and each result row's.
    ImGuiID fieldId();
    std::vector<ImGuiID> resultIds();
    bool fieldActive();
    // Builds the index at the next frame the shell can.
    void requestIndex();
    int indexSize();
    int indexBuilds();
    // Where the last pick is: "seeking", "found" or "" (none), what it
    // highlighted (its key and kind: the control itself, or the switch or
    // card it sits under), and for how long more (seconds).
    struct JumpState {
        std::string phase;
        std::string page;
        std::string target;      // the picked control's label
        std::string shownKind;   // "control", "switch", "card", or "" until found
        std::string shownLabel;
        ImVec4 rect{};           // x, y, w, h of what's highlighted
        bool inView = false;
        float highlightLeft = 0.f;
        int frames = 0;
    };
    JumpState jumpState();
    // Clears the box (what Escape does), letting go of the keyboard.
    void clear();

    // Ctrl is held and the menu that draws the box has the keyboard (last
    // frame): an F now goes to the box, not to a hotkey bound to F.
    // cocosCtrl is the keyboard dispatcher's own Ctrl (m_bControlPressed),
    // which Geode sets right before each key reaches cocos. ImGui's Ctrl
    // alone isn't enough: imgui-cocos hands ImGui a key going down only
    // while ImGui already wants the keyboard (a box being typed in), so with
    // the menu merely open ImGui never sees Ctrl or F go down.
    bool ctrlFIsMine(bool cocosCtrl);
    // Ctrl+F from cocos's keyboard: the box takes the cursor at its next
    // field(), when the menu has the keyboard and nothing is being typed in.
    void focusBox();

    // Back (a phone's back button, or Escape as cocos sends it) while a menu
    // text box is being typed in: the box lets go instead of the game going
    // back. True when it was taken.
    bool takeBack();

} // namespace gucci::ui::search
