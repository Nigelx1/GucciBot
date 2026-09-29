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
}
