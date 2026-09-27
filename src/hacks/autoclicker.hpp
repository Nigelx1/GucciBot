#pragma once

#include <vector>

namespace gucci {

    struct Autoclicker {
        static Autoclicker* get();

        // Ported from Silicate 1.1.0's autoclicker rework (git.puppy.lgbt/silicate/silicate,
        // src/assist/autoclicker.hpp): independent hold/release timing and clicks-per-hold
        // per player, instead of one shared pair applied to both. GucciBot's own
        // "Only While Holding" feature (not in Silicate) is preserved as-is.
        struct PlayerSettings {
            bool enabled = true;
            int holdTicks = 1;
            int releaseTicks = 1;
            int clicksPerHold = 1; // extra full press/release cycles fired the instant a hold starts
            // Silicate's swift clicks: each click is released on the tick it
            // was pressed, instead of held for holdTicks.
            bool swifts = false;
            // Absense's "Auto black orb UFO": a fixed five-tick loop recorded
            // from a straight black orb UFO spam at 720 TPS. Replaces the
            // timings above while on.
            bool blackOrbUfo = false;
        };

        bool enabled = false;
        PlayerSettings p1{};
        PlayerSettings p2{};
        bool onlyWhileHolding = false;

        int tickCounterP1 = 0;
        int tickCounterP2 = 0;
        // Position in the black orb loop; -1 starts it over (every attempt,
        // and whenever the loop is switched on).
        int loopStepP1 = -1;
        int loopStepP2 = -1;
        bool currentlyHoldingP1 = false;
        bool currentlyHoldingP2 = false;
        bool userHoldingP1 = false;
        bool userHoldingP2 = false;
        bool isAutoclickerInput = false;

        // This tick's button events for each player, in the order they are
        // queued: true = press, false = release. A list rather than one event,
        // because clicks-per-hold, swift clicks and the black orb loop all put
        // several on one tick.
        struct TickResult {
            std::vector<bool> p1;
            std::vector<bool> p2;
        };

        TickResult processTick();
        void reset();
        void trackUserInput(bool pressed, bool isPlayer2);
        // Copies p1's timing settings onto p2 -- a "Sync to P1" button in the GUI,
        // not automatic/continuous syncing (matches Silicate's own one-shot copy,
        // not a permanently-linked setting).
        void syncP2FromP1() { p2 = p1; }
    };

} // namespace gucci
