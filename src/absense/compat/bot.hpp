#pragma once

// Stands in for Absense's bot/bot.hpp, bot/updater.hpp, replay/system.hpp,
// checkpoint/*.hpp, render/renderer.hpp and assist/autoclicker.hpp.
//
// Absense is built on Silicate, and so is GucciBot's engine, so most of what
// its code asks for already exists under the same member names: Bot (in
// analysis/ac/shim.hpp) forwards updater(), replaySystem() and practiceFix()
// to GucciEngine. This header adds the rest under Absense's type names.

#include "absense/compat/settings.hpp"
#include "core/GucciBot.hpp"
#include "core/action_types.hpp"
#include "core/checkpoint_player.hpp"
#include "render/renderer.hpp"

namespace slc {
    using ActionAtom = gucci::gb::ActionAtom;
}

// Silicate spells the engine's parts with these names.
using BotUpdater = gucci::GucciUpdater;
using ReplaySystem = gucci::GucciReplaySystem;
using PracticeFix = gucci::GucciPracticeFix;

// Absense's renderer is Silicate's; GucciBot's is the same renderer ported.
struct Renderer {
    static Renderer* get() {
        static Renderer inst;
        return &inst;
    }
    bool isRecording() const { return gucci::SLRenderer::get()->isRecording(); }
};

// Absense's trajectory asks its autoclicker whether it is driving the player,
// for the paths it DRAWS (the pathfinder never uses those). GucciBot's own
// autoclicker has a different shape and its trajectory preview is its own, so
// this is Absense's autoclicker as far as that question goes: switched off.
class AbsAutoclicker {
public:
    struct PlayerSettings {
        bool enabled = true;
        uint32_t holdTicks = 1;
        uint32_t releaseTicks = 1;
        uint32_t clicksPerHold = 1;
        bool swifts = false;
        bool blackOrbUfo = false;
        bool isClicking() const { return false; }
    };
    bool m_enabledFlag = false;
    SLValuePtr<bool> m_enabled = SLValue<bool>::create("autoclicker.enabled", &m_enabledFlag);
    PlayerSettings m_player1;
    PlayerSettings m_player2;
    // Never reached: m_enabled stays false, and the trajectory only asks the
    // autoclicker to drive a copy when it is on.
    void update(PlayerObject*, PlayerSettings&, bool, int, bool) {}
};
