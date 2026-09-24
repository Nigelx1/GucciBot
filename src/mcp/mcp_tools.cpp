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
#include "core/GucciBot.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
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
            if (Pathfinder::get()->active)
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
            return e;
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
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};

            server.addTool({
                "gucci_pathfinder",
                "Drive the pathfinder: start a search, cancel it, or read how "
                "far it has got and what it has found.",
                schema,
                [](matjson::Value const& a) {
                    auto* pf = Pathfinder::get();
                    auto const what = argStr(a, "action");
                    auto out = obj();

                    if (what == "start") {
                        requireFreeRun();
                        requireLevel();
                        pf->begin();
                        out["started"] = pf->active;
                        return out;
                    }
                    if (what == "cancel") {
                        pf->cancel();
                        out["cancelled"] = true;
                        return out;
                    }
                    if (what == "status") {
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
    }

} // namespace gucci::mcp
