// The fork service on Absense's simulation (absense/trajectory). See the
// header for what asks it what. Written 2026-10-03; the questions it answers
// are the ones the callers already asked, the answers come from Absense's
// copies of the player (Absent, GPL-3 via Silicate):
//   - the drawn path preview is Absense's own (Trajectory::update), with
//     GucciBot's length and moving-object settings handed to it;
//   - every survival question and Frame Extrapolation's next position are
//     Absense scripted runs (Trajectory::run), which step the copies with
//     the hooks holding the real level off and put the game back after;
//   - the sub-tick preview's two questions are anticroom's, ported onto the
//     same copies (Trajectory::subtickPose / subtickBranch).

#include "analysis/trajectory.hpp"

#include "absense/compat/bot.hpp"
#include "absense/judge.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "analysis/ac/shim.hpp"
#include "core/GucciBot.hpp"

#include <algorithm>
#include <cmath>
#include <span>

using namespace geode::prelude;

namespace gucci {

    namespace {

        // The path preview's length, in ticks at the game's speed: what the
        // Length slider sets, and what the sub-tick branches run for too.
        constexpr int kMinPathTicks = 10;
        constexpr int kMaxPathTicks = 2000;
        // Two paths closer than this everywhere are the same path.
        constexpr float kAgencyGap = 0.01f;

        TrajectoryManager& copies() {
            return ::Bot::get()->trajectory();
        }

        bool jumpHeld(PlayerObject* p) {
            auto const it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            return it != p->m_holdingButtons.end() && it->second;
        }

        // A run that reached the end of the level lived as long as it could.
        int livedFor(RunResult const& r, int maxTicks) {
            if (r.complete && !r.died)
                return maxTicks;
            return r.survived;
        }

        int pathTicks(PlayLayer* pl) {
            int const ticks = std::clamp(GucciEngine::get()->pathLength, kMinPathTicks, kMaxPathTicks);
            float const warp = pl ? pl->m_gameState.m_timeWarp : 1.f;
            // A slowed-down level takes longer to cover the same ground, as
            // Absense's own length does (it divides by the time warp too).
            return std::max(1, (int)std::lround(ticks / std::max(warp, 0.05f)));
        }

        int g_lastKiller = -1;

        void rememberKiller() {
            auto const k = copies().lastKiller();
            g_lastKiller = k.valid ? k.id : -1;
        }

    } // namespace

    TrajectoryPredictionService& TrajectoryPredictionService::get() {
        static TrajectoryPredictionService s_service;
        return s_service;
    }

    bool TrajectoryPredictionService::isActiveSimulation() const {
        auto& m = copies();
        return m.drawing() || m.simulating();
    }

    bool TrajectoryPredictionService::ownsPreviewPlayer(PlayerObject* player) const {
        return player && copies().isFakePlayer(player);
    }

    // ------------------------------------------------------------ the copies

    namespace {
        // Absense's Trajectory puts its line node beside the debug draw node and
        // its copies into the object layer; neither exists before the level is.
        // Only for a level on screen: while a level is being left (onQuit has
        // already taken the copies down) the fade out of it is the running
        // scene, and copies made then would belong to a layer about to go.
        bool canMake(PlayLayer* pl) {
            if (!pl || pl != PlayLayer::get() || !pl->m_player1 || !pl->m_levelSettings || !pl->m_objectLayer ||
                !pl->m_debugDrawNode || !pl->m_debugDrawNode->getParent())
                return false;
            auto* scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
            return scene && pl->getParent() == scene;
        }
    } // namespace

    void TrajectoryPredictionService::attach(PlayLayer* pl) {
        if (!copies().exists() && canMake(pl))
            copies().init();
    }

    void TrajectoryPredictionService::markDirty() {
        if (auto* t = copies().unsafeInner())
            t->invalidateCache();
    }

    void TrajectoryPredictionService::sync(PlayLayer* pl, bool force) {
        // GJBaseGameLayer::update moves m_currentProgress on every tick, and a
        // reset (respawn, restart, step back) moves the reset count: between
        // them they say whether the real player is the one the copies last
        // took. The frame counter does not - it moves in the frame midhook,
        // after the tick, so a question asked before it would match the last
        // tick's.
        Key const key{static_cast<uint32_t>(pl->m_gameState.m_currentProgress),
                      GucciEngine::get()->updater.m_resetCount, pl};
        if (!force && m_haveSync && key == m_synced)
            return;
        // Drops Absense's cached copy of the real player, and samples where the
        // moving objects are (their speed is told from two samples a tick apart).
        copies().realStateChanged();
        m_synced = key;
        m_haveSync = true;
    }

    void TrajectoryPredictionService::onRealTick() {
        auto* pl = PlayLayer::get();
        // Nothing made yet, nothing to keep fresh. Absense's pathfinder keeps
        // its own copies fresh while it runs.
        if (!pl || !copies().exists() || ::Bot::get()->pathfinder().drivesGame())
            return;
        sync(pl, true);
    }

    bool TrajectoryPredictionService::ready(PlayLayer* pl, PlayerObject* player) {
        if (!pl || pl != PlayLayer::get() || !player || !pl->m_levelSettings)
            return false;
        if (player != pl->m_player1 && player != pl->m_player2)
            return false;
        if (player->m_isDead)
            return false;
        // The judge plays a script for real and puts it back; Absense's
        // pathfinder owns the copies while it drives the game (and may have a
        // kept start in use, which every run would begin from instead).
        if (absense::judge::active() || ::Bot::get()->pathfinder().drivesGame())
            return false;
        if (!copies().exists()) {
            if (!canMake(pl))
                return false;
            copies().init();
        }
        auto* t = copies().unsafeInner();
        if (!t || t->searching() || t->usingStart() || t->drawing() || t->simulating())
            return false;
        sync(pl, false);
        return true;
    }

    // ------------------------------------------------------------ the path preview

    void TrajectoryPredictionService::applySettings() {
        auto* gb = GucciEngine::get();
        auto* t = copies().unsafeInner();
        auto& ts = SLSettings::get()->trajectory;
        // Absense measures its line in seconds at 240 TPS or more and draws
        // length * max(tps, 240) / timeWarp ticks; GucciBot's slider is in
        // ticks, so the seconds are worked back from them.
        int const ticks = std::clamp(gb->pathLength, kMinPathTicks, kMaxPathTicks);
        ts.length = ticks / std::max(gb->updater.getTps(), 240.0);
        int const every = std::clamp(gb->pathMoveStepInterval, 1, 30);
        if (t) {
            t->m_displayMovers.on = gb->pathMovingObjects;
            t->m_displayMovers.every = every;
            // The line's cache only knows the settings Absense has; these two
            // are GucciBot's, so a change of either redraws it here.
            if (m_appliedMoving != gb->pathMovingObjects || m_appliedEvery != every)
                t->invalidateCache();
        }
        m_appliedMoving = gb->pathMovingObjects;
        m_appliedEvery = every;
    }

    void TrajectoryPredictionService::setOverlaySuppressed(bool suppressed) {
        m_suppressed = suppressed;
    }

    void TrajectoryPredictionService::updatePreview(PlayLayer* pl) {
        if (!pl) {
            // The level is going (onQuit takes the copies down with it).
            m_haveSync = false;
            m_step[0].valid = m_step[1].valid = false;
            m_suppressed = false;
            return;
        }
        auto* gb = GucciEngine::get();
        auto& m = copies();
        auto& ts = SLSettings::get()->trajectory;
        // Not while Calculate or the Classic pathfinder replay the level (the
        // lines would be redrawn on every one of their ticks), nor while the
        // judge plays a script it will take back.
        bool const want = gb->enabled && gb->pathPreview && !m_suppressed && !gb->fwAnalyzing &&
                          !absense::judge::active();
        if (!want) {
            ts.enabled = false;
            // Hides the line and the copies (Absense's update returns right
            // after that when it is off).
            if (m.exists())
                m.update(pl);
            return;
        }
        if (!m.exists()) {
            if (!canMake(pl))
                return;
            m.init();
        }
        ts.enabled = true;
        applySettings();
        m.update(pl);
    }

    // ------------------------------------------------------------ survival

    int TrajectoryPredictionService::survivesFor(PlayLayer* pl, PlayerObject* player, int maxTicks, int input) {
        if (!ready(pl, player))
            return -1;
        if (maxTicks <= 0)
            return 0;
        bool const down = jumpHeld(player);
        TickInput const in{0, input > 0 ? true : input < 0 ? false : down};
        RunResult const r =
            copies().run(pl, player == pl->m_player1, std::span<const TickInput>(&in, 1), maxTicks, down);
        rememberKiller();
        if (r.simulated == 0)
            return -1;
        return livedFor(r, maxTicks);
    }

    int TrajectoryPredictionService::survivesScript(PlayLayer* pl, PlayerObject* player, int maxTicks,
                                                    std::vector<std::pair<int, bool>> const& events,
                                                    std::vector<cocos2d::CCPoint>* path) {
        if (path)
            path->clear();
        if (!ready(pl, player))
            return -1;
        if (maxTicks <= 0)
            return 0;

        // One input per tick. GucciBot plays an input stamped frame F on the
        // tick that ends at F, so offset k (F = now + k) is tick k, index
        // k - 1. Offset 0 has no tick of its own left and goes on the first.
        // Several events on one tick: a press after a release in it is a press
        // of its own (Absense lets go first when the button is down), a press
        // while held and a release while up change nothing.
        bool const down = jumpHeld(player);
        std::vector<TickInput> inputs((size_t)maxTicks);
        bool held = down;
        size_t next = 0;
        for (int tick = 0; tick < maxTicks; tick++) {
            TickInput in{0, held};
            while (next < events.size() && std::max(events[next].first, 1) - 1 <= tick) {
                if (events[next].second) {
                    if (!in.held)
                        in.presses++;
                    in.held = true;
                } else {
                    in.held = false;
                }
                next++;
            }
            inputs[(size_t)tick] = in;
            held = in.held;
        }

        std::vector<TraceSample> trace;
        RunResult const r = copies().run(pl, player == pl->m_player1, inputs, maxTicks, down,
                                         path ? &trace : nullptr);
        rememberKiller();
        if (r.simulated == 0)
            return -1;
        if (path) {
            path->reserve(trace.size());
            for (auto const& s : trace)
                path->push_back(cocos2d::CCPoint(s.x, s.y));
        }
        return livedFor(r, maxTicks);
    }

    int TrajectoryPredictionService::lastForkKillerId() const {
        return g_lastKiller;
    }

    void TrajectoryPredictionService::noteSimulatedDeath(PlayerObject* player, GameObject* object) {
        // A copy: what Absense's own destroyPlayer branch does
        // (hook_playlayer.cpp), for a hook that reached it first. A real
        // player dying while a copy runs is not a copy's death.
        auto& m = copies();
        if (!player || !m.isFakePlayer(player))
            return;
        m.noteKiller(player, object);
        m.hasDied(player);
    }

    // ------------------------------------------------------------ Frame Extrapolation

    bool TrajectoryPredictionService::predictStep(PlayLayer* pl, PlayerObject* player, bool hold,
                                                  cocos2d::CCPoint& position, float& rotation) {
        if (!pl || !player)
            return false;
        bool const second = player == pl->m_player2;
        // Player 2 is hidden outside the dual part: nothing to draw ahead.
        if (second && !pl->m_gameState.m_isDualMode)
            return false;
        Step& cached = m_step[second ? 1 : 0];
        Key const now{static_cast<uint32_t>(pl->m_gameState.m_currentProgress),
                      GucciEngine::get()->updater.m_resetCount, pl};
        // Every drawn frame between two ticks asks the same question.
        if (cached.valid && cached.key == now && cached.hold == hold) {
            position = cached.position;
            rotation = cached.rotation;
            return true;
        }
        if (!ready(pl, player))
            return false;

        bool const down = jumpHeld(player);
        TickInput const in{0, hold};
        std::vector<TraceSample> trace;
        trace.reserve(1);
        copies().run(pl, !second, std::span<const TickInput>(&in, 1), 1, down, &trace);
        if (trace.empty())
            return false;
        cached.valid = true;
        cached.key = now;
        cached.hold = hold;
        cached.position = cocos2d::CCPoint(trace.front().x, trace.front().y);
        cached.rotation = trace.front().rotation;
        position = cached.position;
        rotation = cached.rotation;
        return true;
    }

    // ------------------------------------------------------------ agency

    bool TrajectoryPredictionService::probeAgency(PlayLayer* pl, PlayerObject* player, AgencyResult& out,
                                                  int frames) {
        auto* gb = GucciEngine::get();
        gb->pfAgencyValid = false;
        if (!ready(pl, player) || frames <= 0)
            return false;

        bool const p1 = player == pl->m_player1;
        bool const down = jumpHeld(player);
        TickInput const hold{0, true};
        TickInput const release{0, false};
        std::vector<TraceSample> held, let;
        held.reserve((size_t)frames);
        let.reserve((size_t)frames);
        RunResult const rh = copies().run(pl, p1, std::span<const TickInput>(&hold, 1), frames, down, &held);
        RunResult const rr = copies().run(pl, p1, std::span<const TickInput>(&release, 1), frames, down, &let);
        if (rh.simulated == 0 || rr.simulated == 0)
            return false;

        float gap = 0.f;
        size_t const n = std::min(held.size(), let.size());
        for (size_t i = 0; i < n; i++)
            gap = std::max(gap, std::hypot(held[i].x - let[i].x, held[i].y - let[i].y));

        out.divergence = gap;
        out.holdSurvived = livedFor(rh, frames);
        out.releaseSurvived = livedFor(rr, frames);
        out.matters = gap > kAgencyGap || out.holdSurvived != out.releaseSurvived;

        // The agency map (pathfinder.cpp's AgencyOverlay) draws from these.
        gb->pfAgencyValid = true;
        gb->pfAgencyMatters = out.matters;
        gb->pfAgencyDivergence = out.divergence;
        gb->pfAgencyHoldSurvived = out.holdSurvived;
        gb->pfAgencyReleaseSurvived = out.releaseSurvived;
        return true;
    }

    // ------------------------------------------------------------ the sub-tick preview

    bool TrajectoryPredictionService::extrapolateSubtick(PlayLayer* pl, PlayerObject* player, float fraction,
                                                         SubtickPose& out) {
        if (!ready(pl, player))
            return false;
        TrajectoryPlayerData data{};
        bool died = false;
        if (!copies().unsafeInner()->subtickPose(pl, player == pl->m_player1, fraction, data, died))
            return false;
        out.position = data.position;
        out.hitbox = data.hitbox;
        out.innerHitbox = data.innerHitbox;
        out.rotation = data.rotation;
        out.died = died;
        return true;
    }

    void TrajectoryPredictionService::traceSubtickBranch(PlayLayer* pl, PlayerObject* player, float fraction,
                                                         bool hold, cocos2d::CCDrawNode* node,
                                                         cocos2d::ccColor4F color, float width) {
        if (!node || !ready(pl, player))
            return;
        // The branches meet moving objects as the path preview does.
        applySettings();
        std::vector<cocos2d::CCPoint> path;
        if (!copies().unsafeInner()->subtickBranch(pl, player == pl->m_player1, fraction, hold, pathTicks(pl),
                                                   path))
            return;
        for (size_t i = 1; i < path.size(); i++)
            node->drawSegment(path[i - 1], path[i], width, color);
    }

} // namespace gucci
