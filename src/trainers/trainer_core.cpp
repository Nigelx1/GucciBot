// The practice trainers' engine: clock, input, scoring, segments, stats and
// the Trainer's macro and track. See trainer_core.hpp for the shape of it.

#include "trainers/trainer_core.hpp"
#include "core/bot_switch.hpp"
#include "trainers/jupiterghost.hpp"
#include "trainers/trainerghost.hpp"

#include "absense/glue.hpp"
#include "analysis/pathfinder.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>

#include <fmt/format.h>

using namespace geode::prelude;

namespace gucci::trainers {

    namespace fs = std::filesystem;

    namespace {

        constexpr int kKinds = 2;

        int idx(Kind k) {
            return k == Kind::Jupiter ? 0 : 1;
        }

        // Saved-value keys, one prefix per trainer.
        std::string key(Kind k, const char* name) {
            return std::string(k == Kind::Jupiter ? "trainer_jmf_" : "trainer_any_") + name;
        }

        // The Trainer's notes and segments belong to the macro they were made
        // for, so they are saved under its name; switching macros brings back
        // that macro's own.
        std::string perMacroKey(Kind k, const char* name) {
            if (k == Kind::Jupiter)
                return key(k, name);
            return key(k, name) + ":" + GucciEngine::get()->trainerMacroName;
        }

        bool g_loaded = false;
        bool g_macroRestored = false;
        // Frames drawn (drawScene calls) and the frame each page was last drawn on.
        int64_t g_tick = 0;
        std::array<int64_t, kKinds> g_drawnAt{-100, -100};

        // The open level as the per-tick step last saw it.
        PlayLayer* g_stepLevel = nullptr;
        std::array<double, kKinds> g_lastLevelSec{-1.0, -1.0};
        // The segment loop: its checkpoint is down for the range it was put
        // down for (a changed range lays a new one).
        std::array<bool, kKinds> g_loopArmed{false, false};
        std::array<double, kKinds> g_armedStart{-1.0, -1.0};
        std::array<double, kKinds> g_armedEnd{-1.0, -1.0};
        std::array<int, kKinds> g_cleanLaps{0, 0};
        bool g_resetQueued = false;

        struct SegmentCache {
            std::string parsedFrom;
            bool parsed = false;
            std::vector<Segment> list;
        };
        std::array<SegmentCache, kKinds> g_segments;

        double steadySeconds() {
            auto const since = std::chrono::steady_clock::now().time_since_epoch();
            return std::chrono::duration<double>(since).count();
        }

        // Calculate and both pathfinders reset and rewind the level constantly
        // and own it while they do; none of that is the player practising.
        bool levelBusy() {
            auto* gb = GucciEngine::get();
            return gb->fwAnalyzing || absense::isRunning() || absense::startPending() ||
                   Pathfinder::get()->active;
        }

        double macroTps(State const& s) {
            return s.macro.clickBarTps > 0.0 ? s.macro.clickBarTps : 240.0;
        }

        double gameTps() {
            double const tps = GucciEngine::get()->updater.m_tps;
            return tps > 0.0 ? tps : 240.0;
        }

        // Drops the player's clicks at or after `fromSec` and frees the macro
        // clicks there for matching again, as if that stretch had not been
        // played. The tallies start over: they describe one go at it.
        void rewindComparison(State& s, double fromSec) {
            double const cut = fromSec - 1e-6;
            std::erase_if(s.myPresses, [&](double t) { return t >= cut; });
            std::erase_if(s.myReleases, [&](double t) { return t >= cut; });
            auto const& iv = s.macro.clickIntervalsSec;
            auto& sc = s.score;
            if (sc.answeredPress.size() == iv.size() && sc.answeredRelease.size() == iv.size()) {
                for (size_t i = 0; i < iv.size(); ++i) {
                    if (iv[i].first >= cut)
                        sc.answeredPress[i] = false;
                    if (iv[i].second >= cut)
                        sc.answeredRelease[i] = false;
                }
            }
            sc.perfect = sc.ok = sc.miss = 0;
            sc.hasLastReading = false;
        }

        void record(Kind k, bool down, double atSec, double tps) {
            auto s = state(k);
            (down ? s.myPresses : s.myReleases).push_back(atSec);
            if (!s.macro.clickIntervalsSec.empty())
                GucciEngine::get()->scoreRealClick(s.macro.clickIntervalsSec, s.score, atSec, down, tps);
        }

        // ---- segment text
        //
        // One segment a line: name, start, end, note, tab-separated, with tab,
        // newline and backslash escaped inside the text fields.

        std::string escapeField(std::string const& in) {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                if (c == '\\')
                    out += "\\\\";
                else if (c == '\t')
                    out += "\\t";
                else if (c == '\n')
                    out += "\\n";
                else if (c != '\r')
                    out += c;
            }
            return out;
        }

        std::string unescapeField(std::string const& in) {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); ++i) {
                if (in[i] == '\\' && i + 1 < in.size()) {
                    char const n = in[++i];
                    out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
                } else {
                    out += in[i];
                }
            }
            return out;
        }

        std::string serialize(std::vector<Segment> const& list) {
            std::string out;
            for (auto const& seg : list) {
                out += escapeField(seg.name);
                out += '\t';
                out += fmt::format("{:.4f}", seg.startSec);
                out += '\t';
                out += fmt::format("{:.4f}", seg.endSec);
                out += '\t';
                out += escapeField(seg.note);
                out += '\n';
            }
            return out;
        }

        bool parseNumber(std::string const& text, double& out) {
            if (text.empty())
                return false;
            char* end = nullptr;
            out = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && std::isfinite(out);
        }

        // Lines that don't read (or name an empty or backwards range) are
        // skipped; how many were is returned through `bad`.
        std::vector<Segment> parse(std::string const& text, int* bad = nullptr) {
            std::vector<Segment> list;
            int skipped = 0;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                std::vector<std::string> f;
                size_t from = 0;
                for (;;) {
                    size_t const tab = line.find('\t', from);
                    f.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                    if (tab == std::string::npos)
                        break;
                    from = tab + 1;
                }
                Segment seg;
                if (f.size() < 3 || !parseNumber(f[1], seg.startSec) || !parseNumber(f[2], seg.endSec) ||
                    seg.startSec < 0.0 || seg.endSec <= seg.startSec) {
                    ++skipped;
                    continue;
                }
                seg.name = unescapeField(f[0]);
                if (f.size() > 3)
                    seg.note = unescapeField(f[3]);
                list.push_back(std::move(seg));
            }
            if (bad)
                *bad = skipped;
            return list;
        }

        // ---- base64, for share codes that survive being pasted into a chat

        constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string toBase64(std::string const& in) {
            std::string out;
            out.reserve((in.size() + 2) / 3 * 4);
            size_t i = 0;
            for (; i + 2 < in.size(); i += 3) {
                uint32_t const v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
                out += kB64[v >> 18 & 63];
                out += kB64[v >> 12 & 63];
                out += kB64[v >> 6 & 63];
                out += kB64[v & 63];
            }
            if (size_t const rest = in.size() - i; rest > 0) {
                uint32_t v = (uint8_t)in[i] << 16;
                if (rest == 2)
                    v |= (uint8_t)in[i + 1] << 8;
                out += kB64[v >> 18 & 63];
                out += kB64[v >> 12 & 63];
                out += rest == 2 ? kB64[v >> 6 & 63] : '=';
                out += '=';
            }
            return out;
        }

        bool fromBase64(std::string const& in, std::string& out) {
            out.clear();
            uint32_t acc = 0;
            int bits = 0;
            for (char c : in) {
                if (c == '=')
                    break;
                char const* p = std::strchr(kB64, c);
                if (!p || c == '\0')
                    return false;
                acc = acc << 6 | (uint32_t)(p - kB64);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out += (char)(acc >> bits & 0xFF);
                }
            }
            return true;
        }

        constexpr const char* kCodePrefix = "GBSEG1:";

        void loadMacroScoped(Kind k) {
            auto* mod = Mod::get();
            auto s = state(k);
            s.notes = mod->getSavedValue<std::string>(perMacroKey(k, "notes"), "");
            s.segmentsRaw = mod->getSavedValue<std::string>(perMacroKey(k, "segments"), "");
            s.loopEnabled = false;
            s.loopStartIdx = s.loopEndIdx = -1;
        }

    } // namespace

    State state(Kind k) {
        auto* gb = GucciEngine::get();
        if (k == Kind::Jupiter)
            return State{gb->jupiterMacro,
                         gb->jupiterClickBarEnabled,
                         gb->jupiterClickBarWindow,
                         gb->jupiterClickBarPaused,
                         gb->jupiterClickBarPosSec,
                         gb->jupiterClickBarLastRealTime,
                         gb->jupiterClickBarLoop,
                         gb->jupiterClickBarPageVisible,
                         gb->jupiterClickBarMyClicks,
                         gb->jupiterClickBarMyReleases,
                         gb->jupiterClickScore,
                         gb->jupiterMusicEnabled,
                         gb->jupiterMusicOffsetSec,
                         gb->jupiterAttemptCount,
                         gb->jupiterSessionBestPct,
                         gb->jupiterDeathPcts,
                         gb->jupiterLoopEnabled,
                         gb->jupiterLoopStartIdx,
                         gb->jupiterLoopEndIdx,
                         gb->jupiterGhostEnabled,
                         gb->jupiterBestGhostEnabled,
                         gb->jupiterScrubActive,
                         gb->jupiterScrubPercent,
                         gb->jupiterNotes,
                         gb->jupiterSegmentsRaw};
        return State{gb->trainerMacro,
                     gb->trainerClickBarEnabled,
                     gb->trainerClickBarWindow,
                     gb->trainerClickBarPaused,
                     gb->trainerClickBarPosSec,
                     gb->trainerClickBarLastRealTime,
                     gb->trainerClickBarLoop,
                     gb->trainerClickBarPageVisible,
                     gb->trainerClickBarMyClicks,
                     gb->trainerClickBarMyReleases,
                     gb->trainerClickScore,
                     gb->trainerMusicEnabled,
                     gb->trainerMusicOffsetSec,
                     gb->trainerAttemptCount,
                     gb->trainerSessionBestPct,
                     gb->trainerDeathPcts,
                     gb->trainerLoopEnabled,
                     gb->trainerLoopStartIdx,
                     gb->trainerLoopEndIdx,
                     gb->trainerGhostEnabled,
                     gb->trainerBestGhostEnabled,
                     gb->trainerScrubActive,
                     gb->trainerScrubPercent,
                     gb->trainerNotes,
                     gb->trainerSegmentsRaw};
    }

    // ------------------------------------------------------------ settings

    void ensureLoaded() {
        if (g_loaded)
            return;
        g_loaded = true;
        auto* gb = GucciEngine::get();
        auto* mod = Mod::get();
        for (Kind k : {Kind::Jupiter, Kind::Any}) {
            auto s = state(k);
            s.barEnabled = mod->getSavedValue<bool>(key(k, "bar_on"), s.barEnabled);
            s.barWindowSec = std::clamp((float)mod->getSavedValue<double>(key(k, "bar_window"), s.barWindowSec),
                                        0.5f, 10.f);
            s.barLoop = mod->getSavedValue<bool>(key(k, "bar_loop"), s.barLoop);
            s.musicEnabled = mod->getSavedValue<bool>(key(k, "music"), s.musicEnabled);
            s.musicOffsetSec =
                std::clamp((float)mod->getSavedValue<double>(key(k, "music_offset"), s.musicOffsetSec), -5.f, 5.f);
            s.ghost = mod->getSavedValue<bool>(key(k, "ghost"), s.ghost);
            s.bestGhost = mod->getSavedValue<bool>(key(k, "best_ghost"), s.bestGhost);
        }
        gb->clickIndicatorPerfectMs = std::clamp(
            (float)mod->getSavedValue<double>("trainer_click_perfect_ms", gb->clickIndicatorPerfectMs), 1.f, 200.f);
        gb->clickIndicatorOkMs = std::clamp(
            (float)mod->getSavedValue<double>("trainer_click_ok_ms", gb->clickIndicatorOkMs), 1.f, 400.f);

        loadMacroScoped(Kind::Jupiter);
        std::error_code ec;
        gb->trainerMusicImported = fs::exists(importedTrackPath(), ec);
    }

    // The Trainer comes back on the macro it was last pointed at, so its stats
    // and ghosts are live in that level without opening the page. Not on the
    // first frame: the macro may be saved under a custom theme's extension,
    // and custom themes load when the menu sets itself up. By the time a
    // level is open or the page is drawn, they have.
    static void restoreTrainerMacro() {
        if (g_macroRestored)
            return;
        g_macroRestored = true;
        auto* gb = GucciEngine::get();
        auto const saved = Mod::get()->getSavedValue<std::string>("trainer_macro_name", "");
        if (!saved.empty() && gb->trainerMacroName.empty() && gb->loadTrainerMacro(saved))
            loadMacroScoped(Kind::Any);
    }

    void saveSettings(Kind k) {
        auto* mod = Mod::get();
        auto s = state(k);
        mod->setSavedValue<bool>(key(k, "bar_on"), s.barEnabled);
        mod->setSavedValue<double>(key(k, "bar_window"), s.barWindowSec);
        mod->setSavedValue<bool>(key(k, "bar_loop"), s.barLoop);
        mod->setSavedValue<bool>(key(k, "music"), s.musicEnabled);
        mod->setSavedValue<double>(key(k, "music_offset"), s.musicOffsetSec);
        mod->setSavedValue<bool>(key(k, "ghost"), s.ghost);
        mod->setSavedValue<bool>(key(k, "best_ghost"), s.bestGhost);
    }

    void saveScoreWindows() {
        auto* gb = GucciEngine::get();
        Mod::get()->setSavedValue<double>("trainer_click_perfect_ms", gb->clickIndicatorPerfectMs);
        Mod::get()->setSavedValue<double>("trainer_click_ok_ms", gb->clickIndicatorOkMs);
    }

    // ------------------------------------------------------------ the clock

    bool followsLevel(Kind k) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return false;
        return k == Kind::Jupiter ? gbju::isJupiterLevel(pl) : gbtr::isTrainerLevel(pl);
    }

    double levelTimeSec() {
        return (double)(GucciEngine::get()->updater.getFrame() + 1) / gameTps();
    }

    double duration(Kind k) {
        double last = 0.0;
        for (auto const& iv : state(k).macro.clickIntervalsSec)
            last = std::max(last, iv.second);
        return last + 1.0;
    }

    void barBounds(Kind k, double& lo, double& hi) {
        lo = 0.0;
        hi = duration(k);
        double a = 0.0, b = 0.0;
        if (state(k).loopEnabled && loopRange(k, a, b)) {
            lo = a;
            hi = b;
        }
    }

    void markPageDrawn(Kind k) {
        restoreTrainerMacro();
        g_drawnAt[idx(k)] = g_tick;
    }

    void setPaused(Kind k, bool paused) {
        auto s = state(k);
        s.paused = paused;
        s.lastRealTime = steadySeconds();
    }

    void clearComparison(Kind k) {
        auto s = state(k);
        s.myPresses.clear();
        s.myReleases.clear();
        s.score.reset(s.macro.clickIntervalsSec.size());
    }

    void seek(Kind k, double sec) {
        if (followsLevel(k))
            return;  // the bar is the game's time while its level is open
        auto s = state(k);
        sec = std::clamp(sec, 0.0, duration(k));
        if (sec < s.posSec)
            rewindComparison(s, sec);
        s.posSec = sec;
    }

    void restart(Kind k) {
        auto s = state(k);
        double lo = 0.0, hi = 0.0;
        barBounds(k, lo, hi);
        s.posSec = s.barLoop ? lo : 0.0;
        setPaused(k, true);
        clearComparison(k);
    }

    void tick() {
        ensureLoaded();
        ++g_tick;
        double const now = steadySeconds();
        if (!PlayLayer::get())
            g_stepLevel = nullptr;
        else
            restoreTrainerMacro();

        for (Kind k : {Kind::Jupiter, Kind::Any}) {
            auto s = state(k);
            s.pageVisible = g_tick - g_drawnAt[idx(k)] <= 2;
            bool const following = followsLevel(k);

            if (following) {
                // The level is the clock. Pausing it here means leaving the
                // level leaves the bar where the run was, not running on.
                s.posSec = levelTimeSec();
                s.paused = true;
            } else if (!s.paused && s.barEnabled) {
                // Capped so a stall (a load, a dragged window) is not a jump.
                double const dt = std::clamp(now - s.lastRealTime, 0.0, 0.25);
                s.posSec += dt;
                double lo = 0.0, hi = 0.0;
                barBounds(k, lo, hi);
                if (s.posSec >= hi) {
                    if (s.barLoop) {
                        // A new lap is a fresh comparison.
                        s.posSec = lo;
                        clearComparison(k);
                    } else {
                        s.posSec = hi;
                        s.paused = true;
                    }
                }
            }
            s.lastRealTime = now;

            // Music for the bar away from the level; in the level the ghost
            // overlay keeps the track on the game's frame itself.
            bool const preview = s.pageVisible && s.barEnabled && !following && s.macro.loaded;
            if (k == Kind::Jupiter)
                gbju::syncClickBarMusic(preview, s.paused, s.posSec);
            else
                gbtr::syncTrainerClickBarMusic(preview, s.paused, s.posSec);
        }
    }

    // ------------------------------------------------------------ input

    void onLevelInput(bool down) {
        auto* gb = GucciEngine::get();
        if (gb->isPlaying() || levelBusy())
            return;
        double const t = levelTimeSec();
        for (Kind k : {Kind::Jupiter, Kind::Any})
            if (followsLevel(k))
                record(k, down, t, gameTps());
    }

    void onKeyInput(bool down) {
        for (Kind k : {Kind::Jupiter, Kind::Any}) {
            auto s = state(k);
            if (!s.pageVisible || !s.barEnabled || s.paused || followsLevel(k))
                continue;
            record(k, down, s.posSec, macroTps(s));
        }
    }

    // ------------------------------------------------------------ the level, per tick

    // Called after every PlayLayer::postUpdate. Notices the level going back
    // in time (a death, a practice respawn, a restart, a step back) and drops
    // what came after, and runs the segment loop.
    static void onLevelStep(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (pl != g_stepLevel) {
            g_stepLevel = pl;
            g_lastLevelSec.fill(-1.0);
            g_loopArmed.fill(false);
        }
        if (levelBusy())
            return;
        double const t = levelTimeSec();
        for (Kind k : {Kind::Jupiter, Kind::Any}) {
            int const i = idx(k);
            if (!followsLevel(k)) {
                g_lastLevelSec[i] = -1.0;
                continue;
            }
            auto s = state(k);
            double const last = g_lastLevelSec[i];
            if (last >= 0.0 && t < last - 1e-9)
                rewindComparison(s, t);

            double a = 0.0, b = 0.0;
            bool const looping = s.loopEnabled && loopRange(k, a, b) && !gb->isPlaying() && pl->m_isPracticeMode;
            if (looping && (a != g_armedStart[i] || b != g_armedEnd[i])) {
                g_loopArmed[i] = false;  // a different range needs its own checkpoint
                g_armedStart[i] = a;
                g_armedEnd[i] = b;
            }
            // Sent back to before the segment: the loop's checkpoint is gone
            // (removed, or a restart from the beginning), or the player would
            // have come back on it. The next pass lays a new one.
            if (last >= 0.0 && t < last - 1e-9 && t < a - 0.05)
                g_loopArmed[i] = false;
            bool const alive = pl->m_player1 && !pl->m_player1->m_isDead;
            if (looping && alive && last >= 0.0) {
                if (!g_loopArmed[i] && last < a && t >= a) {
                    // GD's own checkpoint path (the checkpoint key goes through
                    // it too), so GucciBot's practice fix captures it like any
                    // other and a respawn there is exact.
                    pl->queueCheckpoint();
                    g_loopArmed[i] = true;
                } else if (g_loopArmed[i] && last < b && t >= b && !g_resetQueued) {
                    // Not from inside the tick: the reset runs on the next frame,
                    // and only in the level that asked for it.
                    g_resetQueued = true;
                    Loader::get()->queueInMainThread([pl, i] {
                        g_resetQueued = false;
                        auto* now = PlayLayer::get();
                        if (now == pl && now->m_player1 && !now->m_player1->m_isDead) {
                            ++g_cleanLaps[i];
                            now->resetLevel();
                        }
                    });
                }
            }
            g_lastLevelSec[i] = t;
        }
    }

    // ------------------------------------------------------------ segments

    std::vector<Segment>& segments(Kind k) {
        auto& c = g_segments[idx(k)];
        auto const& raw = state(k).segmentsRaw;
        if (!c.parsed || c.parsedFrom != raw) {
            c.list = parse(raw);
            c.parsedFrom = raw;
            c.parsed = true;
        }
        return c.list;
    }

    void saveSegments(Kind k) {
        auto& c = g_segments[idx(k)];
        auto s = state(k);
        // Indices into a list that changed under them are checked again.
        int const n = (int)c.list.size();
        if (s.loopStartIdx >= n || s.loopEndIdx >= n || s.loopStartIdx > s.loopEndIdx) {
            s.loopStartIdx = s.loopEndIdx = -1;
            s.loopEnabled = false;
        }
        s.segmentsRaw = serialize(c.list);
        c.parsedFrom = s.segmentsRaw;
        if (k == Kind::Any && GucciEngine::get()->trainerMacroName.empty())
            return;
        Mod::get()->setSavedValue<std::string>(perMacroKey(k, "segments"), s.segmentsRaw);
    }

    void sortSegments(Kind k) {
        auto s = state(k);
        auto& list = segments(k);
        double a = 0.0, b = 0.0;
        bool const looped = loopRange(k, a, b);
        std::stable_sort(list.begin(), list.end(),
                         [](Segment const& x, Segment const& y) { return x.startSec < y.startSec; });
        if (looped) {
            int first = -1, last = -1;
            for (int i = 0; i < (int)list.size(); ++i) {
                if (first < 0 && list[(size_t)i].startSec == a)
                    first = i;
                if (first >= 0 && list[(size_t)i].endSec == b)
                    last = i;
            }
            s.loopStartIdx = first;
            s.loopEndIdx = last;
        }
        saveSegments(k);
    }

    void removeSegment(Kind k, int index) {
        auto s = state(k);
        auto& list = segments(k);
        if (index < 0 || index >= (int)list.size())
            return;
        if (s.loopStartIdx >= 0) {
            if (index < s.loopStartIdx) {
                --s.loopStartIdx;
                --s.loopEndIdx;
            } else if (index <= s.loopEndIdx) {
                if (s.loopStartIdx == s.loopEndIdx)
                    s.loopStartIdx = s.loopEndIdx = -1;  // its only segment went
                else
                    --s.loopEndIdx;
            }
        }
        list.erase(list.begin() + index);
        saveSegments(k);
    }

    void saveNotes(Kind k) {
        if (k == Kind::Any && GucciEngine::get()->trainerMacroName.empty())
            return;
        Mod::get()->setSavedValue<std::string>(perMacroKey(k, "notes"), state(k).notes);
    }

    std::vector<Segment> suggestSegments(Kind k) {
        auto const& iv = state(k).macro.clickIntervalsSec;
        std::vector<double> presses;
        presses.reserve(iv.size());
        for (auto const& p : iv)
            presses.push_back(p.first);
        std::sort(presses.begin(), presses.end());
        if (presses.size() < 8)
            return {};

        // Presses in the two seconds from each press. A stretch is dense when
        // that count is in the macro's top sixth and at least 3 a second, so a
        // calm macro suggests nothing rather than its least calm bits.
        constexpr double kWindow = 2.0;
        constexpr double kPad = 0.25;
        size_t const n = presses.size();
        std::vector<int> counts(n);
        for (size_t i = 0, j = 0; i < n; ++i) {
            j = std::max(j, i);
            while (j < n && presses[j] < presses[i] + kWindow)
                ++j;
            counts[i] = (int)(j - i);
        }
        std::vector<int> sorted = counts;
        std::sort(sorted.begin(), sorted.end());
        int const threshold = std::max(sorted[sorted.size() * 5 / 6], (int)(3 * kWindow));

        std::vector<Segment> spans;
        for (size_t i = 0, j = 0; i < n; ++i) {
            if (counts[i] < threshold)
                continue;
            j = i + (size_t)counts[i] - 1;  // the last press inside this window
            double const a = std::max(0.0, presses[i] - kPad);
            double const b = presses[j] + kPad;
            if (!spans.empty() && a <= spans.back().endSec)
                spans.back().endSec = std::max(spans.back().endSec, b);
            else
                spans.push_back({"", a, b, ""});
        }

        auto const& existing = segments(k);
        std::vector<std::pair<double, Segment>> ranked;
        for (auto& sp : spans) {
            double const len = sp.endSec - sp.startSec;
            bool covered = false;
            for (auto const& e : existing) {
                double const overlap = std::min(e.endSec, sp.endSec) - std::max(e.startSec, sp.startSec);
                if (overlap > len * 0.5) {
                    covered = true;
                    break;
                }
            }
            if (covered)
                continue;
            int clicks = 0;
            for (double p : presses)
                if (p >= sp.startSec && p <= sp.endSec)
                    ++clicks;
            double const rate = clicks / std::max(len, 0.01);
            sp.name = fmt::format("Dense part at {:.1f}s", sp.startSec);
            sp.note = fmt::format("{} presses in {:.1f}s ({:.1f} a second)", clicks, len, rate);
            ranked.push_back({rate, sp});
        }
        std::sort(ranked.begin(), ranked.end(), [](auto const& x, auto const& y) { return x.first > y.first; });
        if (ranked.size() > 6)
            ranked.resize(6);
        std::vector<Segment> out;
        for (auto& r : ranked)
            out.push_back(std::move(r.second));
        std::sort(out.begin(), out.end(), [](Segment const& x, Segment const& y) { return x.startSec < y.startSec; });
        return out;
    }

    std::string exportCode(Kind k) {
        return std::string(kCodePrefix) + toBase64(serialize(segments(k)));
    }

    bool importCode(Kind k, std::string const& code, std::string& error) {
        std::string body;
        for (char c : code)
            if (!std::isspace((unsigned char)c))
                body += c;
        std::string const prefix = kCodePrefix;
        if (body.rfind(prefix, 0) != 0) {
            error = "That isn't a GucciBot segment code (they start with GBSEG1:).";
            return false;
        }
        std::string text;
        if (!fromBase64(body.substr(prefix.size()), text)) {
            error = "The code is damaged: it has characters a code never contains.";
            return false;
        }
        int bad = 0;
        auto got = parse(text, &bad);
        if (got.empty()) {
            error = "The code has no segments in it.";
            return false;
        }
        auto& list = segments(k);
        for (auto& seg : got)
            list.push_back(std::move(seg));
        sortSegments(k);
        error = bad ? fmt::format("{} line(s) of it didn't read and were left out.", bad) : std::string();
        return true;
    }

    bool loopRange(Kind k, double& startSec, double& endSec) {
        auto s = state(k);
        auto const& list = segments(k);
        int const n = (int)list.size();
        if (s.loopStartIdx < 0 || s.loopEndIdx < s.loopStartIdx || s.loopEndIdx >= n)
            return false;
        startSec = list[(size_t)s.loopStartIdx].startSec;
        endSec = list[(size_t)s.loopEndIdx].endSec;
        return endSec > startSec;
    }

    bool loopArmed(Kind k) {
        return g_loopArmed[idx(k)] && followsLevel(k);
    }

    int cleanLaps(Kind k) {
        return g_cleanLaps[idx(k)];
    }

    // ------------------------------------------------------------ stats

    void resetStats(Kind k) {
        auto s = state(k);
        s.attempts = 0;
        s.sessionBestPct = 0.f;
        s.deathPcts.clear();
        g_cleanLaps[idx(k)] = 0;
    }

    // ------------------------------------------------------------ the Trainer's macro and track

    bool selectMacro(std::string const& stem) {
        auto* gb = GucciEngine::get();
        std::string const previous = gb->trainerMacroName;
        if (!gb->loadTrainerMacro(stem)) {
            // loadTrainerMacro has already emptied the Trainer; put the macro
            // that was there back rather than leave its name on nothing.
            if (!previous.empty())
                gb->loadTrainerMacro(previous);
            return false;
        }
        loadMacroScoped(Kind::Any);
        restart(Kind::Any);
        resetStats(Kind::Any);
        g_loopArmed[idx(Kind::Any)] = false;
        return true;
    }

    fs::path importedTrackPath() {
        // trainerghost.cpp's player reads this exact file.
        return Mod::get()->getSaveDir() / "trainer_music.mp3";
    }

    bool importTrack(fs::path const& from, std::string& error) {
        auto* gb = GucciEngine::get();
        // The player holds the old file open while it plays; let it go first.
        gbtr::stopTrainerClickBarMusic();
        std::error_code ec;
        fs::copy_file(from, importedTrackPath(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = fmt::format("Couldn't copy it: {}", ec.message());
            gb->trainerMusicImported = fs::exists(importedTrackPath(), ec);
            return false;
        }
        gb->trainerMusicImported = true;
        if (!gb->trainerMusicEnabled) {
            gb->trainerMusicEnabled = true;
            saveSettings(Kind::Any);
        }
        return true;
    }

    void removeTrack() {
        gbtr::stopTrainerClickBarMusic();
        std::error_code ec;
        fs::remove(importedTrackPath(), ec);
        GucciEngine::get()->trainerMusicImported = fs::exists(importedTrackPath(), ec);
    }

    // ------------------------------------------------------------ the JMF macro's ghost path

    void useLoadedPathForJupiter() {
        auto* gb = GucciEngine::get();
        gb->setJupiterMacroPath(gb->replay.m_pathSamples);
    }

    void clearJupiterPath() {
        GucciEngine::get()->setJupiterMacroPath({});
    }

} // namespace gucci::trainers

// The clock runs once a frame whether or not the menu is open. drawScene, not
// the scheduler: Calculate and the pathfinders call CCScheduler::update many
// times a frame.
class $modify(GBTrainerClock, cocos2d::CCDirector) {
    void drawScene() {
        // Stays hooked while GucciBot is switched off where imgui-cocos draws
        // from drawScene (core/bot_switch.hpp); the clock stops with the rest.
        if (gucci::botswitch::takenOut())
            return CCDirector::drawScene();
        gucci::trainers::tick();
        CCDirector::drawScene();
    }
};

class $modify(GBTrainerPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        gucci::trainers::onLevelStep(this);
    }
};
