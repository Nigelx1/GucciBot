#pragma once

// GucciBot (2026-11-09): an MCP-only test launch, for driving another mod in a
// test game with GucciBot out of its way (Antic's Counter, anticroom's frame
// window counter, which plays macros with its own engine).
//
// Started with GUCCIBOT_MCP_ONLY=1 in a test launch (safe mode forced from the
// start: GUCCIBOT_FORCE_SAFE_MODE, tools/assist.py), GucciBot puts nothing of
// its own into the game for that launch but Assistant Access and what keeps a
// test run from counting or saving:
// - no midhook or byte patch is placed at all (hooks/util_midhook.hpp), the
//   way a launch with the Disable bot switch off does it;
// - every Geode hook it owns comes out as soon as the mod has loaded
//   (core/bot_switch.cpp, applyAtLaunch), except the menu's (ImGui's draw and
//   input, which also carry gucci_screenshot and start Assistant Access) and
//   the ones kept here: safe mode's (hacks/safemode.cpp), the test save guard
//   (hooks/test_save_guard.cpp), running on while minimized
//   (ai/background.cpp), the quiet test window (ai/quiet_window.cpp),
//   gucci_open_level_string's song (mcp/level_string_tool.cpp) and the
//   scheduler hook that runs Assistant Access's calls (core/engine_updater.cpp,
//   which steps straight through to GD while the engine is off);
// - the engine stays off (enabled false) and can't be switched back on.
//
// Never saved: the next launch without the variable is a normal one. Outside
// a test launch the variable is ignored.

#include <Geode/loader/Hook.hpp>

#include <vector>

namespace gucci::mcponly {

    // This launch is MCP-only. Read once; safe to ask during static
    // initialisation (the $execute blocks that place midhooks ask it).
    bool launch();

    // The Geode hooks an MCP-only launch leaves in.
    void keepHook(geode::Hook* hook);
    bool kept(geode::Hook* hook);
    std::vector<geode::Hook*> const& keptHooks();

    // From a $modify class's onModify: every hook of that class stays in.
    template <class Self>
    void keep(Self& self) {
        for (auto& [name, hook] : self.m_hooks)
            keepHook(hook.get());
    }

} // namespace gucci::mcponly
