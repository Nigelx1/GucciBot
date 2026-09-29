#pragma once

// What the rest of GucciBot (the Pathfinder tab, the HUD, gucci_pathfinder)
// needs from Absense's pathfinder, without including its headers: they bring
// in its Trajectory, its Silicate names and its settings, some of which
// would collide with GucciBot's own in those files. Defined in
// absense/compat/bot.cpp.

#include <matjson.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace absense {
    // Which engine the Pathfinder tab and gucci_pathfinder use: Absense's,
    // unless the classic one is picked. Saved with the mod.
    bool classicSelected();
    void selectClassic(bool classic);

    // Starts it from the current tick. False when it cannot; status().message
    // says why.
    bool startPathfinder();

    // Nigel (2026-09-28): start it from the pause menu, and/or from the
    // level's beginning. The start is queued and carried out over the next
    // frames (servicePendingStart, from drawScene): the pause menu is closed
    // (GD's Resume) - or, from the beginning, GD's Full Restart is done - and
    // it starts once the level is running with the player alive. From the
    // beginning, the game is held at frame 0 so the search plays every tick.
    // False when there is no level to start in.
    bool requestStart(bool fromBeginning);
    bool startPending();
    void servicePendingStart();
    // The Pathfinder tab's "start from the beginning" switch (saved).
    bool startFromBeginning();
    void setStartFromBeginning(bool on);
    void stopPathfinder();
    bool isRunning();

    struct Status {
        bool running = false;
        const char* phase = "";     // in words: deciding, playing, going back...
        float progress = 0.0f;      // 0..1 of the level, where the game is
        float bestProgress = 0.0f;  // 0..1, the furthest it has got
        uint64_t startTick = 0;
        uint64_t currentTick = 0;
        uint64_t bestTick = 0;
        uint64_t decisions = 0;
        uint64_t backtracks = 0;
        uint64_t deadEnds = 0;
        uint64_t simulations = 0;
        uint64_t freezes = 0;       // times the game waited for a decision
        double seconds = 0.0;
        std::string lastDecision;
        std::string message;        // why it stopped, or a note while it runs
    };
    Status status();

    // Diagnostics (gucci_abs_simulate): run the pathfinder's simulation from
    // the real player's current state along `script`, one entry per tick, for
    // `ticks` ticks (the last entry repeats). Reports how far it got, the path
    // every `every` ticks, and what killed it. Changes nothing in the real
    // game.
    struct ScriptTick {
        bool press = false;  // a press on this tick (a release first if the button is down)
        bool held = false;   // the button is down after the tick
    };
    matjson::Value simulate(std::vector<ScriptTick> const& script, int ticks, int every);

    // gucci_sim_vs_real: the same script from the same moment, once in the
    // simulation and once for real (absense/judge.hpp), and the first tick the
    // two part. The game is put back afterwards.
    matjson::Value simVsReal(std::vector<ScriptTick> const& script, int ticks, int every);

    // Killers the simulation is wrong about (by object uid): its copies do
    // not die of them. See absense/judge.hpp.
    void distrust(int uid);
    void clearDistrust();
    std::vector<int> distrustedList();
}
