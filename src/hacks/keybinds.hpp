#pragma once

#include <cstdint>

// GucciBot's keyboard hook (hacks/keybinds.cpp), as far as Assistant Access
// needs it (mcp/keys_tool.cpp): whether a key went on to the game.

namespace gucci::keybinds {

    // Key-downs (not repeats) of that key code the hook handed on to the game
    // since launch, and the ones it kept (the Frame Editor's Ctrl keys, a
    // key-capture control's key). 0 on iOS (no hook there).
    uint32_t handedOn(int keyCode);
    uint32_t kept(int keyCode);

} // namespace gucci::keybinds
