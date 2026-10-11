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
//   touch the checkpoints for a harmless reason (a music sync fix), and one
//   that keeps its hook in while it's loaded can't be told apart from one
//   whose fix is switched off, so this warning can be dismissed per mod (and
//   brought back from the Macro page). Mega Hack's Practice Bug Fix is also
//   read from its own switch.
// - Frame extrapolation: read from the switch of the two mods that have it,
//   Mega Hack (asked live through its exported isHackEnabled on Windows,
//   read from its save file elsewhere) and QOLMod. The drawing code it
//   changes is hooked by too many harmless mods (Click Between Frames,
//   speedhacks, overlays) to tell from the code alone.
// - Other bots: TCBot, ToastyReplay PRO and original ToastyReplay, Astral,
//   xdBot, yBot and 40kbot, when loaded. A warning, not a stand-down:
//   core/standdown.hpp is for the bots known to crash alongside GucciBot.

#include <string>
#include <vector>

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
    // The dismissed mods' names, comma-separated ("" if none), and undoing
    // every "It's fine".
    std::string dismissedNames();
    void undismissAll();

    // The popup, once a launch, from the first main menu's init (queued to
    // show once the menu is up). Also logs what the check found.
    void showLaunchPopupOnce();

    // A short toast when recording or playback starts, at most once a minute.
    void toastIfNeeded();

} // namespace gucci::setupcheck
