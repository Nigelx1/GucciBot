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
//     taskbar already made is taken off (ITaskbarList::DeleteTab).
// GD loads its window before GucciBot loads (a non-early mod), so the first
// few seconds of a launch can still show a button; assist.py keeps the window
// off the screen meanwhile. Any other launch is untouched. Windows only.

#include "hacks/safemode.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Windows.h>
#include <ShObjIdl.h>

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

    // Tool window + no flashing + no taskbar button; true when a window was changed.
    bool quieten(ITaskbarList* taskbar) {
        bool changed = false;
        for (HWND h : ourWindows()) {
            LONG_PTR const ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            LONG_PTR const want = (ex | WS_EX_TOOLWINDOW) & ~(LONG_PTR)WS_EX_APPWINDOW;
            if (ex != want) {
                SetWindowLongPtrW(h, GWL_EXSTYLE, want);
                changed = true;
            }
            if (IsWindowVisible(h) && taskbar) taskbar->DeleteTab(h);
        }
        return changed;
    }

    void quietThread() {
        // Off the main thread: the window calls below wait on GD's message
        // loop, and the game must not wait on them. For the first minute,
        // every half second (a window shown again gets its button back).
        bool const com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ITaskbarList* taskbar = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
            FAILED(taskbar->HrInit())) {
            taskbar->Release();
            taskbar = nullptr;
        }
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        int changed = 0;
        while (std::chrono::steady_clock::now() < until) {
            if (quieten(taskbar)) ++changed;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        log::info("[quiet window] done ({} change{})", changed, changed == 1 ? "" : "s");
        if (taskbar) taskbar->Release();
        if (com) CoUninitialize();
    }

}  // namespace

$on_mod(Loaded) {
    if (!quietLaunch()) return;
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
