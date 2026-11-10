// See test_settings.hpp. GucciBot (2026-11-01), for the test plans' batches
// (tools/assist.py): every key here is one a planned test needs.

#include "core/platform.hpp"
#include "mcp/test_settings.hpp"

#include "absense/glue.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/fw_settings_store.hpp"
#include "analysis/pathfinder.hpp"
#include "audio/bigbrrr.hpp"
#include "audio/clicks.hpp"
#include "core/GucciBot.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/hitborders.hpp"
#include "hacks/legit.hpp"
#include "hacks/safemode.hpp"
#include "mcp/mcp_server.hpp"
#include "trainers/click_indicators.hpp"
#include "trainers/trainer_core.hpp"
#if !GB_NATIVE_ENGINE
#include "core/portable_tps.hpp"
#endif

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace gucci::testset {

    using mcp::ToolError;

    namespace {

        // ---- reading a value the way its row takes it

        bool wantBool(char const* key, matjson::Value const& v) {
            if (!v.isBool())
                throw ToolError(fmt::format("{} is true or false", key));
            return v.asBool().unwrapOr(false);
        }

        double wantNumber(char const* key, matjson::Value const& v, double lo, double hi) {
            if (!v.isNumber())
                throw ToolError(fmt::format("{} is a number", key));
            double const d = v.asDouble().unwrapOr(0.0);
            if (!(d >= lo && d <= hi))
                throw ToolError(fmt::format("{} is {} to {}", key, lo, hi));
            return d;
        }

        int64_t wantInt(char const* key, matjson::Value const& v, double lo, double hi) {
            double const d = wantNumber(key, v, lo, hi);
            if (d != std::floor(d))
                throw ToolError(fmt::format("{} is a whole number", key));
            return static_cast<int64_t>(d);
        }

        std::string wantString(char const* key, matjson::Value const& v) {
            if (!v.isString())
                throw ToolError(fmt::format("{} is text (a string)", key));
            return v.asString().unwrapOr("");
        }

        // ---- the whitelist

        enum class Store {
            Field,   // a field the mod keeps in memory, saved under the key by its row
            Live,    // read straight from the save container where it's used
            Memory,  // never saved by anything (this launch's switches)
        };

        struct Key {
            char const* name;
            char const* type;  // for list: bool, int, number, text
            Store store;
            std::function<matjson::Value()> get;
            // `restoring`: reset() putting the user's value back, which may
            // be one a test couldn't set (trainer_macro_name).
            std::function<void(matjson::Value const&, bool restoring)> set;
            std::vector<char const*> alsoShield{};  // keys the shield guards with this one
        };

        GucciEngine* gb() {
            return GucciEngine::get();
        }

        // A bool the engine keeps, set as engineSwitch() sets it.
        Key engineBool(char const* name, std::function<bool&()> ref) {
            return {name, "bool", Store::Field, [ref] { return matjson::Value(ref()); },
                    [ref, name](matjson::Value const& v, bool) {
                        bool const b = wantBool(name, v);
                        ref() = b;
                        Mod::get()->setSavedValue(name, b);
                    }};
        }

        // A float the engine keeps, saved as a double (saveFloat()).
        Key engineFloat(char const* name, double lo, double hi, std::function<float&()> ref) {
            return {name, "number", Store::Field, [ref] { return matjson::Value(static_cast<double>(ref())); },
                    [ref, name, lo, hi](matjson::Value const& v, bool) {
                        float const f = static_cast<float>(wantNumber(name, v, lo, hi));
                        ref() = f;
                        Mod::get()->setSavedValue<double>(name, f);
                    }};
        }

        // A key read from the container where it's used: setting it is the
        // whole of the row's work.
        enum class LiveType { Bool, Int, Text, NumberAsText };
        Key live(char const* name, LiveType t, double lo = 0, double hi = 0) {
            char const* type = t == LiveType::Bool ? "bool" : t == LiveType::Int ? "int" : "text";
            return {name, type, Store::Live,
                    [name] {
                        auto const& c = Mod::get()->getSaveContainerConst();
                        return c.contains(name) ? c[name] : matjson::Value(nullptr);
                    },
                    [name, t, lo, hi](matjson::Value const& v, bool) {
                        auto* mod = Mod::get();
                        switch (t) {
                            case LiveType::Bool: mod->setSavedValue<bool>(name, wantBool(name, v)); break;
                            case LiveType::Int: mod->setSavedValue<int64_t>(name, wantInt(name, v, lo, hi)); break;
                            case LiveType::Text: mod->setSavedValue<std::string>(name, wantString(name, v)); break;
                            case LiveType::NumberAsText: {
                                // Saved as text, read with numFromString
                                // (renderer.cpp, render_seconds_after).
                                double const d =
                                    v.isString() ? geode::utils::numFromString<double>(wantString(name, v)).unwrapOr(-1.0)
                                                 : wantNumber(name, v, lo, hi);
                                if (!(d >= lo && d <= hi))
                                    throw ToolError(fmt::format("{} is {} to {}", name, lo, hi));
                                mod->setSavedValue<std::string>(name, fmt::format("{}", d));
                                break;
                            }
                        }
                    }};
        }

        // test_mute's state: the master group's volume before the mute.
        bool g_muted = false;
        float g_volumeBefore = 1.f;

        FMOD::ChannelGroup* masterGroup() {
            auto* engine = FMODAudioEngine::sharedEngine();
            FMOD::ChannelGroup* group = nullptr;
            if (engine && engine->m_system)
                engine->m_system->getMasterChannelGroup(&group);
            return group;
        }

        std::string g_conflictTestId;

        // The keys that turn on something safe mode counts as a cheat, and
        // whether `value` would (hacks/legit.hpp: refused while Legit only is
        // on, as its switch is greyed out in the menu).
        bool turnsOnCheat(std::string const& name, matjson::Value const& value) {
            if (name == "eng_tick_rate" || name == "eng_speed") {
                double const want = name == "eng_tick_rate" ? 240.0 : 1.0;
                return value.isNumber() && std::abs(value.asDouble().unwrapOr(want) - want) > 1e-6;
            }
            static constexpr std::string_view kSwitches[] = {
                "hack_force_platformer", "hack_no_mirror",   "ind_enabled",         "hack_hitboxes",
                "hack_hitborders",       "show_macro_path",  "autoclicker_enabled", "ci_enabled",
                "trainer_any_ghost",     "trainer_any_best_ghost",
            };
            for (auto k : kSwitches)
                if (name == k)
                    return value.isBool() && value.asBool().unwrapOr(false);
            return false;
        }

        std::vector<Key> const& keys() {
            static std::vector<Key> const k = [] {
                std::vector<Key> v;
                auto e = [] { return gb(); };

                // Hacks (Hacks > Gameplay and Visual).
                v.push_back(engineBool("hack_force_platformer", [e]() -> bool& { return e()->hackForcePlatformer; }));
                v.push_back(engineBool("hack_no_mirror", [e]() -> bool& { return e()->noMirrorEffect; }));
                v.push_back(engineBool("hack_no_mirror_recording_only",
                                       [e]() -> bool& { return e()->noMirrorRecordingOnly; }));
                v.push_back(engineBool("hack_respawn_instant", [e]() -> bool& { return e()->hackRespawnInstant; }));
                v.push_back(engineBool("hack_auto_retry", [e]() -> bool& { return e()->hackAutoRetry; }));
                v.push_back(engineFloat("hack_auto_retry_delay", 0.0, 10.0,
                                        [e]() -> float& { return e()->hackAutoRetryDelay; }));
                v.push_back(engineBool("hack_hide_percentage", [e]() -> bool& { return e()->hackHidePercentage; }));
                v.push_back(engineBool("hack_no_flash", [e]() -> bool& { return e()->hackNoSpikeFlash; }));
                v.push_back(engineBool("ind_enabled", [e]() -> bool& { return e()->survivalIndicator; }));
                // RNG lock and its seed are never saved (the row has no key).
                v.push_back({"hack_rng_lock", "bool", Store::Memory, [] { return matjson::Value(gb()->rngLocked); },
                             [](matjson::Value const& val, bool) { gb()->rngLocked = wantBool("hack_rng_lock", val); }});
                v.push_back({"hack_rng_seed", "int", Store::Memory,
                             [] { return matjson::Value(static_cast<int64_t>(gb()->rngSeedVal)); },
                             [](matjson::Value const& val, bool) {
                                 gb()->rngSeedVal = static_cast<uint32_t>(
                                     wantInt("hack_rng_seed", val, 0, std::numeric_limits<uint32_t>::max()));
                             }});

                // Hitboxes and hitborders, as their cards' switches set them
                // (pages/hitboxes_card.cpp, pages/hitborders_card.cpp). The
                // borders read their settings every drawn frame, so setting
                // the field is all a change needs; the card saves every
                // hitborder key at once, here only the one is written.
                v.push_back(engineBool("hack_hitboxes", [e]() -> bool& { return e()->showHitboxes; }));
                auto hb = [] { return &HitborderOverlay::get()->settings; };
                v.push_back(engineBool("hack_hitborders", [hb]() -> bool& { return hb()->show; }));
                v.push_back(engineBool("hack_hitborders_death", [hb]() -> bool& { return hb()->onDeath; }));
                v.push_back(engineBool("hitborder_merged", [hb]() -> bool& { return hb()->merged; }));
                v.push_back(engineBool("hitborder_blocks_exact", [hb]() -> bool& { return hb()->exactBlocks; }));
                v.push_back(engineBool("hitborder_hide_used", [hb]() -> bool& { return hb()->hideUsed; }));
                v.push_back(engineBool("hitborder_point", [hb]() -> bool& { return hb()->point; }));
                v.push_back(engineBool("hitborder_path", [hb]() -> bool& { return hb()->path; }));
                v.push_back(engineBool("hitborder_darken", [hb]() -> bool& { return hb()->darken; }));
                v.push_back(engineFloat("hitborder_darkness", 0.0, 1.0, [hb]() -> float& { return hb()->darkness; }));
                v.push_back(engineFloat("hitborder_line_width", 0.25, 6.0, [hb]() -> float& { return hb()->lineWidth; }));

                // Macro path and the engine.
                v.push_back(engineBool("show_macro_path", [e]() -> bool& { return e()->showMacroPath; }));
                // The Macro page's Maintain gravity switch (recording only).
                v.push_back(engineBool("feat_maintain_gravity",
                                       [e]() -> bool& { return e()->replay.m_maintainGravity; }));
                // GD's own Flip 2-Player Controls option (game variable 0010,
                // which hasFlippedControls reads), for this launch only: reset
                // puts it back before the game closes and saves its options.
                v.push_back({"gd_flip_2p", "bool", Store::Memory,
                             [] { return matjson::Value(GameManager::get()->getGameVariable("0010")); },
                             [](matjson::Value const& val, bool) {
                                 GameManager::get()->setGameVariable("0010", wantBool("gd_flip_2p", val));
                             }});
                v.push_back(engineBool("trainer_reveal", [e]() -> bool& { return e()->trainerRevealEnabled; }));
                v.push_back(engineFloat("trainer_reveal_ahead", 0.0, 600.0,
                                        [e]() -> float& { return e()->trainerRevealBuffer; }));
                // The Speed card's row: the rate, its saved value, and while
                // recording a TPS change in the macro on this tick.
                v.push_back({"eng_tick_rate", "number", Store::Field,
                             [] { return matjson::Value(static_cast<double>(gb()->updater.m_tps)); },
                             [](matjson::Value const& val, bool) {
                                 double const tps = wantNumber("eng_tick_rate", val, 1.0, 100000.0);
#if !GB_NATIVE_ENGINE
                                 if (!gucci::portable_tps::runnable(tps))
                                     throw ToolError("eng_tick_rate: GD can't run that rate on this platform");
#endif
                                 auto* g = gb();
                                 g->updater.setTps(static_cast<float>(tps));
                                 Mod::get()->setSavedValue<double>("eng_tick_rate", tps);
                                 if (g->isRecording())
                                     g->recordTpsChange(static_cast<float>(tps));
                             }});
                v.push_back({"eng_speed", "number", Store::Field,
                             [] { return matjson::Value(static_cast<double>(gb()->updater.m_speedhack)); },
                             [](matjson::Value const& val, bool) {
                                 double const speed = wantNumber("eng_speed", val, 0.01, 20.0);
                                 gb()->updater.m_speedhack = static_cast<float>(speed);
                                 Mod::get()->setSavedValue<double>("eng_speed", speed);
                             }});
                v.push_back(engineBool("feat_mirror_inputs", [e]() -> bool& { return e()->replay.m_mirrorInputs; }));
                v.push_back(engineBool("scbf_recording", [e]() -> bool& { return e()->replay.m_scbfRecording; }));
                // No row: the saved key only (engine_core.cpp's loader).
                v.push_back(
                    engineBool("debug_tick_logs", [e]() -> bool& { return e()->updater.m_logFrameIncrements; }));
                v.push_back(engineBool("feat_audio_pitch", [e]() -> bool& { return e()->updater.m_speedhackAudio; }));
                v.push_back(engineBool("updater_highTpsPrecision",
                                       [e]() -> bool& { return e()->updater.m_highTpsPrecision; }));
                v.push_back(engineBool("updater_real_time", [e]() -> bool& { return e()->updater.m_realTime; }));
                v.push_back(engineBool("updater_dynamic_upr", [e]() -> bool& { return e()->updater.m_dynamicUpr; }));
                v.push_back({"updater_max_upr", "int", Store::Field,
                             [] { return matjson::Value(static_cast<int64_t>(gb()->updater.m_maxUPR)); },
                             [](matjson::Value const& val, bool) {
                                 int const upr = static_cast<int>(wantInt("updater_max_upr", val, 1, 60));
                                 gb()->updater.m_maxUPR = static_cast<uint32_t>(upr);
                                 Mod::get()->setSavedValue<int>("updater_max_upr", upr);
                             }});
                // Scale with the tick rate: on by default under its own key,
                // through the row's setter (a missing key is on, so an off is
                // written outright; GucciUpdater::saveVisualUpdates).
                v.push_back({"updater_visual_updates2", "bool", Store::Field,
                             [] { return matjson::Value(gb()->updater.m_useVisualUpdates); },
                             [](matjson::Value const& val, bool) {
                                 bool const on = wantBool("updater_visual_updates2", val);
                                 gb()->updater.m_useVisualUpdates = on;
                                 GucciUpdater::saveVisualUpdates(on);
                             }});
                v.push_back(engineBool("replay_block_inputs", [e]() -> bool& { return e()->replay.m_ignoreInputs; }));
                v.push_back(engineBool("feat_autosave_end", [e]() -> bool& { return e()->autosaveAtLevelEnd; }));

                // HUD.
                v.push_back(engineBool("hud_enabled", [e]() -> bool& { return e()->hud.enabled; }));
                v.push_back(engineBool("hud_show_cps", [e]() -> bool& { return e()->hud.showCPS; }));
                v.push_back(engineBool("hud_show_max_cps", [e]() -> bool& { return e()->hud.showMaxCPS; }));
                v.push_back(engineBool("hud_show_frame", [e]() -> bool& { return e()->hud.showFrame; }));

                // Autoclicker.
                auto ac = [] { return Autoclicker::get(); };
                v.push_back(engineBool("autoclicker_enabled", [ac]() -> bool& { return ac()->enabled; }));
                v.push_back(engineBool("autoclicker_only_while_holding",
                                       [ac]() -> bool& { return ac()->onlyWhileHolding; }));
                v.push_back(engineBool("autoclicker_p1_enabled", [ac]() -> bool& { return ac()->p1.enabled; }));
                v.push_back(engineBool("autoclicker_p2_enabled", [ac]() -> bool& { return ac()->p2.enabled; }));

                // Calculate and the pathfinder.
                // The Default Look row: the switch, its saved value, and when
                // it flips the markers drawn again in the new look, as the
                // Calculate page does (pages/calculate.cpp, g_look) - never
                // mid-run: the analyzer owns the screen then and redraws
                // everything when it finishes. reset() putting the user's look
                // back redraws too.
                v.push_back({"fw_default_look", "bool", Store::Field,
                             [] { return matjson::Value(gb()->fwDefaultLook); },
                             [](matjson::Value const& val, bool) {
                                 bool const on = wantBool("fw_default_look", val);
                                 bool const flipped = on != gb()->fwDefaultLook;
                                 gb()->fwDefaultLook = on;
                                 Mod::get()->setSavedValue("fw_default_look", on);
                                 auto& fw = ::Bot::get()->frameWindow();
                                 if (flipped && !fw.running())
                                     fw.clearDisplay();
                             }});
                v.push_back(engineBool("fw_default_look_bells", [e]() -> bool& { return e()->fwDefaultLookBells; }));
                for (char const* fw : {"fwac_tiers", "fwac_time_cap", "fwac_max_window", "fwac_sweep_range",
                                       "fwac_algorithm", "fwac_recovery_range", "fwac_check_past_next", "fwac_suggest",
                                       "fwac_leg_trail", "fwac_fast_subtick", "fwac_dependent_search",
                                       "fwac_show_markers"}) {
                    auto const t = fwstore::getOne(fw);
                    // The Markers row and the bands change how the windows
                    // look: on the Calculate page (pages/calculate.cpp,
                    // g_look) a change there draws the markers again, never
                    // mid-run, as fw_default_look does above. reset() putting
                    // the user's value back goes through here too.
                    std::string_view const name = fw;
                    bool const look = name == "fwac_tiers" || name == "fwac_show_markers";
                    v.push_back({fw, t.isBool() ? "bool" : t.isString() ? "text" : "int", Store::Field,
                                 [fw] { return fwstore::getOne(fw); },
                                 [fw, look](matjson::Value const& val, bool) {
                                     matjson::Value const before = look ? fwstore::getOne(fw) : matjson::Value();
                                     if (auto err = fwstore::setOne(fw, val); !err.empty())
                                         throw ToolError(err);
                                     auto& analyzer = ::Bot::get()->frameWindow();
                                     if (look && !(fwstore::getOne(fw) == before) && !analyzer.running())
                                         analyzer.clearDisplay();
                                 }});
                }
                v.push_back(live("calc_mute", LiveType::Bool));
                v.push_back(engineBool("pf_hide_search", [] () -> bool& { return Pathfinder::get()->hideSearch; }));
                // The Pathfinder page's Exact start flags and Remember
                // solutions: a search reads both from the container when it
                // starts (absense/compat/bot.cpp). Exact start flags go
                // through the row's own getter and setter: on by default, a
                // missing key reads as on, and an off is written outright
                // (setSavedValue would skip it on a missing key).
                v.push_back({"pf_exact_start_flags2", "bool", Store::Live,
                             [] { return matjson::Value(absense::exactStartFlags()); },
                             [](matjson::Value const& val, bool) {
                                 absense::setExactStartFlags(wantBool("pf_exact_start_flags2", val));
                             }});
                v.push_back(live("pf_remember_solutions", LiveType::Bool));
                // Follow the planned route: the route planner keeps it after
                // its first read (route/planner.cpp, g_guidance), so it goes
                // through the row's setter, which sets both.
                v.push_back({"pf_route_mode", "int", Store::Field,
                             [] { return matjson::Value(static_cast<int64_t>(absense::routeGuidance())); },
                             [](matjson::Value const& val, bool) {
                                 absense::setRouteGuidance(static_cast<int>(wantInt("pf_route_mode", val, 0, 2)));
                             }});
                // Not settings: the pathfinder's test switches for the
                // pending write-back (world/materialize.cpp, H1(b)), this
                // launch only. The check of every pending object a copy could
                // meet, on every tick (slow; off by default).
                v.push_back({"pf_pending_verify", "bool", Store::Memory,
                             [] { return matjson::Value(absense::pendingVerify()); },
                             [](matjson::Value const& val, bool) {
                                 absense::setPendingVerify(wantBool("pf_pending_verify", val));
                             }});
                // The cell-by-cell write-back on a level with rows (on by
                // default); off for the old pass of everything, to compare.
                v.push_back({"pf_pending_cells", "bool", Store::Memory,
                             [] { return matjson::Value(absense::pendingCells()); },
                             [](matjson::Value const& val, bool) {
                                 absense::setPendingCells(wantBool("pf_pending_cells", val));
                             }});
                // Not a setting: one more id for Calculate's conflict check.
                v.push_back({"fwac_conflict_test_id", "text", Store::Memory,
                             [] { return matjson::Value(g_conflictTestId); },
                             [](matjson::Value const& val, bool) {
                                 auto const id = wantString("fwac_conflict_test_id", val);
                                 for (char const c : id)
                                     if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
                                           c == '_'))
                                         throw ToolError("fwac_conflict_test_id is a mod id, e.g. absolllute.megahack");
                                 g_conflictTestId = id;
                                 gb()->fwAcConflictsChecked = false;  // the card looks again
                             }});

                // Click Indicators, as their card sets them.
                auto ci = [] { return &clickind::settings(); };
                auto ciInt = [ci](char const* name, int clickind::Settings::*field, int lo, int hi) {
                    return Key{name, "int", Store::Field,
                               [ci, field] { return matjson::Value(static_cast<int64_t>(ci()->*field)); },
                               [ci, field, name, lo, hi](matjson::Value const& val, bool) {
                                   ci()->*field = static_cast<int>(wantInt(name, val, lo, hi));
                                   Mod::get()->setSavedValue<int64_t>(name, ci()->*field);
                               }};
                };
                v.push_back(engineBool("ci_enabled", [ci]() -> bool& { return ci()->enabled; }));
                v.push_back(ciInt("ci_lane_side", &clickind::Settings::laneSide, 0, 2));
                v.push_back(ciInt("ci_note_shape", &clickind::Settings::noteShape, 0, 1));
                v.push_back(engineBool("ci_lane_flip", [ci]() -> bool& { return ci()->laneFlip; }));
                v.push_back(ciInt("ci_offset_ms", &clickind::Settings::offsetMs, -2000, 2000));

                // Click sounds.
                auto cs = [] { return &clicks::settings(); };
                v.push_back(engineBool("clicks_enabled", [cs]() -> bool& { return cs()->enabled; }));
                v.push_back(engineBool("clicks_p2_separate", [cs]() -> bool& { return cs()->separateP2; }));
                v.push_back(engineBool("clicks_noise", [cs]() -> bool& { return cs()->noise; }));
                v.push_back(engineFloat("clicks_noise_volume", 0.0, 2.0, [cs]() -> float& { return cs()->noiseVolume; }));
                for (auto const& pack : {std::pair{"clicks_pack", &clicks::Settings::pack},
                                         std::pair{"clicks_p2_pack", &clicks::Settings::p2Pack}}) {
                    char const* name = pack.first;
                    auto field = pack.second;
                    v.push_back({name, "text", Store::Field, [cs, field] { return matjson::Value(cs()->*field); },
                                 [cs, field, name](matjson::Value const& val, bool) {
                                     cs()->*field = wantString(name, val);
                                     Mod::get()->setSavedValue<std::string>(name, cs()->*field);
                                     clicks::refresh();  // the sounds of the pack now in use
                                 }});
                }

                // The Trainer (Any macro), its saved settings read first.
                auto tr = [] {
                    trainers::ensureLoaded();
                    return trainers::state(trainers::Kind::Any);
                };
                v.push_back(engineBool("trainer_any_ghost", [tr]() -> bool& { return tr().ghost; }));
                v.push_back(engineBool("trainer_any_best_ghost", [tr]() -> bool& { return tr().bestGhost; }));
                // The page's macro pick. A test points it only at its own
                // copies (zz_gbtest_*), which can't be one of Nigel's macros.
                v.push_back({"trainer_macro_name", "text", Store::Field,
                             [] {
                                 trainers::ensureLoaded();
                                 return matjson::Value(gb()->trainerMacroName);
                             },
                             [](matjson::Value const& val, bool restoring) {
                                 auto const name = wantString("trainer_macro_name", val);
                                 if (!restoring && name.rfind("zz_gbtest_", 0) != 0)
                                     throw ToolError("trainer_macro_name takes a test copy's name (zz_gbtest_...)");
                                 if (name.empty()) {
                                     // None was picked: the Trainer goes back to empty.
                                     gb()->trainerMacro = {};
                                     gb()->trainerMacroName.clear();
                                     Mod::get()->setSavedValue<std::string>("trainer_macro_name", "");
                                     return;
                                 }
                                 if (!trainers::selectMacro(name))
                                     throw ToolError("no macro called " + name + " in the replays folder");
                             }});

                // Render (read from the container when a render starts).
                v.push_back(live("render_output_folder", LiveType::Text));
                v.push_back(live("render_name", LiveType::Text));
                v.push_back(live("render_width", LiveType::Int, 16, 7680));
                v.push_back(live("render_height", LiveType::Int, 16, 4320));
                v.push_back(live("render_fps", LiveType::Int, 1, 480));
                v.push_back(live("render_codec", LiveType::Text));
                v.push_back(live("render_pix_fmt", LiveType::Text));
                v.push_back(live("render_file_extension", LiveType::Text));
                v.push_back(live("render_audio_codec", LiveType::Text));
                v.push_back(live("render_seconds_after", LiveType::NumberAsText, 0, 60));
                v.push_back(live("render_intro_enabled", LiveType::Bool));
                v.push_back(live("render_intro_auto_name", LiveType::Bool));
                v.push_back(live("render_intro_name", LiveType::Text));

                // The update check, and the in-flight flag a check writes.
                {
                    auto uc = live("update_check", LiveType::Bool);
                    uc.alsoShield = {"update_check_inflight"};
                    v.push_back(uc);
                }

                // Legit only (hacks/legit.hpp), saved like its switch.
                v.push_back({"legit_only", "bool", Store::Field, [] { return matjson::Value(legit::on()); },
                             [](matjson::Value const& val, bool) { legit::setOn(wantBool("legit_only", val)); }});
                // Sound: this launch's switches.
                v.push_back({"bigbrrr", "bool", Store::Memory,
                             [] { return matjson::Value(BigBrrrManager::get()->enabled); },
                             [](matjson::Value const& val, bool) {
                                 BigBrrrManager::get()->setEnabled(wantBool("bigbrrr", val));
                             }});
                v.push_back({"test_mute", "bool", Store::Memory, [] { return matjson::Value(g_muted); },
                             [](matjson::Value const& val, bool) {
                                 bool const on = wantBool("test_mute", val);
                                 auto* group = masterGroup();
                                 if (!group)
                                     throw ToolError("test_mute: FMOD's master group isn't there yet");
                                 if (on && !g_muted) {
                                     group->getVolume(&g_volumeBefore);
                                     group->setVolume(0.f);
                                 } else if (!on && g_muted) {
                                     group->setVolume(g_volumeBefore);
                                 }
                                 g_muted = on;
                             }});
                return v;
            }();
            return k;
        }

        Key const* find(std::string const& name) {
            for (auto const& k : keys())
                if (name == k.name)
                    return &k;
            return nullptr;
        }

        // What a key held before the test touched it.
        struct Held {
            matjson::Value mem;        // its value in memory
            bool hadKey = false;       // the save container had it
            matjson::Value disk;       // what it was there
            bool shieldOnly = false;   // guarded with another key, never set itself
        };
        std::map<std::string, Held> g_held;

        bool g_shielded = false;
        std::map<std::string, std::optional<matjson::Value>> g_live;  // the test's values while shielded

        Held capture(char const* name) {
            Held h;
            auto const& c = Mod::get()->getSaveContainerConst();
            h.hadKey = c.contains(name);
            if (h.hadKey)
                h.disk = c[name];
            return h;
        }

        // The container under `name` back to what it was before the test.
        void putBack(matjson::Value& c, std::string const& name, Held const& h) {
            if (h.hadKey)
                c[name] = h.disk;
            else
                c.erase(name);
        }

        bool savedStore(std::string const& name) {
            auto const* k = find(name);
            return !k || k->store != Store::Memory;  // a shield-only key is a saved one
        }

    } // namespace

    bool active() {
        return !g_held.empty();
    }

    bool muted() {
        return g_muted;
    }

    std::string const& conflictTestId() {
        return g_conflictTestId;
    }

    bool shieldForWrite() {
        if (g_shielded || g_held.empty())
            return false;
        auto& c = Mod::get()->getSaveContainer();
        g_live.clear();
        for (auto const& [name, h] : g_held) {
            if (!savedStore(name))
                continue;
            g_live[name] = c.contains(name) ? std::optional<matjson::Value>(c[name]) : std::nullopt;
            putBack(c, name, h);
        }
        g_shielded = true;
        return true;
    }

    void unshieldAfterWrite() {
        if (!g_shielded)
            return;
        auto& c = Mod::get()->getSaveContainer();
        for (auto const& [name, v] : g_live) {
            if (v)
                c[name] = *v;
            else
                c.erase(name);
        }
        g_live.clear();
        g_shielded = false;
    }

    matjson::Value set(std::string const& name, matjson::Value const& value) {
        if (!safemode::forced())
            throw ToolError("refused: safe mode isn't forced for this launch (tools/assist.py's launch() forces it, "
                            "or gucci_force_safe_mode)");
        unshieldAfterWrite();  // a DataSaved write's restore still to come
        auto const* k = find(name);
        if (!k) {
            std::string names;
            for (auto const& key : keys())
                names += (names.empty() ? "" : ", ") + std::string(key.name);
            throw ToolError("not a key gucci_test_set takes: \"" + name + "\". Keys: " + names);
        }
        if (legit::on() && turnsOnCheat(name, value))
            throw ToolError("refused: Legit only is on, and " + name + " turns on something safe mode counts as a cheat");

        // The originals are taken once, before the first set.
        std::vector<std::pair<std::string, Held>> fresh;
        if (!g_held.contains(name)) {
            Held h = capture(k->name);
            h.mem = k->get();
            fresh.emplace_back(name, std::move(h));
        }
        for (char const* extra : k->alsoShield) {
            if (!g_held.contains(extra)) {
                Held h = capture(extra);
                h.shieldOnly = true;
                fresh.emplace_back(extra, std::move(h));
            }
        }
        k->set(value, false);  // throws on a bad value, and then nothing is held
        for (auto& [n, h] : fresh)
            g_held.emplace(n, std::move(h));

        auto const& held = g_held.at(name);
        auto out = matjson::Value::object();
        out["key"] = name;
        out["value"] = k->get();
        out["user_value"] = held.mem;
        out["saved"] = k->store == Store::Memory ? matjson::Value("never (this launch only)")
                                                  : matjson::Value("shielded: saved.json keeps the user's value");
        return out;
    }

    matjson::Value reset() {
        unshieldAfterWrite();
        auto errors = matjson::Value::array();
        auto restored = matjson::Value::array();
        auto& c = Mod::get()->getSaveContainer();
        for (auto const& [name, h] : g_held) {
            auto const* k = find(name);
            if (!h.shieldOnly && k && k->store != Store::Live) {
                try {
                    k->set(h.mem, true);
                } catch (ToolError const& e) {
                    errors.push(name + ": " + e.message);
                }
            }
            if (savedStore(name))
                putBack(c, name, h);
            restored.push(name);
        }
        g_held.clear();
        auto out = matjson::Value::object();
        out["restored"] = restored;
        if (errors.size() > 0)
            out["errors"] = errors;
        return out;
    }

    matjson::Value heldKeys() {
        auto arr = matjson::Value::array();
        for (auto const& [name, h] : g_held)
            if (!h.shieldOnly)
                arr.push(name);
        return arr;
    }

    matjson::Value list() {
        unshieldAfterWrite();
        auto arr = matjson::Value::array();
        for (auto const& k : keys()) {
            auto e = matjson::Value::object();
            e["key"] = k.name;
            e["type"] = k.type;
            e["value"] = k.get();
            auto const it = g_held.find(k.name);
            e["held"] = it != g_held.end();
            if (it != g_held.end())
                e["user_value"] = it->second.mem;
            if (k.store == Store::Memory)
                e["never_saved"] = true;
            arr.push(e);
        }
        auto out = matjson::Value::object();
        out["forced"] = safemode::forced();
        out["held"] = heldKeys();
        out["keys"] = arr;
        return out;
    }

} // namespace gucci::testset

// Every write of saved.json, whoever asks for it (Geode's own save when the
// game closes, any saveData call): Geode sends DataSaved inside Mod::saveData
// just before it writes the file, so the user's values go in here and the
// test's come back on the next frame. shell.cpp's writeSavedValues shields
// around its own saveData, and then this finds it shielded already.
$on_mod(DataSaved) {
    if (gucci::testset::shieldForWrite())
        geode::Loader::get()->queueInMainThread([] { gucci::testset::unshieldAfterWrite(); });
}
