#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/hitborders.hpp"
#include "hacks/keybinds.hpp"
#include "hacks/legit.hpp"
#include "hacks/safemode.hpp"
#include "trainers/trainer_core.hpp"
#include "ui/pages.hpp"
#include "ui/search.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <chrono>
#ifndef GEODE_IS_IOS
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#endif

using namespace geode::prelude;
using namespace gucci;

namespace {

    // Assistant Access's view of what went on to the game (hacks/keybinds.hpp).
    uint32_t g_handedOn[256] = {};
    uint32_t g_kept[256] = {};
    void count(uint32_t (&table)[256], int k, bool down, bool repeat) {
        if (down && !repeat && k > 0 && k < 256)
            ++table[k];
    }

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

    // The same for a cheat's key (hacks/legit.hpp): while Legit only is on it
    // only says so, and nothing changes or is saved.
    void flipCheat(bool& value, const char* saveKey) {
        if (legit::blockKey())
            return;
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
            if (legit::blockKey())
                return;
            if (PlayLayer::get())
                gb->updater.togglePaused();
        });
        used |= fire(keys.onReplayToggle, k, [&] {
            if (gb->isPlaying())
                gb->setMode(GucciEngine::Mode::Idle);
            else if (!gb->replay.m_actionAtom.empty() && !legit::blockKey())
                gb->setMode(GucciEngine::Mode::Playing);
        });
        // Every setting a key flips is saved like its menu switch, so it
        // outlasts a restart.
        used |= fire(keys.onNoclip, k, [&] { flipCheat(gb->noclipEnabled, "hack_noclip"); });
        used |= fire(keys.onTrajectory, k, [&] { flipCheat(gb->pathPreview, "hack_trajectory"); });
        used |= fire(keys.onHitboxes, k, [&] { flipCheat(gb->showHitboxes, "hack_hitboxes"); });
        used |= fire(keys.onHitborders, k, [&] {
            if (!legit::blockKey())
                HitborderOverlay::get()->toggleShown();
        });
        used |= fire(keys.onLayoutMode, k, [&] { flipCheat(gb->layoutMode, "hack_layout_mode"); });
        used |= fire(keys.onNoMirror, k, [&] { flipCheat(gb->noMirrorEffect, "hack_no_mirror"); });
        used |= fire(keys.onAudioPitch, k, [&] { flipSaved(gb->updater.m_speedhackAudio, "feat_audio_pitch"); });
        used |= fire(keys.onRngLock, k, [&] { flipSaved(gb->rngLocked, "hack_rng_lock"); });
        used |= fire(keys.onAutoclicker, k, [&] {
            if (legit::blockKey())
                return;
            auto* ac = Autoclicker::get();
            ac->enabled = !ac->enabled;
            ac->saveSettings();
        });
        used |= fire(keys.onIntentionalDeath, k, [&] {
            if (gb->isRecording())
                gb->updater.m_canDie = !gb->updater.m_canDie;
        });
        used |= fire(keys.onAutoFlip, k, [&] { flipSaved(gb->updater.m_autoFlipOnDeath, "feat_auto_flip"); });
        used |= fire(keys.onPreventDeath, k, [&] { flipCheat(gb->updater.m_preventDeath, "feat_prevent_death"); });
        used |= fire(keys.onMirrorInputs, k, [&] { flipSaved(gb->replay.m_mirrorInputs, "feat_mirror_inputs"); });
        used |= fire(keys.onSafeMode, k, [&] {
            flipSaved(gb->protectedMode, "hack_safe_mode");
            // Forced on for this launch (a test run, hacks/safemode.hpp): the
            // key still flips the user's own switch but not what's in force,
            // so it says so instead of flipping in silence.
            if (safemode::forced())
                Notification::create(gb->protectedMode
                                         ? "Safe mode switch on. Safe mode is on for this launch anyway."
                                         : "Safe mode switch off, but safe mode stays on for this launch "
                                           "(a test run turned it on).",
                                     NotificationIcon::Info)
                    ->show();
        });
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

#if GB_GEODE6 && defined(GEODE_IS_WINDOWS)
// Ctrl as the keyboard has it, on Geode 6 for Windows (keybinds.hpp,
// ctrlDown). Neither of the other two places to read it can be trusted there
// (Geode 6.0.0-alpha.2 loader/src/platform/windows/input.cpp,
// handleKeyboardEvent; imgui-cocos 934958c src/hooks.cpp):
// - ImGui's Ctrl sticks down after Ctrl is let go: Geode puts Control in the
//   modifiers of a Ctrl key's own event, its release too, and imgui-cocos
//   hands ImGui those modifiers on every key going up.
// - The dispatcher's misses a release imgui-cocos takes (while ImGui wants
//   the keyboard, a widget held or a box typed in): Geode sets the
//   dispatcher's modifiers only for a key it hands on.
// So this listens where every key arrives first, KeyboardInputEvent, ahead of
// imgui-cocos (a lower number goes first), and never stops a key. A Ctrl key
// says itself whether it's down; any other key carries Geode's own record of
// the modifiers held (its KeyStateTracker), which puts the two right again.
// Shift is read the same way (shiftDown), for the same two reasons.
//
// It also mends ImGui's own Ctrl, Shift and Alt (the post-landing review of
// 900213e): on the release of the last Ctrl key held, the event's modifiers
// lose Control before imgui-cocos reads them, and the same for Shift and Alt,
// so ImGui's io.KeyCtrl/KeyShift/KeyAlt go up with the key. Before, a Ctrl+Z
// or a plain Ctrl tap left io.KeyCtrl set until some other key went up: the
// menu didn't scroll with the wheel and a click on a slider opened it as a
// text box; after Shift the wheel scrolled sideways everywhere. Geode copies
// back only the event's key and action, and hands the dispatcher its own
// record of the modifiers, so only what later listeners read changes.
namespace {
    bool g_ctrlLeft = false, g_ctrlRight = false;
    bool g_shiftLeft = false, g_shiftRight = false;  // the same for Shift (shiftDown)
    bool g_altLeft = false, g_altRight = false;      // and Alt (only to mend ImGui's)

    void drop(KeyboardModifier& mods, uint8_t bit) {
        mods.value = static_cast<uint8_t>(mods.value & ~bit);
    }

    // A Ctrl or Shift key says itself whether it's down; any other key
    // carries whether that modifier is held, which puts the two right again.
    void track(bool& left, bool& right, bool held) {
        if (!held)
            left = right = false;
        else if (!left && !right)
            left = true;
    }
}

$execute {
    KeyboardInputEvent().listen(
        [](KeyboardInputData& d) {
            bool const down = d.action != KeyboardInputData::Action::Release;
            switch (d.key) {
                case enumKeyCodes::KEY_LeftControl:
                case enumKeyCodes::KEY_Control:
                    g_ctrlLeft = down;
                    break;
                case enumKeyCodes::KEY_RightContol:
                    g_ctrlRight = down;
                    break;
                case enumKeyCodes::KEY_LeftShift:
                case enumKeyCodes::KEY_Shift:
                    g_shiftLeft = down;
                    break;
                case enumKeyCodes::KEY_RightShift:
                    g_shiftRight = down;
                    break;
                case enumKeyCodes::KEY_LeftMenu:
                case enumKeyCodes::KEY_Alt:
                    g_altLeft = down;
                    break;
                case enumKeyCodes::KEY_RightMenu:
                    g_altRight = down;
                    break;
                default:
                    break;
            }
            bool const ctrlKey = d.key == enumKeyCodes::KEY_LeftControl || d.key == enumKeyCodes::KEY_Control ||
                                 d.key == enumKeyCodes::KEY_RightContol;
            bool const shiftKey = d.key == enumKeyCodes::KEY_LeftShift || d.key == enumKeyCodes::KEY_Shift ||
                                  d.key == enumKeyCodes::KEY_RightShift;
            bool const altKey = d.key == enumKeyCodes::KEY_LeftMenu || d.key == enumKeyCodes::KEY_Alt ||
                                d.key == enumKeyCodes::KEY_RightMenu;
            if (!ctrlKey)
                track(g_ctrlLeft, g_ctrlRight, static_cast<bool>(d.modifiers & KeyboardModifier::Control));
            if (!shiftKey)
                track(g_shiftLeft, g_shiftRight, static_cast<bool>(d.modifiers & KeyboardModifier::Shift));
            if (!altKey)
                track(g_altLeft, g_altRight, static_cast<bool>(d.modifiers & KeyboardModifier::Alt));
            // The last one of a modifier let go: it isn't held on its own
            // release (Geode says it is), for imgui-cocos after this.
            if (!down) {
                if (ctrlKey && !g_ctrlLeft && !g_ctrlRight)
                    drop(d.modifiers, KeyboardModifier::Control);
                if (shiftKey && !g_shiftLeft && !g_shiftRight)
                    drop(d.modifiers, KeyboardModifier::Shift);
                if (altKey && !g_altLeft && !g_altRight)
                    drop(d.modifiers, KeyboardModifier::Alt);
            }
            return ListenerResult::Propagate;
        },
        -1000)
        .leak();
}

bool gucci::keybinds::ctrlDown() {
    return g_ctrlLeft || g_ctrlRight;
}

bool gucci::keybinds::shiftDown() {
    return g_shiftLeft || g_shiftRight;
}
#else
bool gucci::keybinds::ctrlDown() {
#if defined(GEODE_IS_IOS)  // no keyboard dispatcher binding there
    return false;
#else
    // Geode 5 sets the dispatcher's modifiers right before every key it hands
    // on, and on Windows imgui-cocos takes keys only after that, inside
    // dispatchKeyboardMSG; a lost focus lets go of every key through it too.
    auto* kb = CCKeyboardDispatcher::get();
    return kb && kb->getControlKeyPressed();
#endif
}

bool gucci::keybinds::shiftDown() {
#if defined(GEODE_IS_IOS)
    return false;
#else
    // As ctrlDown, for Shift.
    auto* kb = CCKeyboardDispatcher::get();
    return kb && kb->getShiftKeyPressed();
#endif
}
#endif

uint32_t gucci::keybinds::handedOn(int keyCode) {
    return keyCode > 0 && keyCode < 256 ? g_handedOn[keyCode] : 0;
}

uint32_t gucci::keybinds::kept(int keyCode) {
    return keyCode > 0 && keyCode < 256 ? g_kept[keyCode] : 0;
}

#ifndef GEODE_IS_IOS // no keyboard dispatcher binding there
class $modify(GucciKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        int const k = static_cast<int>(key);
        // Every key not kept goes on to the game from here.
        bool const handedOn = gucciHandle(key, down, repeat);
        count(handedOn ? g_handedOn : g_kept, k, down, repeat);
        if (!handedOn)
            return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }

    // What GucciBot does with a key; false when it keeps it from the game.
    bool gucciHandle(enumKeyCodes key, bool down, bool repeat) {
        int const k = static_cast<int>(key);
        auto* gb = GucciEngine::get();

        // A key-capture control in the menu takes the key first.
        if (down && !repeat && ui::feedKeyCapture(k))
            return false;
        if (ImGui::GetIO().WantTextInput)
            return true;
        // Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z) and Ctrl+A while the Frame Editor has
        // the keyboard are its undo, redo and select all, and go no further:
        // not to GD (its level editor has a Ctrl+Z of its own), not to a
        // hotkey on that key. ImGui never sees them go down from a real
        // keyboard (ui/pages/editor.cpp, shortcuts), as with Ctrl+F below.
        if (ui::pages::editorCtrlKey(k, this->getShiftKeyPressed(), down, repeat))
            return false;
        // The same for its Delete, Backspace, Escape and Left/Right keys, but
        // only while something is selected for them to act on: with nothing
        // selected, Escape still pauses the level, the arrows still move a
        // platformer player, and a hotkey on the key still fires.
        // Ctrl, Alt and the Windows/Command key as Geode set them for this
        // key: with one held it's a shortcut, not the editor's.
        bool const otherMods = ui::ctrlHeld() || this->getAltKeyPressed() || this->getCommandKeyPressed();
        if (ui::pages::editorKey(k, down, repeat, otherMods))
            return false;
        // Ctrl+F in the menu goes to its search box (ui/search.cpp), not to a
        // hotkey bound to F: the box isn't taking typing yet as F goes down.
        // Ctrl is read from this dispatcher too (Geode sets its modifiers
        // right before every key it hands on: ui::ctrlHeld), not from ImGui
        // alone, which never sees these keys go down (ui/search.hpp,
        // ctrlFIsMine).
        if (key == enumKeyCodes::KEY_F && ui::search::ctrlFIsMine(ui::ctrlHeld())) {
            if (down && !repeat)
                ui::search::focusBox();
            return true;
        }

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
            return true;

        // Held-repeat keys: frame step and step back, only while paused.
        if (down && gb->updater.m_paused) {
            auto const& keys = ui::keys();
            if (keys.onFrameStep != 0 && k == keys.onFrameStep)
                gb->updater.userStepForward();
            if (keys.onBackStep != 0 && k == keys.onBackStep)
                gb->updater.userStepBack();
        }

        return true;
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
