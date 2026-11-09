#pragma once

// Other bots GucciBot can't run alongside. When one of them is enabled,
// GucciBot stands down for the whole launch (Geode can't toggle a mod without
// a restart): initialize() switches it off and says why, the menu key shows
// the same message, and nothing that writes into the game's code goes in at
// all -- util_midhook() and the $execute blocks that place byte patches and
// address hooks check standDownBot() first.
//
// - ToastyReplay Lite: running both crashed macro playback for a real user
//   (anticroom's Discord report, 2026-09-08).
// - Silicate: GucciBot's engine is a port of it, so both place the same
//   SafetyHook midhooks and byte patches at the same game addresses (every
//   one GucciBot uses is in Silicate's bot/updater.cpp, hooks/GJBaseGameLayer.cpp
//   or hooks/PlayerObject.cpp). Two midhooks on one instruction is the likely
//   cause of a crash a user sent on 2026-10-03: an access violation writing to
//   0x0 from code in no module, with both mods enabled.
// - Antic's Counter (anticroom's frame-window counter): it plays macros with
//   an engine of its own (its about.md: ToastyReplay's v2.2.4), which hooks
//   the main game update and much of what GucciBot's engine hooks. The two
//   were never run together, and Nigel decided (2026-11-09) that GucciBot
//   stands down for it too. A test that drives the counter launches GucciBot
//   MCP-only (core/mcp_only.hpp), which keeps Assistant Access either way.
//
// This runs while GucciBot loads, which can be before the other mod has, so
// it asks whether the mod is set to load (Mod::shouldLoad, the mod list's
// on/off switch), not whether it already has.

#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Mod.hpp>

namespace gucci {

    struct StandDownBot {
        const char* id;
        const char* name;
        const char* message; // the popup's text after "<name> is enabled, "
    };

    inline const StandDownBot* standDownBot() {
        static constexpr StandDownBot kBots[] = {
            {"toastexgd.toastyreplay-lite",
             "ToastyReplay Lite",
             "and the two mods can't both run live at the same time (this is what's behind crashes on "
             "macro playback). Open the Geode mod list, toggle ToastyReplay Lite off or uninstall it, and "
             "restart. GucciBot rides again from there."},
            {"peony.silicate",
             "Silicate",
             "and GucciBot's engine is built from Silicate's, so with both on they patch the same spots in "
             "the game's code, which crashes the game. GucciBot left the game alone this launch. Open the "
             "Geode mod list, turn one of the two off, and restart."},
            {"anticroom.antics-counter",
             "Antic's Counter",
             "and it plays macros with an engine of its own that hooks the same parts of the game as "
             "GucciBot's. GucciBot left the game alone this launch. Open the Geode mod list, turn one of "
             "the two off, and restart."},
        };
        static const StandDownBot* const s_found = []() -> const StandDownBot* {
            for (auto const& bot : kBots) {
                auto* mod = geode::Loader::get()->getInstalledMod(bot.id);
                if (mod && mod->shouldLoad() && !mod->targetsOutdatedVersion())
                    return &bot;
            }
            return nullptr;
        }();
        return s_found;
    }

} // namespace gucci
