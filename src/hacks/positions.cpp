// Possible positions: see positions.hpp. Written fresh for GucciBot
// (2026-11-19); the idea comes from NaN GD, who isn't involved. The runs are the
// fork service's survivesScript (analysis/trajectory.cpp) on Absense's copies
// of the player (Absent, GPL-3 via Silicate), asked exactly as the Survival
// Indicator and the Classic pathfinder ask it.

#include "hacks/positions.hpp"
#include "core/geode_compat.hpp"

#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/judge.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/pathfinder.hpp"
#include "analysis/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "hacks/hitborders.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace geode::prelude;

namespace gucci::positions {

    namespace {

        using Clock = std::chrono::steady_clock;

        // A run goes to the next input plus this, never further than this cap
        // past its own input (in seconds of level time).
        constexpr double kPastNextSec = 1.0;
        constexpr double kCapSec = 2.0;
        // The real path kept for the accuracy test, at most.
        constexpr size_t kMaxReal = 200000;

        // What identifies the attempt and the macro the entries belong to.
        struct Key {
            PlayLayer* layer = nullptr;
            uint32_t resets = 0;
            void const* actions = nullptr;
            size_t count = 0;
            uint32_t lastFrame = 0;
            std::string name;
            bool operator==(Key const&) const = default;
        };

        struct Counters {
            uint64_t runs = 0;
            uint64_t copyTicks = 0;
            uint64_t busy = 0;          // the copies said no (another feature had them)
            uint64_t framesWorked = 0;  // frames in which at least one run went
            uint64_t framesActive = 0;  // drawn frames in which the band was being worked out
            double msTotal = 0.0;       // over the frames worked
            double msMax = 0.0;
            uint64_t overBudget = 0;
            uint64_t entriesMade = 0;
            uint64_t lanesMade = 0;
            uint64_t framesDrawn = 0;
            int lastQuads = 0;
            int lastSegments = 0;
            double drawMsTotal = 0.0;
            double drawMsMax = 0.0;
        };

        struct RealSample {
            uint32_t frame = 0;
            uint32_t resets = 0;
            float x = 0.f;
            float y = 0.f;
        };

        struct State {
            Key key;
            bool haveKey = false;
            std::vector<Input> inputs;      // player 1's jump inputs
            std::vector<size_t> actionOf;   // ... and where each is in the actions
            std::vector<Entry> entries;
            Counters counters;
            double msPerTick = 0.0;         // a run's cost, measured
            const char* reason = "off";  // a literal: setting it costs nothing
            float budgetMs = 1.5f;
            float lookaheadSec = 1.5f;
            bool recordReal = false;
            bool realWasOn = false;
            std::vector<RealSample> real;
            uint32_t realLastFrame = ~0u;
            uint32_t realLastResets = ~0u;
            // The budget is a drawn frame's: the scheduler's update (and this
            // hook with it) can run once per tick inside the engine's tick loop.
            unsigned budgetFrame = ~0u;
            double spentMs = 0.0;  // in this drawn frame
            int runsThisFrame = 0;
            // One run can't be split across frames, and one takes longer than
            // the budget (a copy tick is a few microseconds, a run hundreds of
            // ticks), so the budget is kept on average: each drawn frame adds
            // its budget to this, a run goes only once this covers what it's
            // expected to cost, and what it really cost comes off.
            double credit = 0.0;
            double waitingFor = 0.0;  // the estimate of the run held back
        };

        State& g() {
            static State s;
            return s;
        }

        // The switch and the borders: the band is wanted at all.
        bool wanted(GucciEngine* gb) {
            auto const& hb = HitborderOverlay::get()->settings;
            return gb->enabled && hb.positions && hb.show;
        }

        // Someone else has the run: no runs, and (Calculate) no band drawn.
        const char* busyWith(GucciEngine* gb) {
            if (gb->fwAnalyzing || gb->analyzerOwnsRun())
                return "Calculate is running";
            if (Pathfinder::get()->active || absense::isRunning())
                return "a pathfinder is running";
            if (absense::judge::active())
                return "the judge is playing a script";
            return nullptr;
        }

        void rebuildInputs(State& s, GucciEngine* gb) {
            s.inputs.clear();
            s.actionOf.clear();
            auto const& actions = gb->replay.m_actionAtom.m_actions;
            for (size_t k = 0; k < actions.size(); ++k) {
                auto const& a = actions[k];
                if (a.m_type != gb::ActionType::Jump || a.m_player2)
                    continue;
                s.inputs.push_back({a.m_frame, a.m_holding});
                s.actionOf.push_back(k);
            }
        }

        // Calculate's counted window for this input, when the macro has one it
        // can use (a hand-typed window, a desynced or hidden mark: no).
        Window windowOf(Input const& in) {
            auto& fw = ::Bot::get()->frameWindow();
            auto const& marks = fw.results();
            auto it = std::lower_bound(marks.begin(), marks.end(), in.frame,
                                       [](FrameWindowMark const& m, uint32_t f) { return m.frame < f; });
            for (; it != marks.end() && it->frame == in.frame; ++it) {
                if (it->player2 || it->release == in.press)
                    continue;
                bool const usable = !it->manual && !it->desynced && !it->hidden;
                // A sub-tick mark's low and high are in slots; the mark doesn't
                // keep how many a tick, but its count does: about
                // (high - low + 1) slots over `subframe` ticks.
                double perTick = 0.0;
                if (it->cbf && it->subframe > 0.f)
                    perTick = static_cast<double>(it->high - it->low + 1) / static_cast<double>(it->subframe);
                return windowFor(usable, it->low, it->high, it->cbf, perTick);
            }
            return Window{};
        }

        Entry makeEntry(State& s, size_t i, double tps) {
            Entry e;
            e.input = i;
            e.action = s.actionOf[i];
            e.frame = s.inputs[i].frame;
            e.press = s.inputs[i].press;
            e.window = windowOf(s.inputs[i]);
            auto shifts = pickShifts(e.window);
            // The macro as it is first: if the copies can't live through it,
            // the other timings aren't worth running.
            e.shifts.push_back(0);
            for (int sh : shifts)
                if (sh != 0)
                    e.shifts.push_back(sh);
            e.horizon = horizonFor(s.inputs, i, static_cast<int>(std::lround(kPastNextSec * tps)),
                                   static_cast<int>(std::lround(kCapSec * tps)));
            e.nextFrame = i + 1 < s.inputs.size() ? s.inputs[i + 1].frame : 0;
            s.counters.entriesMade++;
            return e;
        }

        void finish(State& s, Entry& e) {
            e.done = true;
            if (!e.nominalFailed && !e.runs.empty() && e.runs.front().shift == 0) {
                e.lane = envelope(e.runs, 0);
                if (!e.lane.empty())
                    s.counters.lanesMade++;
            }
            e.runs.clear();
            e.runs.shrink_to_fit();
        }

        // One run of `e` at its next shift. False when the copies were busy
        // (nothing consumed: the next frame tries again).
        bool step(State& s, Entry& e, PlayLayer* pl, uint32_t now) {
            int const shift = e.shifts[e.nextShift];
            if (!shiftAllowed(s.inputs, e.input, shift, now) || e.horizon <= now) {
                e.skipped++;
                if (shift == 0)
                    e.nominalFailed = true;  // the input is behind us: nothing to measure against
                e.nextShift++;
                return true;
            }
            int const ticks = static_cast<int>(e.horizon - now);
            auto const script = scriptFor(s.inputs, e.input, shift, now, e.horizon);
            std::vector<CCPoint> path;
            auto const t0 = Clock::now();
            int const lived =
                TrajectoryPredictionService::get().survivesScript(pl, pl->m_player1, ticks, script, &path);
            double const ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (lived < 0) {
                s.counters.busy++;
                return false;
            }
            s.counters.runs++;
            s.counters.copyTicks += static_cast<uint64_t>(path.size());
            if (!path.empty()) {
                double const per = ms / static_cast<double>(path.size());
                s.msPerTick = s.msPerTick > 0.0 ? s.msPerTick * 0.8 + per * 0.2 : per;
            }
            e.ran++;
            e.nextShift++;
            int const counted = countedPoints(lived, ticks, now, e.nextFrame);
            if (counted < 0) {
                e.died++;
                e.diedShifts.push_back(shift);
                if (shift == 0)
                    e.nominalFailed = true;
                return true;
            }
            Run r;
            r.shift = shift;
            r.firstFrame = now + 1;
            size_t const n = std::min(path.size(), static_cast<size_t>(counted));
            r.points.reserve(n);
            for (size_t k = 0; k < n; ++k)
                r.points.push_back({path[k].x, path[k].y});
            e.runs.push_back(std::move(r));
            e.counted++;
            e.countedShifts.push_back(shift);
            return true;
        }

        void recordRealTick(State& s, GucciEngine* gb, PlayLayer* pl) {
            if (s.recordReal != s.realWasOn) {
                // Turned on: a fresh recording.
                if (s.recordReal) {
                    s.real.clear();
                    s.realLastFrame = s.realLastResets = ~0u;
                }
                s.realWasOn = s.recordReal;
            }
            if (!s.recordReal || !pl || !pl->m_player1 || s.real.size() >= kMaxReal)
                return;
            uint32_t const f = gb->updater.getFrame();
            uint32_t const r = gb->updater.m_resetCount;
            if (f == s.realLastFrame && r == s.realLastResets)
                return;
            s.realLastFrame = f;
            s.realLastResets = r;
            auto const p = pl->m_player1->getPosition();
            s.real.push_back({f, r, p.x, p.y});
        }

        // A new drawn frame: the last one's work goes into the counters.
        void closeFrame(State& s) {
            unsigned const frame = CCDirector::get()->getTotalFrames();
            if (frame == s.budgetFrame)
                return;
            if (s.runsThisFrame > 0) {
                s.counters.framesWorked++;
                s.counters.msTotal += s.spentMs;
                s.counters.msMax = std::max(s.counters.msMax, s.spentMs);
                if (s.spentMs > std::clamp(static_cast<double>(s.budgetMs), 0.1, 50.0))
                    s.counters.overBudget++;
            }
            double const budget = std::clamp(static_cast<double>(s.budgetMs), 0.1, 50.0);
            s.counters.framesActive++;
            // Never more saved up than two frames' worth, or the run waiting.
            s.credit = std::min(s.credit + budget, std::max(2.0 * budget, s.waitingFor));
            s.budgetFrame = frame;
            s.spentMs = 0.0;
            s.runsThisFrame = 0;
        }

        void afterFrame() {
            auto& s = g();
            auto* gb = GucciEngine::get();
            // Off (and no test recording the real path): three bools and out.
            // This runs on every scheduler update, Calculate's and the tick
            // loop's included.
            bool const want = wanted(gb);
            if (!want && !s.recordReal && !s.realWasOn) {
                if (s.haveKey || !s.entries.empty())
                    clear();
                s.reason = "off";
                return;
            }
            if (gb->updater.m_onlyRefresh || gb->updater.m_predicting)
                return;
            auto* pl = PlayLayer::get();
            if ((s.recordReal || s.realWasOn) && pl && pl->m_player1 &&
                !::Bot::get()->trajectory().isFakePlayer(pl->m_player1))
                recordRealTick(s, gb, pl);

            if (!want) {
                if (s.haveKey || !s.entries.empty())
                    clear();
                s.reason = "off";
                return;
            }
            if (!pl) {
                if (s.haveKey)
                    clear();
                s.reason = "no level";
                return;
            }
            if (!gb->isPlaying()) {
                if (s.haveKey)
                    clear();
                s.reason = "no macro playing";
                return;
            }
            if (auto const* why = busyWith(gb)) {
                s.reason = why;
                return;
            }
            Key key{pl,
                    gb->updater.m_resetCount,
                    gb->replay.m_actionAtom.m_actions.data(),
                    gb->replay.m_actionAtom.m_actions.size(),
                    gb->replay.m_actionAtom.m_actions.empty() ? 0u : gb->replay.m_actionAtom.m_actions.back().m_frame,
                    gb->replay.m_replayName};
            if (!s.haveKey || !(key == s.key)) {
                // Every new attempt reads the clicks afresh: the Frame Editor
                // moves an input in place (edit_core), which changes nothing
                // the key can see. One pass over the actions, once an attempt.
                s.entries.clear();
                rebuildInputs(s, gb);
                s.key = key;
                s.haveKey = true;
            }
            PlayerObject* p1 = pl->m_player1;
            if (!p1 || ::Bot::get()->trajectory().isFakePlayer(p1)) {
                s.reason = "no player";
                return;
            }
            if (p1->m_isDead || pl->m_playerDied || pl->m_levelEndAnimationStarted || !pl->m_started) {
                s.reason = "the player isn't playing (dead, finished or not started)";
                return;
            }
            if (p1->m_isPlatformer) {
                s.reason = "platformer isn't covered yet";
                return;
            }
            if (s.inputs.empty()) {
                s.reason = "the macro has no player 1 clicks";
                return;
            }
            s.reason = "working";

            uint32_t const now = gb->updater.getFrame();
            double const tps = std::max(1.0, gb->updater.getTps());
            auto const keepBehind = static_cast<uint32_t>(std::max(HitborderOverlay::get()->settings.pathTicks, 1));
            auto const ahead = static_cast<uint32_t>(std::lround(std::clamp(s.lookaheadSec, 0.1f, 10.f) * tps));

            // Behind the player by more than the path's length: gone.
            std::erase_if(s.entries, [&](Entry const& e) {
                uint32_t const end = e.done && !e.lane.empty() ? e.lane.endFrame() : e.horizon;
                return end + keepBehind < now;
            });
            // Coming up within the look-ahead, not made yet.
            size_t const firstNew = s.entries.empty() ? 0 : s.entries.back().input + 1;
            auto from = std::upper_bound(s.inputs.begin(), s.inputs.end(), now,
                                         [](uint32_t f, Input const& in) { return f < in.frame; });
            for (size_t i = std::max(firstNew, static_cast<size_t>(from - s.inputs.begin()));
                 i < s.inputs.size() && s.inputs[i].frame <= now + ahead; ++i)
                s.entries.push_back(makeEntry(s, i, tps));

            // The runs, one at a time, nearest input first, under the budget.
            closeFrame(s);
            auto const t0 = Clock::now();
            int runs = 0;
            for (auto& e : s.entries) {
                if (e.done)
                    continue;
                while (e.nextShift < e.shifts.size()) {
                    if (e.nominalFailed) {
                        e.nextShift = e.shifts.size();
                        break;
                    }
                    double const estimate = s.msPerTick * static_cast<double>(e.horizon > now ? e.horizon - now : 0);
                    if (estimate > s.credit) {
                        s.waitingFor = estimate;
                        goto out;
                    }
                    auto const r0 = Clock::now();
                    if (!step(s, e, pl, now))
                        goto out;
                    s.credit -= std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
                    s.waitingFor = 0.0;
                    runs++;
                }
                finish(s, e);
            }
        out:
            if (runs > 0) {
                s.spentMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                s.runsThisFrame += runs;
            }
        }

        matjson::Value laneJson(Lane const& lane, bool samples, uint32_t from, uint32_t to) {
            auto out = matjson::Value::object();
            out["start"] = static_cast<int64_t>(lane.startFrame);
            out["end"] = static_cast<int64_t>(lane.endFrame());
            out["runs"] = static_cast<int64_t>(lane.runs);
            out["x_breaks"] = static_cast<int64_t>(lane.xBreaks);
            out["max_width"] = static_cast<double>(lane.maxWidth);
            out["x0"] = static_cast<double>(lane.x0);
            out["x1"] = static_cast<double>(lane.x1);
            out["y0"] = static_cast<double>(lane.y0);
            out["y1"] = static_cast<double>(lane.y1);
            if (samples) {
                auto arr = matjson::Value::array();
                for (size_t k = 0; k < lane.samples.size(); ++k) {
                    uint32_t const f = lane.startFrame + static_cast<uint32_t>(k);
                    if ((from && f < from) || (to && f > to))
                        continue;
                    auto const& v = lane.samples[k];
                    auto row = matjson::Value::array();
                    row.push(static_cast<int64_t>(f));
                    row.push(static_cast<double>(v.x));
                    row.push(static_cast<double>(v.yMin));
                    row.push(static_cast<double>(v.yMax));
                    arr.push(row);
                }
                out["samples"] = arr;  // [frame, x, yMin, yMax]
            }
            return out;
        }

    } // namespace

    std::vector<Entry> const& entries() {
        return g().entries;
    }

    bool shouldDraw() {
        auto* gb = GucciEngine::get();
        if (!wanted(gb) || gb->fwAnalyzing || gb->analyzerOwnsRun() || g().entries.empty())
            return false;
        // Not through a death and its respawn delay, nor the level's end: the
        // band is where you could go next, and there is no next. (The entries
        // are dropped at the respawn, when the reset count moves.)
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 || pl->m_player1->m_isDead || pl->m_playerDied || pl->m_levelEndAnimationStarted)
            return false;
        return true;
    }

    void clear() {
        auto& s = g();
        s.entries.clear();
        s.inputs.clear();
        s.actionOf.clear();
        s.haveKey = false;
        s.key = Key{};
    }

    void noteDrawn(int quads, int segments, double ms) {
        auto& c = g().counters;
        c.framesDrawn++;
        c.lastQuads = quads;
        c.lastSegments = segments;
        c.drawMsTotal += ms;
        c.drawMsMax = std::max(c.drawMsMax, ms);
    }

    float& budgetMs() {
        return g().budgetMs;
    }

    float& lookaheadSec() {
        return g().lookaheadSec;
    }

    bool& recordReal() {
        return g().recordReal;
    }

    matjson::Value describe(bool samples, uint32_t from, uint32_t to) {
        auto& s = g();
        auto* gb = GucciEngine::get();
        auto const& hb = HitborderOverlay::get()->settings;
        auto out = matjson::Value::object();
        out["switch"] = hb.positions;
        out["hitborders_shown"] = hb.show;
        out["playing"] = gb->isPlaying();
        out["reason"] = std::string(s.reason);
        out["draw_nodes"] = HitborderOverlay::get()->positionsNodesMade();
        out["drawing"] = shouldDraw();
        out["frame"] = static_cast<int64_t>(gb->updater.getFrame());
        // Drawn frames so far (cocos' count), for a frame rate over a stretch.
        out["director_frames"] = static_cast<int64_t>(CCDirector::get()->getTotalFrames());
        out["budget_ms"] = static_cast<double>(s.budgetMs);
        out["lookahead_s"] = static_cast<double>(s.lookaheadSec);
        out["inputs"] = static_cast<int64_t>(s.inputs.size());

        auto c = matjson::Value::object();
        c["runs"] = static_cast<int64_t>(s.counters.runs);
        c["copy_ticks"] = static_cast<int64_t>(s.counters.copyTicks);
        c["busy"] = static_cast<int64_t>(s.counters.busy);
        c["frames_worked"] = static_cast<int64_t>(s.counters.framesWorked);
        c["ms_avg"] = s.counters.framesWorked ? s.counters.msTotal / static_cast<double>(s.counters.framesWorked) : 0.0;
        c["ms_max"] = s.counters.msMax;
        c["ms_total"] = s.counters.msTotal;
        c["frames_over_budget"] = static_cast<int64_t>(s.counters.overBudget);
        c["frames_active"] = static_cast<int64_t>(s.counters.framesActive);
        c["ms_per_active_frame"] =
            s.counters.framesActive ? s.counters.msTotal / static_cast<double>(s.counters.framesActive) : 0.0;
        c["credit_ms"] = s.credit;
        c["ms_per_copy_tick"] = s.msPerTick;
        c["entries_made"] = static_cast<int64_t>(s.counters.entriesMade);
        c["lanes_made"] = static_cast<int64_t>(s.counters.lanesMade);
        c["frames_drawn"] = static_cast<int64_t>(s.counters.framesDrawn);
        c["last_quads"] = static_cast<int64_t>(s.counters.lastQuads);
        c["last_segments"] = static_cast<int64_t>(s.counters.lastSegments);
        c["draw_ms_avg"] = s.counters.framesDrawn ? s.counters.drawMsTotal / static_cast<double>(s.counters.framesDrawn)
                                                  : 0.0;
        c["draw_ms_max"] = s.counters.drawMsMax;
        out["counters"] = c;

        auto list = matjson::Value::array();
        for (auto const& e : s.entries) {
            if (from && e.frame + 600 < from)
                continue;
            if (to && e.frame > to)
                continue;
            auto j = matjson::Value::object();
            j["action"] = static_cast<int64_t>(e.action);
            j["input"] = static_cast<int64_t>(e.input);
            j["frame"] = static_cast<int64_t>(e.frame);
            j["press"] = e.press;
            j["window_low"] = static_cast<int64_t>(e.window.low);
            j["window_high"] = static_cast<int64_t>(e.window.high);
            j["from_mark"] = e.window.fromMark;
            auto sh = matjson::Value::array();
            for (int v : e.shifts)
                sh.push(static_cast<int64_t>(v));
            j["shifts"] = sh;
            auto cs = matjson::Value::array();
            for (int v : e.countedShifts)
                cs.push(static_cast<int64_t>(v));
            j["counted_shifts"] = cs;
            auto ds = matjson::Value::array();
            for (int v : e.diedShifts)
                ds.push(static_cast<int64_t>(v));
            j["died_shifts"] = ds;
            j["horizon"] = static_cast<int64_t>(e.horizon);
            j["ran"] = static_cast<int64_t>(e.ran);
            j["counted"] = static_cast<int64_t>(e.counted);
            j["died"] = static_cast<int64_t>(e.died);
            j["skipped"] = static_cast<int64_t>(e.skipped);
            j["nominal_failed"] = e.nominalFailed;
            j["done"] = e.done;
            if (!e.lane.empty())
                j["lane"] = laneJson(e.lane, samples, from, to);
            list.push(j);
        }
        out["entries"] = list;

        auto real = matjson::Value::object();
        real["on"] = s.recordReal;
        real["count"] = static_cast<int64_t>(s.real.size());
        if (samples) {
            auto arr = matjson::Value::array();
            for (auto const& r : s.real) {
                if ((from && r.frame < from) || (to && r.frame > to))
                    continue;
                auto row = matjson::Value::array();
                row.push(static_cast<int64_t>(r.frame));
                row.push(static_cast<int64_t>(r.resets));
                row.push(static_cast<double>(r.x));
                row.push(static_cast<double>(r.y));
                arr.push(row);
            }
            real["samples"] = arr;  // [frame, resets, x, y]
        }
        out["real"] = real;
        return out;
    }

} // namespace gucci::positions

// After the frame's ticks, as the Survival Indicator takes its reading.
class $modify(GBPositionsScheduler, CCScheduler) {
    void update(float dt) override {
        CCScheduler::update(dt);
        gucci::positions::afterFrame();
    }
};
