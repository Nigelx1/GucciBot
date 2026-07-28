// keybinds.cpp — GucciBot 10.0
#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "gui.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
using namespace geode::prelude;

class $modify(GB7KeyHandler, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        auto* ui = MenuInterface::get();
        auto* gb = GucciEngine::get();
        int k = (int)key;

        // Rebind mode
        if (down && !repeat && ui && ui->rebindTarget) {
            if (key == enumKeyCodes::KEY_Escape) ui->rebindTarget = nullptr;
            else {
                *ui->rebindTarget = k;
                int* tgt = ui->rebindTarget;
                ui->rebindTarget = nullptr;
                // v10.1: persist immediately. Save the full settings AND log it
                // so we can confirm the write actually happens. If keybinds still
                // don't survive a restart after this, the issue is in load order,
                // not the save.
                ui->saveSettings();
                geode::log::info("[GucciBot] Keybind rebound to {} and saved (ptr {})",
                                 k, (void*)tgt);
            }
            return true;
        }

        if (ImGui::GetIO().WantTextInput)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);

        bool handled = false;
        static KeybindSet s_fallbackKeybinds;          // never read: all kb uses are ui-guarded
        auto& kb = ui ? ui->keybinds : s_fallbackKeybinds;

        if (down && !repeat && ui) {
            auto check = [&](int hotkey, auto fn) {
                if (hotkey != 0 && k == hotkey) { handled = true; fn(); }
            };

            check(kb.menu, [&]{
                if (!ui->shown) { ui->shown = true; ui->anim.opening = true; ui->anim.openProgress = 0.f; }
                else { ui->anim.closing = true; ui->anim.opening = false; }
            });

            auto* pl = PlayLayer::get();
            check(kb.frameAdvance, [&]{ if (pl) gb->updater.togglePaused(); });
            check(kb.replayToggle, [&]{
                if (gb->isPlaying()) gb->setMode(GucciEngine::Mode::Idle);
                else if (!gb->replay.m_actionAtom.empty()) gb->setMode(GucciEngine::Mode::Playing);
            });
            check(kb.noclip,         [&]{ gb->noclipEnabled = !gb->noclipEnabled; });
            check(kb.trajectory,     [&]{ gb->pathPreview = !gb->pathPreview; });
            check(kb.hitboxes,       [&]{ gb->showHitboxes = !gb->showHitboxes; });
            check(kb.layoutMode,     [&]{ gb->layoutMode = !gb->layoutMode; });
            check(kb.noMirror,       [&]{ gb->noMirrorEffect = !gb->noMirrorEffect; });
            check(kb.audioPitch,     [&]{ gb->audioPitchEnabled = !gb->audioPitchEnabled; });
            check(kb.rngLock,        [&]{ gb->rngLocked = !gb->rngLocked; });
            check(kb.autoclicker,    [&]{ Autoclicker::get()->enabled = !Autoclicker::get()->enabled; });
            check(kb.intentionalDeath, [&]{ if (gb->isRecording()) gb->updater.m_canDie = !gb->updater.m_canDie; });
            check(kb.autoFlip,       [&]{ gb->updater.m_autoFlipOnDeath = !gb->updater.m_autoFlipOnDeath; });
            check(kb.preventDeath,   [&]{ gb->updater.m_preventDeath = !gb->updater.m_preventDeath; });
            check(kb.mirrorInputs,   [&]{ gb->replay.m_mirrorInputs = !gb->replay.m_mirrorInputs; });
            check(kb.safeMode,       [&]{ gb->protectedMode = !gb->protectedMode; });
        }

        if (down && ui) {
            auto& kb2 = ui->keybinds;
            if (kb2.frameStep != 0 && k == kb2.frameStep && gb->updater.m_paused) {
                handled = true; gb->updater.m_stepOnce_ = true;
            }
            if (kb2.backStep != 0 && k == kb2.backStep &&
                gb->updater.m_paused && gb->updater.m_backwardsStepping) {
                handled = true; gb->updater.backwardsStep(1);
            }
        }

        if (!repeat && !handled && PlayLayer::get()) {
            auto k2 = key;
            if (k2==enumKeyCodes::KEY_Space || k2==enumKeyCodes::KEY_Up || k2==enumKeyCodes::KEY_W)
                Autoclicker::get()->trackUserInput(down, false);
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};
