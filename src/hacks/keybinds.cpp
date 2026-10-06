#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "hacks/autoclicker.hpp"
#include "trainers/trainer_core.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
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
        used |= fire(keys.onNoclip, k, [&] { gb->noclipEnabled = !gb->noclipEnabled; });
        used |= fire(keys.onTrajectory, k, [&] { gb->pathPreview = !gb->pathPreview; });
        used |= fire(keys.onHitboxes, k, [&] { gb->showHitboxes = !gb->showHitboxes; });
        used |= fire(keys.onLayoutMode, k, [&] { gb->layoutMode = !gb->layoutMode; });
        used |= fire(keys.onNoMirror, k, [&] { gb->noMirrorEffect = !gb->noMirrorEffect; });
        used |= fire(keys.onAudioPitch, k, [&] { gb->audioPitchEnabled = !gb->audioPitchEnabled; });
        used |= fire(keys.onRngLock, k, [&] { gb->rngLocked = !gb->rngLocked; });
        used |= fire(keys.onAutoclicker, k, [&] { Autoclicker::get()->enabled = !Autoclicker::get()->enabled; });
        used |= fire(keys.onIntentionalDeath, k, [&] {
            if (gb->isRecording())
                gb->updater.m_canDie = !gb->updater.m_canDie;
        });
        used |= fire(keys.onAutoFlip, k, [&] { gb->updater.m_autoFlipOnDeath = !gb->updater.m_autoFlipOnDeath; });
        used |= fire(keys.onPreventDeath, k, [&] { gb->updater.m_preventDeath = !gb->updater.m_preventDeath; });
        used |= fire(keys.onMirrorInputs, k, [&] { gb->replay.m_mirrorInputs = !gb->replay.m_mirrorInputs; });
        used |= fire(keys.onSafeMode, k, [&] { gb->protectedMode = !gb->protectedMode; });
        used |= fire(keys.onCompactMode, k, [&] { ui::setCompactMode(!ui::compactMode()); });
        return used;
    }

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

        bool handled = false;
        if (down && !repeat)
            handled = runToggles(k);
        if (!live)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);

        // Held-repeat keys: frame step and step back, only while paused.
        if (down && gb->updater.m_paused) {
            auto const& keys = ui::keys();
            if (keys.onFrameStep != 0 && k == keys.onFrameStep) {
                gb->updater.userStepForward();
                handled = true;
            }
            if (keys.onBackStep != 0 && k == keys.onBackStep && gb->updater.userStepBack())
                handled = true;
        }

        if (!repeat && !handled && PlayLayer::get() && jumpKey(key))
            Autoclicker::get()->trackUserInput(down, false);

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
#endif
