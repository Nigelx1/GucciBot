#pragma once

// GucciBot's master switch: turn the whole bot off from its own menu instead
// of the Geode mod list and a restart, and have the game run as if GucciBot
// weren't there while it is off. Asked for by Dihmaster500 (2026-10-05);
// Nigel's spec: switched off, the menu shows one switch to turn it back on,
// and nothing else.
//
// Off, GucciBot takes itself out of the game as far as it safely can:
// - every SafetyHook midhook and byte patch comes out (hooks/util_midhook.hpp:
//   setEngineCodeIn) -- Windows only, there are none anywhere else;
// - every Geode hook it owns is disabled (the $modify classes and the address
//   hooks on CCActionManager::update, UILayer::onCheck and FMOD's
//   System::update), except the few the menu needs to open and draw that one
//   switch: imgui-cocos's draw and input hooks, the keyboard hook that carries
//   the menu key (hacks/keybinds.cpp) and, on phones, the pause menu's
//   GucciBot button (ui/pause_button.cpp);
// - Assistant Access stops listening (it comes back with the bot if it was
//   on), and Click Between Frames / Superb Input Precision get back whatever
//   GucciBot paused (GucciEngine::syncInputMods).
//
// The choice is saved. Launched with it off, GucciBot loads like a stand-down
// (core/standdown.hpp): no midhook or patch is placed at all, its Geode hooks
// are disabled as soon as the mod has loaded, and the menu shows the switch.
// Turning it on then places everything for the first time.
//
// The change itself only happens with no level and no editor open. Turning
// GucciBot on or off underneath a running level would leave the level and
// the bot disagreeing: GucciBot keeps practice checkpoints in its own list
// rather than GD's (hook_playlayer.cpp, storeCheckpoint), counts frames from
// the level's start, and reads the level's length in its PlayLayer::init. So
// asked for inside a level, GucciBot stops everything it is doing straight
// away (a recording keeps what it recorded) and finishes the switch the
// moment the level closes; turning it on inside a level waits the same way.
//
// The switch runs from the ImGui draw (service(), once a frame): at that
// point the game's update for the frame is over, so none of the functions
// whose hooks come out is part-way through, and the only GucciBot hooks on
// the stack are the drawScene ones, which have already called through to GD.

namespace gucci::botswitch {

    // The saved value. Its absence means on: GucciBot ran before there was a
    // switch.
    inline constexpr const char* kSaveKey = "bot_enabled";

    // What the player asked for (the saved choice).
    bool wanted();

    // True while GucciBot is out of the game: its code and hooks taken out.
    // A stand-down for another bot (core/standdown.hpp) is not this.
    bool takenOut();

    // Wanted and in: the whole menu, the keybinds and the overlays.
    inline bool on() {
        return wanted() && !takenOut();
    }

    // From the menu. Off stops whatever GucciBot is doing at once and takes
    // it out of the game as soon as no level is open; on puts it back the
    // same way. Ignored while standing down for another bot.
    void request(bool on);

    // Why a requested change hasn't happened yet, for the menu; nullptr once
    // it has.
    const char* waitingFor();

    // Once a frame, from the ImGui draw callback (ui/shell.cpp).
    void service();

    // From GucciEngine::initialize(): launched with the switch saved off.
    void applyAtLaunch();

} // namespace gucci::botswitch
