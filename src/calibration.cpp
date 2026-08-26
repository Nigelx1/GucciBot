#include "calibration.hpp"
#include "clicksounds.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <cmath>
#include <numeric>

using namespace geode::prelude;

namespace {
    constexpr float kClickTimeoutSec = 2.5f;
}

CalibrationService& CalibrationService::get() {
    static CalibrationService inst;
    return inst;
}

const char* CalibrationService::gamemodeName(int index) {
    switch (index) {
        case GM_Cube:   return "Cube";
        case GM_Ship:   return "Ship";
        case GM_Ball:   return "Ball";
        case GM_Ufo:    return "UFO";
        case GM_Wave:   return "Wave";
        case GM_Robot:  return "Robot";
        case GM_Spider: return "Spider";
        default:        return "?";
    }
}

int CalibrationService::gamemodeIndexFor(PlayerObject* player) {
    if (!player) return GM_Cube;
    if (player->m_isShip)   return GM_Ship;
    if (player->m_isBird)   return GM_Ufo;
    if (player->m_isBall)   return GM_Ball;
    if (player->m_isDart)   return GM_Wave;
    if (player->m_isRobot)  return GM_Robot;
    if (player->m_isSpider) return GM_Spider;
    return GM_Cube;
}

void CalibrationService::start(int gamemodeIndex) {
    if (gamemodeIndex < 0 || gamemodeIndex >= GM_Count) return;
    active = true;
    calibratingMode = gamemodeIndex;
    repsDone = 0;
    samples.clear();
    samples.reserve(repsTarget);
    waitingForClick = false;
    timeSinceCue = 0.f;
    timeToNextCue = intervalSec;
}

void CalibrationService::cancel() {
    active = false;
    waitingForClick = false;
}

void CalibrationService::tick(float dt) {
    if (!active) return;

    if (!PlayLayer::get()) {
        cancel();
        return;
    }

    if (waitingForClick) {
        timeSinceCue += dt;
        if (timeSinceCue > kClickTimeoutSec) {
                        cancel();
        }
        return;
    }

    timeToNextCue -= dt;
    if (timeToNextCue <= 0.f) {
        auto* csm = ClickSoundManager::get();
        if (csm->enabled) {
            csm->playClick(true, false);
        }
        waitingForClick = true;
        timeSinceCue = 0.f;
    }
}

void CalibrationService::onRealClick() {
    if (!active || !waitingForClick) return;

    samples.push_back(timeSinceCue);
    waitingForClick = false;
    repsDone++;

    if (repsDone >= repsTarget) {
        finish();
    } else {
        timeToNextCue = intervalSec;
    }
}

void CalibrationService::finish() {
    if (!samples.empty()) {
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        double mean = sum / static_cast<double>(samples.size());

        double variance = 0.0;
        for (float s : samples) {
            double d = static_cast<double>(s) - mean;
            variance += d * d;
        }
        variance /= static_cast<double>(samples.size());

        auto& result = modes[calibratingMode];
        result.leadMs = static_cast<float>(mean * 1000.0);
        result.jitterMs = static_cast<float>(std::sqrt(variance) * 1000.0);
        result.sampleCount = static_cast<int>(samples.size());
        save();
    }
    active = false;
}

void CalibrationService::resetMode(int gamemodeIndex) {
    if (gamemodeIndex < 0 || gamemodeIndex >= GM_Count) return;
    modes[gamemodeIndex].leadMs = 0.f;
    modes[gamemodeIndex].jitterMs = 0.f;
    modes[gamemodeIndex].sampleCount = 0;
    save();
}

void CalibrationService::load() {
    auto* mod = Mod::get();
    for (int i = 0; i < GM_Count; ++i) {
        std::string prefix = "calib_" + std::string(gamemodeName(i));
        modes[i].leadMs = mod->getSavedValue<float>(prefix + "_lead", 0.f);
        modes[i].jitterMs = mod->getSavedValue<float>(prefix + "_jitter", 0.f);
        modes[i].sampleCount = mod->getSavedValue<int>(prefix + "_samples", 0);
        modes[i].guideEnabled = mod->getSavedValue<bool>(prefix + "_guide", true);
    }
}

void CalibrationService::save() const {
    auto* mod = Mod::get();
    for (int i = 0; i < GM_Count; ++i) {
        std::string prefix = "calib_" + std::string(gamemodeName(i));
        mod->setSavedValue(prefix + "_lead", modes[i].leadMs);
        mod->setSavedValue(prefix + "_jitter", modes[i].jitterMs);
        mod->setSavedValue(prefix + "_samples", modes[i].sampleCount);
        mod->setSavedValue(prefix + "_guide", modes[i].guideEnabled);
    }
}

class $modify(GB7CalibrationTickPL, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        CalibrationService::get().tick(dt);
    }
};
