#pragma once

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
        };

        bool enabled = false;
        PlayerSettings p1{};
        PlayerSettings p2{};
        bool onlyWhileHolding = false;

        int tickCounterP1 = 0;
        int tickCounterP2 = 0;
        bool currentlyHoldingP1 = false;
        bool currentlyHoldingP2 = false;
        bool userHoldingP1 = false;
        bool userHoldingP2 = false;
        bool isAutoclickerInput = false;

        struct TickResult {
            bool p1Fire = false;
            bool p1Press = false;
            int p1Clicks = 1; // only meaningful when p1Fire && p1Press
            bool p2Fire = false;
            bool p2Press = false;
            int p2Clicks = 1;
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
