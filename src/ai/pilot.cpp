#include "ai/pilot.hpp"

#include "absense/glue.hpp"
#include "absense/judge.hpp"
#include "ai/net.hpp"
#include "ai/observe.hpp"
#include "ai/record.hpp"
#include "ai/sim.hpp"
#include "ai/start.hpp"
#include "ai/tape.hpp"
#include "core/GucciBot.hpp"
#include "core/platform.hpp"
#include "tools/macro_convert.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace gucci::ai::pilot {

    namespace {

        using Clock = job::Clock;

        std::string utf8(fs::path const& p) {
            auto const u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        fs::path u8path(std::string const& s) {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        // Letters, digits, spaces and a few marks: a macro's file name.
        std::string fileSafe(std::string const& s) {
            std::string out;
            for (unsigned char c : s) {
                if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')') out += (char)c;
                else out += '_';
            }
            while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
            if (out.empty()) out = "AI run";
            return out.substr(0, 100);
        }

        // ------------------------------------------------------------ the networks

        struct Model {
            char const* name;
            net::Net net;
            std::string path;
            std::string error;
        };

        struct Models {
            Model pilot{"pilot"}, critic{"critic"}, reader{"reader"};
            bool tried = false;
            int hold = -1;
            int aliveHold = -1, aliveRelease = -1;
        };

        Models& models() {
            static Models m;
            return m;
        }

        fs::path findWeights(char const* name) {
            std::error_code ec;
            std::string const file = std::string(name) + ".gbnn";
            fs::path const res = Mod::get()->getResourcesDir();
            for (fs::path const& dir : {weightsDir(), res / "ai", res}) {
                auto const p = dir / file;
                if (fs::is_regular_file(p, ec)) return p;
            }
            return {};
        }

        void loadOne(Model& m, std::string const& hash) {
            m.net = net::Net();
            m.path.clear();
            m.error.clear();
            auto const p = findWeights(m.name);
            if (p.empty()) {
                m.error = fmt::format("no {}.gbnn", m.name);
                return;
            }
            m.path = utf8(p);
            std::ifstream f(p, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!f && !f.eof()) {
                m.error = "can't read " + m.path;
                return;
            }
            std::string why;
            if (!m.net.load(bytes.data(), bytes.size(), hash, why)) {
                m.net = net::Net();
                m.error = why;
            }
        }

        void refuse(Model& m, std::string const& why) {
            m.net = net::Net();
            m.error = why;
        }

        matjson::Value modelJson(Model const& m) {
            auto o = matjson::Value::object();
            o["name"] = m.name;
            o["loaded"] = m.net.loaded();
            o["path"] = m.path.empty() ? matjson::Value(nullptr) : matjson::Value(m.path);
            if (m.net.loaded()) {
                o["layout_hash"] = m.net.layoutHash();
                o["kind"] = m.net.kind() == net::Kind::Mlp ? "mlp" : "conv1d";
                o["inputs"] = (int)m.net.inputs();
                o["outputs"] = (int)m.net.outputs();
                o["layers"] = (int)m.net.layers().size();
                auto names = matjson::Value::array();
                for (auto const& n : m.net.outputNames()) names.push(n);
                o["output_names"] = names;
            } else {
                o["error"] = m.error;
            }
            return o;
        }

        // ------------------------------------------------------------ the menu's view

        Live s_live;

        void say(std::string const& line) {
            log::info("[AI] {}", line);
            s_live.lines.push_back(line);
            while (s_live.lines.size() > 8) s_live.lines.pop_front();
        }

        // ------------------------------------------------------------ deaths in words

        constexpr char const* kModeNames[8] = {"cube", "ship", "ball", "UFO", "wave", "robot", "spider", "swing"};

        char const* modeName(int mode) {
            return mode >= 0 && mode < 8 ? kModeNames[mode] : "player";
        }

        char const* aOrAn(char const* word) {
            return std::string("aeiouAEIOU").find(word[0]) != std::string::npos && word[0] != 'U' ? "an" : "a";
        }

        // What an object is, in a word. Only ids that are certain get a name.
        std::string objectWord(obs::Item const* item, int id) {
            switch (id) {
                case 8: return "spike";
                case 39: return "small spike";
                case 103: return "medium spike";
                case 392: return "tiny spike";
                case 88: case 89: case 98: case 397: case 398: case 399: return "saw";
                default: break;
            }
            if (item) {
                switch (item->type) {
                    case obs::Item::Hazard: return item->hazardSlope ? "deadly slope" : fmt::format("hazard (object {})", id);
                    case obs::Item::Block: return fmt::format("block (object {})", id);
                    case obs::Item::Slope: return fmt::format("slope (object {})", id);
                    case obs::Item::Special: return fmt::format("object {}", id);
                }
            }
            return fmt::format("object {}", id);
        }

        // ------------------------------------------------------------ the practice loop

        struct Cand {
            uint32_t t = 0;
            uint32_t len = 1;
            bool v = false;  // what it forces: held or released
        };

        struct Target {
            uint32_t D = 0;       // the tick it died during
            float x = 0.0f;
            float percent = 0.0f;
            int attempt = 0;      // the attempt that died here: its line is the base
            std::string cause;
            std::map<uint32_t, bool> base;  // the forced decisions of that line
            uint32_t lo = 0;      // the window copies below cover ticks [lo, D]
            std::vector<uint8_t> d;
            std::vector<float> logit, adv;
            int level = -1;       // escalation level of `cands`
            std::vector<Cand> cands;
            size_t next = 0;
            // The human-like retry (2026-11-03): attempts at this death that
            // didn't get past it, and the variety rounds played so far.
            int fails = 0;
            int varRound = 0;
            int varLeft = 0;      // tries left in the current round
        };

        // How a death is attacked, from the first try to the last: a single
        // tick changed, then short, longer and long holds or releases over a
        // wider stretch before the death.
        struct Level {
            double windowS;
            double lenS;
            int minLen;
            int stride240;
            char const* what;
        };
        constexpr Level kLevels[] = {
            {1.5, 0.0, 1, 1, "one tick changed"},
            {1.5, 1.0 / 30.0, 2, 2, "short changes"},
            {2.0, 0.1, 3, 4, "longer changes"},
            {3.0, 0.25, 4, 8, "long changes over the last 3 seconds"},
        };
        constexpr int kLevelCount = (int)(sizeof(kLevels) / sizeof(kLevels[0]));
        constexpr uint32_t kSnapEvery = 30;
        constexpr size_t kMaxSnaps = 512;
        constexpr float kProgressX = 30.0f;  // a block further counts as getting past a death

        // The human-like retry (2026-11-03): single changes next to a death can
        // stay stuck on it for tens of thousands of attempts. Every kVarietyAfter
        // failed attempts at the same death, a round of kVarietyTries replays the
        // last few seconds (1.5 s, then 3 s, then 6 s) from the snapshot before
        // them, the pilot choosing hold or release by sampling its own
        // probabilities at a temperature that grows round by round. A try that
        // gets past the death is kept (its sampled choices become forced
        // decisions of the line), and the pilot plays greedy again from there.
        // Sampling is seeded from the level and the attempt, so a run reproduces.
        constexpr int kVarietyAfter = 20;
        constexpr int kVarietyTries = 8;
        constexpr double kVarietyWindowS[] = {1.5, 3.0, 6.0};
        constexpr float kVarietyTemp0 = 1.0f;
        constexpr float kVarietyTempStep = 0.5f;
        constexpr float kVarietyTempMax = 4.0f;

        uint64_t splitmix(uint64_t& x) {
            uint64_t z = (x += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        uint32_t divergence(std::map<uint32_t, bool> const& a, std::map<uint32_t, bool> const& b) {
            auto ia = a.begin();
            auto ib = b.begin();
            while (ia != a.end() && ib != b.end()) {
                if (ia->first != ib->first) return std::min(ia->first, ib->first);
                if (ia->second != ib->second) return ia->first;
                ++ia, ++ib;
            }
            if (ia != a.end()) return ia->first;
            if (ib != b.end()) return ib->first;
            return std::numeric_limits<uint32_t>::max();
        }

        template <class T>
        bool writeArray(fs::path const& p, T const* v, size_t n) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<char const*>(v), (std::streamsize)(n * sizeof(T)));
            return (bool)f;
        }

        class Play : public job::Job {
        public:
            char const* kind() const override { return "play"; }
            bool holdsGame() const override { return m_start.requested(); }

            PlayOptions opt;

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
                        m_start.release();
                        return true;
                    }
                    return begin();
                }
                if (!sim::open()) {
                    error = "the AI session was closed (the level was left?)";
                    m_start.release();
                    partialResult("cancelled");
                    return true;
                }
                sim::resume();
                bool const over = run(until);
                if (!over) {
                    sim::suspend();
                    progress = fmt::format("attempt {}: tick {}, {:.1f}% now, best {:.1f}%", m_attempt, m_t,
                                           percentOf(m_lastX), m_bestPercent);
                    s_live.bestPercent = m_bestPercent;
                    s_live.attempts = m_attempt;
                }
                return over;
            }

            void cancel() override {
                if (sim::open()) sim::end();
                m_start.release();
                partialResult("cancelled");
                say(fmt::format("Stopped after {} attempts; best {:.1f}%.", m_attempt, m_bestPercent));
            }

        private:
            struct Snap {
                sim::SnapshotPtr s;
                obs::Observer::History h;
                bool prev = false;
            };

            LevelStart m_start;
            bool m_requested = false;
            bool m_running = false;
            Clock::time_point m_t0{};
            obs::Observer m_obs;
            std::unordered_map<int, obs::Item const*> m_byUid;
            int m_startFrame = 0;
            double m_tps = 240.0;
            uint32_t m_H = 240;
            float m_levelLength = 0.0f;
            std::string m_levelName;
            int64_t m_levelId = 0;
            uint32_t m_limit = 0;
            bool m_critic = false;
            std::string m_stamp;

            // the line being played: one entry a tick from 0 to m_t
            uint32_t m_t = 0;
            bool m_prev = false;
            std::vector<uint8_t> m_d;
            std::vector<float> m_logit, m_adv;
            std::map<uint32_t, bool> m_forced;
            std::map<uint32_t, Snap> m_snaps;
            std::vector<sim::SnapshotPtr> m_pool;
            float m_lastX = 0.0f;
            // practice log rows of the line (when asked)
            std::vector<float> m_stateRows, m_metaRows, m_geoRows;
            std::vector<uint8_t> m_scanRows, m_tokRows, m_actRows;

            // the practice
            std::vector<Target> m_stack;
            int m_tryIdx = -1;      // the target whose candidate this attempt tries (-1: the first play)
            Cand m_try;
            // the variety retry this attempt plays (if any)
            bool m_tryVariety = false;
            double m_varWindowS = 0.0;
            bool m_varietyOn = false;  // sampling now
            uint32_t m_varFrom = 0;
            float m_varPastX = 0.0f;
            float m_varTemp = 1.0f;
            uint64_t m_varRng = 0;
            int m_varietyTries = 0;
            int m_varietyKept = 0;
            int m_attempt = 0;
            uint64_t m_freeRng = 0;  // the network-alone mode's sampling
            uint32_t m_first = 0;   // the tick this attempt began at (restored to)
            int m_branchAttempt = 0;
            uint64_t m_ticks = 0;   // simulated in all
            int m_deathsTotal = 0;
            float m_bestPercent = 0.0f;
            float m_bestX = 0.0f;
            int m_bestAttempt = 0;
            std::vector<matjson::Value> m_deaths;  // the newest 200
            double m_obsUs = 0.0;
            double m_netUs = 0.0;
            int m_logsWritten = 0;
            std::string m_logError;

            float percentOf(float x) const {
                if (m_levelLength <= 0.0f) return 0.0f;
                return std::clamp(100.0f * x / m_levelLength, 0.0f, 100.0f);
            }

            uint32_t ticksOf(double seconds) const {
                return (uint32_t)std::max(1.0, std::round(seconds * m_tps));
            }

            bool begin() {
                auto* pl = PlayLayer::get();
                if (!pl || !pl->m_level) {
                    error = "the level was left";
                    m_start.release();
                    return true;
                }
                auto& M = models();
                m_critic = M.critic.net.loaded();
                m_levelName = std::string(pl->m_level->m_levelName);
                m_levelId = (int64_t)pl->m_level->m_levelID.value();
                std::string why;
                if (!m_obs.begin(pl, why)) {
                    error = "the level could not be read: " + why;
                    m_start.release();
                    return true;
                }
                auto start = m_start.start();
                if (!sim::begin(pl, start, why)) {
                    error = "the AI session could not begin: " + why;
                    m_start.release();
                    return true;
                }
                m_tps = GucciEngine::get()->updater.getTps();
                m_H = (uint32_t)std::max(1.0, std::round(m_tps));
                m_obs.setTps(m_tps);
                m_startFrame = sim::startFrame(*start);
                sim::State st;
                sim::read(st);
                m_levelLength = st.levelLength > 0.0f ? st.levelLength : pl->m_levelLength;
                m_lastX = st.p[0].x;
                m_limit = ticksOf(20.0 * 60.0);  // 20 minutes of game time
                if (auto const* index = m_obs.index())
                    for (auto const& it : index->items)
                        if (it.object) m_byUid[it.object->m_uniqueID] = &it;
                {
                    auto const t = std::time(nullptr);
                    char buf[32] = {};
                    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
                    m_stamp = buf;
                }
                m_attempt = 1;
                m_first = 0;
                m_running = true;
                m_freeRng = ((uint64_t)m_levelId * 0x100000001B3ull) ^ 0xD1B54A32D192ED03ull;
                if (opt.practice)
                    say(fmt::format("Playing {} with the pilot and the old practice rules (comparison only){}; up to {} "
                                    "attempts and {:.0f} seconds.",
                                    m_levelName,
                                    m_critic ? ", the critic picking mistakes" : ", mistakes picked from the pilot's own doubts",
                                    opt.maxAttempts, opt.maxSeconds));
                else
                    say(fmt::format("Playing {} with the network alone, no practice rules: attempt 1 is its best guess, "
                                    "later attempts start over and sample from its own odds. Up to {} attempts and {:.0f} "
                                    "seconds.",
                                    m_levelName, opt.maxAttempts, opt.maxSeconds));
                sim::suspend();  // the first slice runs on the next frame
                return false;
            }

            sim::SnapshotPtr takeSnap() {
                if (m_pool.empty()) return sim::newSnapshot();
                auto s = std::move(m_pool.back());
                m_pool.pop_back();
                return s;
            }

            void saveSnap() {
                Snap s;
                s.s = takeSnap();
                sim::save(*s.s);
                s.h = m_obs.history();
                s.prev = m_prev;
                m_snaps.emplace(m_t, std::move(s));
                if (m_snaps.size() > kMaxSnaps) {
                    // Thin out the oldest (tick 0 stays): every other one more
                    // than a few seconds back.
                    uint32_t const keep = ticksOf(4.0 * kLevels[kLevelCount - 1].windowS);
                    bool drop = false;
                    for (auto it = std::next(m_snaps.begin()); it != m_snaps.end();) {
                        if (it->first + keep >= m_t) break;
                        if (drop) {
                            m_pool.push_back(std::move(it->second.s));
                            it = m_snaps.erase(it);
                        } else {
                            ++it;
                        }
                        drop = !drop;
                    }
                }
            }

            // Steps the line until the attempt ends or the slice is used up.
            bool run(Clock::time_point until) {
                obs::Record rec;
                float x[obs::kObsDim];
                float out[4];
                auto& M = models();
                sim::State st;
                for (int n = 1;; n++) {
                    if (m_t >= m_limit) return finish("time", "It played 20 minutes of game time without dying or finishing.");
                    bool const forcedHere = m_forced.count(m_t) != 0;
                    if ((m_t % kSnapEvery == 0 || (forcedHere && (m_t == 0 || !m_forced.count(m_t - 1)))) &&
                        !m_snaps.count(m_t))
                        saveSnap();
                    auto const o0 = Clock::now();
                    m_obs.observe(rec);
                    obs::expand(rec, x);
                    auto const o1 = Clock::now();
                    M.pilot.net.forward(x, out);
                    float const logit = out[M.hold];
                    float adv = 0.0f;
                    if (m_critic) {
                        M.critic.net.forward(x, out);
                        adv = out[M.aliveHold] - out[M.aliveRelease];
                    }
                    auto const o2 = Clock::now();
                    m_obsUs += std::chrono::duration<double, std::micro>(o1 - o0).count();
                    m_netUs += std::chrono::duration<double, std::micro>(o2 - o1).count();
                    bool held = forcedHere ? m_forced[m_t] : net::holds(logit);
                    if (!opt.practice && m_attempt > 1 && !forcedHere) {
                        // The network alone (no practice rules): after its first, greedy
                        // attempt it samples from its own odds.
                        double const z = (double)logit / std::max(0.05, opt.temperature);
                        double const ph = 1.0 / (1.0 + std::exp(-z));
                        double const u = (double)(splitmix(m_freeRng) >> 11) * (1.0 / 9007199254740992.0);
                        held = u < ph;
                    }
                    if (m_varietyOn && !forcedHere && m_t >= m_varFrom) {
                        if (m_lastX > m_varPastX) {
                            m_varietyOn = false;  // past the death: greedy again
                        } else {
                            // Sample the pilot's own P(hold) at the round's temperature;
                            // a choice that differs from greedy is kept as a forced one,
                            // so the line replays exactly.
                            double const ph = 1.0 / (1.0 + std::exp(-(double)logit / (double)m_varTemp));
                            double const u = (double)(splitmix(m_varRng) >> 11) * (1.0 / 9007199254740992.0);
                            bool const pick = u < ph;
                            if (pick != held) m_forced[m_t] = pick;
                            held = pick;
                        }
                    }
                    uint8_t const presses = held && !m_prev ? 1 : 0;
                    bool const dead = sim::step(presses, held);
                    sim::read(st);
                    bool const act = st.p[0].button;
                    m_obs.after(act);
                    m_d.push_back(held ? 1 : 0);
                    m_logit.push_back(logit);
                    m_adv.push_back(adv);
                    if (opt.logPractice) {
                        m_stateRows.insert(m_stateRows.end(), rec.state, rec.state + obs::kStateDim);
                        m_scanRows.insert(m_scanRows.end(), rec.scan, rec.scan + obs::kScanDim);
                        m_tokRows.insert(m_tokRows.end(), rec.tok, rec.tok + obs::kTokDim);
                        m_metaRows.insert(m_metaRows.end(), rec.meta, rec.meta + obs::kMetaDim);
                        m_geoRows.insert(m_geoRows.end(), rec.geo, rec.geo + obs::kGeoDim);
                        m_actRows.push_back(act ? 1 : 0);
                    }
                    m_prev = held;
                    m_t++;
                    m_ticks++;
                    m_lastX = st.p[0].x;
                    if (dead) {
                        if (onDeath(st)) return true;
                    } else if (st.complete) {
                        return onComplete(st);
                    }
                    if ((n & 31) == 0) {
                        if (Clock::now() >= until) return false;
                        if (secondsUsed() >= opt.maxSeconds) {
                            writeLog(false, -1, 0.0f);
                            return finish("seconds", fmt::format("Out of time ({:.0f} seconds) in attempt {}.",
                                                                 opt.maxSeconds, m_attempt));
                        }
                    }
                }
            }

            double secondsUsed() const {
                return std::chrono::duration<double>(Clock::now() - m_t0).count();
            }

            std::string describe(sim::State const& st) const {
                auto const& p = st.p[st.deadP2 ? 1 : 0];
                std::string who = st.deadP2 ? "player 2, " : "";
                if (st.voidDeath) return fmt::format("{}fell out of the level, as {} {}", who, aOrAn(modeName(p.mode)),
                                                     modeName(p.mode));
                if (st.killerId == 0)
                    return fmt::format("{}hit a block's side, the floor or the ceiling, as {} {}", who,
                                       aOrAn(modeName(p.mode)), modeName(p.mode));
                obs::Item const* item = nullptr;
                if (auto it = m_byUid.find(st.killerUid); it != m_byUid.end()) item = it->second;
                std::string const what = objectWord(item, st.killerId);
                float dx = st.killerX - p.x;
                float dy = st.killerY - p.y;
                if (p.upsideDown) dy = -dy;
                char const* side = std::fabs(dy) > std::fabs(dx) ? (dy > 0 ? "from above" : "from below")
                                                                  : (dx >= 0 ? "ahead" : "behind");
                return fmt::format("{}{} {} {}, as {} {}", who, aOrAn(what.c_str()), what, side,
                                   aOrAn(modeName(p.mode)), modeName(p.mode));
            }

            int frameOf(uint32_t t) const { return m_startFrame + 1 + (int)t; }

            void buildCands(Target& tg, int level) {
                Level const& L = kLevels[level];
                tg.level = level;
                tg.cands.clear();
                tg.next = 0;
                uint32_t const W = ticksOf(L.windowS);
                uint32_t const from = std::max(tg.lo, tg.D >= W ? tg.D - W : 0u);
                uint32_t const len = std::max<uint32_t>(L.minLen, L.lenS > 0 ? ticksOf(L.lenS) : 1);
                uint32_t const stride = (uint32_t)std::max(1.0, std::round(L.stride240 * m_tps / 240.0));
                for (int64_t t = tg.D; t >= (int64_t)from; t -= stride) {
                    Cand c;
                    c.t = (uint32_t)t;
                    c.len = len;
                    c.v = tg.d[c.t - tg.lo] == 0;
                    tg.cands.push_back(c);
                }
                // Ranked, most recent first among equals (stable sorts).
                if (m_critic) {
                    // How strongly the critic preferred what it did not do.
                    auto pref = [&](Cand const& c) {
                        float const a = tg.adv[c.t - tg.lo];
                        return c.v ? a : -a;
                    };
                    std::stable_sort(tg.cands.begin(), tg.cands.end(),
                                     [&](Cand const& a, Cand const& b) { return pref(a) > pref(b); });
                } else {
                    // The pilot's least-sure decisions, near its last presses
                    // and releases first.
                    std::vector<uint32_t> edges;
                    for (uint32_t t = tg.lo + 1; t <= tg.D; t++)
                        if (tg.d[t - tg.lo] != tg.d[t - tg.lo - 1]) edges.push_back(t);
                    uint32_t const nearT = ticksOf(0.25);
                    auto key = [&](Cand const& c) {
                        bool close = false;
                        for (uint32_t e : edges)
                            if ((e > c.t ? e - c.t : c.t - e) <= nearT) {
                                close = true;
                                break;
                            }
                        int const unsure = (int)std::floor(std::fabs(tg.logit[c.t - tg.lo]) / 0.5f);
                        return std::pair<int, int>(close ? 0 : 1, unsure);
                    };
                    std::stable_sort(tg.cands.begin(), tg.cands.end(),
                                     [&](Cand const& a, Cand const& b) { return key(a) < key(b); });
                }
            }

            void pushTarget(uint32_t D, float x, std::string const& cause) {
                Target tg;
                tg.D = D;
                tg.x = x;
                tg.percent = percentOf(x);
                tg.attempt = m_attempt;
                tg.cause = cause;
                tg.base = m_forced;
                uint32_t const back = ticksOf(kLevels[kLevelCount - 1].windowS);
                tg.lo = D >= back ? D - back : 0;
                tg.d.assign(m_d.begin() + tg.lo, m_d.begin() + D + 1);
                tg.logit.assign(m_logit.begin() + tg.lo, m_logit.begin() + D + 1);
                tg.adv.assign(m_adv.begin() + tg.lo, m_adv.begin() + D + 1);
                buildCands(tg, 0);
                m_stack.push_back(std::move(tg));
            }

            // A death. True when the job is over.
            bool onDeath(sim::State const& st) {
                uint32_t const D = m_t - 1;
                float const x = st.p[0].x;
                float const pct = percentOf(x);
                std::string const cause = describe(st);
                m_deathsTotal++;
                if (opt.logPractice && !m_metaRows.empty()) m_metaRows[m_metaRows.size() - 1] = 1.0f;
                writeLog(false, (int64_t)D, x);
                if (x > m_bestX) {
                    m_bestX = x;
                    m_bestPercent = pct;
                    m_bestAttempt = m_attempt;
                }
                if (!opt.practice) return freeDeath(st, D, x, pct, cause);
                // What this attempt's change did.
                std::string lead;
                m_varietyOn = false;
                if (m_tryIdx < 0) {
                    pushTarget(D, x, cause);
                } else {
                    Target& tg = m_stack[(size_t)m_tryIdx];
                    if (m_tryVariety)
                        lead = fmt::format("Tried the last {:g} s again with more variety: ", m_varWindowS);
                    if (x > tg.x + kProgressX) {
                        lead += fmt::format("{} got past {:.1f}%. ", m_tryVariety ? "it" : "The change", tg.percent);
                        if (m_tryVariety) m_varietyKept++;
                        m_stack.resize((size_t)m_tryIdx + 1);
                        pushTarget(D, x, cause);
                    } else if (std::fabs(x - tg.x) <= kProgressX) {
                        lead += m_tryVariety ? "the same death again. " : "The same death again. ";
                        tg.fails++;
                    } else {
                        lead += fmt::format("{}orse: it died before {:.1f}%. ", m_tryVariety ? "w" : "W", tg.percent);
                        tg.fails++;
                    }
                }
                // The next mistake to change, or a variety retry of the last seconds.
                std::string turn;
                bool variety = false;
                if (!m_stack.empty()) {
                    Target& tg = m_stack.back();
                    if (tg.varLeft > 0) {
                        variety = true;
                    } else if (tg.fails >= kVarietyAfter * (tg.varRound + 1)) {
                        tg.varRound++;
                        tg.varLeft = kVarietyTries;
                        variety = true;
                    }
                }
                while (!variety && !m_stack.empty()) {
                    Target& tg = m_stack.back();
                    if (tg.next < tg.cands.size()) break;
                    if (tg.level + 1 < kLevelCount) {
                        buildCands(tg, tg.level + 1);
                        turn += fmt::format("No change of that kind got past {:.1f}%: now {}. ", tg.percent,
                                            kLevels[tg.level].what);
                        continue;
                    }
                    turn += fmt::format("Nothing near {:.1f}% got past it: going back to the death before. ", tg.percent);
                    m_stack.pop_back();
                }
                std::string const head = fmt::format("Attempt {} died at {:.1f}% (frame {}): {}.", m_attempt, pct,
                                                     frameOf(D), cause);
                if (m_stack.empty()) {
                    say(head + " " + lead + turn);
                    return finish("stuck", "Every change it knows how to try failed.");
                }
                if (m_attempt >= opt.maxAttempts) {
                    say(head + " " + lead);
                    return finish("attempts", fmt::format("Out of attempts ({}).", opt.maxAttempts));
                }
                if (secondsUsed() >= opt.maxSeconds) {
                    say(head + " " + lead);
                    return finish("seconds", fmt::format("Out of time ({:.0f} seconds).", opt.maxSeconds));
                }
                size_t const idx = m_stack.size() - 1;
                Target& tg = m_stack[idx];
                if (variety) {
                    int const r = std::min(tg.varRound - 1, (int)std::size(kVarietyWindowS) - 1);
                    double const windowS = kVarietyWindowS[r];
                    float const temp =
                        std::min(kVarietyTempMax, kVarietyTemp0 + kVarietyTempStep * (float)(tg.varRound - 1));
                    int const tryNo = kVarietyTries - tg.varLeft + 1;
                    tg.varLeft--;
                    std::string const fix = fmt::format("the last {:g} s again with more variety", windowS);
                    std::string const sentence = fmt::format(
                        "{} {}{}The same death {} times: trying {} (round {}, try {} of {}, temperature {:.1f}; "
                        "death {} of the run).",
                        head, lead, turn, tg.fails, fix, tg.varRound, tryNo, kVarietyTries, temp, idx + 1);
                    say(sentence);
                    auto dj = matjson::Value::object();
                    dj["attempt"] = m_attempt;
                    dj["tick"] = (int64_t)D;
                    dj["frame"] = frameOf(D);
                    dj["x"] = (double)x;
                    dj["percent"] = (double)pct;
                    dj["cause"] = cause;
                    dj["killer_id"] = st.killerId;
                    dj["mistake_tick"] = matjson::Value(nullptr);
                    dj["mistake_was"] = "variety";
                    dj["fix"] = fix;
                    dj["sentence"] = sentence;
                    m_deaths.push_back(dj);
                    if (m_deaths.size() > 200) m_deaths.erase(m_deaths.begin());
                    applyVariety(idx, windowS, temp);
                    m_attempt++;
                    return false;
                }
                Cand const c = tg.cands[tg.next++];
                bool const was = !c.v;
                std::string sure;
                if (m_critic) {
                    float const a = tg.adv[c.t - tg.lo];
                    sure = fmt::format("the critic rated {} {:.2f} better", c.v ? "holding" : "releasing", c.v ? a : -a);
                } else {
                    float const l = tg.logit[c.t - tg.lo];
                    float const p = 1.0f / (1.0f + std::exp(-l));
                    sure = fmt::format("the pilot was {:.0f}% sure", 100.0f * (was ? p : 1.0f - p));
                }
                std::string const mistake = fmt::format("{} at frame {} ({})", was ? "holding" : "releasing",
                                                        frameOf(c.t), sure);
                std::string const fix = c.len == 1
                                            ? fmt::format("{} at frame {} instead", c.v ? "hold" : "release", frameOf(c.t))
                                            : fmt::format("{} for {} ticks from frame {}", c.v ? "hold" : "release", c.len,
                                                          frameOf(c.t));
                std::string const sentence = fmt::format("{} {}{}Mistake: {}. Trying: {} ({} of {}, death {} of the run).",
                                                         head, lead, turn, mistake, fix, tg.next, tg.cands.size(),
                                                         idx + 1);
                say(sentence);
                auto dj = matjson::Value::object();
                dj["attempt"] = m_attempt;
                dj["tick"] = (int64_t)D;
                dj["frame"] = frameOf(D);
                dj["x"] = (double)x;
                dj["percent"] = (double)pct;
                dj["cause"] = cause;
                dj["killer_id"] = st.killerId;
                dj["mistake_tick"] = (int64_t)c.t;
                dj["mistake_frame"] = frameOf(c.t);
                dj["mistake_was"] = was ? "held" : "released";
                dj["fix"] = fix;
                dj["sentence"] = sentence;
                m_deaths.push_back(dj);
                if (m_deaths.size() > 200) m_deaths.erase(m_deaths.begin());

                apply(idx, c);
                m_attempt++;
                return false;
            }

            // A death with the practice rules off: say it, then start over from
            // the level's start (the network alone decides every tick).
            bool freeDeath(sim::State const& st, uint32_t D, float x, float pct, std::string const& cause) {
                std::string const sentence =
                    fmt::format("Attempt {} died at {:.1f}% (frame {}): {}.", m_attempt, pct, frameOf(D), cause);
                say(sentence);
                auto dj = matjson::Value::object();
                dj["attempt"] = m_attempt;
                dj["tick"] = (int64_t)D;
                dj["frame"] = frameOf(D);
                dj["x"] = (double)x;
                dj["percent"] = (double)pct;
                dj["cause"] = cause;
                dj["killer_id"] = st.killerId;
                dj["sentence"] = sentence;
                m_deaths.push_back(dj);
                if (m_deaths.size() > 200) m_deaths.erase(m_deaths.begin());
                if (m_attempt >= opt.maxAttempts)
                    return finish("attempts", fmt::format("Out of attempts ({}).", opt.maxAttempts));
                if (secondsUsed() >= opt.maxSeconds)
                    return finish("seconds", fmt::format("Out of time ({:.0f} seconds).", opt.maxSeconds));
                for (auto it = m_snaps.begin(); it != m_snaps.end();) {
                    if (it->first != 0) {
                        m_pool.push_back(std::move(it->second.s));
                        it = m_snaps.erase(it);
                    } else {
                        ++it;
                    }
                }
                auto const& s0 = m_snaps.at(0);
                sim::restore(*s0.s);
                m_obs.setHistory(s0.h);
                m_prev = s0.prev;
                m_t = 0;
                m_d.clear();
                m_logit.clear();
                m_adv.clear();
                m_stateRows.clear(), m_scanRows.clear(), m_tokRows.clear();
                m_metaRows.clear(), m_geoRows.clear(), m_actRows.clear();
                m_forced.clear();
                m_first = 0;
                m_tryIdx = -1;
                {
                    sim::State s;
                    sim::read(s);
                    m_lastX = s.p[0].x;
                }
                m_attempt++;
                return false;
            }

            // Takes the line back to just before the change and puts it in.
            void apply(size_t idx, Cand const& c) {
                Target const& tg = m_stack[idx];
                std::map<uint32_t, bool> F = tg.base;
                for (uint32_t i = c.t; i < c.t + c.len; i++) F[i] = c.v;
                uint32_t const split = divergence(m_forced, F);
                // Saved states after the lines part belong to the old line.
                for (auto it = m_snaps.upper_bound(split); it != m_snaps.end();) {
                    m_pool.push_back(std::move(it->second.s));
                    it = m_snaps.erase(it);
                }
                uint32_t const want = std::min({split, c.t, m_t});
                auto it = m_snaps.upper_bound(want);
                --it;  // tick 0 is always there
                sim::restore(*it->second.s);
                m_obs.setHistory(it->second.h);
                m_prev = it->second.prev;
                m_t = it->first;
                m_d.resize(m_t);
                m_logit.resize(m_t);
                m_adv.resize(m_t);
                if (opt.logPractice) {
                    m_stateRows.resize((size_t)m_t * obs::kStateDim);
                    m_scanRows.resize((size_t)m_t * obs::kScanDim);
                    m_tokRows.resize((size_t)m_t * obs::kTokDim);
                    m_metaRows.resize((size_t)m_t * obs::kMetaDim);
                    m_geoRows.resize((size_t)m_t * obs::kGeoDim);
                    m_actRows.resize(m_t);
                }
                m_forced = std::move(F);
                m_tryIdx = (int)idx;
                m_try = c;
                m_tryVariety = false;
                m_varietyOn = false;
                m_first = m_t;
                m_branchAttempt = tg.attempt;
            }

            // Back to the snapshot before the last windowS seconds of the target's
            // line; from there the pilot samples its own choices until it is a
            // block past the death (run()).
            void applyVariety(size_t idx, double windowS, float temp) {
                Target const& tg = m_stack[idx];
                uint32_t const W = ticksOf(windowS);
                uint32_t const from = tg.D >= W ? tg.D - W : 0;
                std::map<uint32_t, bool> F = tg.base;
                uint32_t const split = divergence(m_forced, F);
                for (auto it = m_snaps.upper_bound(split); it != m_snaps.end();) {
                    m_pool.push_back(std::move(it->second.s));
                    it = m_snaps.erase(it);
                }
                uint32_t const want = std::min({split, from, m_t});
                auto it = m_snaps.upper_bound(want);
                --it;  // tick 0 is always there
                sim::restore(*it->second.s);
                m_obs.setHistory(it->second.h);
                m_prev = it->second.prev;
                m_t = it->first;
                m_d.resize(m_t);
                m_logit.resize(m_t);
                m_adv.resize(m_t);
                if (opt.logPractice) {
                    m_stateRows.resize((size_t)m_t * obs::kStateDim);
                    m_scanRows.resize((size_t)m_t * obs::kScanDim);
                    m_tokRows.resize((size_t)m_t * obs::kTokDim);
                    m_metaRows.resize((size_t)m_t * obs::kMetaDim);
                    m_geoRows.resize((size_t)m_t * obs::kGeoDim);
                    m_actRows.resize(m_t);
                }
                {
                    sim::State st;
                    sim::read(st);
                    m_lastX = st.p[0].x;  // where the restored line stands
                }
                m_forced = std::move(F);
                m_tryIdx = (int)idx;
                m_try = Cand{from, W, false};
                m_tryVariety = true;
                m_varWindowS = windowS;
                m_varietyOn = true;
                m_varFrom = from;
                m_varPastX = tg.x + kProgressX;
                m_varTemp = temp;
                // Seeded from the level and the attempt: the same run samples the same.
                m_varRng = ((uint64_t)m_levelId * 0x100000001B3ull) ^ ((uint64_t)(m_attempt + 1) * 0xD1B54A32D192ED03ull);
                m_varietyTries++;
                m_first = m_t;
                m_branchAttempt = tg.attempt;
            }

            std::string attemptName(int attempt) const {
                return fmt::format("{}_{:05d}", m_stamp, attempt);
            }

            // The attempt's practice log (AI contract v2 section 6): rows
            // from the tick it began at to its end.
            void writeLog(bool completed, int64_t diedTick, float diedX) {
                if (!opt.logPractice || m_levelId <= 0) return;
                uint32_t const T = m_t;
                if (T <= m_first) return;
                size_t const n = T - m_first;
                auto const dir = aiRoot() / "practice" / std::to_string(m_levelId);
                std::error_code ec;
                fs::create_directories(dir, ec);
                auto const base = utf8(dir / attemptName(m_attempt));
                auto pathOf = [&](char const* ext) { return u8path(base + ext); };
                std::vector<float> p(n);
                std::vector<int32_t> alive(n);
                for (size_t r = 0; r < n; r++) {
                    uint32_t const t = m_first + (uint32_t)r;
                    p[r] = (float)(1.0 / (1.0 + std::exp(-(double)m_logit[t])));
                    int64_t a;
                    if (completed) a = m_H;
                    else if (diedTick >= 0) a = diedTick - (int64_t)t;
                    else a = (int64_t)T - (int64_t)t;
                    alive[r] = (int32_t)std::clamp<int64_t>(a, 0, m_H);
                }
                bool ok = writeArray(pathOf(".state.f32"), &m_stateRows[(size_t)m_first * obs::kStateDim], n * obs::kStateDim) &&
                          writeArray(pathOf(".scan.u8"), &m_scanRows[(size_t)m_first * obs::kScanDim], n * obs::kScanDim) &&
                          writeArray(pathOf(".tok.u8"), &m_tokRows[(size_t)m_first * obs::kTokDim], n * obs::kTokDim) &&
                          writeArray(pathOf(".act.u8"), &m_actRows[m_first], n) &&
                          writeArray(pathOf(".meta.f32"), &m_metaRows[(size_t)m_first * obs::kMetaDim], n * obs::kMetaDim) &&
                          writeArray(pathOf(".geo.f32"), &m_geoRows[(size_t)m_first * obs::kGeoDim], n * obs::kGeoDim) &&
                          writeArray(pathOf(".p.f32"), p.data(), n) && writeArray(pathOf(".alive.i32"), alive.data(), n);
                auto h = matjson::Value::object();
                h["format"] = "gbai-run";
                h["version"] = 2;
                h["source"] = "ai";
                h["level_id"] = m_levelId;
                h["level_name"] = m_levelName;
                h["tps"] = m_tps;
                h["ticks"] = (int64_t)n;
                h["first_tick"] = (int64_t)m_first;
                h["layout_hash"] = obs::layoutHash();
                h["completed"] = completed;
                // Ticks from the level's start (contract section 1), like first_tick.
                h["died_tick"] = diedTick >= 0 ? matjson::Value(diedTick) : matjson::Value(nullptr);
                h["died_x"] = diedTick >= 0 ? matjson::Value((double)diedX) : matjson::Value(nullptr);
                h["level_length_x"] = (double)m_levelLength;
                h["attempt"] = attemptName(m_attempt);
                h["start_frame"] = m_startFrame;
                if (m_tryIdx >= 0) {
                    auto b = matjson::Value::object();
                    b["attempt"] = attemptName(m_branchAttempt);
                    b["tick"] = (int64_t)m_try.t;
                    h["branch_of"] = b;
                }
                auto forced = matjson::Value::array();
                for (auto const& [t, v] : m_forced) {
                    if (t >= T) break;
                    auto f = matjson::Value::array();
                    f.push((int64_t)t);
                    f.push(v ? 1 : 0);
                    forced.push(f);
                }
                h["forced"] = forced;
                h["pilot_file"] = models().pilot.path;
                h["critic"] = m_critic;
                h["build"] = GB_BUILD_LABEL;
                {
                    std::ofstream f(pathOf(".json"), std::ios::binary | std::ios::trunc);
                    f << h.dump(2);
                    ok = ok && (bool)f;
                }
                if (ok) m_logsWritten++;
                else if (m_logError.empty()) m_logError = "could not write the practice log in " + utf8(dir);
            }

            bool onComplete(sim::State const&) {
                uint32_t const T = m_t;
                writeLog(true, -1, 0.0f);
                m_bestPercent = 100.0f;
                m_bestX = m_levelLength;
                m_bestAttempt = m_attempt;
                say(fmt::format("Attempt {} finished the level ({} ticks).", m_attempt, T));
                // The inputs, one a tick, for the macro and the judge.
                std::vector<TickInput> inputs(T);
                bool prev = false;
                for (uint32_t t = 0; t < T; t++) {
                    bool const h = m_d[t] != 0;
                    inputs[t].held = h;
                    inputs[t].presses = h && !prev ? 1 : 0;
                    prev = h;
                }
                sim::end();
                auto out = baseResult("completed");
                // Saved as a GucciBot macro (frame labels: tick t is frame F + 1 + t).
                {
                    auto* gb = GucciEngine::get();
                    convert::Macro m;
                    m.tps = m_tps;
                    m.levelName = m_levelName;
                    m.levelId = m_levelId;
                    m.name = opt.saveMacro.empty() ? "AI - " + m_levelName : opt.saveMacro;
                    m.bot = "GucciBot AI pilot";
                    bool down = false;
                    for (uint32_t t = 0; t < T; t++) {
                        bool const h = m_d[t] != 0;
                        if (h == down) continue;
                        convert::Input in;
                        in.frame = frameOf(t);
                        in.down = h;
                        m.inputs.push_back(in);
                        down = h;
                    }
                    if (down) {
                        convert::Input in;
                        in.frame = frameOf(T);
                        in.down = false;
                        m.inputs.push_back(in);
                    }
                    auto const bytes = convert::toGucciBot(m);
                    auto const path = gb->getReplayDir() / u8path(fileSafe(m.name) + ".gbr6");
                    std::error_code ec;
                    fs::create_directories(path.parent_path(), ec);
                    std::ofstream f(path, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size());
                    if (f) {
                        out["macro_file"] = utf8(path);
                        out["macro_inputs"] = (int64_t)m.inputs.size();
                    } else {
                        out["macro_error"] = "could not write " + utf8(path);
                    }
                }
                // Played in the real game from the same start.
                if (opt.judge) {
                    absense::judge::Options jo;
                    jo.maxMs = 8000.0;
                    std::vector<TraceSample> trace;
                    // The real game notes the end a little after the copies do (2026-11-03:
                    // Clutterfunk survived all T ticks with no death and no level end), so the
                    // judge gets a released tail of up to two seconds to reach it.
                    std::vector<TickInput> judged = inputs;
                    judged.resize(T + (uint32_t)std::max(60, 2 * (int)m_tps));
                    auto const r = absense::judge::run(judged, (int)judged.size(), &trace, jo);
                    auto j = matjson::Value::object();
                    j["ran"] = r.ran;
                    if (!r.ran) {
                        j["error"] = r.error;
                    } else {
                        j["completed"] = r.complete;
                        j["died"] = r.died;
                        j["survived_ticks"] = r.survived;
                        float const x = trace.empty() ? 0.0f : trace.back().x;
                        j["died_x"] = r.died ? matjson::Value((double)x) : matjson::Value(nullptr);
                        j["died_percent"] = r.died ? matjson::Value((double)percentOf(x)) : matjson::Value(nullptr);
                        if (r.died) j["killer_id"] = r.killerId;
                        j["stopped_by_clock"] = r.stoppedByClock;
                    }
                    out["judge"] = j;
                    if (r.ran && r.complete) say("The real game played it to the end too.");
                    else if (r.ran && r.died)
                        say(fmt::format("In the real game it died at {:.1f}%: the simulation and the game disagree there.",
                                        percentOf(trace.empty() ? 0.0f : trace.back().x)));
                    else say("The real game could not check it: " + (r.ran ? std::string("it ran out of time") : r.error));
                }
                m_start.release();
                result = out;
                progress = "done: completed";
                s_live.bestPercent = 100.0f;
                s_live.attempts = m_attempt;
                return true;
            }

            matjson::Value baseResult(char const* stopped) const {
                auto out = matjson::Value::object();
                out["completed"] = std::string(stopped) == "completed";
                out["stopped"] = stopped;
                out["best_percent"] = (double)m_bestPercent;
                out["best_x"] = (double)m_bestX;
                out["best_attempt"] = m_bestAttempt;
                out["attempts"] = m_attempt;
                out["ticks_simulated"] = (int64_t)m_ticks;
                out["deaths_total"] = m_deathsTotal;
                auto deaths = matjson::Value::array();
                for (auto const& d : m_deaths) deaths.push(d);
                out["deaths"] = deaths;
                out["seconds"] = m_t0 == Clock::time_point{} ? 0.0 : secondsUsed();
                out["level_id"] = m_levelId;
                out["level_name"] = m_levelName;
                out["level_length_x"] = (double)m_levelLength;
                out["tps"] = m_tps;
                out["start_frame"] = m_startFrame;
                out["critic"] = m_critic;
                out["practice"] = opt.practice;
                out["temperature"] = opt.temperature;
                out["pilot"] = models().pilot.path;
                out["pilot_layout_hash"] = models().pilot.net.layoutHash();
                out["changes_kept"] = (int64_t)m_forced.size();
                out["variety_tries"] = m_varietyTries;
                out["variety_kept"] = m_varietyKept;
                out["snapshots"] = (int64_t)m_snaps.size();
                out["observe_us_avg"] = m_ticks ? m_obsUs / (double)m_ticks : 0.0;
                out["networks_us_avg"] = m_ticks ? m_netUs / (double)m_ticks : 0.0;
                if (opt.logPractice) {
                    out["practice_logs"] = m_logsWritten;
                    if (!m_logError.empty()) out["practice_log_error"] = m_logError;
                }
                return out;
            }

            void partialResult(char const* stopped) {
                if (!result.isNull()) return;
                result = baseResult(stopped);
            }

            bool finish(char const* stopped, std::string const& why) {
                sim::end();
                m_start.release();
                auto out = baseResult(stopped);
                out["why"] = why;
                result = out;
                progress = fmt::format("done: {}", stopped);
                say(fmt::format("{} Best {:.1f}% (attempt {}), {} attempts.", why, m_bestPercent, m_bestAttempt,
                                m_attempt));
                s_live.bestPercent = m_bestPercent;
                s_live.attempts = m_attempt;
                return true;
            }
        };

    }  // namespace

    namespace {
        fs::path g_weightsDir;  // setWeightsDir's, for this launch

        fs::path underRoot(fs::path p) {
            return p.is_absolute() ? p : aiRoot() / p;
        }
    }  // namespace

    fs::path weightsDir() {
        if (!g_weightsDir.empty()) return g_weightsDir;
        static fs::path const s_env = [] {
            char const* v = std::getenv("GUCCIBOT_AI_WEIGHTS");
            return v && *v ? underRoot(fs::path(std::u8string(v, v + std::strlen(v)))) : fs::path();
        }();
        return s_env.empty() ? aiRoot() / "weights" : s_env;
    }

    void setWeightsDir(std::string const& dir) {
        g_weightsDir = dir.empty() ? fs::path() : underRoot(fs::path(std::u8string(dir.begin(), dir.end())));
    }

    matjson::Value reload() {
        auto& M = models();
        M.tried = true;
        auto const obsHash = obs::layoutHash();
        loadOne(M.pilot, obsHash);
        loadOne(M.critic, obsHash);
        loadOne(M.reader, tape::layoutHash());
        M.hold = M.aliveHold = M.aliveRelease = -1;
        if (M.pilot.net.loaded()) {
            auto& n = M.pilot.net;
            M.hold = n.output("hold_logit");
            if (M.hold < 0 && n.outputs() == 1) M.hold = 0;
            if (n.kind() != net::Kind::Mlp) refuse(M.pilot, "pilot.gbnn is not a dense network (mlp)");
            else if (n.inputs() != (uint32_t)obs::kObsDim)
                refuse(M.pilot, fmt::format("pilot.gbnn takes {} inputs; the pilot sees {}", n.inputs(), obs::kObsDim));
            else if (M.hold < 0) refuse(M.pilot, "pilot.gbnn has no hold_logit output");
            else if (n.outputs() > 4) refuse(M.pilot, "pilot.gbnn has more outputs than a pilot has");
        }
        if (M.critic.net.loaded()) {
            auto& n = M.critic.net;
            M.aliveHold = n.output("alive_hold");
            M.aliveRelease = n.output("alive_release");
            if (n.kind() != net::Kind::Mlp) refuse(M.critic, "critic.gbnn is not a dense network (mlp)");
            else if (n.inputs() != (uint32_t)obs::kObsDim)
                refuse(M.critic, fmt::format("critic.gbnn takes {} inputs; the pilot sees {}", n.inputs(), obs::kObsDim));
            else if (M.aliveHold < 0 || M.aliveRelease < 0)
                refuse(M.critic, "critic.gbnn needs the outputs alive_hold and alive_release");
            else if (n.outputs() > 4) refuse(M.critic, "critic.gbnn has more outputs than a critic has");
        }
        if (M.reader.net.loaded()) {
            auto& n = M.reader.net;
            if (n.kind() != net::Kind::Conv1d) refuse(M.reader, "reader.gbnn is not a conv1d stack");
            else if (n.inputs() != (uint32_t)tape::kChannels)
                refuse(M.reader, fmt::format("reader.gbnn takes {} channels; the tape has {}", n.inputs(), tape::kChannels));
        }
        auto out = matjson::Value::object();
        auto list = matjson::Value::array();
        list.push(modelJson(M.pilot));
        list.push(modelJson(M.critic));
        list.push(modelJson(M.reader));
        out["models"] = list;
        out["pilot_loaded"] = M.pilot.net.loaded();
        out["critic_loaded"] = M.critic.net.loaded();
        out["reader_loaded"] = M.reader.net.loaded();
        out["reader_used"] = false;  // the v2 record has no route inputs yet
        out["obs_layout_hash"] = obsHash;
        out["tape_layout_hash"] = tape::layoutHash();
        out["weights_dir"] = utf8(weightsDir());
        out["summary"] = weightsLine();
        for (Model const* m : {&M.pilot, &M.critic, &M.reader})
            log::info("[AI] {}: {}", m->name, m->net.loaded() ? "loaded from " + m->path : m->error);
        return out;
    }

    net::Net* pilotNet(int& holdIndex, std::string& path) {
        if (!ready()) return nullptr;
        auto& M = models();
        holdIndex = M.hold;
        path = M.pilot.path;
        return &M.pilot.net;
    }

    bool ready() {
        if (!models().tried) reload();
        return models().pilot.net.loaded();
    }

    std::string weightsLine() {
        auto& M = models();
        if (!M.tried) reload();
        if (!M.pilot.net.loaded()) {
            if (M.pilot.path.empty())
                return fmt::format("No trained AI yet: there is no pilot.gbnn in {}. The trainer (tools/ai) puts it "
                                   "there; then press Reload.",
                                   utf8(weightsDir()));
            return fmt::format("The pilot in {} can't be used: {}", M.pilot.path, M.pilot.error);
        }
        std::string s = fmt::format("Pilot loaded ({}).", M.pilot.path);
        if (M.critic.net.loaded()) s += " Critic loaded.";
        else s += " No critic: mistakes are picked from the pilot's own doubts.";
        return s;
    }

    std::unique_ptr<job::Job> makePlay(PlayOptions const& options, std::string& why) {
#if !GB_NATIVE_ENGINE
        why = "The AI's simulation is Windows-only for now: its model of the level reads GD's Windows memory layout.";
        return nullptr;
#else
        if (!ready()) {
            why = weightsLine();
            return nullptr;
        }
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 || !pl->m_level) {
            why = "open a level first";
            return nullptr;
        }
        if (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode) {
            why = "the AI pilot plays classic levels, not platformer ones";
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
        auto job = std::make_unique<Play>();
        job->opt = options;
        job->opt.maxAttempts = std::clamp(job->opt.maxAttempts, 1, 1000000);
        job->opt.maxSeconds = std::clamp(job->opt.maxSeconds, 1.0, 24.0 * 3600.0);
        s_live.lines.clear();
        s_live.bestPercent = 0.0f;
        s_live.attempts = 0;
        return job;
#endif
    }

    Live const& live() {
        return s_live;
    }

    void setLiveJob(int id) {
        s_live.job = id;
    }

    bool engineAi() {
        return Mod::get()->getSavedValue<bool>("pf_engine_ai", true);
    }

    void setEngineAi(bool on) {
        Mod::get()->setSavedValue("pf_engine_ai", on);
    }

} // namespace gucci::ai::pilot
