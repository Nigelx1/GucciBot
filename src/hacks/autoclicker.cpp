#include "hacks/autoclicker.hpp"

#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace gucci;

Autoclicker* Autoclicker::get() {
    static Autoclicker instance;
    return &instance;
}

void Autoclicker::reset() {
    tickCounterP1 = 0;
    tickCounterP2 = 0;
    currentlyHoldingP1 = false;
    currentlyHoldingP2 = false;
}

void Autoclicker::trackUserInput(bool pressed, bool isPlayer2) {
    if (isPlayer2)
        userHoldingP2 = pressed;
    else
        userHoldingP1 = pressed;
}

Autoclicker::TickResult Autoclicker::processTick() {
    TickResult result;
    if (!enabled)
        return result;

    auto processPlayer = [&](PlayerSettings const& s,
                             bool userHolding,
                             int& counter,
                             bool& holding,
                             bool& fire,
                             bool& press,
                             int& clicks) {
        if (!s.enabled)
            return;

        if (onlyWhileHolding && !userHolding) {
            if (holding) {
                fire = true;
                press = false;
                holding = false;
            }
            counter = 0;
            return;
        }

        counter++;

        if (holding) {
            if (counter >= s.holdTicks) {
                fire = true;
                press = false;
                holding = false;
                counter = 0;
            }
        } else {
            if (counter >= s.releaseTicks) {
                fire = true;
                press = true;
                holding = true;
                counter = 0;
                clicks = std::max(1, s.clicksPerHold);
            }
        }
    };

    processPlayer(p1,
                  userHoldingP1,
                  tickCounterP1,
                  currentlyHoldingP1,
                  result.p1Fire,
                  result.p1Press,
                  result.p1Clicks);
    processPlayer(p2,
                  userHoldingP2,
                  tickCounterP2,
                  currentlyHoldingP2,
                  result.p2Fire,
                  result.p2Press,
                  result.p2Clicks);

    return result;
}

class $modify(AutoclickerPlayLayer, PlayLayer) {
    void resetLevel() {
        Autoclicker::get()->reset();
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        Autoclicker::get()->reset();
        PlayLayer::resetLevelFromStart();
    }
};
