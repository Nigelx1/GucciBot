#include "pathfinder.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/devlog.hpp"
#include "absense/gpu/client.hpp"
#include "absense/compat/modal.hpp"
#include "gui/gui.hpp"

#include <string_view>

using namespace geode::prelude;

// Where a found path is written. Absense wrote .slc files to a folder of its
// own; here it goes where GucciBot's macros live and is named as the classic
// Pathfinder names its result: the macro's own name if the user gave it
// one, otherwise the level's name made safe for a file name, in the current
// theme's format. `suffix` goes on the end of the name ("" or " furthest").
static std::filesystem::path gucciMacroPath(PlayLayer* pl, const std::string& suffix, std::string& nameOut) {
    auto* gb = gucci::GucciEngine::get();
    std::string base = gb->replayName;
    if (base.empty()) {
        if (pl && pl->m_level) {
            static constexpr std::string_view kIllegal = "\\/:*?\"<>|";
            for (char c : std::string(pl->m_level->m_levelName)) {
                if ((unsigned char)c < 0x20 || kIllegal.find(c) != std::string_view::npos) continue;
                base += c;
            }
            // Windows does not keep trailing dots or spaces.
            while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
            const size_t first = base.find_first_not_of(' ');
            base = first == std::string::npos ? std::string() : base.substr(first);
        }
        if (base.empty()) base = "pathfinder";
    }
    nameOut = base + suffix;
    return gb->getReplayDir() / (nameOut + gucci::currentThemeExtension(gucci::MenuInterface::get()));
}

namespace {

constexpr int kMinHorizon = 24;
constexpr int kMaxHorizon = 4000;
// However short the look-ahead is set, a decision is judged over at least
// this much time: a cube's jump alone lasts about 70 ticks at 240 TPS, so a
// shorter one commits to jumps whose landing it never sees (0.375 s on
// Slaughterhouse: the jump was approved as "survives the whole horizon" and
// the spike it dies on sat exactly one horizon later).
constexpr double kMinLookahead = 0.75;
// Fixed, not settings: four seconds of look-ahead per decision, a repair may
// resume up to three seconds before the dead end, and a slice takes this much
// of every frame (as much as it can while the game still draws and the stop
// key still answers - the search runs at about 95% of wall clock).
constexpr double kLookahead = 4.0;
constexpr double kBackSeconds = 3.0;
// The most a slice may take while the game is frozen and the search is the
// only thing running. Not 100: with the game costing about 1.5 ms a frame a
// 33 ms slice still gives the search 96% of the wall clock (a 100 ms one
// gives 98.5%), so the last 67 ms buy 2% more searching and cost 20 frames a
// second of a page the user is reading. The "time per frame" setting
// overrides it; this is only the fallback.
constexpr double kFrameBudgetMs = 33.0;
// While the plan is playing for real (m_live) the game must keep real speed,
// so a slice never takes more than this much of a frame at the target rate.
constexpr double kLiveFrameShare = 0.5;
// Never cut a slice below this: under it the search makes no progress at all.
constexpr double kMinBudgetMs = 2.0;
// What one round on the graphics card is sized from. Fixed on purpose: the
// pacing budget must never reach the decisions, or changing how the work is
// spread over frames would change which ideas get scored. This is what
// m_budgetMs * 0.5 already clamped to in 78% of frames.
constexpr double kGpuTargetMs = 20.0;
// How far back a repair can still check its work: an idea at a stop is run
// as far as the spot being repaired, up to this long. Going back further
// than this is pointless - the ideas there cannot see the spot at all, so
// they are approved on "survives the horizon" and walk into it again.
constexpr double kMaxRepairSeconds = 6.0;
// A spot every stop has failed at widens (AbsensePathfinder::escalate): the floor,
// the start and a repair's horizon may reach a second further back per level,
// never more than this before the death - going back stays local. It stays
// inside what keepStart() and the stored frames keep, so a widened floor
// still has a kept start to probe from instead of going back for real.
constexpr double kMaxWidenSeconds = 2.0 * kBackSeconds + kLookahead;  // 10 s (keepStart and the stored frames keep this much and 64 ticks)
// How far (units) the real player may drift from the simulated plan before
// the search thinks again, and how many ticks it plays at least in between.
constexpr float kDivergence = 1.0f;
constexpr uint64_t kMinTicksBetweenReplans = 8;
// Real deaths around one spot before the spot widens (see escalate), and how
// many other ideas are tried from the same stop before the next stop back.
constexpr int kMaxFailsPerSpot = 60;
constexpr int kRetriesPerStop = 3;
// A retry from the same stop only makes sense when the stop is far enough
// before the death for a different idea to change the outcome.
constexpr double kMinRetryDistance = 8.0;
// Simulated ticks one branching search may spend per decision, by effort,
// and how many a decision simulates per slice before letting the frame go
// on (the frame loop calls again while it has time).
constexpr int kSearchBudget[3] = {40000, 100000, 160000};
// A search runs this many ticks between looks at the clock.
constexpr int kSearchStep = 200;
// A frame for going back is stored every this many ticks (x the TPS scale);
// the ticks in between are stepped again from the history. Kept states
// (anchors) are at least this far apart.
constexpr int kStoreEvery = 8;
constexpr int kAnchorEveryMin = 240;

double secondsSince(const std::chrono::steady_clock::time_point& t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// The first cheap look every idea gets before the few that survive it are run
// over the whole horizon. An eighth of the plain look-ahead, not an eighth of
// the horizon: a repair widens the horizon to as much as kMaxRepairSeconds so
// the bar can reach the spot being repaired, and that widening used to widen
// this screen with it. At 2000 TPS a repair screened eleven hundred ideas over
// 1500 ticks each - 1.7 million simulated ticks - to answer the one question
// the screen asks, which is which of them die at once.
int screenTicks(int horizon) { return std::max(96, std::min(horizon, kMaxHorizon) / 8); }

}  // namespace

AbsensePathfinder::AbsensePathfinder() : m_rng(std::random_device{}()) {}

// ------------------------------------------------------------- helpers

void AbsensePathfinder::log(const char* fmt, ...) {
    if (!devlog::on(devlog::Cat::AbsensePathfinder)) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    devlog::log(devlog::Cat::AbsensePathfinder, buf);
}

bool AbsensePathfinder::levelReady() const {
    auto* pl = PlayLayer::get();
    return pl && pl->m_player1 && !pl->m_hasCompletedLevel;
}

uint64_t AbsensePathfinder::currentTick() const { return Bot::get()->updater().getFrame(); }

float AbsensePathfinder::levelLength() const {
    auto* pl = PlayLayer::get();
    return pl ? pl->m_levelLength : 0.0f;
}

float AbsensePathfinder::playerX() const {
    auto* pl = PlayLayer::get();
    return (pl && pl->m_player1) ? pl->m_player1->getPositionX() : 0.0f;
}

bool AbsensePathfinder::playerHeld(bool player2) const {
    auto* pl = PlayLayer::get();
    PlayerObject* p = pl ? (player2 ? pl->m_player2 : pl->m_player1) : nullptr;
    if (!p) return false;
    auto& held = p->m_holdingButtons;
    auto it = held.find(static_cast<int>(PlayerButton::Jump));
    return it != held.end() && it->second;
}

bool AbsensePathfinder::separateNow() const {
    auto* pl = PlayLayer::get();
    return pl && Bot::get()->trajectory().separateControls(pl);
}

RunResult AbsensePathfinder::runFor(const Decision& d, std::span<const TickInput> inputs, int ticks, std::vector<TraceSample>* trace,
                             bool eitherDeath) {
    auto* pl = PlayLayer::get();
    if (!pl) return {};
    if (!d.pair) return Bot::get()->trajectory().run(pl, true, inputs, ticks, d.held, trace);
    return Bot::get()->trajectory().run(pl, !d.player2, inputs, ticks, d.held, trace, d.other, d.otherHeld, eitherDeath);
}

uint64_t AbsensePathfinder::hashPrefix(const std::vector<TickInput>& inputs, int length) {
    uint64_t h = 1469598103934665603ull;
    const int n = std::min<int>(length, (int)inputs.size());
    for (int i = 0; i < n; i++) {
        h ^= (uint64_t)inputs[(size_t)i].presses * 3u + (inputs[(size_t)i].held ? 1u : 0u);
        h *= 1099511628211ull;
    }
    return h ^ (uint64_t)n;
}

bool AbsensePathfinder::isTabu(uint64_t tick, const std::vector<TickInput>& inputs, bool player2) const {
    auto it = m_tabu.find(tick);
    if (it == m_tabu.end()) return false;
    // How often this stop has been looked at (every probe counts one).
    uint32_t visits = 0;
    if (auto v = m_visits.find(tick); v != m_visits.end()) visits = v->second;
    for (const Tabu& t : it->second) {
        if (t.length <= 0 || t.length > (int)inputs.size()) continue;
        if (!t.pair) {
            if (!player2 && hashPrefix(inputs, t.length) == t.hash) return true;  // player 1's own dead end
            continue;
        }
        if (!player2) {
            // The pair's dead end: player 1 may repeat its part the first time
            // round (it may well have been player 2's fault). Once this stop
            // has been come back to, player 1 has to change too, or a dead end
            // that was its fault could never be got round here.
            if (visits > 1 && hashPrefix(inputs, t.length) == t.hash) return true;
            continue;
        }
        // Player 2, with player 1's choice fixed: the pair that led nowhere.
        if (t.length <= (int)m_pairChosen1.size() && hashPrefix(m_pairChosen1, t.length) == t.hash &&
            hashPrefix(inputs, t.length) == t.hash2)
            return true;
    }
    return false;
}

// ------------------------------------------------------------ lifecycle

bool AbsensePathfinder::start() {
    auto* bot = Bot::get();
    auto* pl = PlayLayer::get();
    m_stats = Stats{};
    if (!pl || !pl->m_player1) {
        m_stats.message = "Open a level first.";
        return false;
    }
    if (!pl->m_started) {
        m_stats.message = "Start playing the level first (the pathfinder picks up from the current tick).";
        return false;
    }
    if (pl->m_isPaused) {
        m_stats.message = "Close the pause menu first.";
        return false;
    }
    if (pl->m_hasCompletedLevel) {
        m_stats.message = "The level is already complete.";
        return false;
    }
    if (!bot->trajectory().exists()) {
        m_stats.message = "The simulation is not ready for this level.";
        return false;
    }
    if (Renderer::get()->isRecording()) {
        m_stats.message = "Not while rendering.";
        return false;
    }

    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();
    auto& rs = bot->replaySystem();

    // Remember what to put back.
    m_savedPaused = updater.m_paused;
    m_savedBackstep = updater.m_backwardsStepping;
    m_savedLockDelta = updater.m_lockDelta;
    m_savedStoredFrames = Bot::get()->updater().m_maxBackstepFrames;
    m_savedMode = (int)gucci::GucciEngine::get()->mode;  // GucciBot keeps the mode on the engine

    // Record from here: whatever the replay had after this tick goes (the
    // human macro, if that is what was loaded, is kept as an idea first).
    const uint64_t tick = currentTick();
    absense::human::Reference::get().open(pl, &rs.m_actionAtom, updater.getTps());
    if (!bot->isRecording()) {
        // Silicate labels an input with the frame counter before its tick;
        // GucciBot one later (getFrame() + 1). "Everything from tick t on" is
        // clipActions(t) there and clipFrom(t + 1) here.
        rs.m_actionAtom.clipFrom((uint32_t)tick + 1);
        rs.m_inputIndex = rs.m_actionAtom.length();
        bot->setMode(Bot::Mode::Recording);
    }
    updater.m_backwardsStepping = true;
    (void)0;  /* notifyChange: GucciBot's fields are plain */
    if (!updater.m_lockDelta) {
        updater.m_lockDelta = true;
        (void)0;  /* notifyChange: GucciBot's fields are plain */
    }
    // Going back uses the stored frames: keep enough to reach the limit
    // (and one more window beyond it) without touching the kept states. A
    // frame is stored every few ticks while the search runs (a checkpoint
    // per tick is what made big levels crawl); the ticks in between are
    // stepped again from the history.
    m_savedStoreEvery = pf.m_storeEvery;
    {
        const auto& cfg = SLSettings::get()->pathfinder;
        const double tps = updater.getTps();
        const double scale = std::max(1.0, tps / 240.0);
        pf.m_storeEvery = (uint32_t)std::max(1.0, std::round(kStoreEvery * scale));
        const double limit = kBackSeconds * tps;
        const double ticks = std::clamp(limit * 2.0 + kLookahead * tps + 64.0, 600.0, 20000.0);
        // Exactly as many as the limit going back can use: every one is a
        // full game checkpoint, and 1500 of them on a big level is more than
        // a gigabyte of memory and a stall on every frame.
        const uint32_t want = (uint32_t)(ticks / pf.m_storeEvery) + 16;
        if (Bot::get()->updater().m_maxBackstepFrames != want) {
            Bot::get()->updater().m_maxBackstepFrames = want;
            (void)0;  /* notifyChange: GucciBot's fields are plain */
        }
    }
    updater.setPaused(true);
    if (updater.m_paused != m_savedPaused) (void)0;  /* notifyChange: GucciBot's fields are plain */
    // An intentional death would be recorded as a Death action on the way
    // back; the search never wants that.
    if (updater.m_canDie) {
        updater.m_canDie = false;
        (void)0;  /* notifyChange: GucciBot's fields are plain */
    }

    m_history.clear();
    m_history2.clear();
    m_historyPair.clear();
    m_plan.clear();
    m_plan2.clear();
    m_planIndex = 0;
    m_decision = Decision{};
    m_pairSecondPass = false;
    m_tabu.clear();
    m_visits.clear();
    m_played.clear();
    releaseAnchors();
    m_bestActions = rs.m_actionAtom.m_actions;
    m_repair = Repair{};
    m_lastResume = LastResume{};
    m_traps.clear();
    m_startMoves = 0;
    m_gpuWarned = false;
    m_live = false;
    m_livePending = false;
    m_ahead = Ahead{};
    m_aheadFailed = false;
    m_confirmedKillers.clear();
    m_planKiller = 0;
    m_lessons.clear();
    m_planTail.clear();
    m_planTailTick = UINT64_MAX;
    m_planTrace.clear();
    m_lastReplanTick = 0;
    m_restorePending = false;
    m_restepping = false;
    m_wouldDie = false;
    m_expectReset = false;
    m_completed = false;
    m_starts.clear();
    forgetRunMemo();
    m_probe = Probe{};
    m_goTo = UINT64_MAX;
    bot->trajectory().useStart(nullptr);
    bot->trajectory().clearPhantoms();
    m_deadEndAt = 0;
    m_deadEndReal = false;
    m_deadEndUnseen = false;  // it belongs to the dead end it was set for, not to the next run
    m_deadEndX = 0.0f;

    m_stats.startTick = tick;
    m_stats.currentTick = tick;
    m_stats.bestTick = tick;
    m_worldDriftBase = world::drifts();
    m_stats.bestX = playerX();
    m_startedAt = std::chrono::steady_clock::now();
    m_lastRateAt = m_startedAt;
    m_lastRateTicks = 0;
    m_stats.message = "Searching";

    absense::gpu::Client::get().setEnabled(SLSettings::get()->pathfinder.gpu);
    // On the press that starts the search, not inside the first decision: the
    // same program, the same greeting, the same answers - only the third of a
    // second it costs moves out of a slice.
    absense::gpu::Client::get().prewarm();
    if (SLSettings::get()->pathfinder.learn) absense::memory::Store::get().open(pl);
    keepAnchor();
    keepStart();
    m_phase = Phase::Deciding;
    m_runningFlag = true;

    log("started at tick %llu, x %.1f of %.1f, tps %.0f, lookahead %.2fs, effort %d%s%s",
        (unsigned long long)tick, playerX(), levelLength(), updater.getTps(),
        kLookahead, SLSettings::get()->pathfinder.effort,
        Bot::get()->trajectory().simulatesBoth(PlayLayer::get(), true) ? ", dual: both players simulated" : "",
        (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode) ? ", two-player: each player decided in turn" : "");
    return true;
}

void AbsensePathfinder::stop(const std::string& reason, bool leavingLevel) {
    if (m_phase == Phase::Idle) return;
    auto* bot = Bot::get();
    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();

    const bool done = m_completed;
    if (currentTick() >= m_stats.bestTick) rememberBest();
    m_live = false;
    m_livePending = false;
    m_ahead = Ahead{};
    m_lessons.clear();  // unconfirmed: not learned
    absense::human::Reference::get().close();
    m_phase = Phase::Idle;
    m_runningFlag = false;
    m_restorePending = false;
    m_restepping = false;
    m_expectReset = false;
    m_plan.clear();
    m_plan2.clear();
    m_planIndex = 0;
    m_pairSecondPass = false;
    if (m_decision.searching && !leavingLevel && bot->trajectory().searching()) {
        std::vector<TickInput> dropped;
        (void)bot->trajectory().finishSearch(dropped);  // puts the game state back
    }
    m_decision = Decision{};
    if (!leavingLevel || PlayLayer::get()) bot->trajectory().useStart(nullptr);  // puts the objects' flags back
    m_probe = Probe{};
    m_goTo = UINT64_MAX;
    m_starts.clear();
    forgetRunMemo();
    if (!leavingLevel || PlayLayer::get()) bot->trajectory().clearPhantoms();

    if (PlayLayer::get()) {
        updater.m_backwardsStepping = m_savedBackstep;
        (void)0;  /* notifyChange: GucciBot's fields are plain */
        if (updater.m_lockDelta != m_savedLockDelta) {
            updater.m_lockDelta = m_savedLockDelta;
            (void)0;  /* notifyChange: GucciBot's fields are plain */
        }
        pf.m_storeEvery = std::max<uint32_t>(1u, m_savedStoreEvery);
        if (Bot::get()->updater().m_maxBackstepFrames != m_savedStoredFrames) {
            Bot::get()->updater().m_maxBackstepFrames = m_savedStoredFrames;
            (void)0;  /* notifyChange: GucciBot's fields are plain */
        }
        // The game goes back to however it was before the search started.
        // It used to stay frozen wherever the search gave up, which looks
        // exactly like the game hanging.
        updater.setPaused(m_savedPaused);
    }
    releaseAnchors();

    m_stats.seconds = secondsSince(m_startedAt);
    m_stats.message = reason;
    // The program stays for the next run (it ends with the game, or when the
    // setting goes off): killing it here made the first decision of every run
    // pay for launching it - a third of a second inside one slice.
    absense::gpu::Client::get().forgetLevel();
    absense::memory::Store::get().close(m_stats.bestProgress);
    log("stopped: %s (%llu ticks committed, %llu decisions, %llu simulations, %llu backtracks, best tick %llu, %.1fs)",
        reason.c_str(), (unsigned long long)m_stats.committedTicks, (unsigned long long)m_stats.decisions,
        (unsigned long long)m_stats.simulations, (unsigned long long)m_stats.backtracks,
        (unsigned long long)m_stats.bestTick, m_stats.seconds);

    if (done) {
        m_phase = Phase::Done;
        if (SLSettings::get()->pathfinder.saveWhenDone) saveResult();
    } else if (!leavingLevel && PlayLayer::get() && m_stats.bestTick > m_stats.startTick &&
               m_bestActions.size() > bot->replaySystem().m_actionAtom.length()) {
        // Stopped after going back: the furthest path is not the live replay
        // any more, so it goes to its own file.
        saveFurthest();
    }
}

void AbsensePathfinder::saveFurthest() {
    auto* bot = Bot::get();
    auto& rs = bot->replaySystem();
    std::string name;
    const auto path = gucciMacroPath(PlayLayer::get(), " furthest", name);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (gucci::GucciEngine::get()->replayBackupsEnabled) rs.backupExisting(path);

    // Swap the furthest actions in, write, swap back.
    std::vector<slc::Action> live = rs.m_actionAtom.m_actions;
    rs.m_actionAtom.m_actions = m_bestActions;
    rs.save(path);
    rs.m_actionAtom.m_actions = std::move(live);
    m_stats.message += " - furthest path saved as " + name;
    log("furthest path (tick %llu) saved as %s", (unsigned long long)m_stats.bestTick, name.c_str());
}

void AbsensePathfinder::onQuit() {
    if (isRunning()) stop("level closed", true);
    releaseAnchors();
    m_phase = Phase::Idle;
}

void AbsensePathfinder::onForeignReset() {
    // The game has just reset every object's flags itself (and its checkpoint
    // put back the ones before the respawn): whatever a kept start had cleared
    // belongs to the attempt that is over and must not be written back.
    Bot::get()->trajectory().forgetClearedFlags();
    if (!isRunning()) return;
    if (m_expectReset) {
        m_expectReset = false;
        m_restorePending = false;
        m_restepping = false;
        return;
    }
    stop("the level was reset");
}

void AbsensePathfinder::onLevelComplete() {
    if (!isRunning()) return;
    m_completed = true;
}

bool AbsensePathfinder::onWouldDie(PlayerObject* player, GameObject* object) {
    // Either player dying while the search drives the game is a dead end;
    // the death itself is skipped and the search goes back instead.
    if (!isRunning()) return false;
    m_wouldDie = true;
    m_wouldDieTick = currentTick();
    if (player) m_wouldDieRect = player->getObjectRect(0.3f, 0.3f);
    m_wouldDieP2 = player && PlayLayer::get() && player == PlayLayer::get()->m_player2;
    // Asked here, while the object is still at hand: the death is looked at a
    // tick later (afterTick), by which time the game has moved on.
    m_wouldDieCertainty = world::certaintyOf(GJBaseGameLayer::get(), object);
    return true;
}

// --------------------------------------------------------------- anchors

void AbsensePathfinder::keepAnchor(uint64_t at) {
    auto* bot = Bot::get();
    auto* pl = PlayLayer::get();
    if (!pl) return;
    const uint64_t tick = at == UINT64_MAX ? currentTick() : at;
    if (!m_anchors.empty() && m_anchors.back().tick == tick) return;
    if (m_hasStartAnchor && m_startAnchor.tick == tick) return;

    CheckpointObject* obj = pl->createCheckpoint();
    if (!obj) return;
    obj->retain();
    Anchor a;
    // GucciBot's createCheckpoint takes the frame the state belongs to (Silicate's
    // takes the attempt start and reads the frame itself) -- as in anticroom's port.
    a.state = bot->practiceFix().createCheckpoint(obj, bot->updater().getFrame());
    a.tick = tick;

    // The start state is kept on its own: it is always the last way back.
    if (!m_hasStartAnchor) {
        m_startAnchor = std::move(a);
        m_hasStartAnchor = true;
        return;
    }
    m_anchors.push_back(std::move(a));

    // Every kept state is a full game checkpoint - megabytes on a big level -
    // so there are never many: the recent ones stay dense, the older ones
    // are thinned (going back is local; an old state is the last resort).
    const size_t max = std::clamp<size_t>(SLSettings::get()->pathfinder.maxAnchors, 8, 64);
    while (m_anchors.size() > max) {
        size_t victim = 1;
        uint64_t smallest = UINT64_MAX;
        const size_t half = std::max<size_t>(2, m_anchors.size() / 2);
        for (size_t i = 1; i < half; i++) {
            const uint64_t gap = m_anchors[i + 1].tick - m_anchors[i - 1].tick;
            if (gap < smallest) {
                smallest = gap;
                victim = i;
            }
        }
        if (m_anchors[victim].state.m_checkpoint) m_anchors[victim].state.m_checkpoint->release();
        m_anchors.erase(m_anchors.begin() + (std::ptrdiff_t)victim);
    }
}

size_t AbsensePathfinder::dropOldAnchors(size_t keep) {
    size_t dropped = 0;
    while (m_anchors.size() > keep) {
        if (m_anchors.front().state.m_checkpoint) m_anchors.front().state.m_checkpoint->release();
        m_anchors.pop_front();
        dropped++;
    }
    return dropped;
}

void AbsensePathfinder::dropAnchorsAfter(uint64_t tick) {
    while (!m_anchors.empty() && m_anchors.back().tick > tick) {
        if (m_anchors.back().state.m_checkpoint) m_anchors.back().state.m_checkpoint->release();
        m_anchors.pop_back();
    }
}

void AbsensePathfinder::releaseAnchors() {
    for (auto& a : m_anchors) {
        if (a.state.m_checkpoint) a.state.m_checkpoint->release();
    }
    m_anchors.clear();
    if (m_hasStartAnchor) {
        if (m_startAnchor.state.m_checkpoint) m_startAnchor.state.m_checkpoint->release();
        m_startAnchor = Anchor{};
        m_hasStartAnchor = false;
    }
}

// ---------------------------------------------------------------- starts

void AbsensePathfinder::keepStart(uint64_t at) {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    const uint64_t tick = at == UINT64_MAX ? currentTick() : at;
    forgetRunMemo();  // the real game moved on: runs from kept starts may come out differently now
    if (!m_starts.empty() && m_starts.back().tick >= tick) dropStartsAfter(tick > 0 ? tick - 1 : 0);
    Start st;
    st.tick = tick;
    st.sim = Bot::get()->trajectory().captureStart(pl, true);
    if (!st.sim) return;
    m_starts.push_back(std::move(st));
    // As far back as a repair may look, twice over (the limit can be
    // stretched once), plus a little.
    const auto& cfg = SLSettings::get()->pathfinder;
    const double tps = Bot::get()->updater().getTps();
    const size_t keep = (size_t)(kBackSeconds * tps * 2.0 + kLookahead * tps + 64.0);
    while (m_starts.size() > keep) m_starts.pop_front();
}

void AbsensePathfinder::dropStartsAfter(uint64_t tick) {
    forgetRunMemo();  // a dropped start's address can be handed out again
    while (!m_starts.empty() && m_starts.back().tick > tick) m_starts.pop_back();
}

const AbsensePathfinder::Start* AbsensePathfinder::startAt(uint64_t tick) const {
    if (m_starts.empty() || tick < m_starts.front().tick || tick > m_starts.back().tick) return nullptr;
    // One per tick, in order: index by distance from the front.
    const size_t idx = (size_t)(tick - m_starts.front().tick);
    if (idx < m_starts.size() && m_starts[idx].tick == tick) return &m_starts[idx];
    for (const Start& st : m_starts) {
        if (st.tick == tick) return &st;
    }
    return nullptr;
}

// ------------------------------------------------------------- the loop

void AbsensePathfinder::tick() {
    if (!isRunning()) return;
    if (!levelReady()) {
        if (m_completed) {
            noteProgress();
            stop("level complete");
        } else {
            stop("the level is gone");
        }
        return;
    }
    auto* pl = PlayLayer::get();
    if (pl->m_isPaused || absense::modal::isOpen()) return;  // wait for the player

    const auto frameStart = std::chrono::steady_clock::now();
    // How long this slice may take. Two different jobs, so two budgets:
    //  - live: the plan is being played at real speed and this only decides
    //    about where the plan ends. It takes what is left of a frame at the
    //    target rate and never more than half of one, so the game does not
    //    stutter and the real player does not drift off the plan.
    //  - frozen: the game is stopped waiting for a decision, so the slice
    //    takes the whole "time per frame" setting - less what the frame costs
    //    anyway, so the frame lands near the setting instead of on top of it.
    //    The frame is still drawn after every slice, so the page being read
    //    and the stop key keep answering.
    // What the frame costs besides the slice is measured (noteFrameCost)
    // rather than inferred from the gap between slices, and it is always
    // subtracted. There is no branch that hands back the whole cap because
    // the game was slow - that one was backwards, and it was the whole reason
    // realtime sat at ten frames a second: a game costing 58 ms was answered
    // with 100 ms of thinking on top of it instead of 2 ms. And there is no
    // "a little more next time", which used to climb to the cap during quiet
    // play and also rewarded a slice that had just overrun by two seconds.
    {
        const auto& pcfg = SLSettings::get()->pathfinder;
        const double targetFps = std::clamp(Bot::get()->updater().m_fpsTarget, 30.0, 360.0);
        const double frame = 1000.0 / targetFps;
        const double cap = std::clamp(pcfg.frameBudgetMs, kMinBudgetMs, 200.0);
        // The worse of the last frame and the running average, so one spike is
        // paid for at once and one quiet frame does not undo it.
        const double gameMs = std::clamp(std::max(m_gameMs, m_gameMsAvg), 0.0, 500.0);
        if (m_live) {
            m_budgetMs = std::clamp(frame - gameMs, kMinBudgetMs, std::min(cap, frame * kLiveFrameShare));
        } else if (!pcfg.smooth) {
            m_budgetMs = cap;  // as fast as it goes, but the frame still gets drawn
        } else {
            m_budgetMs = std::clamp(cap - gameMs, kMinBudgetMs, cap);
        }
    }
    const double budget = m_budgetMs / 1000.0;
    m_forcedThisSlice = false;  // one forced unit per slice, not one per loop
    m_unitTimed = false;        // nothing of the last slice is still being timed
    m_stats.budgetMs = m_budgetMs;
    m_deadline = frameStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(budget));

    while (isRunning() && secondsSince(frameStart) < budget) {
        switch (m_phase) {
            case Phase::Restoring:
                if (m_restorePending) {
                    // The restore runs in the frozen tick after this frame.
                    goto done;
                }
                finishRestore();
                break;
            case Phase::Deciding:
                if (!m_live && m_pauseAt != UINT64_MAX) {
                    // Stopped in realtime a few ticks late (the frame's ticks
                    // ran on): back to where it was meant to stop.
                    const uint64_t at = m_pauseAt;
                    m_pauseAt = UINT64_MAX;
                    if (currentTick() > at && rewindTo(at)) goto done;
                }
                if (m_ahead.active && !m_live && currentTick() > m_ahead.tick) {
                    // The plan ran out mid-frame and the game ran on a tick
                    // or two past where the decision is about: back there
                    // first, so the plan it makes starts where it should.
                    if (rewindTo(m_ahead.tick)) goto done;  // the restore runs in the frozen tick
                    cancelAhead();  // no stored frame to go back to: decide from where it is
                }
                decide();
                break;
            case Phase::Probing:
                probe();
                break;
            case Phase::Committing:
                if (m_live) {
                    // The game plays the plan itself; this decides about
                    // where the plan ends meanwhile.
                    if (m_ahead.active) {
                        decide();
                    } else if (m_planIndex < m_plan.size()) {
                        beginAhead();
                        if (!m_ahead.active) goto done;  // nothing to decide ahead of: wait for the plan
                    } else {
                        pauseLive();
                        m_phase = Phase::Deciding;
                    }
                    break;
                }
                if (!commitTick()) goto done;
                break;
            case Phase::Backtracking:
                if (m_ahead.active) {
                    // A dead end seen from where the plan ends, not from
                    // where the game is: the plan (verified that far) plays
                    // out, and the decision at its end, from the real state,
                    // sees the same dead end with the right bookkeeping.
                    cancelAhead();
                    m_aheadFailed = true;
                    m_phase = Phase::Committing;
                    break;
                }
                pauseLive();
                beginBacktrack();
                goto done;
            default:
                goto done;
        }
        if (m_completed) {
            noteProgress();
            stop("level complete");
            goto done;
        }
    }
done:
    m_lastSliceStart = frameStart;
    m_lastSliceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
    m_stats.sliceMs = m_lastSliceMs;
    m_stats.currentTick = currentTick();
    m_stats.seconds = secondsSince(m_startedAt);
    m_stats.worldDrift = world::drifts() - m_worldDriftBase;
    const float len = levelLength();
    m_stats.progress = len > 0.0f ? std::clamp(playerX() / len, 0.0f, 1.0f) : 0.0f;
    const double sinceRate = secondsSince(m_lastRateAt);
    if (sinceRate >= 1.0) {
        m_stats.ticksPerSecond = (double)(m_stats.committedTicks - m_lastRateTicks) / sinceRate;
        m_lastRateTicks = m_stats.committedTicks;
        m_lastRateAt = std::chrono::steady_clock::now();
    }
    // The memory is written in a frame the search already owns, never in the
    // middle of a live physics tick (see Store::remember).
    if (!m_live) absense::memory::Store::get().saveIfDue();
}

// One unit of work is indivisible: a whole run of one idea over the look, a
// whole try of the trim, a whole robustness check. At a high tick rate, or
// with the look widened by a repair, one of those is tens of milliseconds -
// more than a whole slice is allowed. Each of those loops used to force one
// unit of its own ("or nothing would ever get done"), and one slice runs
// several of the loops, so a slice allowed eight milliseconds ran five units
// and took a hundred. Only the first unit of the whole slice is forced now;
// the rest wait for the next frame. The same units run, on the same ideas, in
// the same order - only which frame each one lands in changes, and the game
// is either frozen or playing a plan that was verified before any of this.
bool AbsensePathfinder::mayWork() {
    const auto at = std::chrono::steady_clock::now();
    // The unit that was allowed through last time has just finished: this is
    // where it gets measured. What one costs is not knowable in advance (it
    // depends on the tick rate, on how wide a repair has opened the look, and
    // on how often a steer re-asks) so it is learned from the ones that run.
    if (m_unitTimed) {
        m_unitTimed = false;
        const double cost = std::chrono::duration<double, std::milli>(at - m_unitStart).count();
        if (cost > 0.0 && cost < 1000.0) {
            // Believed quickly when a piece turns out cheap, slowly when one
            // turns out dear. The stages are not alike - judging one of the
            // hundreds of cheap ideas is a fraction of a millisecond, a steer
            // over a widened look is tens - and an estimate left high by the
            // dear stage would let only one cheap idea through a frame.
            m_unitMs = m_unitMs <= 0.0             ? cost
                       : cost < m_unitMs           ? m_unitMs * 0.4 + cost * 0.6
                                                   : m_unitMs * 0.75 + cost * 0.25;
            m_stats.unitMs = m_unitMs;
        }
    }
    auto go = [&] {
        m_unitStart = at;
        m_unitTimed = true;
        m_forcedThisSlice = true;
        return true;
    };
    if (at < m_deadline) {
        // A unit cannot be stopped once it has started, so starting one that
        // will not fit is how a slice allowed eight milliseconds ends up
        // taking fifty. One only starts if what is left of the slice can hold
        // most of one - unless the slice has not managed a single unit yet,
        // in which case it starts anyway and the slice runs long, because a
        // slice that does nothing would never get anywhere.
        const double left = std::chrono::duration<double, std::milli>(m_deadline - at).count();
        if (!m_forcedThisSlice || m_unitMs <= 0.0 || left >= m_unitMs * 0.6) return go();
        return false;
    }
    if (m_forcedThisSlice) return false;
    return go();
}

void AbsensePathfinder::noteFrameCost(double gameMs, double restoreMs) {
    // What the frame cost besides the slice itself. The restore is the
    // search's own work but it lands outside the slice (the frozen tick runs
    // after tick() returns), so it is counted here: the next slice is cut by
    // it, instead of being charged to "the game" and misread as the game
    // being slow.
    const double cost = std::clamp(gameMs + restoreMs, 0.0, 500.0);
    m_gameMs = cost;
    m_gameMsAvg = m_gameMsAvg <= 0.0 ? cost : m_gameMsAvg * 0.8 + cost * 0.2;
    m_stats.gameMs = cost;
}

void AbsensePathfinder::noteProgress() {
    const uint64_t tick = currentTick();
    if (tick > m_stats.bestTick) {
        m_stats.bestTick = tick;
        m_stats.bestX = playerX();
        const float len = levelLength();
        m_stats.bestProgress = len > 0.0f ? std::clamp(playerX() / len, 0.0f, 1.0f) : 0.0f;
    }
}

void AbsensePathfinder::rememberBest() {
    m_bestActions = Bot::get()->replaySystem().m_actionAtom.m_actions;
}

// --------------------------------------------------------------- search

// How far into the level a run ended, in whole blocks. Whole blocks, not
// units, so that two runs that end in the same place have the same key
// whatever the last tick's fraction: the trim then only separates them when
// something really did carry one of them further - a speed portal, a
// teleport portal. 0 whenever "further" means nothing: a platformer level
// (the player can walk back the way it came) and a reversed section (the
// player travels towards smaller x, so the run that gets further has the
// smaller x and would look like a step back).
static int reachOf(float x, bool goingLeft) {
    auto* pl = PlayLayer::get();
    if (!pl || pl->m_isPlatformer) return 0;
    if (goingLeft) return 0;  // the run's own end state, not the live player's
    return (int)std::floor(x / 30.0f);
}

// Events are presses plus state changes without a press (a release, or a
// hold that starts without a fresh push), counted from the real state.
void AbsensePathfinder::countEvents(Candidate& c, bool held, int window) {
    c.events = 0;
    c.eventsWindow = 0;
    c.firstEvent = -1;
    for (size_t i = 0; i < c.inputs.size(); i++) {
        const bool prevHeld = i == 0 ? held : c.inputs[i - 1].held;
        const int ev = c.inputs[i].presses + ((c.inputs[i].held != prevHeld && c.inputs[i].presses == 0) ? 1 : 0);
        if (ev > 0 && c.firstEvent < 0) c.firstEvent = (int)i;
        c.events += ev;
        if ((int)i < window) c.eventsWindow += ev;
    }
}

void AbsensePathfinder::addCandidate(std::vector<Candidate>& out, std::vector<TickInput> inputs, std::string name, bool held) {
    Candidate c;
    c.name = std::move(name);
    c.inputs = std::move(inputs);
    countEvents(c, held, m_decision.window);
    out.push_back(std::move(c));
}

// Is there an orb the game re-fires on every new press (physics/player.cpp:206)
// within reach of this decision? Only a ring can be spammed - m_isMultiActivate
// is set on pads and other enhanced objects too (physics/object.cpp:6) - and only
// one this decision can actually get to is worth hundreds of spam ideas.
static bool multiActivateRingNear(GJBaseGameLayer* pl, float fromX, float toX) {
    static GJBaseGameLayer* forLayer = nullptr;
    static int forCount = -1;
    static std::vector<float> xs;  // sorted; the object list does not change while played
    const int n = pl && pl->m_objects ? pl->m_objects->count() : 0;
    if (pl != forLayer || n != forCount) {
        forLayer = pl;
        forCount = n;
        xs.clear();
        for (int i = 0; i < n; i++) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            switch (o->m_objectType) {  // the kinds the collision pass treats as rings (physics/collisions.cpp:530-540)
                case GameObjectType::CustomRing:
                case GameObjectType::DashRing:
                case GameObjectType::DropRing:
                case GameObjectType::GravityDashRing:
                case GameObjectType::GravityRing:
                case GameObjectType::GreenRing:
                case GameObjectType::PinkJumpRing:
                case GameObjectType::RedJumpRing:
                case GameObjectType::SpiderOrb:
                case GameObjectType::YellowJumpRing:
                case GameObjectType::TeleportOrb: break;
                default: continue;
            }
            auto* e = geode::cast::typeinfo_cast<EnhancedGameObject*>(o);
            if (e && e->m_isMultiActivate) xs.push_back((float)o->m_positionX);
        }
        std::sort(xs.begin(), xs.end());
    }
    if (xs.empty() || fromX > toX) return false;
    const auto it = std::lower_bound(xs.begin(), xs.end(), fromX);
    return it != xs.end() && *it <= toX;
}

void AbsensePathfinder::buildCandidates(std::vector<Candidate>& out, bool held, int horizon) {
    const auto& cfg = SLSettings::get()->pathfinder;
    const double tps = Bot::get()->updater().getTps();
    const double scale = std::max(1.0, tps / 240.0);
    const int H = horizon;

    auto tick = [&](double base) { return std::clamp<int>((int)std::lround(base * scale), 0, H - 1); };

    // Where the first change of input can happen.
    std::vector<int> offsets;
    {
        static constexpr int kBase[] = {0, 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 240};
        int idx = 0;
        for (int b : kBase) {
            if (cfg.effort <= 0 && (idx++ % 2) == 1) continue;
            const int k = tick(b);
            if (k >= H) break;
            if (offsets.empty() || offsets.back() != k) offsets.push_back(k);
        }
        if (cfg.effort >= 2) {
            // Every tick for the first 24 at 240 TPS; at higher tick rates
            // the same moments, not every one of the many more ticks.
            const int fine = std::min(H - 1, tick(24));
            const int stride = std::max(1, (int)std::lround(scale));
            for (int k = 0; k <= fine; k += stride) {
                if (std::find(offsets.begin(), offsets.end(), k) == offsets.end()) offsets.push_back(k);
            }
            std::sort(offsets.begin(), offsets.end());
        }
    }

    auto base = [&]() { return std::vector<TickInput>((size_t)H, TickInput{0, held}); };
    char name[64];

    // Nothing changes.
    addCandidate(out, base(), "keep", held);

    // Holds of one and two ticks (a press let go on the next tick, or the
    // one after) up to long ones; the same lengths let go and pressed again
    // when already holding.
    // The lengths humans hold for: from 1552 human macros (680,760 presses),
    // a hold is 11 ticks at the 10th percentile, 25 at the median and 65 at
    // the 90th, at 240 TPS. The short ones are the spam and swift shapes.
    std::vector<int> holdLengths = {1, 2, std::max(3, tick(3)), tick(6), tick(11), tick(25), tick(48), tick(65)};
    holdLengths.erase(std::unique(holdLengths.begin(), holdLengths.end()), holdLengths.end());

    // The fastest speed in the game (bot/updater.cpp:82), so the window can
    // only be too wide - a speed portal inside the horizon must never hide an
    // orb that is in fact reachable - and both ways, for reversed sections.
    const float span = (576.0f / (float)tps) * (float)H + 60.0f;
    // Around where this decision starts (a kept start, or a start carried to
    // the end of the plan being played), not where the live player is now.
    const float fromX = decisionState(m_decision).x;
    const bool multi = cfg.doubleClicks &&
                       multiActivateRingNear(PlayLayer::get(), fromX - span, fromX + span);
    const int screen = screenTicks(H);  // the Rest stage's first look

    for (int k : offsets) {
        if (cfg.holdInputs) {
            // Hold from k on (or let go at k when already holding).
            auto v = base();
            if (!held) {
                v[(size_t)k] = {1, true};
                for (int j = k + 1; j < H; j++) v[(size_t)j] = {0, true};
                std::snprintf(name, sizeof(name), "hold from %d", k);
            } else {
                for (int j = k; j < H; j++) v[(size_t)j] = {0, false};
                std::snprintf(name, sizeof(name), "release at %d", k);
            }
            addCandidate(out, std::move(v), name, held);
        }
        // A tap at k, released after.
        {
            auto v = base();
            v[(size_t)k] = {1, false};
            for (int j = k + 1; j < H; j++) v[(size_t)j] = {0, false};
            std::snprintf(name, sizeof(name), "tap at %d", k);
            addCandidate(out, std::move(v), name, held);
        }
        if (cfg.doubleClicks) {
            auto v = base();
            v[(size_t)k] = {2, false};
            for (int j = k + 1; j < H; j++) v[(size_t)j] = {0, false};
            std::snprintf(name, sizeof(name), "double at %d", k);
            addCandidate(out, std::move(v), name, held);
        }
        // The gate on where a burst may start (the first tick(48) of the
        // horizon) is only there to keep the idea count down. On a level
        // with an orb that fires again on every press the spam is the way
        // past, and the corridor it is needed in can be anywhere in the
        // horizon - at 1000 TPS tick(48) is 200 of 4000 ticks, and the orb
        // at x 207.7 is 660 ticks into a decision made at tick 0.
        if (cfg.doubleClicks && (k <= tick(48) || multi)) {
            // Orb spam: a press, a release and a press inside one tick, let
            // go on the next - the UFO's black orb spam unit - so many times
            // in a row.
            // The unit is two ticks at any tick rate, so the counts are
            // scaled: 16 cycles is 133 ms at 240 TPS but only 32 ms at 1000,
            // and a drop-orb corridor is ridden on seconds of it. The raw
            // counts stay as well, or the cheapest idea at 1000 TPS would be
            // four activations and "touch this orb once" would be gone.
            // Crossing a corridor whose whole width is orb is not a count at
            // all: that is the shape right below, which never stops.
            std::vector<int> spamCycles;
            const int spamUnit = std::max(1, (int)std::lround(scale));
            for (const int c : {1, 2, 4, 8, 16}) {
                spamCycles.push_back(c);
                if (spamUnit > 1) spamCycles.push_back(c * spamUnit);
            }
            std::sort(spamCycles.begin(), spamCycles.end());
            spamCycles.erase(std::unique(spamCycles.begin(), spamCycles.end()), spamCycles.end());
            for (int cycles : spamCycles) {
                if (cycles <= 0 || k + 2 * cycles > H) continue;
                auto v = base();
                for (int j = 0; j < cycles; j++) {
                    v[(size_t)(k + 2 * j)] = {2, true};
                    v[(size_t)(k + 2 * j + 1)] = {0, false};
                }
                for (int j = k + 2 * cycles; j < H; j++) v[(size_t)j] = {0, false};
                std::snprintf(name, sizeof(name), "orb spam x%d at %d", cycles, k);
                addCandidate(out, std::move(v), name, held);
            }
            // ... and one that never stops. A UFO rides a drop-orb corridor on
            // this unit repeated the whole way, which no fixed number of cycles
            // reaches: sixteen of them is 32 ticks, a thirtieth of a second at
            // 1000 TPS, against a horizon of four seconds.
            // Below the screen the sustained block already builds this exact
            // script as "orb spam on/2 at k": one of them, not both.
            if (!multi || k >= screen) {
                auto v = base();
                int j = k;
                for (; j + 1 < H; j += 2) {
                    v[(size_t)j] = {2, true};
                    v[(size_t)(j + 1)] = {0, false};
                }
                for (; j < H; j++) v[(size_t)j] = {0, false};  // an odd last tick: let go, not left held
                std::snprintf(name, sizeof(name), "orb spam at %d", k);
                addCandidate(out, std::move(v), name, held);
            }
        }
        if (cfg.holdInputs) {
            for (int L : holdLengths) {
                if (L <= 0 || k + L >= H) continue;
                auto v = base();
                if (!held) {
                    v[(size_t)k] = {1, true};
                    for (int j = k + 1; j < k + L; j++) v[(size_t)j] = {0, true};
                    for (int j = k + L; j < H; j++) v[(size_t)j] = {0, false};
                    std::snprintf(name, sizeof(name), "hold %d at %d", L, k);
                } else {
                    for (int j = k; j < k + L; j++) v[(size_t)j] = {0, false};
                    v[(size_t)(k + L)] = {1, true};
                    for (int j = k + L + 1; j < H; j++) v[(size_t)j] = {0, true};
                    std::snprintf(name, sizeof(name), "release %d at %d", L, k);
                }
                addCandidate(out, std::move(v), name, held);
            }
        }
    }

    // Sustained orb spam. A corridor ridden on a multi-activate orb (the
    // black orb UFO spam) needs the unit pressed over and over for as long
    // as the corridor lasts - 1170 ticks of it at 1000 TPS on the 390 unit
    // one at x 195..585 - so the shape has to run to the end of the horizon
    // instead of stopping after sixteen cycles (32 ticks, ten units of x).
    //   The cadence is ticks per press. The unit measured on this level is a
    // press every other tick at 1000 TPS, so the raw 2 comes first; the
    // rate-scaled ones follow in case what an orb wants is the fire rate and
    // not the tick (at 240 TPS they collapse back to 2, 3 and 4).
    //   Where it may start: every ordinary offset inside the screen - the
    // first max(96, H/8) ticks every idea is judged over in the Rest stage -
    // at every cadence, because those are the starts the screen can tell
    // apart. An idea whose first press falls past the screen screens exactly
    // as "keep" does (the shortcut in decide()), so beyond it only the
    // measured cadence is offered, one start every tick(48): enough to reach
    // an orb the next decision is not yet within a screen of, without
    // hundreds of indistinguishable ideas crowding the tie for the 32 places
    // the whole horizon is run for.
    if (multi) {
        std::vector<int> cadences;
        for (const double b : {2.0, 2.0 * scale, 3.0 * scale, 4.0 * scale}) {
            const int p = std::max(2, (int)std::lround(b));
            if (std::find(cadences.begin(), cadences.end(), p) == cadences.end()) cadences.push_back(p);
        }
        auto addSpam = [&](const int k, const int period) {
            if (k < 0 || period <= 0 || k + 2 * period >= H) return;
            auto v = base();
            for (int j = k; j < H; j++)
                v[(size_t)j] = ((j - k) % period == 0) ? TickInput{2, true} : TickInput{0, false};
            std::snprintf(name, sizeof(name), "orb spam on/%d at %d", period, k);
            addCandidate(out, std::move(v), name, held);
        };
        for (const int k : offsets) {
            if (k >= screen) continue;
            for (const int period : cadences) addSpam(k, period);
        }
        for (int k = screen; k < H; k += std::max(1, tick(48))) addSpam(k, cadences.front());
    }

    // Random scripts: a handful of toggles and taps at random ticks. More of
    // them the more often this tick has been visited.
    uint32_t visits = 0;
    if (auto it = m_visits.find(m_decision.tick); it != m_visits.end()) visits = it->second;
    const int randomBase = cfg.effort <= 0 ? 16 : cfg.effort == 1 ? 40 : 100;
    const int randoms = randomBase * (1 + (int)std::min<uint32_t>(visits, 4) + std::min(m_decision.level, 4));  // more at a widened spot
    std::uniform_int_distribution<int> tickDist(0, H - 1);
    std::uniform_int_distribution<int> countDist(1, 6);
    std::uniform_int_distribution<int> kindDist(0, 9);
    for (int r = 0; r < randoms; r++) {
        auto v = base();
        const int n = countDist(m_rng);
        std::vector<int> at;
        for (int i = 0; i < n; i++) at.push_back(tickDist(m_rng));
        std::sort(at.begin(), at.end());
        bool state = held;
        size_t ai = 0;
        for (int t = 0; t < H; t++) {
            TickInput in{0, state};
            while (ai < at.size() && at[ai] == t) {
                const int kind = kindDist(m_rng);
                if (kind < 4 && cfg.holdInputs) {
                    // toggle
                    if (!state) in.presses = (uint8_t)std::min(2, in.presses + 1);
                    state = !state;
                    in.held = state;
                } else if (kind < 8 || !cfg.doubleClicks) {
                    in.presses = (uint8_t)std::min(2, in.presses + 1);
                    state = false;
                    in.held = false;
                } else {
                    in.presses = 2;
                    state = false;
                    in.held = false;
                }
                ai++;
            }
            v[(size_t)t] = in;
        }
        std::snprintf(name, sizeof(name), "random #%d", r + 1);
        addCandidate(out, std::move(v), name, held);
    }
}

// The player as the card's protocol carries it.
static absense::gpu::PlayerState cardPlayer(const Trajectory::StartState& st, double tps, bool held) {
    absense::gpu::PlayerState ps{};
    ps.x = st.x;
    ps.y = st.y;
    ps.yVelocity = st.yVelocity;
    ps.gravity = st.gravity;
    ps.gravityMod = st.gravityMod;
    ps.yStart = st.yStart;
    ps.playerSpeed = st.playerSpeed;
    ps.speedMultiplier = st.speedMultiplier;
    ps.vehicleSize = st.vehicleSize;
    ps.dt = (float)(1.0 / tps);
    ps.mode = (uint32_t)st.mode;
    ps.upsideDown = st.upsideDown ? 1u : 0u;
    ps.onGround = st.onGround ? 1u : 0u;
    ps.held = held ? 1u : 0u;
    return ps;
}

// The graphics card runs a simplified model of the game over tens of
// thousands of ideas at once - far more than the simulation could try in a
// whole decision - and says how long each one lasted. It decides nothing:
// the best few come back as ordinary candidates and the game's own physics
// judges them like any other, so a wrong answer costs a little time.
int AbsensePathfinder::buildGpuCandidates(Decision& d) {
    auto& client = absense::gpu::Client::get();
    if (!client.enabled()) return 0;
    auto* pl = PlayLayer::get();
    if (!pl) return 0;

    Trajectory::StartState st;
    if (m_probe.active) {
        const Start* kept = startAt(m_probe.tick);
        if (!kept || !kept->sim) return 0;
        st = Trajectory::startState(*kept->sim, d.player2);
    } else if (m_ahead.active && m_ahead.start) {
        st = Trajectory::startState(*m_ahead.start, d.player2);
    } else {
        st = Trajectory::liveState(pl, !d.player2);
    }

    const double tps = Bot::get()->updater().getTps();
    // The game's own x a tick (absense::gpu::unitsPerSecond): speedMultiplier is already the game's x velocity
    // constant, so multiplying it by 5.978 again put the card's reach about six times too short.
    const float perTick = absense::gpu::unitsPerSecond(st.playerSpeed) * (float)(1.0 / tps);
    client.sendLevel(pl, st.x, st.x + perTick * (float)d.H + 120.0f, currentTick(), (uint32_t)d.H);

    absense::gpu::PlayerState ps = cardPlayer(st, tps, d.held);
    // A kept start or a decision made ahead stands at another tick than the
    // slice's objects were placed at: what is on the move is carried that far
    // as well, exactly as the exact simulation carries it (MovingObjects::origin).
    ps.moveOrigin = (int32_t)((int64_t)d.tick - (int64_t)client.levelTick());

    const auto& cfg = SLSettings::get()->pathfinder;
    uint32_t want = std::clamp<uint32_t>(cfg.gpuScripts, 1000, 200000);
    // The setting is the least a round scores. A round of a few thousand
    // takes the card a millisecond or two and it then sits idle for the rest
    // of the decision, so a round that got further is followed by a bigger
    // one - filling half of this slice's budget, and never more than twice
    // the setting, so no stop can cost a multiple of what it used to.
    if (d.gpuGained && client.lastCount() >= 1000 && client.lastMs() > 0.0) {
        const double targetMs = kGpuTargetMs;
        const double fits = targetMs / client.lastMs() * (double)client.lastCount();
        want = (uint32_t)std::clamp(fits, (double)want, (double)want * 2.0);
    }
    // Only ask the card about the stretch it can judge. A run that crosses a
    // dual, solo or teleport portal, a teleport or custom orb, or a gravity,
    // rotation or teleport trigger is scored by the wrong physics from there on.
    uint32_t H = (uint32_t)d.H;
    {
        const float stopX = client.nextUnknown(st.x + 30.0f);
        if (stopX < 1.0e30f && perTick > 0.0f) {
            const uint32_t reach = (uint32_t)std::max(0.0f, (stopX - st.x) / perTick);
            // Too little in front of it to tell one idea from another: every
            // one of ten thousand would come back with the same number, and
            // the forty-eight the exact simulation then pays for would be a
            // coin toss. Leave the card out of this decision.
            if (reach < 96u) return 0;
            H = std::min<uint32_t>(reach, H);  // never clamp with lo > hi
        }
    }
    using absense::gpu::Script;
    using absense::gpu::Result;

    // What an idea can do next, in the order the search likes them.
    const bool flying = st.mode == Trajectory::kModeShip || st.mode == Trajectory::kModeDart ||
                        st.mode == Trajectory::kModeBird || st.mode == Trajectory::kModeSwing;
    std::vector<uint32_t> actions;
    if (flying) {
        actions = {absense::gpu::kEventHold, absense::gpu::kEventRelease};
    } else {
        actions = {absense::gpu::kEventTap, absense::gpu::kEventHold, absense::gpu::kEventRelease};
    }
    if (cfg.doubleClicks) actions.push_back(absense::gpu::kEventDouble);
    actions.push_back(absense::gpu::kEventOrb);

    // A beam: a round of ideas is scored, the best few are kept, and every
    // one of them is tried again with one more jump on the end. Several rounds
    // of it, so a spot needing several timed inputs in a row can come out of
    // the card in one decision - a single-event idea never could. One round
    // per slice, so a big batch never costs the frame it lands in.
    using Beam = Decision::GpuIdea;
    // Up to what a script holds: three rounds made three-input ideas at most,
    // and a UFO, ship or orb stretch over the horizon needs more. The three
    // rounds of old always run (a round more a level at a widened spot); past
    // them only while a round gets clearly further and the card is cheap.
    const int rounds = (int)absense::gpu::kMaxEvents;
    if (d.gpuRound == 0) {
        d.gpuBeam.clear();
        d.gpuBest.clear();
        // The root has not been scored, so it is given the whole horizon to
        // sweep. Giving it 0 made the first round place its one input in
        // ticks 0..47 only - 24 ms of play at 2000 TPS - and spend the other
        // nine thousand seven hundred ideas of the round on random hashes.
        d.gpuBeam.push_back(Beam{Script{absense::gpu::kEvents, 0, 0, 0, {}}, (uint32_t)d.H, 0.0f});
    }
    std::vector<Beam>& beam = d.gpuBeam;
    std::vector<Beam>& best = d.gpuBest;

    std::vector<Script> pool;
    std::vector<Result> results;
    {
        const int round = d.gpuRound;
        pool.clear();
        pool.reserve(want);
        // Carrying on unchanged is always in the first round.
        if (round == 0) pool.push_back(Script{absense::gpu::kKeep, 0, 0, 0, {}});

        for (const Beam& parent : beam) {
            // A random script's inputs live in its seed: giving it an event
            // list would throw them away, and the child would be a bare tap
            // rather than the parent with one more jump on the end. It still
            // competes as it is.
            if (parent.script.kind != absense::gpu::kEvents && parent.script.kind != absense::gpu::kKeep) continue;
            const uint32_t from = parent.script.count == 0
                                      ? 0u
                                      : absense::gpu::eventTick(parent.script.events[parent.script.count - 1]) + 2u;
            // Every tick from where the parent leaves off to where it died,
            // a little past it, thinned out when there are many parents.
            const uint32_t to = std::min(H, parent.survived + 48u);
            if (from >= to) continue;
            const uint32_t span = to - from;
            const uint32_t perParent = std::max<uint32_t>(1, want / std::max<size_t>(beam.size(), 1) / (uint32_t)actions.size());
            // Rounded up: a span of 4000 with room for 1600 starts asked for
            // 2000 of them and the sweep was cut off four fifths of the way.
            const uint32_t per = std::max<uint32_t>(perParent, 1);
            const uint32_t step = std::max<uint32_t>(1, (span + per - 1) / per);
            for (uint32_t t = from; t < to && pool.size() < want; t += step) {
                for (const uint32_t action : actions) {
                    if (pool.size() >= want) break;
                    Script next = parent.script;
                    if (next.count >= absense::gpu::kMaxEvents) break;
                    next.kind = absense::gpu::kEvents;
                    next.events[next.count++] = absense::gpu::makeEvent(t, action);
                    pool.push_back(next);
                }
            }
        }
        // The rest of the round is random sequences, so nothing is missed by
        // the beam being narrow.
        const uint32_t base = m_gpuSeed * 1000003u;
        m_gpuSeed++;
        for (uint32_t seed = 0; pool.size() < want; seed++) {
            pool.push_back(Script{absense::gpu::kRandom, base + seed, 1 + (seed % 6), 0, {}});
        }

        if (!client.score(ps, H, pool, results)) {
            if (!m_gpuWarned) {
                m_gpuWarned = true;
                log("graphics card search not used: %s", client.message().empty() ? "no answer" : client.message().c_str());
            }
            d.gpuRound = rounds;
            return 0;
        }
        m_stats.gpuScripts += pool.size();

        std::vector<uint32_t> order(results.size());
        for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
        const size_t keep = std::min<size_t>(order.size(), 48);
        std::partial_sort(order.begin(), order.begin() + keep, order.end(), [&](uint32_t a, uint32_t b) {
            if (results[a].survived != results[b].survived) return results[a].survived > results[b].survived;
            return results[a].x > results[b].x;
        });
        // The best of every round so far, not only of this one: a round that
        // extends the beam can do worse than the one before it.
        for (size_t k = 0; k < keep; k++) {
            best.push_back(Beam{pool[order[k]], results[order[k]].survived, results[order[k]].x});
        }
        std::stable_sort(best.begin(), best.end(), [](const Beam& a, const Beam& b) {
            if (a.survived != b.survived) return a.survived > b.survived;
            // Lasting as long: fewer inputs (a random script's are in its seed, so it counts as full).
            const uint32_t ia = a.script.kind == absense::gpu::kRandom ? absense::gpu::kMaxEvents : a.script.count;
            const uint32_t ib = b.script.kind == absense::gpu::kRandom ? absense::gpu::kMaxEvents : b.script.count;
            return ia < ib;
        });
        if (best.size() > 48) best.resize(48);
        // The next beam: the best of the scripts that can be extended (event
        // lists and "keep"), whatever the random ones did - a random one in
        // the beam only ever produced a bare single tap.
        std::vector<uint32_t> extendable;
        for (uint32_t i = 0; i < results.size(); i++) {
            if (pool[i].kind == absense::gpu::kEvents || pool[i].kind == absense::gpu::kKeep) extendable.push_back(i);
        }
        const size_t beamKeep = std::min<size_t>(extendable.size(), 24);
        std::partial_sort(extendable.begin(), extendable.begin() + (std::ptrdiff_t)beamKeep, extendable.end(),
                          [&](uint32_t a, uint32_t b) {
                              if (results[a].survived != results[b].survived) return results[a].survived > results[b].survived;
                              return results[a].x > results[b].x;
                          });
        m_stats.gpuBatches++;
        if (d.gpuRound == 0) d.gpuFirstMs = client.lastMs();
        d.gpuMs += client.lastMs();
        d.gpuRound++;
        const uint32_t peak = best.empty() ? 0u : best[0].survived;
        const uint32_t gain = peak > d.gpuPeak ? peak - d.gpuPeak : 0u;
        d.gpuPeak = std::max(d.gpuPeak, peak);
        d.gpuGained = gain > 0u;
        // Another round unless something already lasts the whole horizon: the
        // three of old always (a round more a level at a widened spot, never
        // more for a decision made ahead, whose plan is running out), and past
        // them only while the last round got clearly further and the rounds
        // together still cost little - a peak creeping up a few ticks a round
        // at every stop of a repair is exactly the slowness to avoid.
        const int least = m_ahead.active ? 3 : 3 + std::min(d.level, 3);
        const uint32_t minGain = std::max<uint32_t>(12u, H / 32u);
        const bool gaining = !m_ahead.active && gain >= minGain && d.gpuMs < std::max(3.0 * d.gpuFirstMs, 30.0);
        const bool more = d.gpuRound < rounds && peak < H && (d.gpuRound < least || gaining);
        if (more) {
            beam.clear();
            for (size_t k = 0; k < beamKeep; k++) {
                beam.push_back(Beam{pool[extendable[k]], results[extendable[k]].survived, results[extendable[k]].x});
            }
            if (beam.empty()) beam.push_back(Beam{Script{absense::gpu::kEvents, 0, 0, 0, {}}, (uint32_t)d.H, 0.0f});
            return -1;  // the next slice runs it
        }
        d.gpuRound = rounds;
    }

    if (best.empty()) return 0;

    // The best of them go in for a first cheap look (see the Gpu stage).
    size_t added = 0;
    char name[64];
    for (const Beam& b : best) {
        if (added >= 48) break;
        std::vector<TickInput> inputs((size_t)d.H, TickInput{0, d.held});
        uint32_t held = d.held ? 1u : 0u;
        const uint32_t startHeld = held;
        // The card was asked about the front of the plan; the script itself
        // still has to cover all of it, or its tail would be an accident of
        // where the card stopped looking rather than part of the idea.
        for (uint32_t t = 0; t < (uint32_t)d.H; t++) {
            const uint32_t presses = absense::gpu::scriptInput(b.script, t, startHeld, held);
            inputs[t].presses = (uint8_t)std::min<uint32_t>(presses, 2);
            inputs[t].held = held != 0;
        }
        std::snprintf(name, sizeof(name), "card (%u jumps, lasts %u)", (unsigned)b.script.count, (unsigned)b.survived);
        addCandidate(d.candidates, std::move(inputs), name, d.held);
        d.candidates.back().screen = true;
        // What the card made of it over the whole horizon, for the ties the
        // cheap look leaves behind (see the Gpu and Rest stages).
        d.candidates.back().card = (int)b.survived;
        added++;
    }
    return (int)added;
}

// The mod's own ideas on the card: every one of them spelled out tick by
// tick and run over the whole horizon by the card's simplified model. What
// comes back only breaks ties among ideas the exact short look could not tell
// apart - the exact simulation still runs and judges every one of them, so a
// wrong answer here costs nothing but the order they are tried in.
void AbsensePathfinder::scoreOnCard(Decision& d, const std::vector<size_t>& which) {
    auto& client = absense::gpu::Client::get();
    if (!client.enabled() || which.empty()) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    const Trajectory::StartState st = decisionState(d);
    const double tps = Bot::get()->updater().getTps();
    const float perTick = absense::gpu::unitsPerSecond(st.playerSpeed) * (float)(1.0 / tps);
    client.sendLevel(pl, st.x, st.x + perTick * (float)d.H + 120.0f, currentTick(), (uint32_t)d.H);
    // Only as far as the card's model holds (see buildGpuCandidates).
    uint32_t H = (uint32_t)d.H;
    const float stopX = client.nextUnknown(st.x + 30.0f);
    if (stopX < 1.0e30f && perTick > 0.0f) {
        H = std::min<uint32_t>(H, (uint32_t)std::max(0.0f, (stopX - st.x) / perTick));
    }
    // Too little in front of them to tell one from another.
    if (H < 96u) return;
    absense::gpu::PlayerState ps = cardPlayer(st, tps, d.held);
    ps.moveOrigin = (int32_t)((int64_t)d.tick - (int64_t)client.levelTick());

    const uint32_t wordsPerScript = (H + 7) / 8;
    // A batch is a pipe transfer and a wait inside this slice: four megabytes
    // of it at most. Every one of them or none - scoring a part of a tied set
    // would leave the rest without the key the sorts below rank by, and "equal
    // to everything it did not score" is not an order at all (a std::sort
    // handed one is free to walk off the end of the list).
    const size_t most = std::max<size_t>(64, (1u << 20) / std::max<uint32_t>(wordsPerScript, 1u));
    if (which.size() > most) return;
    const size_t count = which.size();
    std::vector<uint32_t> words(count * wordsPerScript, 0u);
    for (size_t k = 0; k < count; k++) {
        const std::vector<TickInput>& in = d.candidates[which[k]].inputs;
        bool held = d.held;
        for (uint32_t t = 0; t < H; t++) {
            uint32_t presses = 0;
            if (t < in.size()) {
                presses = in[t].presses;
                held = in[t].held;
            }
            absense::gpu::packRawTick(words.data() + k * wordsPerScript, t, presses, held);
        }
    }
    std::vector<absense::gpu::Result> results;
    if (!client.scoreRaw(ps, H, (uint32_t)count, words, results)) {
        if (!m_gpuWarned) {
            m_gpuWarned = true;
            log("graphics card search not used: %s", client.message().empty() ? "no answer" : client.message().c_str());
        }
        return;
    }
    m_stats.gpuBatches++;
    m_stats.gpuScripts += count;
    for (size_t k = 0; k < count; k++) d.candidates[which[k]].card = (int)results[k].survived;
}

Trajectory::StartState AbsensePathfinder::decisionState(const Decision& d) {
    if (m_probe.active) {
        const Start* kept = startAt(m_probe.tick);
        if (kept && kept->sim) return Trajectory::startState(*kept->sim, d.player2);
    }
    if (m_ahead.active && m_ahead.start) return Trajectory::startState(*m_ahead.start, d.player2);
    return Trajectory::liveState(PlayLayer::get(), !d.player2);
}

// Scripts made by steering the simulated player through what is ahead
// (see Trajectory::steer): with a short look-ahead it hugs walls and
// switches late, with a long one it switches early - a wave corridor, a
// ship under a ceiling, a UFO through spikes all come out of these. The
// steering itself runs when the idea is evaluated.
void AbsensePathfinder::buildSteeredCandidates(std::vector<Candidate>& out, bool held, int horizon) {
    (void)held;
    (void)horizon;
    const double tps = Bot::get()->updater().getTps();
    const double scale = std::max(1.0, tps / 240.0);
    static constexpr int kLookaheads[] = {2, 4, 8, 16, 32, 64};
    const int effort = SLSettings::get()->pathfinder.effort;
    char name[64];
    for (int base : kLookaheads) {
        if (effort <= 0 && (base == 4 || base == 16 || base == 64)) continue;
        const int lookahead = std::max(1, (int)std::lround(base * scale));
        Candidate c;
        std::snprintf(name, sizeof(name), "steer %d", lookahead);
        c.name = name;
        c.steer = lookahead;
        out.push_back(std::move(c));
    }
}

// The script the branching search found (see Trajectory::search), once it
// has run: judged like any other by running it.
void AbsensePathfinder::addSearchCandidate(std::vector<Candidate>& out, bool held, int horizon, const RunResult& r,
                                    std::vector<TickInput> script) {
    m_stats.simulatedTicks += (uint64_t)std::max(0, r.simulated);
    m_stats.branches += (uint64_t)std::max(0, r.branches);
    if (script.empty()) return;
    const TickInput last = script.back();
    while ((int)script.size() < horizon) script.push_back({0, last.held});
    addCandidate(out, std::move(script), "search", held);
    out.back().claimed = r.complete ? horizon : r.survived;
    if (r.dualDeath && r.died) m_stats.dualDeaths++;
}

bool AbsensePathfinder::trimStep(Decision& d) {
    auto* pl = PlayLayer::get();
    Candidate& c = d.chosen;
    // Forced short only because a death follows the safe stretch is still
    // worth trimming; forced INTO the death (forcedCommit = survived + 1)
    // is not - that plan is not a way past anything.
    if (!pl || c.inputs.empty() || (d.forcedCommit > 0 && d.forcedCommit > c.survived)) return true;
    const int H = d.H;
    const bool held = d.held;
    const int window = std::max(1, std::min(d.window, (int)c.inputs.size()));
    // How far a try has to hold up: the bar this decision applied (d.need) and
    // enough past the ordinary commit for it not to shrink (that one is
    // min(K, survived - margin)). Past that a try is look-ahead the next
    // decision makes again - and at 2000 TPS that was a whole horizon a try,
    // up to ninety-six of them. It is not the last word on any of them: what
    // the trim ends with is run over the whole horizon once below, and only
    // kept if it gets as far there as the idea did, so the realtime commit
    // (two thirds of the horizon, further than this cap) is covered as well.
    // ... and a plan that reaches the end of the level past this cap can never
    // be seen to reach it by a try that stops at the cap: no variant is ever as
    // good, nothing is trimmed, and the pass spends up to trimMax runs proving
    // it. That one gets the whole horizon, where a try that still completes
    // says so and the settle run below is neither possible nor needed.
    const int trimTo = c.complete ? H : std::min(H, std::max(d.need, d.K + d.margin));

    // The ticks inside the window where the input changes.
    std::vector<int> sw;
    auto collect = [&]() {
        sw.clear();
        bool state = held;
        for (int i = 0; i < window; i++) {
            if (c.inputs[(size_t)i].presses > 0 || c.inputs[(size_t)i].held != state) sw.push_back(i);
            state = c.inputs[(size_t)i].held;
        }
    };
    collect();
    if (d.trimMax <= 0) d.trimMax = std::min(96, 8 + 6 * (int)sw.size());

    // What a switch is: a tap that leaves the state as it was (0), a change
    // of state undone by the next switch (1), or one that stays (2).
    auto kindOf = [&](size_t k) {
        const int i = sw[k];
        const bool before = i == 0 ? held : c.inputs[(size_t)i - 1].held;
        if (c.inputs[(size_t)i].held == before) return 0;
        if (k + 1 < sw.size() && c.inputs[(size_t)sw[k + 1]].held == before) return 1;
        return 2;
    };

    while (d.trimAt < sw.size() && d.trimTries < d.trimMax && mayWork()) {
        const int i = sw[d.trimAt];
        const bool before = i == 0 ? held : c.inputs[(size_t)i - 1].held;
        const TickInput at = c.inputs[(size_t)i];
        const int kind = kindOf(d.trimAt);
        const int variants = kind == 2 ? 2 : 1;
        if (d.trimVariant >= variants) {
            d.trimAt++;
            d.trimVariant = 0;
            continue;
        }
        std::vector<TickInput> variant = c.inputs;
        if (kind == 0) {
            // A tap (or a re-press) that leaves the state as it was: drop it.
            variant[(size_t)i] = TickInput{0, before};
        } else if (kind == 1) {
            // A change of state undone by the next switch: flatten the two.
            const int j = sw[d.trimAt + 1];
            for (int t = i; t <= j; t++) variant[(size_t)t] = TickInput{0, before};
        } else if (d.trimVariant == 0) {
            // A change of state that stays: not at all ...
            for (size_t t = (size_t)i; t < variant.size(); t++) variant[t] = TickInput{0, before};
        } else {
            // ... or later (a switch made as soon as the far future looked
            // bad, where a little later would do just as well).
            const int later = i + std::max(2, d.margin / 2);
            if (later >= (int)variant.size()) {
                d.trimVariant++;
                continue;
            }
            for (int t = i; t < later; t++) variant[(size_t)t] = TickInput{0, before};
            variant[(size_t)later] = at;
        }
        // The tabu is checked on an idea as it was generated, and the trim turns many ideas
        // into the same few: a variant that is what already led nowhere from this tick would
        // be played again under another idea's name.
        if (isTabu(d.tick, variant, d.player2)) {
            d.trimVariant++;
            continue;
        }
        d.trimTries++;
        d.scratchTrace.clear();
        const RunResult r = runFor(d, variant, trimTo, &d.scratchTrace);
        m_stats.simulations++;
        m_stats.simulatedTicks += (uint64_t)std::max(0, r.survived);
        const bool asGood = r.complete ? true : (!c.complete && r.survived >= std::min(c.survived, trimTo));
        if (asGood) {
            c.inputs = std::move(variant);
            // Measured, except where the try only reached the cap - that one is
            // settled by the whole-horizon run below. Where the cap is the
            // horizon there is no such run and nothing to settle: reaching it
            // is the whole answer, and holding on to the old figure would
            // understate an idea the trim made last longer.
            if (r.complete || r.survived < trimTo || trimTo >= H) c.survived = r.survived;
            c.complete = r.complete;
            d.trimmed++;
            d.trimVariant = 0;
            d.bestTrace.swap(d.scratchTrace);  // the trimmed script's own samples
            d.bestTraceFor = -1;               // it is the chosen one's now, not a candidate's
            d.bestTraceTick = currentTick();
            collect();  // the switch list changed; look at the same place again
        } else {
            d.trimVariant++;
        }
    }
    const bool done = d.trimAt >= sw.size() || d.trimTries >= d.trimMax;
    if (done) {
        if (d.trimmed > 0 && trimTo < H) {
            // The script the trim ends with, over the whole horizon, once: the
            // tries only had to hold up as far as this decision acts, and what
            // is committed is never approved on less than a full run.
            d.scratchTrace.clear();
            const RunResult r = runFor(d, c.inputs, H, &d.scratchTrace);
            m_stats.simulations++;
            m_stats.simulatedTicks += (uint64_t)std::max(0, r.survived);
            if (r.complete || (!d.trimComplete && r.survived >= d.trimFrom)) {
                c.survived = r.survived;
                c.complete = r.complete;
                d.bestTrace.swap(d.scratchTrace);
                d.bestTraceFor = -1;
                d.bestTraceTick = currentTick();
            } else {
                // Fewer clicks, but it does not get as far over the whole
                // horizon: the idea as it was chosen is what is played.
                c.inputs = d.chosenRaw;
                c.survived = d.trimFrom;
                c.complete = d.trimComplete;
                d.trimmed = 0;
                d.bestTraceTick = UINT64_MAX;  // no samples for this script: the commit makes its own
            }
        }
        countEvents(c, held, d.window);
    }
    return done;
}

namespace {
constexpr uint64_t kRunSalt = 0x72756e;           // a plain run
constexpr uint64_t kLateSalt = 0x6c617465;        // robust()'s late run (+ its bar)
constexpr uint64_t kSteerSalt = 0x7374656572;     // a steered idea (+ its look-ahead)
constexpr uint64_t kSearchSalt = 0x736561726368;  // the search (+ its budget and widening)

// Two independent 64-bit hashes of (start, ticks, button, salt, script): the
// entries of the script a run of `ticks` reads, and how many there are.
std::pair<uint64_t, uint64_t> memoKey(const Trajectory::SimStart* from, int ticks, bool held,
                                      std::span<const TickInput> in, uint64_t salt) {
    uint64_t a = 1469598103934665603ull, b = 0x9e3779b97f4a7c15ull;
    auto mix = [&](uint64_t v) {
        a = (a ^ v) * 1099511628211ull;
        b = (b ^ v) * 0xff51afd7ed558ccdull;
        b ^= b >> 33;
    };
    mix((uint64_t)(uintptr_t)from);
    mix(salt);
    mix((uint64_t)(uint32_t)ticks);
    mix(held ? 1u : 0u);
    const size_t n = std::min(in.size(), (size_t)std::max(0, ticks));
    mix((uint64_t)n);
    for (size_t i = 0; i < n; i++) mix((uint64_t)in[i].presses * 3u + (in[i].held ? 1u : 0u));
    return {a, b};
}
}  // namespace

// The kept start this decision's runs begin from (a probe's), or nullptr when
// they begin from the live player or from a start carried ahead - those depend
// on where the real game is. Two-player decisions carry the other player's
// script too and are left out.
const Trajectory::SimStart* AbsensePathfinder::memoStart() const {
    if (!m_probe.active || m_decision.pair) return nullptr;
    const Start* st = startAt(m_probe.tick);
    return st && st->sim ? st->sim.get() : nullptr;
}

bool AbsensePathfinder::robust(const Candidate& c, int horizon, bool held, int need) {
    auto* pl = PlayLayer::get();
    if (!pl) return true;
    // No press and no change of state anywhere in it: the script a tick late
    // is the same script, and it was already run over this horizon.
    if (c.events == 0 && c.evaluated && !c.inputs.empty()) return c.complete || c.survived >= need;
    std::vector<TickInput> late;
    late.reserve(c.inputs.size());
    late.push_back({0, held});
    for (size_t i = 0; i + 1 < c.inputs.size(); i++) late.push_back(c.inputs[i]);
    const Trajectory::SimStart* lateFrom = memoStart();
    const auto lateKey = lateFrom ? memoKey(lateFrom, horizon, m_decision.held, late, kLateSalt + ((uint64_t)(uint32_t)need << 32))
                                  : std::pair<uint64_t, uint64_t>{0, 0};
    if (lateFrom) {
        const auto it = m_runMemo.find(lateKey.first);
        if (it != m_runMemo.end() && it->second.check == lateKey.second) return it->second.complete || it->second.survived >= need;
    }
    // Only whether it lasts `need` ticks is asked: a run of min(horizon, need)
    // ticks gives the same answer (it is a prefix of the longer one).
    const RunResult r = runFor(m_decision, late, std::min(horizon, std::max(1, need)));
    m_stats.simulations++;
    m_stats.simulatedTicks += (uint64_t)std::max(0, r.survived);
    if (lateFrom) {
        if (m_runMemo.size() >= kMaxRunMemo) m_runMemo.clear();
        RunMemo& m = m_runMemo[lateKey.first];
        m = RunMemo{};
        m.check = lateKey.second;
        m.survived = r.survived;
        m.complete = r.complete;
    }
    return r.complete || r.survived >= need;
}

bool AbsensePathfinder::evaluate(Candidate& c, int horizon, bool held, std::vector<TraceSample>* trace) {
    if (c.evaluated) return true;
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    // From a kept start, what an earlier pass or retry already ran there is looked
    // up instead of run again (see memoStart) - the steering as well.
    const Trajectory::SimStart* from = memoStart();
    const auto steerKey = from && c.steer > 0 ? memoKey(from, horizon, held, {}, kSteerSalt + (uint64_t)(uint32_t)c.steer)
                                              : std::pair<uint64_t, uint64_t>{0, 0};
    if (from && c.steer > 0 && c.inputs.empty()) {
        const auto it = m_runMemo.find(steerKey.first);
        if (it != m_runMemo.end() && it->second.check == steerKey.second) {
            if (it->second.script.empty()) {
                c.evaluated = true;
                c.survived = -1;
                return true;
            }
            c.inputs = it->second.script;
            const TickInput last = c.inputs.back();
            while ((int)c.inputs.size() < horizon) c.inputs.push_back({0, last.held});
            countEvents(c, held, m_decision.window);
            c.claimed = it->second.claimed;
        }
    }
    if (c.steer > 0 && c.inputs.empty()) {
        // The steering itself, then its script is judged like any other.
        const Decision& d = m_decision;
        const RunResult sr = d.pair ? Bot::get()->trajectory().steer(pl, !d.player2, horizon, held, c.steer, c.inputs, d.other, d.otherHeld)
                                    : Bot::get()->trajectory().steer(pl, true, horizon, held, c.steer, c.inputs);
        m_stats.simulations++;
        m_stats.simulatedTicks += (uint64_t)std::max(0, sr.simulated);
        if (from) {
            if (m_runMemo.size() >= kMaxRunMemo) m_runMemo.clear();
            RunMemo& m = m_runMemo[steerKey.first];
            m = RunMemo{};
            m.check = steerKey.second;
            m.script = c.inputs;
            m.claimed = sr.complete ? horizon : sr.survived;
        }
        if (c.inputs.empty()) {
            // Nothing could be steered at all (the state is already lost):
            // this is not an idea. It used to count as one that "lasts 0",
            // win the tie on fewest clicks when everything dies at once, and
            // be committed as a plan with nothing in it - which plays
            // nothing, so the same decision ran again a hundred times a
            // second at the same tick, forever.
            c.evaluated = true;
            c.survived = -1;
            return true;
        }
        const TickInput last = c.inputs.back();
        while ((int)c.inputs.size() < horizon) c.inputs.push_back({0, last.held});
        countEvents(c, held, m_decision.window);
        c.claimed = sr.complete ? horizon : sr.survived;
    }
    const auto runKey = from ? memoKey(from, horizon, held, c.inputs, kRunSalt) : std::pair<uint64_t, uint64_t>{0, 0};
    if (from) {
        const auto it = m_runMemo.find(runKey.first);
        if (it != m_runMemo.end() && it->second.check == runKey.second) {
            const RunMemo& m = it->second;
            c.survived = m.survived;
            c.complete = m.complete;
            c.reach = reachOf(m.x, m.goingLeft);
            c.evaluated = true;
            // The simulation is right about where it stands: nothing in the
            // level can move it, or everything that can is a kind the World
            // runs for this run (world::certaintyOf).
            c.killerStatic = m.died && m.killer.valid && m.killer.id != 0 &&
                             m.killer.certainty != Trajectory::Killer::Certainty::Uncertain;
            c.killerId = m.killer.valid ? m.killer.id : 0;
            c.killerUid = m.killer.valid ? m.killer.uid : 0;
            c.killerX = m.killer.x;
            c.killerY = m.killer.y;
            return true;
        }
    }
    const RunResult r = runFor(m_decision, c.inputs, horizon, trace);
    m_stats.simulations++;
    m_stats.simulatedTicks += (uint64_t)std::max(0, r.survived) + (r.died ? 1u : 0u);
    c.survived = r.survived;
    c.complete = r.complete;
    c.reach = reachOf(r.x, r.goingLeft);
    c.evaluated = true;
    {
        const auto& k = Bot::get()->trajectory().lastKiller();
        c.killerStatic = r.died && k.valid && k.id != 0 &&
                         k.certainty != Trajectory::Killer::Certainty::Uncertain;
        c.killerId = k.valid ? k.id : 0;
        c.killerUid = k.valid ? k.uid : 0;
        c.killerX = k.x;
        c.killerY = k.y;
        if (from) {
            if (m_runMemo.size() >= kMaxRunMemo) m_runMemo.clear();
            RunMemo& m = m_runMemo[runKey.first];
            m = RunMemo{};
            m.check = runKey.second;
            m.survived = r.survived;
            m.complete = r.complete;
            m.died = r.died;
            m.x = r.x;
            m.goingLeft = r.goingLeft;
            m.killer = k;
        }
    }
    if (c.claimed >= 0 && std::abs(c.claimed - (r.complete ? horizon : r.survived)) > 1) {
        m_stats.unrepeatable++;
        // The steering and a plain run of its script disagree: the
        // simulation's branch-and-restore is not putting something back.
        log("%s says it lasts %d ticks but its script lasts %d", c.name.c_str(), c.claimed, r.survived);
    }
    return true;
}

// A decision is made in slices (one per call, the frame loop calls again
// while it has time): the cheap ideas are judged a few at a time, and only
// when none of them lasts the whole horizon does the branching search run,
// also in slices, so the game keeps drawing whatever the search costs.
void AbsensePathfinder::decide() {
    auto* bot = Bot::get();
    auto* pl = PlayLayer::get();
    if (!pl) return;
    Decision& d = m_decision;
    const auto sliceStart = std::chrono::steady_clock::now();
    struct SliceTimer {
        double& into;
        std::chrono::steady_clock::time_point at;
        ~SliceTimer() { into += secondsSince(at); }
    } sliceTimer{m_stats.secondsSim, sliceStart};

    if (!d.active) {
        const auto& cfg = SLSettings::get()->pathfinder;
        const double tps = bot->updater().getTps();
        d = Decision{};
        d.active = true;
        d.H = std::clamp((int)std::lround(std::max(kLookahead, kMinLookahead) * tps), kMinHorizon, kMaxHorizon);
        d.K = std::max(6, d.H / 3);
        d.margin = std::max(3, d.H / 8);
        d.tick = m_probe.active ? m_probe.tick : m_ahead.active ? m_ahead.tick : currentTick();
        d.level = widenLevel(d.tick);  // 0 unless a spot every stop failed at is ahead (see escalate)
        // Repairing a dead end from a stop further back than the look-ahead
        // reaches: the ideas are run as far as the spot being repaired (up
        // to 3 s), or the verdict could not tell whether one gets past it
        // and would send the search back into the same trap on the word of
        // "lasts the whole horizon" (44 times over on one pit). What is
        // played per decision (K) stays as it was.
        // ... and the same after the resume, until the path is past the spot:
        // the decisions right after a resume were back on the plain horizon,
        // could not see the death 1143 ticks ahead, approved anything that
        // "lasts the whole horizon", left the one route that gets past, and
        // walked into the same death forever (Sonic Wave, 40%).
        const uint64_t seeTo = m_repair.active ? m_repair.deathTick : m_lastResume.valid ? m_lastResume.deathTick : 0;
        if (seeTo > d.tick) {
            const uint64_t dist = seeTo - d.tick + (uint64_t)(2 * d.margin);
            // kMaxHorizon is a count of 240-TPS ticks (16.7 s there, 4 s at
            // 1000 TPS), while the floor the walk back puts down is
            // kMaxRepairSeconds of real time: at 1000 TPS that floor is 6000
            // ticks back and this cap was 4000, so a stop at the floor could
            // not see the spot it was repairing at all - the case the comment
            // above is about. Scaled with the tick rate, the cap is the same
            // stretch of time at any rate.
            const int maxTicks = (int)std::lround(kMaxHorizon * std::max(1.0, tps / 240.0));
            // ... and as far back as a widened spot's floor may go (a second more
            // a level, see escalate), or a stop below the old floor could not see
            // the spot and would approve ideas that only survive the horizon.
            const double seeSeconds = std::min(kMaxWidenSeconds, kMaxRepairSeconds + (double)d.level);
            const int cap = std::max(d.H, std::min(maxTicks, (int)std::lround(seeSeconds * tps)));
            d.H = std::clamp((int)std::min<uint64_t>(dist, (uint64_t)cap), d.H, cap);
        }
        // The bar an idea has to clear to be worth playing (the verdict's
        // "need"), settled here from the horizon and the margin this decision
        // was set up with, so the verdict does not work the same one out a
        // second time and the two cannot drift apart.
        d.need = d.margin;
        if (seeTo > d.tick) {
            // Past the spot, and a little - not past it by a whole margin as
            // well. At 2000 TPS the margin is 500 ticks, so a stop 1067 ticks
            // before the death was asking one decision for 1567: 253 units of
            // x, eight more gaps of the orb corridor, done in one go or the
            // stop was thrown away. Getting past the spot is what a repair is
            // for; the clearance only stops an idea that dies on the very next
            // tick past it from counting as a way through.
            const int clear = std::max(8, d.margin / 8);
            // ... at every distance: kept at no less than the margin, a stop 50 ticks before the death needed 500 at
            // 1000 TPS and threw away an idea lasting 351 (tick 5290), while stops further back asked distance + clearance.
            d.need = (int)std::min<uint64_t>(seeTo - d.tick, (uint64_t)(d.H - clear)) + clear;
        }
        // Two-player mode: player 1 is decided first with player 2 carrying
        // on, then player 2 with player 1's choice (the second pass).
        d.pair = m_probe.active ? m_probe.pair : m_ahead.active ? m_ahead.pair : separateNow();
        d.player2 = d.pair && m_pairSecondPass;
        if (!d.pair) m_pairSecondPass = false;
        d.held = m_probe.active ? (d.player2 ? m_probe.held2 : m_probe.held)
                 : m_ahead.active ? (d.player2 ? m_ahead.held2 : m_ahead.held)
                                  : playerHeld(d.player2);
        if (d.pair) {
            if (d.player2) {
                d.other = m_pairChosen1;
                d.otherHeld = m_pairHeld1;
            } else {
                d.otherHeld = m_probe.active ? m_probe.held2 : m_ahead.active ? m_ahead.held2 : playerHeld(true);
                if (m_planIndex < m_plan2.size()) d.other.assign(m_plan2.begin() + (std::ptrdiff_t)m_planIndex, m_plan2.end());
                const TickInput last = d.other.empty() ? TickInput{0, d.otherHeld} : d.other.back();
                while ((int)d.other.size() < d.H) d.other.push_back({0, last.held});
            }
            d.other.resize((size_t)d.H);
        }
        // How much of a plan gets played before the next decision: the
        // flying kinds drift from the simulation sooner, so less for them.
        // Clicks inside this window are the ones that count against an idea.
        PlayerObject* who = d.player2 ? pl->m_player2 : pl->m_player1;
        const bool flyingNow = m_probe.active ? (d.player2 ? m_probe.flying2 : m_probe.flying)
                               : m_ahead.active ? (d.player2 ? m_ahead.flying2 : m_ahead.flying)
                                                : (who && (who->m_isDart || who->m_isShip || who->m_isBird || who->m_isSwing));
        d.window = flyingNow ? std::max(1, std::min(d.K, (int)std::lround(120.0 * std::max(1.0, tps / 240.0)))) : d.K;
        m_prefixLength = d.K;
        m_decisionTick = d.tick;
        if (!d.player2) m_stats.decisions++;
        d.candidates.reserve(256);
        // Carrying on with the previous plan is always one of the ideas.
        const std::vector<TickInput>& previous = d.player2 ? m_plan2 : m_plan;
        // The verified rest of the last plan counts whenever this decision is
        // about the tick it starts at (after a probe's plan has been played
        // too, not only when deciding ahead), and it is kept whole: cut to
        // the horizon, a 1335-tick tail looked no better than a steer that
        // dies just past it, and its remainder was thrown away each time.
        const bool tailFits = !d.player2 && m_planTailTick == d.tick && !m_planTail.empty();
        if (tailFits || (!m_ahead.active && m_planIndex < previous.size())) {
            std::vector<TickInput> rest = tailFits ? m_planTail
                                                   : std::vector<TickInput>(previous.begin() + (std::ptrdiff_t)m_planIndex, previous.end());
            const TickInput last = rest.back();
            while ((int)rest.size() < d.H) rest.push_back({0, last.held});
            addCandidate(d.candidates, std::move(rest), "continue", d.held);
            d.candidates.back().carryOn = true;
        }
        // What got it past here in an earlier run goes first: judged like
        // any other idea, but if it still works the decision is one idea.
        d.hard = m_repair.active;
        // The same script twice is the same answer twice: over a stretch the
        // human does not touch the button in, all five shifts are the same
        // vector, and each would cost a whole-horizon run. (TickInput has no
        // operator==; comparing a dozen scripts is microseconds.)
        auto sameScript = [](const std::vector<TickInput>& a, const std::vector<TickInput>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i].presses != b[i].presses || a[i].held != b[i].held) return false;
            }
            return true;
        };
        auto seen = [&](const std::vector<TickInput>& s) {
            for (const Candidate& c : d.candidates) {
                if (c.steer == 0 && sameScript(c.inputs, s)) return true;
            }
            return false;
        };
        const bool learning = SLSettings::get()->pathfinder.learn && absense::memory::Store::get().isOpen();
        if (learning) {
            auto& mem = absense::memory::Store::get();
            const Trajectory::StartState st = decisionState(d);
            d.situation = mem.situation(st, m_repair.active);
            std::vector<size_t> known;
            mem.nearby(st, known);
            for (const size_t i : known) {
                const auto& sol = mem.level().solutions[i];
                char name[64];
                std::snprintf(name, sizeof(name), "remembered #%zu", i);
                std::vector<TickInput> expanded = absense::memory::Store::expand(sol.events, d.H, d.held);
                mem.used(i);  // looked at either way
                if (seen(expanded)) continue;  // the same idea again: one run is enough
                addCandidate(d.candidates, std::move(expanded), name, d.held);
                d.candidates.back().memoryIndex = (int)i;
                m_stats.remembered++;
            }
        }
        // The human's macro for this level, at this tick, a tick or two
        // either side as well (a different tick rate rounds the timing).
        if (!d.player2 && absense::human::Reference::get().available()) {
            const auto& human = absense::human::Reference::get();
            std::vector<TickInput> script;
            // A widened spot asks for the macro further either side too (two
            // more 240-TPS ticks each way a level, up to eight): a macro made
            // at another rate, or timed a few ticks off this route.
            std::vector<int> shifts = {0, 1, -1, 2, -2};
            {
                const int unit = std::max(1, (int)std::lround(tps / 240.0));
                for (int s = 3; s <= std::min(8, 2 + 2 * d.level); s++) {
                    shifts.push_back(s * unit);
                    shifts.push_back(-s * unit);
                }
            }
            for (const int shift : shifts) {
                // Not a break: a shift asks for the macro at another tick, so
                // the end of the macro falls past one shift while an earlier
                // one still has inputs to give.
                if (!human.script(d.tick, d.H, tps, shift, d.held, script)) continue;
                if (seen(script)) continue;  // no event fell in the window: the same script as a shift already there
                char name[32];
                if (shift == 0) std::snprintf(name, sizeof(name), "human");
                else std::snprintf(name, sizeof(name), "human %+d", shift);
                addCandidate(d.candidates, script, name, d.held);
                m_stats.humanTried++;
            }
        }
        // A stop of a repair: what was played from here into the dead end, with one of its inputs moved a few ticks.
        // Nothing else at a stop can move a jump a second before a wall (the search branches no more than 128 ticks
        // before the death it hits). Judged whole with the first ideas; an unchanged copy is the tabu's.
        if (m_repair.active && !d.pair && d.tick >= m_stats.startTick && d.tick < m_repair.from) {
            const size_t from = (size_t)(d.tick - m_stats.startTick);
            if (from < m_repair.failed.size()) {
                const int played = (int)std::min<size_t>(m_repair.failed.size() - from, (size_t)d.H);
                std::vector<TickInput> path(m_repair.failed.begin() + (std::ptrdiff_t)from,
                                            m_repair.failed.begin() + (std::ptrdiff_t)(from + (size_t)played));
                const TickInput last = path.back();
                while ((int)path.size() < d.H) path.push_back({0, last.held});
                std::vector<int> events;
                bool state = d.held;
                for (int i = 0; i < played && events.size() < 12; i++) {
                    if (path[(size_t)i].presses > 0 || path[(size_t)i].held != state) events.push_back(i);
                    state = path[(size_t)i].held;
                }
                const int unit = std::max(1, (int)std::lround(std::max(1.0, tps / 240.0)));
                char name[64];
                for (size_t e = 0; e < events.size(); e++) {
                    const int at = events[e];
                    const int prev = e > 0 ? events[e - 1] : -1;
                    int next = d.H;
                    {
                        bool s = path[(size_t)at].held;
                        for (int i = at + 1; i < played; i++) {
                            if (path[(size_t)i].presses > 0 || path[(size_t)i].held != s) {
                                next = i;
                                break;
                            }
                            s = path[(size_t)i].held;
                        }
                    }
                    const bool before = at == 0 ? d.held : path[(size_t)at - 1].held;
                    const TickInput moved = path[(size_t)at];
                    for (const int step : {-4, -2, -1, 1, 2, 4}) {
                        const int to = at + step * unit;
                        if (to < 0 || to <= prev || to >= next || to >= d.H) continue;
                        std::vector<TickInput> v = path;
                        if (to > at) {
                            for (int t = at; t < to; t++) v[(size_t)t] = TickInput{0, before};
                        } else {
                            for (int t = to + 1; t <= at; t++) v[(size_t)t] = TickInput{0, moved.held};
                        }
                        v[(size_t)to] = moved;
                        if (seen(v)) continue;  // already an idea here (the macro's, the memory's): one run is enough
                        std::snprintf(name, sizeof(name), "played, input %d moved %+d", at, to - at);
                        addCandidate(d.candidates, std::move(v), name, d.held);
                    }
                }
            }
        }
        buildSteeredCandidates(d.candidates, d.held, d.H);
        d.steersEnd = d.candidates.size();
        buildCandidates(d.candidates, d.held, d.H);
        if (learning && d.candidates.size() > d.steersEnd) {
            // The kinds of idea that usually win in this situation go first
            // (scored once each: a score builds a string and walks two maps,
            // and a comparison sort asked for two of them per comparison).
            auto& mem = absense::memory::Store::get();
            const size_t n = d.candidates.size() - d.steersEnd;
            std::vector<std::pair<uint32_t, size_t>> order(n);
            for (size_t i = 0; i < n; i++) order[i] = {mem.score(d.situation, d.candidates[d.steersEnd + i].name), i};
            std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<Candidate> sorted;
            sorted.reserve(n);
            for (const auto& [score, i] : order) sorted.push_back(std::move(d.candidates[d.steersEnd + i]));
            std::move(sorted.begin(), sorted.end(), d.candidates.begin() + (std::ptrdiff_t)d.steersEnd);
        }
        for (size_t i = d.steersEnd; i < d.candidates.size(); i++) {
            if (d.candidates[i].name == "keep") {
                d.keep = (int)i;
                break;
            }
        }
        d.stage = Decision::Steers;
    }

    const int H = d.H;
    auto better = [&](const Candidate& a, const Candidate& b) {
        if (a.complete != b.complete) return a.complete;
        // Among ideas that last the whole horizon, the verified rest of the
        // last plan beats a fresh one (which used to win on fewer clicks).
        if (a.survived >= H && b.survived >= H && a.carryOn != b.carryOn) return a.carryOn;
        if (a.survived != b.survived) return a.survived > b.survived;
        // Lasting the same ticks but ending further into the level: a speed
        // portal or a teleport carried one of them on. In whole blocks, so
        // the fraction of a tick never decides it, and 0 for both wherever
        // "further" means nothing (see reachOf).
        // ... and only between ideas that did not last the whole look: among
        // full-horizon survivors a longer reach means one of them clicked a
        // dash orb or crossed a speed portal, and fewest clicks decides those.
        if (a.survived < H && b.survived < H && a.reach != b.reach) return a.reach > b.reach;
        if (a.eventsWindow != b.eventsWindow) return a.eventsWindow < b.eventsWindow;
        if (a.events != b.events) return a.events < b.events;
        return a.firstEvent > b.firstEvent;
    };
    auto haveFull = [&]() {
        return d.best >= 0 && (d.candidates[(size_t)d.best].complete || d.candidates[(size_t)d.best].survived >= H);
    };
    // Judges the ideas [d.next, end) a slice at a time; true once they are all
    // done (or one of them settled it).
    auto judge = [&](size_t end, bool stopOnCalm) {
        while (d.next < end && mayWork()) {
            Candidate& c = d.candidates[d.next++];
            if (c.evaluated && !c.screen) continue;  // judged in full by an earlier stage (the card's ideas sit inside the ordinary ideas' range): not twice
            if (!c.inputs.empty() && isTabu(d.tick, c.inputs, d.player2)) continue;
            const int hz = c.screen && d.screenHorizon > 0 ? d.screenHorizon : H;
            // Exact shortcut: every input before an idea's first event is
            // "keep"'s own, so an idea that changes nothing up to and
            // including the tick keep dies at (or over the whole look, when
            // keep outlasts it) ends exactly as keep did. The simulation is
            // deterministic: this is the run's own answer, not a guess.
            if (!c.evaluated && c.steer == 0 && c.claimed < 0 && !c.inputs.empty() && d.keep >= 0 &&
                (int)(d.next - 1) != d.keep) {
                const Candidate& k = d.candidates[(size_t)d.keep];
                if (k.evaluated && !k.screen && !k.inputs.empty()) {
                    const int kEnd = std::min(k.survived, hz);          // keep's ticks inside this look
                    const bool kDies = !k.complete && k.survived < hz;  // ... and it dies inside it
                    const int firstMayDiffer = kDies ? kEnd + 1 : kEnd;
                    if (c.firstEvent < 0 || c.firstEvent >= firstMayDiffer) {
                        c.survived = kEnd;
                        c.complete = k.complete && k.survived <= hz;
                        c.killerStatic = kDies && k.killerStatic;
                        c.killerId = kDies ? k.killerId : 0;
                        c.killerX = k.killerX;
                        c.killerY = k.killerY;
                        c.reach = k.reach;  // the same path over this look: it ends where keep ended
                        c.evaluated = true;
                    }
                }
            }
            d.scratchTrace.clear();
            if (!evaluate(c, hz, d.held, c.screen ? nullptr : &d.scratchTrace)) break;
            if (c.steer > 0 && isTabu(d.tick, c.inputs, d.player2)) continue;
            if (c.inputs.empty()) continue;  // a steer that found nothing: not an idea
            if (c.screen) continue;  // only a first look: it does not count as an idea yet
            d.tried.push_back(d.next - 1);
            if (d.best < 0 || better(c, d.candidates[(size_t)d.best])) d.best = (int)d.next - 1;
            if (d.best == (int)d.next - 1 && !d.scratchTrace.empty()) {
                // The winner's own samples: the commit takes its trace from
                // here instead of running the same script a third time.
                d.bestTrace.swap(d.scratchTrace);
                d.bestTraceFor = d.best;
                d.bestTraceTick = currentTick();
            }
            if (c.complete) return true;  // reaching the end beats everything
            // The verified rest of the last plan still lasting the whole
            // horizon, and a tick late as well: that settles it in one run,
            // and the steers are not simulated at all.
            if (c.carryOn && c.survived >= H && robust(c, H, d.held, H)) {
                d.slackChosen = (int)d.next - 1;  // already checked a tick late: the verdict need not run it again
                d.slackDone = true;
                return true;
            }
            if (c.survived >= H) {
                d.fullSurvivors++;
                // Once something calm survives the whole horizon, or enough
                // ideas do, the rest is not needed.
                if (stopOnCalm && (c.events == 0 || d.fullSurvivors >= 6)) return true;
            }
        }
        return d.next >= end;
    };

    // 1. Carrying on and the steered ideas: cheap, and enough most of the time.
    if (d.stage == Decision::Steers) {
        if (!judge(d.steersEnd, false)) return;  // more next time
        d.stage = haveFull() ? Decision::Verdict : Decision::Gpu;
    }

    // 2. Nothing cheap lasts: ask the graphics card, which tries tens of
    // thousands of ideas at once. Its best are run here against a short
    // piece of the horizon, and the best of those over the whole of it.
    if (d.stage == Decision::Gpu) {
        if (d.gpuEnd == 0) {
            d.hard = true;
            const size_t before = d.candidates.size();
            const int round = buildGpuCandidates(d);
            if (round < 0) return;  // another round of it next slice
            const size_t added = (size_t)round;
            d.gpuEnd = before + added;
            if (added == 0) {
                d.stage = Decision::Rest;  // no card: the cheap ideas, then the search
            } else {
                // The card's ideas sit at the end of the list; the ordinary
                // ones in the middle have not been judged yet, so where they
                // were up to is kept and put back afterwards. (Leaving the
                // cursor at the end is what made the whole "rest" stage -
                // every tap, hold, double click and orb spam - be skipped
                // whenever the card was on.)
                d.gpuResume = d.next;
                d.next = before;
                d.screenHorizon = screenTicks(d.H);
            }
        }
        if (d.stage == Decision::Gpu) {
            if (!judge(d.gpuEnd, false)) return;  // more next time
            // The ones that got through the short run, over the whole horizon.
            std::vector<size_t> best;
            for (size_t i = d.steersEnd; i < d.gpuEnd; i++) {
                if (d.candidates[i].screen && d.candidates[i].evaluated) best.push_back(i);
            }
            // Dozens of the card's ideas reach the end of the short look and
            // tie there; without a second and a third key which six go on to
            // the whole horizon is whatever the sort happens to leave.
            const int look = d.screenHorizon;
            std::sort(best.begin(), best.end(), [&](size_t a, size_t b) {
                const Candidate& ca = d.candidates[a];
                const Candidate& cb = d.candidates[b];
                if (ca.survived != cb.survived) return ca.survived > cb.survived;
                // As in better(): among ideas that last the whole look a longer
                // reach means one of them fired a dash orb or crossed a speed
                // portal, and fewest clicks decides those. (Every one of these
                // really was run over the look - keep is not judged until the
                // Rest stage, so the exact shortcut cannot fire here - so the
                // reaches below the look are comparable.)
                if (ca.survived < look && cb.survived < look && ca.reach != cb.reach) return ca.reach > cb.reach;
                // Tied over the look: how far the card saw each one go over the whole horizon.
                // Only between two it scored - an unscored idea is not a worse one.
                if (ca.card >= 0 && cb.card >= 0 && ca.card != cb.card) return ca.card > cb.card;
                return ca.eventsWindow < cb.eventsWindow;
            });
            // Fewest clicks breaks the verdict's ties; it is no guess at what lasts
            // past the look. The ideas that last the whole look take turns by kind
            // of click (one change / two / a few / many), calmest first in each, or
            // the card's longer chains of inputs never got a whole-horizon run.
            {
                size_t tied = 0;
                while (tied < best.size() && d.candidates[best[tied]].survived >= look) tied++;
                std::vector<size_t> byClass[4];
                for (size_t k = 0; k < tied; k++) {
                    const int e = d.candidates[best[k]].eventsWindow;
                    byClass[e <= 1 ? 0 : e <= 2 ? 1 : e <= 8 ? 2 : 3].push_back(best[k]);
                }
                size_t out = 0;
                for (size_t round = 0; out < tied; round++)
                    for (auto& cls : byClass)
                        if (round < cls.size()) best[out++] = cls[round];
            }
            const size_t full = std::min<size_t>(best.size(), 6u * (size_t)(1 + std::min(d.level, 3)));  // more at a widened spot
            const size_t at = d.candidates.size();
            for (size_t k = 0; k < full; k++) {
                Candidate again = d.candidates[best[k]];
                again.screen = false;
                again.evaluated = false;
                again.survived = 0;
                again.claimed = -1;
                d.candidates.push_back(std::move(again));
            }
            for (const size_t i : best) d.candidates[i].screen = false;  // the first look is over: not a screen for the Rest stage either
            d.screenHorizon = 0;
            d.gpuEnd = d.candidates.size();
            d.next = at;
            d.stage = Decision::GpuFull;
        }
    }
    if (d.stage == Decision::GpuFull) {
        if (!judge(d.gpuEnd, true)) return;  // more next time
        d.next = d.gpuResume;  // back to the ordinary ideas
        d.stage = haveFull() ? Decision::Verdict : Decision::Rest;  // the hundreds of cheap ideas before the one dear search
    }

    // 2. Nothing cheap lasts: the branching search, in slices.
    if (d.stage == Decision::Search) {
        if (!d.searching) {
            const int effort = std::clamp(SLSettings::get()->pathfinder.effort, 0, 2);
            // The search walks the last plan's verified tail first (when this
            // decision is about where that plan ends) and branches only
            // where it runs out, instead of finding the same way again.
            std::span<const TickInput> base;
            if (!d.player2 && m_planTailTick == d.tick && !m_planTail.empty()) base = m_planTail;
            // At the stops of a repair the search is the last thing tried at
            // every one of many stops: a third of the budget there, so a stop
            // costs a fifth of a second instead of most of one.
            // The budget is simulated ticks, but the horizon it must cover is
            // seconds: at 1000 TPS one run of the horizon is 4000 ticks, so
            // 160000 bought about 40 of them where at 240 TPS it bought 166.
            // Scaled with the tick rate, a search covers the same play at any rate.
            // ... but the horizon is a count of ticks too, and the plain one is
            // capped at kMaxHorizon: at 2000 TPS a run is 4000 ticks, not the
            // 8000 the tick rate asks for, so scaling the budget by the rate as
            // well bought 333 whole-horizon runs where 240 TPS bought 166 - the
            // two scalings multiplied. The scale is whichever is smaller, the
            // rate or how much longer a run really is, so a search covers the
            // same play at any rate and never twice as much.
            const int refHorizon = std::max(1, (int)std::lround(std::max(kLookahead, kMinLookahead) * 240.0));
            const double budgetScale =
                std::max(1.0, std::min(bot->updater().getTps() / 240.0, (double)H / (double)refHorizon));
            const int fullBudget = (int)std::lround(kSearchBudget[effort] * budgetScale);
            // ... but not at the floor. The last stop is the only one whose
            // answer decides between "try something anyway" and giving up, and
            // a third of the budget there is a third of a search: in the orb
            // corridor the first forward decision at tick 867 spent 3.0 s and
            // found an idea lasting 1035 ticks, while the stop at that same
            // tick 867 spent 1.08 s, found one that did not even clear the
            // margin, and so skipped the "trying anyway" branch - four passes
            // over, then it gave up.
            const bool atFloorStop = m_repair.active && d.tick <= m_repair.floor;
            // A stop more than two margins before the death is where the approach can still change: the whole budget.
            const bool farStop = m_repair.active && m_repair.deathTick > d.tick + 2 * (uint64_t)d.margin;
            // A widened spot (see escalate): the whole budget at every stop, and a
            // bigger one a level (up to four times), with every tick a branch point
            // further before each death (see beginSearch). The search is
            // deterministic: the same stop on the same budget is the same answer.
            const int widened =
                (int)std::min<int64_t>((int64_t)fullBudget * (int64_t)(1 + std::min(d.level, 3)), (int64_t)INT32_MAX);
            const int budget = d.level > 0 ? widened
                               : m_repair.active && !atFloorStop && !farStop
                                   ? std::max((int)std::lround(kSearchBudget[0] * budgetScale), fullBudget / 3)
                                   : fullBudget;
            // The same search from the same kept start finds the same script (see memoStart).
            const Trajectory::SimStart* memoFrom = memoStart();
            d.searchMemoFrom = memoFrom;
            d.searchKey = memoFrom ? memoKey(memoFrom, H, d.held, base,
                                             kSearchSalt + ((uint64_t)(uint32_t)budget << 32) + (uint64_t)std::min(d.level, 3))
                                   : std::pair<uint64_t, uint64_t>{0, 0};
            const auto memo = memoFrom ? m_runMemo.find(d.searchKey.first) : m_runMemo.end();
            if (memo != m_runMemo.end() && memo->second.check == d.searchKey.second) {
                RunResult r;
                r.survived = memo->second.survived;
                r.complete = memo->second.complete;
                r.died = memo->second.died;
                r.dualDeath = memo->second.dualDeath;
                d.restEnd = d.candidates.size();
                addSearchCandidate(d.candidates, d.held, H, r, memo->second.script);  // (a copy: evaluate may rehash the memo)
                if (d.candidates.size() > d.restEnd) {
                    Candidate& c = d.candidates.back();
                    d.scratchTrace.clear();
                    if (!isTabu(d.tick, c.inputs, d.player2) && evaluate(c, H, d.held, &d.scratchTrace)) {
                        d.tried.push_back(d.candidates.size() - 1);
                        if (d.best < 0 || better(c, d.candidates[(size_t)d.best])) d.best = (int)d.candidates.size() - 1;
                        if (d.best == (int)d.candidates.size() - 1 && !d.scratchTrace.empty()) {
                            d.bestTrace.swap(d.scratchTrace);
                            d.bestTraceFor = d.best;
                            d.bestTraceTick = currentTick();
                        }
                    }
                }
                d.stage = Decision::Verdict;
            } else {
                const bool began = d.pair ? bot->trajectory().beginSearch(pl, !d.player2, H, d.held, budget, d.other,
                                                                          d.otherHeld, {}, d.level)
                                          : bot->trajectory().beginSearch(pl, true, H, d.held, budget, {}, false, base, d.level);
                if (began) {
                    d.searching = true;
                    m_stats.simulations++;
                } else {
                    d.stage = Decision::Verdict;
                }
            }
        }
        if (d.searching) {
            // One call per slice: called kSearchStep ticks at a time the search was
            // paused and resumed every 200 simulated ticks - a snapshot, a restore,
            // the moving objects put back and carried out again, and the whole game
            // state moved out and deep-copied back in.
            const bool over = bot->trajectory().stepSearch(kSearchStep, m_deadline);
            if (!over) return;  // more next time
            std::vector<TickInput> script;
            const RunResult r = bot->trajectory().finishSearch(script);
            d.searching = false;
            if (d.searchMemoFrom && d.searchMemoFrom == memoStart()) {
                if (m_runMemo.size() >= kMaxRunMemo) m_runMemo.clear();
                RunMemo& m = m_runMemo[d.searchKey.first];
                m = RunMemo{};
                m.check = d.searchKey.second;
                m.script = script;
                m.survived = r.survived;
                m.complete = r.complete;
                m.died = r.died;
                m.dualDeath = r.dualDeath;
            }
            d.restEnd = d.candidates.size();
            addSearchCandidate(d.candidates, d.held, H, r, std::move(script));
            if (d.candidates.size() > d.restEnd) {
                Candidate& c = d.candidates.back();
                d.scratchTrace.clear();
                if (!isTabu(d.tick, c.inputs, d.player2) && evaluate(c, H, d.held, &d.scratchTrace)) {
                    d.tried.push_back(d.candidates.size() - 1);
                    if (d.best < 0 || better(c, d.candidates[(size_t)d.best])) d.best = (int)d.candidates.size() - 1;
                    if (d.best == (int)d.candidates.size() - 1 && !d.scratchTrace.empty()) {
                        d.bestTrace.swap(d.scratchTrace);
                        d.bestTraceFor = d.best;
                        d.bestTraceTick = currentTick();
                    }
                }
            }
            d.stage = Decision::Verdict;
        }
    }

    // 3. Still nothing: the single-change and random ideas (orbs, double
    // clicks, spam) - the search does not try those shapes. Before the
    // search: hundreds of them cost what the search alone costs, and they
    // are what the "ideas" counter counts.
    if (d.stage == Decision::Rest) {
        if (d.restEnd == 0) {
            // Hundreds of ideas, most of which die early: all of them get a
            // look over a fifth of the horizon first (an idea that dies in
            // it would die at the same tick over the whole of it), and only
            // the best few dozen are run over the whole horizon. The bulk of
            // a decision's time was ideas that lasted 900 ticks and lost.
            d.restEnd = d.candidates.size();
            d.restScreenStart = d.next;
            // "keep" first, over the whole horizon: one run, and every idea
            // that does nothing before the tick keep dies at is keep over
            // that stretch and ends the same way (see judge).
            if (d.keep >= (int)d.next && d.keep < (int)d.restEnd && d.keep != (int)d.next) {
                std::swap(d.candidates[d.next], d.candidates[(size_t)d.keep]);
                d.keep = (int)d.next;
            }
            for (size_t i = d.next; i < d.restEnd; i++) {
                if (!d.candidates[i].evaluated && (int)i != d.keep) d.candidates[i].screen = true;
            }
            d.screenHorizon = screenTicks(d.H);
        }
        if (d.screenHorizon > 0) {
            if (!judge(d.restEnd, false)) return;  // more next time
            std::vector<size_t> best;
            for (size_t i = d.restScreenStart; i < d.restEnd; i++) {
                if (d.candidates[i].screen && d.candidates[i].evaluated) best.push_back(i);
            }
            // Hundreds of ideas reach the end of the cheap look and tie
            // there - more of them since the look stopped widening with a
            // repair's horizon - and without a second and a third key which
            // thirty-two go on to the whole horizon is whatever the sort
            // happens to leave.
            // The look only tries the ideas that act inside it. An idea whose
            // first event falls past it is "keep" over the look and takes
            // keep's outcome without being run (the shortcut in judge): it
            // lasted the whole look because keep did, and the reach it was
            // handed is keep's own, taken after up to H ticks - sixteen blocks
            // against the two a screened idea covers in 500 at 2000 TPS. So it
            // beat every idea really run over the look on the reach key, and
            // then on the clicks key as well (it presses nothing inside the
            // window), and all thirty-two whole-horizon places went to scripts
            // that are keep over every tick that gets played. The two kinds are
            // ranked apart and share the places.
            const int look = d.screenHorizon;
            // Hundreds of them reach the end of the look and tie there, and
            // nothing the look measured says which of them lasts past it. One
            // batch on the card runs every tied idea over the whole horizon,
            // as the per-tick script it is, and what comes back breaks those
            // ties below. It approves nothing: every idea still gets its exact
            // run, and with the card off, broken or slow the keys decide
            // exactly as they did before.
            {
                std::vector<size_t> tied;
                for (const size_t i : best) {
                    const Candidate& c = d.candidates[i];
                    if (c.survived >= look && c.card < 0 && !c.inputs.empty()) tied.push_back(i);
                }
                // Only when there are more of them than whole-horizon places
                // to give out - otherwise the order changes nothing and the
                // batch is a round trip for nothing. All of them go in one
                // batch or none of them does (see scoreOnCard): the sorts
                // below compare two ideas by what the card made of them only
                // where it saw both, and a set where it saw some of them is no
                // order for a std::sort.
                if (tied.size() > 32u * (size_t)(1 + std::min(d.level, 3))) scoreOnCard(d, tied);
            }
            // An idea that changes nothing up to and including the tick keep dies at over the
            // whole horizon is keep over the whole horizon too (the full run takes keep's
            // outcome by the same shortcut): a place given to it runs nothing.
            int keepSame = H;
            if (d.keep >= 0) {
                const Candidate& k = d.candidates[(size_t)d.keep];
                if (k.evaluated && !k.screen && !k.inputs.empty() && !k.complete && k.survived < H) keepSame = k.survived + 1;
            }
            std::vector<size_t> acts, later;
            for (const size_t i : best) {
                const Candidate& c = d.candidates[i];
                if (c.firstEvent < 0 || c.firstEvent >= keepSame) continue;
                ((c.firstEvent >= 0 && c.firstEvent < look) ? acts : later).push_back(i);
            }
            std::sort(acts.begin(), acts.end(), [&](size_t a, size_t b) {
                const Candidate& ca = d.candidates[a];
                const Candidate& cb = d.candidates[b];
                if (ca.survived != cb.survived) return ca.survived > cb.survived;
                // As in better(): among ideas that last the whole look a longer
                // reach means a dash orb or a speed portal, and fewest clicks
                // decides those.
                if (ca.survived < look && cb.survived < look && ca.reach != cb.reach) return ca.reach > cb.reach;
                // Tied over the look: the card ran the same script over the whole horizon
                // (only where it ran both of them; an unscored idea is not a worse one).
                if (ca.card >= 0 && cb.card >= 0 && ca.card != cb.card) return ca.card > cb.card;
                return ca.eventsWindow < cb.eventsWindow;
            });
            // Fewest clicks breaks the verdict's ties; it is no guess at what lasts past the look.
            // With the death beyond the look hundreds of ideas tie there, and in click order every
            // double, hold, spam and multi-press random sat behind the single changes. Ideas that
            // last the whole look take turns by kind of click (one change / two / a few / many),
            // calmest first within each kind.
            {
                size_t tied = 0;
                while (tied < acts.size() && d.candidates[acts[tied]].survived >= look) tied++;
                std::vector<size_t> byClass[4];
                for (size_t k = 0; k < tied; k++) {
                    const int e = d.candidates[acts[k]].eventsWindow;
                    byClass[e <= 1 ? 0 : e <= 2 ? 1 : e <= 8 ? 2 : 3].push_back(acts[k]);
                }
                size_t out = 0;
                for (size_t round = 0; out < tied; round++)
                    for (auto& cls : byClass)
                        if (round < cls.size()) acts[out++] = cls[round];
            }
            // Nothing the look measured says anything about these - they all
            // carry keep's own figures - so the one that acts soonest after it,
            // and so soonest before the tick keep dies at, goes first.
            std::sort(later.begin(), later.end(), [&](size_t a, size_t b) {
                const Candidate& ca = d.candidates[a];
                const Candidate& cb = d.candidates[b];
                if (ca.survived != cb.survived) return ca.survived > cb.survived;
                // The card ran these past the look, where they differ (and where it ran both).
                if (ca.card >= 0 && cb.card >= 0 && ca.card != cb.card) return ca.card > cb.card;
                const int fa = ca.firstEvent < 0 ? H : ca.firstEvent;
                const int fb = cb.firstEvent < 0 ? H : cb.firstEvent;
                if (fa != fb) return fa < fb;
                return ca.eventsWindow < cb.eventsWindow;
            });
            best.clear();
            for (size_t a = 0, b = 0; a < acts.size() || b < later.size();) {
                if (a >= acts.size()) {
                    best.push_back(later[b++]);
                    continue;
                }
                if (b >= later.size()) {
                    best.push_back(acts[a++]);
                    continue;
                }
                // Lasting longer still decides; where they lasted the same, one
                // of each in turn, so neither kind shuts the other out of the
                // whole horizon.
                const int sa = d.candidates[acts[a]].survived;
                const int sb = d.candidates[later[b]].survived;
                if (sa > sb) {
                    best.push_back(acts[a++]);
                } else if (sb > sa) {
                    best.push_back(later[b++]);
                } else {
                    best.push_back(acts[a++]);
                    best.push_back(later[b++]);
                }
            }
            const size_t full = std::min<size_t>(best.size(), 32u * (size_t)(1 + std::min(d.level, 3)));  // more at a widened spot
            const size_t at = d.candidates.size();
            for (size_t k = 0; k < full; k++) {
                Candidate again = d.candidates[best[k]];
                again.screen = false;
                again.evaluated = false;
                again.survived = 0;
                again.claimed = -1;
                d.candidates.push_back(std::move(again));
            }
            d.screenHorizon = 0;
            d.next = at;
            d.restEnd = d.candidates.size();
        }
        if (!judge(d.restEnd, true)) return;  // more next time
        d.stage = haveFull() ? Decision::Verdict : Decision::Search;
    }

    // ---- the verdict: which idea, and whether it is any good
    if (d.stage == Decision::Verdict) {
        const uint64_t tick = d.tick;
        const bool held = d.held;
        const int margin = d.margin;
        std::vector<Candidate>& candidates = d.candidates;
        const Candidate* best = d.best >= 0 ? &candidates[(size_t)d.best] : nullptr;

        // No idea at all, or none that lasts even a tick (a plan the game did
        // not follow left the player somewhere already lost): a dead end at
        // this very tick, and the way on is looked for from before it. Not
        // something to play.
        if (!best || best->inputs.empty() || (!best->complete && best->survived <= 1 && !m_repair.active)) {
            if (m_repair.active && tick <= m_repair.floor) {
                // Nothing at all to try at the floor (every idea there is a dead
                // end already remembered): walking back to this same floor asked
                // the same question for ever. The spot widens instead.
                log("tick %llu: nothing to try here (%zu ideas tried) - the floor", (unsigned long long)tick, d.tried.size());
                escalate("every stop down to the floor failed");
                return;
            }
            d.active = false;
            m_pairSecondPass = false;
            m_stats.deadEnds++;
            m_deadEndAt = tick;
            m_deadEndReal = false;
            m_deadEndX = decisionState(d).x;
            // Its kept start goes with it: without this a stop with no kept start
            // after it was gone back to for real with the old one still in use.
            if (m_probe.active) endProbe();
            log("dead end at tick %llu: nothing lasts even a tick from here (%zu ideas tried); going back",
                (unsigned long long)tick, d.tried.size());
            if (!m_ahead.active) {
                // Decided ahead: the plan (verified that far) plays out and the
                // decision at its end, from the real state, sees this dead end
                // (see tick()). Clearing it here also left m_planIndex at 0 for
                // the pending live tick's afterTick(), which indexes the trace
                // by m_planIndex - 1.
                m_plan.clear();
                m_plan2.clear();
                m_planIndex = 0;
            }
            m_phase = Phase::Backtracking;
            return;
        }

        // The real game is never quite the simulation: of the best ideas, take
        // the first that also works with every input a tick late. A plan that
        // only works to the tick dies for real more often than not. (A run
        // per slice: at a high tick rate one run is most of a frame.)
        // A stop of a repair whose best idea is under the bar goes one stop further back
        // whatever the check finds (none lasts longer than the best): running its ties a
        // tick late only renames the idea. The floor still runs it - it picks the try there.
        if (best && !best->complete && !d.slackDone && m_repair.active && tick > m_repair.floor && best->survived < d.need) {
            d.slackChosen = d.best;
            d.slackDone = true;
        }
        if (best && !best->complete && !d.slackDone) {
            if (d.slackOrder.empty() && d.slackAt == 0 && d.slackChecked == 0) {
                d.slackOrder = d.tried;
                std::sort(d.slackOrder.begin(), d.slackOrder.end(),
                          [&](size_t a, size_t b) { return better(candidates[a], candidates[b]); });
                // Never below the bar this verdict is about to apply (d.need), and
                // never further than the best idea itself - a bar of margin/2 ticks
                // less could sit under d.need and hand the verdict an idea that does
                // not get past the spot being repaired. Between the two the check
                // still does its job: an idea within half a margin of the best that
                // survives every input a tick late is preferred to a frame-perfect one.
                d.slackNeeded = best->survived >= H
                                    ? H
                                    : std::min(best->survived,
                                               std::max(std::max(1, d.need), best->survived - std::max(1, margin / 2)));
                d.slackChosen = -1;
            }
            while (d.slackAt < d.slackOrder.size() && d.slackChecked < 6 && mayWork()) {
                const size_t idx = d.slackOrder[d.slackAt];
                const Candidate& c = candidates[idx];
                if (c.survived < d.slackNeeded) {  // the rest are worse still
                    d.slackAt = d.slackOrder.size();
                    break;
                }
                d.slackChecked++;
                if (robust(c, H, held, d.slackNeeded)) {
                    d.slackChosen = (int)idx;
                    d.slackAt = d.slackOrder.size();
                    break;
                }
                d.slackAt++;
            }
            if (d.slackAt < d.slackOrder.size() && d.slackChecked < 6) return;  // more next time
            d.slackDone = true;
        }
        if (best && !best->complete) {
            if (d.slackChosen >= 0 && d.slackChosen != d.best) {
                const Candidate* chosen = &candidates[(size_t)d.slackChosen];
                log("tick %llu: %s only works to the tick (lasts %d), taking %s (lasts %d) instead; %d needed here",
                    (unsigned long long)tick, best->name.c_str(), best->survived, chosen->name.c_str(), chosen->survived,
                    d.need);
                best = chosen;
            } else if (d.slackChosen < 0) {
                m_stats.fragile++;
            }
        }

        // While repairing a dead end an idea only counts when it gets past the
        // spot the search died at; otherwise it would die there again (a spot
        // beyond the horizon cannot be seen from here: then the whole horizon
        // is asked for).
        // Settled when the decision was set up, from the horizon and the
        // margin it was set up with (the repaired death stays the bar after
        // the resume too: an idea that does not get past it is not viable,
        // however long it lasts).
        const int need = d.need;
        const bool atFloor = m_repair.active && tick <= m_repair.floor;
        // Clearing the bar is enough. Asking it to last past the margin as
        // well threw the best thing the search ever found away as a dead end
        // (an idea lasting exactly 500 ticks where the margin was 500); the
        // plan of one tick that was the worry is dealt with where the commit
        // length is worked out, not here.
        const bool viable = best && (best->complete || best->survived >= need);
        // Playing an idea the simulation says dies: up to just before it does.
        int forcedCommit = -1;
        m_planKnownDeath = UINT64_MAX;

        if (!viable) {
            if (m_repair.active) {
                // A killer the game confirmed is final. One the simulation only
                // calls static is final until the spot has widened once: from
                // then on the game is asked at the floor too (kRetriesPerStop a
                // level), because a static killer is exactly where a slightly-off
                // simulation goes unnoticed - the human macro that finishes level
                // 88203501 dies in it at tick 562, and no stop was ever let past.
                const bool certainDeath =
                    best && ((best->killerId != 0 && m_confirmedKillers.count(best->killerUid) != 0) ||
                             (best->killerStatic && d.level == 0));
                // Playing the floor's best idea over and over is a loop that
                // ends nowhere (one log has four whole-level resets in
                // fourteen seconds, all from the same stop). Only so many
                // times per spot, and played into the tick the simulation sees
                // it die in - nothing decided ahead of that - so the game's
                // word can confirm the killer; once they are spent the spot
                // widens (see escalate).
                const double tpsNow = bot->updater().getTps();
                Trap* floorTrap = trapAt(m_repair.deathTick, (uint64_t)std::lround(32.0 * std::max(1.0, tpsNow / 240.0)));
                if (atFloor && best && best->survived > margin && !certainDeath &&
                    (!floorTrap || floorTrap->anyway < kRetriesPerStop)) {
                    // As far back as the limit allows and still no idea that
                    // clearly gets past: play the best one anyway (the real game
                    // does not always agree with the simulation) - it is a
                    // different idea from the ones that failed, thanks to the
                    // memory of dead ends.
                    // Two-player decides this tick twice (once per player): the
                    // try is counted once.
                    if (floorTrap && (!d.pair || d.player2)) floorTrap->anyway++;
                    // Into the death tick, not up to just before it: a plan
                    // that stops a tick short never runs the tick the game
                    // would have disagreed about, so the killer is never
                    // confirmed and the try is spent for nothing.
                    forcedCommit = best->survived + 1;
                    m_planKnownDeath = tick + (uint64_t)std::max(0, best->survived);
                    log("tick %llu is the limit going back; trying %s anyway (lasts %d, needed %d)", (unsigned long long)tick,
                        best->name.c_str(), best->survived, need);
                } else if (atFloor) {
                    // Every stop down to the floor failed. Not the end of the
                    // search: the spot widens and the stops are walked again
                    // (escalate logs what changed). Walking them again as they
                    // were gave the same answer at every stop, four times over,
                    // and then "AbsensePathfinder stopped".
                    log("tick %llu: nothing here either (best %s lasts %d, needed %d) - the floor",
                        (unsigned long long)tick, best ? best->name.c_str() : "-", best ? best->survived : -1, need);
                    escalate("every stop down to the floor failed");
                    return;
                } else {
                    // Nothing here: one stop further back. Logged, or the stop
                    // leaves no trace at all: thirteen of the fifteen stops of
                    // every pass of the orb corridor went through here, so 52
                    // of that run's 67 decisions were invisible and a stop that
                    // nearly made it read exactly like one that died at once.
                    log("tick %llu: nothing here (best %s lasts %d, needed %d); one stop further back",
                        (unsigned long long)tick, best ? best->name.c_str() : "-", best ? best->survived : -1, need);
                    d.active = false;
                    m_pairSecondPass = false;
                    m_plan.clear();
                    m_plan2.clear();
                    m_planIndex = 0;
                    if (m_probe.active) endProbe();
                    m_phase = Phase::Backtracking;
                    return;
                }
            } else if (!best) {
                d.active = false;
                m_pairSecondPass = false;
                m_stats.deadEnds++;
                log("dead end at tick %llu: nothing to try", (unsigned long long)tick);
                if (!m_ahead.active) {  // decided ahead: the plan plays out (see tick())
                    m_plan.clear();
                    m_plan2.clear();
                    m_planIndex = 0;
                }
                m_deadEndAt = tick;
                m_deadEndReal = false;
                m_deadEndX = decisionState(d).x;
                m_phase = Phase::Backtracking;
                return;
            } else if ((best->killerId == 0 || best->killerStatic ||
                        m_confirmedKillers.count(best->killerUid) != 0) &&
                       best->survived > std::max(8, margin / 8)) {
                // Nothing the simulation can name killed it, or what did
                // cannot move - and there is a stretch of safe ticks before
                // the end worth playing at all. A tight corridor is exactly
                // this: every idea finishes on a spike sooner or later, and
                // the best of them still carries the player some way down it.
                // Play the safe part and think again from there, where the
                // look-ahead reaches further along - the player never gets
                // near the spike, because the plan stops short of it and the
                // next decision is made in that gap.
                // The killer used to have to be one the simulation could name
                // and call static; on a level of plain blocks both the game
                // and the simulation destroy the player with no object at all
                // ("P1 hit object 0 at (0.00, 0.00)" in the log), so killerId
                // was 0, killerStatic was false, and this branch could never
                // run: every decision in the orb corridor fell through to
                // "play into the death and let the game speak" and really
                // died there - ticks 1589, 2016 and 2030, three deaths in a
                // hundred milliseconds, and nothing learned from any of them
                // because there was no killer id to confirm. A killer the
                // simulation does name and calls movable still goes the other
                // way: that is the one case where the game is likely to
                // disagree, and it is worth the death to find out.
                // Calling this a dead end is what stopped the UFO corridors:
                // nothing in one ever lasts the half second the bar asks for,
                // so every decision in one went back instead of forward.
                // Short of it by a third of the safe run, and never by more
                // than half a margin, so every decision still plays most of
                // what it found and the last one before the spike stops a few
                // ticks away, where the repair has the whole search to find
                // the way past.
                forcedCommit = std::max(1, best->survived - std::max(1, std::min(margin / 2, best->survived / 3)));
                log("tick %llu: nothing gets past the death at tick %llu (object %d at (%.0f, %.0f)) - so %s is played to %d ticks short of it",
                    (unsigned long long)tick, (unsigned long long)(tick + (uint64_t)std::max(0, best->survived)),
                    best->killerId, best->killerX, best->killerY, best->name.c_str(), best->survived - forcedCommit);
            } else if (best->killerStatic || (best->killerId != 0 && m_confirmedKillers.count(best->killerUid) != 0)) {
                // The simulation sees no way past the spot ahead, the object
                // it dies at cannot move (or the game already confirmed a
                // death at it), and there is no safe stretch to play first:
                // it is right, and playing into it would only kill the
                // player. A dead end here; the way on
                // is looked for from the ticks before it.
                d.active = false;
                m_pairSecondPass = false;
                m_stats.deadEnds++;
                m_deadEndAt = tick + (uint64_t)std::max(0, best->survived);
                m_deadEndReal = false;
                m_deadEndX = best->killerX;
                log("dead end at tick %llu: nothing gets past object %d at (%.0f, %.0f) - the death at tick %llu - and it cannot move; going back",
                    (unsigned long long)tick, best->killerId, best->killerX, best->killerY, (unsigned long long)m_deadEndAt);
                if (!m_ahead.active) {  // decided ahead: the plan plays out (see tick())
                    m_plan.clear();
                    m_plan2.clear();
                    m_planIndex = 0;
                }
                m_phase = Phase::Backtracking;
                return;
            } else {
                // The simulation sees no way past the spot ahead. It can be
                // wrong about that (a block that moves out of the way, a
                // trigger it does not know): the best idea is played into the
                // tick it dies in, and the real game has the last word. A real
                // death there starts the repair, from the real spot; the way
                // on is then looked for from the ticks before it.
                // Into the death tick, not up to just before it: stopping a
                // tick short had the decision at the plan's end find that
                // nothing lasts even a tick and declare a dead end of its own,
                // so the game never spoke and no killer was ever confirmed.
                forcedCommit = best->survived + 1;
                m_planKnownDeath = tick + (uint64_t)std::max(0, best->survived);
                log("tick %llu: the simulation sees no way past tick %llu; playing %s into it to see what the game says",
                    (unsigned long long)tick, (unsigned long long)m_planKnownDeath, best->name.c_str());
            }
        }

        // The kept samples belong to whichever idea was the best while the
        // ideas were judged; the slack check may have taken another one.
        if (d.bestTraceFor != (int)(best - candidates.data())) d.bestTraceTick = UINT64_MAX;
        d.viable = viable;
        d.chosen = *best;
        d.chosenRaw = best->inputs;  // before the trim: what a probe at this tick would generate again
        d.trimFrom = best->survived;
        d.trimComplete = best->complete;
        d.forcedCommit = forcedCommit;
        d.trimTries = 0;
        d.trimMax = 0;
        d.trimAt = 0;
        d.trimVariant = 0;
        d.trimmed = 0;
        d.stage = Decision::Trim;
    }

    // ---- the same path with the clicks it does not need taken out (a wave
    // that zigzags instead of spamming, a cube that does not tap twice). In
    // slices of its own: before, it ran in whatever was left of the frame the
    // verdict fell in - nothing, most of the time, since the ideas are judged
    // right up to the deadline - so plans went out with every click in them.
    if (d.stage == Decision::Trim) {
        if (!trimStep(d)) return;  // more next time
        if (d.trimmed > 0) m_stats.clicksDropped += (uint64_t)d.trimmed;
    }
    // Only a decision that has been through the verdict and the trim is
    // committed. A stage that hands on to one whose block is above it in
    // this function (the ordinary ideas to the search) is picked up by the
    // next call; falling through here committed an empty idea.
    if (d.stage != Decision::Trim) return;

    // ---- two-player mode: player 1 decided, now player 2 (with this choice fixed)
    if (d.pair && !d.player2) {
        m_pairChosen1 = d.chosen.inputs;
        m_pairHeld1 = d.held;
        m_pairSurvived1 = d.chosen.survived;
        m_pairComplete1 = d.chosen.complete;
        m_pairForced1 = d.forcedCommit;
        m_pairKnownDeath1 = m_planKnownDeath;
        m_pairName1 = d.chosen.name;
        if (d.trimmed > 0) {}  // already counted
        d.active = false;
        m_pairSecondPass = true;
        return;  // the next slice starts player 2's decision
    }

    // ---- commit
    const uint64_t tick = d.tick;
    const bool held = d.pair ? m_pairHeld1 : d.held;  // player 1's button
    const int K = d.K;
    const int margin = d.margin;
    const double tps = bot->updater().getTps();
    const Candidate* best = &d.chosen;
    int forcedCommit = d.forcedCommit;
    const int dropped = d.trimmed;
    const size_t triedCount = d.tried.size();
    const size_t ideaCount = d.candidates.size();
    const bool pair = d.pair;
    d.active = false;  // the decision is made
    m_pairSecondPass = false;

    // Made whether or not the memory is on: confirmLessons only touches the
    // memory when it is, and the human macro's wins are counted from these.
    // A plan the simulation already knows dies (a forced commit, or the best
    // idea played anyway at the floor) is not a way past anything, so there
    // is nothing to learn from it.
    if (!pair && d.viable && forcedCommit <= 0) {
        const bool learning = SLSettings::get()->pathfinder.learn && absense::memory::Store::get().isOpen();
        // Learned only once the real game has got as far as this idea was
        // judged to last (see confirmLessons).
        Lesson lesson;
        lesson.decidedAt = tick;
        lesson.confirmAt = tick + (uint64_t)std::max(1, std::min(best->survived, d.H));
        lesson.situation = d.situation;
        lesson.name = best->name;
        lesson.memoryIndex = best->memoryIndex;
        if (learning && best->memoryIndex < 0 && d.hard && best->survived > 0) {
            lesson.state = decisionState(d);
            lesson.inputs = best->inputs;
            lesson.lasted = best->survived;
        }
        if (m_probe.active) {
            // Decided from a kept start: the game goes back there first, and
            // the restore drops every lesson past that tick - this one is
            // filed there instead, once its plan is about to be played.
            m_probe.lesson = std::move(lesson);
            m_probe.hasLesson = true;
        } else {
            m_lessons.push_back(std::move(lesson));
        }
    }

    // In two-player mode the pair lasts as long as the shorter of the two.
    int survived = best->survived;
    bool complete = best->complete;
    std::string name = best->name;
    if (pair) {
        survived = std::min(survived, m_pairSurvived1);
        complete = complete && m_pairComplete1;
        name = "P1 " + m_pairName1 + " + P2 " + best->name;
        if (m_pairForced1 > 0) forcedCommit = forcedCommit > 0 ? std::min(forcedCommit, m_pairForced1) : m_pairForced1;
        if (m_pairKnownDeath1 != UINT64_MAX) m_planKnownDeath = std::min(m_planKnownDeath, m_pairKnownDeath1);
    }

    // Never a plan of one or two ticks: the decision a tick later sees the
    // same thing and costs the same again. An idea that only just gets past
    // still plays a sixth of what it is known to last (and is thought about
    // again on the way), instead of being thrown away as a dead end.
    int commit = complete ? std::max(1, survived)
                          : std::max(std::max(1, survived / 6), std::min(K, survived - margin));
    if (!complete && SLSettings::get()->pathfinder.smooth && survived >= H) {
        // Realtime: a plan that lasts the whole horizon is played for two
        // thirds of it - the next decision is made meanwhile, from a start
        // the simulation carries to its end - so there are half as many.
        commit = std::max(commit, std::min((2 * H) / 3, survived - margin));
    }
    const bool flying = m_probe.active ? (m_probe.flying || (m_probe.pair && m_probe.flying2))
                                       : ((pl->m_player1 && (pl->m_player1->m_isDart || pl->m_player1->m_isShip ||
                                                             pl->m_player1->m_isBird || pl->m_player1->m_isSwing)) ||
                                          (pair && pl->m_player2 && (pl->m_player2->m_isDart || pl->m_player2->m_isShip ||
                                                                     pl->m_player2->m_isBird || pl->m_player2->m_isSwing)));
    if (!complete && flying) {
        // The flying kinds drift from the simulation sooner: think again more often.
        commit = std::max(1, std::min(commit, (int)std::lround(120.0 * std::max(1.0, tps / 240.0))));
    }
    // A repair's try into the death (forcedCommit past what the idea lasts) is
    // played to the death tick in one go. Cut at K it stopped short ("trying
    // release 25 at 9 anyway (lasts 529)" then "committing 192" at 144 TPS),
    // the decision on the way was no longer the repair's, and the game never
    // had its word. Everything else is thought about again on the way.
    if (forcedCommit > 0) {
        commit = (m_repair.active && forcedCommit > survived) ? forcedCommit : std::min(forcedCommit, std::max(1, K));
    }
    // What this tick really played, before the trim: a stop walked back to
    // here forbids this idea, and a fresh one generated there is compared
    // against it as it was generated.
    if (!pair) m_played[tick] = Tabu{commit, hashPrefix(d.chosenRaw, commit)};
    // The verified rest of the script, for the next decision to start from.
    // The object itself, not its kind: one plain spike the game confirmed made
    // every spike of the level certain death, the moving ones included.
    m_planKiller = (forcedCommit > 0 && best->killerId != 0) ? best->killerUid : 0;
    if (!pair && (int)best->inputs.size() > commit && forcedCommit <= 0) {
        m_planTail.assign(best->inputs.begin() + commit, best->inputs.end());
        m_planTailTick = tick + (uint64_t)commit;
    } else {
        m_planTail.clear();
        m_planTailTick = UINT64_MAX;
    }
    std::vector<TickInput> plan, plan2;
    if (pair) {
        plan.assign(m_pairChosen1.begin(), m_pairChosen1.begin() + std::min<size_t>((size_t)commit, m_pairChosen1.size()));
        plan2.assign(best->inputs.begin(), best->inputs.begin() + std::min<size_t>((size_t)commit, best->inputs.size()));
    } else {
        plan.assign(best->inputs.begin(), best->inputs.begin() + std::min<size_t>((size_t)commit, best->inputs.size()));
    }
    if (plan.empty()) {
        // Nothing to play: a dead end here rather than a decision that plays
        // nothing and is made again at once.
        log("tick %llu: %s has nothing to play; a dead end here", (unsigned long long)tick, name.c_str());
        pauseLive();
        cancelAhead();
        m_stats.deadEnds++;
        m_deadEndAt = tick;
        m_deadEndReal = false;
        m_deadEndX = decisionState(d).x;
        m_plan.clear();
        m_plan2.clear();
        m_planIndex = 0;
        m_phase = Phase::Backtracking;
        return;
    }
    // What the simulation expects of this plan, so the real game can be
    // checked against it while it is played.
    std::vector<TraceSample> trace;
    if (pair) {
        (void)Bot::get()->trajectory().run(pl, true, plan, (int)plan.size(), held, &trace, plan2, d.held, true);
    } else if (d.bestTraceTick == currentTick() && d.bestTrace.size() >= plan.size()) {
        // The winning run's own samples: the same start, the same script and
        // the same tick of the game, so the same path - one whole-horizon run
        // saved. Only with the game where it was: a run places the moving
        // objects for the tick it is made at, and a decision made ahead is
        // made while the plan plays (see bestTraceTick).
        trace.assign(d.bestTrace.begin(), d.bestTrace.begin() + (std::ptrdiff_t)plan.size());
    } else {
        (void)Bot::get()->trajectory().run(pl, true, plan, (int)plan.size(), held, &trace);
    }
    const bool ahead = m_ahead.active;
    if (ahead) {
        // Decided about where the plan ends while it was playing: on the end
        // of it (the trace's ticks count from the start of the whole plan).
        const int base = (int)m_plan.size();
        for (TraceSample& sample : trace) sample.tick += base;
        m_plan.insert(m_plan.end(), plan.begin(), plan.end());
        if (!plan2.empty()) {
            m_plan2.resize(m_plan.size() - plan.size(), TickInput{0, m_pairHeld1});
            m_plan2.insert(m_plan2.end(), plan2.begin(), plan2.end());
        }
        m_planTrace.insert(m_planTrace.end(), trace.begin(), trace.end());
        m_stats.aheadDecisions++;
        m_ahead = Ahead{};
        Bot::get()->trajectory().useStart(nullptr);
    } else {
        m_plan = std::move(plan);
        m_plan2 = std::move(plan2);
        m_planIndex = 0;
        m_planTrace = std::move(trace);
        m_aheadFailed = false;
    }
    m_lastReplanTick = tick;
    (void)ahead;

    char buf[200];
    std::snprintf(buf, sizeof(buf), "tick %llu: %s (lasts %s%d ticks), committing %d%s%s", (unsigned long long)tick,
                  name.c_str(), survived >= H ? ">=" : "", survived, commit,
                  m_repair.active ? " [repair]" : "", dropped > 0 ? " [fewer clicks]" : "");
    if (m_probe.active) {
        // Found from a kept start: now the game goes back there for real,
        // and the plan is played once it has (see finishRestore).
        log("%s [%zu of %zu ideas tried] - going back there", buf, triedCount, ideaCount);
        m_probe.found = true;
        m_probe.plan = m_plan;
        m_probe.plan2 = m_plan2;
        m_probe.trace = m_planTrace;
        m_probe.decision = buf;
        m_probe.knownDeath = m_planKnownDeath;  // the restore would forget it (see finishRestore)
        m_plan.clear();
        m_plan2.clear();
        m_planIndex = 0;
        m_planTrace.clear();
        endProbe();
        m_goTo = m_probe.tick;
        m_phase = Phase::Backtracking;
        return;
    }
    m_stats.lastDecision = buf;
    log("%s [%zu of %zu ideas tried]%s", buf, triedCount, ideaCount, ahead ? " (ahead)" : "");
    if (m_repair.active) resumeFrom(tick);
    m_phase = Phase::Committing;
    // Realtime: the game plays the plan on its own clock from here.
    if (SLSettings::get()->pathfinder.smooth && !m_live && m_planIndex < m_plan.size()) goLive();
}

void AbsensePathfinder::goLive() {
    if (m_live) return;
    m_live = true;
    m_livePending = false;
    m_liveNextTick = currentTick();  // the tick about to run, as commitTick sees it
    m_pauseAt = UINT64_MAX;
    Bot::get()->updater().setPaused(false);
}

void AbsensePathfinder::pauseLive() {
    if (!m_live) return;
    m_live = false;
    Bot::get()->updater().setPaused(true);
    // The last tick's bookkeeping (its physics is done: this runs between frames).
    if (m_livePending) {
        m_livePending = false;
        (void)afterTick(m_liveTick, m_liveIn, m_liveIn2);
    }
}

void AbsensePathfinder::cancelAhead() {
    if (!m_ahead.active) return;
    auto* bot = Bot::get();
    if (m_decision.searching && bot->trajectory().searching()) {
        std::vector<TickInput> dropped;
        (void)bot->trajectory().finishSearch(dropped);
    }
    m_decision = Decision{};
    m_pairSecondPass = false;
    m_ahead = Ahead{};
    bot->trajectory().useStart(nullptr);
}

// The next decision is about where the plan ends, made while the plan is
// still playing: the simulation carries the kept start of this tick to the
// end of the plan, and the decision is made from there like a probe.
void AbsensePathfinder::beginAhead() {
    if (m_ahead.active || m_probe.active) return;
    // A plan played into a death the simulation is not sure about is there
    // to let the real game speak: nothing to decide ahead of it. And once
    // a decision about this plan's end found nothing, the plan plays out
    // and the real state decides (deciding again from here made the same
    // plan again, four ticks at a time, for ever).
    if (m_planKnownDeath != UINT64_MAX || m_aheadFailed) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    if (separateNow()) return;  // two-player mode decides from the real state
    // Between frames the last live tick's bookkeeping has not run yet (it runs
    // when the next tick asks for its input), so the start of the tick after
    // it is not kept: carry from the pending tick's own start, with its input
    // in front of the rest of the plan. Looking for the start that does not
    // exist yet is why almost no plan ever got a decision made ahead of it.
    const bool pending = m_livePending;
    const uint64_t from = pending ? m_liveTick : m_liveNextTick;
    const size_t at = pending ? m_planIndex - 1 : m_planIndex;
    if (at >= m_plan.size()) return;
    const Start* now = startAt(from);
    if (!now || !now->sim) return;
    std::span<const TickInput> rest(m_plan.data() + at, m_plan.size() - at);
    std::span<const TickInput> rest2;
    if (at < m_plan2.size()) rest2 = std::span<const TickInput>(m_plan2.data() + at, m_plan2.size() - at);
    auto future = Bot::get()->trajectory().advanceStart(pl, now->sim, rest, rest2);
    if (!future) return;  // the plan dies before its end: that death comes first
    // A plan that changes the player's kind leaves a start the simulation
    // judges wrongly: every ahead decision in the logs whose plan crossed a
    // portal was settled on a copy at floor level and the game left the
    // appended plan eight ticks later by some 550 units, while the one that
    // stayed in one kind was followed for 153 ticks. Such a plan plays out and
    // the decision at its end is made from the real state.
    if (Trajectory::startState(*now->sim).mode != Trajectory::startState(*future).mode) return;
    if (Trajectory::startCount(*future) == 2 &&
        Trajectory::startState(*now->sim, true).mode != Trajectory::startState(*future, true).mode)
        return;
    m_ahead.active = true;
    m_ahead.tick = from + rest.size();
    m_ahead.start = future;
    m_ahead.held = Trajectory::startHeld(*future);
    m_ahead.flying = Trajectory::startFlying(*future);
    m_ahead.pair = Trajectory::startCount(*future) == 2 && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
    m_ahead.held2 = Trajectory::startHeld2(*future);
    m_ahead.flying2 = Trajectory::startFlying2(*future);
    Bot::get()->trajectory().useStart(future);
    m_decision = Decision{};
    m_pairSecondPass = false;
}

bool AbsensePathfinder::rewindTo(uint64_t target) {
    auto* bot = Bot::get();
    auto& pf = bot->practiceFix();
    int stored = -1;
    for (int i = (int)pf.m_storedFrames.size() - 1; i >= 0; i--) {
        const uint64_t f = pf.m_storedFrames[(size_t)i].frame;
        if (f <= target && f >= m_stats.startTick) {
            stored = i;
            break;
        }
    }
    if (stored < 0) return false;
    if (currentTick() >= m_stats.bestTick) rememberBest();
    // The rest of the plan from the tick being gone back to stays an idea (the
    // stepped path keeps it in m_plan; the restore clears that, and the
    // decision afterwards had to find the same way again from nothing).
    if (m_planIndex < m_plan.size() && m_liveNextTick == target && m_plan2.empty()) {
        m_planTail.assign(m_plan.begin() + (std::ptrdiff_t)m_planIndex, m_plan.end());
        m_planTailTick = target;
    }
    m_backtrackTarget = pf.m_storedFrames[(size_t)stored].frame;
    m_restepTo = target;
    m_expectReset = true;
    m_restorePending = true;
    m_restepping = false;
    m_wouldDie = false;
    bot->updater().backwardsStep((int)(pf.m_storedFrames.size() - (size_t)stored));
    m_phase = Phase::Restoring;
    return true;
}

// Realtime hooks. The game asks for a tick's input at the start of the
// tick; the previous tick's bookkeeping is done right then, when its
// physics is final - so it does not matter where in a tick the frame
// counter advances (doing it on the counter put every plan a tick off its
// own trace, and the search thought again every 8 ticks).
void AbsensePathfinder::liveInput() {
    if (!m_live) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    if (m_livePending) {
        m_livePending = false;
        const auto t0 = std::chrono::steady_clock::now();
        (void)afterTick(m_liveTick, m_liveIn, m_liveIn2);
        m_stats.secondsReal += secondsSince(t0);
        if (!m_live) return;  // it died, or drifted: the game is stopped
    }
    if (m_phase != Phase::Committing) return;
    if (m_planIndex >= m_plan.size()) {
        // The plan ran out before the next decision was ready: the game
        // waits. This tick and the frame's remaining ticks still run, with no
        // bookkeeping of their own: back to exactly here before deciding (see
        // the m_pauseAt check in tick()), whether or not a decision about the
        // plan's end is in progress. Without it the decision was made one or
        // two ticks past the end, the verified rest no longer fitted the tick
        // it was about, and the ticks in between were replayed as a release
        // the game never made.
        m_pauseAt = m_liveNextTick;
        pauseLive();
        m_stats.freezes++;
        m_phase = Phase::Deciding;
        return;
    }
    const uint64_t tick = m_liveNextTick++;
    const TickInput in = m_plan[m_planIndex];
    const bool twoPlayer = m_planIndex < m_plan2.size() && separateNow();
    const TickInput in2 = twoPlayer ? m_plan2[m_planIndex] : TickInput{0, playerHeld(true)};
    m_planIndex++;
    applyInput(in);
    if (twoPlayer) applyInput(in2, true);
    m_wouldDie = false;
    m_liveTick = tick;
    m_liveIn = in;
    m_liveIn2 = in2;
    m_livePending = true;
}

void AbsensePathfinder::afterLiveTick() {
    // Nothing here any more: see liveInput().
}

void AbsensePathfinder::giveUp(const std::string& reason) {
    pauseLive();
    cancelAhead();
    absense::modal::info({.title = "AbsensePathfinder stopped", .message = reason});
    stop(reason);
}

int AbsensePathfinder::widenLevel(uint64_t at) {
    uint64_t spot = 0;
    if (m_repair.active) {
        spot = m_repair.deathTick;
    } else if (m_lastResume.valid && m_lastResume.deathTick > at) {
        spot = m_lastResume.deathTick;
    }
    if (spot == 0) return 0;
    const double tps = Bot::get()->updater().getTps();
    const Trap* t = trapAt(spot, (uint64_t)std::lround(32.0 * std::max(1.0, tps / 240.0)));
    return t ? t->level : 0;
}

// Every stop down to the floor failed. The search never stops on its own for
// that: the spot's level goes up, and every level widens what is tried at the
// stops already there - the whole search budget at every stop and more of it,
// more of the ideas run over the whole horizon, the human macro shifted
// further, more random ideas and card rounds, the game's word at the floor even
// for a killer the simulation calls static - and the floor goes a second
// further back when anything is kept there (the start too, into the frames
// stored before it), never further from the death than a decision at that
// level still sees. Only the player, closing the level or completing it ends
// the search.
void AbsensePathfinder::escalate(const char* why) {
    auto* bot = Bot::get();
    pauseLive();
    cancelAhead();
    if (m_probe.active) endProbe();
    if (m_decision.searching && bot->trajectory().searching()) {
        std::vector<TickInput> dropped;
        (void)bot->trajectory().finishSearch(dropped);
    }
    m_decision = Decision{};
    m_pairSecondPass = false;
    m_plan.clear();
    m_plan2.clear();
    m_planIndex = 0;
    m_goTo = UINT64_MAX;

    const double tps = bot->updater().getTps();
    const double scale = std::max(1.0, tps / 240.0);
    Trap* trap = trapAt(m_repair.deathTick, (uint64_t)std::lround(32.0 * scale));
    if (!trap) {
        m_traps.push_back({m_repair.deathTick, 1, m_deadEndX});
        trap = &m_traps.back();
    }
    trap->level++;
    trap->anyway = 0;  // a new level may give the game its word again (see the verdict)
    const int level = trap->level;
    m_stats.escalations++;

    const double seconds = std::min(kMaxWidenSeconds, kMaxRepairSeconds + (double)level);
    const uint64_t reach = (uint64_t)std::lround(seconds * tps);
    const uint64_t nearest = m_repair.deathTick > reach ? m_repair.deathTick - reach : 0;
    const uint64_t second = (uint64_t)std::max(1.0, std::round(tps));
    const uint64_t lowest = std::max(nearest, m_stats.startTick);
    const uint64_t stepped = m_repair.floor > second ? m_repair.floor - second : 0;
    const uint64_t want = std::max(stepped, lowest);
    char moved[160];
    if (want < m_repair.floor) {
        std::snprintf(moved, sizeof(moved), "the floor goes from tick %llu to %llu", (unsigned long long)m_repair.floor,
                      (unsigned long long)want);
        m_repair.floor = want;
    } else if (m_repair.floor <= m_stats.startTick + 2 && nearest < m_stats.startTick) {
        log("%s (spot at tick %llu): level %d; the floor is where the search started, so the start goes back a step",
            why, (unsigned long long)m_repair.deathTick, level);
        if (moveStartBack()) return;  // the walk starts over from there, at this level
        std::snprintf(moved, sizeof(moved), "the floor stays at tick %llu (no stored frame before the start)",
                      (unsigned long long)m_repair.floor);
    } else {
        std::snprintf(moved, sizeof(moved), "the floor stays at tick %llu (as far from the spot as this level sees)",
                      (unsigned long long)m_repair.floor);
    }
    m_repair.passes++;
    m_repair.probe = 0;
    m_repair.retries = 0;
    m_repair.retryTarget = UINT64_MAX;
    log("%s (spot at tick %llu): level %d - search budget x%d at every stop, decisions see %.0f s before the spot; %s",
        why, (unsigned long long)m_repair.deathTick, level, 1 + std::min(level, 3), seconds, moved);
    m_phase = Phase::Backtracking;
}

// The way past is before the tick the search was started at. Rather than
// give up there, the start itself moves back into the frames stored before
// it (backwards stepping keeps one per tick while the player plays), and
// the search starts over from the earliest one within the limit, as if it
// had been started there. Never further from the spot than a decision can
// still see it, or a hopeless spot would walk it all the way back to the
// beginning of the level.
bool AbsensePathfinder::moveStartBack() {
    auto* bot = Bot::get();
    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();
    const double tps = updater.getTps();
    const uint64_t limit = (uint64_t)std::max(1.0, kBackSeconds * tps);
    // A third of the limit (a second) at a time: far enough to matter, not
    // so far it looks like the level restarting.
    const uint64_t step = std::max<uint64_t>(1, limit / 3);
    uint64_t earliest = m_stats.startTick > step ? m_stats.startTick - step : 0;
    // Bounded by the spot, not by a count of moves (four, then "AbsensePathfinder
    // stopped"): the start never goes further from the death being repaired
    // than a decision at the spot's level can still see (see escalate), so a
    // hopeless spot cannot walk it back to the beginning of the level, and a
    // spot the start is close to can always take the next step.
    if (m_repair.active) {
        const uint64_t reach =
            (uint64_t)std::lround(std::min(kMaxWidenSeconds, kMaxRepairSeconds + (double)widenLevel()) * tps);
        if (m_repair.deathTick > reach) earliest = std::max(earliest, m_repair.deathTick - reach);
    }
    if (earliest + 1 >= m_stats.startTick) return false;
    pauseLive();
    cancelAhead();
    int stored = -1;
    for (int i = 0; i < (int)pf.m_storedFrames.size(); i++) {
        const uint64_t f = pf.m_storedFrames[(size_t)i].frame;
        if (f >= earliest && f + 1 < m_stats.startTick) {  // the earliest within the limit
            stored = i;
            break;
        }
    }
    if (stored < 0) return false;
    const uint64_t landing = pf.m_storedFrames[(size_t)stored].frame;
    const uint64_t was = m_stats.startTick;
    m_startMoves++;
    log("the way past is before tick %llu where it was started; moving the start back to tick %llu (move %d, level %d)",
        (unsigned long long)was, (unsigned long long)landing, m_startMoves, widenLevel());

    // The search starts over from there.
    if (m_probe.active) endProbe();
    if (m_decision.searching && bot->trajectory().searching()) {
        std::vector<TickInput> dropped;
        (void)bot->trajectory().finishSearch(dropped);
    }
    m_decision = Decision{};
    m_pairSecondPass = false;
    m_probe = Probe{};
    m_goTo = UINT64_MAX;
    m_plan.clear();
    m_plan2.clear();
    m_planIndex = 0;
    m_planTrace.clear();
    m_history.clear();
    m_history2.clear();
    m_historyPair.clear();
    m_repair = Repair{};
    m_lastResume = LastResume{};
    m_deadEndAt = 0;
    m_deadEndReal = false;
    m_deadEndUnseen = false;
    m_deadEndX = 0.0f;
    m_stats.startTick = landing;
    // The old start's kept state is past the new one and nothing points at it
    // any more: it is let go of here, or its whole checkpoint leaks per move.
    if (m_hasStartAnchor && m_startAnchor.state.m_checkpoint) m_startAnchor.state.m_checkpoint->release();
    m_startAnchor = Anchor{};
    m_hasStartAnchor = false;  // the kept state of the old start is past the new one; a new one is kept on landing
    // The furthest path first: kept after the clip it was the replay cut back
    // to the new start, and the "-furthest" file lost everything past it.
    if (currentTick() >= m_stats.bestTick) rememberBest();
    // The replay's actions between there and the old start were the
    // player's: they go, exactly as at a start.
    auto& rs = bot->replaySystem();
    rs.m_actionAtom.clipFrom((uint32_t)landing + 1);  // GucciBot's labels: see start()
    rs.m_inputIndex = rs.m_actionAtom.length();

    m_backtrackTarget = landing;
    m_restepTo = landing;
    m_expectReset = true;
    m_restorePending = true;
    m_restepping = false;
    m_wouldDie = false;
    updater.backwardsStep((int)(pf.m_storedFrames.size() - (size_t)stored));
    m_phase = Phase::Restoring;
    return true;
}

// The real game got as far as these ideas were judged to last: now they
// are learned (the way past, the idea that worked again, the kind that won).
void AbsensePathfinder::confirmLessons(uint64_t now) {
    if (m_lessons.empty()) return;
    auto& mem = absense::memory::Store::get();
    const bool learning = SLSettings::get()->pathfinder.learn && mem.isOpen();
    for (auto it = m_lessons.begin(); it != m_lessons.end();) {
        if (it->confirmAt > now) {
            ++it;
            continue;
        }
        if (learning) {
            mem.win(it->situation, it->name);
            if (it->memoryIndex >= 0) {
                mem.won((size_t)it->memoryIndex);
                m_stats.rememberedWins++;
            } else if (!it->inputs.empty()) {
                mem.remember(it->state, it->inputs, it->lasted, true);
            }
        }
        if (it->name.rfind("human", 0) == 0) m_stats.humanWins++;
        it = m_lessons.erase(it);
    }
}

void AbsensePathfinder::dropLessonsAfter(uint64_t tick) {
    std::erase_if(m_lessons, [tick](const Lesson& l) { return l.confirmAt > tick; });
}

void AbsensePathfinder::resumeFrom(uint64_t tick) {
    log("resuming at tick %llu, %llu ticks before the dead end at %llu (%d stops tried)", (unsigned long long)tick,
        (unsigned long long)(m_repair.deathTick - tick), (unsigned long long)m_repair.deathTick, m_repair.probe);
    m_lastResume.valid = true;
    m_lastResume.tick = tick;
    m_lastResume.deathTick = m_repair.deathTick;
    m_lastResume.probe = m_repair.probe;
    m_lastResume.retries = m_repair.retries;
    m_repair.active = false;
    m_repair.failed.clear();
    m_repair.failed2.clear();
    m_repair.failedPair.clear();
}

void AbsensePathfinder::probe() {
    if (!m_probe.active) {
        m_phase = Phase::Deciding;
        return;
    }
    decide();
}

void AbsensePathfinder::endProbe() {
    m_probe.active = false;
    Bot::get()->trajectory().useStart(nullptr);
}

bool AbsensePathfinder::applyInput(const TickInput& in, bool player2) {
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    // Whatever the player pressed meanwhile is not part of the search
    // (player 1's inputs are queued first; player 2's are added to them).
    if (!player2) pl->m_queuedButtons.clear();
    bool down = playerHeld(player2);
    for (uint8_t k = 0; k < in.presses; k++) {
        if (down) pl->queueButton(1, false, player2, 0.0);
        pl->queueButton(1, true, player2, 0.0);
        down = true;
    }
    if (down != in.held) {
        pl->queueButton(1, in.held, player2, 0.0);
    }
    return true;
}

void AbsensePathfinder::stepGame() {
    const auto t0 = std::chrono::steady_clock::now();
    auto& updater = Bot::get()->updater();
    updater.setPaused(true);
    updater.stepOnce();
    CCScheduler::get()->update(static_cast<float>(updater.getPhysicsDt()));
    Bot::get()->trajectory().realStateChanged();  // the player has moved: the copies come from it again
    m_stats.secondsReal += secondsSince(t0);
}

bool AbsensePathfinder::commitTick() {
    if (m_planIndex >= m_plan.size()) {
        m_phase = Phase::Deciding;
        return true;
    }
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    const uint64_t tick = currentTick();
    const TickInput in = m_plan[m_planIndex];
    const bool twoPlayer = m_planIndex < m_plan2.size() && separateNow();
    const TickInput in2 = twoPlayer ? m_plan2[m_planIndex] : TickInput{0, playerHeld(true)};
    m_planIndex++;

    applyInput(in);
    if (twoPlayer) applyInput(in2, true);
    m_wouldDie = false;
    stepGame();
    return afterTick(tick, in, in2);
}

// After a tick of the plan has run (stepped here, or by the game itself in
// realtime): the history, the kept start, and whether the game agreed.
bool AbsensePathfinder::afterTick(uint64_t tick, const TickInput& in, const TickInput& in2) {
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    Bot::get()->trajectory().realStateChanged();  // a tick has run: the player is somewhere else
    // The tick after this one: what the frame counter reads once the tick
    // has run (in realtime it may or may not have advanced yet).
    const uint64_t now = tick + 1;
    const size_t idx = (size_t)(tick - m_stats.startTick);
    if (m_history.size() <= idx) m_history.resize(idx + 1);
    m_history[idx] = in;
    if (m_history2.size() <= idx) m_history2.resize(idx + 1);
    m_history2[idx] = in2;
    if (m_historyPair.size() <= idx) m_historyPair.resize(idx + 1, 0);
    m_historyPair[idx] = separateNow() ? 1 : 0;
    m_stats.committedTicks++;
    const bool died = m_wouldDie || pl->m_player1->m_isDead || pl->m_playerDied;
    // A tick that killed the player confirms nothing: a lesson due exactly now
    // promised this very tick alive (its plan lasted `survived` whole ticks).
    if (!died) confirmLessons(now);
    keepStart(now);

    if (m_completed) return false;  // handled by tick()

    if (died) {
        m_stats.deadEnds++;
        pauseLive();
        cancelAhead();
        log("died at tick %llu while committing (%s)", (unsigned long long)now, m_stats.lastDecision.c_str());
        m_plan.clear();
        m_plan2.clear();
        m_planIndex = 0;
        m_deadEndAt = now;
        m_deadEndReal = true;
        m_deadEndX = pl->m_player1->m_position.x;
        // The simulation approved this plan and the game killed it: something
        // it does not model is here (a block that moves, a trigger). The spot
        // is kept as deadly for the simulation from now on, so it looks for a
        // way around it instead of approving the same path again. (A death
        // the simulation did see coming - the idea was played to find out -
        // teaches it nothing.)
        const bool expected = m_planKnownDeath != UINT64_MAX && now + 2 >= m_planKnownDeath;
        m_deadEndUnseen = !expected;
        if (expected && m_planKiller != 0 && m_confirmedKillers.insert(m_planKiller).second) {
            log("the game agreed: the object the plan died at (unique id %d) is deadly; the search will not play into it again",
                m_planKiller);
        }
        // A death the pathfinder did not intercept itself (the game killed
        // the player outright) names no object: it is uncertain, as every
        // death was before any of this.
        const world::Certainty killedBy = m_wouldDie ? m_wouldDieCertainty : world::Certainty::Uncertain;
        // A spot is only worth keeping where the World cannot follow what
        // killed the player. At an object it does follow - one nothing in the
        // level can move, or one every trigger that reaches it is a kind the
        // World runs - the simulation and the game already agree about where
        // it is, and a rect that kills every run through that corridor for the
        // rest of the attempt buys nothing; the search learns from those
        // deaths as confirmed killers instead.
        if (!expected && killedBy != world::Certainty::Uncertain) {
            log("the simulation did not see that death, but what it happened at is %s: no spot kept",
                world::certaintyName(killedBy));
        }
        if (!expected && killedBy == world::Certainty::Uncertain) {
            cocos2d::CCRect spot = m_wouldDie ? m_wouldDieRect : pl->m_player1->getObjectRect(0.3f, 0.3f);
            // The spot covers the way in, not just the point of impact: the
            // simulation is often a tick or two late here, so the next try
            // slips past a rect around the death alone and dies just after it
            // (two deaths 5 units apart, then a third...). The last few ticks
            // of the path that led in are poisoned with it.
            {
                const size_t at = m_planIndex >= 1 ? m_planIndex - 1 : 0;
                // The last few ticks of the way in: a fixed four is 17 ms at
                // 240 TPS and 4 ms at 1000, where the spot hardly stretches at
                // all and the next try dies just past it again.
                const size_t span = (size_t)std::max<long>(1, std::lround(4.0 * std::max(1.0, Bot::get()->updater().getTps() / 240.0)));
                const size_t back = at >= span ? at - span : 0;
                if (back < m_planTrace.size() && at < m_planTrace.size()) {
                    // The way in is the dead player's own path: player 2's from
                    // the trace when it was the one the game killed.
                    const bool p2 = m_wouldDie && m_wouldDieP2 && m_planTrace[at].dual;
                    const TraceSample& b = m_planTrace[back];
                    const TraceSample& a = m_planTrace[at];
                    const float dx = std::clamp((p2 ? b.x2 : b.x) - (p2 ? a.x2 : a.x), -45.0f, 45.0f);
                    const float dy = std::clamp((p2 ? b.y2 : b.y) - (p2 ? a.y2 : a.y), -45.0f, 45.0f);
                    const float minX = std::min(spot.getMinX(), spot.getMinX() + dx);
                    const float minY = std::min(spot.getMinY(), spot.getMinY() + dy);
                    const float maxX = std::max(spot.getMaxX(), spot.getMaxX() + dx);
                    const float maxY = std::max(spot.getMaxY(), spot.getMaxY() + dy);
                    spot = cocos2d::CCRect(minX, minY, maxX - minX, maxY - minY);
                }
            }
            spot.origin.x -= 3.0f;
            spot.origin.y -= 3.0f;
            spot.size.width += 6.0f;
            spot.size.height += 6.0f;
            // The spot belongs to this real tick: a run only dies in it while
            // it is passing through that moment (Trajectory::hitsPhantom).
            Bot::get()->trajectory().addPhantom(spot, (int)now);
            m_stats.phantoms++;
            log("the simulation did not see that death: (%.1f, %.1f) %.0fx%.0f is a spot to avoid around tick %llu (%zu kept)",
                spot.getMidX(), spot.getMidY(), spot.size.width, spot.size.height, (unsigned long long)now,
                Bot::get()->trajectory().phantomCount());
        }
        m_phase = Phase::Backtracking;
        return false;
    }

    const double anchorScale = std::max(1.0, Bot::get()->updater().getTps() / 240.0);
    const uint32_t every = std::max<uint32_t>(SLSettings::get()->pathfinder.anchorEvery,
                                              (uint32_t)std::lround(kAnchorEveryMin * anchorScale));
    if ((now - m_stats.startTick) % every == 0) keepAnchor(now);
    if ((now - m_stats.startTick) % 64 == 0) Bot::get()->trajectory().dropPhantomsBefore(playerX() - 1500.0f);
    noteProgress();

    // The plan was chosen on the simulation's word; when the real game has
    // gone somewhere else, the rest of the plan is built on sand - think
    // again from where the player really is (a few ticks between rethinks,
    // or a simulation that is always a little off would never get anywhere).
    if (m_planIndex >= 1 && m_planIndex <= m_planTrace.size() && pl->m_player1) {
        const TraceSample& expected = m_planTrace[m_planIndex - 1];
        // A run switches on a dual or solo portal now (Sim::enterDual), so a
        // plan can be judged across one. This stays until the two can be held
        // against each other with the game running: if the real game ends up
        // with a different number of players than the plan was judged with,
        // the rest of the plan was judged for the wrong players, and thinking
        // again from here costs a decision rather than an attempt.
        const bool dualNow = pl->m_player2 && pl->m_gameState.m_isDualMode;
        if (expected.dual != dualNow && m_planIndex < m_plan.size()) {
            log("the game %s dual mode at tick %llu but the plan was judged %s it: thinking again from here",
                dualNow ? "entered" : "left", (unsigned long long)now, dualNow ? "without" : "with");
            if (m_live) m_pauseAt = now;
            pauseLive();
            cancelAhead();
            m_planTrace.clear();
            m_phase = Phase::Deciding;  // the rest of the plan stays one of the ideas, judged with the right copies now
            return true;
        }
        float dx = std::abs(pl->m_player1->m_position.x - expected.x);
        float dy = std::abs(pl->m_player1->m_position.y - expected.y);
        if (expected.dual && pl->m_player2 && pl->m_gameState.m_isDualMode) {
            dx = std::max(dx, std::abs(pl->m_player2->m_position.x - expected.x2));
            dy = std::max(dy, std::abs(pl->m_player2->m_position.y - expected.y2));
        }
        // The gap between rethinks is a time, not a tick count: 33 ms at 240
        // TPS and 8 ms at 1000, where a decision costs seconds.
        const uint64_t replanGap = (uint64_t)std::max<long>(1, std::lround((double)kMinTicksBetweenReplans * anchorScale));
        if ((dx > kDivergence || dy > kDivergence) && now >= m_lastReplanTick + replanGap) {
            m_stats.divergences++;
            log("real path left the simulated one at tick %llu (off by %.2f, %.2f): thinking again from here",
                (unsigned long long)now, dx, dy);
            if (m_live) m_pauseAt = now;  // the frame's remaining ticks still run: back to here first
            pauseLive();
            cancelAhead();
            m_planTrace.clear();
            m_phase = Phase::Deciding;  // the rest of the plan stays one of the ideas
        }
    }
    return true;
}

// Distance of the n-th stop back from the dead end: one tick at a time for
// the first few, then growing steps (4, 6, 8, 12, 16, 24, 32 ...), scaled
// with the tick rate so it means the same amount of time at any TPS.
uint64_t AbsensePathfinder::backDistance(int probe) const {
    const double tps = Bot::get()->updater().getTps();
    const double scale = std::max(1.0, tps / 240.0);
    if (probe <= 0) return 0;
    double d;
    if (probe <= 4) {
        d = probe;
    } else {
        // 4 * (1.5, 2, 3, 4, 6, 8 ...): alternating x1.5 and x1.333
        d = 4.0;
        for (int i = 4; i < probe; i++) d *= (i % 2 == 0) ? 1.5 : (4.0 / 3.0);
    }
    return (uint64_t)std::max(1.0, std::round(d * scale));
}

void AbsensePathfinder::beginRepair() {
    // In realtime the rest of the frame's ticks run on past the death (the
    // pause takes effect at the next frame): the dead end is where afterTick
    // said, not where the counter reads now.
    const uint64_t tick = m_deadEndReal ? std::min(currentTick(), m_deadEndAt) : currentTick();
    // Where the obstacle is: the real death, or the death the simulation
    // sees ahead when the dead end was its verdict.
    const uint64_t obstacle = std::max(m_deadEndAt, tick);
    const auto& cfg = SLSettings::get()->pathfinder;
    const double tps = Bot::get()->updater().getTps();
    const double scale = std::max(1.0, tps / 240.0);
    const uint64_t limit = (uint64_t)std::max(1.0, kBackSeconds * tps);

    // Dying again around the same spot (a few ticks either way count as the
    // same spot, so gaining a tick or two does not reset it): this repair
    // starts one stop further back than the last one did, so every failure
    // moves the retry a little earlier instead of hammering the same tick.
    const uint64_t slack = (uint64_t)std::lround(32.0 * scale);
    Trap* trap = trapAt(obstacle, slack);
    if (trap) {
        // Only a real death counts: the simulation declaring the same dead end
        // again is the search still looking, not the spot biting again, and
        // counting those sent every repair hundreds of ticks back.
        if (m_deadEndReal) trap->fails++;
    } else {
        // The obstacle's x, not the player's: a dead end the simulation
        // declares from far before the spot must be filed under the same spot
        // as a real death at it (the memory matches spots within 100 units).
        auto* layer = PlayLayer::get();
        const float atX = m_deadEndAt != 0 ? m_deadEndX
                                           : (layer && layer->m_player1 ? layer->m_player1->m_position.x : 0.0f);
        const bool learning = SLSettings::get()->pathfinder.learn && absense::memory::Store::get().isOpen();
        const int known = learning ? absense::memory::Store::get().spotFails(atX) : 0;
        m_traps.push_back({obstacle, std::max(1, known), atX});
        trap = &m_traps.back();
        if (known > 0) log("this spot bit %d times in earlier runs: the repair starts further back at once", known);
    }
    if (SLSettings::get()->pathfinder.learn) absense::memory::Store::get().spotFailed(trap->x, trap->fails);

    // Dying here again and again means the game keeps dying where the
    // simulation sees a way. That stopped the search; now every so many real
    // deaths the spot widens (see escalate) and the count starts again, as far
    // back as a repair here begins anyway.
    if (trap->fails > kMaxFailsPerSpot) {
        trap->fails = 8;
        trap->level++;
        trap->anyway = 0;
        m_stats.escalations++;
        log("dead end at tick %llu: %d real deaths around here; the spot widens to level %d", (unsigned long long)tick,
            kMaxFailsPerSpot, trap->level);
    }

    m_repair.active = true;
    m_repair.deathTick = obstacle;
    m_repair.from = tick;
    // A widened spot begins its walk a second further back a level, up to three.
    const uint64_t back = limit + (uint64_t)std::lround((double)std::min(trap->level, 3) * tps);
    m_repair.floor = tick > m_stats.startTick + back ? tick - back : m_stats.startTick;
    // Never past the point where an idea can still be run as far as the spot:
    // from further back every idea is approved on "survives the horizon" and
    // replays into the same death (6.4 s of replay per try on Slaughterhouse).
    // A widened spot's decisions see further (see decide), so its floor may too.
    {
        const uint64_t reach =
            (uint64_t)std::lround(std::min(kMaxWidenSeconds, kMaxRepairSeconds + (double)trap->level) * tps);
        const uint64_t earliest = obstacle > reach ? obstacle - reach : m_stats.startTick;
        if (m_repair.floor < earliest) m_repair.floor = std::max(earliest, m_stats.startTick);
    }
    m_repair.failed = m_history;
    m_repair.failed2 = m_history2;
    m_repair.failedPair = m_historyPair;
    m_repair.retryTarget = UINT64_MAX;
    m_repair.passes = 0;  // a retry is a new repair: the passes of the one that resumed are not its own

    // Died at the same spot again after resuming from a stop with an idea
    // the simulation approved of: the game disagrees with the simulation
    // here, so another idea from that same stop is worth more than a stop
    // further back (the failed one is remembered there and skipped).
    if (m_lastResume.valid && m_lastResume.tick + (uint64_t)std::lround(kMinRetryDistance * scale) <= tick &&
        tick <= m_lastResume.tick + (uint64_t)std::lround(240.0 * scale) &&  // a resume a second or more back is not "the same stop" any more
        trapAt(m_lastResume.deathTick, slack) == trap && m_lastResume.retries < kRetriesPerStop) {
        m_repair.retries = m_lastResume.retries + 1;
        m_repair.probe = m_lastResume.probe;
        m_repair.retryTarget = m_lastResume.tick;
        log("dead end at tick %llu again (%d here so far); trying something else from tick %llu (retry %d of %d)",
            (unsigned long long)tick, trap->fails, (unsigned long long)m_lastResume.tick, m_repair.retries, kRetriesPerStop);
        return;
    }
    m_repair.retries = 0;
    m_repair.passes = 0;
    m_repair.probe = std::min(trap->fails - 1, 7);  // the next stop is probe + 1: backDistance(8) = 16 ticks at 240 TPS
    // The game killed the player where the simulation saw nothing: the stops
    // one and two ticks back cannot change anything (the player is already
    // committed, and the simulation approves the same idea again), so the
    // first stop is about a tenth of a second back. Only for a death the game
    // really dealt: the flag belongs to that dead end, and a dead end the
    // simulation declared used to inherit it from the last real one.
    if (m_deadEndReal && m_deadEndUnseen) m_repair.probe = std::max(m_repair.probe, 6);
    if (obstacle > tick) {
        log("dead end at tick %llu: the simulation sees the death at %llu (%d here so far); looking back one stop at a time, "
            "no further than tick %llu",
            (unsigned long long)tick, (unsigned long long)obstacle, trap->fails, (unsigned long long)m_repair.floor);
    } else {
        log("dead end at tick %llu (%d here so far); looking back one stop at a time, no further than tick %llu",
            (unsigned long long)tick, trap->fails, (unsigned long long)m_repair.floor);
    }
}

AbsensePathfinder::Trap* AbsensePathfinder::trapAt(uint64_t tick, uint64_t slack) {
    for (Trap& t : m_traps) {
        if (tick + slack >= t.tick && tick <= t.tick + slack) return &t;
    }
    return nullptr;
}

void AbsensePathfinder::beginBacktrack() {
    auto* bot = Bot::get();
    auto& updater = bot->updater();
    auto& pf = bot->practiceFix();
    pauseLive();
    cancelAhead();
    // A realtime death is noticed a tick or two late: the stops count back
    // from where the game died, not from where the counter came to rest.
    const uint64_t tick = m_deadEndReal ? std::min(currentTick(), m_deadEndAt) : currentTick();
    const auto& cfg = SLSettings::get()->pathfinder;
    const double tps = updater.getTps();
    const uint64_t limit = (uint64_t)std::max(1.0, kBackSeconds * tps);

    if (!m_repair.active) beginRepair();
    if (!isRunning()) return;
    // At the start there is nothing behind the game: the stop below is then
    // the tick it is at, decided in place, and a floor that fails widens the
    // spot (see escalate) - it never ends the search.

    // The next stop back (or the same one again, for another idea), or the
    // stop a probe found a way on from.
    uint64_t target;
    const bool probed = m_goTo != UINT64_MAX && m_goTo < tick;
    const bool retry = !probed && m_repair.retryTarget != UINT64_MAX && m_repair.retryTarget < tick;
    if (probed) {
        target = m_goTo;
        m_goTo = UINT64_MAX;
    } else if (retry) {
        target = m_repair.retryTarget;
        m_repair.retryTarget = UINT64_MAX;
    } else {
        // The next stop that is actually before where the game is now. After
        // a real step back, the stops counted from the dead end all landed on
        // the same tick until the distance caught up (14 looks at one tick).
        m_repair.probe++;
        // Every walk after the first stops a third, then two thirds, of the way
        // along each gap to the next stop: walked again on the same ticks a walk
        // started every idea from the same states (four passes printed the same
        // best idea at all nineteen stops on UNKNOWN).
        const auto stopBack = [this](int p) {
            const uint64_t at = backDistance(p);
            const int phase = m_repair.passes % 3;
            return phase == 0 ? at : at + (backDistance(p + 1) - at) * (uint64_t)phase / 3;
        };
        uint64_t back = stopBack(m_repair.probe);
        target = m_repair.from > m_stats.startTick + back ? m_repair.from - back : m_stats.startTick;
        while (target >= tick && target > m_stats.startTick && m_repair.probe < 64) {
            m_repair.probe++;
            back = stopBack(m_repair.probe);
            target = m_repair.from > m_stats.startTick + back ? m_repair.from - back : m_stats.startTick;
        }
    }
    if (target >= tick) target = tick > m_stats.startTick ? tick - 1 : m_stats.startTick;  // always at least one tick back
    if (!retry && !probed && target < m_repair.floor) {
        if (m_repair.floor <= m_stats.startTick) {
            target = m_stats.startTick;
        } else {
            // Everything down to the limit was hopeless: allow one more
            // window, as far as an idea can still be checked against the spot.
            const uint64_t reach =
                (uint64_t)std::lround(std::min(kMaxWidenSeconds, kMaxRepairSeconds + (double)widenLevel()) * tps);
            const uint64_t earliest = m_repair.deathTick > reach ? m_repair.deathTick - reach : m_stats.startTick;
            const uint64_t want = m_repair.floor > m_stats.startTick + limit ? m_repair.floor - limit : m_stats.startTick;
            const uint64_t next = std::max(want, std::max(earliest, m_stats.startTick));
            if (next < m_repair.floor) {
                m_stats.escalations++;
                m_repair.floor = next;
                if (target < m_repair.floor) target = m_repair.floor;
                log("nothing works within the limit; looking back as far as tick %llu now", (unsigned long long)m_repair.floor);
            } else {
                // As far back as an idea can still be judged against the spot.
                target = m_repair.floor;
            }
        }
    }
    // No stop before where the game is (it is at the start, or it went back
    // for real as far as the floor): this tick is the stop, decided in place
    // below. It is the floor, so a verdict there that finds nothing widens the
    // spot and walks again (see escalate) instead of ending the search.
    if (target >= tick) {
        target = tick;
        m_repair.floor = std::max(m_repair.floor, tick);
    }

    if (!probed) {
        m_stats.probes++;
        // Remember that what was played from the target on led nowhere: any
        // idea starting the same way is skipped there from now on.
        const size_t idx = (size_t)(target - m_stats.startTick);
        if (idx < m_repair.failed.size()) {
            std::vector<TickInput> prefix(m_repair.failed.begin() + (std::ptrdiff_t)idx, m_repair.failed.end());
            // Only as far as the decision at this stop itself played: a longer
            // prefix runs into the next decisions' plans, and a trimmed one at
            // that, so no fresh idea here ever matched it and the same one was
            // picked again at every visit.
            const auto played = m_played.find(target);
            const int length = std::min<int>((int)prefix.size(), played != m_played.end() ? std::max(1, played->second.length)
                                                                                          : std::max(1, m_prefixLength));
            Tabu t{length, hashPrefix(prefix, length)};
            // In the dual part of a two-player level it is the pair of prefixes that led nowhere.
            if (idx < m_repair.failedPair.size() && m_repair.failedPair[idx] && idx < m_repair.failed2.size()) {
                std::vector<TickInput> prefix2(m_repair.failed2.begin() + (std::ptrdiff_t)idx, m_repair.failed2.end());
                t.hash2 = hashPrefix(prefix2, length);
                t.pair = true;
            }
            // Once each: every walk of a widened spot comes back to the stop,
            // and a copy per visit had every idea there hash its prefix again.
            auto& forbidden = m_tabu[target];
            auto forbid = [&forbidden](const Tabu& x) {
                for (const Tabu& y : forbidden) {
                    if (y.length == x.length && y.hash == x.hash && y.hash2 == x.hash2 && y.pair == x.pair) return;
                }
                forbidden.push_back(x);
            };
            forbid(t);
            if (played != m_played.end()) forbid(played->second);  // the idea as it was generated, before the trim
        }
        m_visits[target]++;

        if (target == tick) {
            // The stop is where the game is: decided from the real state, with
            // what failed from here forbidden above (in realtime the frame's
            // last ticks ran on past the death: back to it first).
            if (currentTick() > tick && rewindTo(tick)) return;
            if (m_probe.active) endProbe();
            m_decision = Decision{};
            m_pairSecondPass = false;
            m_plan.clear();
            m_plan2.clear();
            m_planIndex = 0;
            log("stop %d: nothing before tick %llu to go back to; deciding there again (%llu before the dead end, level %d)",
                m_repair.probe, (unsigned long long)tick,
                (unsigned long long)(m_repair.deathTick > tick ? m_repair.deathTick - tick : 0), widenLevel());
            m_phase = Phase::Deciding;
            return;
        }

        // The stop is looked at from the kept start first, in the
        // simulation; the game only goes back for real when there is
        // something to play there (or the start is out of reach).
        if (const Start* st = startAt(target); st && st->sim) {
            m_probe = Probe{};
            m_probe.active = true;
            m_probe.tick = target;
            m_probe.held = Trajectory::startHeld(*st->sim);
            m_probe.flying = Trajectory::startFlying(*st->sim);
            {
                auto* pll = PlayLayer::get();
                m_probe.pair = Trajectory::startCount(*st->sim) == 2 && pll && pll->m_levelSettings &&
                               pll->m_levelSettings->m_twoPlayerMode;
                m_probe.held2 = Trajectory::startHeld2(*st->sim);
                m_probe.flying2 = Trajectory::startFlying2(*st->sim);
            }
            bot->trajectory().useStart(st->sim);
            m_decision = Decision{};
            m_pairSecondPass = false;
            if (retry) {
                log("looking at tick %llu again (%llu before the dead end)", (unsigned long long)target,
                    (unsigned long long)(m_repair.deathTick - target));
            } else {
                log("stop %d: looking at tick %llu (%llu before the dead end)", m_repair.probe, (unsigned long long)target,
                    (unsigned long long)(m_repair.deathTick - target));
            }
            m_phase = Phase::Probing;
            return;
        }
    }
    m_stats.backtracks++;

    // Leaving the furthest point reached: keep its replay before it is cut.
    if (tick >= m_stats.bestTick) rememberBest();

    const uint64_t distance = tick - target;
    m_backtrackTarget = target;
    m_restepTo = target;
    m_expectReset = true;
    m_restorePending = true;
    m_restepping = false;
    m_wouldDie = false;

    // The latest stored frame at or before the target; the ticks from it to
    // the target are stepped again from the history once it is loaded.
    int stored = -1;
    for (int i = (int)pf.m_storedFrames.size() - 1; i >= 0; i--) {
        if (pf.m_storedFrames[(size_t)i].frame <= target && pf.m_storedFrames[(size_t)i].frame >= m_stats.startTick) {
            stored = i;
            break;
        }
    }
    if (stored >= 0) {
        const uint64_t landing = pf.m_storedFrames[(size_t)stored].frame;
        if (retry) {
            log("back %llu ticks to %llu again (%llu before the dead end)", (unsigned long long)distance, (unsigned long long)target,
                (unsigned long long)(m_repair.deathTick - target));
        } else {
            log("stop %d: back %llu ticks to %llu (%llu before the dead end)", m_repair.probe, (unsigned long long)distance,
                (unsigned long long)target, (unsigned long long)(m_repair.deathTick - target));
        }
        m_backtrackTarget = landing;
        updater.backwardsStep((int)(pf.m_storedFrames.size() - (size_t)stored));
    } else {
        // Further than the stored frames reach: the closest kept state.
        const Anchor* anchor = nullptr;
        for (auto it = m_anchors.rbegin(); it != m_anchors.rend(); ++it) {
            if (it->tick <= target) {
                anchor = &*it;
                break;
            }
        }
        if (!anchor && m_hasStartAnchor && m_startAnchor.tick <= target) anchor = &m_startAnchor;
        if (!anchor) {
            // Nothing kept at or before the stop (the start's own kept state
            // could not be made): the nearest stored frame after it instead, or,
            // with none before where the game is, this tick as the floor - so a
            // verdict that finds nothing widens the spot (see escalate) instead
            // of ending the search.
            int later = -1;
            for (int i = 0; i < (int)pf.m_storedFrames.size(); i++) {
                const uint64_t f = pf.m_storedFrames[(size_t)i].frame;
                if (f > target && f < tick && f >= m_stats.startTick) {
                    later = i;
                    break;
                }
            }
            if (later >= 0) {
                const uint64_t landing = pf.m_storedFrames[(size_t)later].frame;
                log("stop %d: nothing kept at or before tick %llu; back to the stored frame at %llu instead", m_repair.probe,
                    (unsigned long long)target, (unsigned long long)landing);
                m_backtrackTarget = landing;
                m_restepTo = landing;
                updater.backwardsStep((int)(pf.m_storedFrames.size() - (size_t)later));
                m_phase = Phase::Restoring;
                return;
            }
            m_expectReset = false;
            m_restorePending = false;
            m_restepping = false;
            m_repair.floor = std::max(m_repair.floor, tick);
            if (currentTick() > tick && rewindTo(tick)) return;  // realtime ran on past the death: back to it first
            if (m_probe.active) endProbe();
            m_decision = Decision{};
            m_pairSecondPass = false;
            log("stop %d: nothing kept at or before tick %llu and no stored frame before tick %llu; deciding there (level %d)",
                m_repair.probe, (unsigned long long)target, (unsigned long long)tick, widenLevel());
            m_phase = Phase::Deciding;
            return;
        }
        m_backtrackTarget = anchor->tick;
        log("stop %d: back to the kept state at %llu (%llu before the dead end)", m_repair.probe,
            (unsigned long long)anchor->tick, (unsigned long long)(m_repair.deathTick - anchor->tick));
        const SavedCheckpoint* state = &anchor->state;
        updater.scheduleFrozenFunction([state](float) {
            auto* bot = Bot::get();
            auto* pl = PlayLayer::get();
            if (!pl) return;
            auto& fix = bot->practiceFix();
            fix.clearStoredFrames();
            fix.m_isBackstep = true;
            fix.resetWithState(*state);
        });
    }
    m_phase = Phase::Restoring;
}

void AbsensePathfinder::finishRestore() {
    const auto t0 = std::chrono::steady_clock::now();
    const double realBefore = m_stats.secondsReal;
    struct RestoreTimer {
        Stats& st;
        std::chrono::steady_clock::time_point at;
        double realBefore;
        ~RestoreTimer() {
            st.secondsReal = realBefore;  // the re-stepped ticks are part of going back
            st.secondsRestore += secondsSince(at);
        }
    } restoreTimer{m_stats, t0, realBefore};
    if (!m_restepping) {
        Bot::get()->trajectory().realStateChanged();  // the game was put back: the player is another state
    }
    uint64_t tick = currentTick();
    if (!m_restepping && (tick > m_backtrackTarget + 2 || tick + 2 < m_backtrackTarget)) {
        log("restore landed at %llu instead of %llu", (unsigned long long)tick, (unsigned long long)m_backtrackTarget);
    }
    // From the stored frame to the tick that was asked for: the same inputs
    // again (they are real ticks, recorded like the first time).
    // Landing on a kept state puts this hundreds to thousands of whole game
    // ticks short of the stop, each one a scheduler update over the whole
    // level and a kept start: one indivisible lump of hundreds of
    // milliseconds, with nothing drawn in it. It is cut at the slice's
    // deadline and carried on next frame, with the phase left at Restoring so
    // nothing below runs until the last tick is done - the same thing the
    // Committing phase already does when the budget runs out mid-plan.
    if (m_restepTo > tick && PlayLayer::get()) {
        auto* pl = PlayLayer::get();
        if (!m_restepping) dropStartsAfter(tick);
        m_restepping = true;
        bool firstStep = true;  // always at least one, or nothing would ever get done
        while (currentTick() < m_restepTo && pl->m_player1 && !pl->m_player1->m_isDead && !pl->m_playerDied) {
            if (!firstStep && std::chrono::steady_clock::now() >= m_deadline) {
                m_wouldDie = false;
                return;  // still Phase::Restoring: the next slice carries on from here
            }
            firstStep = false;
            const size_t idx = (size_t)(currentTick() - m_stats.startTick);
            const TickInput in = idx < m_history.size() ? m_history[idx] : TickInput{0, playerHeld()};
            applyInput(in);
            if (idx < m_history2.size() && separateNow()) applyInput(m_history2[idx], true);
            m_wouldDie = false;
            stepGame();
            if (m_wouldDie) break;  // the game did not agree with itself; decide from here
            keepStart();
        }
        m_wouldDie = false;
        tick = currentTick();
    }
    m_restepping = false;
    const size_t idx = (size_t)(tick > m_stats.startTick ? tick - m_stats.startTick : 0);
    if (m_history.size() > idx) m_history.resize(idx);
    if (m_history2.size() > idx) m_history2.resize(idx);
    if (m_historyPair.size() > idx) m_historyPair.resize(idx);
    dropAnchorsAfter(tick);
    dropStartsAfter(tick);
    dropLessonsAfter(tick);  // the path from here changes: nothing past here is learned
    std::erase_if(m_played, [tick](const auto& e) { return e.first > tick; });  // those decisions are void
    m_plan.clear();
    m_plan2.clear();
    m_planIndex = 0;
    m_planTrace.clear();  // the plan is gone with it: a decision made ahead appends its trace to what is here
    m_wouldDie = false;
    m_pairSecondPass = false;
    if (m_probe.found && m_probe.tick == tick && !m_probe.plan.empty()) {
        // Back where the probe found a way on: play it.
        m_plan = std::move(m_probe.plan);
        m_plan2 = std::move(m_probe.plan2);
        m_planTrace = std::move(m_probe.trace);
        m_planIndex = 0;
        m_lastReplanTick = tick;
        // A try played to let the game speak stays one: reset here, its death
        // was "not seen", filed as a phantom, and the killer never confirmed.
        m_planKnownDeath = m_probe.knownDeath;
        m_stats.lastDecision = m_probe.decision;
        if (m_probe.hasLesson) m_lessons.push_back(std::move(m_probe.lesson));  // after the drop above: the stale ones are gone
        m_probe = Probe{};
        m_aheadFailed = false;  // a plan of its own: the decision about its end is worth making again
        if (m_repair.active) resumeFrom(tick);
        m_phase = Phase::Committing;
        // Realtime: the game plays this plan on its own clock too, and the
        // next decision is made about its end meanwhile.
        if (SLSettings::get()->pathfinder.smooth && !m_live && m_planIndex < m_plan.size()) goLive();
        return;
    }
    m_probe = Probe{};
    // Decided from the real state from here. A probe that ended without
    // endProbe (the "nothing lasts even a tick" verdict used to) left its kept
    // start in use, and every run of the decision below started there. (A
    // decision made ahead keeps its own start: the rewind was for it.)
    if (!m_ahead.active) Bot::get()->trajectory().useStart(nullptr);
    // A start that moved back has no kept state of its own yet: this is it.
    if (!m_hasStartAnchor) keepAnchor();
    // ... and no kept start either (the restep keeps one per tick it steps,
    // and it did not step): without one the stop at this tick is never looked
    // at in place but always gone back to for real.
    if (!startAt(tick)) keepStart(tick);
    m_phase = Phase::Deciding;
}

void AbsensePathfinder::saveResult() {
    auto* gb = gucci::GucciEngine::get();
    auto& rs = Bot::get()->replaySystem();
    const bool named = !gb->replayName.empty();
    std::string name;
    auto path = gucciMacroPath(PlayLayer::get(), "", name);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (!named) {
        // A name of our own: numbered past any macro already called that,
        // rather than over it (the classic Pathfinder does the same).
        const std::string base = name;
        const std::string ext = path.extension().string();
        for (int n = 2; std::filesystem::exists(path, ec); ++n) {
            name = fmt::format("{} {}", base, n);
            path = gb->getReplayDir() / (name + ext);
        }
    } else if (gb->replayBackupsEnabled) {
        rs.backupExisting(path);
    }
    // The Macro tab shows (and saves to) this name from now on.
    gb->replayName = name;
    rs.save(path);
    log("saved %s", path.filename().string().c_str());
    Notification::create(fmt::format("Macro saved as \"{}\"", name), NotificationIcon::Success)->show();
}
