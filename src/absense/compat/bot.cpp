// Bot's accessors for Absense's pathfinder stack (declared in
// analysis/ac/shim.hpp). Function-local statics, like the analyzer's, so the
// shim header needs no include of these classes.

#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"

#include "absense/world/world.hpp"

#include <Geode/loader/Mod.hpp>
#include <fmt/format.h>

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

matjson::Value absense::simulate(std::vector<ScriptTick> const& script, int ticks, int every) {
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto* bot = Bot::get();
    if (!pl || !pl->m_player1) {
        out["error"] = "not in a level";
        return out;
    }
    if (!bot->trajectory().exists())
        bot->trajectory().init();
    out["world_on"] = !world::World::disabled;
    out["world_off_reason"] = std::string(world::World::disabled && world::World::disabledReason
                                              ? world::World::disabledReason
                                              : "");
    auto* t = bot->trajectory().unsafeInner();
    if (!t) {
        out["error"] = "the simulation could not be made";
        return out;
    }

    // The script as the pathfinder would hand it over: a press on each tick
    // the button goes down, and whether it is down after the tick.
    auto& buttons = pl->m_player1->m_holdingButtons;
    auto const it = buttons.find(static_cast<int>(PlayerButton::Jump));
    bool const downNow = it != buttons.end() && it->second;
    std::vector<TickInput> inputs;
    inputs.reserve(script.size());
    bool down = downNow;
    for (auto const& s : script) {
        TickInput in;
        in.presses = (s.press || (s.held && !down)) ? 1 : 0;
        in.held = s.held || s.press;
        inputs.push_back(in);
        down = in.held;
    }
    if (inputs.empty())
        inputs.push_back(TickInput{0, downNow});

    t->clearKiller();
    std::vector<TraceSample> trace;
    auto const r = t->run(pl, true, inputs, ticks, downNow, &trace);

    out["start_frame"] = (int64_t)bot->updater().getFrame();
    out["button_down_at_start"] = downNow;
    out["survived"] = (int64_t)r.survived;
    out["died"] = r.died;
    out["complete"] = r.complete;
    out["end_x"] = (double)r.x;
    out["end_y"] = (double)r.y;
    out["min_y"] = (double)r.minY;
    out["max_y"] = (double)r.maxY;
    out["phantoms"] = (int64_t)bot->trajectory().phantomCount();

    auto const& k = t->lastKiller();
    auto killer = matjson::Value::object();
    killer["valid"] = k.valid;
    if (k.valid) {
        killer["tick"] = (int64_t)k.tick;
        killer["object_id"] = (int64_t)k.id;
        killer["uid"] = (int64_t)k.uid;
        killer["type"] = (int64_t)k.type;
        killer["x"] = (double)k.x;
        killer["y"] = (double)k.y;
        killer["rotation"] = (double)k.rot;
        killer["rect"] = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}",
                                     k.rect.origin.x, k.rect.origin.y, k.rect.size.width, k.rect.size.height);
        killer["player_rect"] = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}",
                                            k.playerRect.origin.x, k.playerRect.origin.y,
                                            k.playerRect.size.width, k.playerRect.size.height);
        killer["certainty"] = k.certainty == world::Certainty::Static      ? "static"
                              : k.certainty == world::Certainty::Modelled ? "modelled"
                                                                            : "uncertain";
        killer["movable"] = k.movable;
    }
    out["killer"] = killer;

    auto path = matjson::Value::array();
    int const step = std::max(1, every);
    for (size_t i = 0; i < trace.size(); i++) {
        auto const& s = trace[i];
        if (s.tick % step != 0 && i + 1 != trace.size() && !s.dead)
            continue;
        path.push(fmt::format("{} x={:.1f} y={:.1f} vy={:.2f}{}{}{}",
                              s.tick, s.x, s.y, s.yVel,
                              s.held ? " H" : "",
                              s.onGround ? " G" : "",
                              s.dead ? " DEAD" : ""));
    }
    out["path"] = path;
    return out;
}
