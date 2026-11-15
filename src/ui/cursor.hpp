#pragma once

// GucciBot (2026-11-15): the mouse cursor shows while the menu is open (a
// user's request: "when the menu is open, have your mouse be visible at all
// times, even if it wouldnt be there without it (middle of the game)").
//
// How GD does it on Windows (read from its libcocos2d.dll and PlayLayer,
// 2.2081): CCEGLView::showCursor(bool) and toggleLockCursor(bool) are the only
// calls that hide, show or lock the cursor (they set GLFW's cursor mode;
// "locked" is GLFW's disabled mode, which hides it too). A level hides it as
// it starts or resumes (PlayLayer::init, resume, resumeAndRestart:
// toggleLockCursor(true) with Lock Cursor on, else showCursor(false) unless
// Show Cursor In-Game is on) and shows it on pause, the end screen, the retry
// screen and leaving. GD keeps its wish in m_bShouldHideCursor and
// m_bCursorLocked; the window losing the focus unlocks it, and the cursor
// coming back into the window hides it again while GD wants it hidden.
//
// While the menu is open GucciBot holds the cursor shown and unlocked, and
// keeps GD's wish aside: GD's own showCursor / toggleLockCursor calls in that
// time (a level starting, a resume) only change the wish. When the menu
// closes, the cursor goes to what GD wants at that moment, the way GD itself
// would have set it. Outside a level GD wants it shown, so nothing changes.
//
// Windows only. Phones have no cursor; on a Mac GD hides it through its own
// PlatformToolbox code, which this doesn't touch (the Mac is as before).

namespace gucci::ui::cursor {

    // Once a frame from the menu's draw (ui/shell.cpp, frame()), open or not.
    void frame(bool menuOpen);

    // For Assistant Access (gucci_get_state's "cursor").
    struct State {
        bool supported = false;      // Windows: the menu holds the cursor
        bool menuHasIt = false;      // the menu is holding it shown right now
        bool gdWantsHidden = false;  // GD's wish (kept aside while the menu has it)
        bool gdWantsLocked = false;
        bool hiddenNow = false;      // GD's view of the cursor: hidden, or locked (also hidden)
        bool lockedNow = false;
        bool gameplayActive = false; // GD: a level is being played (not paused)
        bool showInGame = false;     // game option 0024, Show Cursor In-Game
        bool lockOption = false;     // game option 0128, Lock Cursor
    };
    State state();

} // namespace gucci::ui::cursor
