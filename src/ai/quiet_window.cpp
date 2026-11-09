// GucciBot (2026-11-03): test games stay out of sight - no taskbar button, no
// flashing, never the focus.
//
// The AI's learning loop runs several Geometry Dash games at once in the
// background (tools/ai/rl_loop.py --games) while Nigel uses his PC, and each
// one asked for attention: GD's window wants the focus while it loads, and
// Windows, which refuses it to a game started from a background script, flashes
// its taskbar button instead. tools/assist.py starts a game it keeps minimized
// with GUCCIBOT_QUIET_WINDOW=1 (only in a test launch: safe mode forced), and
// from the moment GucciBot loads:
//   - SetForegroundWindow, FlashWindow and FlashWindowEx do nothing in that
//     game (it can't take the focus or flash any more);
//   - its window becomes a tool window, so it has no taskbar button (and isn't
//     in Alt+Tab), any flashing started before is stopped, and the button the
//     taskbar already made is taken off (ITaskbarList::DeleteTab);
//   - minimized, it is kept off the screen (a minimized tool window would sit
//     at the bottom left of the desktop as a small title bar).
// GD loads its window before GucciBot loads (a non-early mod), so the first
// few seconds of a launch can still show a button; assist.py keeps the window
// off the screen meanwhile. Any other launch is untouched. Windows only.

#include "hacks/safemode.hpp"
#include "core/mcp_only.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Windows.h>
#include <ShObjIdl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {

    bool quietLaunch() {
        char const* v = std::getenv("GUCCIBOT_QUIET_WINDOW");
        return v && *v && std::string_view(v) != "0" && gucci::safemode::testLaunch();
    }

    BOOL WINAPI noForeground(HWND) { return TRUE; }
    BOOL WINAPI noFlash(HWND, BOOL) { return FALSE; }
    BOOL WINAPI noFlashEx(PFLASHWINFO) { return FALSE; }

    template <class Detour>
    void hookOne(char const* name, Detour detour) {
        HMODULE const user32 = GetModuleHandleW(L"user32.dll");
        void* addr = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, name)) : nullptr;
        if (!addr) {
            log::warn("[quiet window] no {} in user32", name);
            return;
        }
        auto res = Mod::get()->hook(addr, detour, fmt::format("user32 {} (quiet test window)", name),
                                    tulip::hook::TulipConvention::Default);
        if (res.isErr()) log::warn("[quiet window] couldn't hook {}: {}", name, res.unwrapErr());
        else gucci::mcponly::keepHook(res.unwrap());  // stays in an MCP-only test launch (core/mcp_only.hpp)
    }

    std::vector<HWND> ourWindows() {
        std::vector<HWND> out;
        EnumWindows(
            [](HWND h, LPARAM p) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == GetCurrentProcessId() && !GetWindow(h, GW_OWNER))
                    reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&out));
        return out;
    }

    // Tool window, no taskbar button, and when minimized, off the screen: a
    // minimized tool window is otherwise parked as a little title bar at the
    // bottom left of the desktop (seen 2026-11-03), since the taskbar has no
    // button to shrink it into. True when a window was changed.
    bool quieten(ITaskbarList* taskbar) {
        bool changed = false;
        for (HWND h : ourWindows()) {
            if (!IsWindowVisible(h)) continue;
            LONG_PTR const ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            LONG_PTR const want = (ex | WS_EX_TOOLWINDOW) & ~(LONG_PTR)WS_EX_APPWINDOW;
            if (ex != want) {
                SetWindowLongPtrW(h, GWL_EXSTYLE, want);
                if (taskbar) taskbar->DeleteTab(h);
                changed = true;
            }
            RECT r{};
            if (IsIconic(h) && GetWindowRect(h, &r) && r.left > -30000) {
                WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
                if (GetWindowPlacement(h, &wp)) {
                    wp.flags |= WPF_SETMINPOSITION | WPF_ASYNCWINDOWPLACEMENT;
                    wp.ptMinPosition = POINT{-32000, -32000};
                    wp.showCmd = SW_SHOWMINNOACTIVE;
                    SetWindowPlacement(h, &wp);
                    changed = true;
                }
            }
        }
        return changed;
    }

    void quietThread() {
        // Off the main thread: the window calls below wait on GD's message
        // loop, and the game must not wait on them. Every half second for as
        // long as the game runs: assist.py shows the window for a screenshot
        // and minimizes it again.
        bool const com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ITaskbarList* taskbar = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
            FAILED(taskbar->HrInit())) {
            taskbar->Release();
            taskbar = nullptr;
        }
        int changed = 0;
        for (;;) {
            if (quieten(taskbar) && ++changed <= 5) log::info("[quiet window] window quietened");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        (void)com;
    }

    // GucciBot (2026-11-09): an MCP-only test launch (core/mcp_only.hpp)
    // drives another mod's UI, which may open windows of its own (an OS file
    // dialog, a message box), and GD puts its own window back over the whole
    // monitor when it is restored. None of the game's top-level windows may
    // come on the screen: one that is shown or moves onto a monitor is put to
    // the right of every monitor at once, never activated. In-context, so this
    // runs in the thread that showed or moved the window, before the desktop
    // draws it there.
    void CALLBACK keepOffScreen(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
        if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return;
        int const vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int const vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (r.right <= vx || r.left >= vr || r.bottom <= vy || r.top >= vb) return;  // off every monitor already
        SetWindowPos(hwnd, nullptr, vr + 200, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        static std::atomic<int> s_logged{0};
        if (s_logged.fetch_add(1) < 20) {
            wchar_t cls[64]{};
            GetClassNameW(hwnd, cls, 64);
            log::info("[quiet window] MCP-only launch: a window ({}) came onto the screen at {},{}; moved off it",
                      geode::utils::string::wideToUtf8(cls), r.left, r.top);
        }
    }

    void keepWindowsOffScreen() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&keepOffScreen), &self);
        if (!SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, self, &keepOffScreen,
                             GetCurrentProcessId(), 0, WINEVENT_INCONTEXT))
            log::warn("[quiet window] couldn't watch the game's windows (SetWinEventHook: {})", GetLastError());
        else
            log::info("[quiet window] MCP-only launch: every window of the game is kept off the screen");
        // Whatever is on a monitor already goes now.
        for (HWND h : ourWindows())
            keepOffScreen(nullptr, EVENT_OBJECT_LOCATIONCHANGE, h, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    }

}  // namespace

$on_mod(Loaded) {
    if (!quietLaunch()) return;
    if (gucci::mcponly::launch()) keepWindowsOffScreen();
    log::info("[quiet window] test launch with GUCCIBOT_QUIET_WINDOW: no focus, no flashing, no taskbar button");
    // Any flashing started while GD loaded stops, before FlashWindowEx is a no-op.
    for (HWND h : ourWindows()) {
        FLASHWINFO stop{sizeof(FLASHWINFO), h, FLASHW_STOP, 0, 0};
        FlashWindowEx(&stop);
    }
    hookOne("SetForegroundWindow", &noForeground);
    hookOne("FlashWindow", &noFlash);
    hookOne("FlashWindowEx", &noFlashEx);
    std::thread(quietThread).detach();
}

#endif
