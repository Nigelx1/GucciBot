// Bot's accessors for Absense's pathfinder stack (declared in
// analysis/ac/shim.hpp). Function-local statics, like the analyzer's, so the
// shim header needs no include of these classes.

#include "core/platform.hpp"
#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/judge.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/route/atlas.hpp"
#include "absense/route/planner.hpp"

#include "absense/world/census.hpp"
#include "absense/world/world.hpp"

#include <Geode/loader/Mod.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>

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

namespace {
    struct PendingStart {
        bool active = false;
        bool fromBeginning = false;
        bool resetDone = false;
        bool wasPaused = false;  // GucciBot's own pause, before the request
        int frames = 0;          // frames waited for the level to be ready
    };
    PendingStart s_pending;
    std::string s_pendingNote;  // why a queued start gave up (status().message)

    PauseLayer* pauseMenu() {
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        return scene ? scene->getChildByType<PauseLayer>(0) : nullptr;
    }
}  // namespace

bool absense::requestStart(bool fromBeginning) {
#if !GB_NATIVE_ENGINE
    (void)fromBeginning;
    s_pendingNote = "The Pathfinder is Windows-only for now: its model of the level reads GD's Windows memory layout.";
    return false;
#endif
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) {
        s_pendingNote = "Open a level first.";
        return false;
    }
    if (Bot::get()->pathfinder().isRunning())
        return true;
    s_pending = PendingStart{};
    s_pending.active = true;
    s_pending.fromBeginning = fromBeginning;
    s_pending.wasPaused = gucci::GucciEngine::get()->updater.m_paused;
    s_pendingNote.clear();
    return true;
}

bool absense::startPending() {
    return s_pending.active;
}

void absense::servicePendingStart() {
    if (!s_pending.active)
        return;
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_player1) {
        s_pending.active = false;
        s_pendingNote = "The level was left before the pathfinder could start.";
        return;
    }
    auto& upd = gucci::GucciEngine::get()->updater;

    // The pause menu first: GD's own buttons, so the game resumes (or
    // restarts) exactly as when they are pressed.
    if (auto* menu = pauseMenu()) {
        if (s_pending.fromBeginning && !s_pending.resetDone) {
            menu->onRestartFull(nullptr);
            s_pending.resetDone = true;
            upd.setPaused(true);  // held at frame 0 until the search takes it
        } else {
            menu->onResume(nullptr);
        }
        return;  // the next frame, with the menu gone
    }
    if (s_pending.fromBeginning && !s_pending.resetDone) {
        pl->fullReset();
        s_pending.resetDone = true;
        upd.setPaused(true);
        return;
    }

    // Ready: the level running, the player alive, not finished. While the
    // game is held paused - at frame 0 after the restart, or paused by the
    // user - it gets one update a frame until then: the level's start, GD's
    // resume timer and a respawn all only move on when the game updates
    // (and none of them moves the player).
    const bool ready = pl->m_started && !pl->m_isPaused && pl->m_resumeTimer <= 0 &&
                       !pl->m_player1->m_isDead && !pl->m_hasCompletedLevel;
    if (!ready) {
        if (++s_pending.frames > 600) {
            s_pending.active = false;
            s_pendingNote = pl->m_hasCompletedLevel
                                ? "The level is already complete (turn on \"start from the beginning\")."
                                : "The level did not get going within ten seconds; the start was given up.";
        } else if (upd.m_paused) {
            upd.stepOnce();
        }
        return;
    }
    s_pending.active = false;
    if (absense::startPathfinder()) {
        if (s_pending.resetDone)
            Bot::get()->pathfinder().setRestorePaused(s_pending.wasPaused);
    } else if (s_pending.resetDone) {
        upd.setPaused(s_pending.wasPaused);
    }
}

bool absense::startFromBeginning() {
    return geode::Mod::get()->getSavedValue<bool>("pf_start_from_zero", false);
}

void absense::setStartFromBeginning(bool on) {
    geode::Mod::get()->setSavedValue("pf_start_from_zero", on);
}

// GucciBot (2026-11-01): Exact start flags, on by default since build -c.
// The A/B on build -b, truly cold: The Nightmare passes only with them on;
// ToE2, Clubstep and Deadlocked pass with them on; Bloodbath parted from the
// game 142 times without them and once with them. Saved under a new key: the
// old one (pf_exact_start_flags) holds the off the test switch was saved
// with, and nothing reads it now.
static constexpr const char* kExactStartFlagsKey = "pf_exact_start_flags2";

bool absense::exactStartFlags() {
    // A missing key is on. Read without getSavedValue's default, which would
    // write the key into the save on a mere look.
    auto const& saved = geode::Mod::get()->getSaveContainerConst();
    return !saved.contains(kExactStartFlagsKey) || saved[kExactStartFlagsKey].asBool().unwrapOr(true);
}

void absense::setExactStartFlags(bool on) {
    // Written outright: Geode's setSavedValue writes nothing when the value
    // equals what the key reads as, and to it a missing key reads as off - so
    // switching this off before it was ever saved would not stick.
    geode::Mod::get()->getSaveContainer()[kExactStartFlagsKey] = on;
}

bool absense::rememberSolutions() {
    return geode::Mod::get()->getSavedValue<bool>("pf_remember_solutions", false);
}

void absense::setRememberSolutions(bool on) {
    geode::Mod::get()->setSavedValue("pf_remember_solutions", on);
}

bool absense::tryOtherRoutes() {
    return geode::Mod::get()->getSavedValue<bool>("pf_try_other_routes", false);
}

void absense::setTryOtherRoutes(bool on) {
    geode::Mod::get()->setSavedValue("pf_try_other_routes", on);
}


void absense::stopPathfinder() {
    if (s_pending.active) {
        s_pending.active = false;
        s_pendingNote = "stopped";
    }
    auto& pf = Bot::get()->pathfinder();
    if (pf.isRunning())
        pf.stop("stopped");
}

absense::Status absense::status() {
    auto& pf = Bot::get()->pathfinder();
    auto const& s = pf.stats();
    Status out;
    out.running = pf.isRunning();
    out.phase = s_pending.active ? "starting" : phaseName();
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
    out.exactStartFlags = out.running && SLSettings::get()->pathfinder.exactStartFlags;
    if (s_pending.active)
        out.message = s_pending.fromBeginning ? "Restarting the level to start from its beginning..." : "Starting...";
    else if (!out.running && !s_pendingNote.empty())
        out.message = s_pendingNote;
    return out;
}

namespace {
    // The script as the pathfinder would hand it over: a press on each tick
    // the button goes down (or is pressed again), and whether it is down
    // after the tick.
    // GucciBot (2026-10-24): a press with the button up after the tick is a
    // tap inside the tick (the script's t<N>): {1, false}, which Sim::step and
    // the real game (judge::run) both play as a press and a release before the
    // tick's physics. A press used to leave the button down whatever the step.
    std::vector<TickInput> scriptInputs(std::vector<absense::ScriptTick> const& script, bool downNow) {
        std::vector<TickInput> inputs;
        inputs.reserve(script.size());
        bool down = downNow;
        for (auto const& s : script) {
            TickInput in;
            in.presses = (s.press || (s.held && !down)) ? 1 : 0;
            in.held = s.held;
            inputs.push_back(in);
            down = in.held;
        }
        if (inputs.empty())
            inputs.push_back(TickInput{0, downNow});
        return inputs;
    }

    // The kind's name in Trajectory::FakeMode numbering ("?" outside 0-7),
    // in the words of the step3-brief rulebook's model::modeName - the
    // rulebook itself isn't on master.
    const char* kindName(int mode) {
        static constexpr const char* kKinds[] = {"cube", "ship", "ball", "UFO", "wave", "robot", "spider", "swing"};
        return mode >= 0 && mode < 8 ? kKinds[mode] : "?";
    }

    // GucciBot (2026-10-24, step 3): the kind, size, speed, gravity and the
    // object fired on the tick (TraceSample's newer fields), for the lines below.
    std::string sampleState(TraceSample const& s) {
        return fmt::format(" {}{} s{:.2f}{}{}", kindName(s.mode), s.mini ? " mini" : "", s.speed,
                           s.upsideDown ? " UD" : "", s.fired != 0 ? fmt::format(" fired {}", s.fired) : std::string());
    }

    std::string sampleText(TraceSample const& s) {
        return fmt::format("x={:.3f} y={:.3f} vy={:.3f}{}{}{}{}", s.x, s.y, s.yVel, s.held ? " H" : "",
                           s.onGround ? " G" : "", s.dead ? " DEAD" : "", sampleState(s));
    }
}  // namespace

matjson::Value absense::simulate(std::vector<ScriptTick> const& script, int ticks, int every,
                                 std::string const& watch) {
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
    auto const inputs = scriptInputs(script, downNow);

    // The objects to follow, picked before the run moves anything.
    std::vector<GameObject*> watched;
    if (!watch.empty() && pl->m_objects) {
        float nx = 0.0f, ny = 0.0f, nr = -1.0f;
        std::vector<int> uids;
        if (watch.rfind("near:", 0) == 0) {
            if (std::sscanf(watch.c_str() + 5, "%f,%f,%f", &nx, &ny, &nr) != 3) nr = -1.0f;
        } else {
            for (auto const& part : geode::utils::string::split(watch, ",")) {
                if (auto v = geode::utils::numFromString<int>(geode::utils::string::trim(part))) uids.push_back(*v);
            }
        }
        for (auto* o : geode::cocos::CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!o) continue;
            if (nr >= 0.0f) {
                if (std::hypot(o->getPositionX() - nx, o->getPositionY() - ny) <= nr) watched.push_back(o);
            } else if (std::find(uids.begin(), uids.end(), o->m_uniqueID) != uids.end()) {
                watched.push_back(o);
            }
            if (watched.size() >= 64) break;
        }
    }
    std::vector<std::string> notes;
    auto events = matjson::Value::array();
    auto watchOut = matjson::Value::array();
    int const watchEvery = std::max(1, every);
    std::string lastNote;
    auto describe = [](int tick, GameObject* o) {
        auto const rect = o->getObjectRect();
        return fmt::format("{} uid {} id {} at {:.1f},{:.1f} rect {:.1f},{:.1f} {:.1f}x{:.1f}{}{}", tick, o->m_uniqueID,
                           o->m_objectID, o->getPositionX(), o->getPositionY(), rect.origin.x, rect.origin.y,
                           rect.size.width, rect.size.height, o->m_isGroupDisabled ? " group-off" : "",
                           o->m_isDisabled ? " off" : "");
    };
    if (!watch.empty()) {
        for (auto* o : watched) watchOut.push(describe(0, o));
        world::g_debugGroups.clear();
        world::g_debugUids.clear();
        for (auto* o : watched) {
            world::g_debugUids.push_back(o->m_uniqueID);
            if (o->m_groups)
                for (int g = 0; g < o->m_groupCount && g < 10; g++) world::g_debugGroups.push_back((*o->m_groups)[(size_t)g]);
        }
        world::g_debugNotes = &notes;
        t->m_traceWatch = [&](int tick) {
            for (auto const& n : notes) {
                if (n == lastNote) continue;  // a contact repeats every tick
                lastNote = n;
                if (events.size() < 600) events.push(fmt::format("{} {}", tick, n));
            }
            notes.clear();
            if (tick % watchEvery == 0)
                for (auto* o : watched) watchOut.push(describe(tick, o));
        };
    }

    t->clearKiller();
    std::vector<TraceSample> trace;
    auto const r = t->run(pl, true, inputs, ticks, downNow, &trace);
    t->m_traceWatch = nullptr;
    world::g_debugNotes = nullptr;
    if (!watch.empty()) {
        out["watched"] = watchOut;
        out["events"] = events;
    }

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
        // (A tick that fired something is always listed.)
        if (s.tick % step != 0 && i + 1 != trace.size() && !s.dead && s.fired == 0)
            continue;
        path.push(fmt::format("{} x={:.1f} y={:.1f} vy={:.2f}{}{}{}{}",
                              s.tick, s.x, s.y, s.yVel,
                              s.held ? " H" : "",
                              s.onGround ? " G" : "",
                              s.dead ? " DEAD" : "", sampleState(s)));
    }
    out["path"] = path;
    return out;
}

matjson::Value absense::benchmark(int ticks, int snapshots) {
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto* bot = Bot::get();
    if (!pl || !pl->m_player1) {
        out["error"] = "not in a level";
        return out;
    }
    if (!bot->trajectory().exists())
        bot->trajectory().init();
    auto* t = bot->trajectory().unsafeInner();
    if (!t) {
        out["error"] = "the simulation could not be made";
        return out;
    }
    auto const b = t->benchmark(pl, ticks, snapshots);
    if (b.error) {
        out["error"] = std::string(b.error);
        return out;
    }
    double const perSecond = b.usPerTick > 0.0 ? 1e6 / b.usPerTick : 0.0;
    out["frame"] = (int64_t)bot->updater().getFrame();
    out["world_on"] = b.worldOn;
    out["runs"] = (int64_t)b.runs;
    out["simulated_ticks"] = (int64_t)b.ticks;
    out["us_per_tick"] = b.usPerTick;
    out["ticks_per_second"] = perSecond;
    out["us_per_run_setup"] = b.usPerRun;
    out["snapshots"] = (int64_t)b.snapshots;
    out["us_per_snapshot_save"] = b.usPerSave;
    out["us_per_snapshot_restore"] = b.usPerRestore;
    out["snapshot_bytes_roughly"] = (int64_t)b.snapshotBytes;
    out["branches"] = (int64_t)b.branches;
    out["us_per_branch"] = b.usPerBranch;
    out["written_before_branches"] = (int64_t)b.written;
    out["groups_reached"] = (int64_t)b.reach;
    for (int k = 0; k < 2; k++) {
        auto p = matjson::Value::object();
        p["world"] = b.usWorld[k];
        p["movers"] = b.usMovers[k];
        p["near"] = b.usNear[k];
        p["iterate"] = b.usIterate[k];
        p["after"] = b.usAfter[k];
        out[k == 0 ? "us_a_tick_plain" : "us_a_tick_branch"] = p;
    }
    {
        static constexpr const char* kSetup[11] = {"scan",  "state", "void",  "movers",  "copies", "capture",
                                                   "def",   "table", "carry", "objects", "rest"};
        static constexpr const char* kEnd[4] = {"copies", "objects", "movers", "state"};
        auto setup = matjson::Value::object();
        for (int k = 0; k < 11; k++) setup[kSetup[k]] = b.usSetup[k];
        out["us_a_run_setup"] = setup;
        auto end = matjson::Value::object();
        for (int k = 0; k < 4; k++) end[kEnd[k]] = b.usEnd[k];
        out["us_a_run_end"] = end;
        std::string parts;
        for (int k = 0; k < 11; k++) parts += fmt::format("{}{} {:.0f}", k ? ", " : "", kSetup[k], b.usSetup[k]);
        parts += "; at the end";
        for (int k = 0; k < 4; k++) parts += fmt::format("{}{} {:.0f}", k ? ", " : " ", kEnd[k], b.usEnd[k]);
        geode::log::info("[AbsPF] speed test: setting a run up, in us: {}", parts);
        static constexpr const char* kCapture[12] = {"contacts", "values",  "events",    "def",
                                                     "tier_de",  "commands", "spawns",   "toggles_items",
                                                     "timers",   "listeners", "sequences", "uncertain"};
        auto capture = matjson::Value::object();
        std::string captureParts;
        for (int k = 0; k < 12; k++) {
            capture[kCapture[k]] = b.usCapture[k];
            captureParts += fmt::format("{}{} {:.0f}", k ? ", " : "", kCapture[k], b.usCapture[k]);
        }
        out["us_a_capture"] = capture;
        static constexpr const char* kNear[5] = {"pending", "dirty_buckets", "written", "reach", "carried"};
        static constexpr const char* kNearCounts[6] = {"looks", "written_looked_at", "groups", "members", "refreshes",
                                                       "writes"};
        for (int k = 0; k < 2; k++) {
            auto nearOut = matjson::Value::object();  // (near is a Windows macro)
            for (int p = 0; p < 5; p++) nearOut[kNear[p]] = b.usNearParts[k][p];
            for (int p = 0; p < 6; p++) nearOut[kNearCounts[p]] = b.nearCounts[k][p];
            out[k == 0 ? "near_a_tick_plain" : "near_a_tick_branch"] = nearOut;
        }
        geode::log::info("[AbsPF] speed test: the World's capture, in us: {}", captureParts);
    }
    geode::log::info("[AbsPF] speed test at frame {}: {:.0f} simulated ticks a second ({:.2f} us a tick over {} ticks "
                     "in {} runs), {:.0f} us to set a run up, a snapshot {:.1f} us to take and {:.1f} us to put back, "
                     "about {} bytes, World {}",
                     bot->updater().getFrame(), perSecond, b.usPerTick, b.ticks, b.runs, b.usPerRun, b.usPerSave,
                     b.usPerRestore, b.snapshotBytes, b.worldOn ? "on" : "off");
    geode::log::info("[AbsPF] speed test: a branch (put back, one tick) {:.1f} us with {} objects written and {} groups "
                     "reached; a tick's parts in us, plain / branch: World {:.1f} / {:.1f}, movers {:.1f} / {:.1f}, "
                     "near the copies {:.1f} / {:.1f}, the copies {:.1f} / {:.1f}, the rest {:.1f} / {:.1f}",
                     b.usPerBranch, b.written, b.reach, b.usWorld[0], b.usWorld[1], b.usMovers[0], b.usMovers[1],
                     b.usNear[0], b.usNear[1], b.usIterate[0], b.usIterate[1], b.usAfter[0], b.usAfter[1]);
    return out;
}

void absense::profileRuns(bool on) {
    Trajectory::profileRuns(on);
}

namespace {
// GucciBot (2026-11-12): on by default. A decision made ahead of a plan's end
// is judged from a start the copy carried forward itself, and on Slaughterhouse
// and GRIEF those plans walked into deaths the copy did not see (the real and
// simulated player stood on the same spot; from the real game the copy saw
// the same death). With every decision made from the real game, GRIEF's wave
// part had no missed death (17 before), Slaughterhouse went from 59% to 100%
// cold, and Bloodbath and The Nightmare still pass. pf_no_ahead false puts
// the old way back for a test.
bool g_noAhead = true;
}

bool absense::noAhead() {
    return g_noAhead;
}

void absense::setNoAhead(bool on) {
    g_noAhead = on;
}

bool absense::pendingVerify() {
    return world::g_pendingVerify;
}

void absense::setPendingVerify(bool on) {
    world::g_pendingVerify = on;
}

bool absense::pendingCells() {
    return world::g_pendingCells;
}

void absense::setPendingCells(bool on) {
    world::g_pendingCells = on;
}

matjson::Value absense::runProfile() {
    const Trajectory::RunProfile r = Trajectory::runProfile();
    const world::NearProfile& np = world::g_nearProfile;
    const world::CaptureProfile& cp = world::g_captureProfile;
    auto out = matjson::Value::object();
    const double steps = (double)std::max<uint64_t>(1, r.steps);
    const double begins = (double)std::max<uint64_t>(1, r.begins);
    const double ends = (double)std::max<uint64_t>(1, r.ends);
    out["on"] = np.on;
    out["simulated_ticks"] = (int64_t)r.steps;
    // In all, seconds: what the ticks, the runs' setting up and putting
    // away, and the snapshots took.
    double ticksUs = 0.0;
    for (double v : r.step) ticksUs += v;
    double setupUs = 0.0;
    for (double v : r.setup) setupUs += v;
    double endUs = 0.0;
    for (double v : r.end) endUs += v;
    auto total = matjson::Value::object();
    total["ticks_s"] = ticksUs / 1e6;
    total["run_setup_s"] = setupUs / 1e6;
    total["run_end_s"] = endUs / 1e6;
    total["snapshots_s"] = r.snapshotUs / 1e6;
    total["restores_s"] = r.restoreUs / 1e6;
    out["total"] = total;
    static constexpr const char* kStep[5] = {"world", "movers", "near", "iterate", "after"};
    auto tick = matjson::Value::object();
    for (int k = 0; k < 5; k++) tick[kStep[k]] = r.step[k] / steps;
    out["us_a_tick"] = tick;
    static constexpr const char* kNear[5] = {"pending", "dirty_buckets", "written", "reach", "carried"};
    auto nearOut = matjson::Value::object();  // (near is a Windows macro)
    for (int k = 0; k < 5; k++) nearOut[kNear[k]] = np.parts[k] / steps;
    nearOut["looks"] = (double)np.looks / steps;
    nearOut["written_looked_at"] = (double)np.written / steps;
    nearOut["groups"] = (double)np.groups / steps;
    nearOut["members"] = (double)np.members / steps;
    nearOut["refreshes"] = (double)np.refreshes / steps;
    nearOut["writes"] = (double)np.writes / steps;
    out["near_a_tick"] = nearOut;
    static constexpr const char* kSetup[11] = {"scan",  "state", "void",  "movers",  "copies", "capture",
                                               "def",   "table", "carry", "objects", "rest"};
    static constexpr const char* kEnd[4] = {"copies", "objects", "movers", "state"};
    auto runs = matjson::Value::object();
    runs["begun"] = (int64_t)r.begins;
    for (int k = 0; k < 11; k++) runs[std::string("setup_") + kSetup[k]] = r.setup[k] / begins;
    for (int k = 0; k < 4; k++) runs[std::string("end_") + kEnd[k]] = r.end[k] / ends;
    out["us_a_run"] = runs;
    auto snaps = matjson::Value::object();
    snaps["taken"] = (int64_t)r.snapshots;
    snaps["put_back"] = (int64_t)r.restores;
    snaps["us_to_take"] = r.snapshots ? r.snapshotUs / (double)r.snapshots : 0.0;
    snaps["us_to_put_back"] = r.restores ? r.restoreUs / (double)r.restores : 0.0;
    out["snapshots"] = snaps;
    static constexpr const char* kCapture[12] = {"contacts", "values",   "events",    "def",       "tier_de",  "commands",
                                                 "spawns",   "toggles_items", "timers", "listeners", "sequences", "uncertain"};
    auto capture = matjson::Value::object();
    capture["calls"] = (int64_t)cp.calls;
    const double calls = (double)std::max<uint64_t>(1, cp.calls);
    for (int k = 0; k < 12; k++) capture[kCapture[k]] = cp.parts[k] / calls;
    out["us_a_capture"] = capture;
    return out;
}

namespace {
    matjson::Value rangeJson(bool have, float lo, float hi) {
        auto a = matjson::Value::array();
        if (have) {
            a.push((double)lo);
            a.push((double)hi);
        }
        return a;
    }

    matjson::Value classesJson(world::CensusClasses const& c) {
        auto o = matjson::Value::object();
        o["collidable"] = (int64_t)c.collidable;
        o["hazards"] = (int64_t)c.hazards;
        o["solids"] = (int64_t)c.solids;
        o["specials"] = (int64_t)c.specials;
        o["modifiers"] = (int64_t)c.modifiers;
        o["extended"] = (int64_t)c.extended;
        return o;
    }

    const char* tierName(world::Tier t) {
        return t == world::Tier::A ? "A" : t == world::Tier::B ? "B" : "C";
    }
}  // namespace

matjson::Value absense::levelCensus(int realTicks, float binWidth, int top) {
    using Clock = std::chrono::steady_clock;
    auto const started = Clock::now();
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto* bot = Bot::get();
    if (!pl || !pl->m_player1) {
        out["error"] = "not in a level";
        return out;
    }
    top = std::max(1, top);
    // The World checks its offsets against the level when the simulation is
    // made (the first thing that simulates in a level makes it), and the
    // level's first read runs the check of what was read: either can turn the
    // World off, and then nothing here reads the level through it.
    if (!bot->trajectory().exists())
        bot->trajectory().init();
    std::shared_ptr<const world::WorldDef> def;
    if (!world::World::disabled)
        def = world::WorldDef::get(pl);
    bool const on = def && !world::World::disabled;
    std::string const offReason = on ? std::string()
                                     : std::string(world::World::disabled && world::World::disabledReason
                                                       ? world::World::disabledReason
                                                       : "the level is still loading");
    world::Census c;
    if (on)
        c = world::takeCensus(pl, def, binWidth);

    std::string const name = pl->m_level ? std::string(pl->m_level->m_levelName) : std::string();
    int64_t const id = pl->m_level ? (int64_t)pl->m_level->m_levelID.value() : 0;
    int64_t const objects = pl->m_objects ? (int64_t)pl->m_objects->count() : 0;
    {
        auto level = matjson::Value::object();
        level["name"] = name;
        level["id"] = id;
        level["objects"] = objects;
        level["length_x"] = (double)gucci::GucciEngine::get()->m_levelLength;
        level["platformer"] = pl->m_isPlatformer;
        level["two_player"] = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
        level["dual_now"] = pl->m_gameState.m_isDualMode;
        level["practice"] = pl->m_isPracticeMode;
        if (on) {
            level["y_collidable"] = rangeJson(c.haveCollidable, c.yCollidable[0], c.yCollidable[1]);
            level["y_all"] = rangeJson(c.haveAll, c.yAll[0], c.yAll[1]);
        }
        out["level"] = level;
    }
    {
        auto w = matjson::Value::object();
        w["on"] = on;
        w["off_reason"] = offReason;
        if (on) {
            w["triggers"] = (int64_t)c.triggers;
            w["spawn_channels"] = (int64_t)c.spawnChannels;
            w["stray_members"] = (int64_t)c.strayMembers;
            w["section_factor_x"] = (double)c.factorX;
            w["section_factor_y"] = (double)c.factorY;
            w["columns"] = (int64_t)c.columns;
            w["rows_max"] = (int64_t)c.rowsMax;
            w["writable"] = (int64_t)c.writable;
            w["writable_extended"] = (int64_t)c.writableExtended;
            w["pending_path"] = std::string(c.pendingPath);
            w["tier_de_checked"] = c.tierDEChecked;
            w["targets_objects_by_key"] = c.targetsObjectsByKey;
        }
        out["world"] = w;
    }
    if (on) {
        auto kinds = matjson::Value::array();
        for (size_t i = 0; i < c.kinds.size() && (int)i < top; i++) {
            auto const& k = c.kinds[i];
            auto e = matjson::Value::object();
            e["kind"] = std::string(world::kindName(k.kind));
            e["ids"] = k.ids;
            e["shipped"] = k.shipped;
            e["tier"] = std::string(tierName(k.tier));
            e["count"] = (int64_t)k.count;
            auto fires = matjson::Value::object();
            fires["walk"] = (int64_t)k.walk;
            fires["spawn"] = (int64_t)k.spawn;
            fires["touch"] = (int64_t)k.touch;
            e["fires"] = fires;
            e["x"] = rangeJson(true, k.xLo, k.xHi);
            e["groups"] = (int64_t)k.groups;
            e["reach"] = classesJson(k.reach);
            e["reach_x"] = rangeJson(k.reach.collidable > 0, k.reachLo, k.reachHi);
            kinds.push(e);
        }
        out["kinds"] = kinds;
        out["kinds_present"] = (int64_t)c.kinds.size();

        auto um = classesJson(c.unmodelled);
        um["groups"] = (int64_t)c.unmodelledGroups;
        um["reaches_any_object"] = (int64_t)c.reachesAnyObject;
        um["bin"] = (double)binWidth;
        auto byX = matjson::Value::array();
        for (auto const& b : c.unmodelledByX) {
            auto e = matjson::Value::object();
            e["x"] = (double)b.x;
            e["hazards"] = (int64_t)b.hazards;
            e["solids"] = (int64_t)b.solids;
            e["specials"] = (int64_t)b.specials;
            byX.push(e);
        }
        um["by_x"] = byX;
        out["unmodelled"] = um;

        auto rd = matjson::Value::object();
        rd["move_lock_player"] = (int64_t)c.moveLockPlayer;
        rd["move_lock_camera"] = (int64_t)c.moveLockCamera;
        rd["dynamic"] = (int64_t)c.dynamicMoves;
        rd["dynamic_rotates"] = (int64_t)c.dynamicRotates;
        rd["follow_player_y"] = (int64_t)c.followPlayerY;
        rd["touch"] = (int64_t)c.touch;
        rd["toggle_orbs"] = (int64_t)c.toggleOrbs;
        rd["toggle_blocks"] = (int64_t)c.toggleBlocks;
        rd["collision"] = (int64_t)c.collision;
        rd["collision_blocks"] = (int64_t)c.collisionBlocks;
        rd["items"] = (int64_t)c.items;
        rd["reset"] = (int64_t)c.reset;
        rd["random"] = (int64_t)c.random;
        auto randomX = matjson::Value::array();
        for (float x : c.randomX) randomX.push((double)x);
        rd["random_x"] = randomX;
        out["route_dependent"] = rd;

        auto sp = matjson::Value::object();
        sp["dual"] = (int64_t)c.dual;
        sp["solo"] = (int64_t)c.solo;
        sp["teleports"] = (int64_t)c.teleports;
        sp["mirrors"] = (int64_t)c.mirrors;
        sp["gravity_portals"] = (int64_t)c.gravityPortals;
        sp["rotate_gameplay"] = (int64_t)c.rotateGameplay;
        sp["reverse"] = (int64_t)c.reverse;
        sp["gravity_triggers"] = (int64_t)c.gravityTriggers;
        sp["speed_portals"] = (int64_t)c.speedPortals;
        auto cam = matjson::Value::object();
        cam["zoom"] = (int64_t)c.cameraZoom;
        cam["static"] = (int64_t)c.cameraStatic;
        cam["offset"] = (int64_t)c.cameraOffset;
        cam["edge"] = (int64_t)c.cameraEdge;
        cam["mode"] = (int64_t)c.cameraMode;
        cam["gameplay_offset"] = (int64_t)c.gameplayOffset;
        sp["camera"] = cam;
        out["special_objects"] = sp;

        auto live = matjson::Value::object();
        live["frame"] = (int64_t)bot->updater().getFrame();
        auto cmds = matjson::Value::object();
        static constexpr const char* kCommand[6] = {"move", "rotate", "follow", "follow_player_y", "transform",
                                                    "keyframe"};
        for (int k = 0; k < 6; k++) cmds[kCommand[k]] = (int64_t)c.commands[k];
        live["commands"] = cmds;
        live["spawns"] = (int64_t)c.spawns;
        live["timers"] = (int64_t)c.timers;
        auto area = matjson::Value::object();
        area["move"] = (int64_t)c.areaMove;
        area["rotate"] = (int64_t)c.areaRotate;
        area["scale"] = (int64_t)c.areaScale;
        live["area_effects"] = area;
        auto dyn = matjson::Value::object();
        dyn["move"] = (int64_t)c.dynamicMoveNow;
        dyn["rotate"] = (int64_t)c.dynamicRotateNow;
        live["dynamic"] = dyn;
        live["advanced_follows"] = (int64_t)c.advancedFollows;
        live["area_objects_to_reset"] = (int64_t)c.areaObjectsToReset;
        live["uncertain_groups"] = (int64_t)c.uncertainGroups;
        live["uncertain_any"] = c.uncertainAny;
        out["live_now"] = live;
    }

    // The real game's cost per tick: no input (the button up), deaths noted
    // and carried on through, the game put back. After everything above, so
    // what was read is the game as the caller found it. Capped at 6 s: the
    // server gives a tool 10 (mcp_server.cpp).
    std::string realText = "not sampled";
    {
        auto real = matjson::Value::object();
        if (realTicks <= 0) {
            real["skipped"] = "real_ticks is 0";
        } else {
            std::vector<float> us;
            us.reserve((size_t)realTicks);
            judge::Options opt;
            opt.throughDeaths = true;
            opt.tickUs = &us;
            opt.maxMs = 6000.0;
            std::vector<TickInput> const inputs{TickInput{0, false}};
            auto const r = judge::run(inputs, realTicks, nullptr, opt);
            if (!r.ran) {
                real["error"] = r.error;
                realText = "not sampled (" + r.error + ")";
            } else {
                std::vector<float> sorted = us;
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (float v : sorted) sum += v;
                size_t const m = sorted.size();
                double const mean = m ? sum / (double)m : 0.0;
                auto tick = matjson::Value::object();
                tick["mean"] = mean;
                tick["p50"] = m ? (double)sorted[m / 2] : 0.0;
                tick["p95"] = m ? (double)sorted[std::min(m - 1, m * 95 / 100)] : 0.0;
                tick["max"] = m ? (double)sorted.back() : 0.0;
                real["ticks"] = (int64_t)m;
                real["us_a_tick"] = tick;
                real["deaths_noted"] = (int64_t)r.deaths.size();
                real["stopped_by_clock"] = r.stoppedByClock;
                real["complete"] = r.complete;
                auto deaths = matjson::Value::array();
                for (size_t i = 0; i < r.deaths.size() && (int)i < top; i++) {
                    auto const& d = r.deaths[i];
                    auto e = matjson::Value::object();
                    e["tick"] = (int64_t)d.tick;
                    e["uid"] = (int64_t)d.uid;
                    e["id"] = (int64_t)d.id;
                    e["type"] = (int64_t)d.type;
                    e["x"] = (double)d.x;
                    e["y"] = (double)d.y;
                    deaths.push(e);
                }
                real["deaths"] = deaths;
                real["put_back"] = fmt::format("frame {} (started {}), x {:.3f} (started {:.3f})", r.frameAfter,
                                               r.startFrame, r.xAfter, r.xBefore);
                realText = fmt::format("{:.0f} us a tick over {} ticks{}", mean, m,
                                       r.stoppedByClock ? " (stopped by the clock)" : "");
            }
        }
        out["real_cost"] = real;
    }
    out["ms"] = std::chrono::duration<double, std::milli>(Clock::now() - started).count();

    // One line for the harness to grep.
    if (on) {
        std::string notRun;
        for (auto const& k : c.kinds) {
            if (k.shipped) continue;
            notRun += fmt::format("{}{} {} ({} hazards, {} solids, {} specials)", notRun.empty() ? "" : ", ",
                                  world::kindName(k.kind), k.count, k.reach.hazards, k.reach.solids,
                                  k.reach.specials);
        }
        geode::log::info("[AbsPF] census: {} ({}): {} objects, factors x {} y {} ({}), {} triggers; not run: {}; "
                         "unmodelled collidable {} ({}/{}/{}); real {}",
                         name, id, objects, c.factorX, c.factorY, c.pendingPath, c.triggers,
                         notRun.empty() ? std::string("nothing") : notRun, c.unmodelled.collidable,
                         c.unmodelled.hazards, c.unmodelled.solids, c.unmodelled.specials, realText);
    } else {
        geode::log::info("[AbsPF] census: {} ({}): {} objects, the World is off ({}); real {}", name, id, objects,
                         offReason, realText);
    }
    return out;
}

namespace {
    // A gate's option for Assistant Access (step 4): its kind and band, the
    // object it is about, whether that must or must not fire, and the gravity
    // it leaves.
    matjson::Value choiceJson(absense::route::Option const& o) {
        auto e = matjson::Value::object();
        e["kind"] = std::string(absense::route::optionKindName(o.kind));
        e["band"] = fmt::format("{:.0f}-{:.0f}", o.band.lo, o.band.hi);
        if (o.objectUid != 0) e["uid"] = (int64_t)o.objectUid;
        if (o.mustFire) e["fire"] = std::string("must");
        if (o.mustNotFire) e["fire"] = std::string("must not");
        // GucciBot (step 3): a teleport followed to its exit.
        if (o.teleports) {
            const auto out = absense::route::afterOption(o);
            e["teleports_to"] = o.keepsOffset ? fmt::format("{:+.0f} from where it goes in", o.teleportDy)
                                              : fmt::format("y {:.0f}", (out.lo + out.hi) * 0.5f);
        }
        if (o.flipsGravity) e["gravity_after"] = std::string("flips");
        else if (o.gravityAfter == 0) e["gravity_after"] = std::string("normal");
        else if (o.gravityAfter == 1) e["gravity_after"] = std::string("upside down");
        return e;
    }
}  // namespace

bool absense::levelMapShown() {
    return route::overlayEnabled();
}

void absense::showLevelMap(bool on) {
    route::setOverlayEnabled(on);
}

std::string absense::levelMapSummary() {
    auto const* a = route::atlasFor(PlayLayer::get());
    return a ? route::summary(*a) : std::string();
}

matjson::Value absense::levelMap(float from, float to) {
    auto out = matjson::Value::object();
    auto const* a = route::atlasFor(PlayLayer::get());
    if (!a) {
        out["error"] = "not in a level, or it is still loading";
        return out;
    }
    out["summary"] = route::summary(*a);
    out["length"] = (double)a->length;
    auto columns = matjson::Value::array();
    for (auto const& c : a->columns) {
        if (c.x0 + route::kColumnWidth < from || c.x0 > to) continue;
        auto e = matjson::Value::object();
        e["x"] = (double)c.x0;
        e["mode"] = (int64_t)c.mode;
        e["mini"] = c.mini;
        e["moving"] = c.dynamic;
        e["timed"] = c.timed;
        e["unknown"] = c.unknown;
        e["band"] = fmt::format("{:.0f}-{:.0f}", c.bandLo, c.bandHi);
        auto gaps = matjson::Value::array();
        for (auto const& o : c.openings) gaps.push(fmt::format("{:.0f}-{:.0f}", o.lo, o.hi));
        e["openings"] = gaps;
        columns.push(e);
    }
    out["columns"] = columns;
    auto gates = matjson::Value::array();
    for (auto const& g : a->gates) {
        if (g.x < from || g.x > to) continue;
        auto e = matjson::Value::object();
        e["kind"] = std::string(route::gateKindName(g.kind));
        if (*g.what) e["what"] = std::string(g.what);
        e["x"] = (double)g.x;
        e["y"] = (double)g.y;
        // Step 4: the gate's stable key, column/kind/object uid.
        e["key"] = fmt::format("{}/{}/{}", g.key.column, route::gateKindName(g.key.kind), g.key.objectUid);
        if (g.key.objectUid != 0) e["uid"] = (int64_t)g.key.objectUid;
        auto options = matjson::Value::array();
        for (auto const& o : g.options) options.push(fmt::format("{:.0f}-{:.0f}", o.lo, o.hi));
        e["options"] = options;
        auto choices = matjson::Value::array();
        for (auto const& o : g.choices) choices.push(choiceJson(o));
        e["choices"] = choices;
        gates.push(e);
    }
    out["gates"] = gates;
    return out;
}

int absense::routeGuidance() {
    return (int)route::guidance();
}

void absense::setRouteGuidance(int mode) {
    route::setGuidance((route::Guidance)std::clamp(mode, 0, 2));
}

bool absense::routeShown() {
    return route::routeShown();
}

void absense::showRoute(bool on) {
    route::setRouteShown(on);
}

std::string absense::routeSummary() {
    auto const* p = route::shownPlan(PlayLayer::get());
    return p ? route::summary(*p) : std::string();
}

matjson::Value absense::routePlan(float fromX, float fromY, int limit) {
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto const* a = route::atlasFor(pl);
    if (!a) {
        out["error"] = "not in a level, or it is still loading";
        return out;
    }
    // Planned with player 1's gravity now (step 4: it changes how a cube or
    // robot climbs and falls, and gravity orbs, pads and portals change it).
    const int gravity = pl && pl->m_player1 && pl->m_player1->m_isUpsideDown ? 1 : 0;
    auto const p = route::plan(*a, fromX, fromY, gravity);
    out["summary"] = route::summary(p);
    out["gravity"] = std::string(gravity ? "upside down" : "normal");
    auto routes = matjson::Value::array();
    for (size_t k = 0; k < p.routes.size(); k++) {
        auto const& r = p.routes[k];
        auto e = matjson::Value::object();
        e["cost"] = r.cost;
        if (k == 0) {
            // Each waypoint: where, the option it takes in which gap, and the
            // gravity after it.
            auto points = matjson::Value::array();
            for (size_t i = 0; i < r.points.size() && (int)i < limit; i++) {
                auto const& w = r.points[i];
                if (w.gate < 0 || (size_t)w.gate >= a->gates.size()) {
                    points.push(fmt::format("x {:.0f}: from y {:.0f}, gravity {}", w.x, w.band.lo,
                                            w.gravity ? "upside down" : "normal"));
                    continue;
                }
                points.push(fmt::format("x {:.0f}: {} (gap {:.0f}-{:.0f}, gravity {} after, step {:.1f})", w.x,
                                        route::describe(a->gates[(size_t)w.gate], w.option), w.band.lo, w.band.hi,
                                        w.gravity ? "upside down" : "normal", w.step));
            }
            e["points"] = points;
        } else {
            // Where it first leaves the best route: another option or gap at
            // a gate.
            auto const& best = p.routes.front().points;
            for (size_t i = 0; i < r.points.size() && i < best.size(); i++) {
                if (r.points[i].gate == best[i].gate && r.points[i].choice == best[i].choice) continue;
                e["leaves_best_at_x"] = (double)r.points[i].x;
                break;
            }
        }
        routes.push(e);
    }
    out["routes"] = routes;
    // Step 4: from (fromX, fromY), the next gate's options best first by the
    // cost to go (the way there plus the cheapest way on to the level's end).
    auto next = matjson::Value::array();
    for (auto const& c : route::nextChoices(*a, fromX, fromY, gravity)) {
        if (c.gate < 0 || (size_t)c.gate >= a->gates.size()) continue;
        auto const& g = a->gates[(size_t)c.gate];
        auto e = choiceJson(c.option);
        e["x"] = (double)c.x;
        e["gate"] = *g.what ? std::string(g.what) : std::string(route::gateKindName(g.kind));
        e["cost"] = c.cost;
        e["step"] = c.step;
        e["gravity_then"] = std::string(c.gravity ? "upside down" : "normal");
        next.push(e);
    }
    out["next_gate"] = next;
    return out;
}

matjson::Value absense::routeLeg(float gateX, float lo, float hi, int maxTicks, int frontierCap, bool exact,
                                 double maxMs, float cellY) {
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto* bot = Bot::get();
    if (!pl || !pl->m_player1) {
        out["error"] = "not in a level";
        return out;
    }
    std::string option;  // what the route does at that gate, when it is the route's
    if (gateX <= 0.0f) {
        // The next gate of the planned route, a column or more ahead, in the
        // band of the option the route takes there (step 4: an orb's contact
        // region for a USE, the gap beside it for a SKIP, the whole gap for
        // FREE).
        auto const* plan = route::shownPlan(pl);
        if (!plan || plan->routes.empty()) {
            out["error"] = "no planned route here";
            return out;
        }
        auto const* a = route::atlasFor(pl);
        const float px = pl->m_player1->getPositionX();
        auto const& points = plan->routes.front().points;
        for (size_t i = 1; i < points.size(); i++) {  // [0] is where it was planned from, not a gate
            auto const& w = points[i];
            if (w.x <= px + route::kColumnWidth) continue;
            gateX = w.x;
            lo = w.option.band.lo;
            hi = w.option.band.hi;
            if (a && w.gate >= 0 && (size_t)w.gate < a->gates.size())
                option = route::describe(a->gates[(size_t)w.gate], w.option);
            break;
        }
        if (gateX <= 0.0f) {
            out["error"] = "no gate ahead on the planned route";
            return out;
        }
    }
    if (!bot->trajectory().exists())
        bot->trajectory().init();
    auto* t = bot->trajectory().unsafeInner();
    if (!t) {
        out["error"] = "the simulation could not be made";
        return out;
    }
    Trajectory::LegGoal goal;
    goal.x = gateX;
    goal.lo = lo;
    goal.hi = hi;
    goal.maxTicks = maxTicks;
    goal.frontierCap = frontierCap;
    goal.exact = exact;
    goal.maxMs = maxMs;
    goal.cellY = std::max(0.0f, cellY);
    auto const leg = t->sweepLeg(pl, goal);
    if (leg.error) {
        out["error"] = std::string(leg.error);
        return out;
    }
    const char* outcome = leg.outcome == Trajectory::Leg::Outcome::Found        ? "found"
                          : leg.outcome == Trajectory::Leg::Outcome::Impossible ? "impossible"
                          : leg.outcome == Trajectory::Leg::Outcome::NotFound   ? "not_found"
                                                                                : "out_of_budget";
    out["outcome"] = std::string(outcome);
    out["gate_x"] = (double)gateX;
    out["band"] = lo > hi ? std::string("any") : fmt::format("{:.0f}-{:.0f}", lo, hi);
    if (!option.empty()) out["option"] = option;
    out["ticks"] = (int64_t)leg.ticks;
    out["simulated_ticks"] = (int64_t)leg.steps;
    out["peak_states"] = (int64_t)leg.peakFrontier;
    out["states_dropped_for_room"] = leg.capped;
    // GucciBot (BLOODBATH2.md): how many classes shared the cap at most (a
    // flying copy's height cells among them, with `cell`), and whether it
    // ended with everything dead rather than out of steps, time or ticks.
    out["classes"] = (int64_t)leg.peakClasses;
    out["emptied"] = leg.emptied;
    out["exact"] = exact;
    out["passed_outside_the_band"] = (int64_t)leg.exitsElsewhere;
    out["ms"] = leg.ms;
    // The way as a script: h<N> hold, r<N> release, p<N> a press on each tick,
    // t<N> a tap inside each tick (GucciBot, 2026-10-24: a press let go inside
    // its own tick used to come out as p, which plays back held).
    const auto stepOf = [](TickInput const& in) { return in.presses > 0 ? (in.held ? 'p' : 't') : (in.held ? 'h' : 'r'); };
    std::string script;
    for (size_t i = 0; i < leg.witness.size();) {
        auto const& in = leg.witness[i];
        const char c = stepOf(in);
        size_t j = i + 1;
        while (j < leg.witness.size()) {
            if (stepOf(leg.witness[j]) != c) break;
            j++;
        }
        script += fmt::format("{}{}{}", script.empty() ? "" : " ", c, j - i);
        i = j;
    }
    if (!script.empty()) out["script"] = script;
    route::showLeg(pl, leg.dots, leg.path);
    geode::log::info("[AbsPF] leg to x {:.0f} ({}{}): {} after {} ticks, {} simulated, {} states at most{}, {:.0f} ms",
                     gateX, lo > hi ? std::string("any height") : fmt::format("{:.0f}-{:.0f}", lo, hi),
                     option.empty() ? std::string() : ", the route's " + option, outcome, leg.ticks, leg.steps,
                     leg.peakFrontier, leg.capped ? " (some dropped for room)" : "", leg.ms);
    return out;
}

matjson::Value absense::simVsReal(std::vector<ScriptTick> const& script, int ticks, int every,
                                  std::string const& watch) {
    auto out = matjson::Value::object();
    auto* pl = PlayLayer::get();
    auto* bot = Bot::get();
    if (!pl || !pl->m_player1) {
        out["error"] = "not in a level";
        return out;
    }
    if (!bot->trajectory().exists())
        bot->trajectory().init();
    auto* t = bot->trajectory().unsafeInner();
    if (!t) {
        out["error"] = "the simulation could not be made";
        return out;
    }
    auto& buttons = pl->m_player1->m_holdingButtons;
    auto const bit = buttons.find(static_cast<int>(PlayerButton::Jump));
    bool const downNow = bit != buttons.end() && bit->second;
    auto const inputs = scriptInputs(script, downNow);
    uint32_t const startFrame = bot->updater().getFrame();

    // The same script twice from the same moment: the simulation's copy
    // (which puts the game back itself), then the real player (judge::run
    // puts it back too).
    // GucciBot (2026-11-12): objects to follow by unique id, where each run
    // has them every `every` ticks (the real run then carries on through its
    // deaths, so the objects can be followed to the end).
    std::vector<GameObject*> watched;
    if (!watch.empty() && pl->m_objects) {
        std::vector<int> uids;
        for (auto const& part : geode::utils::string::split(watch, ","))
            if (auto v = geode::utils::numFromString<int>(geode::utils::string::trim(part))) uids.push_back(*v);
        for (auto* o : geode::cocos::CCArrayExt<GameObject*>(pl->m_objects)) {
            if (o && std::find(uids.begin(), uids.end(), o->m_uniqueID) != uids.end()) watched.push_back(o);
            if (watched.size() >= 16) break;
        }
    }
    std::map<int, std::string> simWatch, realWatch;
    auto describeWatched = [&](int tick) {
        std::string s;
        for (auto* o : watched) {
            auto const rect = o->getObjectRect();
            s += fmt::format("{}uid {} {:.2f},{:.2f} rot {:.2f} rect {:.1f},{:.1f} {:.1f}x{:.1f}{}{}",
                             s.empty() ? "" : "; ", o->m_uniqueID, (float)o->m_positionX, (float)o->m_positionY,
                             o->getRotation(), rect.origin.x, rect.origin.y, rect.size.width, rect.size.height,
                             o->m_isGroupDisabled ? " group-off" : "", o->m_isDisabled ? " off" : "");
        }
        return s;
    };
    int const watchEvery = std::max(1, every);
    if (!watched.empty()) {
        simWatch[0] = describeWatched(0);
        t->m_traceWatch = [&](int tick) {
            if (tick % watchEvery == 0) simWatch[tick] = describeWatched(tick);
        };
    }

    t->clearKiller();
    std::vector<TraceSample> sim;
    // Following objects, the copy carries on through what it touches too (a
    // copy that died would stop following them); the first killer is still
    // named.
    // Both are put back by a guard, so a run that throws can't leave every
    // copy unable to die, or a watch pointing at this dead stack frame.
    struct WatchReset {
        Trajectory* t;
        ~WatchReset() {
            Trajectory::s_noDeaths = false;
            t->m_traceWatch = nullptr;
        }
    };
    Trajectory::s_noDeaths = !watched.empty();
    auto const sr = [&] {
        WatchReset reset{t};
        return t->run(pl, true, inputs, ticks, downNow, &sim);
    }();
    auto const k = t->lastKiller();
    std::vector<TraceSample> real;
    judge::Options jopt;
    if (!watched.empty()) {
        jopt.throughDeaths = true;
        realWatch[0] = describeWatched(0);
        jopt.perTick = [&](int tick) {
            if (tick % watchEvery == 0) realWatch[tick] = describeWatched(tick);
        };
    }
    auto const rr = judge::run(inputs, ticks, &real, jopt);
    if (!rr.ran) {
        out["error"] = rr.error;
        return out;
    }

    out["start_frame"] = (int64_t)startFrame;
    out["world_on"] = !world::World::disabled;
    out["after_restore"] = fmt::format(
        "frame {} (started {}), x {:.3f} (started {:.3f}), step armed {} (left armed after {} of the stepped "
        "ticks; refresh-only at start: {})",
        rr.frameAfter, rr.startFrame, rr.xAfter, rr.xBefore, rr.stepArmedAfter ? "yes" : "no", rr.stepLeftArmed,
        rr.onlyRefreshAtStart ? "yes" : "no");
    {
        auto s = matjson::Value::object();
        s["survived"] = (int64_t)sr.survived;
        // In watch mode the copy can't die (it goes on through what it
        // touches), so `died` is whether it hit a killer at all.
        s["died"] = watched.empty() ? sr.died : (sr.died || k.valid);
        s["complete"] = sr.complete;
        if (k.valid) {
            s["killer_id"] = (int64_t)k.id;
            s["killer_uid"] = (int64_t)k.uid;
            s["killer_type"] = (int64_t)k.type;
            s["killer_at"] = fmt::format("{:.1f},{:.1f} rot {:.1f}", k.x, k.y, k.rot);
            s["killer_rect"] = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}", k.rect.origin.x, k.rect.origin.y,
                                           k.rect.size.width, k.rect.size.height);
            s["player_rect"] = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}", k.playerRect.origin.x,
                                           k.playerRect.origin.y, k.playerRect.size.width,
                                           k.playerRect.size.height);
            s["killer_certainty"] = k.certainty == world::Certainty::Static     ? "static"
                                    : k.certainty == world::Certainty::Modelled ? "modelled"
                                                                                  : "uncertain";
        } else {
            s["killer"] = "none named (a solid block's side, the floor or the ceiling, or no death)";
        }
        out["sim"] = s;
    }
    {
        auto s = matjson::Value::object();
        s["survived"] = (int64_t)rr.survived;
        s["died"] = rr.died;
        s["complete"] = rr.complete;
        if (rr.died) {
            s["killer_id"] = (int64_t)rr.killerId;
            s["killer_uid"] = (int64_t)rr.killerUid;
            s["killer_type"] = (int64_t)rr.killerType;
            s["killer_at"] = fmt::format("{:.1f},{:.1f}", rr.killerX, rr.killerY);
        }
        out["real"] = s;
    }
    if (!watched.empty())
        out["watch_note"] = "watch mode: both runs go on through deaths, so survived counts every tick played, not "
                            "the ticks before the first death; died and killer_* name the first death. Run without "
                            "watch for the survived counts.";

    // The first tick the two part: a position apart, or one dead and not the
    // other, or one run over while the other goes on.
    size_t const n = std::min(sim.size(), real.size());
    int first = -1;
    for (size_t i = 0; i < n; i++) {
        if (std::fabs(sim[i].x - real[i].x) > 0.01f || std::fabs(sim[i].y - real[i].y) > 0.01f ||
            sim[i].dead != real[i].dead) {
            first = sim[i].tick;
            break;
        }
    }
    if (first < 0 && sim.size() != real.size())
        first = (int)n + 1;
    out["first_difference_tick"] = (int64_t)first;
    out["first_difference_frame"] = first < 0 ? (int64_t)-1 : (int64_t)(startFrame + (uint32_t)first);
    if (first > 0) {
        auto around = matjson::Value::array();
        for (int tk = std::max(1, first - 3); tk <= first + 3; tk++) {
            size_t const i = (size_t)(tk - 1);
            around.push(fmt::format("{} sim  {}", tk, i < sim.size() ? sampleText(sim[i]) : "-"));
            around.push(fmt::format("{} real {}", tk, i < real.size() ? sampleText(real[i]) : "-"));
        }
        out["around"] = around;
    }
    auto path = matjson::Value::array();
    int const step = std::max(1, every);
    size_t const most = std::max(sim.size(), real.size());
    for (size_t i = 0; i < most; i++) {
        int const tk = (int)i + 1;
        if (tk % step != 0 && i + 1 != most)
            continue;
        path.push(fmt::format("{} sim {} | real {}", tk,
                              i < sim.size() ? fmt::format("{:.1f},{:.1f}", sim[i].x, sim[i].y) : "-",
                              i < real.size() ? fmt::format("{:.1f},{:.1f}", real[i].x, real[i].y) : "-"));
    }
    out["path"] = path;
    if (!watched.empty()) {
        auto w = matjson::Value::array();
        for (auto const& [tk, r] : realWatch) {
            auto const it = simWatch.find(tk);
            w.push(fmt::format("{} sim {} | real {}", tk, it == simWatch.end() ? "-" : it->second, r));
        }
        out["watched"] = w;
        if (!rr.deaths.empty()) {
            auto d = matjson::Value::array();
            for (auto const& nd : rr.deaths)
                if (d.size() < 40)
                    d.push(fmt::format("{} uid {} id {} at {:.1f},{:.1f}", nd.tick, nd.uid, nd.id, nd.x, nd.y));
            out["real_deaths"] = d;
        }
    }
    return out;
}

void absense::distrust(int uid) {
    judge::distrust(uid);
}

void absense::clearDistrust() {
    judge::clearDistrust();
}

std::vector<int> absense::distrustedList() {
    return judge::distrustedList();
}
