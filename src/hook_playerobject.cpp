#include "GucciBot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
using namespace geode::prelude;

class $modify(GB7PlayerObject, PlayerObject) {
    void update(float dt) {
        auto& upd = GucciEngine::get()->updater;
        upd.m_lastPlayerX = upd.m_currentPlayerX;
        PlayerObject::update(dt);
        upd.m_currentPlayerX = getPositionX();
    }
    void playDeathEffect() {
        auto& upd = GucciEngine::get()->updater;
        if (upd.m_preventDeath || upd.m_predicting) return;
        PlayerObject::playDeathEffect();
    }
    void playSpawnEffect() {
        if (GucciEngine::get()->practiceFix.m_loadCheckpoint) return;
        PlayerObject::playSpawnEffect();
    }
    void releaseAllButtons() {
        auto* gb  = GucciEngine::get();
        if (gb->updater.m_canDie || gb->isPlaying())
            PlayerObject::releaseAllButtons();
    }
};
