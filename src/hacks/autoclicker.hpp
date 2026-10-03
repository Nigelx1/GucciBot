#pragma once

// Autoclicker: presses and releases jump on a rhythm counted in physics ticks.
//
// Ported 2026-10-02 from Silicate's src/assist/autoclicker.cpp (peony,
// GPL-3.0, the licence GucciBot is under too), replacing the stopgap that
// stood in after the ToastyReplay-derived one was removed. Per player, as in
// Silicate: on/off, hold and release lengths, extra clicks on each press tick
// and swift clicks. Absent's black orb UFO loop (from Absense, his Silicate
// port) is in as well, and "only while holding" is GucciBot's own.
//
// Nothing here touches the game's buttons. engine_updater.cpp calls
// processTick() once a tick, after the frame counter moves, and queues what
// it returns for the next tick; while recording those land in the macro like
// any other input. The clicker does nothing while a macro plays, while
// Calculate or a pathfinder owns the run, and outside a level (see
// waitingFor()).

#include <cstdint>
#include <vector>

namespace gucci {

    struct Autoclicker {
        static Autoclicker* get() {
            static Autoclicker s_instance;
            return &s_instance;
        }

        struct PlayerSettings {
            bool enabled = true;
            int holdTicks = 1;         // ticks the button stays down
            int releaseTicks = 1;      // ticks it stays up before the next click
            int clicksPerHold = 1;     // above 1: a release and a press more on the press tick, each
            bool swiftClicks = false;  // let go on the press tick (holdTicks goes unused)
            bool blackOrbUfo = false;  // the fixed five-tick loop instead of all of the above
        };

        bool enabled = false;
        // Click only while the player holds jump themselves.
        bool onlyWhileHolding = false;
        // Player 2's settings only apply in two-player levels: anywhere else
        // the game gives Player 2's input to Player 1, so its clicks would
        // land on Player 1 a second time.
        PlayerSettings p1{};
        PlayerSettings p2{};

        static constexpr int kMaxTicks = 100000;
        static constexpr int kMaxClicksPerHold = 100;

        struct TickResult {
            std::vector<bool> p1;  // in order; true = press, false = release
            std::vector<bool> p2;
        };

        TickResult processTick();
        // Every reset of the level (new attempt, respawn, step back, quit,
        // editor playtest): the rhythm starts over on the next tick.
        void reset();
        // The player's own jump input, for onlyWhileHolding.
        void trackUserInput(bool pressed, bool isPlayer2);
        // One-shot copy, not a link: change either side afterwards.
        void syncP2FromP1();

        // Whether the clicker has the button down right now.
        bool isClicking(bool player2) const;
        // What the clicker is waiting for while it is on ("a macro is
        // playing"), or nullptr when nothing stops it.
        const char* waitingFor() const;

        // The "autoclicker_*" saved values. Loaded once when the mod loads
        // (autoclicker.cpp); the menu card saves whenever it changes something.
        void loadSettings();
        void saveSettings() const;

    private:
        static constexpr uint64_t kNever = UINT64_MAX;

        // What the clicker has done on one player's button.
        struct Lane {
            uint64_t lastTick = kNever;   // tick of the last press or let-go decision
            uint64_t loopStart = kNever;  // tick the black orb loop's first tick fell on
            bool down = false;            // the clicker is holding the button
        };
        Lane m_lanes[2];
        bool m_userHolding[2] = {false, false};

        void runLane(int index, PlayerSettings const& cfg, bool applies, bool userHolds, bool busy,
                     uint64_t tick, std::vector<bool>& events);
        static void rhythmTick(Lane& lane, PlayerSettings const& cfg, uint64_t tick, std::vector<bool>& events);
        static void blackOrbTick(Lane& lane, uint64_t tick, std::vector<bool>& events);
    };

} // namespace gucci
