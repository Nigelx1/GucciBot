#pragma once

// The setup check: things that break GucciBot's engine when they're switched
// on alongside it. Silicate's engine (GucciBot's is a port of it) is picky
// about auto checkpoints, frame extrapolation and other mods' practice fixes
// -- from a message on Silicate's Discord, relayed by Nigel 2026-10-08 -- and
// running another bot at the same time is trouble too.
//
// GucciBot only warns: it never changes the game's settings or another mod's.
// Once a launch at the first main menu (a popup), on the Macro page (a card),
// and when recording or playback starts (a toast).
//
// How each one is found (Nigel's call: check the game itself where it can be,
// not each mod's settings):
// - Auto-Checkpoints: the game's own option, game variable 0027 (the same one
//   PlayerObject::tryPlaceCheckpoint reads, hooks/hook_playerobject.cpp).
// - Practice fixes: from the game's code. Geode keeps every mod's hooks; a
//   switched-on hook from another mod on the checkpoint save/restore
//   functions is changing what GucciBot's own practice fix changes. Matched
//   by address (GucciBot's own hooks on those functions say where they are,
//   so a hook is caught whatever its mod named it) and by name. A mod can
//   touch the checkpoints for a harmless reason (a music sync fix), so this
//   warning can be dismissed per mod. Mega Hack's Practice Bug Fix is also
//   read from its own switch, since its settings file is read anyway.
// - Frame extrapolation: read from the switch of the two mods that have it,
//   Mega Hack and QOLMod. The drawing code it changes is hooked by too many
//   harmless mods (Click Between Frames, speedhacks, overlays) to tell from
//   the code alone.
// - Other bots: TCBot and ToastyReplay PRO, switched on in the mod list. A
//   warning, not a stand-down: core/standdown.hpp is for the bots known to
//   crash alongside GucciBot.

#include <string>
#include <vector>

namespace cocos2d {
    class CCNode;
}

namespace gucci::setupcheck {

    struct Problem {
        std::string what;  // "Auto-Checkpoints is on"
        std::string fix;   // how to turn it off
        std::string modId; // set when the warning can be dismissed for this mod
    };

    // Everything that's on right now, minus dismissed mods. Worked out at most
    // once a second. Empty while GucciBot is out of the game (stood down, or
    // switched off from its own menu).
    std::vector<Problem> const& current();

    // "It's fine": stop warning about this mod's checkpoint hooks. Saved.
    void dismissMod(std::string const& id);

    // The popup, once a launch, on the first main menu (scene: that MenuLayer,
    // which the popup is added to so the menu's transition doesn't drop it).
    // Also logs what the check found, for the logs.
    void showLaunchPopupOnce(cocos2d::CCNode* scene);

    // A short toast when recording or playback starts, at most once a minute.
    void toastIfNeeded();

} // namespace gucci::setupcheck
