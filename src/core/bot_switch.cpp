// GucciBot's master switch. What it does and why it waits for the level to
// close: core/bot_switch.hpp.

#include "core/bot_switch.hpp"

#include "absense/glue.hpp"
#include "analysis/ac/shim.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/pathfinder.hpp"
#include "audio/bigbrrr.hpp"
#include "core/GucciBot.hpp"
#include "core/platform.hpp"
#include "hooks/util_midhook.hpp"
#include "render/renderer.hpp"
#include "trainers/videomode.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <string_view>
#include <vector>

using namespace geode::prelude;

namespace gucci::botswitch {

    namespace {

        bool s_wanted = true;
        bool s_takenOut = false;
        bool s_loaded = false;
        // The Geode hooks this switch disabled, so turning it back on enables
        // exactly those and nothing that was off for another reason.
        std::vector<Hook*> s_disabledHooks;

        // The hooks that stay in while GucciBot is off, by Geode's display
        // name: what the menu needs to open and draw its switch. Every one of
        // imgui-cocos's (it is built into GucciBot, so its hooks are
        // GucciBot's): the draw, keyboard, mouse wheel, text and touch input.
        // GucciBot's own keyboard hook shares the keyboard name and has to
        // stay anyway, for the menu key. imgui-cocos draws from swapBuffers on
        // Windows and iOS and from drawScene elsewhere; where it is drawScene,
        // GucciBot's own drawScene hooks share the name and stay in too, and
        // step aside themselves (engine_updater.cpp, trainer_core.cpp).
        constexpr std::string_view kMenuHooks[] = {
            "cocos2d::CCEGLView::swapBuffers",
            "cocos2d::CCEGLView::toggleFullScreen",
#if !defined(GEODE_IS_WINDOWS) && !defined(GEODE_IS_IOS)
            "cocos2d::CCDirector::drawScene",
#endif
            "cocos2d::CCKeyboardDispatcher::dispatchKeyboardMSG",
            "cocos2d::CCKeyboardDispatcher::updateModifierKeys",  // the menu key on a Mac (Option)
            "cocos2d::CCMouseDispatcher::dispatchScrollMSG",
            "cocos2d::CCIMEDispatcher::dispatchInsertText",
            "cocos2d::CCIMEDispatcher::dispatchDeleteBackward",
            "cocos2d::CCTouchDispatcher::touches",
            "PauseLayer::customSetup",  // the menu's button on a phone
        };

        bool menuNeeds(Hook* hook) {
            auto const name = hook->getDisplayName();
            for (auto const& keep : kMenuHooks)
                if (name == keep)
                    return true;
            return false;
        }

        void ensureLoaded() {
            if (s_loaded)
                return;
            s_loaded = true;
            s_wanted = Mod::get()->getSavedValue<bool>(kSaveKey, true);
        }

        bool levelOpen() {
            return PlayLayer::get() || LevelEditorLayer::get();
        }

        // Something GucciBot started is still winding down. Each was told to
        // stop when the switch went off; these finish over the next frames
        // (a render drains its encoder, Calculate walks the player back).
        bool stillStopping() {
            auto* gb = GucciEngine::get();
            auto* sl = SLRenderer::get();
            if (sl->isRecording() || sl->m_recordThread.joinable() || sl->m_shouldStart)
                return true;
            auto& analyzer = ::Bot::get()->frameWindow();
            if (analyzer.running() || analyzer.returning() || gb->fwAnalyzing)
                return true;
            if (Pathfinder::get()->active)
                return true;
            if (absense::isRunning() || absense::startPending())
                return true;
            return false;
        }

        // Everything GucciBot can be in the middle of, stopped the way its
        // own buttons stop it. Runs the moment the switch goes off, while
        // GucciBot is still fully in the game, so each one winds down through
        // its own path (the render's teardown and Calculate's walk back need
        // the hooks that are about to come out).
        void stopEverything() {
            auto* gb = GucciEngine::get();
            // Assistant Access first: a tool call waiting for the game is
            // answered before the server goes, so stopping can't wait on it,
            // and nothing it asks for starts after the rest has stopped.
            ui::suspendAssistantAccess();

            if (gb->fwAnalyzing)
                gb->cancelAnalysis();
            auto& analyzer = ::Bot::get()->frameWindow();
            if (analyzer.running())
                analyzer.cancel();
            if (analyzer.returning())
                analyzer.stopReturn();
            if (Pathfinder::get()->active)
                Pathfinder::get()->cancel();
            absense::stopPathfinder();

            auto* sl = SLRenderer::get();
            sl->m_shouldStart = false;
            sl->m_startOnNextLevel = false;
            if (sl->isRecording())
                sl->flushAndStop();

            // Recording keeps what it recorded: going idle leaves the macro
            // in memory, ready to save.
            if (!gb->isIdle())
                gb->setMode(GucciEngine::Mode::Idle);
            // Frame advance would otherwise hold the game still with no key
            // left to let it go.
            gb->updater.setPaused(false);

            // The two things GucciBot plays outside a level.
            if (gb->jupiterVideoModeEnabled) {
                gb->jupiterVideoModeEnabled = false;
                videomode::drawOverlay();  // with the mode off: stops the decoder, frees the texture
            }
            if (BigBrrrManager::get()->enabled)
                BigBrrrManager::get()->setEnabled(false);
        }

        // Every Geode hook GucciBot owns, bar the menu's, off.
        void disableHooks() {
            int count = 0;
            for (auto* hook : Mod::get()->getHooks()) {
                if (!hook->isEnabled() || menuNeeds(hook))
                    continue;
                if (auto res = hook->disable(); res.isErr()) {
                    log::error("[GucciBot] switch: hook {} would not come out: {}", hook->getDisplayName(),
                               res.unwrapErr());
                    continue;
                }
                s_disabledHooks.push_back(hook);
                ++count;
            }
            log::info("[GucciBot] switch: {} hooks out, {} left in for the menu", count,
                      Mod::get()->getHooks().size() - count);
        }

        void enableHooks() {
            for (auto* hook : s_disabledHooks) {
                if (auto res = hook->enable(); res.isErr())
                    log::error("[GucciBot] switch: hook {} would not go back in: {}", hook->getDisplayName(),
                               res.unwrapErr());
            }
            s_disabledHooks.clear();
        }

        void takeOut() {
            auto* gb = GucciEngine::get();
            gb->enabled = false;
            // Hands Click Between Frames and Superb Input Precision back
            // whatever GucciBot paused (enabled is false now, so it wants
            // nothing paused). drawScene called this every frame; with that
            // hook out, service() calls it from here on.
            gb->syncInputMods();
#if GB_NATIVE_ENGINE
            setEngineCodeIn(false);
#endif
            disableHooks();
            s_takenOut = true;
            log::info("[GucciBot] Switched off: out of the game until it is switched back on");
        }

        void putBack() {
            auto* gb = GucciEngine::get();
            enableHooks();
#if GB_NATIVE_ENGINE
            // The first time after a launch with the switch off, this places
            // them (hooks/util_midhook.hpp).
            setEngineCodeIn(true);
#endif
            gb->enabled = true;
            s_takenOut = false;
            ui::resumeAssistantAccess();
            log::info("[GucciBot] Switched on: back in the game ({} midhooks, {} patches placed so far)",
                      g_midhookAttempts - g_midhookFailures, g_patchAttempts - g_patchFailures);
        }

    } // namespace

    bool wanted() {
        ensureLoaded();
        return s_wanted;
    }

    bool takenOut() {
        return s_takenOut;
    }

    void request(bool on) {
        ensureLoaded();
        if (on == s_wanted)
            return;
        if (on && GucciEngine::get()->standingDown)
            return;
        s_wanted = on;
        Mod::get()->setSavedValue<bool>(kSaveKey, on);
        // Only while GucciBot is still in: when it is already out there is
        // nothing running to stop.
        if (!on && !s_takenOut)
            stopEverything();
        // Changed its mind before leaving the level: it never went out, so
        // only Assistant Access (stopped above) has to come back.
        if (on && !s_takenOut)
            ui::resumeAssistantAccess();
        service();
    }

    const char* waitingFor() {
        ensureLoaded();
        if (s_wanted != s_takenOut)
            return nullptr;  // settled
        if (levelOpen())
            return s_wanted ? "GucciBot comes back on when you leave this level."
                            : "GucciBot has stopped, and leaves the game when you leave this level. Until then "
                              "its hacks stay as they are.";
        if (!s_wanted)
            return "Waiting for GucciBot to finish stopping...";
        return nullptr;
    }

    void service() {
        ensureLoaded();
        auto* gb = GucciEngine::get();
        if (s_takenOut) {
            // One compare once settled: a pause a crash left behind, or the
            // first frame after a launch with the switch off.
            gb->syncInputMods();
        }
        if (s_wanted != s_takenOut)
            return;  // settled
        if (levelOpen())
            return;
        if (!s_wanted) {
            if (stillStopping())
                return;
            takeOut();
        } else {
            if (gb->standingDown)
                return;
            putBack();
        }
    }

    void applyAtLaunch() {
        ensureLoaded();
        if (s_wanted)
            return;
        // No midhook or patch went in (util_midhook/util_patch read the same
        // saved value); the Geode hooks did, while the mod loaded, and come
        // out here, before the game has run any of them. Click Between Frames
        // is seen to on the first frame (service()).
        GucciEngine::get()->enabled = false;
        disableHooks();
        s_takenOut = true;
        log::info("[GucciBot] Switched off at launch: nothing placed, hooks out");
    }

} // namespace gucci::botswitch
