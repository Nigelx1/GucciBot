// Assistant Access tools for the AI pilot (stage A, 2026-11-02): its jobs,
// queued and polled (every call returns at once), and the smoke test of its
// access to GD's physics. The AI never goes through the old search.

#include "mcp_server.hpp"

#include "ai/job.hpp"
#include "ai/observe.hpp"
#include "ai/pilot.hpp"
#include "ai/record.hpp"
#include "ai/rollout.hpp"
#include "ai/smoke.hpp"
#include "ai/tape.hpp"

#include <Geode/Geode.hpp>

#include <cctype>
#include <stdexcept>

namespace gucci::mcp {

    namespace {
        matjson::Value schemaFor(std::initializer_list<std::array<char const*, 3>> fields) {
            auto props = matjson::Value::object();
            for (auto const& f : fields) {
                auto p = matjson::Value::object();
                p["type"] = f[1];
                p["description"] = f[2];
                props[f[0]] = p;
            }
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            return schema;
        }

        int64_t intArg(matjson::Value const& a, char const* key, int64_t def) {
            if (!a.contains(key)) return def;
            if (auto i = a[key].asInt()) return *i;
            if (auto d = a[key].asDouble()) return (int64_t)*d;
            return def;
        }

        // Another Assistant Access tool, called from an AI job on the main
        // thread (opening and leaving levels): a ToolError comes back as
        // std::runtime_error.
        ai::ToolCall toolCaller(Server& server) {
            return [&server](char const* name, matjson::Value const& args) -> matjson::Value {
                for (auto const& t : server.tools()) {
                    if (t.name != name) continue;
                    try {
                        return t.run(args);
                    } catch (ToolError const& e) {
                        throw std::runtime_error(e.message);
                    }
                }
                throw std::runtime_error(std::string("no tool ") + name);
            };
        }

        std::string pathArg(matjson::Value const& a) {
            auto const path = a.contains("macro_path") ? a["macro_path"].asString().unwrapOr("") : "";
            if (path.empty()) throw ToolError("macro_path is required");
            return path;
        }

        void requireNoOwner() {
            auto const who = runOwner();
            if (!who.empty()) throw ToolError(who + " currently owns the run; wait for it or cancel it first");
        }

        int jobArg(matjson::Value const& a) {
            if (!a.contains("job")) throw ToolError("job is required (the id a gucci_ai_* tool returned)");
            return (int)intArg(a, "job", 0);
        }
    }  // namespace

    void registerAiTools(Server& server) {
        server.addTool({
            "gucci_ai_layout",
            "What the AI pilot sees and how it is laid out (AI contract v2 section 3): the per-tick record's "
            "state slots, scan, token table and meta, N (472), obs_layout_hash (networks carry it), and the level "
            "tape's layout and tape_layout_hash.",
            schemaFor({}),
            [](matjson::Value const&) {
                auto out = ai::obs::layoutJson();
                auto t = ai::tape::layoutJson();
                for (auto const& [k, v] : t) out[k] = v;
                out["ai_root"] = std::string(reinterpret_cast<char const*>(ai::aiRoot().u8string().c_str()));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_record",
            "Record a human macro (any format the converter reads) as AI training data: the level is opened if "
            "needed (online levels download), the macro played in the AI's own session of GD's physics from the "
            "level's start at the macro's tick rate, for each frame offset until one completes (default 0, 1, -1; "
            "Silicate files 0 only: their +1 is in the converter), then the completing (or furthest) offset played "
            "again with the observer on and written to AI_ROOT\\data (contract section 4, golden rows included) "
            "with the level's tape if it has none. Returns {job}; poll gucci_ai_status.",
            schemaFor({{"macro_path", "string", "full path of the macro file"},
                       {"level_id", "integer", "the level it is for (main levels 1-22 by number)"},
                       {"offsets", "array", "frame offsets to try in order (optional)"},
                       {"tps", "number", "tick rate (default: the macro's)"}}),
            [&server](matjson::Value const& a) {
                auto const path = pathArg(a);
                requireNoOwner();
                std::vector<int> offsets;
                bool given = false;
                if (a.contains("offsets") && a["offsets"].isArray()) {
                    given = true;
                    for (auto const& v : a["offsets"]) offsets.push_back((int)v.asInt().unwrapOr(0));
                    if (offsets.empty()) throw ToolError("offsets is empty");
                }
                double tps = 0.0;
                if (a.contains("tps")) tps = a["tps"].asDouble().unwrapOr(0.0);
                std::string why;
                auto job = ai::makeRecord(path, intArg(a, "level_id", 0), std::move(offsets), given, tps,
                                          toolCaller(server), why);
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_validate_real",
            "Play a macro in the real game to check it: it is converted, shifted by frame_offset, saved as a "
            "zz_gbtest_ GucciBot macro, loaded and played by GucciBot's own playback from the level's start (safe "
            "mode must be on; the level is opened if needed), in real time. Result: {completed, died_x, percent}. "
            "The copy is deleted after and the mode set back to idle (the loaded macro is replaced). Returns {job}.",
            schemaFor({{"macro_path", "string", "full path of the macro file"},
                       {"level_id", "integer", "the level it is for"},
                       {"frame_offset", "integer", "frames added to every input (default 0)"}}),
            [&server](matjson::Value const& a) {
                auto const path = pathArg(a);
                requireNoOwner();
                std::string why;
                auto job = ai::makeValidateReal(path, intArg(a, "level_id", 0), (int)intArg(a, "frame_offset", 0),
                                                toolCaller(server), why);
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_find_level",
            "Search GD's servers for a level by name: {candidates: [{id, name, author, stars, demon, length, "
            "exact_name}]} (the first page of GD's own search). The caller decides; an ambiguous name is skipped, "
            "not guessed. Returns {job}.",
            schemaFor({{"name", "string", "the level's name"}}),
            [](matjson::Value const& a) {
                auto const name = a.contains("name") ? a["name"].asString().unwrapOr("") : "";
                std::string why;
                auto job = ai::makeFindLevel(name, why);
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_reload",
            "Load the AI's networks again: pilot.gbnn, critic.gbnn and reader.gbnn from AI_ROOT\\weights (else the "
            "copies shipped with the mod in resources/ai; none are shipped yet). A network trained for another "
            "layout is refused, and the answer says which layout it was trained for. Returns {models: [{name, loaded, "
            "path, layout_hash, inputs, outputs, error}], pilot_loaded, critic_loaded, reader_loaded, obs_layout_hash, "
            "tape_layout_hash, weights_dir, summary}.",
            schemaFor({}),
            [](matjson::Value const&) {
                if (ai::job::busy()) throw ToolError("an AI job is running; wait for it or cancel it first");
                return ai::pilot::reload();
            },
        });

        server.addTool({
            "gucci_ai_play",
            "The AI pilot plays the open level in its own session of GD's physics: the real game is restarted and "
            "held at its start, then the pilot plays tick by tick. By default (practice false) the network alone "
            "plays: attempt 1 is its greedy best guess, later attempts start over from the level's start and sample "
            "from its own odds at `temperature`; max_attempts 1 is a cold exam. With practice true (the old "
            "hand-written practice rules, kept for comparison only): on each death it names the cause, "
            "picks the decision most likely to be the mistake (the critic's advantage, or without a critic the "
            "pilot's least-sure decisions near its last presses and releases, most recent first), changes it and "
            "goes again from a saved state just before it. A finished run is saved as a GucciBot macro and played "
            "in the real game (the judge) to confirm it. Never uses the old search. Returns {job}; the result "
            "(gucci_ai_status) is {completed, best_percent, attempts, ticks_simulated, deaths: [{tick, x, percent, "
            "cause, mistake_tick, mistake_was, fix, sentence}], macro_file?, judge?: {completed, died_x}}.",
            schemaFor({{"max_attempts", "integer", "stop after this many attempts (default 50000)"},
                       {"max_seconds", "number", "stop after this many seconds (default 300)"},
                       {"save_macro", "string", "name of the macro a finished run is saved as (default AI - <level>)"},
                       {"log_practice", "boolean", "write practice logs to AI_ROOT\\practice (default false)"},
                       {"judge", "boolean", "play a finished run in the real game to confirm it (default true)"},
                       {"practice", "boolean", "use the old hand-written practice rules (default false: the network alone)"},
                       {"temperature", "number", "network alone: sampling temperature of attempts after the first (default 1)"}}),
            [](matjson::Value const& a) {
                requireNoOwner();
                ai::pilot::PlayOptions o;
                if (a.contains("practice")) o.practice = a["practice"].asBool().unwrapOr(false);
                if (a.contains("temperature")) o.temperature = a["temperature"].asDouble().unwrapOr(1.0);
                o.maxAttempts = (int)intArg(a, "max_attempts", o.maxAttempts);
                if (a.contains("max_seconds")) o.maxSeconds = a["max_seconds"].asDouble().unwrapOr(o.maxSeconds);
                if (a.contains("save_macro")) o.saveMacro = a["save_macro"].asString().unwrapOr("");
                if (a.contains("log_practice")) o.logPractice = a["log_practice"].asBool().unwrapOr(false);
                if (a.contains("judge")) o.judge = a["judge"].asBool().unwrapOr(true);
                std::string why;
                auto job = ai::pilot::makePlay(o, why);
                if (!job) throw ToolError(why);
                int const id = ai::job::submit(std::move(job));
                ai::pilot::setLiveJob(id);
                auto out = matjson::Value::object();
                out["job"] = id;
                return out;
            },
        });

        server.addTool({
            "gucci_ai_rollout",
            "Reinforcement learning experience: the pilot plays the open level in the AI's own session of GD's "
            "physics for `seconds`, sampling hold or release each tick from its own probability (at `temperature`). "
            "Episodes start at the level's start, at a point along the human run of the level (AI_ROOT\\data, "
            "replayed in the copies) or at the furthest point reached so far (start_mix {start, human, frontier}); "
            "an episode ends at a death, the level's end or max_episode_seconds (truncated). Reward per tick: "
            "progress (a whole level about +10), death -1, finishing +2. Every tick is written to "
            "AI_ROOT\\rl\\rollouts\\<tag>_<n>.* for tools/ai/rl_ppo.py. Returns {job}; the result has ticks, "
            "episodes, mean_return, mean_progress, ticks_per_second, files.",
            schemaFor({{"level_id", "integer", "the level it is for (checked against the open level; 0 skips)"},
                       {"seconds", "number", "working time, wall clock (default 60)"},
                       {"max_ticks", "integer", "stop after this many ticks (default 400000)"},
                       {"temperature", "number", "sampling temperature (default 1)"},
                       {"seed", "integer", "random seed (default 1)"},
                       {"max_episode_seconds", "number", "game seconds before an episode is cut (default 30)"},
                       {"start_mix", "object", "{start, human, frontier} weights (default 0.4, 0.4, 0.2)"},
                       {"tag", "string", "file name prefix (default rollout)"},
                       {"slice_ms", "number", "AI work per frame while it runs, ms (default: unchanged, 10)"}}),
            [](matjson::Value const& a) {
                requireNoOwner();
                ai::rollout::Options o;
                o.levelId = intArg(a, "level_id", 0);
                if (a.contains("seconds")) o.seconds = a["seconds"].asDouble().unwrapOr(o.seconds);
                o.maxTicks = intArg(a, "max_ticks", o.maxTicks);
                if (a.contains("temperature")) o.temperature = a["temperature"].asDouble().unwrapOr(1.0);
                o.seed = (uint64_t)intArg(a, "seed", 1);
                if (a.contains("max_episode_seconds"))
                    o.maxEpisodeSeconds = a["max_episode_seconds"].asDouble().unwrapOr(o.maxEpisodeSeconds);
                if (a.contains("start_mix") && a["start_mix"].isObject()) {
                    auto const& m = a["start_mix"];
                    o.mixStart = m.contains("start") ? m["start"].asDouble().unwrapOr(0.0) : 0.0;
                    o.mixHuman = m.contains("human") ? m["human"].asDouble().unwrapOr(0.0) : 0.0;
                    o.mixFrontier = m.contains("frontier") ? m["frontier"].asDouble().unwrapOr(0.0) : 0.0;
                }
                if (a.contains("tag")) o.tag = a["tag"].asString().unwrapOr(o.tag);
                for (char& c : o.tag)
                    if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
                if (o.tag.empty()) o.tag = "rollout";
                if (a.contains("slice_ms")) o.sliceMs = a["slice_ms"].asDouble().unwrapOr(0.0);
                std::string why;
                auto job = ai::rollout::make(o, why);
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_status",
            "The state of an AI job (queued, running, done or failed), its progress, and its result or error. "
            "Long AI work runs as a job in 10 ms slices a frame; poll this until it is done or failed.",
            schemaFor({{"job", "integer", "the job id"}}),
            [](matjson::Value const& a) {
                try {
                    return ai::job::status(jobArg(a));
                } catch (std::out_of_range const&) {
                    throw ToolError("no such job (finished jobs are kept for a while, then forgotten)");
                }
            },
        });

        server.addTool({
            "gucci_ai_cancel",
            "Stop a queued or running AI job; it puts back what it changed (the session, the tick rate, the "
            "paused state).",
            schemaFor({{"job", "integer", "the job id"}}),
            [](matjson::Value const& a) {
                auto out = matjson::Value::object();
                out["cancelled"] = ai::job::cancel(jobArg(a));
                return out;
            },
        });

        server.addTool({
            "gucci_ai_sim_smoke",
            "Play a macro of any format (read through the converter) in the AI's own session of GD's physics "
            "from the level's start: the real game is restarted and held at its first tick, the start captured, "
            "and the inputs stepped tick by tick in the simulated copies. Also checks snapshot/restore (back to "
            "the last snapshot, same inputs, the same end to the bit). Returns {job}; the result (gucci_ai_status) "
            "is {ticks, completed, died_tick, died_x, final_x, ...}. The level must be open; GucciBot idle.",
            schemaFor({{"macro_path", "string", "full path of the macro file"},
                       {"level_id", "integer", "the level it is for (checked against the open level; 0 skips)"},
                       {"offset", "integer", "frames added to every input (default 0)"},
                       {"max_ticks", "integer", "stop after this many ticks (default: the last input plus a minute)"},
                       {"probe", "boolean", "every tick of both copies into gucci_tick_probe's reference (diff it "
                                            "against a real replay recorded with gucci_tick_probe start)"}}),
            [](matjson::Value const& a) {
                auto const path = a.contains("macro_path") ? a["macro_path"].asString().unwrapOr("") : "";
                if (path.empty()) throw ToolError("macro_path is required");
                auto const who = runOwner();
                if (!who.empty()) throw ToolError(who + " currently owns the run; wait for it or cancel it first");
                std::string why;
                auto job = ai::makeSimSmoke(path, intArg(a, "level_id", 0), (int)intArg(a, "offset", 0),
                                            intArg(a, "max_ticks", 0), why,
                                            a.contains("probe") && a["probe"].asBool().unwrapOr(false));
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });
    }

} // namespace gucci::mcp
