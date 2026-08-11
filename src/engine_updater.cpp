#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "trajectory.hpp"
#include "util_midhook.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <safetyhook.hpp>
#include <fstream>

using namespace geode::prelude;

static std::ofstream g_slopeLog;
static void slopeLog(const std::string& line) {
    if (!g_slopeLog.is_open()) {
        auto path = Mod::get()->getSaveDir() / "guccibot_slope.log";
        g_slopeLog.open(path, std::ios::out | std::ios::trunc);
        log::info("[SLOPE] log file at: {}", path.string());
    }
    if (g_slopeLog.is_open()) {
        g_slopeLog << line << '\n';
        g_slopeLog.flush();
    }
    log::info("[SLOPE] {}", line);
}

float GucciUpdater::getTimeWarp() const {
    if (auto* pl = PlayLayer::get()) {
        float tw = pl->m_gameState.m_timeWarp;
        return tw > 0.0f ? tw : 1.0f;
    }
    return 1.0f;
}

uint32_t GucciUpdater::getFrame() const {
    if (PlayLayer::get())
        return m_frame + static_cast<uint32_t>(m_frameOnLastAttempt);
    if (auto* lel = LevelEditorLayer::get())
        return static_cast<uint32_t>(std::max(0, (int)lel->m_gameState.m_currentProgress - 1));
    return 0;
}

bool GucciUpdater::useFastLockDelta() const {
    auto* gb = GucciEngine::get();
    // Calculate must always take the substepped branch (see call site, engine_updater.cpp
    // runUpdates), but which substep count it uses should track m_lockDeltaMode the exact
    // same way normal play and the SLRenderer render pass do -- NOT be hardcoded to fast.
    // Forcing it to fast unconditionally (as before) only matched the renderer when the
    // user's Lock Delta Mode happened to be Performance; on Accuracy mode (the in-class
    // default), the renderer takes the slow/4-substep path while Calculate stayed forced
    // fast/1-substep, reproducing the exact "renderer works, Calculate doesn't" mismatch.
    if (gb->fwAnalyzing) return m_lockDeltaMode == LockDeltaMode::Performance;
    return m_lockDelta &&
           m_lockDeltaMode == LockDeltaMode::Performance &&
           gb->isPlaying() &&
           !gb->renderer.recording;
}

void GucciUpdater::calculateSteps(float dt, float targetDt) {
    dt += (float)m_tpsOverflow;

    float wantedDt = targetDt * std::fmin(getTimeWarp(), 1.0f);
    if (wantedDt == 0.0f) return;

    int steps = (int)std::floor(dt / wantedDt);
    int stepLimit = (int)m_maxUPR;

    if (m_respawnTimer > 0) m_tpsOverflow = 0.0;

    if (m_useVisualUpdates) {
        float fps = GameManager::get()->m_customFPSTarget;
        if (fps <= 10.0f) fps = 240.0f;
        int modifier = (int)m_tps / (int)fps;
        stepLimit *= std::max(1, modifier);
    }

    bool rendering = GucciEngine::get()->renderer.recording;
                                                                                                                                                                                                                                                                                                                                                                                              if (!m_realTime && !rendering) steps = std::min(steps, stepLimit);

    m_tpsOverflow = dt - steps * wantedDt;
    m_shouldRender = false;

    if (!m_realTime && !rendering) {
        if (steps == stepLimit) m_tpsOverflow = 0.0;
    }

    if (m_paused && !rendering) {
        steps = 1;
        m_tpsOverflow = 0;
        m_shouldRender = true;
        if (auto* pl = PlayLayer::get()) pl->m_extraDelta = 0.0f;
    }

    totalStepCount     = steps;
    estimatedStepCount = steps;
}

void GucciUpdater::breakLoop() {
    estimatedStepCount = 0;
    totalStepCount     = 0;
    m_tpsOverflow      = 0.0;
}

static void runFastLockDelta(GucciUpdater& upd,
                              std::function<void(float)> update,
                              float realDt) {
    upd.m_allowedToProcessActions = false;
    auto nextInput = GucciEngine::get()->replay.getCurrentQueuedInput();

    if (!nextInput.has_value()) {
        upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, upd.getPhysicsDt());
        if (upd.estimatedStepCount >= 1)
            update(realDt * upd.m_speedhack);
    } else {
        upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, upd.getPhysicsDt());
        int steps = upd.totalStepCount;
        while (steps > 0) {
            auto inp = GucciEngine::get()->replay.getCurrentQueuedInput();
            uint64_t safeSteps = inp.has_value()
                ? inp->m_frame - upd.getFrame()
                : (uint64_t)steps;
            safeSteps = std::min(safeSteps, (uint64_t)steps);
            if (safeSteps > 0) {
                upd.estimatedStepCount = (int)safeSteps;
                update(upd.getPhysicsDt() * safeSteps);
                steps -= (int)safeSteps;
            }
            if (steps > 0) {
                upd.estimatedStepCount = 1;
                update(realDt * upd.m_speedhack);
                steps--;
            }
        }
    }
}

static void runSlowLockDelta(GucciUpdater& upd,
                              std::function<void(float)> update,
                              float realDt,
                              bool calcSsb) {
    GJBaseGameLayer* pl = PlayLayer::get();
    if (!pl) pl = LevelEditorLayer::get();
    if (!pl) return;

    float newTfx  = pl->m_player1
        ? pl->timeForPos(pl->m_player1->m_position, 0,
                         pl->m_gameState.m_currentChannel, true, 0)
        : upd.m_lastTfp;
    float ssbDelta = std::abs(newTfx - upd.m_lastTfp);
    if (!calcSsb || ssbDelta <= 0) ssbDelta = (float)upd.getPhysicsDt();
    upd.m_lastTfp = newTfx;

    float delta = (float)upd.getPhysicsDt();
    upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, ssbDelta);

    upd.m_shouldRender = false;

    for (int i = 0; i < upd.estimatedStepCount - 1; i++) {
        update(delta);
        if (!((PlayLayer*)pl)->m_hasCompletedLevel && upd.estimatedStepCount > 0) {
            if (calcSsb && pl->m_player1) {
                float ntfx = pl->timeForPos(pl->m_player1->m_position, 0,
                                            pl->m_gameState.m_currentChannel, true, 0);
                float sd   = std::abs(ntfx - upd.m_lastTfp);
                if (sd <= 0) sd = delta;
                upd.m_lastTfp = ntfx;
                upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, sd);
            }
        }
    }

    upd.m_shouldRender = true;
    if (upd.estimatedStepCount > 0) update(delta);
}

void GucciUpdater::runUpdates(std::function<void(float)> update,
                               float realDt, bool frozen) {
    auto* gb = GucciEngine::get();
    m_allowedToProcessActions = true;

    if (frozen) {
        m_onlyRefresh = true;
        update(realDt);
        m_onlyRefresh = false;
        if (PlayLayer::get())
            TrajectoryPredictionService::get().updatePreview(PlayLayer::get());
        return;
    }

    GJBaseGameLayer* pl = PlayLayer::get();
    bool isPlayLayer = true;
    if (!pl) { pl = LevelEditorLayer::get(); isPlayLayer = false; }

    if (!pl || (isPlayLayer && ((PlayLayer*)pl)->m_isPaused)) {
        m_tpsOverflow = 0.0;
        update(realDt);
        return;
    }

    if (m_paused && !gb->renderer.recording) {
        m_tpsOverflow = 0.0;
        m_respawnTimer = 0;
        if (consumeStep()) m_shouldRender = true;
        else return;
    }

    bool calcSsb = isPlayLayer &&
        !((PlayLayer*)pl)->m_isPaused &&
        !((PlayLayer*)pl)->m_hasCompletedLevel &&
        pl->m_started &&
        !pl->m_isPlatformer &&
        m_ssbFix &&
        gb->renderer.recording;

    bool useAccLockDelta = m_lockDelta &&
        (m_lockDeltaMode == LockDeltaMode::Accuracy || gb->renderer.recording);

                        if (gb->fwAnalyzing && (getFrame() % 25) == 0) {
        auto qi = gb->replay.getCurrentQueuedInput();
        log::info("[FWDISP] f={} enterBlock={} fastFn={} lockD={} mode={} acc={} hasInput={} realDt={:.6f} est={}",
                  getFrame(), (m_lockDelta || useAccLockDelta) && isPlayLayer, useFastLockDelta(),
                  m_lockDelta, (int)m_lockDeltaMode, useAccLockDelta, qi.has_value(), realDt, estimatedStepCount);
    }

    if ((m_lockDelta || useAccLockDelta) && isPlayLayer) {
        if (useFastLockDelta()) runFastLockDelta(*this, update, realDt);
        else                    runSlowLockDelta(*this, update, realDt, calcSsb);
    } else {
        m_shouldRender = true;
        if (m_respawnTimer > 0) {
            m_tpsOverflow = 0.0;
            pl->m_extraDelta = 0.0;
        }
        update(realDt * m_speedhack);
    }
}

void GucciUpdater::runFrozenTick() {
    if (m_frozenScheduledFunctions.empty()) return;
    for (auto& fn : m_frozenScheduledFunctions) fn(0.0f);
    runUpdates([](float dt){
        CCDirector::sharedDirector()->getScheduler()->update(dt);
    }, 0.0f, true);
    m_frozenScheduledFunctions.clear();
}

void GucciUpdater::backwardsStep(int n) {
    m_paused = true;
    if (n <= 0 || !m_backwardsStepping) return;

    auto* pl = PlayLayer::get();
    auto* gb = GucciEngine::get();
    if (!pl) return;

    for (int i = 0; i < n - 1; i++)
        gb->practiceFix.dropLastStoredFrame();

    if (!gb->practiceFix.canRestoreState()) return;

    gb->practiceFix.m_loadCheckpoint = true;
    gb->practiceFix.m_isBackstep     = true;
    pl->resetLevel();
    gb->practiceFix.m_loadCheckpoint = false;
}

void GucciUpdater::updateAudioSpeedhack() {
    FMOD::ChannelGroup* master;
    FMODAudioEngine::get()->m_system->getMasterChannelGroup(&master);
    if (master) master->setPitch(m_speedhackAudio ? (float)m_speedhack : 1.0f);
}

static void physDtMidhook(SafetyHookContext& ctx) {
    auto* pl = GJBaseGameLayer::get();
    if (!pl) return;
    auto& upd = GucciEngine::get()->updater;
    if (upd.m_onlyRefresh) return;
    ctx.xmm1.f64[0] *= upd.m_tps / 60.0;
    ctx.rip += 0x08;
}

static void physStepCountMidhook(SafetyHookContext& ctx) {
    auto& upd = GucciEngine::get()->updater;
    bool fastBypass = upd.useFastLockDelta() || !upd.m_lockDelta;
    if (!fastBypass && PlayLayer::get()) return;
    ctx.rdx = 2 - upd.estimatedStepCount;
}

static void restorePhysDtMidhook(SafetyHookContext& ctx) {
    auto& upd = GucciEngine::get()->updater;
    bool fastBypass = upd.useFastLockDelta() || !upd.m_lockDelta;
    if (!fastBypass && PlayLayer::get()) return;
    ctx.xmm9.f64[0] = upd.getPhysicsDt() * upd.estimatedStepCount;
}

static void earlyUpdateMidhook(SafetyHookContext&) {
    auto* gb  = GucciEngine::get();
    auto& upd = gb->updater;
    if (upd.m_onlyRefresh) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
        if (!pl->m_playerDied && upd.m_backwardsStepping && !gb->renderer.recording) {
        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp) return;
        cp->retain();
        gb->practiceFix.saveState(cp, upd.getFrame());
    }
}

static char gamemodeChar(PlayerObject* p) {
    if (!p) return 'C';
    if (p->m_isRobot)  return 'R';
    if (p->m_isSpider) return 'X';
    if (p->m_isShip)   return 'H';
    if (p->m_isBall)   return 'B';
    if (p->m_isBird)   return 'U';
    if (p->m_isDart)   return 'V';
    return 'C';
}

static void frameUpdateMidhook(SafetyHookContext&) {
    auto* gb  = GucciEngine::get();
    auto& upd = gb->updater;
    auto* pl  = GJBaseGameLayer::get();
    if (!pl || pl->m_resumeTimer > 0) return;

    if (!pl->m_playerDied) {
        if (PlayLayer::get()) upd.incrementFrame();

        if (gb->isRecording()) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_player1) {
                MacroPathSample smp;
                smp.p1x = plr->m_player1->m_position.x;
                smp.p1y = plr->m_player1->m_position.y;
                smp.gamemode1 = gamemodeChar(plr->m_player1);
                if (plr->m_player2) {
                    smp.p2x = plr->m_player2->m_position.x;
                    smp.p2y = plr->m_player2->m_position.y;
                    smp.gamemode2 = gamemodeChar(plr->m_player2);
                }
                gb->replay.m_pathSamples.push_back(smp);
            }
        }
    }

                            if (gb->isPlaying()) {
        bool logIt = !gb->fwAnalyzing || (gb->fwState == GucciEngine::FwState::Capturing);
        if (logIt) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_player1) {
                auto* p = plr->m_player1;
                                                                                bool hold = false;
                if (plr->m_uiLayer)
                    hold = plr->m_uiLayer->m_p1Jumping || plr->m_uiLayer->m_p1TouchId != -1;
                log::info("[DIAG] {} f={} x={:.3f} y={:.3f} xs={:.3f} ys={:.3f} rot={:.3f} g={} flip={} hold={} est={} ovf={:.4f} rs={}",
                    gb->fwAnalyzing ? "CALC" : "PLAY",
                    upd.getFrame(),
                    (double)p->m_position.x, (double)p->m_position.y,
                    (double)p->m_playerSpeed, (double)p->m_yVelocity,
                    (double)p->getRotation(),
                    p->m_isOnGround ? 1 : 0,
                    p->m_isUpsideDown ? 1 : 0,
                    hold ? 1 : 0,
                    upd.estimatedStepCount, (double)upd.m_tpsOverflow, upd.m_respawnTimer);
            }
        }
    }

                gb->fwCkptCreatedThisFrame = false;
    if (gb->fwAnalyzing) gb->fwTick();

                                bool slRender = SLRenderer::get()->isRecording();
    if (gb->isPlaying() || slRender) {
        bool logIt = slRender || !gb->fwAnalyzing || (gb->fwState == GucciEngine::FwState::Capturing);
        if (logIt) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_player1) {
                auto* p = plr->m_player1;
                bool hold = false;
                if (plr->m_uiLayer)
                    hold = plr->m_uiLayer->m_p1Jumping || plr->m_uiLayer->m_p1TouchId != -1;
                char mode = p->m_isRobot ? 'R'
                          : p->m_isSpider ? 'X'
                          : p->m_isSwing  ? 'G'
                          : p->m_isShip   ? 'H'
                          : p->m_isBall   ? 'B'
                          : p->m_isBird   ? 'U'
                          : p->m_isDart   ? 'V'
                          : 'C';
                slopeLog(fmt::format(
                    "{} f={} x={:.3f} y={:.3f} xs={:.3f} ys={:.3f} rot={:.3f} "
                    "g={} flip={} dash={} mode={} hold={} steps={} ovf={:.4f} rs={} ckpt={} fast={} q={} "
                    "onS={} wasS={} sVel={:.4f} preSV={:.4f} colS={} ang={:.2f} st={:.3f} et={:.3f} curS={}",
                    slRender ? "REND" : (gb->fwAnalyzing ? "CALC" : "PLAY"),
                    upd.getFrame(),
                    (double)p->m_position.x, (double)p->m_position.y,
                    (double)p->m_playerSpeed, (double)p->m_yVelocity,
                    (double)p->getRotation(),
                    p->m_isOnGround ? 1 : 0,
                    p->m_isUpsideDown ? 1 : 0,
                    p->m_isDashing ? 1 : 0,
                    mode,
                    hold ? 1 : 0,
                    upd.estimatedStepCount,
                    (double)upd.m_tpsOverflow,
                    upd.m_respawnTimer,
                    gb->fwCkptCreatedThisFrame ? 1 : 0,
                    upd.useFastLockDelta() ? 1 : 0,
                    gb->replay.getCurrentQueuedInput().has_value() ? 1 : 0,
                    p->m_isOnSlope ? 1 : 0,
                    p->m_wasOnSlope ? 1 : 0,
                    (double)p->m_slopeVelocity,
                    (double)p->m_yVelocityBeforeSlope,
                    p->m_isCollidingWithSlope ? 1 : 0,
                    (double)p->m_slopeAngle,
                    (double)p->m_slopeStartTime,
                    (double)p->m_slopeEndTime,
                    p->m_currentSlope ? 1 : 0));
            }
        }
    }

        if (auto* pll = PlayLayer::get()) {
        auto res = Autoclicker::get()->processTick();
        if (res.p1Fire) pll->queueButton(1, res.p1Press, false, 0.0);
        if (res.p2Fire) pll->queueButton(1, res.p2Press, true,  0.0);
    }
}

class $modify(GB7CCScheduler, CCScheduler) {
    void update(float dt) override {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_onlyRefresh || !gb->enabled) {
            CCScheduler::update(dt);
            return;
        }
                                                                                        gb->updater.runUpdates(
            [this](float d){ this->CCScheduler::update(d); }, dt, false);
    }
};

class $modify(GB7CCDirector, CCDirector) {
    void drawScene() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) return CCDirector::drawScene();

        gb->scheduler.update(this->getDeltaTime());
        gb->updater.updateAudioSpeedhack();

        auto* pl = PlayLayer::get();

                                auto* sl = SLRenderer::get();
        if (sl->m_shouldStart) sl->startIfQueued();
        if (pl && sl->isRecording() && !pl->m_isPaused &&
            (pl->m_started || sl->m_settings.m_firstAttemptPause)) {
            float dt = sl->getDt();
            if (sl->m_halting.load()) {
                sl->displayPreview();
                this->m_pobOpenGLView->swapBuffers();
                gb->updater.runFrozenTick();
                return;
            }
            if (sl->m_needsCleanup) {
                sl->m_texture.postCapture();
                sl->m_needsCleanup = false;
            }
            if (!m_bPaused) m_pScheduler->update(dt);
            if (m_pNextScene) this->setNextScene();
            sl->update(pl);
            sl->displayPreview();
            this->m_pobOpenGLView->swapBuffers();
            gb->updater.runFrozenTick();
            return;
        }

        if (!pl) return CCDirector::drawScene();

        auto& rend = gb->renderer;
        if (rend.recording && !pl->m_isPaused && pl->m_started) {
            int frame = gb->updater.getFrame();
            if (!m_bPaused) m_pScheduler->update(1.0f / rend.fps);
            if (m_pNextScene) this->setNextScene();
            rend.handleRecording(pl, frame);
            this->m_pobOpenGLView->swapBuffers();
            gb->updater.runFrozenTick();
            return;
        }

                                                                        if (gb->fwAnalyzing && !pl->m_isPaused && pl->m_started) {
            if (!m_bPaused) m_pScheduler->update(sl->getDt());
            if (m_pNextScene) this->setNextScene();
            if (m_pRunningScene) m_pRunningScene->visit();
            this->m_pobOpenGLView->swapBuffers();
            gb->updater.runFrozenTick();
            return;
        }

        CCDirector::drawScene();
        gb->updater.runFrozenTick();
    }
};

constexpr int ACTIONMGR_UPDATE_OFFSET = 0x38B90;
static void (*actionMgrOrig)(void*, float) = nullptr;

static void actionMgrHook(void* self, float dt) {
    auto& upd = GucciEngine::get()->updater;
    if (upd.m_onlyRefresh) return;
    if (actionMgrOrig) actionMgrOrig(self, dt);
}

$execute {
    util_midhook(geode::base::get() + 0x237A7C, "physDt",        physDtMidhook);
    util_midhook(geode::base::get() + 0x237DCE, "physStepCount", physStepCountMidhook);
    util_midhook(geode::base::get() + 0x238F6E, "restorePhysDt", restorePhysDtMidhook);
    util_midhook(geode::base::get() + 0x237E42, "earlyUpdate",   earlyUpdateMidhook);
    util_midhook(geode::base::get() + 0x238BAA, "frameUpdate",   frameUpdateMidhook);

        actionMgrOrig = reinterpret_cast<void(*)(void*,float)>(
        geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET);
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET),
        &actionMgrHook,
        "CCActionManager::update",
        tulip::hook::TulipConvention::Fastcall);

            auto p1 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x23B4EA),
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
         0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90});

        auto p2 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x4cd95c),
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
         0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90});

        auto p3 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x3B994C),
        {0xeb, 0x5e});

        auto p4 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x3BA508),
        {0x90,0x90,0x90,0x90,0x90});

    g_patchAttempts = 4;
    g_patchFailures = (p1.isErr()?1:0)+(p2.isErr()?1:0)+(p3.isErr()?1:0)+(p4.isErr()?1:0);
    if (g_patchFailures)
        geode::log::error("[GucciBot] {} of 4 binary patches FAILED to apply", g_patchFailures);
}
