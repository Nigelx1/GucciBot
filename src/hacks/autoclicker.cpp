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
    loopStepP1 = -1;
    loopStepP2 = -1;
    currentlyHoldingP1 = false;
    currentlyHoldingP2 = false;
}

void Autoclicker::trackUserInput(bool pressed, bool isPlayer2) {
    if (isPlayer2)
        userHoldingP2 = pressed;
    else
        userHoldingP1 = pressed;
}

namespace {
    // Absense's black orb UFO loop (assist/autoclicker.cpp), per tick:
    //   1  tap, then push (hold starts)
    //   2  release, then tap
    //   3  tap, then push (hold starts)
    //   4  still held
    //   5  release, then tap
    // No pause between cycles; the hold is one tick on odd cycles and two on
    // even ones. Recorded at 720 TPS -- other TPS values change the rhythm.
    struct LoopTick {
        bool tapFirst;  // a tap (press + release) at the start of the tick
        bool push;      // a press that stays held at the end of the tick
        bool release;   // a release at the start of the tick (the hold ends)
        bool tapAfter;  // a tap after the release
    };
    constexpr LoopTick kBlackOrbLoop[] = {
        {true, true, false, false},
        {false, false, true, true},
        {true, true, false, false},
        {false, false, false, false},
        {false, false, true, true},
    };
    constexpr int kBlackOrbLoopLength = sizeof(kBlackOrbLoop) / sizeof(kBlackOrbLoop[0]);
} // namespace

Autoclicker::TickResult Autoclicker::processTick() {
    TickResult result;
    if (!enabled)
        return result;

    auto processPlayer = [&](PlayerSettings const& s,
                             bool userHolding,
                             int& counter,
                             bool& holding,
                             int& loopStep,
                             std::vector<bool>& events) {
        if (!s.enabled)
            return;

        if (onlyWhileHolding && !userHolding) {
            if (holding) {
                events.push_back(false);
                holding = false;
            }
            counter = 0;
            loopStep = -1;
            return;
        }

        if (s.blackOrbUfo) {
            if (loopStep < 0) {
                loopStep = 0;
                if (holding) {
                    events.push_back(false);
                    holding = false;
                }
            }
            LoopTick const& t = kBlackOrbLoop[loopStep];
            loopStep = (loopStep + 1) % kBlackOrbLoopLength;

            if (t.release && holding) {
                events.push_back(false);
                holding = false;
            }
            if (t.tapFirst || t.tapAfter) {
                if (holding) {
                    events.push_back(false);
                    holding = false;
                }
                events.push_back(true);
                events.push_back(false);
            }
            if (t.push) {
                events.push_back(true);
                holding = true;
            }
            return;
        }
        loopStep = -1;

        counter++;

        if (holding) {
            if (counter >= s.holdTicks) {
                events.push_back(false);
                holding = false;
                counter = 0;
            }
        } else if (counter >= s.releaseTicks) {
            events.push_back(true);
            // Extra clicks-per-hold (Silicate 1.1.0 parity): N-1 more full
            // release/press cycles on the press that starts a hold.
            for (int i = 1; i < std::max(1, s.clicksPerHold); i++) {
                events.push_back(false);
                events.push_back(true);
            }
            holding = true;
            counter = 0;
            if (s.swifts) {
                // Released on the same tick, as Silicate's performSwifts; the
                // next click comes releaseTicks later.
                events.push_back(false);
                holding = false;
            }
        }
    };

    processPlayer(p1, userHoldingP1, tickCounterP1, currentlyHoldingP1, loopStepP1, result.p1);
    processPlayer(p2, userHoldingP2, tickCounterP2, currentlyHoldingP2, loopStepP2, result.p2);

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
