#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>

using namespace geode::prelude;

// Icon hack (Nigel, 2026-11-21: "could we jus add an icon hack toggle"): while
// it's on, GD's garage takes every icon, color, trail, death effect and ship
// fire as unlocked, so any of them can be picked and worn. It's only how the
// player looks, so neither safe mode nor Legit only counts it as a cheat.
// GD asks these three to decide what's locked. GJItem unlocks (keys, chests'
// rewards, features GD hands out) aren't icons and stay as they are. Nothing
// is saved as unlocked: switched off, GD's own answers come back.

namespace {
    bool iconHackOn() {
        auto* gb = gucci::GucciEngine::get();
        return gb && gb->hackIconHack;
    }
}

class $modify(GBIconHackGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (iconHackOn())
            return true;
        return GameManager::isIconUnlocked(id, type);
    }

    bool isColorUnlocked(int id, UnlockType type) {
        if (iconHackOn())
            return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

class $modify(GBIconHackGameStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        if (iconHackOn() && type != UnlockType::GJItem)
            return true;
        return GameStatsManager::isItemUnlocked(type, id);
    }
};
