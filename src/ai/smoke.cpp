#include "ai/smoke.hpp"

#include "ai/sim.hpp"
#include "core/platform.hpp"
#include "ai/start.hpp"
#include "core/GucciBot.hpp"
#include "tools/macro_convert.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace geode::prelude;

namespace gucci::ai {

    namespace {

        struct Event {
            int64_t frame = 0;  // GucciBot numbering, offset included
            bool down = false;
        };

        class SimSmoke : public job::Job {
        public:
            char const* kind() const override { return "sim_smoke"; }
            bool holdsGame() const override { return m_start.requested(); }

            std::string path;
            std::string fileName;
            int64_t levelId = 0;
            int offset = 0;
            int64_t maxTicks = 0;
            convert::Macro macro;
            std::string format;
            std::vector<Event> events;
            int p2Merged = 0;
            int p2Dropped = 0;
            int otherButtons = 0;

            bool slice(job::Clock::time_point until) override {
                if (!m_running) {
                    if (!m_requested) {
                        m_start.request(macro.tps);
                        m_requested = true;
                        progress = "holding the level at its start";
                    }
                    std::string why;
                    auto const step = m_start.service(why);
                    if (step == LevelStart::Step::Waiting) return false;
                    if (step == LevelStart::Step::Failed) {
                        error = why;
                        m_start.release();
                        return true;
                    }
                    return begin();
                }
                return run(until);
            }

            void cancel() override {
                if (sim::open()) sim::end();
                m_start.release();
            }

        private:
            LevelStart m_start;
            bool m_requested = false;
            bool m_running = false;
            int m_startFrame = 0;
            float m_startX = 0.0f;
            size_t m_next = 0;
            int m_early = 0;
            bool m_down = false;
            uint32_t m_ticks = 0;
            uint32_t m_limit = 0;
            float m_finalX = 0.0f;
            double m_simMs = 0.0;
            sim::State m_state;
            static constexpr uint32_t kSnapEvery = 1000;
            sim::SnapshotPtr m_snap = sim::newSnapshot();
            uint32_t m_snapTick = 0;
            size_t m_snapNext = 0;
            bool m_snapDown = false;
            int m_saves = 0;
            double m_saveUs = 0.0;

            bool begin() {
                auto* pl = PlayLayer::get();
                auto start = m_start.start();
                std::string why;
                if (!sim::begin(pl, start, why)) {
                    error = "the AI session could not begin: " + why;
                    m_start.release();
                    return true;
                }
                m_startFrame = sim::startFrame(*start);
                m_startX = sim::startX(*start);
                sim::read(m_state);
                m_down = m_state.p[0].held;
                m_finalX = m_state.p[0].x;
                int64_t last = events.empty() ? 0 : events.back().frame;
                int64_t const tps = (int64_t)GucciEngine::get()->updater.getTps();
                int64_t limit = maxTicks > 0 ? maxTicks : std::max<int64_t>(0, last - m_startFrame) + 60 * tps;
                m_limit = (uint32_t)std::clamp<int64_t>(limit, 1, 50'000'000);
                // Inputs at or before the start's own frame happened in ticks
                // the real game ran getting the level going: they go in on the
                // first simulated tick (counted, see early_inputs).
                for (Event const& e : events) {
                    if (e.frame - m_startFrame - 1 < 0) m_early++;
                }
                m_running = true;
                progress = "simulating";
                sim::suspend();  // the first slice starts on the next frame
                return false;
            }

            // Steps until the run is over (true) or, with a deadline, the time
            // is up (false).
            bool advance(job::Clock::time_point until, bool timed, char const*& why) {
                for (;;) {
                    if (m_ticks >= m_limit) {
                        why = "out of ticks";
                        return true;
                    }
                    // A snapshot every kSnapEvery ticks into the same object
                    // (the restore check below; and its cost, for the record).
                    if (timed && m_ticks % kSnapEvery == 0) {
                        auto const s0 = job::Clock::now();
                        sim::save(*m_snap);
                        m_saveUs += std::chrono::duration<double, std::micro>(job::Clock::now() - s0).count();
                        m_saves++;
                        m_snapTick = m_ticks;
                        m_snapNext = m_next;
                        m_snapDown = m_down;
                    }
                    uint8_t presses = 0;
                    bool held = m_down;
                    int64_t const frame = (int64_t)m_startFrame + 1 + (int64_t)m_ticks;
                    while (m_next < events.size() && events[m_next].frame <= frame) {
                        if (events[m_next].down) {
                            if (presses < 255) presses++;
                            held = true;
                        } else {
                            held = false;
                        }
                        m_next++;
                    }
                    m_down = held;
                    bool const dead = sim::step(presses, held);
                    m_ticks++;
                    sim::read(m_state);
                    m_finalX = m_state.p[0].x;
                    if (dead || m_state.complete) return true;
                    if (timed && (m_ticks & 127) == 0 && job::Clock::now() >= until) return false;
                }
            }

            bool run(job::Clock::time_point until) {
                if (!sim::open()) {
                    error = "the AI session was closed (the level was left?)";
                    m_start.release();
                    return true;
                }
                auto const t0 = job::Clock::now();
                sim::resume();
                char const* why = nullptr;
                bool const over = advance(until, true, why);
                m_simMs += std::chrono::duration<double, std::milli>(job::Clock::now() - t0).count();
                if (!over) {
                    sim::suspend();
                    progress = fmt::format("tick {} of at most {}, x {:.1f}", m_ticks, m_limit, m_finalX);
                    return false;
                }
                // The restore check: back to the last snapshot, the same inputs
                // again, and the run has to end the same to the bit.
                auto check = matjson::Value::object();
                if (m_saves > 0) {
                    uint32_t const ticks1 = m_ticks;
                    float const x1 = m_finalX, y1 = m_state.p[0].y;
                    bool const dead1 = m_state.dead, complete1 = m_state.complete;
                    sim::State const state1 = m_state;
                    auto const r0 = job::Clock::now();
                    sim::restore(*m_snap);
                    double const restoreUs = std::chrono::duration<double, std::micro>(job::Clock::now() - r0).count();
                    m_ticks = m_snapTick;
                    m_next = m_snapNext;
                    m_down = m_snapDown;
                    char const* why2 = nullptr;
                    (void)advance({}, false, why2);
                    bool const same = m_ticks == ticks1 && m_finalX == x1 && m_state.p[0].y == y1 &&
                                      m_state.dead == dead1 && m_state.complete == complete1;
                    check["from_tick"] = (int64_t)m_snapTick;
                    check["same"] = same;
                    check["restore_us"] = restoreUs;
                    check["save_us_avg"] = m_saveUs / m_saves;
                    check["saves"] = m_saves;
                    if (!same) {
                        check["second_ticks"] = (int64_t)m_ticks;
                        check["second_x"] = (double)m_finalX;
                        check["second_y"] = (double)m_state.p[0].y;
                        m_ticks = ticks1;
                        m_state = state1;
                        m_finalX = x1;
                    }
                }
                finish(why, check);
                return true;
            }

            void finish(char const* why, matjson::Value const& check) {
                auto out = matjson::Value::object();
                bool const died = m_state.dead;
                out["ticks"] = (int64_t)m_ticks;
                out["completed"] = m_state.complete && !died;
                out["died_tick"] = died ? matjson::Value((int64_t)m_ticks - 1) : matjson::Value(nullptr);
                out["died_frame"] =
                    died ? matjson::Value((int64_t)m_startFrame + (int64_t)m_ticks) : matjson::Value(nullptr);
                out["died_x"] = died ? matjson::Value((double)m_finalX) : matjson::Value(nullptr);
                out["final_x"] = (double)m_finalX;
                out["final_y"] = (double)m_state.p[0].y;
                out["level_length"] = (double)m_state.levelLength;
                if (died) {
                    auto k = matjson::Value::object();
                    k["id"] = m_state.killerId;
                    k["uid"] = m_state.killerUid;
                    k["x"] = (double)m_state.killerX;
                    k["y"] = (double)m_state.killerY;
                    k["void"] = m_state.voidDeath;
                    k["player2"] = m_state.deadP2;
                    out["killer"] = k;
                }
                if (why) out["stopped"] = why;
                out["start_frame"] = m_startFrame;
                out["start_x"] = (double)m_startX;
                out["tps"] = GucciEngine::get()->updater.getTps();
                out["offset"] = offset;
                out["early_inputs"] = m_early;
                out["inputs_used"] = (int64_t)m_next;
                auto mac = matjson::Value::object();
                mac["file"] = fileName;
                mac["format"] = format;
                mac["tps"] = macro.tps;
                mac["inputs"] = (int64_t)events.size();
                mac["last_frame"] = events.empty() ? 0 : events.back().frame;
                mac["p2_merged"] = p2Merged;
                mac["p2_dropped"] = p2Dropped;
                mac["other_buttons"] = otherButtons;
                mac["tps_changes"] = (int64_t)macro.tpsChanges.size();
                out["macro"] = mac;
                out["restore_check"] = check;
                out["sim_ms"] = m_simMs;
                out["us_per_tick"] = m_ticks ? m_simMs * 1000.0 / m_ticks : 0.0;
                result = out;
                sim::end();
                m_start.release();
                progress = "done";
                geode::log::info("[AI] sim smoke {} offset {}: {} ticks, {}, x {:.1f} of {:.1f}", fileName, offset,
                                 m_ticks, m_state.complete && !died ? "completed" : (died ? "died" : "stopped"),
                                 m_finalX, m_state.levelLength);
            }
        };

    }  // namespace

    std::unique_ptr<job::Job> makeSimSmoke(std::string const& macroPath, int64_t levelId, int offset,
                                           int64_t maxTicks, std::string& why) {
#if !GB_NATIVE_ENGINE
        (void)macroPath, (void)levelId, (void)offset, (void)maxTicks;
        why = "The AI's simulation is Windows-only for now: its model of the level reads GD's Windows memory layout.";
        return nullptr;
#endif
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_level || !pl->m_player1) {
            why = "open the level first";
            return nullptr;
        }
        int64_t const openId = (int64_t)pl->m_level->m_levelID.value();
        if (levelId != 0 && openId != levelId) {
            why = fmt::format("the open level is {}, not {}: open it first", openId, levelId);
            return nullptr;
        }
        auto* gb = GucciEngine::get();
        if (gb->isRecording() || gb->isPlaying()) {
            why = "GucciBot is recording or playing a macro: set the mode to idle first";
            return nullptr;
        }
        std::filesystem::path const p(std::u8string(macroPath.begin(), macroPath.end()));
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            why = "can't open " + macroPath;
            return nullptr;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto const u8name = p.filename().u8string();
        std::string const fileName(u8name.begin(), u8name.end());
        // Any format the converter reads; GucciBot's own (GBR6) it hands back
        // as already ours, so that one is read directly, with the offset added.
        convert::ReadResult read;
        if (convert::isGucciBot(bytes)) {
            std::string err;
            if (!convert::fromGucciBot(bytes, read.macro, err)) {
                why = "the GucciBot macro could not be read: " + err;
                return nullptr;
            }
            for (auto& in : read.macro.inputs) in.frame += offset;
            for (auto& c : read.macro.tpsChanges) c.frame += offset;
        } else {
            read = convert::read(bytes, fileName, offset);
            if (!read.ok()) {
                why = "the macro could not be read: " +
                      (read.error.empty() ? std::string("unknown format") : read.error);
                return nullptr;
            }
        }
        auto job = std::make_unique<SimSmoke>();
        job->path = macroPath;
        job->fileName = fileName;
        job->levelId = openId;
        job->offset = offset;
        job->maxTicks = maxTicks;
        job->format = read.format ? std::string(read.format->id) : std::string("gbr6");
        job->macro = std::move(read.macro);
        // One button stream for player 1: in a one-player level the game hands
        // player 2's inputs to player 1 too (DATA.md section 8). A two-player
        // level's player 2 is not simulated here (stage A).
        bool const twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
        for (auto const& in : job->macro.inputs) {
            if (in.button != 1) {
                job->otherButtons++;
                continue;
            }
            if (in.player2) {
                if (twoPlayer) {
                    job->p2Dropped++;
                    continue;
                }
                job->p2Merged++;
            }
            job->events.push_back(Event{in.frame, in.down});
        }
        return job;
    }

} // namespace gucci::ai
