#pragma once

// The level's start for an AI run (stage A, 2026-11-02): the real game is
// restarted and held paused at its first tick (absense::servicePendingStart's
// pattern: the pause menu's restart, or fullReset, then one update a frame
// until the level is going), and the start is captured there, so the AI's
// session begins while the game still stands at that tick (the start takes
// the full World with its running actions). The run uses the live TPS; a
// macro made at another rate sets it for the run only (never saved) and it is
// put back with the paused state when the run lets go.

#include "ai/sim.hpp"

#include <memory>
#include <string>

namespace gucci::ai {

    class LevelStart {
    public:
        enum class Step { Waiting, Ready, Failed };

        // tps <= 0: the live rate.
        void request(double tps);
        // Once a frame until Ready (start() is then set) or Failed (why).
        Step service(std::string& why);
        std::shared_ptr<sim::Start> start() const { return m_start; }
        // Puts the rate and the paused state back (the game stays where it is).
        void release();
        bool requested() const { return m_requested; }

    private:
        bool m_requested = false;
        bool m_resetDone = false;
        bool m_wasPaused = false;
        double m_oldTps = 0.0;
        bool m_tpsSet = false;
        int m_frames = 0;
        std::shared_ptr<sim::Start> m_start;
    };

} // namespace gucci::ai
