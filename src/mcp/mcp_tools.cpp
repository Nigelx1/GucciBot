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
#include "audio/clicks.hpp"
#include "mcp/events.hpp"
#include "mcp/scene.hpp"
#include "mcp/screenshot.hpp"
#include "mcp/test_settings.hpp"
#include "mcp/tickprobe.hpp"
#include "analysis/trajectory.hpp"
#include "tools/macro_check.hpp"
#include "core/GucciBot.hpp"
#include "hacks/hitborder_probe.hpp"
#include "hacks/hitborders.hpp"
#include "hacks/safemode.hpp"
#include "render/renderer.hpp"
#include "trainers/click_indicators.hpp"
#include "ui/probe.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

    // Who, if anyone, currently owns the run. Returned by get_state so the
    // caller can see it, used to refuse the driving tools, and watched by the
    // event recorder (mcp/events.hpp).
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

        // GD's flip fields, under the names these bindings give them, or
        // nothing where they have neither (framewindow.cpp does the same: the
        // CI and local bindings have differed).
        template <class L>
        auto cameraFlipOf(L const* l, int) -> std::optional<decltype(l->m_cameraFlip)> {
            return l->m_cameraFlip;
        }
        template <class L>
        std::optional<float> cameraFlipOf(L const*, long) {
            return std::nullopt;
        }
        template <class S>
        auto flipProgressOf(S const& gs, int) -> std::optional<decltype(gs.m_levelFlipProgress)> {
            return gs.m_levelFlipProgress;
        }
        template <class S>
        auto flipProgressOf(S const& gs, long) -> std::optional<decltype(gs.m_levelFlipping)> {
            return gs.m_levelFlipping;
        }
        template <class S>
        std::optional<float> flipProgressOf(S const&, ...) {
            return std::nullopt;
        }

        // A file name a tool may write in the screenshots folder: 1 to 64
        // letters, digits, - or _ (no path, no extension).
        bool bareName(std::string const& name) {
            bool bare = !name.empty() && name.size() <= 64;
            for (char const c : name)
                bare = bare && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_');
            return bare;
        }

        fs::path screenshotsDir() {
            auto const dir = Mod::get()->getSaveDir() / "screenshots";
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec)
                throw ToolError("couldn't make the screenshots folder: " + ec.message());
            return dir;
        }

        // gucci_ui's arguments (and a find for gucci_ui_view) as the menu
        // probe's query (ui/probe.hpp).
        ui::probe::Query uiQuery(matjson::Value const& a) {
            ui::probe::Query q;
            q.window = argStr(a, "window", "all");
            q.card = argStr(a, "card");
            auto const format = argStr(a, "format", "text");
            if (format != "text" && format != "json")
                throw ToolError("format is text or json");
            q.text = format == "text";
            q.visibleOnly = argBool(a, "visible_only", false);
            q.lint = argBool(a, "lint", false);
            q.max = (int)std::clamp<int64_t>(argInt(a, "max", 800), 1, 5000);
            q.maxLines = (int)std::clamp<int64_t>(argInt(a, "max_lines", 400), 1, 20000);
            if (a.contains("find")) {
                auto const& f = a["find"];
                if (!f.isObject())
                    throw ToolError("find is an object: {window, card, row, label, kind, value, context, id, index}");
                q.finding = true;
                q.findWindow = argStr(f, "window");
                q.findCard = argStr(f, "card");
                q.findRow = argStr(f, "row");
                q.findLabel = argStr(f, "label");
                q.findKind = argStr(f, "kind");
                if (f.contains("value"))
                    q.findValue = f["value"].isString() ? argStr(f, "value") : f["value"].dump(matjson::NO_INDENTATION);
                q.findContext = argStr(f, "context");
                if (f.contains("id")) {
                    auto const id = argStr(f, "id");
                    try {
                        q.findId = (uint32_t)std::stoul(id, nullptr, 16);
                    } catch (...) {
                        throw ToolError("find.id is an ImGui ID as gucci_ui shows it, e.g. \"0x8F3A12C0\"");
                    }
                }
                if (f.contains("index"))
                    q.findIndex = (int)argInt(f, "index", 0);
                if (argBool(f, "visible_only", false))
                    q.visibleOnly = true;
            }
            return q;
        }

        // What gucci_ui and gucci_ui_view add to the probe's answer: the
        // engine's frame and the menu's state.
        void uiHeader(matjson::Value& out) {
            out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
            auto const s = ui::menuState();
            auto menu = obj();
            menu["open"] = s.open;
            menu["compact"] = s.compact;
            menu["page"] = s.page;
            menu["theme"] = s.themeName;
            menu["test_theme"] = s.testTheme;
            menu["effects"] = s.effects;
            out["menu"] = menu;
            if (out.contains("outline")) {
                std::string head = fmt::format("menu {}  page={}  theme={}{}{}\n", s.open ? "open" : "shut", s.page,
                                               s.themeName, s.compact ? "  compact" : "",
                                               s.testTheme ? "  (a test's theme)" : "");
                out["outline"] = head + out["outline"].asString().unwrapOr("");
            }
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
            "test_mute, and clicks: the click sounds' switches, packs, background "
            "noise and the last 20 clicks played) and test_settings (the keys "
            "gucci_test_set holds). status_bar is the menu's status bar in words "
            "{state: IDLE/RENDERING/CALCULATING/PATHFINDING/RECORDING/PLAYING, "
            "tone, safe: the SAFE chip, text: the numbers beside it}; menu {open, "
            "page, compact, theme}; start_pos_warning the Macro card's start "
            "position warning (\"\" for none).",
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
                    {
                        // GucciBot (2026-11-01): the click sounds, and what they
                        // played last. A pack's status loads it, so only while
                        // they're on (they load at launch then anyway).
                        auto const& cs = clicks::settings();
                        auto c = obj();
                        c["enabled"] = cs.enabled;
                        c["pack"] = cs.pack;
                        c["p2_separate"] = cs.separateP2;
                        c["p2_pack"] = cs.p2Pack;
                        c["volume"] = (double)cs.volume;
                        c["during_playback"] = cs.duringPlayback;
                        c["in_renders"] = cs.inRenders;
                        if (cs.enabled) {
                            auto packs = matjson::Value::array();
                            for (bool const p2 : {false, true}) {
                                if (p2 && !cs.separateP2)
                                    break;
                                auto const st = clicks::status(p2);
                                auto p = obj();
                                p["player2"] = p2;
                                p["name"] = st.name;
                                p["missing"] = st.missing;
                                p["built_in"] = st.builtIn;
                                p["audio_ready"] = st.audioReady;
                                p["clicks"] = (int64_t)st.clicks;
                                p["releases"] = (int64_t)st.releases;
                                p["soft_clicks"] = (int64_t)st.softClicks;
                                p["soft_releases"] = (int64_t)st.softReleases;
                                p["unreadable"] = (int64_t)st.unreadable;
                                p["has_noise"] = st.hasNoise;
                                packs.push(p);
                            }
                            c["packs"] = packs;
                        }
                        auto noise = obj();
                        noise["on"] = cs.noise;
                        noise["volume"] = (double)cs.noiseVolume;
                        noise["playing"] = clicks::noisePlaying();
                        c["noise"] = noise;
                        auto last = matjson::Value::array();
                        for (auto const& p : clicks::recent()) {
                            auto e = obj();
                            e["frame"] = (int64_t)p.frame;
                            e["p2"] = p.p2;
                            e["pack"] = p.pack;
                            e["kind"] = std::string(p.kind);
                            e["source"] = std::string(p.source);
                            last.push(e);
                        }
                        c["last"] = last;
                        audio["clicks"] = c;
                    }
                    out["audio"] = audio;
                }
                out["test_settings"] = testset::heldKeys();
                {
                    // GucciBot (2026-11-01): the menu's status bar in words (the
                    // bar is drawn from the same ui::statusLine), and the menu.
                    auto const line = ui::statusLine();
                    auto bar = obj();
                    bar["state"] = line.state;
                    bar["tone"] = line.tone;
                    bar["safe"] = line.safe;
                    bar["text"] = line.text;
                    out["status_bar"] = bar;
                    auto const ms = ui::menuState();
                    auto menu = obj();
                    menu["open"] = ms.open;
                    menu["page"] = ms.page;
                    menu["compact"] = ms.compact;
                    menu["theme"] = ms.themeName;
                    out["menu"] = menu;
                    out["start_pos_warning"] = gb->startPosWarning;
                }

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
                "switch), test_mute (FMOD's master volume 0), fwac_conflict_test_id "
                "(one more mod id for Calculate's conflict check), pf_pending_verify "
                "(the pathfinder checks, every tick, that each object the real game "
                "moved and a copy could meet stands where the run has it; its stop line "
                "counts the misses) and pf_pending_cells (on by default: a level with "
                "rows of sections writes those objects back a cell at a time; off, all "
                "at a run's first look, as before). A change made in "
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
            "The macro files on disk, with size and extension (macros). Also "
            "what the Saved macros card lists right now: listed [{name, ext, tag "
            "(the theme it was saved under, \"\" for none), selected (the loaded "
            "one)}], in the card's order, and foreign [{file, format, known}]: "
            "other bots' macros in the replays folder, which the card offers to "
            "convert. The card's list follows the folder within about 1.5 s "
            "while the Macro page is open (or on Refresh).",
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
                // GucciBot (2026-11-01): the card's own list, as it shows it.
                auto listed = matjson::Value::array();
                for (auto const& m : ui::listedMacros()) {
                    auto e = obj();
                    e["name"] = m.name;
                    e["ext"] = m.ext;
                    e["tag"] = m.tag;
                    e["selected"] = m.selected;
                    listed.push(e);
                }
                out["listed"] = listed;
                auto foreign = matjson::Value::array();
                for (auto const& m : ui::foreignMacroList()) {
                    auto e = obj();
                    e["file"] = m.file;
                    e["format"] = m.format;
                    e["known"] = m.known;
                    foreign.push(e);
                }
                out["foreign"] = foreign;
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
        {
            auto schema = schemaOf(
                {{"name", "string", "the file's name: 1 to 64 letters, digits, - or _, no extension"},
                 {"frames", "integer",
                  "frames from now (or from the trigger) to the first shot, 1 to 600 (default 3; 1 with a trigger: "
                  "the frame it fires on)"},
                 {"count", "integer", "a burst: this many shots, 1 to 120 (default 1)"},
                 {"every", "integer", "a burst: frames between its shots, 1 to 600 (default 1)"},
                 {"scale", "integer", "1, 2 or 4: each side divided by it (default 1)"},
                 {"trigger", "string",
                  "now (default), death (player 1 dies), respawn (back alive, or the frame went back), complete (the "
                  "level's end) or frame:<n> (the engine's frame reaches n)"},
                 {"wait", "boolean", "answer once every file is written (default false: pending, then status)"},
                 {"status", "boolean", "true: how the last shot went, instead of taking one"}});
            auto crop = obj();
            crop["description"] =
                "a part of the window: [x, y, w, h] in ImGui's pixels (gucci_ui's rects), menu, page, compact, "
                "player (a square around player 1, 160 px each side) or player:<half>, or card:<title> (a card in "
                "the last gucci_ui capture)";
            schema["properties"]["crop"] = crop;
            server.addTool({
                "gucci_screenshot",
                "Save what the game window shows (the level, the menu, other mods' "
                "overlays) as a PNG: screenshots/<name>.png in GucciBot's save folder. "
                "It's taken `frames` frames from now (default 3: a window that has just "
                "opened is hidden for its first frame), so this returns {pending: true, "
                "path} at once; then call it with status: true until done is true "
                "(done, ok, error, path, width, height, ms). With wait: true it answers "
                "once the file is on disk instead, with the same fields. A burst "
                "(count > 1) is <name>_000.png..., `every` frames apart, and a burst or "
                "a trigger also writes <name>.json: each shot's engine frame, game "
                "tick, ImGui frame, ms since the trigger, player 1 {x, y, dead} and "
                "rect. A trigger (death, respawn, complete, frame:<n>) is checked "
                "in the frame's swap itself, so a death flash a frame after the "
                "death is caught exactly; status says armed while it waits. crop "
                "takes part of the window, scale shrinks it (a full 1080p shot stalls "
                "the frame a few ms: crop and scale a burst during anything timing "
                "matters to). A minimized game draws nothing, so its shots wait. "
                "Windows only for now.",
                schema,
                [](matjson::Value const& a) -> matjson::Value {
#ifdef GEODE_IS_WINDOWS
                    auto out = obj();
                    auto const statusJson = [] {
                        auto const s = shot::status();
                        auto st = obj();
                        st["done"] = s.done;
                        st["ok"] = s.ok;
                        st["error"] = s.asked ? s.error : std::string("no screenshot asked for this launch");
                        st["pending"] = s.pending;
                        st["armed"] = s.armed;
                        st["frames_left"] = (int64_t)s.framesLeft;
                        st["path"] = geode::utils::string::pathToString(s.path);
                        st["width"] = (int64_t)s.width;
                        st["height"] = (int64_t)s.height;
                        st["ms"] = s.ms;
                        st["count"] = (int64_t)s.count;
                        st["taken"] = (int64_t)s.taken;
                        st["written"] = (int64_t)s.written;
                        if (s.count > 1 || !s.manifest.empty()) {
                            auto files = matjson::Value::array();
                            for (auto const& f : s.files)
                                files.push(geode::utils::string::pathToString(f));
                            st["files"] = files;
                            st["manifest"] = geode::utils::string::pathToString(s.manifest);
                        }
                        return st;
                    };
                    if (argBool(a, "status", false))
                        return statusJson();
                    // A bare name, as gucci_read_log's: nothing outside the folder.
                    auto const name = argStr(a, "name");
                    if (!bareName(name))
                        throw ToolError("name must be 1 to 64 letters, digits, - or _ (no path, no extension)");

                    // GucciBot (2026-11-01): bursts, crops, scale and triggers.
                    shot::Burst b;
                    b.dir = screenshotsDir();
                    b.name = name;
                    b.count = (int)argInt(a, "count", 1);
                    if (b.count < 1 || b.count > 120)
                        throw ToolError("count is 1 to 120");
                    b.every = (int)argInt(a, "every", 1);
                    if (b.every < 1 || b.every > 600)
                        throw ToolError("every is 1 to 600");
                    b.scale = (int)argInt(a, "scale", 1);
                    if (b.scale != 1 && b.scale != 2 && b.scale != 4)
                        throw ToolError("scale is 1, 2 or 4");
                    auto const trigger = argStr(a, "trigger", "now");
                    if (trigger == "now")
                        b.trigger = shot::Trigger::Now;
                    else if (trigger == "death")
                        b.trigger = shot::Trigger::Death;
                    else if (trigger == "respawn")
                        b.trigger = shot::Trigger::Respawn;
                    else if (trigger == "complete")
                        b.trigger = shot::Trigger::Complete;
                    else if (trigger.rfind("frame:", 0) == 0) {
                        b.trigger = shot::Trigger::Frame;
                        try {
                            b.frame = std::stoll(trigger.substr(6));
                        } catch (...) {
                            throw ToolError("trigger frame:<n> needs a frame number, e.g. frame:600");
                        }
                    } else {
                        throw ToolError("trigger is now, death, respawn, complete or frame:<n>");
                    }
                    int64_t const frames = argInt(a, "frames", b.trigger == shot::Trigger::Now ? 3 : 1);
                    if (frames < 1 || frames > 600)
                        throw ToolError("frames is 1 to 600");
                    b.frames = (int)frames;
                    if (a.contains("crop")) {
                        auto const& c = a["crop"];
                        auto rectOf = [&b](ImVec4 r) {
                            b.crop.kind = shot::Crop::Kind::Rect;
                            b.crop.x = r.x;
                            b.crop.y = r.y;
                            b.crop.w = r.z;
                            b.crop.h = r.w;
                        };
                        if (c.isArray()) {
                            auto const& v = c.asArray().unwrap();
                            if (v.size() != 4)
                                throw ToolError("crop as a list is [x, y, w, h]");
                            rectOf(ImVec4((float)v[0].asDouble().unwrapOr(0.0), (float)v[1].asDouble().unwrapOr(0.0),
                                          (float)v[2].asDouble().unwrapOr(0.0), (float)v[3].asDouble().unwrapOr(0.0)));
                            if (b.crop.w <= 0.f || b.crop.h <= 0.f)
                                throw ToolError("crop's w and h are above 0");
                        } else if (c.isString()) {
                            auto const spec = c.asString().unwrap();
                            if (spec == "menu" || spec == "page" || spec == "compact" || spec == "macros") {
                                auto const w = ui::probe::live(spec);
                                if (!w)
                                    throw ToolError("crop " + spec + ": that window isn't drawn now");
                                rectOf(w->rect);
                            } else if (spec == "player" || spec.rfind("player:", 0) == 0) {
                                b.crop.kind = shot::Crop::Kind::Player;
                                if (spec.size() > 7) {
                                    try {
                                        b.crop.half = std::stof(spec.substr(7));
                                    } catch (...) {
                                        throw ToolError("crop player:<half> needs a number of pixels, e.g. player:120");
                                    }
                                    if (b.crop.half < 8.f || b.crop.half > 2000.f)
                                        throw ToolError("crop player:<half> is 8 to 2000 pixels");
                                }
                            } else if (spec.rfind("card:", 0) == 0) {
                                auto const r = ui::probe::cardRect(spec.substr(5));
                                if (!r)
                                    throw ToolError("crop " + spec + ": no such card in the last gucci_ui capture "
                                                    "(call gucci_ui with the card in view first)");
                                rectOf(*r);
                            } else {
                                throw ToolError("crop is [x, y, w, h], menu, page, compact, player, player:<half> or "
                                                "card:<title>");
                            }
                        } else {
                            throw ToolError("crop is [x, y, w, h] or a name (menu, page, player, card:<title>...)");
                        }
                    }
                    b.manifest = b.count > 1 || b.trigger != shot::Trigger::Now;
                    bool const wait = argBool(a, "wait", false);
                    // Waiting holds the call, and the caller gives up after 10 s:
                    // only for what starts now and fits in about 6 s at 60 fps.
                    if (wait && (b.trigger != shot::Trigger::Now || frames + (int64_t)b.count * b.every > 360))
                        throw ToolError("wait is for shots that start now and take at most 360 frames (frames + "
                                        "count x every); ask without wait, then poll with status: true");
                    shot::request(b);
                    auto const first = shot::status().path;
                    // GucciBot (2026-11-01): answered once it's on disk (a call
                    // that spans frames, mcp_server.hpp's Defer).
                    if (wait)
                        throw Defer{[first, statusJson]() -> std::optional<matjson::Value> {
                                        auto const s = shot::status();
                                        if (s.path != first)
                                            throw ToolError("another screenshot was asked for meanwhile");
                                        if (!s.done)
                                            return std::nullopt;
                                        return statusJson();
                                    },
                                    {}, 3000};
                    out["pending"] = true;
                    out["path"] = geode::utils::string::pathToString(first);
                    if (b.manifest)
                        out["manifest"] = geode::utils::string::pathToString(b.dir / (name + ".json"));
                    out["count"] = (int64_t)b.count;
                    out["armed"] = b.trigger != shot::Trigger::Now;
                    return out;
#else
                    (void)a;
                    throw ToolError("Windows only for now");
#endif
                },
            });
        }

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

        // GucciBot (2026-11-01): the menu as text (ui/probe.hpp). The next
        // frame the menu draws is captured, so this answers a frame or two
        // later (a call that spans frames, mcp_server.hpp's Defer).
        {
            auto schema = schemaOf(
                {{"page", "string", "show this page first (its title in the sidebar, as gucci_menu's page)"},
                 {"open", "boolean", "open (true) or shut (false) the menu first"},
                 {"window", "string",
                  "all (default), menu, compact, overlays (the boxes over the level), popups, or a window's key"},
                 {"card", "string", "only this card (its title)"},
                 {"format", "string", "text (default: an outline to read) or json (every element, for scripts)"},
                 {"visible_only", "boolean", "only what's in view (default false: what's scrolled away too)"},
                 {"lint", "boolean", "layout checks too: clipped_right, choice_option_clipped, overlaps, offscreen"},
                 {"max", "integer", "most elements (default 800, max 5000)"},
                 {"max_lines", "integer", "most text lines a window (default 400)"}});
            auto find = obj();
            find["type"] = "object";
            find["description"] =
                "only the elements matching all that's given, each exact, then ignoring case, then a part: "
                "{window, card, row, label (a control's row, a button's text, a text's words), kind (card, row, "
                "section, label, paragraph, hint, note, chip, progress, switch, choice, dropdown, slider, input, "
                "button, color, key, macro_row, item), value, context (e.g. a macro's name in Saved macros), id "
                "(\"0x8F3A12C0\"), index (the n-th match), visible_only}";
            schema["properties"]["find"] = find;
            server.addTool({
                "gucci_ui",
                "GucciBot's menu as text, read off one frame of it, including what's "
                "scrolled out of view: every page's cards, rows, controls with their "
                "values (enabled or not, in view or not), texts, notes and chips with "
                "their tone, the macro list's rows, open popups, Compact Mode and the "
                "boxes over the level. The text outline reads top to bottom: \"## "
                "Card\" headings (\"(below the view)\" when scrolled away), \"Row: "
                "controls  ~ hint\" (v/^ in front: below/above the view), [x]/[ ] a "
                "switch, (Chosen) Other a choice, [Button] (\"-\" after: disabled), "
                "{value} an input, slider or dropdown, <CHIP>, \"! \" a warning, \"> \" "
                "a note, \"~ \" a hint, \"-- Section --\". format json gives every "
                "element {n, win, card, row, ctx, kind, label, text, tone, value, "
                "options, ids, id, enabled, visible, rect} plus windows {name, key, "
                "role, rect, scroll_y, scroll_max_y, view_h} and text by window. With "
                "find, only the matches, each with its path. lint adds layout "
                "checks (data, not pass or fail: some clipping is meant). Only reads; "
                "page and open change only what gucci_menu would.",
                schema,
                [](matjson::Value const& a) -> matjson::Value {
                    auto const q = uiQuery(a);
                    for (char const* key : {"page", "card", "window"})
                        if (a.contains(key) && !a[key].isString())
                            throw ToolError(std::string(key) + " is a name (a string)");
                    if (a.contains("open") && !a["open"].isBool())
                        throw ToolError("open is true or false");
                    if (a.contains("page")) {
                        auto const title = argStr(a, "page");
                        if (!ui::listsPage(title, nullptr)) {
                            std::string pages;
                            for (auto const& p : ui::pageTitles())
                                pages += (pages.empty() ? "" : ", ") + p;
                            throw ToolError("no page called \"" + title + "\" in the sidebar. Pages: " + pages);
                        }
                    }
                    if (argBool(a, "open", false) && GucciEngine::get()->standingDown)
                        throw ToolError("GucciBot is standing down (another bot is enabled), so its menu stays shut");
                    // A page just shown, or the menu just opened, lays out over
                    // a frame or two (its cards size themselves): captured after.
                    int settle = 0;
                    if (a.contains("page")) {
                        settle = ui::pageTitle() == argStr(a, "page") ? settle : 2;
                        ui::setPageByTitle(argStr(a, "page"));
                    }
                    if (a.contains("open")) {
                        bool const open = argBool(a, "open", true);
                        if (open != ui::isOpen())
                            settle = 2;
                        ui::setOpen(open);
                    }
                    auto wait = std::make_shared<int>(settle);
                    if (*wait == 0)
                        ui::probe::request();
                    throw Defer{[wait, q]() -> std::optional<matjson::Value> {
                                    if (*wait > 0) {
                                        if (--*wait == 0)
                                            ui::probe::request();
                                        return std::nullopt;
                                    }
                                    if (!ui::probe::ready())
                                        return std::nullopt;
                                    auto out = ui::probe::result(q);
                                    uiHeader(out);
                                    return out;
                                },
                                {}, 120};
                },
            });
        }

        // GucciBot (2026-11-01): the menu scrolled, and a whole page in shots.
        // Where things are comes from a capture of the menu (ui/probe.hpp);
        // the scroll is ImGui's own, which nothing saves (the menu keeps no
        // .ini). It runs over several frames (Defer).
        {
            auto schema = schemaOf(
                {{"to", "string", "top, bottom, card (with card) or element (with find)"},
                 {"card", "string", "to card: the card's title; it's scrolled to the top of the view"},
                 {"y", "number", "scroll to this height (ImGui's pixels from the top)"},
                 {"by", "number", "scroll by this much (down is more)"},
                 {"window", "string",
                  "what scrolls: page (default: the menu's page), macros (the Saved macros list) or a window's name"},
                 {"shots", "string",
                  "a name: shots of the whole window top to bottom, <name>_000.png..., with a manifest <name>.json; "
                  "scrolled back after (Windows only)"},
                 {"overlap", "number", "shots: how much each overlaps the one before, 0 to 0.5 (default 0.1)"},
                 {"crop", "string", "shots: the window itself (default) or menu (the whole menu window)"}});
            auto find = obj();
            find["type"] = "object";
            find["description"] = "to element: one element, as gucci_ui's find ({card, row, label, kind, context, id, "
                                  "index...}); it's scrolled to the middle of the view";
            schema["properties"]["find"] = find;
            server.addTool({
                "gucci_ui_view",
                "Scroll one of the menu's windows (the page by default) and say what's "
                "in view after: {window, scroll_y, scroll_max_y, view_h, visible_cards}. "
                "to top/bottom, to card (its title at the top of the view), to element "
                "(a find, in the middle of the view), y or by. With shots: shots of the "
                "whole window, scrolled from the top to the bottom a view at a time "
                "(each overlapping the last by `overlap`), then scrolled back where it "
                "was; it answers {files [{path, scroll_y, rect, width, height}], "
                "manifest, content_h, view_h...} once every file is written, and "
                "tools/assist.py's page_sheet()/stitch() put them together. A view "
                "only: nothing is saved, and it works during a run.",
                schema,
                [](matjson::Value const& a) -> matjson::Value {
                    struct View {
                        enum class Stage { Look, Settle, Report, Shot, ShotWait };
                        Stage stage = Stage::Look;
                        std::string key = "page";
                        std::string to;
                        std::string card;
                        std::optional<double> y;
                        std::optional<double> by;
                        ui::probe::Query find;
                        std::string shots;
                        double overlap = 0.1;
                        bool cropMenu = false;
                        std::string name;  // the ImGui window that scrolls
                        float original = 0.f;
                        float step = 0.f;
                        int wait = 0;
                        int index = 0;
                        fs::path dir;
                        fs::path path;
                        float shotScroll = 0.f;
                        ImVec4 shotRect{};
                        matjson::Value files = matjson::Value::array();
                    };
                    auto st = std::make_shared<View>();
                    st->key = argStr(a, "window", "page");
                    st->to = argStr(a, "to");
                    st->card = argStr(a, "card");
                    if (a.contains("y"))
                        st->y = a["y"].asDouble().unwrapOr(0.0);
                    if (a.contains("by"))
                        st->by = a["by"].asDouble().unwrapOr(0.0);
                    st->shots = argStr(a, "shots");
                    st->overlap = std::clamp(a.contains("overlap") ? a["overlap"].asDouble().unwrapOr(0.1) : 0.1, 0.0, 0.5);
                    auto const crop = argStr(a, "crop", "page");
                    if (crop != "page" && crop != "menu")
                        throw ToolError("crop is page (the window itself) or menu");
                    st->cropMenu = crop == "menu";
                    if (!st->to.empty() && st->to != "top" && st->to != "bottom" && st->to != "card" &&
                        st->to != "element")
                        throw ToolError("to is top, bottom, card or element");
                    if (st->to == "card" && st->card.empty())
                        throw ToolError("to card needs card: the card's title");
                    if (st->to == "element") {
                        if (!a.contains("find"))
                            throw ToolError("to element needs find: {card, row, label, kind, context, id, index}");
                        auto q = obj();
                        q["find"] = a["find"];
                        st->find = uiQuery(q);
                    }
                    if (!st->shots.empty()) {
#ifdef GEODE_IS_WINDOWS
                        if (!bareName(st->shots))
                            throw ToolError("shots is a name: 1 to 64 letters, digits, - or _");
                        st->dir = screenshotsDir();
#else
                        throw ToolError("shots are Windows only for now");
#endif
                    } else if (st->to.empty() && !st->y && !st->by) {
                        throw ToolError("say where: to (top, bottom, card, element), y, by, or shots");
                    }
                    ui::probe::request();

                    auto poll = [st]() -> std::optional<matjson::Value> {
                        using Stage = View::Stage;
                        switch (st->stage) {
                            case Stage::Look: {
                                if (!ui::probe::ready())
                                    return std::nullopt;
                                auto const w = ui::probe::window(st->key);
                                if (!w)
                                    throw ToolError("no window \"" + st->key +
                                                    "\" was drawn (is the menu open on a page? gucci_ui lists the "
                                                    "windows)");
                                st->name = w->name;
                                st->original = w->scrollY;
                                float target = w->scrollY;
                                if (!st->shots.empty()) {
                                    st->step = std::max(8.f, w->viewH * (1.f - static_cast<float>(st->overlap)));
                                    ui::probe::scrollTo(st->name, 0.f);
                                    st->wait = 2;
                                    st->stage = Stage::Shot;
                                    return std::nullopt;
                                }
                                if (st->to == "top")
                                    target = 0.f;
                                else if (st->to == "bottom")
                                    target = w->scrollMaxY;
                                else if (st->to == "card") {
                                    auto const r = ui::probe::cardRect(st->card);
                                    if (!r)
                                        throw ToolError("no card called \"" + st->card + "\" on this page");
                                    target = w->scrollY + (r->y - w->rect.y) - 6.f;
                                } else if (st->to == "element") {
                                    auto const hits = ui::probe::find(st->find);
                                    if (hits.empty())
                                        throw ToolError("nothing matches find");
                                    if (hits.size() > 1) {
                                        std::string list;
                                        for (size_t i = 0; i < hits.size() && i < 10; ++i)
                                            list += fmt::format("\n[{}] {}", hits[i].n, hits[i].path);
                                        throw ToolError(fmt::format("{} elements match find (add index, or more of "
                                                                    "the path):{}",
                                                                    hits.size(), list));
                                    }
                                    // rect is x, y, width, height: its middle to the view's.
                                    auto const& r = hits[0].rect;
                                    target = w->scrollY + (r.y - w->rect.y) - (w->viewH - r.w) * 0.5f;
                                }
                                if (st->y)
                                    target = static_cast<float>(*st->y);
                                if (st->by)
                                    target = w->scrollY + static_cast<float>(*st->by);
                                ui::probe::scrollTo(st->name, std::max(0.f, target));
                                st->wait = 2;
                                st->stage = Stage::Settle;
                                return std::nullopt;
                            }
                            case Stage::Settle:
                                if (--st->wait > 0)
                                    return std::nullopt;
                                ui::probe::request();
                                st->stage = Stage::Report;
                                return std::nullopt;
                            case Stage::Report: {
                                if (!ui::probe::ready())
                                    return std::nullopt;
                                auto out = obj();
                                auto const w = ui::probe::window(st->key);
                                out["window"] = st->name;
                                out["from"] = std::round(st->original);
                                if (w) {
                                    out["scroll_y"] = std::round(w->scrollY);
                                    out["scroll_max_y"] = std::round(w->scrollMaxY);
                                    out["view_h"] = std::round(w->viewH);
                                }
                                out["visible_cards"] = ui::probe::visibleCards();
                                return out;
                            }
#ifdef GEODE_IS_WINDOWS
                            case Stage::Shot: {
                                if (--st->wait > 0)
                                    return std::nullopt;
                                auto const w = ui::probe::live(st->name);
                                auto const area = st->cropMenu ? ui::probe::live("menu") : w;
                                if (!w || !area)
                                    throw ToolError("the window closed while its shots were being taken");
                                st->shotScroll = w->scrollY;
                                st->shotRect = area->rect;
                                st->path = st->dir / fmt::format("{}_{:03}.png", st->shots, st->index);
                                shot::Crop c;
                                c.kind = shot::Crop::Kind::Rect;
                                c.x = area->rect.x;
                                c.y = area->rect.y;
                                c.w = area->rect.z;
                                c.h = area->rect.w;
                                shot::request(st->path, 1, c);
                                st->stage = Stage::ShotWait;
                                return std::nullopt;
                            }
                            case Stage::ShotWait: {
                                auto const s = shot::status();
                                if (s.path != st->path)
                                    throw ToolError("another screenshot was asked for meanwhile");
                                if (!s.done)
                                    return std::nullopt;
                                if (!s.ok)
                                    throw ToolError("a shot failed: " + s.error);
                                auto f = obj();
                                f["path"] = geode::utils::string::pathToString(st->path);
                                f["scroll_y"] = std::round(st->shotScroll);
                                f["rect"] = std::vector<double>{st->shotRect.x, st->shotRect.y, st->shotRect.z,
                                                                st->shotRect.w};
                                f["width"] = (int64_t)s.width;
                                f["height"] = (int64_t)s.height;
                                st->files.push(f);
                                st->index++;
                                auto const w = ui::probe::live(st->name);
                                float const max = w ? w->scrollMaxY : 0.f;
                                if (!w || st->shotScroll + 0.5f >= max || st->index >= 40) {
                                    ui::probe::scrollTo(st->name, st->original);
                                    auto out = obj();
                                    out["name"] = st->shots;
                                    out["window"] = st->name;
                                    out["files"] = st->files;
                                    out["view_h"] = w ? std::round(w->viewH) : 0.f;
                                    out["scroll_max_y"] = std::round(max);
                                    out["content_h"] = std::round(max + (w ? w->viewH : 0.f));
                                    out["overlap"] = st->overlap;
                                    auto const manifest = st->dir / (st->shots + ".json");
                                    if (auto r = geode::utils::file::writeString(manifest, out.dump()); !r)
                                        throw ToolError("couldn't write the manifest: " + r.unwrapErr());
                                    out["manifest"] = geode::utils::string::pathToString(manifest);
                                    return out;
                                }
                                ui::probe::scrollTo(st->name, st->shotScroll + st->step);
                                st->wait = 2;
                                st->stage = Stage::Shot;
                                return std::nullopt;
                            }
#endif
                            default:
                                throw ToolError("internal error: unknown stage");
                        }
                    };
                    // A page left scrolled for shots goes back where it was.
                    auto cancel = [st] {
                        if (!st->shots.empty() && !st->name.empty())
                            ui::probe::scrollTo(st->name, st->original);
                    };
                    throw Defer{poll, cancel, 900};
                },
            });
        }

        // GucciBot (2026-11-01): Click Indicators as they stand, since its lane
        // is drawn straight onto ImGui's background and is in no shot's text.
        server.addTool({
            "gucci_click_indicators",
            "Click Indicators right now: enabled (its switch), active (showing in "
            "the level this frame: only in a level, while GucciBot neither plays "
            "nor records), showing (what it shows, \"\" for nothing), tally "
            "{perfect, ok, miss} (the scoring of your own clicks), and settings: "
            "the lane, its side, look-ahead, offset and the scoring's windows. "
            "Only reads.",
            schemaOf({}),
            [](matjson::Value const&) {
                auto const& s = clickind::settings();
                auto out = obj();
                out["enabled"] = s.enabled;
                out["active"] = clickind::active();
                auto const* showing = clickind::showing();
                out["showing"] = std::string(showing ? showing : "");
                auto const t = clickind::tally();
                auto tally = obj();
                tally["perfect"] = (int64_t)t.perfect;
                tally["ok"] = (int64_t)t.ok;
                tally["miss"] = (int64_t)t.miss;
                out["tally"] = tally;
                auto set = obj();
                set["offset_ms"] = (int64_t)s.offsetMs;
                set["look_ahead_s"] = (double)s.lookAhead;
                set["max_notes"] = (int64_t)s.maxNotes;
                set["lane"] = s.lane;
                static char const* const kSides[] = {"left", "right", "middle"};
                set["lane_side"] = std::string(kSides[std::clamp(s.laneSide, 0, 2)]);
                set["lane_window_s"] = (double)s.laneWindow;
                set["note_shape"] = s.noteShape == 1 ? "circles" : "slabs";
                set["p2_lane"] = s.p2Lane;
                set["accuracy"] = s.accuracy;
                set["perfect_ms"] = (int64_t)s.perfectMs;
                set["ok_ms"] = (int64_t)s.okMs;
                out["settings"] = set;
                return out;
            },
        });

        // GucciBot (2026-11-01): GD's own screens as data (mcp/scene.hpp).
        {
            auto schema = schemaOf({{"root", "string", "all (default: the scene, then Geode's overlay), scene, "
                                                       "overlay, ui (the level's UI layer) or id:<node id>"},
                                    {"depth", "integer", "how deep to walk (default 14)"},
                                    {"max", "integer", "most nodes listed (default 1500, max 5000)"},
                                    {"visible_only", "boolean", "only nodes drawn on screen (default true)"},
                                    {"objects", "boolean", "walk the level's objects too (default false: counted)"},
                                    {"format", "string", "text (default: one line a node) or json"}});
            auto find = obj();
            find["type"] = "object";
            find["description"] =
                "{id, class, text}: only the nodes that match all that are given (a part of each, ignoring "
                "case; id \"a|b\" for either), each with every text under it, e.g. {id: \"nigelx1.guccibot/\"}";
            schema["properties"]["find"] = find;
            server.addTool({
                "gucci_scene",
                "GD's screen as data: the cocos node tree with each node's class, ID, "
                "text (labels, text boxes), box on screen (cocos units, y up from the "
                "bottom; win is the screen, px_per_unit turns units into pixels), "
                "shown (drawn and on screen), opacity, colour, scale, a menu item's "
                "enabled and a draw node's vertices (Calculate's markers, ghosts, the "
                "macro path). Also gd (in a level: GD's percentage and attempts "
                "labels and progress bar, from PlayLayer itself), popups (alerts and "
                "Geode popups: title, body, buttons) and notifications (text, box, "
                "offscreen). Other mods' nodes show the same way. The level's "
                "objects are counted, not listed, unless objects is true. Only reads.",
                schema,
                [](matjson::Value const& a) {
                    scene::Options o;
                    o.root = argStr(a, "root", "all");
                    o.depth = (int)std::clamp<int64_t>(argInt(a, "depth", 14), 1, 64);
                    o.max = (int)std::clamp<int64_t>(argInt(a, "max", 1500), 1, 5000);
                    o.visibleOnly = argBool(a, "visible_only", true);
                    o.objects = argBool(a, "objects", false);
                    if (a.contains("find")) {
                        auto const& f = a["find"];
                        if (!f.isObject())
                            throw ToolError("find is an object: {id, class, text}");
                        o.findId = argStr(f, "id");
                        o.findClass = argStr(f, "class");
                        o.findText = argStr(f, "text");
                    }
                    auto const format = argStr(a, "format", "text");
                    if (format != "text" && format != "json")
                        throw ToolError("format is text or json");
                    return scene::walk(o, format == "text");
                },
            });
        }

        // GucciBot (2026-11-01): everything over the level in one call: the
        // menu probe's capture for GucciBot's own ImGui windows, the scene for
        // its nodes, GD's labels, notifications and alerts.
        server.addTool({
            "gucci_overlays",
            "Everything over the level right now, as text and data, in one call (a "
            "frame later): imgui (GucciBot's ImGui windows other than the menu, with "
            "their text: Calculate's box, the pathfinder's boxes and cover, Compact "
            "Mode, Video Mode's bar and settings), gucci_nodes (GucciBot's own nodes "
            "in the scene by ID: the HUD, the Survival Indicator, Calculate's labels "
            "and markers, the safe-mode dot, Click Indicators... each with its text "
            "and box), gd (GD's percentage, attempts and progress bar), "
            "notifications (text, box, offscreen: whether it fits the screen), "
            "alerts (title, body, buttons), click_indicators {enabled, active, "
            "showing, tally} (its lane is drawn on ImGui's background, so it's read "
            "from its state) and status_bar. Only reads.",
            schemaOf({}),
            [](matjson::Value const&) -> matjson::Value {
                ui::probe::request();
                throw Defer{[]() -> std::optional<matjson::Value> {
                                if (!ui::probe::ready())
                                    return std::nullopt;
                                auto out = obj();
                                {
                                    ui::probe::Query q;
                                    q.text = false;
                                    q.max = 300;
                                    auto imgui = obj();
                                    auto windows = matjson::Value::array();
                                    auto text = obj();
                                    for (char const* which : {"overlays", "compact"}) {
                                        q.window = which;
                                        auto part = ui::probe::result(q);
                                        for (auto const& w : part["windows"])
                                            windows.push(w);
                                        for (auto const& [key, value] : part["text"])
                                            text[key] = value;
                                    }
                                    imgui["windows"] = windows;
                                    imgui["text"] = text;
                                    out["imgui"] = imgui;
                                }
                                {
                                    scene::Options o;
                                    o.findId = "nigelx1.guccibot/|framewindow";
                                    o.max = 300;
                                    o.visibleOnly = false;
                                    auto nodes = scene::walk(o, false);
                                    out["gucci_nodes"] = nodes["nodes"];
                                    out["notifications"] = nodes["notifications"];
                                    out["alerts"] = nodes["popups"];
                                }
                                if (auto* pl = PlayLayer::get())
                                    out["gd"] = scene::gdLabels(pl);
                                {
                                    auto ci = obj();
                                    ci["enabled"] = clickind::settings().enabled;
                                    ci["active"] = clickind::active();
                                    auto const* showing = clickind::showing();
                                    ci["showing"] = std::string(showing ? showing : "");
                                    auto const t = clickind::tally();
                                    auto tally = obj();
                                    tally["perfect"] = (int64_t)t.perfect;
                                    tally["ok"] = (int64_t)t.ok;
                                    tally["miss"] = (int64_t)t.miss;
                                    ci["tally"] = tally;
                                    out["click_indicators"] = ci;
                                }
                                {
                                    auto const line = ui::statusLine();
                                    auto bar = obj();
                                    bar["state"] = line.state;
                                    bar["safe"] = line.safe;
                                    bar["text"] = line.text;
                                    out["status_bar"] = bar;
                                }
                                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                                return out;
                            },
                            {}, 120};
            },
        });

        // GucciBot (2026-11-01): the event recorder (mcp/events.hpp).
        {
            auto schema = schemaOf({{"since", "integer", "only events after this seq (default 0: all it holds)"},
                                    {"limit", "integer", "most events returned (default 200, max 2000)"}});
            auto kinds = obj();
            kinds["description"] =
                "only these kinds, as a list or comma-separated: notification, alert, alert_closed, scene, "
                "level_open, level_quit, death, respawn, reset, complete, mode, run_start, run_end, calculate, "
                "render, imgui_popup";
            schema["properties"]["kinds"] = kinds;
            server.addTool({
                "gucci_events",
                "What happened on screen and in the game, recorded as it happened "
                "(every frame while Assistant Access is on), so a notification that "
                "shows for two seconds or a death between two calls isn't missed: "
                "Geode notifications (text, box, offscreen; updated: its text "
                "changed), alerts and popups (title, body, buttons) and their "
                "closing, the screen changing, a level opening or closing, player "
                "1's death (x, y, percent), respawn, reset (the frame went back) "
                "and the level's end, GucciBot's mode, a run starting and ending "
                "(render, analyzer, pathfinder), Calculate's last message, a "
                "render's result and ImGui popups opening. Each has seq, frame, "
                "tick (in a level), ms since the recorder started, kind, text and "
                "data. Pass the answer's seq back as since to get only what's "
                "new; dropped counts events the 2000-event ring no longer holds.",
                schema,
                [](matjson::Value const& a) {
                    int64_t const since = std::max<int64_t>(0, argInt(a, "since", 0));
                    size_t const limit = (size_t)std::clamp<int64_t>(argInt(a, "limit", 200), 1, 2000);
                    std::vector<std::string> kinds;
                    if (a.contains("kinds")) {
                        auto const& k = a["kinds"];
                        if (k.isArray()) {
                            for (auto const& v : k.asArray().unwrap())
                                if (v.isString())
                                    kinds.push_back(v.asString().unwrap());
                        } else if (k.isString()) {
                            std::stringstream in(k.asString().unwrap());
                            for (std::string part; std::getline(in, part, ',');) {
                                part.erase(0, part.find_first_not_of(' '));
                                part.erase(part.find_last_not_of(' ') + 1);
                                if (!part.empty())
                                    kinds.push_back(part);
                            }
                        } else {
                            throw ToolError("kinds is a list of kinds, or one string of them split by commas");
                        }
                    }
                    return events::read((uint64_t)since, kinds, limit);
                },
            });
        }

        // --- the analyzer --------------------------------------------------

        {
            auto schema = obj();
            auto props = obj();
            auto action = obj();
            action["type"] = "string";
            action["enum"] = std::vector<std::string>{"start", "cancel", "results", "progress"};
            action["description"] = "start a run, cancel one, read the marks, or how a run is going";
            props["action"] = action;
            for (auto const* k : {"from", "to"}) {
                auto p = obj();
                p["type"] = "integer";
                p["description"] = std::string("start: only the inputs from this frame ") +
                                   (k[0] == 'f' ? "on (default 0)" : "up to (default: the end)");
                props[k] = p;
            }
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"action"};

            server.addTool({
                "gucci_analyzer",
                "Drive the frame-window analyzer (Calculate): start it on the "
                "loaded macro (from/to: only the inputs in that frame range, as "
                "Calculate's own range does), cancel it, read the resulting "
                "windows (results: each mark with its window and how it was "
                "counted, plus the text labels added over the level as "
                "messages), or read how a run is going (progress: running, "
                "stage, status, index/total, progress, elapsed_s, measured, "
                "skipped, desynced, last_run_ok, and report/ok, the last "
                "message the Calculate page shows).",
                schema,
                [](matjson::Value const& a) {
                    auto& fw = Bot::get()->frameWindow();
                    auto const what = argStr(a, "action");
                    auto out = obj();

                    if (what == "start") {
                        requireFreeRun();
                        auto* pl = requireLevel();
                        // GucciBot (2026-11-01): a range, as Calculate's own does
                        // (FrameWindowAnalyzer::startRange).
                        bool const range = a.contains("from") || a.contains("to");
                        int64_t const from = argInt(a, "from", 0);
                        int64_t const to = argInt(a, "to", (int64_t)UINT32_MAX);
                        if (range && (from < 0 || to < from))
                            throw ToolError("from and to are frames, from <= to");
                        auto rep = range ? fw.startRange(pl, (uint32_t)from,
                                                         (uint32_t)std::min<int64_t>(to, UINT32_MAX))
                                         : fw.start(pl);
                        out["started"] = rep.ok;
                        out["message"] = rep.message;
                        if (range) {
                            out["from"] = from;
                            out["to"] = to;
                        }
                        return out;
                    }
                    if (what == "progress") {
                        auto* gb = GucciEngine::get();
                        out["running"] = fw.running();
                        out["capturing"] = fw.capturing();
                        out["returning"] = fw.returning();
                        if (fw.returning())
                            out["return_frame"] = (int64_t)fw.returnFrame();
                        out["stage"] = fw.stage();
                        out["status"] = fw.status();
                        out["index"] = (int64_t)fw.progressIndex();
                        out["total"] = (int64_t)fw.progressTotal();
                        out["progress"] = (double)fw.progress();
                        out["elapsed_s"] = fw.elapsedSeconds();
                        out["measured"] = (int64_t)fw.measuredCount();
                        out["skipped"] = (int64_t)fw.skippedCount();
                        out["desynced"] = (int64_t)fw.desyncCount();
                        out["last_run_ok"] = fw.lastRunOk();
                        out["report"] = gb->fwAcReport;
                        out["ok"] = gb->fwAcOk;
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
                            // GucciBot (2026-11-01): how it was counted, for the
                            // Calculate checks (framewindow.hpp, FrameWindowMark).
                            if (mk.subframe != 0.f)
                                e["subframe"] = (double)mk.subframe;
                            e["gamemode"] = std::string(1, mk.gamemode);
                            if (mk.suggestion > 0.f) {
                                // A suspicious click's "my suggestion is x frames".
                                e["suggestion"] = (double)mk.suggestion;
                                e["suggest_alignments"] = (int64_t)mk.suggestAlignments;
                                e["suggest_coverage"] = (double)mk.suggestCoverage;
                            }
                            if (mk.dependent > 0.f) {
                                e["dependent"] = (double)mk.dependent;
                                e["dependent_min"] = (double)mk.dependentMin;
                                e["dependent_max"] = (double)mk.dependentMax;
                                e["dependent_coverage"] = (double)mk.dependentCoverage;
                            }
                            if (mk.manual) {
                                e["manual"] = true;
                                e["had_auto"] = mk.hadAuto;
                                if (mk.hadAuto)
                                    e["auto_window"] = (int64_t)mk.autoWindow;
                            }
                            if (mk.setupGroup) {
                                e["setup_group"] = true;
                                e["setup_low"] = (int64_t)mk.setupLow;
                                e["setup_high"] = (int64_t)mk.setupHigh;
                            }
                            if (mk.hz != 0)
                                e["hz"] = (int64_t)mk.hz;
                            e["position"] = std::vector<double>{mk.position.x, mk.position.y};
                            if (mk.bufferAssisted)
                                e["buffer_assisted"] = true;
                            if (mk.holes > 0)
                                e["holes"] = (int64_t)mk.holes;
                            if (mk.splitWindow) {
                                e["split"] = true;
                                e["split_shift"] = (int64_t)mk.splitShift;
                            }
                            arr.push(e);
                        }
                        out["running"] = fw.running();
                        out["capturing"] = fw.capturing();
                        out["count"] = (int64_t)arr.size();
                        out["marks"] = arr;
                        // The text labels added over the level (Calculate's Add text).
                        auto messages = matjson::Value::array();
                        for (auto const& m : fw.messages()) {
                            auto e = obj();
                            e["text"] = m.text;
                            e["input"] = (int64_t)m.startIndex;
                            e["span"] = (int64_t)m.span;
                            e["scale"] = (double)m.scale;
                            e["offset_y"] = (double)m.offsetY;
                            e["enabled"] = m.enabled;
                            messages.push(e);
                        }
                        out["messages"] = messages;
                        return out;
                    }
                    throw ToolError("action must be start, cancel, results or progress");
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
            name["description"] =
                "the macro's name as gucci_list_macros reports it (no extension), or its exact file name with the "
                "extension. Several files with that name: the newest GucciBot macro of them is loaded, and the "
                "answer says which";
            props["name"] = name;
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"name"};

            server.addTool({
                "gucci_load_macro",
                "Load a macro from the replays folder, by its name or its exact file name. When several files share "
                "the name (another theme's extension, another bot's file), the newest GucciBot macro (GBR6) of them "
                "is loaded and the answer's note names it and the others.",
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
                    if (want.find_first_of("/\\") != std::string::npos)
                        throw ToolError("name is a macro in the replays folder, not a path");

                    // GucciBot (2026-11-02): an exact file name works too, and
                    // of several files with one name the newest is taken - it
                    // took whichever the folder listed first, and a name with
                    // its extension found nothing. Only GBR6 counts: load()
                    // turns anything else down and keeps the macro before,
                    // which this answered as loaded.
                    auto const dir = GucciEngine::get()->getReplayDir();
                    auto const utf8 = [](fs::path const& p) { return geode::utils::string::pathToString(p); };
                    auto const isGbr6 = [](fs::path const& p) {
                        std::ifstream f(p, std::ios::binary);
                        char magic[4] = {};
                        return static_cast<bool>(f.read(magic, 4)) && std::memcmp(magic, "GBR6", 4) == 0;
                    };
                    std::error_code ec;
                    std::vector<fs::path> named;
                    if (fs::path const exact = dir / fs::path(std::u8string(want.begin(), want.end()));
                        fs::is_regular_file(exact, ec)) {
                        named.push_back(exact);
                    } else {
                        for (auto const& it : fs::directory_iterator(dir, ec))
                            if (it.is_regular_file(ec) && utf8(it.path().stem()) == want)
                                named.push_back(it.path());
                    }
                    if (named.empty())
                        throw ToolError("no macro called " + want);

                    struct Candidate {
                        fs::path path;
                        bool gbr6 = false;
                        fs::file_time_type written{};
                    };
                    std::vector<Candidate> files;
                    for (auto const& p : named)
                        files.push_back({p, isGbr6(p), fs::last_write_time(p, ec)});
                    // GucciBot's macros first, the newest of them first.
                    std::stable_sort(files.begin(), files.end(), [](Candidate const& x, Candidate const& y) {
                        if (x.gbr6 != y.gbr6)
                            return x.gbr6;
                        return x.written > y.written;
                    });
                    auto const& chosen = files.front();
                    if (!chosen.gbr6) {
                        std::string list;
                        for (auto const& f : files)
                            list += (list.empty() ? "" : ", ") + utf8(f.path.filename());
                        throw ToolError(list + (files.size() == 1 ? " isn't" : " aren't") +
                                        " a GucciBot macro (GBR6): convert it in Saved macros on the Macro page first");
                    }

                    GucciEngine::get()->replay.load(chosen.path);
                    // The name too, as the menu's load does: the Macro page,
                    // Save and Calculate's results file go by it, and kept
                    // naming the macro loaded before.
                    GucciEngine::get()->replayName = utf8(chosen.path.stem());

                    auto out = obj();
                    out["loaded"] = utf8(chosen.path.filename());
                    out["actions"] = (int64_t)GucciEngine::get()->replay.m_actionAtom.length();
                    if (files.size() > 1) {
                        std::string others;
                        for (size_t i = 1; i < files.size(); ++i)
                            others += (others.empty() ? "" : ", ") + utf8(files[i].path.filename()) +
                                      (files[i].gbr6 ? " (older)" : " (not a GucciBot macro)");
                        out["note"] = fmt::format("{} files are named {}: loaded {}, the newest GucciBot macro of "
                                                  "them; also {}",
                                                  files.size(), want, utf8(chosen.path.filename()), others);
                    }
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
            "gucci_hitborders",
            "Hitborders (Macro page): every hitbox grown by the player's own, so the player is "
            "a point. action \"regions\" (the default): player 1 and the regions within `radius` "
            "of it right now, every kind on and blocks Exact: deadly (the center dies there), "
            "touch (the box touches something solid) and special (orbs, pads, portals), each a "
            "convex polygon in level units with the object it came from and whether it holds "
            "player 1's center. \"audit_on\" starts the accuracy audit from zero: from then on "
            "every collision pass of a real player is checked, did GD's own pass kill it against "
            "was its center in a deadly region, whether or not the borders are shown. \"audit\" "
            "reads the counts and the latest disagreements, \"audit_off\" stops it. A death "
            "noclip saves still counts as GD's. Reads only: nothing in the game changes.",
            schemaOf({{"action", "string", "regions (default), audit, audit_on or audit_off"},
                      {"radius", "integer", "regions: how far from player 1 to list, in level units (default 90)"}}),
            [](matjson::Value const& a) {
                auto const action = argStr(a, "action", "regions");
                auto* overlay = HitborderOverlay::get();
                if (action == "regions") {
                    requireLevel();
                    auto const radius = static_cast<float>(std::clamp<int64_t>(argInt(a, "radius", 90), 1, 2000));
                    return overlay->describe(radius);
                }
                if (action == "audit_on" || action == "audit_off")
                    overlay->setAudit(action == "audit_on");
                else if (action != "audit")
                    throw ToolError("action is regions, audit, audit_on or audit_off");
                auto const& au = overlay->audit();
                auto out = obj();
                out["on"] = au.on;
                out["passes"] = static_cast<int64_t>(au.passes);
                out["deaths"] = static_cast<int64_t>(au.deaths);
                out["agreed"] = static_cast<int64_t>(au.agreed);
                out["missed"] = static_cast<int64_t>(au.missed);
                out["false_alarms"] = static_cast<int64_t>(au.falseAlarms);
                auto notes = matjson::Value::array();
                for (auto const& n : au.notes)
                    notes.push(n);
                out["notes"] = notes;
                out["shown"] = overlay->settings.show;
                return out;
            },
        });

        server.addTool({
            "gucci_hitborder_probe",
            "Hitborders checked against GD itself, with nobody playing. For every object near "
            "player 1 (or between x_from and x_to), each edge Hitborders draws (every kind on, blocks "
            "Exact) is sampled a hair inside and a hair outside (eps), and at each point a hidden copy "
            "of the player, put back to the same state first (at rest, no grace timer, nothing held), "
            "runs GD's own collision pass once: did that object kill the copy, touch it (a block, a "
            "slope) or find it touching (an orb, pad, portal or trigger, which are kept from acting)? "
            "That is held against what is drawn there. Reported per object kind and per edge "
            "(\"deadly/touch top\": drawn inside / outside, which way it faces): agree, mismatch (each "
            "listed with the point), inconclusive (the pass moved the copy before the object gave any "
            "sign: the ground, a landing, a slope), and totals. The player: mode, mini and upside_down "
            "default to player 1's own, rotation to 0. Copies only: the real game is put back and "
            "nothing is saved. Refused while a render, Calculate or a pathfinder owns the run.",
            schemaOf({{"mode", "string", "cube, ship, ball, ufo, wave, robot, spider or swing (default: player 1's)"},
                      {"mini", "boolean", "mini or not (default: player 1's)"},
                      {"upside_down", "boolean", "gravity flipped (default: player 1's)"},
                      {"rotation", "number", "the icon's rotation in degrees; only rotated objects' tests use it (default 0)"},
                      {"radius", "number", "objects whose rect or circle comes within this many units of player 1 (default 90)"},
                      {"x_from", "number", "with x_to: the objects reaching into this range of x instead"},
                      {"x_to", "number", "see x_from"},
                      {"y_from", "number", "with y_to (and x_from/x_to): only those reaching into this range of y"},
                      {"y_to", "number", "see y_from"},
                      {"uid", "integer", "only this object, by unique id (anywhere in the level unless x_from/x_to)"},
                      {"kinds", "string",
                       "comma-separated: spikes, saws, blocks, slopes (hazard slopes too), hazard_slopes, orbs, pads, "
                       "portals, other (default all)"},
                      {"eps", "number", "how far either side of an edge, in level units (default 0.01)"},
                      {"per_edge", "integer", "points along each straight edge (default 3, 1 to 9; a circle's chord gets one)"},
                      {"max_points", "integer", "collision passes at most, two a point (default 8000, max 40000)"},
                      {"max_list", "integer", "mismatches listed (default 40, max 400)"},
                      {"max_ms", "integer", "wall clock limit in ms (default 4000, max 8000: the server waits 10 s)"},
                      {"dt", "number", "the pass's dt in GD's sixtieths (default: one tick at the level's rate)"}}),
            [](matjson::Value const& a) {
                requireFreeRun();
                requireLevel();
                auto num = [&](char const* key, double def) {
                    return a.contains(key) ? a[key].asDouble().unwrapOr(def) : def;
                };
                HitborderProbeArgs args;
                if (a.contains("mode")) {
                    static constexpr std::array<std::pair<char const*, int>, 11> kModes = {{
                        {"cube", 0}, {"ship", 1}, {"ball", 2}, {"ufo", 3}, {"bird", 3}, {"wave", 4},
                        {"dart", 4}, {"robot", 5}, {"spider", 6}, {"swing", 7}, {"swingcopter", 7},
                    }};
                    std::string mode = argStr(a, "mode");
                    std::transform(mode.begin(), mode.end(), mode.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    auto it = std::find_if(kModes.begin(), kModes.end(), [&](auto const& m) { return mode == m.first; });
                    if (it == kModes.end())
                        throw ToolError("mode is cube, ship, ball, ufo, wave, robot, spider or swing");
                    args.mode = it->second;
                }
                if (a.contains("mini"))
                    args.mini = argBool(a, "mini", false) ? 1 : 0;
                if (a.contains("upside_down"))
                    args.upsideDown = argBool(a, "upside_down", false) ? 1 : 0;
                args.rotation = static_cast<float>(num("rotation", 0.0));
                args.dt = static_cast<float>(num("dt", -1.0));
                args.radius = static_cast<float>(std::clamp(num("radius", 90.0), 1.0, 5000.0));
                if (a.contains("x_from") || a.contains("x_to")) {
                    args.xFrom = static_cast<float>(num("x_from", 0.0));
                    args.xTo = static_cast<float>(num("x_to", 0.0));
                    if (!(args.xFrom < args.xTo))
                        throw ToolError("x_from must be below x_to");
                    if (a.contains("y_from") || a.contains("y_to")) {
                        args.yFrom = static_cast<float>(num("y_from", 0.0));
                        args.yTo = static_cast<float>(num("y_to", 0.0));
                        if (!(args.yFrom < args.yTo))
                            throw ToolError("y_from must be below y_to");
                    }
                }
                args.uid = static_cast<int>(argInt(a, "uid", 0));
                if (a.contains("kinds")) {
                    std::string bad;
                    if (!hitborderProbeKinds(argStr(a, "kinds"), args.kinds, bad))
                        throw ToolError("unknown kind \"" + bad +
                                        "\": spikes, saws, blocks, slopes, hazard_slopes, orbs, pads, portals, other");
                }
                args.eps = std::clamp(num("eps", 0.01), 0.001, 1.0);
                args.perEdge = static_cast<int>(std::clamp<int64_t>(argInt(a, "per_edge", 3), 1, 9));
                args.maxPoints = static_cast<int>(std::clamp<int64_t>(argInt(a, "max_points", 8000), 1, 40000));
                args.maxList = static_cast<int>(std::clamp<int64_t>(argInt(a, "max_list", 40), 0, 400));
                args.maxMs = static_cast<double>(std::clamp<int64_t>(argInt(a, "max_ms", 4000), 100, 8000));
                auto out = hitborderProbe(args);
                if (out.contains("error"))
                    throw ToolError(out["error"].asString().unwrapOr("failed"));
                return out;
            },
        });

        server.addTool({
            "gucci_level_info",
            "The open level: name, id, length, object count, platformer / "
            "two-player / dual, time warp, practice mode, and the level's saved "
            "stats as GD holds them now (attempts, jumps, normal_percent, "
            "practice_percent), so a test can check a run under safe mode left "
            "them alone. start_pos: the attempt starts from a start position; "
            "start_positions: start position objects (id 31) in the level's "
            "object list; mirror {flipped: the level is mirrored as the game "
            "counts it, camera_flip: GD's m_cameraFlip, how the picture is drawn "
            "(No Mirror leaves it as it was), flipping: GD's m_levelFlipping}; "
            "gd_labels: GD's percentage and attempts labels {exists, shown, "
            "text, box} and progress_bar {exists, shown}, as drawn now.",
            obj(),
            [](matjson::Value const&) {
                auto* pl = requireLevel();
                auto* gb = GucciEngine::get();
                auto out = obj();
                // GucciBot (2026-11-01): start positions and the mirror, for
                // the tests of the start position warning and No Mirror.
                out["start_pos"] = pl->m_startPosObject != nullptr;
                int64_t startPositions = 0;
                if (pl->m_objects)
                    for (auto* o : CCArrayExt<GameObject*>(pl->m_objects))
                        if (o && o->m_objectID == 31)
                            startPositions++;
                out["start_positions"] = startPositions;
                {
                    auto mirror = obj();
                    // No Mirror sets this one alone (toggleFlipped,
                    // hook_gjbasegamelayer.cpp), so the picture stays as it was.
                    mirror["flipped"] = pl->m_gameState.m_unkBool10;
                    if (auto const f = cameraFlipOf(pl, 0))
                        mirror["camera_flip"] = (double)*f;
                    if (auto const f = flipProgressOf(pl->m_gameState, 0))
                        mirror["flipping"] = (double)*f;
                    out["mirror"] = mirror;
                }
                // GD's percentage and attempt labels and its progress bar, as
                // drawn (Hide percentage / Hide attempts hide them).
                out["gd_labels"] = scene::gdLabels(pl);
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

        // GucciBot (2026-11-01): H0 of the heavy three (LIMBO, ORBIT,
        // Heliopolis), before anything is recorded: what the World can't see
        // in a level, and what the real game costs a tick there.
        server.addTool({
            "gucci_level_census",
            "What the open level asks of Absense's World, read-only. kinds: every trigger "
            "kind present (the ones the World doesn't run first): count, how they fire "
            "(walk, spawn, touch), their x range, the groups they name and the collidable "
            "objects in those groups (hazards; solids: blocks, slopes, breakables; specials: "
            "portals, pads, orbs, teleports; modifiers: triggers and speed portals). "
            "unmodelled: the groups a kind the World doesn't run names, their objects binned "
            "by x. route_dependent, special_objects, live_now (what's running in the game "
            "now), the section factors and the pending write-back path a run from here takes. "
            "With real_ticks > 0, that many no-input ticks are also played for real through "
            "the judge (deaths noted, not died; the game is put back) for real_cost, the "
            "game's microseconds a tick: only with safe mode forced and nothing else owning "
            "the run. Logged as \"[AbsPF] census\".",
            schemaOf({{"real_ticks", "integer", "no-input ticks to play for real (default 0, max 3000; stops after 6 s)"},
                      {"bin", "integer", "x width of the unmodelled bins (default 1000)"},
                      {"top", "integer", "most entries in kinds and real_cost.deaths (default 20)"}}),
            [](matjson::Value const& a) {
                requireLevel();
                int const realTicks = (int)std::clamp<int64_t>(argInt(a, "real_ticks", 0), 0, 3000);
                int const bin = (int)std::clamp<int64_t>(argInt(a, "bin", 1000), 100, 100000);
                int const top = (int)std::clamp<int64_t>(argInt(a, "top", 20), 1, 500);
                // The static part is a read; playing the real game drives it.
                if (realTicks > 0) {
                    requireFreeRun();
                    if (!safemode::forced())
                        throw ToolError("refused: real_ticks plays the real game, and safe mode isn't forced for "
                                        "this launch (tools/assist.py's launch() forces it, or "
                                        "gucci_force_safe_mode)");
                }
                auto out = absense::levelCensus(realTicks, (float)bin, top);
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
                       "no height shares, the ranking alone. Either way a UFO's one-tick press is left out "
                       "where a held press plays the same, so 0 is not the search from before build -e)"}}),
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
