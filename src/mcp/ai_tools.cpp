// Assistant Access tools for the AI pilot (stage A, 2026-11-02): its jobs,
// queued and polled (every call returns at once), and the smoke test of its
// access to GD's physics. The AI never goes through the old search.

#include "mcp_server.hpp"

#include "ai/job.hpp"
#include "ai/smoke.hpp"

#include <Geode/Geode.hpp>

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

        int jobArg(matjson::Value const& a) {
            if (!a.contains("job")) throw ToolError("job is required (the id a gucci_ai_* tool returned)");
            return (int)intArg(a, "job", 0);
        }
    }  // namespace

    void registerAiTools(Server& server) {
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
                       {"max_ticks", "integer", "stop after this many ticks (default: the last input plus a minute)"}}),
            [](matjson::Value const& a) {
                auto const path = a.contains("macro_path") ? a["macro_path"].asString().unwrapOr("") : "";
                if (path.empty()) throw ToolError("macro_path is required");
                auto const who = runOwner();
                if (!who.empty()) throw ToolError(who + " currently owns the run; wait for it or cancel it first");
                std::string why;
                auto job = ai::makeSimSmoke(path, intArg(a, "level_id", 0), (int)intArg(a, "offset", 0),
                                            intArg(a, "max_ticks", 0), why);
                if (!job) throw ToolError(why);
                auto out = matjson::Value::object();
                out["job"] = ai::job::submit(std::move(job));
                return out;
            },
        });
    }

} // namespace gucci::mcp
