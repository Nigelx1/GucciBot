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

// Geode's own console log doesn't persist to a findable file on every setup
// (confirmed absent on this machine) -- log::info() alone isn't something a
// tester can actually hand back as evidence. Mirrors slopeLog's pattern:
// dedicated file in the mod's save dir, truncated fresh each GD session.
static std::ofstream g_frameIncLog;
void logFrameIncrement(const char* callSite, uint32_t frame) {
    if (!g_frameIncLog.is_open()) {
        auto path = Mod::get()->getSaveDir() / "guccibot_frameinc.log";
        g_frameIncLog.open(path, std::ios::out | std::ios::trunc);
        log::info("[FRAMEINC] log file at: {}", path.string());
    }
    if (g_frameIncLog.is_open()) {
        g_frameIncLog << callSite << " -> frame " << frame << '\n';
        g_frameIncLog.flush();
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
    // Performance mode removed entirely (2026-08-19, Nigel's call, per Juice's
    // testing): it collapsed multiple physics ticks into a single scheduler
    // update to catch up to the next queued input, but GucciBot's own frame
    // counter only increments once per scheduler call -- so the frame count
    // fell behind how much the game had actually simulated, every time that
    // catch-up path engaged. That's what Accuracy mode avoided, and why
    // switching to it visibly fixed/reduced several of Juice's frame-skip
    // reports. Casual botting doesn't need Performance's speed badly enough
    // to be worth the inaccuracy -- always false now; kept as a named method
    // rather than inlining `false` at every call site (runUpdates, the two
    // register-patch midhooks below, hook_gjbasegamelayer.cpp) since removing
    // the method itself would touch more files for no behavioral gain.
    return false;
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

                        if (gb->fwAnalyzing && (getFrame() % 25) == 0) {
        auto qi = gb->replay.getCurrentQueuedInput();
        log::info("[FWDISP] f={} enterBlock={} lockD={} hasInput={} realDt={:.6f} est={}",
                  getFrame(), m_lockDelta && isPlayLayer,
                  m_lockDelta, qi.has_value(), realDt, estimatedStepCount);
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
        // earlyUpdateMidhook (0x237E42) fires before frameUpdateMidhook's own
        // increment (0x238BAA) within the same native tick -- same off-by-one
        // Juice found for checkpoints/clicks in general, confirmed here by
        // the two hooks' relative offsets rather than just by analogy.
        gb->practiceFix.saveState(cp, upd.getFrame() + 1);
    }
}

static char gamemodeChar(PlayerObject* p) {
    if (!p) return 'C';
    if (p->m_isRobot)  return 'R';
    if (p->m_isSpider) return 'X';
    if (p->m_isSwing)  return 'G';
    if (p->m_isShip)   return 'H';
    if (p->m_isBall)   return 'B';
    if (p->m_isBird)   return 'U';
    if (p->m_isDart)   return 'V';
    return 'C';
}

// Mirrors fwClassifyOrbTouch/fwIsDashOrbType/fwIsNonDashOrbType in
// engine_core.cpp -- kept as its own copy here (this codebase's existing
// convention for small per-file classification helpers) rather than shared,
// since this one runs at RECORD time. Ground truth captured here is what
// Calculate's Capturing pass reads back later instead of trusting its own
// live m_touchingRings (see MacroPathSample::p1OrbDash's comment).
static void classifyOrbTouchForCapture(PlayerObject* player, bool& outDash, bool& outNonDash) {
    outDash = false;
    outNonDash = false;
    if (!player || !player->m_touchingRings) return;
    for (auto* obj : CCArrayExt<GameObject*>(player->m_touchingRings)) {
        if (!obj) continue;
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
    auto* gb  = GucciEngine::get();
    auto& upd = gb->updater;
    auto* pl  = GJBaseGameLayer::get();
    if (!pl || pl->m_resumeTimer > 0) return;

    if (!pl->m_playerDied) {
        if (PlayLayer::get()) {
            upd.incrementFrame();
            if (upd.m_logFrameIncrements)
                logFrameIncrement("frameUpdateMidhook", upd.getFrame());
        }

        // Ground-truth capture: live recording always grows this fresh (cleared at
        // record-start). Normal playback (bot replaying a loaded macro, NOT
        // Calculate) backfills the same data the first time a macro plays through --
        // this is what lets an imported/converted macro (no native GucciBot
        // recording behind it) still get path data, just by being played back once.
        // Either way we only ever append past what's already captured, so a macro's
        // first clean pass through a frame is what sticks as ground truth.
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
                // Indexed by frame (see m_pathSamples' index==frame contract in
                // GucciBot.hpp), not appended -- getFrame() is already
                // post-incremented by the time we get here, so a plain
                // push_back() would silently land one slot early. resize()
                // also means a capture gap (e.g. resuming after Calculate's
                // analysis pass skipped capture for a while) fills the missed
                // indices with a blank placeholder instead of permanently
                // shifting every later index out of alignment with its frame.
                samples.resize(frame + 1);
                samples[frame] = smp;
                if (!gb->isRecording()) gb->replay.m_pathSamplesDirty = true;
            }
        }

        if (gb->replay.m_pathSamplesDirty && gb->isPlaying() && !gb->fwAnalyzing) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_hasCompletedLevel) {
                gb->replay.savePathSamplesNow();
                gb->replay.m_pathSamplesDirty = false;
            }
        }

        // Calculate's capture pass: force the player's kinematic state to match
        // ground truth captured during the original recording, instead of trusting
        // this pass's own physics tick to independently re-derive the same values.
        // This is deliberate -- the whole P3 investigation has been chasing WHY
        // Calculate's simulation diverges from the real playthrough at certain
        // frames (e.g. missing a slope-exit launch impulse) without finding the
        // mechanism. Forcing ground truth here sidesteps needing to ever find it,
        // for the capture pass specifically. Only engages when the loaded macro
        // actually has path-sample data (recorded after this feature existed) --
        // older macros silently fall back to the previous (unforced) behavior.
        if (gb->fwAnalyzing && gb->fwState == GucciEngine::FwState::Capturing) {
            auto& samples = gb->replay.m_pathSamples;
            uint32_t frame = upd.getFrame();
            if (frame < samples.size()) {
                auto* plr = PlayLayer::get();
                auto const& s = samples[frame];
                if (plr && plr->m_player1) {
                    auto* p1 = plr->m_player1;
                    p1->setPosition({ s.p1x, s.p1y });
                    p1->m_playerSpeed = s.p1XVel;
                    p1->m_yVelocity   = s.p1YVel;
                    p1->setRotation(s.p1Rot);
                    p1->m_isOnGround   = s.p1OnGround;
                    p1->m_isUpsideDown = s.p1UpsideDown;
                    p1->m_isDashing    = s.p1Dashing;
                }
                if (s.hasP2 && plr && plr->m_player2) {
                    auto* p2 = plr->m_player2;
                    p2->setPosition({ s.p2x, s.p2y });
                    p2->m_playerSpeed = s.p2XVel;
                    p2->m_yVelocity   = s.p2YVel;
                    p2->setRotation(s.p2Rot);
                    p2->m_isOnGround   = s.p2OnGround;
                    p2->m_isUpsideDown = s.p2UpsideDown;
                    p2->m_isDashing    = s.p2Dashing;
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
