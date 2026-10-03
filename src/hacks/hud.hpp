#pragma once

// The HUD: live readouts in a corner of the level (frame, TPS, position,
// velocities, bot state and the debug readouts). Drawn by hud.cpp as one
// cocos label on the PlayLayer's UI layer, the way Silicate draws its labels
// (silicate/src/label/label.cpp, by peony, GPL-3.0 - GucciBot's licence
// too); the Hacks page card is ui/pages/hud_card.cpp.
//
// The switches live in the engine (GucciEngine::hud, a HudConfig) and are
// read every drawn frame, so a change in the card shows at once. This header
// is the one list of readouts: the HUD draws them in this order, the card
// shows a switch for each, and engine_core.cpp's loader reads each one's key
// from it, so the three can never disagree about a name.

#include "core/GucciBot.hpp"

#include <array>

namespace gucci::hud {

    // How the card groups the readouts.
    enum Group : int {
        Bot,     // GucciBot's own state
        Player,  // the player objects
        Level,   // the level and its clocks
        GroupCount
    };

    const char* groupName(Group group);

    struct Readout {
        bool HudConfig::* field;
        const char* key;     // saved value, hud_<field>
        const char* legacy;  // older saved name, read when key is absent (may be null)
        const char* label;   // the card's name for it
        const char* hint;    // the card's hint (may be null)
        Group group;
    };

    // In the order they are drawn, top to bottom.
    inline constexpr std::array<Readout, 22> kReadouts = {{
        {&HudConfig::showFrame, "hud_show_frame", "hud_showFrame", "Frame",
         "GucciBot's frame counter, the one macros are keyed on", Bot},
        {&HudConfig::showTPS, "hud_show_tps", "hud_showTPS", "TPS", "Physics ticks per second", Bot},
        {&HudConfig::showState, "hud_show_state", nullptr, "Bot state", "Idle, recording or playing, and paused",
         Bot},
        {&HudConfig::showActionIndex, "hud_show_action_index", nullptr, "Action index",
         "How far through the macro's actions playback is", Bot},
        {&HudConfig::showLastInput, "hud_show_last_input", nullptr, "Ticks since last input", nullptr, Bot},
        {&HudConfig::showIntentional, "hud_show_intentional", nullptr, "Intentional death",
         "Whether the next death is meant to happen (recording: armed; playback: expected)", Bot},
        {&HudConfig::showTickLimit, "hud_show_tick_limit", nullptr, "Tick limit",
         "Most physics ticks run in one drawn frame (Max UPR, or Dynamic UPR's measure)", Bot},

        {&HudConfig::showX, "hud_show_x", nullptr, "X position", nullptr, Player},
        {&HudConfig::showY, "hud_show_y", nullptr, "Y position", nullptr, Player},
        {&HudConfig::showXVel, "hud_show_x_vel", nullptr, "X velocity",
         "The platformer X velocity; it stays 0 in classic levels", Player},
        {&HudConfig::showYVel, "hud_show_y_vel", nullptr, "Y velocity", nullptr, Player},
        {&HudConfig::showRot, "hud_show_rot", nullptr, "Rotation", nullptr, Player},
        {&HudConfig::showSpeed, "hud_show_speed", nullptr, "Speed", "The speed portal multiplier", Player},
        {&HudConfig::showGravity, "hud_show_gravity", nullptr, "Gravity", "Normal or flipped", Player},
        {&HudConfig::showOnGround, "hud_show_on_ground", nullptr, "On ground", nullptr, Player},
        {&HudConfig::showAlive, "hud_show_alive", nullptr, "Alive or dead", nullptr, Player},
        {&HudConfig::showOrbs, "hud_show_orbs", nullptr, "Touching orbs",
         "Orbs the player is inside and has not used yet (* = multi-activate)", Player},

        {&HudConfig::showGameTick, "hud_show_game_tick", nullptr, "Game tick",
         "GD's own tick counter, separate from GucciBot's frame", Level},
        {&HudConfig::showLevelTime, "hud_show_level_time", nullptr, "Level time", nullptr, Level},
        {&HudConfig::showTimeWarp, "hud_show_time_warp", nullptr, "Time warp", nullptr, Level},
        {&HudConfig::showCheckpoints, "hud_show_checkpoints", nullptr, "Checkpoints",
         "Practice checkpoints, stored step-back frames and platformer checkpoints", Level},
        {&HudConfig::showRandom, "hud_show_random", nullptr, "Random states",
         "GD's random state, then the macro's shake and teleport states", Level},
    }};

    // One entry per HudConfig switch. Too many entries will not compile; too
    // few leaves an empty one at the end, which this catches.
    static_assert(kReadouts.back().field != nullptr);

    // The look's saved keys.
    inline constexpr const char* kKeyEnabled = "hud_enabled";
    inline constexpr const char* kKeyBigFont = "hud_big_font";
    inline constexpr const char* kKeyScale = "hud_scale";
    inline constexpr const char* kKeyOpacity = "hud_opacity";
    inline constexpr const char* kKeyAnchor = "hud_anchor";

    // Corners, in HudConfig::anchor's order.
    enum Anchor : int { TopLeft, TopRight, BottomLeft, BottomRight, AnchorCount };

    // Limits the card offers and the loader clamps to.
    inline constexpr float kMinScale = 0.25f;
    inline constexpr float kMaxScale = 3.f;
    inline constexpr float kMinOpacity = 0.1f;
    inline constexpr float kMaxOpacity = 1.f;

} // namespace gucci::hud
