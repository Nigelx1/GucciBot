#include "ai/rollout.hpp"

#include "absense/glue.hpp"
#include "ai/net.hpp"
#include "ai/observe.hpp"
#include "ai/pilot.hpp"
#include "ai/record.hpp"
#include "ai/sim.hpp"
#include "ai/start.hpp"
#include "core/GucciBot.hpp"
#include "core/platform.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace gucci::ai::rollout {

    float reward(float dx, float levelLength, bool died, bool completed) {
        float r = levelLength > 0.0f ? 10.0f * dx / levelLength : 0.0f;
        if (died) r -= 1.0f;
        if (completed) r += 2.0f;
        return r;
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

        uint64_t splitmix(uint64_t& x) {
            uint64_t z = (x += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        double uniform(uint64_t& rng) {
            return (double)(splitmix(rng) >> 11) * (1.0 / 9007199254740992.0);
        }

        // log(sigmoid(z)), stable for large |z|.
        double logSigmoid(double z) {
            return z >= 0.0 ? -std::log1p(std::exp(-z)) : z - std::log1p(std::exp(z));
        }

        template <class T>
        bool writeArray(fs::path const& p, std::vector<T> const& v) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<char const*>(v.data()), (std::streamsize)(v.size() * sizeof(T)));
            return (bool)f;
        }

        uint64_t fnv1a(std::vector<uint8_t> const& bytes) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint8_t b : bytes) {
                h ^= b;
                h *= 0x100000001b3ull;
            }
            return h;
        }

        // The human run of this level at this tick rate: its act per tick.
        struct HumanRun {
            std::string id;
            std::vector<uint8_t> act;
            uint32_t usable = 0;  // ticks worth starting from
        };

        bool findHuman(int64_t levelId, double tps, HumanRun& out) {
            std::error_code ec;
            auto const dir = aiRoot() / "data";
            if (!fs::is_directory(dir, ec)) return false;
            std::string const prefix = std::to_string(levelId) + "__";
            for (auto const& e : fs::directory_iterator(dir, ec)) {
                auto const name = utf8(e.path().filename());
                if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".json") continue;
                std::ifstream f(e.path(), std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                auto parsed = matjson::parse(text);
                if (!parsed) continue;
                auto const h = parsed.unwrap();
                if (h["layout_hash"].asString().unwrapOr("") != obs::layoutHash()) continue;
                double const rate = h["tps"].asDouble().unwrapOr(0.0);
                if (std::fabs(rate - tps) > 0.5) continue;
                int64_t const T = h["ticks"].asInt().unwrapOr(0);
                bool const completed = h["completed"].asBool().unwrapOr(false);
                int64_t usable = T;
                if (!completed) {
                    int64_t const died = h["died_tick"].asInt().unwrapOr(0);
                    usable = died - (int64_t)std::llround(2.0 * tps);
                }
                if (usable <= (int64_t)out.usable) continue;
                auto base = utf8(e.path());
                base.resize(base.size() - 5);
                std::ifstream a(u8path(base + ".act.u8"), std::ios::binary);
                std::vector<uint8_t> act((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
                if ((int64_t)act.size() != T) continue;
                out.id = utf8(e.path().stem());
                out.act = std::move(act);
                out.usable = (uint32_t)std::max<int64_t>(0, usable);
            }
            return out.usable > 0;
        }

        class Rollout : public job::Job {
        public:
            char const* kind() const override { return "rollout"; }
            bool holdsGame() const override { return m_start.requested(); }

            Options opt;

            bool slice(Clock::time_point until) override {
                if (m_t0 == Clock::time_point{}) m_t0 = Clock::now();
                if (!m_running) {
                    if (!m_requested) {
                        m_start.request(0.0);
                        m_requested = true;
                        progress = "holding the level at its start";
                    }
                    std::string why;
                    auto const step = m_start.service(why);
                    if (step == LevelStart::Step::Waiting) return false;
                    if (step == LevelStart::Step::Failed) {
                        error = why;
                        stopAll();
                        return true;
                    }
                    return begin();
                }
                if (!sim::open()) {
                    error = "the AI session was closed (the level was left?)";
                    stopAll();
                    return true;
                }
                sim::resume();
                auto const w0 = Clock::now();
                bool const over = m_humanPhase ? replayHuman(until) : run(until);
                m_workS += std::chrono::duration<double>(Clock::now() - w0).count();
                if (over) return finish();
                sim::suspend();
                progress = fmt::format("{} ticks in {} episodes, {:.0f} ticks/s; best from the start {:.1f}%",
                                       m_ticksWritten, m_episodes, ticksPerSecond(), m_bestFromStart);
                return false;
            }

            void cancel() override {
                if (sim::open()) sim::end();
                flush();
                stopAll();
            }

        private:
            struct Point {
                sim::SnapshotPtr s;
                obs::Observer::History h;
                bool prev = false;
                uint32_t tick = 0;
                float x = 0.0f;
            };

            LevelStart m_start;
            bool m_requested = false;
            bool m_running = false;
            Clock::time_point m_t0{};
            double m_workS = 0.0;
            double m_oldSlice = 0.0;
            obs::Observer m_obs;
            net::Net* m_pilot = nullptr;
            int m_hold = 0;
            std::string m_pilotPath, m_pilotHash;
            double m_tps = 240.0;
            float m_levelLength = 0.0f;
            std::string m_levelName;
            int64_t m_levelId = 0;
            uint64_t m_rng = 0;
            uint32_t m_maxEp = 0;

            Point m_origin;                  // tick 0
            std::vector<Point> m_human;      // along the human run
            std::vector<Point> m_frontier;   // furthest points reached, newest last
            HumanRun m_run;
            bool m_humanPhase = false;
            uint32_t m_ht = 0;
            bool m_hprev = false;
            uint32_t m_hEvery = 120;

            // the episode
            bool m_inEpisode = false;
            int m_epKind = 0;  // 0 start, 1 human, 2 frontier
            uint32_t m_t = 0;  // tick from the level's start
            uint32_t m_epTicks = 0;
            bool m_prev = false;
            float m_x = 0.0f, m_epX0 = 0.0f, m_epMaxX = 0.0f, m_frontierX = 0.0f;
            double m_epReturn = 0.0;
            int32_t m_ep = 0;

            // the batch being written
            std::vector<float> m_state, m_geo, m_logp, m_reward;
            std::vector<uint8_t> m_scan, m_tok, m_act, m_done, m_trunc;
            std::vector<int32_t> m_tick, m_epId;
            int m_part = 0;
            std::vector<std::string> m_files;
            std::string m_writeError;

            // totals
            int64_t m_ticksWritten = 0;
            int m_episodes = 0, m_deaths = 0, m_completions = 0, m_truncated = 0;
            double m_returnSum = 0.0, m_progressSum = 0.0;
            int m_kindCount[3] = {}, m_kindDone[3] = {};
            double m_kindProgress[3] = {};
            float m_bestFromStart = 0.0f;
            double m_entropySum = 0.0;

            double ticksPerSecond() const { return m_workS > 0.0 ? (double)m_ticksWritten / m_workS : 0.0; }
            double wallSeconds() const { return std::chrono::duration<double>(Clock::now() - m_t0).count(); }
            float percentOf(float x) const {
                return m_levelLength > 0.0f ? std::clamp(100.0f * x / m_levelLength, 0.0f, 100.0f) : 0.0f;
            }

            void stopAll() {
                m_start.release();
                if (m_oldSlice > 0.0) {
                    job::setSliceMs(m_oldSlice);
                    m_oldSlice = 0.0;
                }
            }

            void savePoint(Point& p) {
                if (!p.s) p.s = sim::newSnapshot();
                sim::save(*p.s);
                p.h = m_obs.history();
                p.prev = m_prev;
                p.tick = m_t;
                p.x = m_x;
            }

            void loadPoint(Point const& p) {
                sim::restore(*p.s);
                m_obs.setHistory(p.h);
                m_prev = p.prev;
                m_t = p.tick;
                m_x = p.x;
            }

            bool begin() {
                auto* pl = PlayLayer::get();
                if (!pl || !pl->m_level) {
                    error = "the level was left";
                    stopAll();
                    return true;
                }
                m_levelName = std::string(pl->m_level->m_levelName);
                m_levelId = (int64_t)pl->m_level->m_levelID.value();
                if (opt.levelId != 0 && opt.levelId != m_levelId) {
                    error = fmt::format("the open level is {}, not {}", m_levelId, opt.levelId);
                    stopAll();
                    return true;
                }
                m_pilot = pilot::pilotNet(m_hold, m_pilotPath);
                if (!m_pilot) {
                    error = pilot::weightsLine();
                    stopAll();
                    return true;
                }
                {
                    std::ifstream f(u8path(m_pilotPath), std::ios::binary);
                    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    m_pilotHash = fmt::format("{:016x}", fnv1a(bytes));
                }
                std::string why;
                if (!m_obs.begin(pl, why)) {
                    error = "the level could not be read: " + why;
                    stopAll();
                    return true;
                }
                auto start = m_start.start();
                if (!sim::begin(pl, start, why)) {
                    error = "the AI session could not begin: " + why;
                    stopAll();
                    return true;
                }
                m_tps = GucciEngine::get()->updater.getTps();
                m_obs.setTps(m_tps);
                sim::State st;
                sim::read(st);
                m_levelLength = st.levelLength > 0.0f ? st.levelLength : pl->m_levelLength;
                m_x = st.p[0].x;
                m_t = 0;
                m_prev = false;
                m_maxEp = (uint32_t)std::max(60.0, std::round(opt.maxEpisodeSeconds * m_tps));
                m_rng = opt.seed * 0x9E3779B97F4A7C15ull ^ ((uint64_t)m_levelId * 0x100000001B3ull);
                savePoint(m_origin);
                if (opt.sliceMs > 0.0) {
                    m_oldSlice = job::sliceMs();
                    job::setSliceMs(opt.sliceMs);
                }
                m_humanPhase = opt.mixHuman > 0.0 && findHuman(m_levelId, m_tps, m_run);
                if (m_humanPhase) {
                    m_hEvery = std::max<uint32_t>(30, m_run.usable / 400);
                    m_hprev = false;
                    m_ht = 0;
                }
                m_running = true;
                sim::suspend();
                return false;
            }

            // The human run replayed once in the copies; a saved point every
            // m_hEvery ticks, up to two seconds before its end or death.
            bool replayHuman(Clock::time_point until) {
                sim::State st;
                for (int n = 1;; n++) {
                    bool stop = m_ht >= m_run.usable;
                    if (!stop && m_ht > 0 && m_ht % m_hEvery == 0) {
                        m_human.emplace_back();
                        savePoint(m_human.back());
                    }
                    if (!stop) {
                        bool const held = m_run.act[m_ht] != 0;
                        bool const dead = sim::step(held && !m_hprev ? 1 : 0, held);
                        sim::read(st);
                        m_obs.after(st.p[0].button);
                        m_prev = m_hprev = held;
                        m_ht++;
                        m_t = m_ht;
                        m_x = st.p[0].x;
                        if (dead || st.complete) {
                            // Points within two seconds of where it died are no start.
                            uint32_t const back = (uint32_t)std::llround(2.0 * m_tps);
                            while (!m_human.empty() && m_human.back().tick + back > m_ht) m_human.pop_back();
                            stop = true;
                        }
                    }
                    if (stop) {
                        m_humanPhase = false;
                        loadPoint(m_origin);
                        return false;
                    }
                    if ((n & 255) == 0 && Clock::now() >= until) return false;
                }
            }

            void startEpisode() {
                double const total = std::max(1e-9, opt.mixStart + opt.mixHuman + opt.mixFrontier);
                double u = uniform(m_rng) * total;
                int kind = 0;
                if (u >= opt.mixStart) {
                    u -= opt.mixStart;
                    kind = u < opt.mixHuman ? 1 : 2;
                }
                if (kind == 1 && m_human.empty()) kind = 0;
                // The newest frontier point is where it keeps dying: start a little before it.
                if (kind == 2 && m_frontier.size() < 2) kind = 0;
                if (kind == 0) {
                    loadPoint(m_origin);
                } else if (kind == 1) {
                    loadPoint(m_human[(size_t)(uniform(m_rng) * (double)m_human.size()) % m_human.size()]);
                } else {
                    size_t const n = m_frontier.size() - 1;
                    size_t const lo = n > 6 ? n - 6 : 0;
                    loadPoint(m_frontier[lo + (size_t)(uniform(m_rng) * (double)(n - lo)) % (n - lo)]);
                }
                m_epKind = kind;
                m_epTicks = 0;
                m_epX0 = m_epMaxX = m_x;
                m_epReturn = 0.0;
                m_inEpisode = true;
                m_ep++;
            }

            void endEpisode(bool died, bool completed, bool truncated) {
                if (!m_done.empty()) {
                    m_done.back() = (died || completed) ? 1 : 0;
                    m_trunc.back() = truncated && !died && !completed ? 1 : 0;
                }
                m_inEpisode = false;
                m_episodes++;
                if (died) m_deaths++;
                if (completed) m_completions++;
                if (truncated && !died && !completed) m_truncated++;
                double const prog = m_levelLength > 0.0f ? (double)(m_epMaxX - m_epX0) / m_levelLength : 0.0;
                m_returnSum += m_epReturn;
                m_progressSum += prog;
                m_kindCount[m_epKind]++;
                m_kindProgress[m_epKind] += prog;
                if (completed) m_kindDone[m_epKind]++;
                if (m_epKind == 0) m_bestFromStart = std::max(m_bestFromStart, completed ? 100.0f : percentOf(m_epMaxX));
                if (m_act.size() >= 200000) flush();
            }

            bool run(Clock::time_point until) {
                obs::Record rec;
                float x[obs::kObsDim];
                float out[4];
                sim::State st;
                double const temp = std::max(0.05, opt.temperature);
                uint32_t const frontierEvery = (uint32_t)std::max(30.0, std::round(0.5 * m_tps));
                for (int n = 1;; n++) {
                    if (!m_inEpisode) {
                        if (m_ticksWritten >= opt.maxTicks || wallSeconds() >= opt.seconds) return true;
                        startEpisode();
                    }
                    // A new furthest point (every half second of play): a start for later episodes.
                    if (m_epTicks > 0 && m_epTicks % frontierEvery == 0 && m_x > m_frontierX + 30.0f) {
                        m_frontierX = m_x;
                        if (m_frontier.size() >= 16) {
                            auto s = std::move(m_frontier.front().s);
                            m_frontier.erase(m_frontier.begin());
                            m_frontier.emplace_back();
                            m_frontier.back().s = std::move(s);
                        } else {
                            m_frontier.emplace_back();
                        }
                        savePoint(m_frontier.back());
                    }
                    m_obs.observe(rec);
                    obs::expand(rec, x);
                    m_pilot->forward(x, out);
                    double const z = (double)out[m_hold] / temp;
                    double const ph = 1.0 / (1.0 + std::exp(-z));
                    bool const held = uniform(m_rng) < ph;
                    double const lp = held ? logSigmoid(z) : logSigmoid(-z);
                    if (ph > 0.0 && ph < 1.0) m_entropySum += -(ph * std::log(ph) + (1.0 - ph) * std::log(1.0 - ph));
                    bool const dead = sim::step(held && !m_prev ? 1 : 0, held);
                    sim::read(st);
                    m_obs.after(st.p[0].button);
                    float const nx = st.p[0].x;
                    bool const completed = !dead && st.complete;
                    float const r = reward(nx - m_x, m_levelLength, dead, completed);
                    m_state.insert(m_state.end(), rec.state, rec.state + obs::kStateDim);
                    m_scan.insert(m_scan.end(), rec.scan, rec.scan + obs::kScanDim);
                    m_tok.insert(m_tok.end(), rec.tok, rec.tok + obs::kTokDim);
                    m_geo.insert(m_geo.end(), rec.geo, rec.geo + obs::kGeoDim);
                    m_act.push_back(held ? 1 : 0);
                    m_logp.push_back((float)lp);
                    m_reward.push_back(r);
                    m_done.push_back(0);
                    m_trunc.push_back(0);
                    m_tick.push_back((int32_t)m_t);
                    m_epId.push_back(m_ep);
                    m_ticksWritten++;
                    m_epReturn += r;
                    m_prev = held;
                    m_t++;
                    m_epTicks++;
                    m_x = nx;
                    m_epMaxX = std::max(m_epMaxX, nx);
                    if (dead) endEpisode(true, false, false);
                    else if (completed) endEpisode(false, true, false);
                    else if (m_epTicks >= m_maxEp) endEpisode(false, false, true);
                    if ((n & 63) == 0 && Clock::now() >= until) return false;
                }
            }

            // The batch so far to <tag>_<n>.*
            void flush() {
                if (m_act.empty()) return;
                auto const dir = aiRoot() / "rl" / "rollouts";
                std::error_code ec;
                fs::create_directories(dir, ec);
                auto const base = utf8(dir / fmt::format("{}_{}", opt.tag, m_part++));
                auto pathOf = [&](char const* ext) { return u8path(base + ext); };
                size_t const T = m_act.size();
                bool ok = writeArray(pathOf(".state.f32"), m_state) && writeArray(pathOf(".scan.u8"), m_scan) &&
                          writeArray(pathOf(".tok.u8"), m_tok) && writeArray(pathOf(".geo.f32"), m_geo) &&
                          writeArray(pathOf(".act.u8"), m_act) && writeArray(pathOf(".logp.f32"), m_logp) &&
                          writeArray(pathOf(".reward.f32"), m_reward) && writeArray(pathOf(".done.u8"), m_done) &&
                          writeArray(pathOf(".trunc.u8"), m_trunc) && writeArray(pathOf(".tick.i32"), m_tick) &&
                          writeArray(pathOf(".ep.i32"), m_epId);
                auto h = matjson::Value::object();
                h["format"] = "gbai-rollout";
                h["version"] = 1;
                h["level_id"] = m_levelId;
                h["level_name"] = m_levelName;
                h["level_length_x"] = (double)m_levelLength;
                h["tps"] = m_tps;
                h["rows"] = (int64_t)T;
                h["layout_hash"] = obs::layoutHash();
                h["pilot_hash"] = m_pilotHash;
                h["pilot_file"] = m_pilotPath;
                h["temperature"] = opt.temperature;
                h["seed"] = (int64_t)opt.seed;
                h["human_run"] = m_run.id;
                h["build"] = GB_BUILD_LABEL;
                {
                    std::ofstream f(pathOf(".json"), std::ios::binary | std::ios::trunc);
                    f << h.dump(2);
                    ok = ok && (bool)f;
                }
                if (ok) m_files.push_back(base + ".json");
                else if (m_writeError.empty()) m_writeError = "could not write " + base;
                m_state.clear(), m_scan.clear(), m_tok.clear(), m_geo.clear(), m_act.clear(), m_logp.clear();
                m_reward.clear(), m_done.clear(), m_trunc.clear(), m_tick.clear(), m_epId.clear();
            }

            bool finish() {
                // An episode the clock cut: its last row is truncated, not failed.
                if (m_inEpisode) endEpisode(false, false, true);
                sim::end();
                flush();
                stopAll();
                auto out = matjson::Value::object();
                out["level_id"] = m_levelId;
                out["level_name"] = m_levelName;
                out["ticks"] = m_ticksWritten;
                out["episodes"] = m_episodes;
                out["deaths"] = m_deaths;
                out["completions"] = m_completions;
                out["truncated"] = m_truncated;
                out["mean_return"] = m_episodes ? m_returnSum / m_episodes : 0.0;
                out["mean_progress"] = m_episodes ? m_progressSum / m_episodes : 0.0;
                out["mean_entropy"] = m_ticksWritten ? m_entropySum / (double)m_ticksWritten : 0.0;
                out["best_percent_from_start"] = (double)m_bestFromStart;
                char const* names[3] = {"start", "human", "frontier"};
                auto kinds = matjson::Value::object();
                for (int k = 0; k < 3; k++) {
                    auto o = matjson::Value::object();
                    o["episodes"] = m_kindCount[k];
                    o["completions"] = m_kindDone[k];
                    o["mean_progress"] = m_kindCount[k] ? m_kindProgress[k] / m_kindCount[k] : 0.0;
                    kinds[names[k]] = o;
                }
                out["by_start"] = kinds;
                out["human_run"] = m_run.id;
                out["human_points"] = (int64_t)m_human.size();
                out["frontier_points"] = (int64_t)m_frontier.size();
                out["work_seconds"] = m_workS;
                out["wall_seconds"] = wallSeconds();
                out["ticks_per_second"] = ticksPerSecond();
                out["ticks_per_wall_second"] = wallSeconds() > 0.0 ? (double)m_ticksWritten / wallSeconds() : 0.0;
                out["tps"] = m_tps;
                out["pilot_hash"] = m_pilotHash;
                auto files = matjson::Value::array();
                for (auto const& f : m_files) files.push(f);
                out["files"] = files;
                if (!m_writeError.empty()) out["write_error"] = m_writeError;
                result = out;
                progress = fmt::format("done: {} ticks, {} episodes", m_ticksWritten, m_episodes);
                return true;
            }
        };

    }  // namespace

    std::unique_ptr<job::Job> make(Options const& options, std::string& why) {
#if !GB_NATIVE_ENGINE
        why = "The AI's simulation is Windows-only for now: its model of the level reads GD's Windows memory layout.";
        return nullptr;
#else
        if (!pilot::ready()) {
            why = pilot::weightsLine();
            return nullptr;
        }
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 || !pl->m_level) {
            why = "open a level first";
            return nullptr;
        }
        if (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode) {
            why = "the AI plays classic levels, not platformer ones";
            return nullptr;
        }
        auto* gb = GucciEngine::get();
        if (gb->isRecording() || gb->isPlaying()) {
            why = "GucciBot is recording or playing a macro: set the mode to idle first";
            return nullptr;
        }
        if (absense::status().running || absense::startPending()) {
            why = "the old search is running: stop it first";
            return nullptr;
        }
        if (job::busy()) {
            why = "another AI job is running: wait for it or stop it";
            return nullptr;
        }
        auto job = std::make_unique<Rollout>();
        job->opt = options;
        job->opt.seconds = std::clamp(job->opt.seconds, 1.0, 6.0 * 3600.0);
        job->opt.maxTicks = std::clamp<int64_t>(job->opt.maxTicks, 1, 20'000'000);
        job->opt.maxEpisodeSeconds = std::clamp(job->opt.maxEpisodeSeconds, 0.5, 1200.0);
        job->opt.mixStart = std::max(0.0, job->opt.mixStart);
        job->opt.mixHuman = std::max(0.0, job->opt.mixHuman);
        job->opt.mixFrontier = std::max(0.0, job->opt.mixFrontier);
        return job;
#endif
    }

} // namespace gucci::ai::rollout
