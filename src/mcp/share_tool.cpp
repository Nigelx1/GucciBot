// GucciBot (2026-11-15): gucci_share, Share my windows (ui/share.hpp) for a
// test launch. The button itself is pressed with gucci_ui_press, as a person
// would; this answers what came of it (the picture's path, how long it took,
// or why it failed) and why the button is greyed out, so a test can find the
// file and read it. start: makes one the way the button does (the same
// call), for a test that can't reach the button. import: hands a file to
// Saved macros' Import a macro (ui::importMacroFromFile, the code the file
// picker's result goes through), for the shared .zip's round trip.

#include "mcp_server.hpp"

#include "hacks/safemode.hpp"
#include "core/GucciBot.hpp"
#include "ui/share.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <filesystem>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    void registerShareTools(Server& server) {
        auto props = matjson::Value::object();
        auto start = matjson::Value::object();
        start["type"] = "boolean";
        start["description"] = "make a picture now, as the Share my windows button does";
        props["start"] = start;
        auto import = matjson::Value::object();
        import["type"] = "string";
        import["description"] = "a file to import, as Saved macros' Import a macro does with the file picked";
        props["import"] = import;
        auto schema = matjson::Value::object();
        schema["type"] = "object";
        schema["properties"] = props;

        server.addTool({
            "gucci_share",
            "Share my windows (Calculate card), test launches only. Answers why_not (why the button is greyed out; "
            "empty when it can share), busy, done, ok, path (the PNG, in shares/ in GucciBot's save folder), "
            "error, saved (pictures this launch), seconds (the last one, start to file; game_ms of it on the game's thread) and what it shows (level, "
            "details, score, bands, tight), and the macro file next to it: bundle_ok, bundle_path (the .zip), "
            "bundle_error (why there's none). start: makes one now "
            "through the button's own call; started is empty, or why it didn't. import: a file path handed to "
            "Import a macro's own code; answers import_ok, import_note and the macro selected after (replay_name).",
            schema,
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                auto out = matjson::Value::object();
                if (a.contains("start") && a["start"].asBool().unwrapOr(false))
                    out["started"] = ui::share::start();
                if (a.contains("import")) {
                    auto const p = a["import"].asString().unwrapOr("");
                    auto const [ok, note] = ui::importMacroFromFile(std::filesystem::path(std::u8string(p.begin(), p.end())));
                    out["import_ok"] = ok;
                    out["import_note"] = note;
                    out["replay_name"] = GucciEngine::get()->replayName;
                }
                auto const& s = ui::share::status();
                out["why_not"] = ui::share::whyNot();
                out["busy"] = s.busy;
                out["done"] = s.done;
                out["ok"] = s.ok;
                out["path"] = s.path;
                out["error"] = s.error;
                out["bundle_ok"] = s.bundleOk;
                out["bundle_path"] = s.bundlePath;
                out["bundle_error"] = s.bundleError;
                out["saved"] = s.saved;
                out["seconds"] = s.seconds;
                out["game_ms"] = s.gameMs;
                out["level"] = s.level;
                out["details"] = s.details;
                out["warning"] = s.warning;
                out["score"] = s.score;
                auto bands = matjson::Value::array();
                for (auto const& b : s.bands)
                    bands.push(b);
                out["bands"] = bands;
                auto tight = matjson::Value::array();
                for (auto const& t : s.tight)
                    tight.push(t);
                out["tight"] = tight;
                return out;
            },
        });
    }

} // namespace gucci::mcp
