// Bot's accessors for Absense's pathfinder stack (declared in
// analysis/ac/shim.hpp). Function-local statics, like the analyzer's, so the
// shim header needs no include of these classes.

#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"

#include <Geode/loader/Mod.hpp>

TrajectoryManager& Bot::trajectory() {
    static TrajectoryManager inst;
    return inst;
}

AbsensePathfinder& Bot::pathfinder() {
    static AbsensePathfinder inst;
    return inst;
}

AbsAutoclicker& Bot::autoclicker() {
    static AbsAutoclicker inst;
    return inst;
}

bool absense::classicSelected() {
    return geode::Mod::get()->getSavedValue<bool>("pf_engine_classic", false);
}

void absense::selectClassic(bool classic) {
    geode::Mod::get()->setSavedValue("pf_engine_classic", classic);
}

bool absense::startPathfinder() {
    auto* bot = Bot::get();
    // Absense makes its simulation of the level (the copies of the player
    // and the World) on every level load. Here it is made the first time the
    // pathfinder starts in a level, so a level played without it pays
    // nothing; PlayLayer::init and onQuit take it down again.
    if (!bot->trajectory().exists() && PlayLayer::get())
        bot->trajectory().init();
    return bot->pathfinder().start();
}

static const char* phaseName() {
    using Phase = AbsensePathfinder::Phase;
    switch (Bot::get()->pathfinder().phase()) {
    case Phase::Idle:
        return "idle";
    case Phase::Deciding:
        return "deciding";
    case Phase::Committing:
        return "playing";
    case Phase::Backtracking:
        return "going back";
    case Phase::Probing:
        return "probing";
    case Phase::Restoring:
        return "restoring";
    case Phase::Done:
        return "done";
    }
    return "?";
}

bool absense::isRunning() {
    return Bot::get()->pathfinder().isRunning();
}

void absense::stopPathfinder() {
    auto& pf = Bot::get()->pathfinder();
    if (pf.isRunning())
        pf.stop("stopped");
}

absense::Status absense::status() {
    auto& pf = Bot::get()->pathfinder();
    auto const& s = pf.stats();
    Status out;
    out.running = pf.isRunning();
    out.phase = phaseName();
    out.progress = s.progress;
    out.bestProgress = s.bestProgress;
    out.startTick = s.startTick;
    out.currentTick = s.currentTick;
    out.bestTick = s.bestTick;
    out.decisions = s.decisions;
    out.backtracks = s.backtracks;
    out.deadEnds = s.deadEnds;
    out.simulations = s.simulations;
    out.freezes = s.freezes;
    out.seconds = s.seconds;
    out.lastDecision = s.lastDecision;
    out.message = s.message;
    return out;
}
