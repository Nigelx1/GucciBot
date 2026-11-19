// The Trainer's lane: its notes, how you did on each, its verdicts and its
// settings. See trainers/lane.hpp; the drawing is ui/pages/trainer_lane.cpp.
// Written fresh for GucciBot (2026-11-17), from Juice's design.

#include "trainers/lane.hpp"

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>

using namespace geode::prelude;

namespace gucci::trainers::lane {

    namespace {

        using namespace gucci::lanerules;

        // A hold this long or longer has a tail even when Calculate didn't
        // count its release: you have to know to keep holding.
        constexpr double kHoldSec = 0.12;
        // How far from a note a press still answers it, and how long after
        // a note passes the hit marker unanswered it counts as missed.
        constexpr double kReachSec = 0.15;

        struct Lane {
            uint64_t generation = UINT64_MAX;
            size_t built = SIZE_MAX;  // the macro's click count it was built for
            std::vector<Note> notes;
            std::vector<double> pressTimes;
            std::vector<uint8_t> answered;
            std::vector<Result> results;
            double maxHoldSec = 0.0;
            int counted = 0;
            int holding = -1;
            size_t missScan = 0;
            Flash flash;
            Tally tally;
            std::vector<View::Recent> recent;
        };
        std::array<Lane, 2> g_lanes;
        // A test's held verdict age (ageFlashForTest), or -1.
        std::array<double, 2> g_heldAge{-1.0, -1.0};
        std::array<double, 2> g_shownAt{-100.0, -100.0};

        Settings g_settings;
        bool g_settingsLoaded = false;

        int idx(Kind k) {
            return k == Kind::Jupiter ? 0 : 1;
        }

        double macroTps(State const& s) {
            return s.macro.clickBarTps > 0.0 ? s.macro.clickBarTps : 240.0;
        }

        double laneTps(Kind k) {
            if (followsLevel(k)) {
                double const tps = GucciEngine::get()->updater.m_tps;
                return tps > 0.0 ? tps : 240.0;
            }
            return macroTps(state(k));
        }

        int okTicks(double tps) {
            return std::max(0, (int)std::lround(GucciEngine::get()->clickIndicatorOkMs * tps / 1000.0));
        }

        int8_t clamp8(int v) {
            return (int8_t)std::clamp(v, -127, 127);
        }

        void resetResults(Lane& l) {
            l.answered.assign(l.notes.size(), 0);
            l.results.assign(l.notes.size(), Result{});
            l.holding = -1;
            l.missScan = 0;
            l.tally = {};
            l.recent.clear();
        }

        Lane& built(Kind k) {
            auto& l = g_lanes[(size_t)idx(k)];
            auto const& m = state(k).macro;
            if (l.generation == m.generation && l.built == m.clickIntervalsSec.size())
                return l;
            l.generation = m.generation;
            l.built = m.clickIntervalsSec.size();
            l.notes.clear();
            l.pressTimes.clear();
            l.maxHoldSec = 0.0;
            l.counted = 0;
            l.flash = {};

            auto const& iv = m.clickIntervalsSec;
            bool const info = m.clicks.size() == iv.size();
            std::vector<size_t> order(iv.size());
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return iv[a].first < iv[b].first; });
            l.notes.reserve(iv.size());
            for (size_t i : order) {
                Note n;
                n.pressSec = iv[i].first;
                n.releaseSec = std::max(iv[i].second, iv[i].first);
                double const hold = n.releaseSec - n.pressSec;
                bool releaseCounted = false, releaseFree = false;
                if (info) {
                    auto const& c = m.clicks[i];
                    n.player2 = c.player2;
                    n.pressWindow = c.pressWindow;
                    n.releaseWindow = c.releaseWindow;
                    n.pressTier = tierFor(c.pressWindow, c.pressFree);
                    n.releaseTier = tierFor(c.releaseWindow, c.releaseFree);
                    n.pressAllow = allowance(c.pressWindow, c.pressLow, c.pressHigh, c.pressFree, c.pressSubTick);
                    n.releaseAllow =
                        allowance(c.releaseWindow, c.releaseLow, c.releaseHigh, c.releaseFree, c.releaseSubTick);
                    releaseCounted = c.releaseWindow > 0.f || c.releaseFree;
                    releaseFree = c.releaseFree;
                    if (n.pressTier != Tier::Unknown)
                        ++l.counted;
                }
                if (releaseCounted && !releaseFree)
                    n.tail = Note::Tail::Slider;
                else if (hold >= kHoldSec)
                    n.tail = releaseFree ? Note::Tail::FreeHold : Note::Tail::Hold;
                l.maxHoldSec = std::max(l.maxHoldSec, hold);
                l.pressTimes.push_back(n.pressSec);
                l.notes.push_back(n);
            }
            resetResults(l);
            return l;
        }

        void flash(Lane& l, std::string text, Verdict v, Tier tier, bool release) {
            l.flash.text = std::move(text);
            l.flash.verdict = v;
            l.flash.tier = tier;
            l.flash.atReal = steadyNow();
            l.flash.release = release;
        }

        void remember(Lane& l, int delta, Verdict v) {
            if (l.recent.size() >= 12)
                l.recent.erase(l.recent.begin());
            l.recent.push_back({clamp8(delta), v, steadyNow()});
        }

        void count(Lane& l, Verdict v) {
            switch (v) {
                case Verdict::Perfect: ++l.tally.perfect; break;
                case Verdict::Inside: ++l.tally.inside; break;
                case Verdict::Outside: ++l.tally.outside; break;
                case Verdict::Missed: ++l.tally.missed; break;
            }
        }

    } // namespace

    double steadyNow() {
        auto const since = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(since).count();
    }

    // ------------------------------------------------------------ settings

    Settings& settings() {
        if (!g_settingsLoaded) {
            g_settingsLoaded = true;
            auto* mod = Mod::get();
            Settings const d;
            // Classic unless Lane was picked and saved (0): the old bar is the
            // default since 2026-11-18.
            g_settings.style = mod->getSavedValue<int64_t>("trainer_lane_style", (int64_t)d.style) == 0
                                   ? Style::Lane
                                   : Style::Classic;
            g_settings.leadTicks =
                (int)std::clamp<int64_t>(mod->getSavedValue<int64_t>("trainer_lane_lead", d.leadTicks), 24, 960);
            g_settings.size = std::clamp((float)mod->getSavedValue<double>("trainer_lane_size", d.size), 0.6f, 2.f);
            g_settings.inLevel = mod->getSavedValue<bool>("trainer_lane_in_level", d.inLevel);
            g_settings.levelWidth =
                std::clamp((float)mod->getSavedValue<double>("trainer_lane_width", d.levelWidth), 0.3f, 1.f);
            // A new key with Auto in it (2026-11-18): a test copy's saved Top
            // from before Auto existed doesn't pin the lane there.
            switch (mod->getSavedValue<int64_t>("trainer_lane_where", (int64_t)d.place)) {
                case 0: g_settings.place = Place::Top; break;
                case 1: g_settings.place = Place::Bottom; break;
                default: g_settings.place = Place::Auto; break;
            }
            g_settings.judgments = mod->getSavedValue<bool>("trainer_lane_judgments", d.judgments);
        }
        return g_settings;
    }

    void saveSettings() {
        auto const& s = settings();
        auto* mod = Mod::get();
        mod->setSavedValue<int64_t>("trainer_lane_style", (int64_t)s.style);
        mod->setSavedValue<int64_t>("trainer_lane_lead", s.leadTicks);
        mod->setSavedValue<double>("trainer_lane_size", s.size);
        mod->setSavedValue<bool>("trainer_lane_in_level", s.inLevel);
        mod->setSavedValue<double>("trainer_lane_width", s.levelWidth);
        mod->setSavedValue<int64_t>("trainer_lane_where", (int64_t)s.place);
        mod->setSavedValue<bool>("trainer_lane_judgments", s.judgments);
    }

    // ------------------------------------------------------------ the clock

    double now(Kind k) {
        if (followsLevel(k))
            return levelTimeSec();
        return at(k, steadyNow());
    }

    double at(Kind k, double steadySec) {
        if (followsLevel(k))
            return levelTimeSec();
        auto s = state(k);
        if (s.paused || !s.barEnabled)
            return s.posSec;
        // posSec is the clock as of this frame's start (tick(), from
        // drawScene); run on to the moment asked about, either side of it.
        double const t = s.posSec + std::clamp(steadySec - s.lastRealTime, -0.25, 0.25);
        double lo = 0.0, hi = 0.0;
        barBounds(k, lo, hi);
        return std::clamp(t, 0.0, std::max(hi, s.posSec));
    }

    // ------------------------------------------------------------ the notes

    View view(Kind k) {
        auto& l = built(k);
        if (g_heldAge[(size_t)idx(k)] >= 0.0)
            l.flash.atReal = steadyNow() - g_heldAge[(size_t)idx(k)];
        View v;
        v.notes = &l.notes;
        v.results = &l.results;
        v.maxHoldSec = l.maxHoldSec;
        v.counted = l.counted;
        v.holding = l.holding;
        v.flash = l.flash;
        v.tally = l.tally;
        v.tps = laneTps(k);
        v.recent = &l.recent;
        return v;
    }

    void onInput(Kind k, bool down, double atSec, double tps) {
        auto& l = built(k);
        if (l.notes.empty())
            return;
        if (down) {
            int const i = match(l.pressTimes, l.answered, atSec, kReachSec);
            if (i < 0) {
                // A press near no note: the marker pulses, nothing is judged.
                l.holding = -1;
                flash(l, "", Verdict::Missed, Tier::Unknown, false);
                return;
            }
            auto const& n = l.notes[(size_t)i];
            auto& r = l.results[(size_t)i];
            Judgment const j = judge(ticksOff(atSec, n.pressSec, tps), n.pressAllow, okTicks(tps));
            l.answered[(size_t)i] = 1;
            r.pressed = true;
            r.pressDelta = clamp8(j.delta);
            r.pressVerdict = j.verdict;
            count(l, j.verdict);
            remember(l, j.delta, j.verdict);
            flash(l, label(j), j.verdict, n.pressTier, false);
            l.holding = i;
            return;
        }
        int const i = l.holding;
        l.holding = -1;
        if (i < 0 || (size_t)i >= l.notes.size())
            return;
        auto const& n = l.notes[(size_t)i];
        auto& r = l.results[(size_t)i];
        if (r.released)
            return;
        r.released = true;
        // Only a slider's release is judged: elsewhere Calculate found the
        // release didn't matter, or didn't count it.
        if (n.tail != Note::Tail::Slider)
            return;
        Judgment const j = judge(ticksOff(atSec, n.releaseSec, tps), n.releaseAllow, okTicks(tps));
        r.releaseDelta = clamp8(j.delta);
        r.releaseVerdict = j.verdict;
        count(l, j.verdict);
        remember(l, j.delta, j.verdict);
        flash(l, label(j), j.verdict, n.releaseTier, true);
    }

    void advance(Kind k, double nowSec) {
        auto& l = built(k);
        while (l.missScan < l.notes.size() && l.notes[l.missScan].pressSec + kReachSec < nowSec) {
            size_t const i = l.missScan++;
            if (l.answered[i])
                continue;
            l.answered[i] = 1;
            l.results[i].missed = true;
            count(l, Verdict::Missed);
            Judgment m;
            m.verdict = Verdict::Missed;
            flash(l, label(m), Verdict::Missed, l.notes[i].pressTier, false);
        }
    }

    void rewind(Kind k, double fromSec) {
        auto& l = built(k);
        double const cut = fromSec - 1e-6;
        for (size_t i = 0; i < l.notes.size(); ++i) {
            if (l.notes[i].pressSec >= cut) {
                l.answered[i] = 0;
                l.results[i] = Result{};
            } else if (l.notes[i].releaseSec >= cut) {
                // Pressed before the cut, released after: the release is open again.
                l.results[i].released = false;
                l.results[i].releaseDelta = 0;
                l.results[i].releaseVerdict = Verdict::Missed;
            }
        }
        auto const first = std::lower_bound(l.pressTimes.begin(), l.pressTimes.end(), cut);
        // Notes before the cut that passed while you were dead stay as they were.
        l.missScan = std::min(l.missScan, (size_t)(first - l.pressTimes.begin()));
        l.holding = -1;
        l.tally = {};
        l.flash = {};
        l.recent.clear();
    }

    void clear(Kind k) {
        auto& l = built(k);
        resetResults(l);
        l.flash = {};
    }

    void ageFlashForTest(Kind k, double ageSec, bool hold) {
        auto& l = g_lanes[(size_t)idx(k)];
        l.flash.atReal = steadyNow() - ageSec;
        g_heldAge[(size_t)idx(k)] = hold ? ageSec : -1.0;
    }

    Placement& placement(Kind k) {
        static Placement s_place[2];
        return s_place[(size_t)idx(k)];
    }

    void markShownInLevel(Kind k) {
        g_shownAt[(size_t)idx(k)] = steadyNow();
    }

    bool shownInLevel(Kind k) {
        return steadyNow() - g_shownAt[(size_t)idx(k)] < 0.25 && followsLevel(k);
    }

} // namespace gucci::trainers::lane
