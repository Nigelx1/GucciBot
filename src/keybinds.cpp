#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "gui.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
using namespace geode::prelude;

// GD's standard jump bindings -- shared by Click Trainer's tracking and
// Autoclicker's input tracking below so the two can't drift apart on which
// keys count as a jump.
static bool isJumpKey(enumKeyCodes key) {
    return key == enumKeyCodes::KEY_Space || key == enumKeyCodes::KEY_Up || key == enumKeyCodes::KEY_W;
}

class $modify(GB7KeyHandler, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        auto* ui = MenuInterface::get();
        auto* gb = GucciEngine::get();
        int k = (int)key;

                if (down && !repeat && ui && ui->rebindTarget) {
            if (key == enumKeyCodes::KEY_Escape) ui->rebindTarget = nullptr;
            else {
                *ui->rebindTarget = k;
                int* tgt = ui->rebindTarget;
                ui->rebindTarget = nullptr;
                                                                                ui->saveSettings();
                geode::log::info("[GucciBot] Keybind rebound to {} and saved (ptr {})",
                                 k, (void*)tgt);
            }
            return true;
        }

        if (ImGui::GetIO().WantTextInput)
            return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);

        // Click Trainer's own click/release marks (spacebar, up arrow, W):
        // folded into this existing hook rather than a separate $modify class,
        // since a separate hook risked silently missing keys if it ended up
        // chained after the rebind early-return above (that path returns true
        // without calling the base dispatchKeyboardMSG, breaking the chain for
        // anything ordered after it). Deliberately not gated on PlayLayer::get()
        // (unlike Autoclicker's tracking below) since the click bar works
        // without a level loaded; gated on jupiterClickBarPageVisible, which
        // is recomputed fresh every frame in drawInterface (true only while
        // the page is actually rendering), NOT the sticky jupiterClickBarPageOpen
        // navigation flag -- that one only clears via its own Back button, so
        // leaving the page any other way (menu-close hotkey, switching tabs)
        // left it stuck true and ordinary gameplay jumping kept piling into
        // this history indefinitely.
        if (!repeat && gb->jupiterClickBarPageVisible && isJumpKey(key)) {
            if (down) gb->jupiterClickBarMyClicks.push_back(gb->jupiterClickBarPosSec);
            else gb->jupiterClickBarMyReleases.push_back(gb->jupiterClickBarPosSec);
        }

        // Same as above, for the general Trainer tab's own Click Trainer page.
        if (!repeat && gb->trainerClickBarPageVisible && isJumpKey(key)) {
            if (down) gb->trainerClickBarMyClicks.push_back(gb->trainerClickBarPosSec);
            else gb->trainerClickBarMyReleases.push_back(gb->trainerClickBarPosSec);
        }

        bool handled = false;
        static KeybindSet s_fallbackKeybinds;
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
            check(kb.compactMode,    [&]{ ui->compactMode = !ui->compactMode; });
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

        if (!repeat && !handled && PlayLayer::get() && isJumpKey(key)) {
            Autoclicker::get()->trackUserInput(down, false);
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};
