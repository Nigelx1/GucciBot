#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "hacks/autoclicker.hpp"
#include "trainers/trainer_core.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <chrono>
#ifndef GEODE_IS_IOS
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif

using namespace geode::prelude;
using namespace gucci;

namespace {

    bool jumpKey(enumKeyCodes key) {
        return key == enumKeyCodes::KEY_Space || key == enumKeyCodes::KEY_Up || key == enumKeyCodes::KEY_W;
    }

    template <class F>
    bool fire(int bound, int pressed, F&& action) {
        if (bound == 0 || bound != pressed)
            return false;
        action();
        return true;
    }

    // A toggle by key, saved under the key its menu switch saves to, so it
    // outlasts a restart the same way (Nigel, 2026-10-28).
    void flipSaved(bool& value, const char* saveKey) {
        value = !value;
        Mod::get()->setSavedValue<bool>(saveKey, value);
    }

    // One-shot toggles: the key is consumed when it matches.
    bool runToggles(int k) {
        auto* gb = GucciEngine::get();
        auto& keys = ui::keys();
        bool used = false;
        used |= fire(keys.onMenu, k, [&] {
            if (gb->standingDown) {
                GucciEngine::showStandDownNotification();
                return;
            }
            ui::toggleOpen();
        });
        // Switched off (core/bot_switch.hpp), the menu key is the only one
        // left: it opens the menu that holds the switch.
        if (!botswitch::on())
            return used;
        used |= fire(keys.onFrameAdvance, k, [&] {
            if (PlayLayer::get())
                gb->updater.togglePaused();
        });
        used |= fire(keys.onReplayToggle, k, [&] {
            if (gb->isPlaying())
                gb->setMode(GucciEngine::Mode::Idle);
            else if (!gb->replay.m_actionAtom.empty())
                gb->setMode(GucciEngine::Mode::Playing);
        });
        // Every setting a key flips is saved like its menu switch, so it
        // outlasts a restart. RNG lock isn't: its switch doesn't save either.
        used |= fire(keys.onNoclip, k, [&] { flipSaved(gb->noclipEnabled, "hack_noclip"); });
        used |= fire(keys.onTrajectory, k, [&] { flipSaved(gb->pathPreview, "hack_trajectory"); });
        used |= fire(keys.onHitboxes, k, [&] { flipSaved(gb->showHitboxes, "hack_hitboxes"); });
        used |= fire(keys.onLayoutMode, k, [&] { flipSaved(gb->layoutMode, "hack_layout_mode"); });
        used |= fire(keys.onNoMirror, k, [&] { flipSaved(gb->noMirrorEffect, "hack_no_mirror"); });
        used |= fire(keys.onAudioPitch, k, [&] { flipSaved(gb->updater.m_speedhackAudio, "feat_audio_pitch"); });
        used |= fire(keys.onRngLock, k, [&] { gb->rngLocked = !gb->rngLocked; });
        used |= fire(keys.onAutoclicker, k, [&] {
            auto* ac = Autoclicker::get();
            ac->enabled = !ac->enabled;
            ac->saveSettings();
        });
        used |= fire(keys.onIntentionalDeath, k, [&] {
            if (gb->isRecording())
                gb->updater.m_canDie = !gb->updater.m_canDie;
        });
        used |= fire(keys.onAutoFlip, k, [&] { flipSaved(gb->updater.m_autoFlipOnDeath, "feat_auto_flip"); });
        used |= fire(keys.onPreventDeath, k, [&] { flipSaved(gb->updater.m_preventDeath, "feat_prevent_death"); });
        used |= fire(keys.onMirrorInputs, k, [&] { flipSaved(gb->replay.m_mirrorInputs, "feat_mirror_inputs"); });
        used |= fire(keys.onSafeMode, k, [&] { flipSaved(gb->protectedMode, "hack_safe_mode"); });
        used |= fire(keys.onCompactMode, k, [&] { ui::setCompactMode(!ui::compactMode()); });
        return used;
    }

#ifdef GEODE_IS_MACOS
    // On a Mac a modifier key can reach GucciBot twice: through
    // updateModifierKeys (below) and, maybe, dispatchKeyboardMSG as well -
    // only the Windows binary has been read. One press fires its bindings once.
    bool runTogglesOnce(int k) {
        static std::chrono::steady_clock::time_point s_last[256];
        if (k <= 0 || k >= 256)
            return runToggles(k);
        auto const now = std::chrono::steady_clock::now();
        if (now - s_last[k] < std::chrono::milliseconds(150))
            return true;
        s_last[k] = now;
        return runToggles(k);
    }
#endif

} // namespace

#ifndef GEODE_IS_IOS // no keyboard dispatcher binding there
class $modify(GucciKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        int const k = static_cast<int>(key);
        auto* gb = GucciEngine::get();

        // A key-capture control in the menu takes the key first.
        if (down && !repeat && ui::feedKeyCapture(k))
            return true;
        if (ImGui::GetIO().WantTextInput)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);

        // Switched off (core/bot_switch.hpp), only the menu key does anything.
        bool const live = botswitch::on();

        // The trainers' click bars take the player's own jump key while they
        // run away from a level (in one, GD's handleButton reports it).
        if (live && !repeat && jumpKey(key))
            trainers::onKeyInput(down);

        // Every key goes on to the game as well, bound or not. (Whether one was
        // used only mattered to the autoclicker's old key tracking, which is
        // gone: "only while holding" reads GD's UILayer now.)
        if (down && !repeat) {
#ifdef GEODE_IS_MACOS
            runTogglesOnce(k);
#else
            runToggles(k);
#endif
        }
        if (!live)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);

        // Held-repeat keys: frame step and step back, only while paused.
        if (down && gb->updater.m_paused) {
            auto const& keys = ui::keys();
            if (keys.onFrameStep != 0 && k == keys.onFrameStep)
                gb->updater.userStepForward();
            if (keys.onBackStep != 0 && k == keys.onBackStep)
                gb->updater.userStepBack();
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }

#ifdef GEODE_IS_MACOS
    // macOS reports a modifier pressed on its own (Option, Shift, Control) as
    // a change of modifier flags, which reaches cocos here and not through
    // dispatchKeyboardMSG, so a binding on one never fired: the menu's default
    // is Left Alt, and Option on Nigel's Mac did nothing (2026-10-06). A
    // modifier going down fires its bindings once, on either side of the
    // keyboard (macOS doesn't say which), as the key does on Windows.
    void updateModifierKeys(bool shift, bool ctrl, bool alt, bool cmd) {
        bool const was[3] = {m_bShiftPressed, m_bControlPressed, m_bAltPressed};
        CCKeyboardDispatcher::updateModifierKeys(shift, ctrl, alt, cmd);
        bool const now[3] = {shift, ctrl, alt};
        // generic, left and right key codes, as cocos numbers them on Windows
        static constexpr int kCodes[3][3] = {{0x10, 0xA0, 0xA1}, {0x11, 0xA2, 0xA3}, {0x12, 0xA4, 0xA5}};
        for (int m = 0; m < 3; m++) {
            if (!now[m] || was[m])
                continue;
            // A key-capture control in the menu takes it first, as the left key.
            if (gucci::ui::feedKeyCapture(kCodes[m][1]) || ImGui::GetIO().WantTextInput)
                continue;
            for (int code : kCodes[m])
                runTogglesOnce(code);
        }
    }
#endif
};
#endif
