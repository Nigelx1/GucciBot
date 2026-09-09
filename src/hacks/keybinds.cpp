#include "core/GucciBot.hpp"
#include "hacks/autoclicker.hpp"
#include "gui/gui.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
using namespace geode::prelude;

using namespace gucci;

static bool isJumpKey(enumKeyCodes key) {
    return key == enumKeyCodes::KEY_Space || key == enumKeyCodes::KEY_Up ||
           key == enumKeyCodes::KEY_W;
}

class $modify(GB7KeyHandler, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        auto* ui = MenuInterface::get();
        auto* gb = GucciEngine::get();
        int k = (int)key;

        if (down && !repeat && ui && ui->rebindTarget) {
            if (key == enumKeyCodes::KEY_Escape)
                ui->rebindTarget = nullptr;
            else {
                *ui->rebindTarget = k;
                int* tgt = ui->rebindTarget;
                ui->rebindTarget = nullptr;
                ui->saveSettings();
                geode::log::info(
                    "[GucciBot] Keybind rebound to {} and saved (ptr {})", k, (void*)tgt);
            }
            return true;
        }

        if (ImGui::GetIO().WantTextInput)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);

        if (!repeat && gb->jupiterClickBarPageVisible && isJumpKey(key)) {
            if (down)
                gb->jupiterClickBarMyClicks.push_back(gb->jupiterClickBarPosSec);
            else
                gb->jupiterClickBarMyReleases.push_back(gb->jupiterClickBarPosSec);
        }

        if (!repeat && gb->trainerClickBarPageVisible && isJumpKey(key)) {
            if (down)
                gb->trainerClickBarMyClicks.push_back(gb->trainerClickBarPosSec);
            else
                gb->trainerClickBarMyReleases.push_back(gb->trainerClickBarPosSec);
        }

        bool handled = false;
        static KeybindSet s_fallbackKeybinds;
        auto& kb = ui ? ui->keybinds : s_fallbackKeybinds;

        if (down && !repeat && ui) {
            auto check = [&](int hotkey, auto fn) {
                if (hotkey != 0 && k == hotkey) {
                    handled = true;
                    fn();
                }
            };

            check(kb.menu, [&] {
                // Nigel's ask (2026-09-05): the startup notification is easy
                // to miss if you weren't looking right when the game
                // launched -- re-show it every time someone actually tries
                // to open the menu while ToastyReplay Lite is still missing.
                // Caught in testing (2026-09-06): the menu was still opening
                // underneath the popup, which defeats the whole point of
                // GucciBot "standing down" -- while TTR is missing, the menu
                // shouldn't open at all, just re-show the popup.
                if (gb->ttrRequirementMissing) {
                    GucciEngine::showTtrMissingNotification();
                    return;
                }
                if (gb->ttrEnabledConflict) {
                    GucciEngine::showTtrEnabledNotification();
                    return;
                }
                if (!ui->shown) {
                    ui->shown = true;
                    ui->anim.opening = true;
                    ui->anim.openProgress = 0.f;
                } else {
                    ui->anim.closing = true;
                    ui->anim.opening = false;
                }
            });

            auto* pl = PlayLayer::get();
            check(kb.frameAdvance, [&] {
                if (pl)
                    gb->updater.togglePaused();
            });
            check(kb.replayToggle, [&] {
                if (gb->isPlaying())
                    gb->setMode(GucciEngine::Mode::Idle);
                else if (!gb->replay.m_actionAtom.empty())
                    gb->setMode(GucciEngine::Mode::Playing);
            });
            check(kb.noclip, [&] {
                gb->noclipEnabled = !gb->noclipEnabled;
            });
            check(kb.trajectory, [&] {
                gb->pathPreview = !gb->pathPreview;
            });
            check(kb.hitboxes, [&] {
                gb->showHitboxes = !gb->showHitboxes;
            });
            check(kb.layoutMode, [&] {
                gb->layoutMode = !gb->layoutMode;
            });
            check(kb.noMirror, [&] {
                gb->noMirrorEffect = !gb->noMirrorEffect;
            });
            check(kb.audioPitch, [&] {
                gb->audioPitchEnabled = !gb->audioPitchEnabled;
            });
            check(kb.rngLock, [&] {
                gb->rngLocked = !gb->rngLocked;
            });
            check(kb.autoclicker, [&] {
                Autoclicker::get()->enabled = !Autoclicker::get()->enabled;
            });
            check(kb.intentionalDeath, [&] {
                if (gb->isRecording())
                    gb->updater.m_canDie = !gb->updater.m_canDie;
            });
            check(kb.autoFlip, [&] {
                gb->updater.m_autoFlipOnDeath = !gb->updater.m_autoFlipOnDeath;
            });
            check(kb.preventDeath, [&] {
                gb->updater.m_preventDeath = !gb->updater.m_preventDeath;
            });
            check(kb.mirrorInputs, [&] {
                gb->replay.m_mirrorInputs = !gb->replay.m_mirrorInputs;
            });
            check(kb.safeMode, [&] {
                gb->protectedMode = !gb->protectedMode;
            });
            check(kb.compactMode, [&] {
                ui->compactMode = !ui->compactMode;
                Mod::get()->setSavedValue("ui_compact_mode", ui->compactMode);
            });
        }

        if (down && ui) {
            auto& kb2 = ui->keybinds;
            if (kb2.frameStep != 0 && k == kb2.frameStep && gb->updater.m_paused) {
                handled = true;
                gb->updater.m_stepOnce_ = true;
            }
            if (kb2.backStep != 0 && k == kb2.backStep && gb->updater.m_paused &&
                gb->updater.m_backwardsStepping) {
                handled = true;
                gb->updater.backwardsStep(1);
            }
        }

        if (!repeat && !handled && PlayLayer::get() && isJumpKey(key)) {
            Autoclicker::get()->trackUserInput(down, false);
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};
