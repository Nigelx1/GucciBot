#include "ai/record.hpp"

#include "ai/observe.hpp"
#include "ai/sim.hpp"
#include "ai/start.hpp"
#include "ai/tape.hpp"
#include "core/GucciBot.hpp"
#include "core/platform.hpp"
#include "hacks/safemode.hpp"
#include "tools/macro_convert.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace gucci::ai {

    fs::path aiRoot() {
        return Mod::get()->getSaveDir() / "ai";
    }

    namespace {

        using Clock = job::Clock;

        std::string utf8(fs::path const& p) {
            auto const u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        fs::path u8path(std::string const& s) {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        // ------------------------------------------------------------ the macro

        struct Event {
            int64_t frame = 0;  // GucciBot numbering, before the offset
            bool down = false;
            bool p2 = false;  // player 2's own button (a two-player level)
        };

        struct MacroIn {
            std::string path;
            std::string fileName;
            std::string stem;
            std::string format;
            bool silicate = false;
            convert::Macro macro;
            std::vector<Event> events;
            int p2Merged = 0;
            int p2Dropped = 0;
            int p2Own = 0;  // player 2's inputs on its own button (a two-player level)
            int otherButtons = 0;
        };

        bool readMacro(std::string const& path, bool twoPlayer, MacroIn& out, std::string& why) {
            fs::path const p = u8path(path);
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                why = "can't open " + path;
                return false;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            out.path = path;
            out.fileName = utf8(p.filename());
            out.stem = utf8(p.stem());
            if (convert::isGucciBot(bytes)) {
                std::string err;
                if (!convert::fromGucciBot(bytes, out.macro, err)) {
                    why = "the GucciBot macro could not be read: " + err;
                    return false;
                }
                out.format = "gbr6";
            } else {
                // Untrusted files (a download) are only ever read through the converter.
                auto read = convert::read(bytes, out.fileName, 0);
                if (!read.ok()) {
                    why = "the macro could not be read: " +
                          (read.error.empty() ? std::string("unknown format") : read.error);
                    return false;
                }
                out.format = std::string(read.format->id);
                out.macro = std::move(read.macro);
            }
            out.silicate = out.format.rfind("slc", 0) == 0;
            // In a one-player level the game hands player 2's inputs to player
            // 1 too: one button stream. A two-player level's player 2 has a
            // button of its own, which drives the second copy in the dual part
            // (sim::step2; act stays player 1's).
            for (auto const& in : out.macro.inputs) {
                if (in.button != 1) {
                    out.otherButtons++;
                    continue;
                }
                if (in.player2) {
                    if (twoPlayer) out.p2Own++;
                    else out.p2Merged++;
                }
                out.events.push_back(Event{in.frame, in.down, in.player2 && twoPlayer});
            }
            std::stable_sort(out.events.begin(), out.events.end(),
                             [](Event const& a, Event const& b) { return a.frame < b.frame; });
            return true;
        }

        std::string sanitize(std::string const& s) {
            std::string out;
            for (unsigned char c : s) {
                if (std::isalnum(c) || c == '-' || c == '.') out += (char)c;
                else out += '_';
            }
            if (out.empty()) out = "macro";
            return out.substr(0, 80);
        }

        // ------------------------------------------------------------ opening the level

        class LevelOpener {
        public:
            int64_t id = 0;
            ToolCall tools;
            enum Result { Waiting, Ready, Failed };

            Result service(std::string& why) {
                auto const now = Clock::now();
                if (m_t0 == Clock::time_point{}) m_t0 = now;
                auto* pl = PlayLayer::get();
                bool const here = pl && pl->m_level && (int64_t)pl->m_level->m_levelID.value() == id &&
                                  pl->m_player1;
                if (here) {
                    if (m_here == Clock::time_point{}) m_here = now;
                    // Let the scene's fade finish before the level is restarted.
                    if (!m_opened || now - m_here >= std::chrono::seconds(3)) return Ready;
                    return Waiting;
                }
                m_here = {};
                if (now - m_t0 > std::chrono::seconds(150)) {
                    why = fmt::format("level {} did not open within 150 s", id);
                    return Failed;
                }
                if (!tools) {
                    why = fmt::format("open level {} first", id);
                    return Failed;
                }
                if (pl) {
                    if (!m_quit) {
                        tools("gucci_quit_level", matjson::Value::object());
                        m_quit = true;
                    }
                    return Waiting;
                }
                if (now - m_lastOpen < std::chrono::seconds(2)) return Waiting;
                m_lastOpen = now;
                auto args = matjson::Value::object();
                if (id >= 1 && id <= 22) args["main"] = id;
                else args["id"] = id;
                try {
                    auto r = tools("gucci_open_level", args);
                    m_opened = true;
                    (void)r;
                } catch (std::exception const& e) {
                    // "leave the level first" while the old one tears down, or
                    // a download still going: try again shortly.
                    m_lastError = e.what();
                }
                return Waiting;
            }
            std::string lastError() const { return m_lastError; }

        private:
            Clock::time_point m_t0{}, m_here{}, m_lastOpen{};
            bool m_quit = false;
            bool m_opened = false;
            std::string m_lastError;
        };

        // ------------------------------------------------------------ the recorder

        struct Pass {
            int offset = 0;
            uint32_t ticks = 0;
            bool completed = false;
            bool died = false;
            int64_t diedTick = -1;
            float diedX = 0.0f;
            float finalX = 0.0f;
            int killerId = 0;
            bool killerP2 = false;
        };

        class Record : public job::Job {
        public:
            char const* kind() const override { return "record"; }
            bool holdsGame() const override { return m_start.requested(); }

            MacroIn in;
            int64_t levelId = 0;
            std::vector<int> offsets;
            double tps = 0.0;
            LevelOpener opener;

            bool slice(Clock::time_point until) override {
                switch (m_phase) {
                case Phase::Open: {
                    std::string why;
                    auto const r = opener.service(why);
                    if (r == LevelOpener::Failed) {
                        error = why;
                        return true;
                    }
                    if (r == LevelOpener::Waiting) {
                        progress = "opening the level";
                        return false;
                    }
                    if (!prepare()) return true;
                    m_phase = Phase::Probe;
                    return false;
                }
                case Phase::Probe:
                case Phase::Record: return pass(until);
                }
                return true;
            }

            void cancel() override {
                if (sim::open()) sim::end();
                m_start.release();
            }

        private:
            enum class Phase { Open, Probe, Record };
            Phase m_phase = Phase::Open;
            size_t m_offsetIndex = 0;
            std::vector<Pass> m_passes;
            int m_chosen = 0;
            LevelStart m_start;
            bool m_requested = false;
            bool m_running = false;
            int m_startFrame = 0;
            size_t m_next = 0;
            bool m_down = false;
            bool m_down2 = false;  // player 2's own button (a two-player level)
            uint32_t m_ticks = 0;
            uint32_t m_limit = 0;
            int m_early = 0;
            int m_taps = 0;
            Pass m_pass;
            sim::State m_state;
            obs::Observer m_obs;
            bool m_twoPlayer = false;
            std::string m_levelName;
            double m_simMs = 0.0;
            // the record
            std::vector<float> m_stateRows, m_metaRows;
            std::vector<uint8_t> m_scanRows, m_tokRows, m_actRows;

            bool prepare() {
                auto* pl = PlayLayer::get();
                if (!pl || !pl->m_level) {
                    error = "the level was left";
                    return false;
                }
                m_twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
                m_levelName = std::string(pl->m_level->m_levelName);
                std::string why;
                if (!readMacro(in.path, m_twoPlayer, in, why)) {
                    error = why;
                    return false;
                }
                if (tps <= 0.0) tps = in.macro.tps > 0.0 ? in.macro.tps : 240.0;
                if (offsets.empty()) offsets = in.silicate ? std::vector<int>{0} : std::vector<int>{0, 1, -1};
                return true;
            }

            // One pass of the macro at an offset: a fresh start, then the
            // inputs tick by tick in slices. The record phase also observes.
            bool pass(Clock::time_point until) {
                bool const recording = m_phase == Phase::Record;
                if (!m_running) {
                    if (!m_requested) {
                        m_start.request(tps);
                        m_requested = true;
                        progress = recording ? "recording: holding the level at its start"
                                             : fmt::format("offset {}: holding the level at its start",
                                                           offsets[m_offsetIndex]);
                    }
                    std::string why;
                    auto const step = m_start.service(why);
                    if (step == LevelStart::Step::Waiting) return false;
                    if (step == LevelStart::Step::Failed) {
                        error = why;
                        m_start.release();
                        return true;
                    }
                    return begin(recording);
                }
                if (!sim::open()) {
                    error = "the AI session was closed (the level was left?)";
                    m_start.release();
                    return true;
                }
                auto const t0 = Clock::now();
                sim::resume();
                bool const over = advance(until, recording);
                m_simMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                if (!over) {
                    sim::suspend();
                    progress = fmt::format("{} offset {}: tick {}, x {:.0f}", recording ? "recording" : "trying",
                                           m_pass.offset, m_ticks, m_pass.finalX);
                    return false;
                }
                sim::end();
                m_start.release();
                m_running = false;
                m_requested = false;
                if (!recording) {
                    m_passes.push_back(m_pass);
                    m_offsetIndex++;
                    if (!m_pass.completed && m_offsetIndex < offsets.size()) return false;  // the next offset
                    // The one that completed, else the one that got furthest.
                    size_t best = 0;
                    for (size_t i = 0; i < m_passes.size(); i++) {
                        auto const& p = m_passes[i];
                        auto const& b = m_passes[best];
                        if (p.completed && !b.completed) best = i;
                        else if (p.completed == b.completed && !p.completed && p.finalX > b.finalX) best = i;
                    }
                    m_chosen = m_passes[best].offset;
                    m_phase = Phase::Record;
                    return false;
                }
                return write();
            }

            bool begin(bool recording) {
                auto* pl = PlayLayer::get();
                auto start = m_start.start();
                std::string why;
                int const offset = recording ? m_chosen : offsets[m_offsetIndex];
                // The level read while the game stands at its start, before
                // the session writes anything into it.
                if (recording && !m_obs.begin(pl, why)) {
                    error = "the level could not be read: " + why;
                    m_start.release();
                    return true;
                }
                if (!sim::begin(pl, start, why)) {
                    error = "the AI session could not begin: " + why;
                    m_start.release();
                    return true;
                }
                m_obs.setTps(GucciEngine::get()->updater.getTps());
                m_startFrame = sim::startFrame(*start);
                sim::read(m_state);
                m_down = false;
                m_down2 = false;
                m_next = 0;
                m_ticks = 0;
                m_taps = 0;
                m_early = 0;
                m_pass = Pass{};
                m_pass.offset = offset;
                m_pass.finalX = m_state.p[0].x;
                int64_t const last = in.events.empty() ? 0 : in.events.back().frame + offset;
                int64_t const rate = (int64_t)std::llround(GucciEngine::get()->updater.getTps());
                int64_t const limit = std::max<int64_t>(0, last - m_startFrame) + 60 * rate;
                m_limit = (uint32_t)std::clamp<int64_t>(limit, 1, 20'000'000);
                for (Event const& e : in.events)
                    if (e.frame + offset - m_startFrame - 1 < 0) m_early++;
                if (recording) {
                    size_t const guess = std::min<size_t>(m_limit, 400'000);
                    m_stateRows.clear(), m_stateRows.reserve(guess * obs::kStateDim);
                    m_metaRows.clear(), m_metaRows.reserve(guess * obs::kMetaDim);
                    m_scanRows.clear(), m_scanRows.reserve(guess * obs::kScanDim);
                    m_tokRows.clear(), m_tokRows.reserve(guess * obs::kTokDim);
                    m_actRows.clear(), m_actRows.reserve(guess);
                }
                m_running = true;
                sim::suspend();  // the first slice starts on the next frame
                return false;
            }

            bool advance(Clock::time_point until, bool recording) {
                int const offset = m_pass.offset;
                obs::Record rec;
                for (;;) {
                    if (m_ticks >= m_limit) return true;
                    if (recording) {
                        m_obs.observe(rec);
                        m_stateRows.insert(m_stateRows.end(), rec.state, rec.state + obs::kStateDim);
                        m_scanRows.insert(m_scanRows.end(), rec.scan, rec.scan + obs::kScanDim);
                        m_tokRows.insert(m_tokRows.end(), rec.tok, rec.tok + obs::kTokDim);
                        m_metaRows.insert(m_metaRows.end(), rec.meta, rec.meta + obs::kMetaDim);
                    }
                    uint8_t presses = 0, presses2 = 0;
                    bool held = m_down, held2 = m_down2;
                    int64_t const frame = (int64_t)m_startFrame + 1 + (int64_t)m_ticks;
                    while (m_next < in.events.size() && in.events[m_next].frame + offset <= frame) {
                        Event const& e = in.events[m_next];
                        uint8_t& n = e.p2 ? presses2 : presses;
                        bool& h = e.p2 ? held2 : held;
                        if (e.down) {
                            if (n < 255) n++;
                            h = true;
                        } else {
                            h = false;
                        }
                        m_next++;
                    }
                    m_down = held;
                    m_down2 = held2;
                    bool const dead = sim::step2(presses, held, presses2, held2);
                    sim::read(m_state);
                    bool const act = m_state.p[0].button;
                    if (presses > 0 && !act) m_taps++;
                    if (recording) {
                        m_obs.after(act);
                        m_actRows.push_back(act ? 1 : 0);
                    }
                    m_ticks++;
                    m_pass.ticks = m_ticks;
                    m_pass.finalX = m_state.p[0].x;
                    if (dead) {
                        m_pass.died = true;
                        m_pass.diedTick = (int64_t)m_ticks - 1;
                        m_pass.diedX = m_state.p[0].x;
                        m_pass.killerId = m_state.killerId;
                        m_pass.killerP2 = m_state.deadP2;
                        if (recording) m_metaRows[m_metaRows.size() - 1] = 1.0f;  // the step that died
                        return true;
                    }
                    if (m_state.complete) {
                        m_pass.completed = true;
                        return true;
                    }
                    if ((m_ticks & 63) == 0 && Clock::now() >= until) return false;
                }
            }

            template <class T>
            static bool writeArray(fs::path const& p, std::vector<T> const& v) {
                std::ofstream f(p, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<char const*>(v.data()), (std::streamsize)(v.size() * sizeof(T)));
                return (bool)f;
            }

            bool write() {
                auto const root = aiRoot();
                auto const data = root / "data";
                std::error_code ec;
                fs::create_directories(data, ec);
                std::string const runId = fmt::format("{}__{}__{}", levelId, sanitize(in.stem), sanitize(in.format));
                auto const base = utf8(data / runId);
                auto pathOf = [&](char const* ext) { return u8path(base + ext); };
                uint32_t const T = m_ticks;
                bool ok = writeArray(pathOf(".state.f32"), m_stateRows) && writeArray(pathOf(".scan.u8"), m_scanRows) &&
                          writeArray(pathOf(".tok.u8"), m_tokRows) && writeArray(pathOf(".act.u8"), m_actRows) &&
                          writeArray(pathOf(".meta.f32"), m_metaRows);
                // Golden rows: 32 ticks spread over the run, expanded here.
                std::vector<int32_t> gidx;
                if (T > 0) {
                    int const G = (int)std::min<uint32_t>(32, T);
                    for (int g = 0; g < G; g++) {
                        int32_t const t = G == 1 ? 0 : (int32_t)std::llround((double)g * (double)(T - 1) / (G - 1));
                        if (gidx.empty() || gidx.back() != t) gidx.push_back(t);
                    }
                }
                std::vector<float> golden(gidx.size() * obs::kObsDim);
                for (size_t g = 0; g < gidx.size(); g++) {
                    obs::Record r;
                    size_t const t = (size_t)gidx[g];
                    std::copy_n(&m_stateRows[t * obs::kStateDim], obs::kStateDim, r.state);
                    std::copy_n(&m_scanRows[t * obs::kScanDim], obs::kScanDim, r.scan);
                    std::copy_n(&m_tokRows[t * obs::kTokDim], obs::kTokDim, r.tok);
                    obs::expand(r, &golden[g * obs::kObsDim]);
                }
                ok = ok && writeArray(pathOf(".golden.idx.i32"), gidx) && writeArray(pathOf(".golden.f32"), golden);
                if (!ok) {
                    error = "could not write the record's files in " + utf8(data);
                    return true;
                }

                Pass const& p = m_pass;
                auto h = matjson::Value::object();
                h["format"] = "gbai-run";
                h["version"] = 2;
                h["source"] = "human";
                h["level_id"] = levelId;
                h["level_name"] = m_levelName;
                h["macro_file"] = in.fileName;
                h["macro_path"] = in.path;
                h["macro_format"] = in.format;
                h["tps"] = GucciEngine::get()->updater.getTps();
                h["ticks"] = (int64_t)T;
                h["layout_hash"] = obs::layoutHash();
                h["frame_offset"] = p.offset;
                auto tried = matjson::Value::object();
                for (auto const& q : m_passes) {
                    auto o = matjson::Value::object();
                    o["completed"] = q.completed;
                    o["died_x"] = q.died ? matjson::Value((double)q.diedX) : matjson::Value(nullptr);
                    o["died_tick"] = q.died ? matjson::Value(q.diedTick) : matjson::Value(nullptr);
                    o["final_x"] = (double)q.finalX;
                    o["ticks"] = (int64_t)q.ticks;
                    if (q.died) o["killer_id"] = q.killerId;
                    if (q.died && q.killerP2) o["player2_died"] = true;
                    tried[std::to_string(q.offset)] = o;
                }
                h["offsets_tried"] = tried;
                h["completed"] = p.completed;
                h["died_tick"] = p.died ? matjson::Value(p.diedTick) : matjson::Value(nullptr);
                h["died_x"] = p.died ? matjson::Value((double)p.diedX) : matjson::Value(nullptr);
                h["level_length_x"] = (double)m_state.levelLength;
                h["validated"] = p.completed ? "sim" : "failed";
                h["first_tick"] = 0;
                h["start_frame"] = m_startFrame;
                std::string notes;
                if (!p.completed)
                    notes += fmt::format("Dies in the AI's simulation at x {:.0f} ({:.0f}%) with every offset tried; "
                                         "validate it in the real game before training on it. ",
                                         p.diedX, m_state.levelLength > 0 ? 100.0 * p.diedX / m_state.levelLength : 0.0);
                if (p.died && p.killerP2) notes += "Player 2 died. ";
                if (m_taps) notes += fmt::format("{} zero-length taps (act is 0 on their ticks). ", m_taps);
                if (in.p2Merged) notes += fmt::format("{} player-2 inputs merged into player 1. ", in.p2Merged);
                if (in.p2Dropped) notes += fmt::format("{} player-2 inputs dropped (two-player level). ", in.p2Dropped);
                if (in.p2Own)
                    notes += fmt::format("{} player-2 inputs drove player 2 (two-player level; act is player 1's). ",
                                         in.p2Own);
                if (!in.macro.tpsChanges.empty())
                    notes += fmt::format("{} tick-rate changes in the macro were ignored. ", in.macro.tpsChanges.size());
                if (m_early) notes += fmt::format("{} inputs at or before the start frame went in on tick 0. ", m_early);
                h["notes"] = notes;
                h["zero_length_taps"] = m_taps;
                h["early_inputs"] = m_early;
                h["p2_merged"] = in.p2Merged;
                h["p2_dropped"] = in.p2Dropped;
                h["build"] = GB_BUILD_LABEL;
                {
                    auto const t = std::time(nullptr);
                    char buf[32] = {};
                    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
                    h["recorded"] = std::string(buf);
                }
                auto const& st = m_obs.stats();
                auto o = matjson::Value::object();
                o["items"] = st.items;
                o["movers"] = st.movers;
                o["tokens"] = st.tokens;
                o["configs"] = st.configs;
                o["index_ms"] = st.buildMs;
                o["observe_us_avg"] = st.observed ? st.observeUs / (double)st.observed : 0.0;
                o["observe_us_max"] = st.maxUs;
                o["mover_poses_per_tick"] = st.observed ? (double)st.moverPoses / (double)st.observed : 0.0;
                h["observer"] = o;

                auto const tmp = pathOf(".json.tmp");
                {
                    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                    f << h.dump(2);
                    if (!f) {
                        error = "could not write the record's header";
                        return true;
                    }
                }
                fs::rename(tmp, pathOf(".json"), ec);
                if (ec) {
                    error = "could not write the record's header: " + ec.message();
                    return true;
                }

                // The level's tape, once.
                bool tapeWritten = false;
                std::string tapeWhy;
                if (auto const* index = m_obs.index()) {
                    auto const tapes = root / "tapes";
                    if (!fs::exists(tapes / (std::to_string(levelId) + ".json"), ec)) {
                        auto const tp = tape::build(*index);
                        tapeWritten = tape::write(tapes, *index, m_levelName, tp, tapeWhy);
                    }
                }

                auto out = h;
                out["run_id"] = runId;
                out["file"] = base + ".json";
                out["tape_written"] = tapeWritten;
                if (!tapeWhy.empty()) out["tape_error"] = tapeWhy;
                out["sim_ms"] = m_simMs;
                result = out;
                progress = "done";
                log::info("[AI] recorded {} at offset {}: {} ticks, {}, observe {:.1f} us avg {:.1f} max", runId,
                          p.offset, T, p.completed ? "completed" : "died",
                          st.observed ? st.observeUs / (double)st.observed : 0.0, st.maxUs);
                return true;
            }
        };

        // ------------------------------------------------------------ validate in the real game

        class ValidateReal : public job::Job {
        public:
            char const* kind() const override { return "validate_real"; }

            MacroIn in;
            int64_t levelId = 0;
            int offset = 0;
            LevelOpener opener;

            bool slice(Clock::time_point) override {
                auto* gb = GucciEngine::get();
                switch (m_phase) {
                case 0: {
                    std::string why;
                    auto const r = opener.service(why);
                    if (r == LevelOpener::Failed) {
                        error = why;
                        return true;
                    }
                    if (r == LevelOpener::Waiting) {
                        progress = "opening the level";
                        return false;
                    }
                    return startPlayback();
                }
                case 1: {
                    auto* pl = PlayLayer::get();
                    if (!pl || !pl->m_player1) {
                        finishWith(false, "the level was left");
                        return true;
                    }
                    int64_t const frame = (int64_t)gb->updater.getFrame();
                    // The attempt has begun once the frame moved on from the restart.
                    if (!m_begun) {
                        if (frame > 2 && !pl->m_player1->m_isDead) m_begun = true;
                        if (Clock::now() - m_started > std::chrono::seconds(20)) {
                            finishWith(false, "the playback did not start");
                            return true;
                        }
                        return false;
                    }
                    float const x = pl->m_player1->getPositionX();
                    m_maxX = std::max(m_maxX, x);
                    if (pl->m_hasCompletedLevel || pl->m_levelEndAnimationStarted) {
                        m_completed = true;
                        finishWith(true, "");
                        return true;
                    }
                    if (pl->m_player1->m_isDead || (pl->m_player2 && pl->m_gameState.m_isDualMode && pl->m_player2->m_isDead)) {
                        m_died = true;
                        m_diedX = x;
                        m_diedFrame = frame;
                        finishWith(true, "");
                        return true;
                    }
                    if (frame < m_lastFrame - 5) {
                        // Restarted without us seeing the death (a frame skipped it).
                        m_died = true;
                        m_diedX = m_maxX;
                        finishWith(true, "");
                        return true;
                    }
                    m_lastFrame = frame;
                    if (Clock::now() > m_deadline) {
                        finishWith(false, "timed out");
                        return true;
                    }
                    progress = fmt::format("playing in the real game: frame {}, x {:.0f}", frame, x);
                    return false;
                }
                }
                return true;
            }

            void cancel() override { cleanup(); }

        private:
            int m_phase = 0;
            bool m_begun = false;
            bool m_completed = false;
            bool m_died = false;
            float m_diedX = 0.0f;
            float m_maxX = 0.0f;
            int64_t m_diedFrame = -1;
            int64_t m_lastFrame = 0;
            Clock::time_point m_started{}, m_deadline{};
            fs::path m_copy;
            double m_oldTps = 0.0;
            std::string m_oldName;
            bool m_cleaned = false;
            float m_levelLength = 0.0f;

            bool startPlayback() {
                auto* gb = GucciEngine::get();
                auto* pl = PlayLayer::get();
                std::string why;
                bool const twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
                if (!readMacro(in.path, twoPlayer, in, why)) {
                    error = why;
                    return true;
                }
                convert::Macro m = in.macro;
                for (auto& i : m.inputs) i.frame += offset;
                for (auto& c : m.tpsChanges) c.frame += offset;
                m.inputs.erase(std::remove_if(m.inputs.begin(), m.inputs.end(),
                                              [](convert::Input const& i) { return i.frame < 0; }),
                               m.inputs.end());
                auto const bytes = convert::toGucciBot(m);
                m_copy = gb->getReplayDir() / u8path("zz_gbtest_" + sanitize(in.stem) + ".gbr6");
                {
                    std::error_code ec;
                    fs::create_directories(m_copy.parent_path(), ec);
                    std::ofstream f(m_copy, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size());
                    if (!f) {
                        error = "could not write " + utf8(m_copy);
                        return true;
                    }
                }
                m_oldTps = gb->updater.m_tps;
                m_oldName = gb->replayName;
                gb->replay.load(m_copy);
                gb->replayName = utf8(m_copy.stem());
                gb->setMode(GucciEngine::Mode::Playing);
                gb->updater.setPaused(false);
                pl->resetLevel();
                m_levelLength = pl->m_levelLength;
                m_started = Clock::now();
                int64_t const last = in.events.empty() ? 0 : in.events.back().frame + offset;
                double const rate = std::max(1.0, in.macro.tps);
                m_deadline = m_started + std::chrono::seconds((int64_t)(last / rate) + 60);
                m_phase = 1;
                progress = "playing in the real game";
                return false;
            }

            void cleanup() {
                if (m_cleaned) return;
                m_cleaned = true;
                auto* gb = GucciEngine::get();
                if (m_phase >= 1) {
                    gb->setMode(GucciEngine::Mode::Idle);
                    if (m_oldTps > 0.0) gb->updater.setTps(m_oldTps);
                    gb->replayName = m_oldName;
                }
                if (!m_copy.empty()) {
                    std::error_code ec;
                    fs::remove(m_copy, ec);
                }
            }

            void finishWith(bool ok, std::string const& why) {
                cleanup();
                if (!ok) {
                    error = why;
                    return;
                }
                auto out = matjson::Value::object();
                out["completed"] = m_completed;
                out["died_x"] = m_died ? matjson::Value((double)m_diedX) : matjson::Value(nullptr);
                out["died_frame"] = m_died && m_diedFrame >= 0 ? matjson::Value(m_diedFrame) : matjson::Value(nullptr);
                double const pct = m_completed ? 100.0 : (m_levelLength > 0 ? 100.0 * m_diedX / m_levelLength : 0.0);
                out["percent"] = pct;
                out["level_length_x"] = (double)m_levelLength;
                out["frame_offset"] = offset;
                out["macro_file"] = in.fileName;
                out["macro_format"] = in.format;
                out["tps"] = in.macro.tps;
                out["copy_deleted"] = true;
                result = out;
                progress = "done";
                log::info("[AI] validate_real {} offset {}: {}", in.fileName, offset,
                          m_completed ? std::string("completed") : fmt::format("died at x {:.0f}", m_diedX));
            }
        };

        // ------------------------------------------------------------ find a level online

        class FindLevel : public job::Job {
        public:
            char const* kind() const override { return "find_level"; }
            std::string name;

            bool slice(Clock::time_point) override {
                auto* glm = GameLevelManager::sharedState();
                if (!m_search) {
                    m_search = GJSearchObject::create(SearchType::Search, name);
                    m_search->retain();
                    m_key = m_search->getKey();
                    m_t0 = Clock::now();
                    if (auto* stored = glm->getStoredOnlineLevels(m_key.c_str())) return answer(stored);
                    glm->getOnlineLevels(m_search);
                    progress = "searching GD's servers";
                    return false;
                }
                if (auto* stored = glm->getStoredOnlineLevels(m_key.c_str())) return answer(stored);
                if (Clock::now() - m_t0 > std::chrono::seconds(20)) {
                    release();
                    error = "GD's servers did not answer within 20 s";
                    return true;
                }
                return false;
            }
            void cancel() override { release(); }

        private:
            GJSearchObject* m_search = nullptr;
            std::string m_key;
            Clock::time_point m_t0{};

            void release() {
                if (m_search) m_search->release();
                m_search = nullptr;
            }

            bool answer(CCArray* levels) {
                auto list = matjson::Value::array();
                for (auto* lvl : CCArrayExt<GJGameLevel*>(levels)) {
                    if (!lvl) continue;
                    auto o = matjson::Value::object();
                    o["id"] = (int64_t)lvl->m_levelID.value();
                    o["name"] = std::string(lvl->m_levelName);
                    o["author"] = std::string(lvl->m_creatorName);
                    o["stars"] = (int)lvl->m_stars.value();
                    o["demon"] = (bool)lvl->m_demon.value();
                    o["length"] = (int)lvl->m_levelLength;
                    o["exact_name"] = std::string(lvl->m_levelName) == name;
                    list.push(o);
                }
                release();
                auto out = matjson::Value::object();
                out["query"] = name;
                out["candidates"] = list;
                result = out;
                progress = "done";
                return true;
            }
        };

        bool checkPlatform(std::string& why) {
#if !GB_NATIVE_ENGINE
            why = "The AI's simulation is Windows-only for now: its model of the level reads GD's Windows memory "
                  "layout.";
            return false;
#else
            (void)why;
            return true;
#endif
        }

    }  // namespace

    std::unique_ptr<job::Job> makeRecord(std::string const& macroPath, int64_t levelId, std::vector<int> offsets,
                                         bool offsetsGiven, double tps, ToolCall tools, std::string& why) {
        if (!checkPlatform(why)) return nullptr;
        if (levelId <= 0) {
            why = "level_id is required (a local level has no id that stays the same; it is refused)";
            return nullptr;
        }
        auto* gb = GucciEngine::get();
        if (gb->isRecording() || gb->isPlaying()) {
            why = "GucciBot is recording or playing a macro: set the mode to idle first";
            return nullptr;
        }
        // Fail early on a file that can't be read (read again once the level is open).
        MacroIn probe;
        if (!readMacro(macroPath, false, probe, why)) return nullptr;
        auto job = std::make_unique<Record>();
        job->in.path = macroPath;
        job->levelId = levelId;
        if (offsetsGiven) job->offsets = std::move(offsets);
        job->tps = tps;
        job->opener.id = levelId;
        job->opener.tools = std::move(tools);
        return job;
    }

    std::unique_ptr<job::Job> makeValidateReal(std::string const& macroPath, int64_t levelId, int frameOffset,
                                               ToolCall tools, std::string& why) {
        if (levelId <= 0) {
            why = "level_id is required";
            return nullptr;
        }
        auto* gb = GucciEngine::get();
        if (!gb->protectedMode && !safemode::forced() && !safemode::autoSafeMode()) {
            why = "safe mode is off: a run in the real game needs it on (Hacks > Safe mode, or forced for a test "
                  "launch)";
            return nullptr;
        }
        if (gb->isRecording()) {
            why = "GucciBot is recording: stop first";
            return nullptr;
        }
        MacroIn probe;
        if (!readMacro(macroPath, false, probe, why)) return nullptr;
        auto job = std::make_unique<ValidateReal>();
        job->in.path = macroPath;
        job->levelId = levelId;
        job->offset = frameOffset;
        job->opener.id = levelId;
        job->opener.tools = std::move(tools);
        return job;
    }

    std::unique_ptr<job::Job> makeFindLevel(std::string const& name, std::string& why) {
        if (name.empty()) {
            why = "name is required";
            return nullptr;
        }
        auto job = std::make_unique<FindLevel>();
        job->name = name;
        return job;
    }

} // namespace gucci::ai
