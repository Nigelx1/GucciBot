// The mouse cursor while the menu is open: see ui/cursor.hpp.

#include "ui/cursor.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS

#include <Geode/modify/CCEGLView.hpp>

#include <Windows.h>

#include <cstddef>

// The fields GD's showCursor / toggleLockCursor write (libcocos2d.dll,
// 2.2081): if Geode's header ever moves them, this stops compiling instead of
// reading the wrong bytes.
static_assert(offsetof(CCEGLView, m_bCursorHidden) == 0xb8);
static_assert(offsetof(CCEGLView, m_bShouldHideCursor) == 0xeb);
static_assert(offsetof(CCEGLView, m_bCursorLocked) == 0xec);

namespace {

    bool g_held = false;      // the menu is open and holds the cursor shown
    bool g_wantHide = false;  // GD's wish while it does
    bool g_wantLock = false;
    bool g_hadFocus = true;
    bool g_ours = false;      // our own calls go through the hooks below

    bool gameHasFocus() {
        HWND const fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg)
            GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    char const* wishName() {
        return g_wantLock ? "locked" : g_wantHide ? "hidden" : "shown";
    }

    void showNow(CCEGLView* view) {
        g_ours = true;
        view->showCursor(true);  // also lets go of a lock (GLFW's normal mode)
        g_ours = false;
    }

} // namespace

struct GB7MenuCursor : Modify<GB7MenuCursor, CCEGLView> {
    // While the menu holds the cursor, GD's calls only change its wish (what
    // GD's own code would have done to these two fields), applied when the
    // menu closes.
    void showCursor(bool state) {
        if (g_held && !g_ours) {
            g_wantHide = !state;
            g_wantLock = false;  // showCursor lets go of a lock too
            return;
        }
        CCEGLView::showCursor(state);
    }

    void toggleLockCursor(bool lock) {
        if (g_held && !g_ours) {
            g_wantLock = lock;
            return;
        }
        CCEGLView::toggleLockCursor(lock);
    }
};

namespace gucci::ui::cursor {

    void frame(bool menuOpen) {
        auto* view = CCEGLView::get();
        if (!view)
            return;
        if (menuOpen && !g_held) {
            g_wantHide = view->getShouldHideCursor();
            g_wantLock = view->getCursorLocked();
            g_hadFocus = gameHasFocus();
            g_held = true;
            if (g_wantHide || g_wantLock || view->m_bCursorHidden) {
                showNow(view);
                log::info("[cursor] menu open: cursor shown (GD wants it {})", wishName());
            }
        } else if (!menuOpen && g_held) {
            g_held = false;
            // What GD wants now, set the way GD sets it.
            if (g_wantHide)
                view->showCursor(false);
            if (g_wantLock)
                view->toggleLockCursor(true);
            if (g_wantHide || g_wantLock)
                log::info("[cursor] menu closed: cursor back to {}, as GD wants", wishName());
        }
        if (!g_held)
            return;
        // GD unlocks the cursor when its window loses the focus
        // (CCEGLView::onGLFWWindowFocus) and doesn't lock it again until the
        // next resume, so a lock kept aside goes the same way.
        bool const focused = gameHasFocus();
        if (g_hadFocus && !focused)
            g_wantLock = false;
        g_hadFocus = focused;
        // Anything that hid or locked it without the two calls above (nothing
        // in GD does): take that as GD's wish too, and show it again.
        bool const wantsHide = view->getShouldHideCursor();
        bool const wantsLock = view->getCursorLocked();
        if (wantsHide || wantsLock || view->m_bCursorHidden) {
            g_wantHide = g_wantHide || wantsHide;
            g_wantLock = g_wantLock || wantsLock;
            showNow(view);
        }
    }

    State state() {
        State s;
        s.supported = true;
        s.menuHasIt = g_held;
        if (auto* view = CCEGLView::get()) {
            s.gdWantsHidden = g_held ? g_wantHide : view->getShouldHideCursor();
            s.gdWantsLocked = g_held ? g_wantLock : view->getCursorLocked();
            s.lockedNow = view->getCursorLocked();
            s.hiddenNow = view->m_bCursorHidden || s.lockedNow;
            s.gameplayActive = view->getGameplayActive();
        }
        if (auto* gm = GameManager::get()) {
            s.showInGame = gm->getGameVariable("0024");
            s.lockOption = gm->getGameVariable("0128");
        }
        return s;
    }

} // namespace gucci::ui::cursor

#else

namespace gucci::ui::cursor {

    void frame(bool) {}

    State state() {
        return {};
    }

} // namespace gucci::ui::cursor

#endif
