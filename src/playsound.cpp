#include "GucciBot.hpp"
#include "clicksounds.hpp"
#include "playsound.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
using namespace geode::prelude;

void triggerClickAudio(bool p2, int button, bool pressed) {
    auto* csm = ClickSoundManager::get();
    if (!csm->enabled || button != 1) return;
    if (GucciEngine::get()->isPlaying() && !csm->playDuringPlayback) return;
    csm->playClick(pressed, p2);
}

class $modify(GB7ClickSoundPL, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto* csm = ClickSoundManager::get();
        if (csm->enabled && csm->backgroundNoiseEnabled) csm->startBackgroundNoise();
    }
    void onQuit() {
        ClickSoundManager::get()->clearPendingClicks();
        ClickSoundManager::get()->stopBackgroundNoise();
        PlayLayer::onQuit();
    }
    void resetLevel() {
        ClickSoundManager::get()->clearPendingClicks();
        ClickSoundManager::get()->stopBackgroundNoise();
        PlayLayer::resetLevel();
        auto* csm = ClickSoundManager::get();
        if (csm->enabled && csm->backgroundNoiseEnabled) csm->startBackgroundNoise();
    }
};
