// GucciBot's MCP tools.
//
// Every tool here runs on the game's main thread (see mcp_server.cpp) and is
// deliberately thin: it reads or pokes one thing and hands back JSON. The
// judgement about what to do next lives with whatever is driving, not here.
//
// Two rules the tools hold to:
//   - anything that would DRIVE the game refuses while a render or an
//     analyzer run owns it. Fighting those for control is how you get a
//     desync that looks like a real bug.
//   - reads never refuse, because the whole point is being able to look at a
//     run that has gone wrong while it is still wrong.

#include "mcp_server.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"
#include "analysis/pathfinder.hpp"
#include "absense/glue.hpp"
#include "mcp/tickprobe.hpp"
#include "analysis/trajectory.hpp"
#include "tools/macro_check.hpp"
#include "core/GucciBot.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <sstream>
#include <fstream>

using namespace geode::prelude;
using namespace gucci;

namespace fs = std::filesystem;

namespace gucci::mcp {

    namespace {

        matjson::Value obj() {
            return matjson::Value::object();
        }

        std::string argStr(matjson::Value const& a, char const* key, std::string def = "") {
            if (!a.contains(key))
                return def;
            return a[key].asString().unwrapOr(def);
        }

        int64_t argInt(matjson::Value const& a, char const* key, int64_t def) {
            if (!a.contains(key))
                return def;
            return a[key].asInt().unwrapOr(def);
        }

        bool argBool(matjson::Value const& a, char const* key, bool def) {
            if (!a.contains(key))
                return def;
            return a[key].asBool().unwrapOr(def);
        }

        // Who, if anyone, currently owns the run. Returned by get_state so the
        // caller can see it, and used to refuse the driving tools.
        std::string runOwner() {
            auto* gb = GucciEngine::get();
            if (SLRenderer::get()->isRecording())
                return "render";
            if (gb->analyzerOwnsRun())
                return "analyzer";
            if (Pathfinder::get()->active || absense::isRunning() || absense::startPending())
                return "pathfinder";
            return "";
        }

        void requireFreeRun() {
            auto const who = runOwner();
            if (!who.empty())
                throw ToolError(who + " currently owns the run; cancel it first");
        }

        PlayLayer* requireLevel() {
            auto* pl = PlayLayer::get();
            if (!pl)
                throw ToolError("not in a level");
            return pl;
        }

        char const* modeName(GucciEngine::Mode m) {
            switch (m) {
                case GucciEngine::Mode::Recording: return "record";
                case GucciEngine::Mode::Playing: return "play";
                default: return "idle";
            }
        }

        char const* actionTypeName(gb::ActionType t) {
            switch (t) {
                case gb::ActionType::Jump: return "jump";
                case gb::ActionType::Left: return "left";
                case gb::ActionType::Right: return "right";
                case gb::ActionType::Death: return "death";
                case gb::ActionType::Restart: return "restart";
                case gb::ActionType::RestartFull: return "restart_full";
                case gb::ActionType::TPS: return "tps";
            }
            return "?";
        }

        matjson::Value actionJson(gb::Action const& a) {
            auto e = obj();
            e["frame"] = (int64_t)a.m_frame;
            e["type"] = actionTypeName(a.m_type);
            e["holding"] = a.m_holding;
            if (a.m_player2)
                e["player2"] = true;
            if (a.m_type == gb::ActionType::TPS)
                e["tps"] = a.m_tps;
            if (a.isInput() && a.m_subtick > 0.0)
                e["subtick"] = a.m_subtick;  // SCBF: where in the tick it lands
            return e;
        }

        // A JSON-schema object from (name, type, description) triples.
        // Scripts for the Absense diagnostics: space-separated steps, h<N> =
        // hold N ticks, r<N> = release N ticks, p<N> = press on each of N
        // ticks (a release first when already held).
        std::vector<absense::ScriptTick> parseAbsScript(std::string const& text) {
            std::vector<absense::ScriptTick> held;
            std::istringstream in(text);
            std::string step;
            while (in >> step) {
                if (step.size() < 2 || (step[0] != 'h' && step[0] != 'r' && step[0] != 'p'))
                    throw ToolError("script steps are h<N>, r<N> or p<N>, got \"" + step + "\"");
                int n = 0;
                try {
                    n = std::stoi(step.substr(1));
                } catch (...) {
                    throw ToolError("bad tick count in \"" + step + "\"");
                }
                if (n < 1 || n > 3000)
                    throw ToolError("tick counts are 1 to 3000");
                held.insert(held.end(), (size_t)n, absense::ScriptTick{step[0] == 'p', step[0] != 'r'});
                if (held.size() > 3000)
                    throw ToolError("script longer than 3000 ticks");
            }
            return held;
        }

        matjson::Value schemaOf(std::initializer_list<std::array<char const*, 3>> fields) {
            auto props = obj();
            for (auto const& f : fields) {
                auto p = obj();
                p["type"] = f[1];
                p["description"] = f[2];
                props[f[0]] = p;
            }
            auto schema = obj();
            schema["type"] = "object";
            schema["properties"] = props;
            return schema;
        }

        char const* gamemodeName(PlayerObject* p) {
            if (p->m_isShip) return "ship";
            if (p->m_isBall) return "ball";
            if (p->m_isBird) return "ufo";
            if (p->m_isDart) return "wave";
            if (p->m_isRobot) return "robot";
            if (p->m_isSpider) return "spider";
            if (p->m_isSwing) return "swing";
            return "cube";
        }

        // Everything physics reads off a player -- Absense's player_state,
        // plus the fields the Congregation slope investigation found differing
        // between a normal run and Calculate's (m_hasEverJumped,
        // m_lastJumpTime, m_snapDistance, m_reverseRelated).
        matjson::Value playerJson(PlayerObject* p) {
            auto j = obj();
            j["x"] = (double)p->m_position.x;
            j["y"] = (double)p->m_position.y;
            j["drawn_x"] = (double)p->getPositionX();
            j["drawn_y"] = (double)p->getPositionY();
            j["x_velocity"] = (double)p->getCurrentXVelocity();
            j["platformer_x_velocity"] = p->m_platformerXVelocity;
            j["y_velocity"] = p->m_yVelocity;
            j["rotation"] = (double)p->getRotation();
            j["gamemode"] = gamemodeName(p);
            j["mini"] = p->m_vehicleSize < 1.f;
            j["speed"] = (double)p->m_playerSpeed;
            j["gravity_mod"] = (double)p->m_gravityMod;
            j["upside_down"] = p->m_isUpsideDown;
            j["sideways"] = p->m_isSideways;
            j["going_left"] = p->m_isGoingLeft;
            j["on_ground"] = p->m_isOnGround;
            j["dashing"] = p->m_isDashing;
            j["dead"] = p->m_isDead;
            auto const it = p->m_holdingButtons.find(1);
            j["holding_jump"] = it != p->m_holdingButtons.end() && it->second;
            j["jump_buffered"] = p->m_jumpBuffered;
            j["on_slope"] = p->m_currentSlope != nullptr;
            j["has_ever_jumped"] = p->m_hasEverJumped;
            j["last_jump_time"] = p->m_lastJumpTime;
            j["snap_distance"] = p->m_snapDistance;
            j["reverse_related"] = (int64_t)p->m_reverseRelated;
            return j;
        }

    } // namespace

    void registerTools(Server& server) {

        // --- reading -------------------------------------------------------

        server.addTool({
            "gucci_get_state",
            "Everything about what GucciBot and the game are doing right now: "
            "build, mode, frame, level, player position, and who owns the run.",
            obj(),
            [](matjson::Value const&) {
                auto* gb = GucciEngine::get();
                auto out = obj();

                out["build"] = GB_BUILD_LABEL;
                out["version"] = MOD_VERSION;
                out["enabled"] = gb->enabled;
                out["mode"] = modeName(gb->mode);
                out["frame"] = (int64_t)gb->updater.getFrame();
                out["tps"] = gb->updater.m_tps;
                out["speedhack"] = gb->updater.m_speedhack;
                out["paused"] = gb->updater.m_paused;

                auto owner = runOwner();
                out["run_owner"] = owner.empty() ? "none" : owner;

                out["macro"] = gb->replay.m_replayName;
                out["macro_actions"] = (int64_t)gb->replay.m_actionAtom.length();
                out["macro_input_index"] = (int64_t)gb->replay.m_inputIndex;

                auto* pl = PlayLayer::get();
                out["in_level"] = pl != nullptr;
                if (pl) {
                    auto lvl = obj();
                    if (pl->m_level) {
                        lvl["name"] = std::string(pl->m_level->m_levelName);
                        lvl["id"] = (int64_t)pl->m_level->m_levelID.value();
                        lvl["attempts"] = (int64_t)pl->m_level->m_attempts.value();
                    }
                    lvl["length_x"] = (double)gb->m_levelLength;
                    lvl["practice"] = pl->m_isPracticeMode;
                    out["level"] = lvl;

                    if (pl->m_player1) {
                        auto p = obj();
                        p["x"] = (double)pl->m_player1->getPositionX();
                        p["y"] = (double)pl->m_player1->getPositionY();
                        p["y_velocity"] = pl->m_player1->m_yVelocity;
                        p["on_ground"] = pl->m_player1->m_isOnGround;
                        p["dead"] = pl->m_player1->m_isDead;
                        p["mini"] = pl->m_player1->m_vehicleSize < 1.f;
                        if (gb->m_levelLength > 0.f)
                            p["percent"] =
                                (double)(pl->m_player1->getPositionX() / gb->m_levelLength * 100.f);
                        out["player1"] = p;
                    }
                }
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto from = obj();
            from["type"] = "integer";
            from["description"] = "first frame to include (default 0)";
            auto to = obj();
            to["type"] = "integer";
            to["description"] = "last frame to include (default: end of macro)";
            auto limit = obj();
            limit["type"] = "integer";
            limit["description"] = "max actions to return (default 200)";
            props["from"] = from;
            props["to"] = to;
            props["limit"] = limit;
            schema["type"] = "object";
            schema["properties"] = props;

            server.addTool({
                "gucci_get_macro_actions",
                "The loaded macro's actions in a frame range. Use this to see "
                "exactly what the bot will press, rather than inferring it.",
                schema,
                [](matjson::Value const& a) {
                    auto* gb = GucciEngine::get();
                    auto const& acts = gb->replay.m_actionAtom.m_actions;

                    auto const from = (uint32_t)std::max<int64_t>(0, argInt(a, "from", 0));
                    auto const to = (uint32_t)std::max<int64_t>(0, argInt(a, "to", UINT32_MAX));
                    auto const limit = (size_t)std::clamp<int64_t>(argInt(a, "limit", 200), 1, 5000);

                    auto arr = matjson::Value::array();
                    int64_t skipped = 0;
                    for (auto const& act : acts) {
                        if (act.m_frame < from || act.m_frame > to)
                            continue;
                        if (arr.size() >= limit) {
                            skipped++;
                            continue;
                        }
                        arr.push(actionJson(act));
                    }

                    auto out = obj();
                    out["macro"] = gb->replay.m_replayName;
                    out["total_actions"] = (int64_t)acts.size();
                    out["returned"] = (int64_t)arr.size();
                    if (skipped > 0)
                        out["truncated"] = skipped;
                    out["actions"] = arr;
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_list_macros",
            "The macro files on disk, with size and extension.",
            obj(),
            [](matjson::Value const&) {
                auto dir = GucciEngine::get()->getReplayDir();
                auto arr = matjson::Value::array();
                std::error_code ec;
                if (fs::exists(dir, ec)) {
                    for (auto const& it : fs::directory_iterator(dir, ec)) {
                        if (!it.is_regular_file(ec))
                            continue;
                        auto e = obj();
                        e["name"] = it.path().stem().string();
                        e["ext"] = it.path().extension().string();
                        e["bytes"] = (int64_t)fs::file_size(it.path(), ec);
                        arr.push(e);
                    }
                }
                auto out = obj();
                out["dir"] = dir.string();
                out["macros"] = arr;
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto file = obj();
            file["type"] = "string";
            file["description"] =
                "log file name in the mod's save dir, e.g. guccibot_fw.log or "
                "guccibot_slope.log. Must end in .log.";
            auto lines = obj();
            lines["type"] = "integer";
            lines["description"] = "how many lines from the end (default 200, max 4000)";
            auto grep = obj();
            grep["type"] = "string";
            grep["description"] = "only return lines containing this substring";
            props["file"] = file;
            props["lines"] = lines;
            props["grep"] = grep;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"file"};

            server.addTool({
                "gucci_read_log",
                "Tail one of GucciBot's own log files. This is the tool that "
                "replaces asking someone to paste a log after the fact.",
                schema,
                [](matjson::Value const& a) {
                    auto const name = argStr(a, "file");
                    // Name only, and only a .log -- this must not become a way
                    // to read arbitrary files off the machine.
                    if (name.empty() || name.find('/') != std::string::npos ||
                        name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
                        throw ToolError("file must be a bare log name, no path");
                    if (name.size() < 4 || name.substr(name.size() - 4) != ".log")
                        throw ToolError("file must end in .log");

                    auto path = Mod::get()->getSaveDir() / name;
                    std::ifstream in(path);
                    if (!in.is_open())
                        throw ToolError("no such log: " + path.string());

                    auto const want =
                        (size_t)std::clamp<int64_t>(argInt(a, "lines", 200), 1, 4000);
                    auto const needle = argStr(a, "grep");

                    std::deque<std::string> tail;
                    std::string line;
                    int64_t total = 0;
                    while (std::getline(in, line)) {
                        total++;
                        if (!needle.empty() && line.find(needle) == std::string::npos)
                            continue;
                        tail.push_back(line);
                        if (tail.size() > want)
                            tail.pop_front();
                    }

                    auto arr = matjson::Value::array();
                    for (auto const& l : tail)
                        arr.push(l);

                    auto out = obj();
                    out["file"] = path.string();
                    out["total_lines"] = total;
                    out["returned"] = (int64_t)arr.size();
                    out["lines"] = arr;
                    return out;
                },
            });
        }

        // --- the analyzer --------------------------------------------------

        {
            auto schema = obj();
            auto props = obj();
            auto action = obj();
            action["type"] = "string";
            action["enum"] = std::vector<std::string>{"start", "cancel", "results"};
            action["description"] = "start a run, cancel one, or read the marks";
            props["action"] = action;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};

            server.addTool({
                "gucci_analyzer",
                "Drive the frame-window analyzer (Calculate): start it on the "
                "loaded macro, cancel it, or read the resulting windows.",
                schema,
                [](matjson::Value const& a) {
                    auto& fw = Bot::get()->frameWindow();
                    auto const what = argStr(a, "action");
                    auto out = obj();

                    if (what == "start") {
                        requireFreeRun();
                        auto* pl = requireLevel();
                        auto rep = fw.start(pl);
                        out["started"] = rep.ok;
                        out["message"] = rep.message;
                        return out;
                    }
                    if (what == "cancel") {
                        fw.cancel();
                        out["cancelled"] = true;
                        return out;
                    }
                    if (what == "results") {
                        auto arr = matjson::Value::array();
                        for (auto const& mk : fw.results()) {
                            if (mk.hidden)
                                continue;
                            auto e = obj();
                            e["frame"] = (int64_t)mk.frame;
                            e["window"] = (int64_t)mk.window;
                            e["low"] = mk.low;
                            e["high"] = mk.high;
                            e["percent"] = (double)mk.percent;
                            if (mk.release)
                                e["release"] = true;
                            if (mk.player2)
                                e["player2"] = true;
                            if (mk.unbounded)
                                e["unbounded"] = true;
                            if (mk.desynced)
                                e["desynced"] = true;
                            if (mk.cbf)
                                e["cbf"] = true;
                            if (mk.clampedByNeighbour)
                                e["clamped_by_neighbour"] = true;
                            arr.push(e);
                        }
                        out["running"] = fw.running();
                        out["capturing"] = fw.capturing();
                        out["count"] = (int64_t)arr.size();
                        out["marks"] = arr;
                        return out;
                    }
                    throw ToolError("action must be start, cancel or results");
                },
            });
        }

        server.addTool({
            "gucci_get_lstar",
            "The L* difficulty value for the current analysis, plus the "
            "per-input contributions that make it up.",
            obj(),
            [](matjson::Value const&) {
                auto* s = lstar::Solver::get();
                auto const& r = s->result();
                auto out = obj();
                out["running"] = s->running();
                out["progress"] = (double)s->progress();
                out["ok"] = r.m_ok;
                out["value"] = r.m_value;
                auto arr = matjson::Value::array();
                for (auto v : r.m_perInput)
                    arr.push(v);
                out["per_input"] = arr;
                return out;
            },
        });

        // --- the pathfinder ------------------------------------------------

        {
            auto schema = obj();
            auto props = obj();
            auto action = obj();
            action["type"] = "string";
            action["enum"] = std::vector<std::string>{"start", "cancel", "status"};
            props["action"] = action;
            auto engine = obj();
            engine["type"] = "string";
            engine["enum"] = std::vector<std::string>{"absense", "classic"};
            engine["description"] =
                "which pathfinder to start (default: the one picked in the Pathfinder tab)";
            props["engine"] = engine;
            auto fromBeginning = obj();
            fromBeginning["type"] = "boolean";
            fromBeginning["description"] =
                "Absense's engine: restart the level (Full Restart) and search from frame 0 "
                "(default: the Pathfinder tab's switch). It also starts from the pause menu.";
            props["from_beginning"] = fromBeginning;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};

            server.addTool({
                "gucci_pathfinder",
                "Drive the pathfinder: start a search, cancel it, or read how "
                "far it has got and what it has found. Two engines: Absense's "
                "(plans each input by running copies of the player ahead; its "
                "status is under \"absense\") and the classic one.",
                schema,
                [](matjson::Value const& a) {
                    auto* pf = Pathfinder::get();
                    auto const what = argStr(a, "action");
                    auto out = obj();

                    if (what == "start") {
                        requireFreeRun();
                        requireLevel();
                        auto const eng = argStr(a, "engine");
                        bool const classic =
                            eng.empty() ? absense::classicSelected() : eng == "classic";
                        if (!eng.empty() && eng != "classic" && eng != "absense")
                            throw ToolError("engine must be absense or classic");
                        out["engine"] = classic ? "classic" : "absense";
                        if (classic) {
                            pf->begin();
                            out["started"] = pf->active;
                        } else {
                            // Queued: the start is carried out over the next
                            // frames (pause menu, restart); poll status.
                            bool const fromStart = argBool(a, "from_beginning", absense::startFromBeginning());
                            out["queued"] = absense::requestStart(fromStart);
                            out["from_beginning"] = fromStart;
                            out["message"] = absense::status().message;
                        }
                        return out;
                    }
                    if (what == "cancel") {
                        absense::stopPathfinder();
                        pf->cancel();
                        out["cancelled"] = true;
                        return out;
                    }
                    if (what == "status") {
                        {
                            auto const st = absense::status();
                            auto ab = obj();
                            ab["running"] = st.running;
                            ab["phase"] = std::string(st.phase);
                            ab["progress_percent"] = (double)st.progress * 100.0;
                            ab["best_percent"] = (double)st.bestProgress * 100.0;
                            ab["start_tick"] = (int64_t)st.startTick;
                            ab["current_tick"] = (int64_t)st.currentTick;
                            ab["best_tick"] = (int64_t)st.bestTick;
                            ab["decisions"] = (int64_t)st.decisions;
                            ab["dead_ends"] = (int64_t)st.deadEnds;
                            ab["backtracks"] = (int64_t)st.backtracks;
                            ab["simulations"] = (int64_t)st.simulations;
                            ab["freezes"] = (int64_t)st.freezes;
                            ab["seconds"] = st.seconds;
                            ab["last_decision"] = st.lastDecision;
                            ab["message"] = st.message;
                            out["absense"] = ab;
                        }
                        out["active"] = pf->active;
                        out["stage"] = pf->stage;
                        out["runs"] = (int64_t)pf->runs;
                        out["best_x"] = (double)pf->bestX;
                        out["best_percent"] = (double)pf->bestPct;
                        out["has_result"] = pf->hasResult;
                        out["succeeded"] = pf->lastResultSuccess;
                        out["saved_as"] = pf->savedAs;
                        out["probe_runs"] = (int64_t)pf->probeRuns;
                        out["probe_fails"] = (int64_t)pf->probeFails;
                        out["agency_frames_seen"] = (int64_t)pf->agencyFramesSeen;
                        out["last_point_count"] = (int64_t)pf->lastPointCount;
                        out["last_lookback"] = (int64_t)pf->lastLookback;
                        out["deferred_cramped"] = (int64_t)pf->deferredCramped;
                        out["deferred_replayed"] = (int64_t)pf->deferredReplayed;
                        return out;
                    }
                    throw ToolError("action must be start, cancel or status");
                },
            });
        }

        // --- driving the game ----------------------------------------------

        {
            auto schema = obj();
            auto props = obj();
            auto mode = obj();
            mode["type"] = "string";
            mode["enum"] = std::vector<std::string>{"idle", "record", "play"};
            props["mode"] = mode;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"mode"};

            server.addTool({
                "gucci_set_mode",
                "Put GucciBot into idle, record or play.",
                schema,
                [](matjson::Value const& a) {
                    requireFreeRun();
                    auto* gb = GucciEngine::get();
                    auto const m = argStr(a, "mode");
                    if (m == "idle")
                        gb->setMode(GucciEngine::Mode::Idle);
                    else if (m == "record")
                        gb->setMode(GucciEngine::Mode::Recording);
                    else if (m == "play")
                        gb->setMode(GucciEngine::Mode::Playing);
                    else
                        throw ToolError("mode must be idle, record or play");

                    auto out = obj();
                    out["mode"] = modeName(gb->mode);
                    return out;
                },
            });
        }

        {
            auto schema = obj();
            auto props = obj();
            auto name = obj();
            name["type"] = "string";
            name["description"] = "macro name as gucci_list_macros reports it (no extension)";
            props["name"] = name;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"name"};

            server.addTool({
                "gucci_load_macro",
                "Load a macro from the replays folder.",
                schema,
                [](matjson::Value const& a) {
                    requireFreeRun();
                    auto const want = argStr(a, "name");
                    if (want.empty())
                        throw ToolError("name is required");

                    auto dir = GucciEngine::get()->getReplayDir();
                    std::error_code ec;
                    fs::path found;
                    for (auto const& it : fs::directory_iterator(dir, ec)) {
                        if (it.is_regular_file(ec) && it.path().stem().string() == want) {
                            found = it.path();
                            break;
                        }
                    }
                    if (found.empty())
                        throw ToolError("no macro called " + want);

                    GucciEngine::get()->replay.load(found);

                    auto out = obj();
                    out["loaded"] = found.filename().string();
                    out["actions"] = (int64_t)GucciEngine::get()->replay.m_actionAtom.length();
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_restart_level",
            "Restart the current level from the start or the last checkpoint, "
            "the same as pressing the restart key.",
            obj(),
            [](matjson::Value const&) {
                requireFreeRun();
                auto* pl = requireLevel();
                pl->resetLevel();
                auto out = obj();
                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto paused = obj();
            paused["type"] = "boolean";
            paused["description"] = "true to freeze the engine, false to let it run";
            props["paused"] = paused;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"paused"};

            server.addTool({
                "gucci_set_paused",
                "Freeze or unfreeze the engine, so frames can be stepped one at "
                "a time.",
                schema,
                [](matjson::Value const& a) {
                    requireFreeRun();
                    auto* gb = GucciEngine::get();
                    gb->updater.m_paused = argBool(a, "paused", true);
                    auto out = obj();
                    out["paused"] = gb->updater.m_paused;
                    out["frame"] = (int64_t)gb->updater.getFrame();
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_step_frame",
            "Advance exactly one frame while paused. Returns the frame BEFORE "
            "the step -- the step itself happens on the game's next tick, so "
            "read gucci_get_state afterwards to see where it landed.",
            obj(),
            [](matjson::Value const&) {
                requireFreeRun();
                auto* gb = GucciEngine::get();
                if (!gb->updater.m_paused)
                    throw ToolError("not paused; call gucci_set_paused first");
                gb->updater.stepOnce();
                auto out = obj();
                out["frame_before"] = (int64_t)gb->updater.getFrame();
                return out;
            },
        });

        // ---- ported from Absense's MCP set (2026-09-27): the tools that
        // help a session debug GucciBot. Its level-editor, UI-automation and
        // level-download tools are not ported -- see the commit.

        server.addTool({
            "gucci_player_state",
            "The full physics state of player 1 and, in dual mode, player 2: "
            "physics and drawn position, velocities, rotation, game mode, size, "
            "gravity, ground/slope/dash/dead flags, held buttons, and the jump "
            "bookkeeping GD keeps (has_ever_jumped, last_jump_time, "
            "snap_distance).",
            obj(),
            [](matjson::Value const&) {
                auto* pl = requireLevel();
                auto out = obj();
                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                out["game_tick"] = (int64_t)pl->m_gameState.m_currentProgress;
                out["player1"] = playerJson(pl->m_player1);
                if (pl->m_gameState.m_isDualMode && pl->m_player2)
                    out["player2"] = playerJson(pl->m_player2);
                return out;
            },
        });

        server.addTool({
            "gucci_level_info",
            "The open level: name, id, length, object count, platformer / "
            "two-player / dual, time warp, practice mode.",
            obj(),
            [](matjson::Value const&) {
                auto* pl = requireLevel();
                auto* gb = GucciEngine::get();
                auto out = obj();
                if (pl->m_level) {
                    out["name"] = std::string(pl->m_level->m_levelName);
                    out["id"] = (int64_t)pl->m_level->m_levelID.value();
                }
                out["length_x"] = (double)gb->m_levelLength;
                out["objects"] = (int64_t)(pl->m_objects ? pl->m_objects->count() : 0);
                out["platformer"] = pl->m_isPlatformer;
                out["two_player"] = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
                out["dual"] = pl->m_gameState.m_isDualMode;
                out["time_warp"] = (double)pl->m_gameState.m_timeWarp;
                out["practice"] = pl->m_isPracticeMode;
                return out;
            },
        });

        // Practice checkpoints, driven the way a player drives them, so a
        // session can check that a respawn really puts the level and the
        // player back where they were. `level_hash` fingerprints every
        // object's position, rotation, scale, opacity and disabled flags:
        // take it at a checkpoint on a straight run, respawn, step back to
        // the same frame, and the two must match.
        server.addTool({
            "gucci_practice",
            "Practice mode and checkpoints. action: status | on | off | "
            "checkpoint (the checkpoint key's own path; lands at the end of this "
            "frame) | respawn (back to the last checkpoint). status returns the "
            "frame, saved checkpoint frames and level_hash (every object's "
            "position/rotation/scale/opacity/disabled, hashed).",
            schemaOf({{"action", "string", "status, on, off, checkpoint or respawn"}}),
            [](matjson::Value const& a) {
                auto* pl = requireLevel();
                auto* gb = GucciEngine::get();
                auto const what = argStr(a, "action", "status");

                if (what == "on" || what == "off") {
                    requireFreeRun();
                    bool const on = what == "on";
                    if (pl->m_isPracticeMode != on)
                        pl->togglePracticeMode(on);
                } else if (what == "checkpoint") {
                    requireFreeRun();
                    if (!pl->m_isPracticeMode)
                        throw ToolError("practice mode is off");
                    // GD's checkpoint key handler, hooked in hook_playlayer.cpp.
                    reinterpret_cast<void (*)(void*, void*)>(geode::base::get() + 0x4ce060)(
                        nullptr, nullptr);
                } else if (what == "respawn") {
                    requireFreeRun();
                    pl->resetLevel();
                } else if (what != "status") {
                    throw ToolError("unknown action: " + what);
                }

                uint64_t h = 1469598103934665603ull;
                auto mix = [&h](uint64_t v) {
                    for (int i = 0; i < 8; i++) {
                        h ^= (v >> (i * 8)) & 0xFF;
                        h *= 1099511628211ull;
                    }
                };
                auto bits = [](float f) {
                    uint32_t u;
                    std::memcpy(&u, &f, sizeof u);
                    return (uint64_t)u;
                };
                int64_t count = 0;
                if (pl->m_objects) {
                    for (auto* o : CCArrayExt<GameObject*>(pl->m_objects)) {
                        if (!o)
                            continue;
                        auto const p = o->getPosition();
                        mix(bits(p.x));
                        mix(bits(p.y));
                        mix(bits(o->getRotation()));
                        mix(bits(o->getScaleX()));
                        mix(bits(o->getScaleY()));
                        mix((uint64_t)o->getOpacity());
                        mix((o->m_isDisabled ? 1u : 0u) | (o->m_isDisabled2 ? 2u : 0u));
                        count++;
                    }
                }

                auto out = obj();
                out["frame"] = (int64_t)gb->updater.getFrame();
                out["practice"] = pl->m_isPracticeMode;
                auto frames = matjson::Value::array();
                for (auto const& cp : gb->practiceFix.m_savedCheckpoints)
                    frames.push((int64_t)cp.m_frameOffset);
                out["checkpoints"] = frames;
                out["level_hash"] = fmt::format("{:016x}", h);
                out["objects_hashed"] = count;
                return out;
            },
        });

        server.addTool({
            "gucci_step_back",
            "Step back `frames` ticks while paused (default 1). Needs Backwards "
            "Stepping on. Returns the frame before and after.",
            schemaOf({{"frames", "integer", "how many ticks to step back (default 1)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto* gb = GucciEngine::get();
                if (!gb->updater.m_paused)
                    throw ToolError("not paused; call gucci_set_paused first");
                if (!gb->updater.m_backwardsStepping)
                    throw ToolError("Backwards Stepping is off");
                int const n = (int)std::clamp<int64_t>(argInt(a, "frames", 1), 1, 10000);
                auto out = obj();
                out["frame_before"] = (int64_t)gb->updater.getFrame();
                gb->updater.backwardsStep(n);
                out["frame_after"] = (int64_t)gb->updater.getFrame();
                return out;
            },
        });

        server.addTool({
            "gucci_click",
            "Queue a press or release of jump for the next tick, as if the "
            "player did it (recorded if recording).",
            schemaOf({{"press", "boolean", "true to press, false to release (default true)"},
                      {"player2", "boolean", "player 2 instead of 1 (default false)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                auto* pl = requireLevel();
                bool const press = argBool(a, "press", true);
                bool const p2 = argBool(a, "player2", false);
                pl->queueButton(1, press, p2, 0.0);
                auto out = obj();
                out["queued"] = press ? "press" : "release";
                out["player2"] = p2;
                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                return out;
            },
        });

        server.addTool({
            "gucci_check_macro",
            "Check the loaded macro for problems that desync playback: a press "
            "while already held, a release with nothing held, actions out of "
            "frame order, invalid types or TPS values. Also counts clicks, "
            "holds a death or restart cut, and holds still open at the end.",
            schemaOf({{"limit", "integer", "max findings to return (default 100)"}}),
            [](matjson::Value const& a) {
                auto const& actions = GucciEngine::get()->replay.m_actionAtom.m_actions;
                auto const r = macrocheck::check(actions);
                size_t const limit = (size_t)std::clamp<int64_t>(argInt(a, "limit", 100), 1, 10000);
                auto out = obj();
                out["actions"] = (int64_t)actions.size();
                out["ok"] = r.ok();
                out["problems"] = (int64_t)r.findings.size();
                out["clicks"] = (int64_t)r.clicks;
                out["held_to_end"] = (int64_t)r.heldToEnd;
                out["cut_by_reset"] = (int64_t)r.cutByReset;
                out["release_after_reset"] = (int64_t)r.releaseAfterReset;
                auto list = matjson::Value::array();
                for (size_t i = 0; i < r.findings.size() && i < limit; i++) {
                    auto const& f = r.findings[i];
                    auto e = obj();
                    e["index"] = (int64_t)f.index;
                    e["frame"] = (int64_t)f.frame;
                    e["problem"] = macrocheck::name(f.problem);
                    if (f.lane >= 0) {
                        e["player2"] = f.lane >= 3;
                        e["button"] = (int64_t)(f.lane % 3 + 1);
                    }
                    list.push(e);
                }
                out["findings"] = list;
                return out;
            },
        });

        server.addTool({
            "gucci_abs_simulate",
            "Diagnostic: run Absense's pathfinder simulation (its copies of the "
            "player, Silicate's physics, the World) from the real player's current "
            "state along a scripted input, and report the path, how far it got and "
            "what killed it. `script` is space-separated steps, h<N> = hold N ticks, "
            "r<N> = release N ticks, p<N> = press on each of N ticks (a release "
            "first when already held), e.g. \"h20 r15 p1 h40\"; the last step "
            "repeats to `ticks`. Nothing in the real game changes.",
            schemaOf({{"script", "string", "steps, e.g. h20 r15 p1 h40"},
                      {"ticks", "integer", "ticks to run (default: the script's length, max 3000)"},
                      {"every", "integer", "report the path every N ticks (default 4)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto const held = parseAbsScript(argStr(a, "script"));
                int const ticks = (int)std::clamp<int64_t>(
                    argInt(a, "ticks", std::max<int64_t>(1, (int64_t)held.size())), 1, 3000);
                int const every = (int)std::clamp<int64_t>(argInt(a, "every", 4), 1, 240);
                auto out = absense::simulate(held, ticks, every);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto action = obj();
            action["type"] = "string";
            action["enum"] = std::vector<std::string>{"start", "stop", "save", "diff", "read"};
            props["action"] = action;
            for (auto const* k : {"from", "to", "every"}) {
                auto p = obj();
                p["type"] = "integer";
                props[k] = p;
            }
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};
            server.addTool({
                "gucci_tick_probe",
                "Record player 1's state at every tick, to compare two runs of the "
                "same inputs. start: clear and record. stop: stop recording. save: "
                "keep what was recorded as the reference. diff: the first tick where "
                "the recording differs from the reference, with the ticks around it. "
                "read: the recording from `from` to `to`, every `every` ticks.",
                schema,
                [](matjson::Value const& a) {
                    auto const what = argStr(a, "action");
                    auto out = obj();
                    auto fmtSample = [](uint32_t f, tickprobe::Sample const& s) {
                        return fmt::format("{} x={:.3f} y={:.3f} vy={:.3f} r={:.1f}{}{} {} dt={:.5f}{} xd={:.6f}",
                                           f, s.x, s.y, s.yVel, s.rot,
                                           s.held ? " H" : "", s.onGround ? " G" : "", s.mode, s.dt,
                                           s.split ? " SPLIT" : "", s.extraDelta);
                    };
                    auto range = [](std::map<uint32_t, tickprobe::Sample> const& m) {
                        auto r = obj();
                        r["ticks"] = (int64_t)m.size();
                        r["first"] = m.empty() ? (int64_t)-1 : (int64_t)m.begin()->first;
                        r["last"] = m.empty() ? (int64_t)-1 : (int64_t)m.rbegin()->first;
                        return r;
                    };
                    if (what == "start") {
                        tickprobe::current.clear();
                        tickprobe::armed = true;
                    } else if (what == "stop") {
                        tickprobe::armed = false;
                    } else if (what == "save") {
                        tickprobe::saved = tickprobe::current;
                    } else if (what == "read") {
                        int64_t const from = argInt(a, "from", 0);
                        int64_t const to = argInt(a, "to", 1 << 30);
                        int64_t const every = std::max<int64_t>(1, argInt(a, "every", 1));
                        auto rows = matjson::Value::array();
                        size_t count = 0;
                        for (auto const& [f, s] : tickprobe::current) {
                            if ((int64_t)f < from || (int64_t)f > to || ((int64_t)f - from) % every != 0)
                                continue;
                            rows.push(fmtSample(f, s));
                            if (++count >= 400)
                                break;
                        }
                        out["rows"] = rows;
                    } else if (what == "diff") {
                        auto const& A = tickprobe::saved;
                        auto const& B = tickprobe::current;
                        int64_t compared = 0;
                        int64_t first = -1;
                        for (auto const& [f, s] : A) {
                            auto it = B.find(f);
                            if (it == B.end())
                                continue;
                            compared++;
                            auto const& t = it->second;
                            if (std::fabs(s.x - t.x) > 1e-3f || std::fabs(s.y - t.y) > 1e-3f ||
                                std::fabs(s.yVel - t.yVel) > 1e-3f || s.held != t.held ||
                                s.mode != t.mode) {
                                first = f;
                                break;
                            }
                        }
                        out["compared"] = compared;
                        out["first_difference"] = first;
                        if (first >= 0) {
                            auto around = matjson::Value::array();
                            for (int64_t f = std::max<int64_t>(0, first - 4); f <= first + 4; f++) {
                                auto ia = A.find((uint32_t)f);
                                auto ib = B.find((uint32_t)f);
                                around.push("ref " + (ia == A.end() ? std::to_string(f) + " -"
                                                                     : fmtSample((uint32_t)f, ia->second)));
                                around.push("now " + (ib == B.end() ? std::to_string(f) + " -"
                                                                     : fmtSample((uint32_t)f, ib->second)));
                            }
                            out["around"] = around;
                        }
                    } else {
                        throw ToolError("action must be start, stop, save, diff or read");
                    }
                    out["armed"] = tickprobe::armed;
                    out["recording"] = range(tickprobe::current);
                    out["reference"] = range(tickprobe::saved);
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_backwards_stepping",
            "Turn Backwards Stepping on or off (the per-tick store step-backs "
            "restore from).",
            schemaOf({{"on", "boolean", "on or off"}}),
            [](matjson::Value const& a) {
                auto* gb = GucciEngine::get();
                gb->updater.m_backwardsStepping = argBool(a, "on", true);
                auto out = obj();
                out["on"] = gb->updater.m_backwardsStepping;
                out["stored_frames"] = (int64_t)gb->practiceFix.m_storedFrames.size();
                return out;
            },
        });

        server.addTool({
            "gucci_sim_vs_real",
            "Diagnostic: play the same script from the player's current state "
            "twice -- in Absense's pathfinder simulation, then for real (the "
            "death is caught, not died) -- and report both outcomes, what killed "
            "each, and the first tick the two part. The game is put back to "
            "where it was afterwards. Script as for gucci_abs_simulate.",
            schemaOf({{"script", "string", "steps, e.g. h20 r15 p1 h40"},
                      {"ticks", "integer", "ticks to run (default: the script's length, max 1500)"},
                      {"every", "integer", "report both paths every N ticks (default 10)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto const script = parseAbsScript(argStr(a, "script"));
                int const ticks = (int)std::clamp<int64_t>(
                    argInt(a, "ticks", std::max<int64_t>(1, (int64_t)script.size())), 1, 1500);
                int const every = (int)std::clamp<int64_t>(argInt(a, "every", 10), 1, 240);
                auto out = absense::simVsReal(script, ticks, every);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto action = obj();
            action["type"] = "string";
            action["enum"] = std::vector<std::string>{"add", "clear", "list"};
            props["action"] = action;
            auto uid = obj();
            uid["type"] = "integer";
            uid["description"] = "the object's unique id (killer_uid in the other tools)";
            props["uid"] = uid;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};
            server.addTool({
                "gucci_abs_distrust",
                "Mark an object as a killer Absense's simulation is wrong about: "
                "its copies of the player no longer die of it. add (uid), clear, list.",
                schema,
                [](matjson::Value const& a) {
                    auto const what = argStr(a, "action");
                    if (what == "add") {
                        int64_t const uid = argInt(a, "uid", 0);
                        if (uid == 0)
                            throw ToolError("add needs a uid");
                        absense::distrust((int)uid);
                    } else if (what == "clear") {
                        absense::clearDistrust();
                    } else if (what != "list") {
                        throw ToolError("action must be add, clear or list");
                    }
                    auto out = obj();
                    auto list = matjson::Value::array();
                    for (int u : absense::distrustedList())
                        list.push((int64_t)u);
                    out["distrusted"] = list;
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_open_level",
            "Open one of your own (local, created) levels by its exact name, "
            "from anywhere outside a level. For Assistant Access sessions that "
            "restart the game.",
            schemaOf({{"name", "string", "the level's name, exactly"}}),
            [](matjson::Value const& a) {
                if (PlayLayer::get() || LevelEditorLayer::get())
                    throw ToolError("leave the level (or the editor) first");
                auto const name = argStr(a, "name");
                auto* llm = LocalLevelManager::get();
                GJGameLevel* found = nullptr;
                if (llm && llm->m_localLevels) {
                    for (auto* lvl : CCArrayExt<GJGameLevel*>(llm->m_localLevels)) {
                        if (lvl && std::string(lvl->m_levelName) == name) {
                            found = lvl;
                            break;
                        }
                    }
                }
                if (!found)
                    throw ToolError("no local level called \"" + name + "\"");
                auto* scene = PlayLayer::scene(found, false, false);
                CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
                auto out = obj();
                out["opening"] = name;
                return out;
            },
        });

        server.addTool({
            "gucci_quit_level",
            "Leave the level the way the pause menu's exit does (PlayLayer::onQuit).",
            schemaOf({}),
            [](matjson::Value const&) {
                auto* pl = requireLevel();
                // After this call has answered: leaving tears the level down.
                Loader::get()->queueInMainThread([pl] {
                    if (PlayLayer::get() == pl)
                        pl->onQuit();
                });
                auto out = obj();
                out["leaving"] = true;
                return out;
            },
        });

        server.addTool({
            "gucci_restart_game",
            "Save and restart Geometry Dash (so a freshly built GucciBot loads). "
            "Assistant Access comes back on by itself if it was on.",
            schemaOf({}),
            [](matjson::Value const&) {
                // After this call has answered: the restart ends the process.
                Loader::get()->queueInMainThread([] { geode::utils::game::restart(true); });
                auto out = obj();
                out["restarting"] = true;
                return out;
            },
        });

        {
            auto schema = obj();
            auto props = obj();
            auto mode = obj();
            mode["type"] = "string";
            mode["enum"] = std::vector<std::string>{"accuracy", "performance"};
            mode["description"] = "leave out to only read it";
            props["mode"] = mode;
            auto save = obj();
            save["type"] = "boolean";
            save["description"] = "also save it as the user's setting (default false: this session only)";
            props["save"] = save;
            schema["type"] = "object";
            schema["properties"] = props;
            server.addTool({
                "gucci_lock_delta",
                "Read or switch Lock Delta's mode. Accuracy drives GD one physics step "
                "per update, as recording always does; Performance hands GD several "
                "steps in one update during playback, and GD's own sub-steps do not "
                "always come out the same as single steps. By default a switch is for "
                "this session only and the saved setting is left alone.",
                schema,
                [](matjson::Value const& a) {
                    auto* gb = GucciEngine::get();
                    auto const m = argStr(a, "mode");
                    if (!m.empty()) {
                        if (m != "accuracy" && m != "performance")
                            throw ToolError("mode must be accuracy or performance");
                        gb->updater.m_lockDeltaMode = m == "accuracy" ? GucciUpdater::LockDeltaMode::Accuracy
                                                                      : GucciUpdater::LockDeltaMode::Performance;
                        if (argBool(a, "save", false))
                            Mod::get()->setSavedValue("updater_lockDeltaMode", (int)gb->updater.m_lockDeltaMode);
                    }
                    auto out = obj();
                    out["mode"] = gb->updater.m_lockDeltaMode == GucciUpdater::LockDeltaMode::Accuracy ? "accuracy"
                                                                                                        : "performance";
                    out["saved"] = Mod::get()->getSavedValue<int>("updater_lockDeltaMode", 1) == 1 ? "accuracy"
                                                                                                    : "performance";
                    out["lock_delta_on"] = gb->updater.m_lockDelta;
                    return out;
                },
            });
        }

        server.addTool({
            "gucci_run_to",
            "Let the real game run until the frame counter reaches `frame`, then "
            "pause there. Returns at once; poll gucci_get_state for paused.",
            schemaOf({{"frame", "integer", "frame to pause at (after the current one)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto* gb = GucciEngine::get();
                int64_t const target = argInt(a, "frame", -1);
                if (target <= (int64_t)gb->updater.getFrame())
                    throw ToolError("frame must be after the current frame");
                gb->updater.m_pauseAtFrame = (uint32_t)target;
                gb->updater.setPaused(false);
                auto out = obj();
                out["from"] = (int64_t)gb->updater.getFrame();
                out["to"] = target;
                return out;
            },
        });

        server.addTool({
            "gucci_simulate",
            "Fork the player and see how many of the next `frames` ticks it "
            "survives if it holds, releases, or keeps its buttons as they are. "
            "Nothing in the real game changes.",
            schemaOf({{"frames", "integer", "look-ahead in ticks (default 120)"},
                      {"player2", "boolean", "simulate player 2 (default false)"}}),
            [](matjson::Value const& a) {
                auto* pl = requireLevel();
                int const frames = (int)std::clamp<int64_t>(argInt(a, "frames", 120), 1, 2000);
                auto* player = argBool(a, "player2", false) ? pl->m_player2 : pl->m_player1;
                if (!player)
                    throw ToolError("no such player");
                auto& traj = TrajectoryPredictionService::get();
                auto out = obj();
                out["frames"] = (int64_t)frames;
                out["hold"] = (int64_t)traj.survivesFor(pl, player, frames, 1);
                out["release"] = (int64_t)traj.survivesFor(pl, player, frames, -1);
                out["as_is"] = (int64_t)traj.survivesFor(pl, player, frames, 0);
                return out;
            },
        });
    }

} // namespace gucci::mcp
