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

#include "core/platform.hpp"
#include "mcp_server.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"
#include "analysis/pathfinder.hpp"
#include "absense/glue.hpp"
#include "audio/bigbrrr.hpp"
#include "mcp/screenshot.hpp"
#include "mcp/test_settings.hpp"
#include "mcp/tickprobe.hpp"
#include "analysis/trajectory.hpp"
#include "tools/macro_check.hpp"
#include "core/GucciBot.hpp"
#include "hacks/safemode.hpp"
#include "render/renderer.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>
#include <sstream>
#include <fstream>
#include <chrono>
#include <unordered_map>

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
        // GucciBot (2026-10-24): t<N> = a tap inside each of N ticks: pressed
        // and let go again before the tick's physics, so the button is up when
        // the player moves - the input the pathfinder's taps and its UFO moves
        // give (docs/AI_KNOWS_THE_GAME_PLAN.md step 1: "t1 r20" against "p1 r20"
        // says whether the game acts on such a press).
        std::vector<absense::ScriptTick> parseAbsScript(std::string const& text) {
            std::vector<absense::ScriptTick> held;
            std::istringstream in(text);
            std::string step;
            while (in >> step) {
                if (step.size() < 2 || (step[0] != 'h' && step[0] != 'r' && step[0] != 'p' && step[0] != 't'))
                    throw ToolError("script steps are h<N>, r<N>, p<N> or t<N>, got \"" + step + "\"");
                int n = 0;
                try {
                    n = std::stoi(step.substr(1));
                } catch (...) {
                    throw ToolError("bad tick count in \"" + step + "\"");
                }
                if (n < 1 || n > 3000)
                    throw ToolError("tick counts are 1 to 3000");
                // p and t press; h and p leave the button down after the tick.
                held.insert(held.end(), (size_t)n,
                            absense::ScriptTick{step[0] == 'p' || step[0] == 't', step[0] == 'h' || step[0] == 'p'});
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
            "build, mode, frame, level, player position, who owns the run, and "
            "safe_mode (switch: Hacks > Safe mode; forced: on for this launch for "
            "a test run, by GUCCIBOT_FORCE_SAFE_MODE or gucci_force_safe_mode; "
            "auto: auto safe mode; bot_runs_count: false when a macro, Calculate "
            "or pathfinder run can't save progress or stats; cheating_now: "
            "something on right now counts as a cheat; tainted: this attempt had "
            "one on). Also level.completed (the level's end reached this "
            "attempt), audio (BIG BRRRR enabled/playing/track/holds_game_audio, "
            "FMOD's master volume and pitch, the game's music and sfx volumes, "
            "test_mute) and test_settings (the keys gucci_test_set holds).",
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
                {
                    // GucciBot (2026-10-23): automated runs only with one of these
                    // on (they play on the owner's real account).
                    auto sm = obj();
                    sm["switch"] = gb->protectedMode;
                    sm["forced"] = safemode::forced();
                    sm["auto"] = safemode::autoSafeMode();
                    sm["bot_runs_count"] =
                        !gb->protectedMode && !safemode::forced() && !safemode::autoSafeMode();
                    sm["cheating_now"] = safemode::cheatingNow();
                    sm["tainted"] = safemode::tainted();
                    out["safe_mode"] = sm;
                }
                {
                    // GucciBot (2026-11-01): what's making sound, for the test
                    // batches that mute the game (gucci_test_set test_mute).
                    auto audio = obj();
                    auto* brrr = BigBrrrManager::get();
                    auto big = obj();
                    big["enabled"] = brrr->enabled;
                    big["playing"] = brrr->playing();
                    big["track"] = brrr->trackFile();
                    big["holds_game_audio"] = brrr->holdsGameAudio();
                    audio["bigbrrr"] = big;
                    if (auto* fmod = FMODAudioEngine::sharedEngine()) {
                        FMOD::ChannelGroup* master = nullptr;
                        if (fmod->m_system && fmod->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
                            float volume = 0.f, pitch = 0.f;
                            master->getVolume(&volume);
                            master->getPitch(&pitch);
                            audio["master_volume"] = (double)volume;
                            audio["master_pitch"] = (double)pitch;
                        }
                        audio["music_volume"] = (double)fmod->getBackgroundMusicVolume();
                        audio["sfx_volume"] = (double)fmod->getEffectsVolume();
                    }
                    audio["test_mute"] = testset::muted();
                    out["audio"] = audio;
                }
                out["test_settings"] = testset::heldKeys();

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
                    lvl["completed"] = pl->m_hasCompletedLevel;
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

        // GucciBot (2026-10-31): for a test run on a game started without
        // GUCCIBOT_FORCE_SAFE_MODE (hacks/safemode.hpp). Never refuses: making
        // a run count for less is safe whoever owns it.
        server.addTool({
            "gucci_force_safe_mode",
            "Turn safe mode on until the game closes, whatever the Safe mode switch "
            "says: no run counts, cheat or not. Never saved, and it has no off. "
            "tools/assist.py's launch() starts the game with "
            "GUCCIBOT_FORCE_SAFE_MODE=1 instead, which does the same from the first "
            "frame.",
            schemaOf({}),
            [](matjson::Value const&) {
                safemode::forceForSession();
                auto out = obj();
                out["forced"] = safemode::forced();
                return out;
            },
        });

        // GucciBot (2026-11-01): a test's settings, never saved
        // (mcp/test_settings.hpp has how).
        {
            auto schema = schemaOf({{"key", "string", "the setting's saved key, e.g. hack_force_platformer"},
                                    {"reset", "boolean", "true: put every setting a test changed back"},
                                    {"list", "boolean", "true: every key it takes, its value now, and which it holds"}});
            auto value = obj();
            value["description"] = "the value, as the key's type: true/false, a number or text";
            schema["properties"]["value"] = value;
            server.addTool({
                "gucci_test_set",
                "Put one whitelisted setting into effect for this launch only, for a test "
                "run: {key, value}. It goes through the setter the menu row uses (the "
                "Tick rate row also records a TPS change while recording), and is "
                "refused unless safe mode is forced for the launch. Never saved: every "
                "write of saved.json puts the user's own value under each key it holds "
                "(or leaves the key out, as it was). {reset: true} puts every one back, "
                "in memory too; tools/assist.py's close() does that first. {list: true} "
                "returns every key with its type and value now. Keys that were never "
                "saved anyway: hack_rng_lock, hack_rng_seed, bigbrrr (this launch's "
                "switch), test_mute (FMOD's master volume 0) and fwac_conflict_test_id "
                "(one more mod id for Calculate's conflict check). A change made in "
                "the menu to a key this holds isn't saved this launch either.",
                schema,
                [](matjson::Value const& a) {
                    if (argBool(a, "list", false))
                        return testset::list();
                    if (argBool(a, "reset", false))
                        return testset::reset();
                    if (!a.contains("key") || !a.contains("value"))
                        throw ToolError("give key and value, or reset: true, or list: true");
                    // Not under a run that owns the game: a rate or a render
                    // folder changing under it is a test of nothing.
                    requireFreeRun();
                    return testset::set(argStr(a, "key"), a["value"]);
                },
            });
        }

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

        // GucciBot (2026-10-31): the window as a PNG (mcp/screenshot.hpp), so
        // a session can see the menu and the level for itself.
        server.addTool({
            "gucci_screenshot",
            "Save what the game window shows (the level, the menu, other mods' "
            "overlays) as a PNG: screenshots/<name>.png in GucciBot's save folder. "
            "It's taken `frames` frames from now (default 3: a window that has just "
            "opened is hidden for its first frame), so this returns {pending: true, "
            "path} at once; then call it with status: true until done is true "
            "(done, ok, error, path, width, height, ms). A minimized game draws "
            "nothing, so its shot waits. Windows only for now.",
            schemaOf({{"name", "string", "the file's name: 1 to 64 letters, digits, - or _, no extension"},
                      {"frames", "integer", "frames from now, 1 to 30 (default 3)"},
                      {"status", "boolean", "true: how the last shot went, instead of taking one"}}),
            [](matjson::Value const& a) -> matjson::Value {
#ifdef GEODE_IS_WINDOWS
                auto out = obj();
                if (argBool(a, "status", false)) {
                    auto const s = shot::status();
                    out["done"] = s.done;
                    out["ok"] = s.ok;
                    out["error"] = s.asked ? s.error : std::string("no screenshot asked for this launch");
                    out["pending"] = s.pending;
                    out["frames_left"] = (int64_t)s.framesLeft;
                    out["path"] = geode::utils::string::pathToString(s.path);
                    out["width"] = (int64_t)s.width;
                    out["height"] = (int64_t)s.height;
                    out["ms"] = s.ms;
                    return out;
                }
                // A bare name, as gucci_read_log's: nothing outside the folder.
                auto const name = argStr(a, "name");
                bool bare = !name.empty() && name.size() <= 64;
                for (char const c : name)
                    bare = bare && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                    c == '-' || c == '_');
                if (!bare)
                    throw ToolError("name must be 1 to 64 letters, digits, - or _ (no path, no extension)");
                int64_t const frames = argInt(a, "frames", 3);
                if (frames < 1 || frames > 30)
                    throw ToolError("frames is 1 to 30");

                auto const dir = Mod::get()->getSaveDir() / "screenshots";
                std::error_code ec;
                fs::create_directories(dir, ec);
                if (ec)
                    throw ToolError("couldn't make the screenshots folder: " + ec.message());
                auto const path = dir / (name + ".png");
                shot::request(path, (int)frames);
                out["pending"] = true;
                out["path"] = geode::utils::string::pathToString(path);
                return out;
#else
                (void)a;
                throw ToolError("Windows only for now");
#endif
            },
        });

        // GucciBot (2026-10-31): the menu set up for a screenshot. Its theme
        // and Animations are a test's, never saved (ui.hpp, setTestTheme).
        server.addTool({
            "gucci_menu",
            "Set the menu up for a screenshot (gucci_screenshot). Every argument is "
            "optional: `open` shows or hides it; `page` shows a page by its title in "
            "the sidebar (Settings, JMF Trainer...); `theme` puts on a theme by name, "
            "built-in or custom, as a test that is never saved: theme \"\" puts the "
            "user's own theme and Animations back, and so does a restart; `effects` "
            "sets the Animations switch the same way (off holds the moving backdrops "
            "and a pulsing accent still, so shots compare). Returns open, page, pages (the sidebar's, in "
            "order), theme {id, name, custom}, themes (every name), effects, test "
            "{theme, effects} and compact (with Compact mode on, its small window "
            "shows instead of the pages).",
            schemaOf({{"open", "boolean", "show (true) or hide (false) the menu"},
                      {"page", "string", "a page's title as the sidebar shows it"},
                      {"theme", "string", "a theme's name, built-in or custom; \"\" puts the user's own back"},
                      {"effects", "boolean", "the Animations switch, for this test only"}}),
            [](matjson::Value const& a) {
                auto list = [](std::vector<std::string> const& names) {
                    std::string out;
                    for (auto const& n : names)
                        out += (out.empty() ? "" : ", ") + n;
                    return out;
                };
                // Every argument's type first, so a wrong one changes nothing.
                for (char const* key : {"open", "effects"})
                    if (a.contains(key) && !a[key].isBool())
                        throw ToolError(std::string(key) + " is true or false");
                for (char const* key : {"page", "theme"})
                    if (a.contains(key) && !a[key].isString())
                        throw ToolError(std::string(key) + " is a name (a string)");
                // As the menu key: the menu stays shut while GucciBot stands down.
                if (argBool(a, "open", false) && GucciEngine::get()->standingDown)
                    throw ToolError("GucciBot is standing down (another bot is enabled), so its menu stays shut");

                // The theme and the page are looked up before anything changes,
                // the page in the sidebar as it will be under the theme asked
                // for (GBot's has no Themes, JMF Trainer or Credits), so a
                // wrong name leaves it all as it was.
                std::optional<std::string> themeName;
                if (a.contains("theme")) {
                    themeName = argStr(a, "theme");
                    if (!themeName->empty() && !ui::hasTheme(*themeName))
                        throw ToolError("no theme called \"" + *themeName +
                                        "\". Themes: " + list(ui::menuState().themes));
                }
                std::string const* const themeAsked = themeName ? &*themeName : nullptr;
                if (a.contains("page")) {
                    auto const title = argStr(a, "page");
                    if (!ui::listsPage(title, themeAsked))
                        throw ToolError("no page called \"" + title + "\" in the sidebar. Pages: " +
                                        list(ui::pageTitlesWith(themeAsked)));
                }

                if (themeName)
                    ui::setTestTheme(*themeName);
                if (a.contains("effects"))
                    ui::setTestEffects(argBool(a, "effects", true));
                // After the theme: one can take a page out of the sidebar.
                if (a.contains("page"))
                    ui::setPageByTitle(argStr(a, "page"));
                if (a.contains("open"))
                    ui::setOpen(argBool(a, "open", true));

                auto const s = ui::menuState();
                auto out = obj();
                out["open"] = s.open;
                out["page"] = s.page;
                out["pages"] = s.pages;
                auto theme = obj();
                theme["id"] = (int64_t)s.themeId;
                theme["name"] = s.themeName;
                theme["custom"] = s.themeId == ui::kCustomThemeId;
                out["theme"] = theme;
                out["themes"] = s.themes;
                out["effects"] = s.effects;
                auto test = obj();
                test["theme"] = s.testTheme;
                test["effects"] = s.testEffects;
                out["test"] = test;
                out["compact"] = s.compact;
                return out;
            },
        });

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
                            // Calculate's count stopped at Max Window: the
                            // window is at least `window` (2026-10-23).
                            if (mk.wide && !mk.manual)
                                e["wide"] = true;
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
            auto exactFlags = obj();
            exactFlags["type"] = "boolean";
            exactFlags["description"] =
                "Absense's engine: turn the Pathfinder tab's \"Exact start flags\" switch (on by default) on or off "
                "before starting (saved, as the tab's switch is; default: leave it as it is). A search takes it when "
                "it starts: on, a run from an earlier tick sees each portal, orb, pad and trigger used or not as the "
                "game had it at that tick; off, everything ahead of the player there counts as unused.";
            props["exact_start_flags"] = exactFlags;
            auto remember = obj();
            remember["type"] = "boolean";
            remember["description"] =
                "Absense's engine: turn the Pathfinder tab's \"Remember solutions\" switch on or off before starting "
                "(saved; default: leave it as it is). Off (the default), a search is cold: nothing from earlier runs "
                "of this level is used or saved.";
            props["remember_solutions"] = remember;
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
                            if (a.contains("exact_start_flags"))
                                absense::setExactStartFlags(argBool(a, "exact_start_flags", false));
                            out["exact_start_flags"] = absense::exactStartFlags();
                            if (a.contains("remember_solutions"))
                                absense::setRememberSolutions(argBool(a, "remember_solutions", false));
                            out["remember_solutions"] = absense::rememberSolutions();
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
                            // The running search's exact start flags, and the
                            // switch the next start takes.
                            ab["exact_start_flags"] = st.exactStartFlags;
                            ab["exact_start_flags_switch"] = absense::exactStartFlags();
                            ab["remember_solutions_switch"] = absense::rememberSolutions();
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
            // GitHub issue #32: what the menu's Mode switch does now, for a
            // session to test it. Off by default, so scripts that switch to
            // record during playback get what they always did.
            auto fromHere = obj();
            fromHere["type"] = "boolean";
            fromHere["description"] = "record only: while a macro plays in a level, carry on from the current frame "
                                      "as the menu's Mode switch does (the rest of the macro is cut, held buttons "
                                      "let go). Default false: only the mode changes";
            props["from_here"] = fromHere;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"mode"};

            server.addTool({
                "gucci_set_mode",
                "Put GucciBot into idle, record or play. With from_here, record during playback carries on from "
                "the current frame, as the menu's Mode switch does.",
                schema,
                [](matjson::Value const& a) {
                    requireFreeRun();
                    auto* gb = GucciEngine::get();
                    auto const m = argStr(a, "mode");
                    if (m == "idle")
                        gb->setMode(GucciEngine::Mode::Idle);
                    else if (m == "record" && argBool(a, "from_here", false))
                        gb->setModeFromMenu(GucciEngine::Mode::Recording);
                    else if (m == "record")
                        gb->setMode(GucciEngine::Mode::Recording);
                    else if (m == "play")
                        gb->setMode(GucciEngine::Mode::Playing);
                    else
                        throw ToolError("mode must be idle, record or play");

                    auto out = obj();
                    out["mode"] = modeName(gb->mode);
                    out["actions"] = (int64_t)gb->replay.m_actionAtom.length();
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
                    // As the menu: a click on a saved macro does nothing while
                    // recording (the recording would be lost).
                    if (GucciEngine::get()->isRecording())
                        throw ToolError("stop recording first");
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
                    // The name too, as the menu's load does: the Macro page,
                    // Save and Calculate's results file go by it, and kept
                    // naming the macro loaded before.
                    GucciEngine::get()->replayName = want;

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
            "two-player / dual, time warp, practice mode, and the level's saved "
            "stats as GD holds them now (attempts, jumps, normal_percent, "
            "practice_percent), so a test can check a run under safe mode left "
            "them alone.",
            obj(),
            [](matjson::Value const&) {
                auto* pl = requireLevel();
                auto* gb = GucciEngine::get();
                auto out = obj();
                if (pl->m_level) {
                    out["name"] = std::string(pl->m_level->m_levelName);
                    out["id"] = (int64_t)pl->m_level->m_levelID.value();
                    out["attempts"] = (int64_t)pl->m_level->m_attempts.value();
                    out["jumps"] = (int64_t)pl->m_level->m_jumps.value();
                    out["normal_percent"] = (int64_t)pl->m_level->m_normalPercent.value();
                    out["practice_percent"] = (int64_t)pl->m_level->m_practicePercent;
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

        // GucciBot (2026-10-23): which portals, orbs and pads in a stretch of
        // the level are marked used, and by which player, so a session can
        // check what a respawn or the pathfinder's way back leaves behind (The
        // Nightmare's yellow portal at x 21255 did not fire after going back).
        // GucciBot (2026-10-24): x and y are where the object really is
        // (m_positionX/Y, what a move writes and the collision pass uses);
        // sprite_x / sprite_y are its sprite, which GD's enter effects slide
        // near the screen's edge - reading those made The Nightmare's blue
        // portal look like it moved when it never had.
        server.addTool({
            "gucci_used_objects",
            "Objects a player activates (portals, orbs, pads, coins, triggers) in "
            "a stretch of the level, with their used flags: uid, object id, type, "
            "x, y (where the object really is, m_positionX/Y), sprite_x, sprite_y "
            "(its sprite, which enter effects slide near the screen's edge), p1 "
            "(used by player 1), p2 (used by the other player), multi "
            "(can be activated again) and contact (player 1's and player 2's "
            "entries in the level's contact map, -1 for none). from_x/to_x "
            "default to 100 behind player 1 and 2000 ahead; only_used (default "
            "false) lists just the used ones. At most 200 are listed.",
            schemaOf({{"from_x", "integer", "left edge of the stretch"},
                      {"to_x", "integer", "right edge of the stretch"},
                      {"only_used", "boolean", "only the objects marked used (default false)"}}),
            [](matjson::Value const& a) {
                auto* pl = requireLevel();
                float const px = pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
                float const from = (float)argInt(a, "from_x", (int64_t)px - 100);
                float const to = (float)argInt(a, "to_x", (int64_t)px + 2000);
                bool const onlyUsed = argBool(a, "only_used", false);
                auto const& ids = pl->m_gameState.m_activatedObjectIDs;
                int const uid1 = pl->m_player1 ? pl->m_player1->m_uniqueID : -1;
                int const uid2 = pl->m_player2 ? pl->m_player2->m_uniqueID : -1;
                auto contact = [&](int obj, int player) -> int64_t {
                    if (player < 0) return -1;
                    auto it = ids.find({obj, player});
                    return it == ids.end() ? -1 : (int64_t)it->second;
                };
                auto list = matjson::Value::array();
                int64_t found = 0;
                if (pl->m_objects) {
                    for (auto* o : CCArrayExt<GameObject*>(pl->m_objects)) {
                        if (!o)
                            continue;
                        float const x = (float)o->m_positionX;
                        if (x < from || x > to)
                            continue;
                        switch (o->m_objectType) {
                            case GameObjectType::Solid:
                            case GameObjectType::Hazard:
                            case GameObjectType::Decoration:
                            case GameObjectType::Breakable:
                            case GameObjectType::Slope:
                            case GameObjectType::CollisionObject:
                            case GameObjectType::AnimatedHazard:
                                continue;
                            default:
                                break;
                        }
                        auto* e = typeinfo_cast<EnhancedGameObject*>(o);
                        if (!e)
                            continue;
                        bool const used = e->m_activatedByPlayer1 || e->m_activatedByPlayer2;
                        if (onlyUsed && !used)
                            continue;
                        if (++found > 200)
                            continue;
                        auto item = obj();
                        item["uid"] = (int64_t)o->m_uniqueID;
                        item["id"] = (int64_t)o->m_objectID;
                        item["type"] = (int64_t)o->m_objectType;
                        item["x"] = (double)x;
                        item["y"] = o->m_positionY;
                        item["sprite_x"] = (double)o->getPositionX();
                        item["sprite_y"] = (double)o->getPositionY();
                        item["p1"] = e->m_activatedByPlayer1;
                        item["p2"] = e->m_activatedByPlayer2;
                        if (auto* fx = typeinfo_cast<EffectGameObject*>(o))
                            item["multi"] = fx->canMultiActivate(pl->m_isPlatformer);
                        auto c = matjson::Value::array();
                        c.push(contact(o->m_uniqueID, uid1));
                        c.push(contact(o->m_uniqueID, uid2));
                        item["contact"] = c;
                        list.push(item);
                    }
                }
                auto out = obj();
                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                out["player1_x"] = (double)px;
                out["found"] = found;
                out["objects"] = list;
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
#if GB_NATIVE_ENGINE
                    reinterpret_cast<void (*)(void*, void*)>(geode::base::get() + 0x4ce060)(
                        nullptr, nullptr);
#else
                    // A real function off Windows, hooked there instead
                    // (GBPortableCheckpoint, hook_playlayer.cpp).
                    pl->queueCheckpoint();
#endif
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
                // player2 is the player, queued on the side that moves it
                // (see sideFor; Flip 2-Player Controls swaps them outside
                // platformer).
                pl->queueButton(1, press, GucciEngine::get()->replay.sideFor(pl, p2), 0.0);
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
            "gucci_abs_profile",
            "The speed test's timers left on for every simulated run - a real pathfinder "
            "run's, a search's, a leg's - until switched off: `start` clears and switches "
            "them on, `read` returns what they have gathered (us a simulated tick by part, "
            "materializeNear's parts and counts, run setup and putting away, snapshots, the "
            "World's capture; totals in seconds), `stop` switches them off (the numbers stay "
            "for `read`). Costs a little time on every tick while on.",
            schemaOf({{"action", "string", "start | read | stop"}}),
            [](matjson::Value const& a) {
                auto const action = argStr(a, "action", "read");
                if (action == "start") absense::profileRuns(true);
                else if (action == "stop") absense::profileRuns(false);
                else if (action != "read") throw ToolError("action must be start, read or stop");
                return absense::runProfile();
            },
        });

        server.addTool({
            "gucci_abs_benchmark",
            "Step R0 of AI_PATHFINDER_PLAN.md: time Absense's simulation from the real "
            "player's current state. Two runs (carry on, then hold) give microseconds "
            "per simulated tick and ticks per second; then a snapshot is taken and put "
            "back `snapshots` times for its cost, plus a rough size. Also logged as "
            "\"[AbsPF] speed test\". Nothing in the real game changes.",
            schemaOf({{"ticks", "integer", "most ticks per run (default 2000, max 20000)"},
                      {"snapshots", "integer", "snapshot save/restore pairs to time (default 200, max 5000)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                int const ticks = (int)std::clamp<int64_t>(argInt(a, "ticks", 2000), 1, 20000);
                int const snapshots = (int)std::clamp<int64_t>(argInt(a, "snapshots", 200), 1, 5000);
                auto out = absense::benchmark(ticks, snapshots);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        server.addTool({
            "gucci_route_atlas",
            "Step R1 of AI_PATHFINDER_PLAN.md: the route planner's map of the level being "
            "played. Columns one block wide with the game mode, the room it gives the "
            "player's centre (band) and the gaps the centre fits through (openings), and "
            "the gates (forks, orbs, pads, portals, toggles, tight gaps, waists, spacing) "
            "with the gaps a route can be in there (options), between x `from` and `to`. "
            "Step 4 of docs/ROUTE_ENGINE_PLAN.md: each gate also has its stable key "
            "(column/kind/object uid), what it is (e.g. blue orb) and its choices, what a "
            "route can do there: use/skip an orb, touch/avoid a pad or portal, fire/skip a "
            "toggle, free (anywhere in a gap, tried last), each with its band, the object's "
            "uid, whether it must or must not fire, and the gravity it leaves. Nothing in "
            "the game changes.",
            schemaOf({{"from", "integer", "left edge x (default: 300 behind the player)"},
                      {"to", "integer", "right edge x (default: from + 1200)"}}),
            [](matjson::Value const& a) {
                requireLevel();
                auto* pl = PlayLayer::get();
                int64_t const px = pl && pl->m_player1 ? (int64_t)pl->m_player1->getPositionX() : 0;
                int64_t const from = argInt(a, "from", px - 300);
                int64_t const to = argInt(a, "to", from + 1200);
                auto out = absense::levelMap((float)from, (float)to);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        server.addTool({
            "gucci_route_plan",
            "Step R2 of AI_PATHFINDER_PLAN.md: plan a route through the whole level on "
            "the level map from (from_x, from_y), with player 1's gravity now: at each "
            "gate, the option it takes (step 4: use/skip, touch/avoid, fire/skip, free) "
            "in which gap, and the gravity after it, priced by length, tightness and what "
            "the map cannot vouch for. Returns the best route's first `limit` waypoints, "
            "where the next best leave it, and next_gate: the next gate's options from "
            "(from_x, from_y) best first by the cost to go (the way there plus the "
            "cheapest way on to the level's end; free last). Nothing is played and "
            "nothing in the game changes.",
            schemaOf({{"from_x", "integer", "where to plan from (default: the player's x)"},
                      {"from_y", "integer", "the height to plan from (default: the player's y)"},
                      {"limit", "integer", "waypoints to return (default 60, max 2000)"}}),
            [](matjson::Value const& a) {
                requireLevel();
                auto* pl = PlayLayer::get();
                int64_t const px = pl && pl->m_player1 ? (int64_t)pl->m_player1->getPositionX() : 0;
                int64_t const py = pl && pl->m_player1 ? (int64_t)pl->m_player1->getPositionY() : 105;
                int const limit = (int)std::clamp<int64_t>(argInt(a, "limit", 60), 1, 2000);
                auto out = absense::routePlan((float)argInt(a, "from_x", px), (float)argInt(a, "from_y", py), limit);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        server.addTool({
            "gucci_route_guidance",
            "The Pathfinder page's 'Follow the planned route (AI)' setting, saved with the mod. "
            "off: the pathfinder as it was. ideas (step R4 of AI_PATHFINDER_PLAN.md): each "
            "Absense decision also asks the physics map for the way to the planned route's next "
            "gate, and that idea goes first when it reaches the gate alive and clears the "
            "decision's bar. drives (docs/ROUTE_ENGINE_PLAN.md step 5): each decision aims at "
            "the next gate's options from where it is, best first, one leg search at a time, "
            "and plays the first way physics finds through one only as far as the gate; when "
            "no option gets through, today's engine decides that one. `on` is the old switch "
            "(true: ideas, false: off). Without either, reports the setting.",
            schemaOf({{"mode", "string", "off, ideas or drives"},
                      {"on", "boolean", "the old switch: true for ideas, false for off"}}),
            [](matjson::Value const& a) {
                if (a.contains("mode")) {
                    auto const m = argStr(a, "mode");
                    if (m == "off") absense::setRouteGuidance(0);
                    else if (m == "ideas") absense::setRouteGuidance(1);
                    else if (m == "drives") absense::setRouteGuidance(2);
                    else throw ToolError("mode must be off, ideas or drives");
                } else if (a.contains("on")) {
                    absense::setRouteGuidance(argBool(a, "on", false) ? 1 : 0);
                }
                static char const* const kModes[] = {"off", "ideas", "drives"};
                int const mode = std::clamp(absense::routeGuidance(), 0, 2);
                auto out = matjson::Value::object();
                out["mode"] = std::string(kModes[mode]);
                out["on"] = mode != 0;
                return out;
            },
        });

        server.addTool({
            "gucci_route_leg",
            "Step R3 of AI_PATHFINDER_PLAN.md: the physics map with a goal. From the real "
            "player's state, play every way the button can go tick by tick (merging states "
            "that come out the same) until one reaches x `gate_x` inside the band lo..hi. "
            "Without gate_x: the next gate of the planned route, in the band of the option "
            "the route takes there (an orb's contact region for a use). Returns found (with the way "
            "as a script gucci_abs_simulate can play: h hold, r release, p press, t a tap inside the "
            "tick), impossible (exact and nothing dropped: every move it offers was "
            "tried, close to a proof), not_found (similar states merged: not a proof), or "
            "out_of_budget, and draws it over the level. `classes` is the most classes that "
            "shared the cap on one tick, `emptied` whether it ended with everything dead (with "
            "out_of_budget: some had been dropped for room) rather than out of steps, time or "
            "ticks. The game waits while it runs; nothing in the real game changes.",
            schemaOf({{"gate_x", "integer", "the gate's x (default: the next gate of the planned route)"},
                      {"lo", "integer", "bottom of the band (default: the route's option there)"},
                      {"hi", "integer", "top of the band (default: the route's option there; lo > hi = any height)"},
                      {"max_ticks", "integer", "default 2400"},
                      {"cap", "integer", "states kept a tick (default 192, max 1024)"},
                      {"exact", "boolean", "merge only identical states, for a proof (default false)"},
                      {"max_ms", "integer", "wall clock limit (default 3000, max 8000: the server waits 10 s)"},
                      {"cell", "integer",
                       "any height only: a UFO or swing keeps a share of the cap for each this many units of "
                       "height and each way of going (default 10, what the pathfinder's rescue legs use; 0: "
                       "the ranking alone, as before)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                bool const exact = a.contains("exact") && a["exact"].asBool().unwrapOr(false);
                auto out = absense::routeLeg((float)argInt(a, "gate_x", 0), (float)argInt(a, "lo", 1),
                                             (float)argInt(a, "hi", 0),
                                             (int)std::clamp<int64_t>(argInt(a, "max_ticks", 2400), 1, 20000),
                                             (int)std::clamp<int64_t>(argInt(a, "cap", 192), 1, 1024), exact,
                                             (double)std::clamp<int64_t>(argInt(a, "max_ms", 3000), 100, 8000),
                                             (float)std::clamp<int64_t>(argInt(a, "cell", 10), 0, 10000));
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
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
            "first when already held; the button stays down through the tick), "
            "t<N> = a tap inside each of N ticks (pressed and let go again before "
            "the tick's physics, so the button is up when the player moves), e.g. "
            "\"h20 r15 p1 h40\"; the last step repeats to `ticks`. \"t1 r20\" against "
            "\"p1 r20\" shows whether a press let go inside its own tick does "
            "anything. Nothing in the real game changes.",
            schemaOf({{"script", "string", "steps, e.g. h20 r15 p1 h40 (t1: a tap inside one tick)"},
                      {"ticks", "integer", "ticks to run (default: the script's length, max 3000)"},
                      {"every", "integer", "report the path every N ticks (default 4)"},
                      {"watch", "string",
                       "objects to follow as the run has them, every `every` ticks: uids \"5308,5340\" or "
                       "\"near:x,y,r\"; also lists the touch triggers the copy meets and what the World does "
                       "with them"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto const held = parseAbsScript(argStr(a, "script"));
                int const ticks = (int)std::clamp<int64_t>(
                    argInt(a, "ticks", std::max<int64_t>(1, (int64_t)held.size())), 1, 3000);
                int const every = (int)std::clamp<int64_t>(argInt(a, "every", 4), 1, 240);
                auto out = absense::simulate(held, ticks, every, argStr(a, "watch"));
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
            {
                auto which = obj();
                which["type"] = "string";
                which["description"] = "read: \"reference\" for the saved run (default: the recording)";
                props["which"] = which;
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
                        return fmt::format("{} x={:.3f} y={:.3f} vy={:.3f} r={:.1f}{}{} {} dt={:.5f}{} xd={:.6f} "
                                           "hid={:02x} rings={} btn=+{}-{}",
                                           f, s.x, s.y, s.yVel, s.rot,
                                           s.held ? " H" : "", s.onGround ? " G" : "", s.mode, s.dt,
                                           s.split ? " SPLIT" : "", s.extraDelta, s.hidden, s.rings, s.pushes,
                                           s.releases);
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
                        // `which`: "reference" reads the saved run instead.
                        auto const& src = argStr(a, "which") == "reference" ? tickprobe::saved : tickprobe::current;
                        for (auto const& [f, s] : src) {
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
                                s.mode != t.mode || s.hidden != t.hidden || s.rings != t.rings ||
                                s.pushes != t.pushes || s.releases != t.releases) {
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
            "where it was afterwards. Script as for gucci_abs_simulate: h<N> hold, "
            "r<N> release, p<N> press (held through the tick), t<N> a tap inside "
            "the tick (pressed and let go before the physics). \"t1 r20\" here says "
            "whether the real game and the simulation agree about such a tap.",
            schemaOf({{"script", "string", "steps, e.g. h20 r15 p1 h40 (t1: a tap inside one tick)"},
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
            "an online level by its `id`, or one of RobTop's levels by `main` "
            "(1 Stereo Madness to 22 Dash, nothing downloaded; a level the "
            "level-select screen shows locked is refused), from anywhere outside a "
            "level. An online level not downloaded yet is downloaded first, the way "
            "clicking it in the game does: the reply is then {downloading: true}; "
            "call again until it opens. For Assistant Access sessions that restart "
            "the game or test the pathfinder on a list of levels.",
            schemaOf({{"name", "string", "a local level's name, exactly"},
                      {"id", "integer", "an online level's ID (instead of name)"},
                      {"main", "integer", "one of RobTop's levels, 1 to 22 (instead of name)"}}),
            [](matjson::Value const& a) {
                if (PlayLayer::get() || LevelEditorLayer::get())
                    throw ToolError("leave the level (or the editor) first");
                // GucciBot (2026-11-01): RobTop's levels, as the level-select
                // screen opens them (LevelPage): GD builds them from its own
                // files, so nothing is downloaded. Clubstep, Theory of
                // Everything 2 and Deadlocked are locked until the account has
                // their secret coins (the level's required coins against stat
                // 8, the secret coins collected).
                if (a.contains("main")) {
                    int64_t const id = argInt(a, "main", 0);
                    if (id < 1 || id > 22)
                        throw ToolError("main is 1 to 22 (Stereo Madness to Dash)");
                    auto* glm = GameLevelManager::get();
                    auto* level = glm ? glm->getMainLevel((int)id, false) : nullptr;
                    if (!level)
                        throw ToolError(fmt::format("GD has no main level {}", id));
                    auto* stats = GameStatsManager::sharedState();
                    int const coins = stats ? stats->getStat("8") : 0;
                    if (level->m_requiredCoins > coins)
                        throw ToolError(fmt::format("{} is locked: it needs {} secret coins and this account has {}",
                                                    std::string(level->m_levelName), level->m_requiredCoins, coins));
                    auto* scene = PlayLayer::scene(level, false, false);
                    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
                    auto out = obj();
                    out["main"] = id;
                    out["opening"] = std::string(level->m_levelName);
                    return out;
                }
                if (int const id = (int)argInt(a, "id", 0); id > 0) {
                    auto* glm = GameLevelManager::get();
                    auto* level = glm ? glm->getSavedLevel(id) : nullptr;
                    auto out = obj();
                    out["id"] = (int64_t)id;
                    if (!level || std::string(level->m_levelString).empty()) {
                        // Not here yet: ask the servers for it, as the level's page
                        // does, and let the caller come back. At most once every 15
                        // s per level, however often the caller polls.
                        static std::unordered_map<int, std::chrono::steady_clock::time_point> asked;
                        auto const now = std::chrono::steady_clock::now();
                        auto const it = asked.find(id);
                        if (glm && (it == asked.end() || now - it->second > std::chrono::seconds(15))) {
                            asked[id] = now;
                            glm->downloadLevel(id, false, 0);
                        }
                        out["downloading"] = true;
                        return out;
                    }
                    auto* scene = PlayLayer::scene(level, false, false);
                    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
                    out["opening"] = std::string(level->m_levelName);
                    return out;
                }
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
            schema["type"] = "object";
            schema["properties"] = props;
            // GucciBot (2026-10-29): no "save" any more. It stored the mode under
            // a key nothing loads: GucciBot always starts in Accuracy.
            server.addTool({
                "gucci_lock_delta",
                "Read or switch Lock Delta's mode. Accuracy drives GD one physics step "
                "per update, as recording always does; Performance hands GD several "
                "steps in one update during playback, and GD's own sub-steps do not "
                "always come out the same as single steps. A switch lasts until the "
                "game restarts or a preset loads: GucciBot always starts in Accuracy.",
                schema,
                [](matjson::Value const& a) {
                    auto* gb = GucciEngine::get();
                    auto const m = argStr(a, "mode");
                    if (!m.empty()) {
                        if (m != "accuracy" && m != "performance")
                            throw ToolError("mode must be accuracy or performance");
                        gb->updater.m_lockDeltaMode = m == "accuracy" ? GucciUpdater::LockDeltaMode::Accuracy
                                                                      : GucciUpdater::LockDeltaMode::Performance;
                    }
                    auto out = obj();
                    out["mode"] = gb->updater.m_lockDeltaMode == GucciUpdater::LockDeltaMode::Accuracy ? "accuracy"
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
