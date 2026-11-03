#include "ai/start.hpp"

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>

#include <cmath>

using namespace geode::prelude;

namespace gucci::ai {

    namespace {
        PauseLayer* pauseMenu() {
            auto* scene = CCDirector::sharedDirector()->getRunningScene();
            return scene ? scene->getChildByType<PauseLayer>(0) : nullptr;
        }
    }  // namespace

    void LevelStart::request(double tps) {
        auto& upd = GucciEngine::get()->updater;
        *this = LevelStart{};
        m_requested = true;
        m_wasPaused = upd.m_paused;
        m_oldTps = upd.m_tps;
        if (tps > 0.0 && std::fabs(tps - upd.m_tps) > 1e-6) {
            upd.setTps(tps);  // this run only: never saved (eng_tick_rate is untouched)
            m_tpsSet = true;
        }
    }

    LevelStart::Step LevelStart::service(std::string& why) {
        if (!m_requested) {
            why = "no start was asked for";
            return Step::Failed;
        }
        if (m_start) return Step::Ready;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) {
            why = "the level was left before the run could start";
            return Step::Failed;
        }
        auto& upd = GucciEngine::get()->updater;
        if (auto* menu = pauseMenu()) {
            if (!m_resetDone) {
                menu->onRestartFull(nullptr);
                m_resetDone = true;
                upd.setPaused(true);  // held at frame 0
            } else {
                menu->onResume(nullptr);
            }
            return Step::Waiting;  // the next frame, with the menu gone
        }
        if (!m_resetDone) {
            pl->fullReset();
            m_resetDone = true;
            upd.setPaused(true);
            return Step::Waiting;
        }
        // The level going, the player alive, not finished: until then one
        // update a frame (the level's start and GD's resume timer only move on
        // when the game updates).
        bool const ready = pl->m_started && !pl->m_isPaused && pl->m_resumeTimer <= 0 && !pl->m_player1->m_isDead &&
                           !pl->m_hasCompletedLevel;
        if (!ready) {
            if (++m_frames > 600) {
                why = "the level did not get going within ten seconds";
                return Step::Failed;
            }
            if (upd.m_paused) upd.stepOnce();
            return Step::Waiting;
        }
        m_start = sim::capture(pl, why);
        return m_start ? Step::Ready : Step::Failed;
    }

    void LevelStart::release() {
        if (!m_requested) return;
        auto& upd = GucciEngine::get()->updater;
        if (m_tpsSet) upd.setTps(m_oldTps);
        if (m_resetDone) upd.setPaused(m_wasPaused);
        m_requested = false;
        m_tpsSet = false;
    }

} // namespace gucci::ai
