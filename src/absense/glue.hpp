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
    // The Pathfinder tab's "exact start flags" test switch (saved, off by
    // default): a search takes it when it starts and keeps it to the end (see
    // PathfinderSettings::exactStartFlags).
    bool exactStartFlags();
    void setExactStartFlags(bool on);
    // GucciBot (2026-10-23): the Pathfinder tab's "Human hints" switch (saved,
    // off by default). Off, a search uses no macro at all (Nigel: everything
    // has to work without macros); on, it may try a macro of this level as
    // ideas (absense::human::Reference).
    bool humanHints();
    void setHumanHints(bool on);
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
        bool exactStartFlags = false;  // the running search took exact start flags on (see exactStartFlags())
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
    // `watch`: objects to follow as the run has them, every `every` ticks -
    // a comma-separated list of uids, or "near:x,y,r" for every object within
    // r of (x, y) when the run starts - with the copies' touch triggers and
    // the World's fires along the way. Empty: neither.
    matjson::Value simulate(std::vector<ScriptTick> const& script, int ticks, int every,
                            std::string const& watch = {});

    // gucci_sim_vs_real: the same script from the same moment, once in the
    // simulation and once for real (absense/judge.hpp), and the first tick the
    // two part. The game is put back afterwards.
    matjson::Value simVsReal(std::vector<ScriptTick> const& script, int ticks, int every);

    // Step R0 of AI_PATHFINDER_PLAN.md: how fast the simulation runs from the
    // real player's state (simulated ticks a second, snapshot save, restore
    // and size), also written to the game's log. Nothing in the game changes.
    matjson::Value benchmark(int ticks, int snapshots);
    // GucciBot (2026-10-23): the speed test's timers on for every run until
    // switched off, and what they have gathered (gucci_abs_profile).
    void profileRuns(bool on);
    matjson::Value runProfile();

    // Step R1 of AI_PATHFINDER_PLAN.md: the route planner's map of the whole
    // level (absense/route/atlas.hpp), drawn over it when shown.
    bool levelMapShown();
    void showLevelMap(bool on);
    std::string levelMapSummary();  // empty outside a level
    // Its columns and gates between x `from` and `to`, for Assistant Access.
    matjson::Value levelMap(float from, float to);

    // Step R2: the planned route through the whole level on that map
    // (absense/route/planner.hpp), drawn over it when shown.
    bool routeShown();
    void showRoute(bool on);
    std::string routeSummary();  // empty outside a level
    // A fresh plan from (x, y), its best route's first `limit` waypoints.
    matjson::Value routePlan(float fromX, float fromY, int limit);

    // Step R3: the physics map with a goal, from the real player's state to
    // x `gateX` inside the band lo..hi (gateX <= 0: the next gate of the
    // planned route ahead). Found (with the way as a script), impossible, or
    // out of budget; drawn over the level. The game waits while it runs.
    matjson::Value routeLeg(float gateX, float lo, float hi, int maxTicks, int frontierCap, bool exact, double maxMs);

    // Pathfinder page > Follow the planned route (AI), saved with the mod: 0
    // off (the pathfinder as it was), 1 route ideas (step R4: each decision
    // also tries the physics map's way to the planned route's next gate), 2
    // route drives (docs/ROUTE_ENGINE_PLAN.md step 5: each decision aims at
    // the next gate and plays the way physics finds into it, to the gate).
    int routeGuidance();
    void setRouteGuidance(int mode);

    // Killers the simulation is wrong about (by object uid): its copies do
    // not die of them. See absense/judge.hpp.
    void distrust(int uid);
    void clearDistrust();
    std::vector<int> distrustedList();
}
