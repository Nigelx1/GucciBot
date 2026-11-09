// GucciBot (2026-11-03): AI jobs and test launches keep running while the
// game's window is minimized.
//
// Minimizing GD calls AppDelegate::applicationDidEnterBackground, which pauses
// the level and stops the director's animation: no frame is drawn, and with no
// frame nothing ticks - not the AI's job slices (engine_updater.cpp drawScene),
// not Assistant Access's calls (they run on the main thread between frames).
// Nigel lets the AI practice in the background for days with the monitor off,
// and tools/assist.py minimizes the games it starts; so while an AI job runs,
// or in a test launch (safe mode forced: GUCCIBOT_FORCE_SAFE_MODE or
// gucci_force_safe_mode), going to the background is skipped and the game runs
// on at its normal frame rate. Any other time a minimized game pauses as it
// always has. Coming back skips the foreground half only when the background
// half was skipped. Windows only: a phone's background is the OS taking the
// GL context away, which no game may ignore.

#include "ai/job.hpp"
#include "core/mcp_only.hpp"
#include "hacks/safemode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/AppDelegate.hpp>

using namespace geode::prelude;

namespace {
    bool s_keptRunning = false;  // the last trip to the background was skipped

    bool keepRunning() {
#ifdef GEODE_IS_WINDOWS
        return gucci::safemode::forced() || gucci::ai::job::busy();
#else
        return false;
#endif
    }
}  // namespace

class $modify(GBBackgroundRunHook, AppDelegate) {
    static void onModify(auto& self) {
        gucci::mcponly::keep(self);  // stays in an MCP-only test launch (core/mcp_only.hpp)
    }

    void applicationDidEnterBackground() {
        if (keepRunning()) {
            if (!s_keptRunning) log::info("[AI] minimized: kept running (an AI job or a test launch)");
            s_keptRunning = true;
            return;
        }
        s_keptRunning = false;
        AppDelegate::applicationDidEnterBackground();
    }

    void applicationWillEnterForeground() {
        if (s_keptRunning) {
            s_keptRunning = false;
            return;
        }
        AppDelegate::applicationWillEnterForeground();
    }
};
