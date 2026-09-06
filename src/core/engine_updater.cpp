#include "core/GucciBot.hpp"
#include "hacks/autoclicker.hpp"
#include "analysis/trajectory.hpp"
#include "hooks/util_midhook.hpp"
#include "render/renderer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <safetyhook.hpp>
#include <fstream>

using namespace geode::prelude;

using namespace gucci;

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

static std::ofstream g_frameIncLog;
void gucci::logFrameIncrement(const char* callSite, uint32_t frame, PlayerObject* p) {
    if (!g_frameIncLog.is_open()) {
        auto path = Mod::get()->getSaveDir() / "guccibot_frameinc.log";
        g_frameIncLog.open(path, std::ios::out | std::ios::trunc);
        log::info("[FRAMEINC] log file at: {}", path.string());
    }
    if (g_frameIncLog.is_open()) {
        g_frameIncLog << callSite << " -> frame " << frame;
        if (p) {
            g_frameIncLog << " pos=(" << p->m_position.x << "," << p->m_position.y << ")"
                          << " vel=(" << p->m_playerSpeed << "," << p->m_yVelocity << ")"
                          << " grnd=" << (p->m_isOnGround ? 1 : 0);
        }
        g_frameIncLog << '\n';
        g_frameIncLog.flush();
    }
}

static std::ofstream g_calcDeathLog;
void gucci::logCalcDeathTrace(const std::string& line) {
    if (!g_calcDeathLog.is_open()) {
        auto path = Mod::get()->getSaveDir() / "guccibot_calcdeath.log";
        g_calcDeathLog.open(path, std::ios::out | std::ios::trunc);
        log::info("[CALCDEATH] log file at: {}", path.string());
    }
    if (g_calcDeathLog.is_open()) {
        g_calcDeathLog << line << '\n';
        g_calcDeathLog.flush();
    }
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
    return false;
}

void GucciUpdater::calculateSteps(float dt, float targetDt) {
    dt += (float)m_tpsOverflow;

    float wantedDt = targetDt * std::fmin(getTimeWarp(), 1.0f);
    if (wantedDt == 0.0f)
        return;

    int steps = (int)std::floor(dt / wantedDt);
    int stepLimit = (int)m_maxUPR;

    if (m_respawnTimer > 0)
        m_tpsOverflow = 0.0;

    if (m_useVisualUpdates) {
        float fps = GameManager::get()->m_customFPSTarget;
        if (fps <= 10.0f)
            fps = 240.0f;
        int modifier = (int)m_tps / (int)fps;
        stepLimit *= std::max(1, modifier);
    }

    bool rendering = GucciEngine::get()->renderer.recording;
    if (!m_realTime && !rendering)
        steps = std::min(steps, stepLimit);

    m_tpsOverflow = dt - steps * wantedDt;
    m_shouldRender = false;

    if (!m_realTime && !rendering) {
        if (steps == stepLimit)
            m_tpsOverflow = 0.0;
    }

    if (m_paused && !rendering) {
        steps = 1;
        m_tpsOverflow = 0;
        m_shouldRender = true;
        if (auto* pl = PlayLayer::get())
            pl->m_extraDelta = 0.0f;
    }

    totalStepCount = steps;
    estimatedStepCount = steps;
}

void GucciUpdater::breakLoop() {
    estimatedStepCount = 0;
    totalStepCount = 0;
    m_tpsOverflow = 0.0;
}

static void
runSlowLockDelta(GucciUpdater& upd, std::function<void(float)> update, float realDt, bool calcSsb) {
    GJBaseGameLayer* pl = PlayLayer::get();
    if (!pl)
        pl = LevelEditorLayer::get();
    if (!pl)
        return;

    float newTfx =
        pl->m_player1 ? pl->timeForPos(
                            pl->m_player1->m_position, 0, pl->m_gameState.m_currentChannel, true, 0)
                      : upd.m_lastTfp;
    float ssbDelta = std::abs(newTfx - upd.m_lastTfp);
    if (!calcSsb || ssbDelta <= 0)
        ssbDelta = (float)upd.getPhysicsDt();
    upd.m_lastTfp = newTfx;

    float delta = (float)upd.getPhysicsDt();
    upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, ssbDelta);

    upd.m_shouldRender = false;

    for (int i = 0; i < upd.estimatedStepCount - 1; i++) {
        update(delta);
        if (!((PlayLayer*)pl)->m_hasCompletedLevel && upd.estimatedStepCount > 0) {
            if (calcSsb && pl->m_player1) {
                float ntfx = pl->timeForPos(
                    pl->m_player1->m_position, 0, pl->m_gameState.m_currentChannel, true, 0);
                float sd = std::abs(ntfx - upd.m_lastTfp);
                if (sd <= 0)
                    sd = delta;
                upd.m_lastTfp = ntfx;
                upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, sd);
            }
        }
    }

    upd.m_shouldRender = true;
    if (upd.estimatedStepCount > 0)
        update(delta);
}

void GucciUpdater::runUpdates(std::function<void(float)> update, float realDt, bool frozen) {
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
    if (!pl) {
        pl = LevelEditorLayer::get();
        isPlayLayer = false;
    }

    if (!pl || (isPlayLayer && ((PlayLayer*)pl)->m_isPaused)) {
        m_tpsOverflow = 0.0;
        update(realDt);
        return;
    }

    if (m_paused && !gb->renderer.recording) {
        m_tpsOverflow = 0.0;
        m_respawnTimer = 0;
        if (consumeStep())
            m_shouldRender = true;
        else
            return;
    }

    bool calcSsb = isPlayLayer && !((PlayLayer*)pl)->m_isPaused &&
                   !((PlayLayer*)pl)->m_hasCompletedLevel && pl->m_started && !pl->m_isPlatformer &&
                   m_ssbFix && gb->renderer.recording;

    if (gb->fwAnalyzing && (getFrame() % 25) == 0) {
        auto qi = gb->replay.getCurrentQueuedInput();
        log::info("[FWDISP] f={} enterBlock={} lockD={} hasInput={} realDt={:.6f} est={}",
                  getFrame(),
                  m_lockDelta && isPlayLayer,
                  m_lockDelta,
                  qi.has_value(),
                  realDt,
                  estimatedStepCount);
    }

    if (m_lockDelta && isPlayLayer) {
        runSlowLockDelta(*this, update, realDt, calcSsb);
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
    if (m_frozenScheduledFunctions.empty())
        return;
    for (auto& fn : m_frozenScheduledFunctions)
        fn(0.0f);
    runUpdates(
        [](float dt) {
            CCDirector::sharedDirector()->getScheduler()->update(dt);
        },
        0.0f,
        true);
    m_frozenScheduledFunctions.clear();
}

void GucciUpdater::backwardsStep(int n) {
    m_paused = true;
    if (n <= 0 || !m_backwardsStepping)
        return;

    auto* pl = PlayLayer::get();
    auto* gb = GucciEngine::get();
    if (!pl)
        return;

    for (int i = 0; i < n - 1; i++)
        gb->practiceFix.dropLastStoredFrame();

    if (!gb->practiceFix.canRestoreState())
        return;

    gb->practiceFix.m_loadCheckpoint = true;
    gb->practiceFix.m_isBackstep = true;
    pl->resetLevel();
    gb->practiceFix.m_loadCheckpoint = false;
}

void GucciUpdater::updateAudioSpeedhack() {
    FMOD::ChannelGroup* master;
    FMODAudioEngine::get()->m_system->getMasterChannelGroup(&master);
    if (master)
        master->setPitch(m_speedhackAudio ? (float)m_speedhack : 1.0f);
}

// 2026-09-05: none of the raw midhooks/hooks below used to check
// GucciEngine::enabled at all -- harmless for as long as `enabled` was
// unconditionally forced true at every launch, but the new
// ToastyReplay-Lite-must-be-installed gate (see initialize()) means
// `enabled` can now genuinely stay false for a whole session, and Nigel
// caught it in-game: autoclicker (and everything else these drive) kept
// working with the notification up. These are raw SafetyHook midhooks
// and a direct CCActionManager::update replacement -- they run
// unconditionally once installed, completely independent of the
// GB7CCScheduler/GB7CCDirector path below that already respected
// `enabled` correctly. Added the same guard to all six.
static void physDtMidhook(SafetyHookContext& ctx) {
    auto* pl = GJBaseGameLayer::get();
    if (!pl)
        return;
    auto* gb = GucciEngine::get();
    if (!gb->enabled)
        return;
    auto& upd = gb->updater;
    if (upd.m_onlyRefresh)
        return;
    ctx.xmm1.f64[0] *= upd.m_tps / 60.0;
    ctx.rip += 0x08;
}

static void physStepCountMidhook(SafetyHookContext& ctx) {
    auto* gb = GucciEngine::get();
    if (!gb->enabled)
        return;
    auto& upd = gb->updater;
    bool fastBypass = upd.useFastLockDelta() || !upd.m_lockDelta;
    if (!fastBypass && PlayLayer::get())
        return;
    ctx.rdx = 2 - upd.estimatedStepCount;
}

static void restorePhysDtMidhook(SafetyHookContext& ctx) {
    auto* gb = GucciEngine::get();
    if (!gb->enabled)
        return;
    auto& upd = gb->updater;
    bool fastBypass = upd.useFastLockDelta() || !upd.m_lockDelta;
    if (!fastBypass && PlayLayer::get())
        return;
    ctx.xmm9.f64[0] = upd.getPhysicsDt() * upd.estimatedStepCount;
}

static void earlyUpdateMidhook(SafetyHookContext&) {
    auto* gb = GucciEngine::get();
    if (!gb->enabled)
        return;
    auto& upd = gb->updater;
    if (upd.m_onlyRefresh)
        return;
    auto* pl = PlayLayer::get();
    if (!pl)
        return;
    if (!pl->m_playerDied && upd.m_backwardsStepping && !gb->renderer.recording) {
        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp)
            return;
        cp->retain();
        if (upd.m_logFrameIncrements)
            logFrameIncrement("earlyUpdateMidhook(saveState)", upd.getFrame() + 1, pl->m_player1);
        gb->practiceFix.saveState(cp, upd.getFrame() + 1);
    }
}

static char gamemodeChar(PlayerObject* p) {
    if (!p)
        return 'C';
    if (p->m_isRobot)
        return 'R';
    if (p->m_isSpider)
        return 'X';
    if (p->m_isSwing)
        return 'G';
    if (p->m_isShip)
        return 'H';
    if (p->m_isBall)
        return 'B';
    if (p->m_isBird)
        return 'U';
    if (p->m_isDart)
        return 'V';
    return 'C';
}

static void classifyOrbTouchForCapture(PlayerObject* player, bool& outDash, bool& outNonDash) {
    outDash = false;
    outNonDash = false;
    if (!player || !player->m_touchingRings)
        return;
    for (auto* obj : CCArrayExt<GameObject*>(player->m_touchingRings)) {
        if (!obj)
            continue;
        auto type = obj->m_objectType;
        if (type == GameObjectType::DashRing || type == GameObjectType::GravityDashRing) {
            outDash = true;
            continue;
        }
        switch (type) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::CustomRing:
        case GameObjectType::TeleportOrb:
            outNonDash = true;
            break;
        default:
            break;
        }
    }
}

static void frameUpdateMidhook(SafetyHookContext&) {
    auto* gb = GucciEngine::get();
    if (!gb->enabled)
        return;
    auto& upd = gb->updater;
    auto* pl = GJBaseGameLayer::get();
    if (!pl || pl->m_resumeTimer > 0)
        return;

    if (!pl->m_playerDied) {
        if (PlayLayer::get()) {
            // Checked/cleared BEFORE incrementFrame() below on purpose, so
            // stage 2 captures while getFrame() still reads the label it
            // was queued under -- one tick after storeCheckpoint queued it
            // (hook_playlayer.cpp), so this tick's own physics settle has
            // already happened. Don't move this after incrementFrame() or
            // collapse the two-stage promotion -- both changes reintroduce
            // stale-position checkpoint capture.
            auto& pf = gb->practiceFix;
            if (pf.m_pendingCaptureStage == 2) {
                if (upd.m_logFrameIncrements)
                    logFrameIncrement(
                        "frameUpdateMidhook(deferredCapture)", upd.getFrame() + 1, pl->m_player1);
                pf.saveCurrent(pf.m_pendingCaptureCp, pf.m_pendingCaptureFrameOffset);
                pf.m_pendingCaptureCp = nullptr;
                pf.m_pendingCaptureStage = 0;
            } else if (pf.m_pendingCaptureStage == 1) {
                pf.m_pendingCaptureStage = 2;
            }

            upd.incrementFrame();
            if (upd.m_logFrameIncrements)
                logFrameIncrement("frameUpdateMidhook", upd.getFrame(), pl->m_player1);
        }

        bool shouldCapturePath = gb->isRecording() || (gb->isPlaying() && !gb->fwAnalyzing);
        if (shouldCapturePath) {
            auto* plr = PlayLayer::get();
            auto& samples = gb->replay.m_pathSamples;
            uint32_t frame = upd.getFrame();
            if (plr && plr->m_player1 && frame >= samples.size()) {
                auto* p1 = plr->m_player1;
                MacroPathSample smp;
                smp.p1x = p1->m_position.x;
                smp.p1y = p1->m_position.y;
                smp.p1XVel = p1->m_playerSpeed;
                smp.p1YVel = (float)p1->m_yVelocity;
                smp.p1Rot = p1->getRotation();
                smp.p1OnGround = p1->m_isOnGround;
                smp.p1UpsideDown = p1->m_isUpsideDown;
                smp.p1Dashing = p1->m_isDashing;
                classifyOrbTouchForCapture(p1, smp.p1OrbDash, smp.p1OrbNonDash);
                smp.gamemode1 = gamemodeChar(p1);

                if (plr->m_gameState.m_isDualMode && plr->m_player2) {
                    auto* p2 = plr->m_player2;
                    smp.hasP2 = true;
                    smp.p2x = p2->m_position.x;
                    smp.p2y = p2->m_position.y;
                    smp.p2XVel = p2->m_playerSpeed;
                    smp.p2YVel = (float)p2->m_yVelocity;
                    smp.p2Rot = p2->getRotation();
                    smp.p2OnGround = p2->m_isOnGround;
                    smp.p2UpsideDown = p2->m_isUpsideDown;
                    smp.p2Dashing = p2->m_isDashing;
                    classifyOrbTouchForCapture(p2, smp.p2OrbDash, smp.p2OrbNonDash);
                    smp.gamemode2 = gamemodeChar(p2);
                }
                samples.resize(frame + 1);
                samples[frame] = smp;
                if (!gb->isRecording())
                    gb->replay.m_pathSamplesDirty = true;
            }
        }

        if (gb->replay.m_pathSamplesDirty && gb->isPlaying() && !gb->fwAnalyzing) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_hasCompletedLevel) {
                gb->replay.savePathSamplesNow();
                gb->replay.m_pathSamplesDirty = false;
            }
        }

        if (gb->fwAnalyzing && gb->fwState == GucciEngine::FwState::Capturing) {
            auto& samples = gb->replay.m_pathSamples;
            uint32_t frame = upd.getFrame();
            if (frame < samples.size()) {
                auto* plr = PlayLayer::get();
                auto const& s = samples[frame];
                if (plr && plr->m_player1) {
                    auto* p1 = plr->m_player1;
                    p1->setPosition({s.p1x, s.p1y});
                    p1->m_playerSpeed = s.p1XVel;
                    p1->m_yVelocity = s.p1YVel;
                    p1->setRotation(s.p1Rot);
                    p1->m_isOnGround = s.p1OnGround;
                    p1->m_isUpsideDown = s.p1UpsideDown;
                    p1->m_isDashing = s.p1Dashing;
                }
                if (s.hasP2 && plr && plr->m_player2) {
                    auto* p2 = plr->m_player2;
                    p2->setPosition({s.p2x, s.p2y});
                    p2->m_playerSpeed = s.p2XVel;
                    p2->m_yVelocity = s.p2YVel;
                    p2->setRotation(s.p2Rot);
                    p2->m_isOnGround = s.p2OnGround;
                    p2->m_isUpsideDown = s.p2UpsideDown;
                    p2->m_isDashing = s.p2Dashing;
                }
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
                log::info("[DIAG] {} f={} x={:.3f} y={:.3f} xs={:.3f} ys={:.3f} rot={:.3f} g={} "
                          "flip={} hold={} est={} ovf={:.4f} rs={}",
                          gb->fwAnalyzing ? "CALC" : "PLAY",
                          upd.getFrame(),
                          (double)p->m_position.x,
                          (double)p->m_position.y,
                          (double)p->m_playerSpeed,
                          (double)p->m_yVelocity,
                          (double)p->getRotation(),
                          p->m_isOnGround ? 1 : 0,
                          p->m_isUpsideDown ? 1 : 0,
                          hold ? 1 : 0,
                          upd.estimatedStepCount,
                          (double)upd.m_tpsOverflow,
                          upd.m_respawnTimer);
            }
        }
    }

    gb->fwCkptCreatedThisFrame = false;
    if (gb->fwAnalyzing)
        gb->fwTick();

    bool slRender = SLRenderer::get()->isRecording();
    if (gb->isPlaying() || slRender) {
        bool logIt =
            slRender || !gb->fwAnalyzing || (gb->fwState == GucciEngine::FwState::Capturing);
        if (logIt) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_player1) {
                auto* p = plr->m_player1;
                bool hold = false;
                if (plr->m_uiLayer)
                    hold = plr->m_uiLayer->m_p1Jumping || plr->m_uiLayer->m_p1TouchId != -1;
                char mode = p->m_isRobot    ? 'R'
                            : p->m_isSpider ? 'X'
                            : p->m_isSwing  ? 'G'
                            : p->m_isShip   ? 'H'
                            : p->m_isBall   ? 'B'
                            : p->m_isBird   ? 'U'
                            : p->m_isDart   ? 'V'
                                            : 'C';
                slopeLog(fmt::format("{} f={} x={:.3f} y={:.3f} xs={:.3f} ys={:.3f} rot={:.3f} "
                                     "g={} flip={} dash={} mode={} hold={} steps={} ovf={:.4f} "
                                     "rs={} ckpt={} fast={} q={} "
                                     "onS={} wasS={} sVel={:.4f} preSV={:.4f} colS={} ang={:.2f} "
                                     "st={:.3f} et={:.3f} curS={}",
                                     slRender ? "REND" : (gb->fwAnalyzing ? "CALC" : "PLAY"),
                                     upd.getFrame(),
                                     (double)p->m_position.x,
                                     (double)p->m_position.y,
                                     (double)p->m_playerSpeed,
                                     (double)p->m_yVelocity,
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
        if (res.p1Fire) {
            pll->queueButton(1, res.p1Press, false, 0.0);
            // Extra clicks-per-hold (Silicate 1.1.0 parity): fire N-1 more
            // full release/press cycles right away, only on the press that
            // starts a new hold -- matches Silicate's own gating (never on
            // the release side).
            for (int i = 1; i < res.p1Clicks && res.p1Press; i++) {
                pll->queueButton(1, false, false, 0.0);
                pll->queueButton(1, true, false, 0.0);
            }
        }
        if (res.p2Fire) {
            pll->queueButton(1, res.p2Press, true, 0.0);
            for (int i = 1; i < res.p2Clicks && res.p2Press; i++) {
                pll->queueButton(1, false, true, 0.0);
                pll->queueButton(1, true, true, 0.0);
            }
        }
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
            [this](float d) {
                this->CCScheduler::update(d);
            },
            dt,
            false);
    }
};

class $modify(GB7CCDirector, CCDirector) {
    void drawScene() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled)
            return CCDirector::drawScene();

        gb->scheduler.update(this->getDeltaTime());
        gb->updater.updateAudioSpeedhack();

        auto* pl = PlayLayer::get();

        auto* sl = SLRenderer::get();
        if (sl->m_shouldStart)
            sl->startIfQueued();
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
            if (!m_bPaused)
                m_pScheduler->update(dt);
            if (m_pNextScene)
                this->setNextScene();
            sl->update(pl);
            sl->displayPreview();
            this->m_pobOpenGLView->swapBuffers();
            gb->updater.runFrozenTick();
            return;
        }

        if (!pl)
            return CCDirector::drawScene();

        auto& rend = gb->renderer;
        if (rend.recording && !pl->m_isPaused && pl->m_started) {
            int frame = gb->updater.getFrame();
            if (!m_bPaused)
                m_pScheduler->update(1.0f / rend.fps);
            if (m_pNextScene)
                this->setNextScene();
            rend.handleRecording(pl, frame);
            this->m_pobOpenGLView->swapBuffers();
            gb->updater.runFrozenTick();
            return;
        }

        if (gb->fwAnalyzing && !pl->m_isPaused && pl->m_started) {
            if (!m_bPaused)
                m_pScheduler->update(sl->getDt());
            if (m_pNextScene)
                this->setNextScene();
            if (m_pRunningScene)
                m_pRunningScene->visit();
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
    auto* gb = GucciEngine::get();
    if (!gb->enabled) {
        if (actionMgrOrig)
            actionMgrOrig(self, dt);
        return;
    }
    auto& upd = gb->updater;
    if (upd.m_onlyRefresh)
        return;
    if (actionMgrOrig)
        actionMgrOrig(self, dt);
}

$execute {
    util_midhook(geode::base::get() + 0x237A7C, "physDt", physDtMidhook);
    util_midhook(geode::base::get() + 0x237DCE, "physStepCount", physStepCountMidhook);
    util_midhook(geode::base::get() + 0x238F6E, "restorePhysDt", restorePhysDtMidhook);
    util_midhook(geode::base::get() + 0x237E42, "earlyUpdate", earlyUpdateMidhook);
    util_midhook(geode::base::get() + 0x238BAA, "frameUpdate", frameUpdateMidhook);

    actionMgrOrig =
        reinterpret_cast<void (*)(void*, float)>(geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET);
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET),
        &actionMgrHook,
        "CCActionManager::update",
        tulip::hook::TulipConvention::Fastcall);

    auto p1 =
        Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + 0x23B4EA),
                          {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                           0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});

    auto p2 = Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + 0x4cd95c),
                                {0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90,
                                 0x90});

    auto p3 =
        Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + 0x3B994C), {0xeb, 0x5e});

    auto p4 = Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + 0x3BA508),
                                {0x90, 0x90, 0x90, 0x90, 0x90});

    g_patchAttempts = 4;
    g_patchFailures =
        (p1.isErr() ? 1 : 0) + (p2.isErr() ? 1 : 0) + (p3.isErr() ? 1 : 0) + (p4.isErr() ? 1 : 0);
    if (g_patchFailures)
        geode::log::error("[GucciBot] {} of 4 binary patches FAILED to apply", g_patchFailures);
}
