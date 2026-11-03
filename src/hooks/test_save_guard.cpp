// GucciBot (2026-11-03): a test launch never writes GD's save files.
//
// Claude's test launches (tools/assist.py starts GD with GUCCIBOT_FORCE_SAFE_MODE
// set) play on Nigel's real account, and GD saves what they did when it closes or
// autosaves: the online levels they opened went into CCGameManager.dat (GLM_03),
// with attempt counts and song entries. So in a test launch every save GD would
// write is skipped: GManager::save is what writes CCGameManager(2).dat and
// CCLocalLevels(2).dat (both managers are GManagers), and AppDelegate::trySaveGame
// is the exit/background/autosave path that calls them. Normal launches, and a
// game only forced into safe mode later by the gucci_force_safe_mode tool, save
// as they always have.

#include "hacks/safemode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/AppDelegate.hpp>
#include <Geode/modify/GManager.hpp>

using namespace geode::prelude;

namespace {
    bool skipSave(char const* what) {
        if (!gucci::safemode::testLaunch()) return false;
        static int s_logged = 0;
        if (s_logged < 20) {
            ++s_logged;
            log::info("[save guard] test launch: skipped {}", what);
        }
        return true;
    }
}  // namespace

class $modify(GBTestSaveGuardGM, GManager) {
    void save() {
        if (skipSave("GManager::save")) return;
        GManager::save();
    }
};

class $modify(GBTestSaveGuardApp, AppDelegate) {
    void trySaveGame(bool force) {
        if (skipSave("AppDelegate::trySaveGame")) return;
        AppDelegate::trySaveGame(force);
    }
};
