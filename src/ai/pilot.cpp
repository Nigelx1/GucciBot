#include "ai/pilot.hpp"
#include "core/geode_compat.hpp" // gucci::stdPath

#include "absense/glue.hpp"
#include "absense/judge.hpp"
#include "ai/net.hpp"
#include "ai/observe.hpp"
#include "ai/record.hpp"
#include "ai/sim.hpp"
#include "ai/start.hpp"
#include "ai/tape.hpp"
#include "ai/train.hpp"
#include "core/GucciBot.hpp"
#include "core/platform.hpp"
#include "tools/macro_convert.hpp"
#include "ui/ui.hpp"  // the theme's macro extension, for the furthest run's macro

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
            bool builtIn = false;  // the copy that ships with the mod, not the user's own
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

        // The user's own first (weightsDir()), else the copy that ships with the
        // mod. GucciBot (2026-11-19): the pilot ships from 2.0 on, so the AI
        // engine's Start works without training anything (Nigel: "whats the
        // point of even having the interface if its greyed out"). Its source is
        // resources/ai/pilot.gbnn; Geode puts every resource file straight in
        // the mod's resources folder, so the package holds it at
        // resources/nigelx1.guccibot/pilot.gbnn and it is found in `res`.
        fs::path findWeights(char const* name, bool& builtIn) {
            std::error_code ec;
            builtIn = false;
            std::string const file = std::string(name) + ".gbnn";
            if (auto const own = weightsDir() / file; fs::is_regular_file(own, ec)) return own;
            fs::path const res = gucci::stdPath(Mod::get()->getResourcesDir());
            for (fs::path const& dir : {res / "ai", res}) {
                auto const p = dir / file;
                if (fs::is_regular_file(p, ec)) {
                    builtIn = true;
                    return p;
                }
            }
            return {};
        }

        void loadOne(Model& m, std::string const& hash) {
            m.net = net::Net();
            m.path.clear();
            m.error.clear();
            auto const p = findWeights(m.name, m.builtIn);
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
            o["built_in"] = !m.path.empty() && m.builtIn;
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

        // Self-training (2026-11-19): a round learns from the newest kKeepTicks
        // ticks of a sampled run (2 minutes at 240 TPS), leaving out a death's
        // last quarter second (the mistake is in there); a new round when the
        // best is a block further than the last round's, while learning has
        // taken at most kLearnShare of the playing time. A round makes passes
        // until the network makes every decision of the run (then a cold play
        // repeats it), at most kRoundPasses, a finished run's kFinalPasses.
        // In game on 11-19 a trained network swapped in blindly played worse
        // (one pass at 3e-4 on its own greedy run flipped 6 of 559 decisions:
        // the cold play fell from 3.48% to 2.86%), and 6 passes at 1e-4 fit a
        // sampled run badly (197 of 741 decisions still differed). So a round
        // is kept only when the next attempt, the trained network's best guess,
        // gets further (judgeCheck), which lets it learn hard: 12 passes at
        // 3e-4 still left 282 of a sampled run's 921 decisions different (run
        // 3), and learning only the 1.5 s before the best guess's death, its
        // own decisions before that, didn't do (run 4: 161 differ, and its best
        // guess went the other way early on), so a round fits the whole run at
        // 1e-3 until every decision is the run's, at most 60 passes.
        constexpr size_t kKeepTicks = 30000;
        constexpr double kSkipBeforeDeathS = 0.25;
        constexpr double kLearnShare = 0.5;
        constexpr size_t kLearnBatch = 64;
        constexpr int kRoundPasses = 60;
        constexpr int kFinalPasses = 60;
        constexpr float kLearnRate = 1e-3f;

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
                if (learning()) {
                    // A self-training round, between attempts (the session stays
                    // suspended) or after the run ended (the session is over).
                    learnSlice(until);
                    if (learning()) {
                        progress = fmt::format("self-training on its best run ({:.1f}%): pass {}, {} of {} ticks",
                                               m_learn.setPercent, m_learn.passes + 1, m_learn.at, m_learn.setY.size());
                        return false;
                    }
                    if (m_ended) {
                        if (result.isObject()) result["self_training"] = learnJson();
                        return true;
                    }
                    return false;
                }
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
                    adoptLive();
                    partialResult("cancelled");
                    if (std::string const saved = saveFurthest(result); !saved.empty()) say(saved.substr(1));
                    // As finish() does: the page's bar shows the best the line names.
                    s_live.bestPercent = m_bestPercent;
                    s_live.attempts = m_attempt;
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
                if (over && learning()) {
                    m_ended = true;  // learn from the last run first (result is set)
                    return false;
                }
                return over;
            }

            void cancel() override {
                if (sim::open()) sim::end();
                m_start.release();
                adoptLive();
                partialResult("cancelled");
                std::string const saved = saveFurthest(result);
                say(fmt::format("Stopped after {} attempts; best {:.1f}%.{}", m_attempt, m_bestPercent, saved));
                // As finish() does: the page's bar shows the best this line names
                // (adoptLive can make the attempt still playing the best one).
                s_live.bestPercent = m_bestPercent;
                s_live.attempts = m_attempt;
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
            // The best attempt's line (one decision a tick from the level's
            // start), kept when it ends, for the furthest macro.
            std::vector<uint8_t> m_bestD;
            bool m_furthestSaved = false;  // once a run
            bool m_completed = false;      // it beat the level: that run is the macro, not a furthest one
            std::vector<matjson::Value> m_deaths;  // the newest 200
            double m_obsUs = 0.0;
            double m_netUs = 0.0;
            int m_logsWritten = 0;
            std::string m_logError;

            // The pilot this run plays with: a copy of the loaded one, or this
            // level's own trained network (self-training, 2026-11-19).
            net::Net m_pilot;
            int m_hold = -1;
            std::string m_pilotFrom;
            bool m_levelNet = false;
            // The line's newest records (what the pilot saw): m_recs[k] is tick m_recFrom + k.
            std::vector<obs::Record> m_recs;
            uint32_t m_recFrom = 0;
            bool m_ended = false;  // the run is over; a last self-training round is going
            // Self-training keeps what gets further, as practice does: after a round
            // the next attempt plays the trained network's best guess (no sampling),
            // and it is kept only if that gets further than the kept network's did.
            bool m_checking = false;
            float m_acceptedX = -1.0f;  // how far the kept network's best guess got

            enum class Phase { Idle, Train, Eval };
            struct Learn {
                bool on = false;
                bool swap = false;       // later attempts play the trained network (the network alone)
                net::Net net;            // the network being trained
                net::Net backup;         // from before the round, if the round goes wrong
                train::Trainer tr;
                Phase phase = Phase::Idle;
                bool finished = false;   // the round of a finished run
                std::vector<obs::Record> setX;  // the run it learns from
                std::vector<float> setY;        // what that run did: held 1, released 0
                std::vector<uint32_t> order;
                std::vector<float> rows, targets;
                uint64_t rng = 0;
                size_t at = 0;           // the next row of this pass or check
                int passes = 0;          // passes this round
                double lossSum = 0.0;
                size_t wrongSum = 0;
                double firstLoss = 0.0;  // the round's first pass, before each minibatch's step
                double lastLoss = 0.0;   // the check after the round's last pass
                size_t lastWrong = 0;    // decisions of the run the network still makes differently
                size_t lastTicks = 0;
                float setPercent = 0.0f;
                uint32_t setFrom = 0;    // the run's first tick it learns from (0: from the level's start)
                float trainedX = -1.0f;  // the best the last round learned from
                int rounds = 0;
                int kept = 0;            // rounds whose network was kept
                uint64_t steps = 0;
                double seconds = 0.0;    // working time spent learning (not counted in maxSeconds)
                std::string baseFp;      // the loaded pilot's fingerprint
                std::string file, error;
            } m_learn;

            bool learning() const { return m_learn.phase != Phase::Idle; }

            static train::Options trainOptions() {
                train::Options o;
                o.lr = kLearnRate;
                return o;
            }

            // The check attempt after a round ended at x: keep the trained network
            // (and save it for the level) if its best guess got further, else go
            // back to the network from before.
            void judgeCheck(float x) {
                auto& L = m_learn;
                m_checking = false;
                if (x > m_acceptedX) {
                    float const before = percentOf(m_acceptedX);
                    m_acceptedX = x;
                    L.kept++;
                    saveLevelNet();
                    say(fmt::format("Its best guess with the trained network got to {:.1f}% (before: {:.1f}%), so it "
                                    "keeps the trained network{}.",
                                    percentOf(x), before,
                                    L.file.empty() ? " (not saved: " + L.error + ")" : " and saved it for this level"));
                } else {
                    L.net = L.backup;
                    m_pilot = L.net;
                    std::string w;
                    L.tr.begin(&L.net, m_hold, trainOptions(), w);  // Adam starts over too
                    say(fmt::format("Its best guess with the trained network got to {:.1f}%, not past {:.1f}%, so it "
                                    "goes back to the network from before.",
                                    percentOf(x), percentOf(m_acceptedX)));
                }
            }

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
                m_pilot = M.pilot.net;
                m_hold = M.hold;
                m_pilotFrom = M.pilot.path;
                m_learn.baseFp = train::fingerprint(train::save(M.pilot.net));
                std::string levelNetLine;
                if (opt.levelNet && m_levelId > 0) levelNetLine = useLevelNet();
                if (opt.selfTrain && opt.practice) {
                    m_learn.error = "it's off while Practice is on (its line replays the network's own "
                                    "decisions, so the network can't change mid-run)";
                } else if (opt.selfTrain) {
                    m_learn.net = m_pilot;
                    std::string w;
                    m_learn.on = m_learn.tr.begin(&m_learn.net, m_hold, trainOptions(), w);
                    m_learn.swap = true;
                    if (!m_learn.on) m_learn.error = "it can't train this pilot: " + w;
                    if (m_learn.on) m_recs.reserve(kKeepTicks + 4096);
                }
                m_attempt = 1;
                m_first = 0;
                m_running = true;
                m_freeRng = ((uint64_t)m_levelId * 0x100000001B3ull) ^ 0xD1B54A32D192ED03ull;
                if (opt.practice)
                    say(fmt::format("Playing {} with the AI and Practice{}; up to {} attempts and {:.0f} seconds.",
                                    m_levelName,
                                    m_critic ? ", the critic picking mistakes" : ", mistakes picked from the pilot's own doubts",
                                    opt.maxAttempts, opt.maxSeconds));
                else
                    say(fmt::format("Playing {} with the AI alone, no Practice: attempt 1 is its best guess, "
                                    "later attempts start over and sample from its own odds. Up to {} attempts and {:.0f} "
                                    "seconds.",
                                    m_levelName, opt.maxAttempts, opt.maxSeconds));
                if (!levelNetLine.empty()) say(levelNetLine);
                if (m_learn.on)
                    say("Self-training is on: each time it gets further, it trains its network on that run, then plays "
                        "one attempt with its best guess, and keeps the trained network (for this level only) when "
                        "that gets further than before (slow; it learns one level at a time).");
                else if (!m_learn.error.empty())
                    say("Self-training: " + m_learn.error + ".");
                sim::suspend();  // the first slice runs on the next frame
                return false;
            }

            // This level's trained network, when an earlier run saved one from
            // the same pilot. The line to say about it (empty: none).
            std::string useLevelNet() {
                auto const p = levelNetPath(m_levelId);
                std::error_code ec;
                if (!fs::is_regular_file(p, ec)) return {};
                std::string base;
                {
                    auto side = p;
                    side.replace_extension(".json");
                    std::ifstream f(side, std::ios::binary);
                    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    auto parsed = matjson::parse(text);
                    if (parsed) {
                        auto const h = parsed.unwrap();
                        base = h["base_fingerprint"].asString().unwrapOr("");
                    }
                }
                if (base != m_learn.baseFp)
                    return "This level's trained network was trained from another pilot, so it plays the loaded "
                           "pilot instead (self-training replaces that file).";
                std::ifstream f(p, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                net::Net n;
                std::string why;
                if (!n.load(bytes.data(), bytes.size(), obs::layoutHash(), why))
                    return "This level's trained network can't be used (" + why + "), so it plays the loaded pilot.";
                int const hold = n.output("hold_logit");
                if (n.kind() != net::Kind::Mlp || n.inputs() != (uint32_t)obs::kObsDim || hold < 0 || n.outputs() > 4)
                    return "This level's trained network isn't a pilot, so it plays the loaded pilot.";
                m_pilot = std::move(n);
                m_hold = hold;
                m_pilotFrom = utf8(p);
                m_levelNet = true;
                return "It plays the network it trained on this level before (" + m_pilotFrom + ").";
            }

            // ------------------------------------------------ self-training (2026-11-19)

            void keepRec(obs::Record const& rec) {
                m_recs.push_back(rec);
                if (m_recs.size() >= kKeepTicks + 4096) {
                    size_t const drop = m_recs.size() - kKeepTicks;
                    m_recs.erase(m_recs.begin(), m_recs.begin() + (std::ptrdiff_t)drop);
                    m_recFrom += (uint32_t)drop;
                }
            }

            // After the line goes back to tick m_t.
            void trimRecs() {
                if (!m_learn.on) return;
                if (m_t >= m_recFrom) {
                    size_t const keep = m_t - m_recFrom;
                    if (m_recs.size() > keep) m_recs.resize(keep);
                } else {
                    m_recs.clear();
                    m_recFrom = m_t;
                }
            }

            // A round on the run that ended at tick `end` (exclusive) at x: a new
            // best far enough past the last round's, or a finished run.
            void learnFrom(uint32_t end, float x, bool finished) {
                auto& L = m_learn;
                if (!L.on || learning()) return;
                if (!finished) {
                    if (L.trainedX >= 0.0f && x < L.trainedX + kProgressX) return;  // a block further
                    if (L.rounds > 0 && L.seconds > kLearnShare * secondsUsed()) return;  // learning's share used
                }
                uint32_t const skip = finished ? 0 : ticksOf(kSkipBeforeDeathS);
                if (end <= m_recFrom + skip) return;
                uint32_t const b = end - skip;
                uint32_t const a = std::max<uint32_t>(m_recFrom, b > (uint32_t)kKeepTicks ? b - (uint32_t)kKeepTicks : 0u);
                if (b < a + 30 || b - m_recFrom > m_recs.size() || b > m_d.size()) return;  // too little (or nothing kept)
                size_t const n = b - a;
                L.setX.assign(m_recs.begin() + (a - m_recFrom), m_recs.begin() + (b - m_recFrom));
                L.setY.resize(n);
                for (size_t k = 0; k < n; k++) L.setY[k] = m_d[a + k] ? 1.0f : 0.0f;
                L.order.resize(n);
                L.finished = finished;
                L.setPercent = finished ? 100.0f : percentOf(x);
                L.setFrom = a;
                L.trainedX = x;
                L.lastTicks = n;
                L.backup = L.net;
                L.passes = 0;
                L.firstLoss = L.lastLoss = 0.0;
                L.lastWrong = 0;
                // Seeded from the level and the round: the same run learns the same.
                L.rng = ((uint64_t)m_levelId * 0x100000001B3ull) ^ ((uint64_t)(L.rounds + 1) * 0x9E3779B97F4A7C15ull);
                startPass();
            }

            void startPass() {
                auto& L = m_learn;
                size_t const n = L.setY.size();
                for (size_t k = 0; k < n; k++) L.order[k] = (uint32_t)k;
                for (size_t k = n; k > 1; k--) std::swap(L.order[k - 1], L.order[(size_t)(splitmix(L.rng) % k)]);
                L.at = 0;
                L.lossSum = 0.0;
                L.wrongSum = 0;
                L.phase = Phase::Train;
            }

            // Minibatches until `until`: a pass in shuffled order, then a check
            // of every tick in order (the loss after, and the decisions it still
            // makes differently).
            void learnSlice(Clock::time_point until) {
                auto& L = m_learn;
                auto const t0 = Clock::now();
                size_t const n = L.setY.size();
                L.rows.resize(kLearnBatch * obs::kObsDim);
                L.targets.resize(kLearnBatch);
                while (learning()) {
                    size_t const cnt = std::min(kLearnBatch, n - L.at);
                    bool const train = L.phase == Phase::Train;
                    for (size_t k = 0; k < cnt; k++) {
                        size_t const r = train ? L.order[L.at + k] : L.at + k;
                        obs::expand(L.setX[r], &L.rows[k * obs::kObsDim]);
                        L.targets[k] = L.setY[r];
                    }
                    size_t wrong = 0;
                    double const l = train ? L.tr.step(L.rows.data(), L.targets.data(), cnt, &wrong)
                                           : L.tr.loss(L.rows.data(), L.targets.data(), cnt, &wrong);
                    if (train) L.steps++;
                    L.lossSum += l * (double)cnt;
                    L.wrongSum += wrong;
                    L.at += cnt;
                    if (L.at >= n) {
                        if (train) {
                            L.passes++;
                            if (L.passes == 1) L.firstLoss = L.lossSum / (double)n;
                            L.phase = Phase::Eval;
                            L.at = 0;
                            L.lossSum = 0.0;
                            L.wrongSum = 0;
                        } else {
                            L.lastLoss = L.lossSum / (double)n;
                            L.lastWrong = L.wrongSum;
                            if (L.lastWrong > 0 && L.passes < (L.finished ? kFinalPasses : kRoundPasses)) startPass();
                            else endRound();
                        }
                    }
                    if (Clock::now() >= until) break;
                }
                L.seconds += std::chrono::duration<double>(Clock::now() - t0).count();
            }

            void endRound() {
                auto& L = m_learn;
                L.phase = Phase::Idle;
                bool finite = std::isfinite(L.lastLoss);
                for (auto const& Ly : L.net.layers()) {
                    for (float v : Ly.w) finite = finite && std::isfinite(v);
                    for (float v : Ly.b) finite = finite && std::isfinite(v);
                }
                if (!finite) {
                    L.net = L.backup;
                    L.on = false;
                    L.error = "self-training went wrong (values that aren't finite), so it stopped and kept the network "
                              "from before";
                    say("Self-training went wrong (values that aren't finite), so it stopped; the network from before "
                        "is kept.");
                    return;
                }
                L.rounds++;
                size_t const n = L.setY.size();
                std::string const what = L.finished ? "the run that finished the level"
                                                    : fmt::format("its best run so far (to {:.1f}%)", L.setPercent);
                std::string after;
                if (!L.finished) {
                    m_pilot = L.net;
                    m_checking = true;
                    after = " Next, one attempt with its best guess decides whether it keeps the trained network.";
                } else if (L.lastWrong == 0 && L.setFrom == 0) {
                    // Every decision from the level's start: its best guess now plays that run.
                    L.kept++;
                    m_acceptedX = m_levelLength;
                    saveLevelNet();
                    after = L.file.empty() ? " It now makes every decision of that run, but it could not be saved: " + L.error + "."
                                           : " It now makes every decision of that run, so it keeps it and saved it for this level.";
                } else {
                    L.net = L.backup;
                    after = L.lastWrong ? " Not every decision yet, so it keeps the network from before."
                                        : " The run is longer than it keeps, so it keeps the network from before.";
                }
                say(fmt::format("Self-training round {}: learned from {}, {} ticks, {} pass{}: loss {:.4f} in the first "
                                "pass, {:.4f} after; {} of the run's {} decisions still differ.{}",
                                L.rounds, what, n, L.passes, L.passes == 1 ? "" : "es", L.firstLoss, L.lastLoss,
                                L.lastWrong, n, after));
                L.setX.clear();
                L.setY.clear();
            }

            void saveLevelNet() {
                auto& L = m_learn;
                if (m_levelId <= 0) {
                    L.error = "the level has no ID (an editor level), so there is nowhere to keep it";
                    return;
                }
                auto const p = levelNetPath(m_levelId);
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                auto put = [&](fs::path const& to, void const* data, size_t size) {
                    auto tmp = to;
                    tmp += ".tmp";
                    bool ok;
                    {
                        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                        f.write(static_cast<char const*>(data), (std::streamsize)size);
                        ok = (bool)f;
                    }
                    std::error_code e;
                    if (ok) fs::rename(tmp, to, e);
                    if (!ok || e) {
                        fs::remove(tmp, e);
                        return false;
                    }
                    return true;
                };
                auto const bytes = train::save(L.net);
                auto j = matjson::Value::object();
                j["format"] = "gbai-level-net";
                j["version"] = 1;
                j["level_id"] = m_levelId;
                j["level_name"] = m_levelName;
                j["base_fingerprint"] = L.baseFp;  // the loaded pilot it was trained from
                j["base_file"] = models().pilot.path;
                j["fingerprint"] = train::fingerprint(bytes);
                j["learned_from_percent"] = (double)L.setPercent;
                j["build"] = GB_BUILD_LABEL;
                std::string const text = j.dump(2);
                auto side = p;
                side.replace_extension(".json");
                if (!put(p, bytes.data(), bytes.size()) || !put(side, text.data(), text.size())) {
                    L.error = "could not write " + utf8(p);
                    L.file.clear();
                    return;
                }
                L.file = utf8(p);
                L.error.clear();
            }

            matjson::Value learnJson() const {
                auto const& L = m_learn;
                auto o = matjson::Value::object();
                o["on"] = L.on;
                o["later_attempts_play_it"] = L.on && L.swap;
                o["rounds"] = L.rounds;
                o["kept"] = L.kept;
                o["kept_best_guess_percent"] = (double)percentOf(std::max(0.0f, m_acceptedX));
                o["steps"] = (int64_t)L.steps;
                o["seconds"] = L.seconds;
                o["last_ticks"] = (int64_t)L.lastTicks;
                o["last_percent"] = (double)L.setPercent;
                o["last_passes"] = L.passes;
                o["last_first_pass_loss"] = L.firstLoss;
                o["last_loss"] = L.lastLoss;
                o["last_decisions_differ"] = (int64_t)L.lastWrong;
                o["file"] = L.file.empty() ? matjson::Value(nullptr) : matjson::Value(L.file);
                if (!L.error.empty()) o["error"] = L.error;
                return o;
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
                    if (m_learn.on) keepRec(rec);
                    auto const o1 = Clock::now();
                    m_pilot.forward(x, out);
                    float const logit = out[m_hold];
                    float adv = 0.0f;
                    if (m_critic) {
                        M.critic.net.forward(x, out);
                        adv = out[M.aliveHold] - out[M.aliveRelease];
                    }
                    auto const o2 = Clock::now();
                    m_obsUs += std::chrono::duration<double, std::micro>(o1 - o0).count();
                    m_netUs += std::chrono::duration<double, std::micro>(o2 - o1).count();
                    bool held = forcedHere ? m_forced[m_t] : net::holds(logit);
                    if (!opt.practice && m_attempt > 1 && !forcedHere && !m_checking) {
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
                        if (learning()) return false;  // a self-training round before the next attempt (slice())
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

            // Playing time: self-training's rounds come on top of the time limit.
            double secondsUsed() const {
                return std::chrono::duration<double>(Clock::now() - m_t0).count() - m_learn.seconds;
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
                // A best-guess run (attempt 1, a check) holds nothing the network doesn't already do.
                bool const bestGuess = m_attempt == 1 || m_checking;
                if (m_learn.on) {
                    if (m_attempt == 1) m_acceptedX = x;  // the best guess of the network it began with
                    else if (m_checking) judgeCheck(x);
                }
                if (x > m_bestX) {
                    m_bestX = x;
                    m_bestPercent = pct;
                    m_bestAttempt = m_attempt;
                    m_bestD = m_d;  // ticks 0 to D: the attempt's whole line
                    if (!bestGuess) learnFrom(D + 1, x, false);  // self-training on this run (slice() runs it next)
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
                trimRecs();
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
                trimRecs();
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
                trimRecs();
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
                h["pilot_file"] = m_pilotFrom;
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

            // The name a run's macro goes by: the one asked for, else "AI -
            // <level name>".
            std::string macroBase() const {
                return opt.saveMacro.empty() ? "AI - " + m_levelName : opt.saveMacro;
            }

            // A line (one decision a tick from the level's start) as a GucciBot
            // macro: tick t is frame F + 1 + t, and a hold still down at the end
            // is let go on the frame after.
            convert::Macro lineMacro(std::vector<uint8_t> const& d, std::string const& name) const {
                convert::Macro m;
                m.tps = m_tps;
                m.levelName = m_levelName;
                m.levelId = m_levelId;
                m.name = name;
                m.bot = "GucciBot AI pilot";
                uint32_t const T = (uint32_t)d.size();
                bool down = false;
                for (uint32_t t = 0; t < T; t++) {
                    bool const h = d[t] != 0;
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
                return m;
            }

            // The attempt playing when the run stops counts as the best if it got
            // further than every attempt that ended (out of time mid-attempt, 20
            // minutes alive, stopped by hand): its line is the furthest run.
            void adoptLive() {
                if (!m_running || m_completed || m_d.empty() || !(m_lastX > m_bestX)) return;
                m_bestX = m_lastX;
                m_bestPercent = percentOf(m_lastX);
                m_bestAttempt = m_attempt;
                m_bestD = m_d;
            }

            // A line saved as a macro, for a finished run and a furthest one
            // alike: in the theme's macro format (its extension), so the Saved
            // macros list shows it, named `base`, or "<base> 2", "<base> 3"...
            // past a name a saved macro has already (any theme's extension), so
            // an earlier run's file is never replaced. ok false: the file couldn't
            // be written, and nothing is left behind.
            struct SavedLine {
                std::string name;
                fs::path path;
                size_t inputs = 0;
                bool ok = false;
            };
            SavedLine saveLine(std::vector<uint8_t> const& d, std::string const& base) const {
                SavedLine s;
                auto* gb = GucciEngine::get();
                s.name = base;
                for (int n = 2; !gb->findMacroFile(s.name).empty(); n++) s.name = fmt::format("{} {}", base, n);
                convert::Macro const m = lineMacro(d, s.name);
                s.inputs = m.inputs.size();
                auto const bytes = convert::toGucciBot(m);
                s.path = gb->getReplayDir() / u8path(s.name + gucci::ui::macroExtension());
                std::error_code ec;
                fs::create_directories(s.path.parent_path(), ec);
                {
                    std::ofstream f(s.path, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<char const*>(bytes.data()), (std::streamsize)bytes.size());
                    s.ok = (bool)f;
                }
                if (!s.ok) {
                    fs::remove(s.path, ec);
                    return s;
                }
                gb->reloadMacroList();  // in Saved macros now, not at the next Refresh
                return s;
            }

            // GucciBot (2026-11-23, Juice's ask 11-22): a run that stops without
            // beating the level saves its furthest run as a macro, as the Search
            // saves "<level name> furthest" (absense/pathfinder/pathfinder.cpp,
            // saveFurthest): "AI - <level name> furthest", numbered past a name a
            // saved macro has already ("... furthest 2"), so an earlier run's file
            // is never replaced (saveLine). The file is the best attempt's own
            // line, kept when it ended (m_bestD), not the last attempt's. Once a
            // run; its words for the stop's line, "" when nothing was saved.
            std::string saveFurthest(matjson::Value& out) {
                if (!opt.saveFurthest || m_furthestSaved || m_completed || m_bestD.empty()) return {};
                m_furthestSaved = true;
                SavedLine const s = saveLine(m_bestD, fileSafe(macroBase()) + " furthest");
                if (!s.ok) {
                    out["furthest_macro_error"] = "could not write " + utf8(s.path);
                    return fmt::format(" Its furthest run couldn't be saved ({} couldn't be written).",
                                       utf8(s.path.filename()));
                }
                out["furthest_macro"] = s.name;
                out["furthest_macro_file"] = utf8(s.path);
                out["furthest_macro_inputs"] = (int64_t)s.inputs;
                out["furthest_macro_ticks"] = (int64_t)m_bestD.size();
                out["furthest_macro_attempt"] = m_bestAttempt;
                out["furthest_macro_percent"] = (double)m_bestPercent;
                return fmt::format(" Its furthest run ({:.1f}%) is saved as \"{}\".", m_bestPercent, s.name);
            }

            bool onComplete(sim::State const&) {
                uint32_t const T = m_t;
                m_completed = true;
                writeLog(true, -1, 0.0f);
                if (m_learn.on && m_checking) judgeCheck(m_levelLength);  // the trained network's best guess finished it
                else if (m_attempt > 1) learnFrom(T, m_levelLength, true);  // the last round (slice() runs it after this)
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
                // Saved as a GucciBot macro (frame labels: tick t is frame F + 1 + t),
                // the way the furthest run is (saveLine; Nigel, 2026-11-23): in the
                // theme's macro format, so Saved macros lists it (it was a .gbr6 the
                // list didn't show), as "AI - <level name>" (or save_macro), numbered
                // past a name that's taken instead of writing over that file.
                {
                    SavedLine const s = saveLine(m_d, fileSafe(macroBase()));
                    if (s.ok) {
                        out["macro"] = s.name;
                        out["macro_file"] = utf8(s.path);
                        out["macro_inputs"] = (int64_t)s.inputs;
                        say(fmt::format("Saved as the macro \"{}\".", s.name));
                    } else {
                        out["macro_error"] = "could not write " + utf8(s.path);
                        say(fmt::format("It couldn't be saved as a macro ({} couldn't be written).",
                                        utf8(s.path.filename())));
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
                out["pilot"] = m_pilotFrom;
                out["pilot_layout_hash"] = models().pilot.net.layoutHash();
                out["level_net"] = m_levelNet;  // it played this level's own trained network
                out["self_training"] = learnJson();
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

            // What a run that stopped early had got to (a Stop, the level
            // left). matjson's Value() is an empty object, not null, so a
            // result nothing has set yet is {}: checking isNull() alone kept a
            // stopped run's result empty (2026-11-24, found in the quickwins
            // lane's in-game test: gucci_ai_status had no best_percent or
            // attempts after a Stop). A result finish() set is kept.
            void partialResult(char const* stopped) {
                if (!result.isNull() && !(result.isObject() && result.size() == 0)) return;
                result = baseResult(stopped);
            }

            bool finish(char const* stopped, std::string const& why) {
                sim::end();
                m_start.release();
                adoptLive();
                auto out = baseResult(stopped);
                out["why"] = why;
                std::string const saved = saveFurthest(out);
                result = out;
                progress = fmt::format("done: {}", stopped);
                say(fmt::format("{} Best {:.1f}% (attempt {}), {} attempts.{}", why, m_bestPercent, m_bestAttempt,
                                m_attempt, saved));
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

    fs::path levelNetPath(int64_t levelId) {
        return weightsDir() / "levels" / (std::to_string(levelId) + ".gbnn");
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
                return fmt::format("No trained AI: there is no pilot.gbnn in {}, and the built-in one is missing from "
                                   "the mod's files. The trainer (tools/ai) puts one there; then press Reload.",
                                   utf8(weightsDir()));
            if (M.pilot.builtIn) return fmt::format("The built-in AI can't be used: {}", M.pilot.error);
            return fmt::format("The pilot in {} can't be used: {}", M.pilot.path, M.pilot.error);
        }
        // GucciBot (2026-11-19): names the network in use. The built-in one is
        // the AI loop's network of 2026-11-15: imitation of human macros
        // (11-03), then reinforcement learning on its own runs (11-10), then
        // more on Stereo Madness practice (11-15).
        std::string s = M.pilot.builtIn ? std::string("Built-in AI (trained on human play, then on its own runs, Nov 2026).")
                                        : fmt::format("Your own AI: pilot loaded ({}).", M.pilot.path);
        if (M.critic.net.loaded()) s += " Critic loaded.";
        else s += " No critic: mistakes are picked from the pilot's own doubts.";
        return s;
    }

    std::unique_ptr<job::Job> makePlay(PlayOptions const& options, std::string& why) {
        // GucciBot (2026-11-15): off Windows too, without the trigger model (the
        // World stays off there, as Windows' own runs fall back to when it
        // switches off). The Pathfinder page says what that costs.
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
    }

    Live const& live() {
        return s_live;
    }

    void setLiveJob(int id) {
        s_live.job = id;
    }

    // GucciBot (2026-11-09): the search engine is the default until a trained
    // network ships in the package (Nigel's call). None did then, so with AI as
    // the default a new install's Pathfinder page opened on "No trained AI yet"
    // with Start greyed out. Someone who picked an engine keeps their pick.
    // 2026-11-19: one ships now (findWeights), and the search stays the
    // default for 2.0: it is far stronger than this early AI.
    bool engineAi() {
        return Mod::get()->getSavedValue<bool>("pf_engine_ai", false);
    }

    void setEngineAi(bool on) {
        Mod::get()->setSavedValue("pf_engine_ai", on);
    }

    // GucciBot (2026-11-19, build -h): the AI card's Practice switch, on by
    // default (Nigel's call). Until now it was off by default under
    // pf_ai_old_practice ("Old practice rules"), and Geode writes a default it
    // reads into the save, so a save that ever showed the AI card holds that
    // off. Saved under a new key, as Exact start flags did
    // (pf_exact_start_flags2, absense/compat/bot.cpp): nothing reads the old
    // one now.
    static constexpr const char* kPracticeKey = "pf_ai_practice";

    bool practiceOn() {
        // A missing key is on. Read without getSavedValue's default, which
        // would write the key into the save on a mere look.
        auto const& saved = Mod::get()->getSaveContainerConst();
        return !saved.contains(kPracticeKey) || saved[kPracticeKey].asBool().unwrapOr(true);
    }

    void setPracticeOn(bool on) {
        // Written outright: Geode's setSavedValue writes nothing when the value
        // equals what the key reads as, and to it a missing key reads as off -
        // so switching this off before it was ever saved would not stick.
        Mod::get()->getSaveContainer()[kPracticeKey] = on;
    }

} // namespace gucci::ai::pilot
