// engine_updater.cpp — GucciBot 10.0
// Direct port of Silicate's BotUpdater + hooks with GucciBot names

#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "trajectory.hpp"
#include "util_midhook.hpp"
#include "render/renderer.hpp"  // SLRenderer (Silicate FFmpeg renderer port)

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <safetyhook.hpp>
#include <fstream>

using namespace geode::prelude;

// ── SLOPE-EXIT DIAGNOSTIC LOGGER (2026-07-04) ─────────────────────────────────
// Comprehensive per-frame log for diffing PLAY vs CALC around slope exits, so the
// frame + field where the Calculate capture diverges is captured durably. Written
// to a dedicated file (Mod save dir / guccibot_slope.log, truncated per GD session)
// AND to geode.log as fallback. See CALC_SLOPE_EXIT.md.
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

// ─────────────────────────────────────────────────────────────────────────────
// GucciUpdater methods
// ─────────────────────────────────────────────────────────────────────────────

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
    // 2026-07-04-e: Calculate (fwAnalyzing) MUST use the FAST path. The [SLOPE] REND
    // data PROVED the renderer catches the f=1735 collision (ys=-5.425) via
    // runFastLockDelta (REND fast=1, dt=sl->getDt()=1/60), while CALC on the slow path
    // (fast=0) missed it (ys=0.127) at the SAME fixed dt. So for fixed-dt CALC the fast
    // path is REQUIRED -- it's what makes the renderer catch collision-threshold events
    // the slow path misses. Prior -b had the right idea (force fast) but patched the
    // wrong condition: m_lockDelta && Performance was the gate that stayed false during
    // CALC, so -b was a no-op (fast stayed 0). -e bypasses the whole gate. CALC is now
    // bit-identical to the renderer's physics path (fast + sl->getDt()).
    if (gb->fwAnalyzing) return true;
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
                                          // Calculate does NOT set rendering=true.
                                          // The working SLRenderer render runs with
                                          // rendering=false (renderer.recording is the
                                          // legacy flag, false during SLRenderer), and
                                          // at 4 steps/frame the clamp/overflow-flush
                                          // this gates never bite anyway. The -i build
                                          // added || fwAnalyzing here for no benefit;
                                          // removed so the capture's path is literally
                                          // the renderer's.
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

// ─────────────────────────────────────────────────────────────────────────────
// Fast lock delta path (Performance mode during playback)
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Slow lock delta path (Accuracy mode — the main path)
// ─────────────────────────────────────────────────────────────────────────────

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
        : upd.m_lastTfp;  // v7.1: null-safe during spawn edge cases
    float ssbDelta = std::abs(newTfx - upd.m_lastTfp);
    if (!calcSsb || ssbDelta <= 0) ssbDelta = (float)upd.getPhysicsDt();
    upd.m_lastTfp = newTfx;

    float delta = (float)upd.getPhysicsDt();
    upd.calculateSteps(realDt * upd.getTimeWarp() * upd.m_speedhack, ssbDelta);

    // Determinism during Calculate now comes from the fixed-dt scheduler override
    // in GB7CCScheduler::update (realDt is already 1/60 while fwAnalyzing), so
    // calculateSteps yields a constant step count (4 at 240 TPS) every frame. The
    // old force-1-step patch (2026-06-30-a) only masked the desync while realDt
    // still varied through the rest of the pipeline; this removes the root cause.

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

// ─────────────────────────────────────────────────────────────────────────────
// runUpdates — main dispatch (called from CCScheduler hook)
// ─────────────────────────────────────────────────────────────────────────────

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
        gb->renderer.recording;  // calcSsb is OFF for BOTH the working SLRenderer
                                 // render AND Calculate. PROVEN 2026-07-02-a via the
                                 // capture log: with calcSsb ON, ssbDelta = per-frame
                                 // timeForPos delta (~1/60), so calculateSteps yields
                                 // floor((1/60)/(1/60)) = 1 step/frame instead of 4 ->
                                 // player runs physics at 1/4 rate while the frame
                                 // counter + inputs assume full rate -> total desync,
                                 // death at ~8% (Silent Clubstep, no slopes). The
                                 // working renderer has calcSsb OFF (renderer.recording
                                 // is the LEGACY flag, false during SLRenderer), so
                                 // ssbDelta collapses to 1/tps -> 4 steps/frame. Do NOT
                                 // add || gb->fwAnalyzing here — that was the -i mistake.

    bool useAccLockDelta = m_lockDelta &&
        (m_lockDeltaMode == LockDeltaMode::Accuracy || gb->renderer.recording);

    // 2026-07-03-c DIAGNOSTIC: log exactly which physics branch Calculate takes and
    // why, every 25 frames. -b's force-fast should make CALC use runFastLockDelta
    // (est=1), but the log still shows est=4 -- so either force-fast isn't engaging
    // or the fast path hits its no-queued-input branch (single coarse step). This
    // logs the dispatch inputs so we can SEE which. REMOVE after diagnosis.
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

// ─────────────────────────────────────────────────────────────────────────────
// Midhooks — direct port from Silicate's updater.cpp
// Offsets: 0x237A7C physDt, 0x237DCE physStepCount,
//          0x238F6E restorePhysDt, 0x237E42 earlyUpdate, 0x238BAA frameUpdate
// ─────────────────────────────────────────────────────────────────────────────

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
    // Save checkpoint for backwards stepping
    if (!pl->m_playerDied && upd.m_backwardsStepping && !gb->renderer.recording) {
        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp) return;
        cp->retain();
        gb->practiceFix.saveState(cp, upd.getFrame());
    }
}

static void frameUpdateMidhook(SafetyHookContext&) {
    auto* gb  = GucciEngine::get();
    auto& upd = gb->updater;
    auto* pl  = GJBaseGameLayer::get();
    if (!pl || pl->m_resumeTimer > 0) return;

    if (!pl->m_playerDied) {
        if (PlayLayer::get()) upd.incrementFrame();
    }

    // ── DIAGNOSTIC (2026-06-23-d): Calculate physics desync ──
    // Log the player state every frame during playback — tagged PLAY (normal
    // playback) or CALC (Calculate's CAPTURE phase only; probing is excluded so
    // it doesn't flood the log). Diff a normal-Play run vs a Calculate run
    // frame-by-frame; the first frame they diverge is the root cause.
    // REMOVE this block once the desync is root-caused.
    if (gb->isPlaying()) {
        bool logIt = !gb->fwAnalyzing || (gb->fwState == GucciEngine::FwState::Capturing);
        if (logIt) {
            auto* plr = PlayLayer::get();
            if (plr && plr->m_player1) {
                auto* p = plr->m_player1;
                // 2026-07-02-f: added flip (m_isUpsideDown) + hold (thrust state) so
                // the PLAY-vs-CALC diff catches gravity/portal and input-application
                // divergences in the ship section (ships touch blocks/slopes, so
                // ground + flip + thrust all matter, not just y/yvel).
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

    // v10.2: drive the async frame-window analyzer. While analyzing, this rides
    // the real per-frame pipeline so the player actually moves (unlike the old
    // synchronous pump). No-op when not analyzing.
    gb->fwCkptCreatedThisFrame = false;   // reset per frame; fwTick's capture loop sets it
    if (gb->fwAnalyzing) gb->fwTick();

    // ── SLOPE-EXIT DIAGNOSTIC (2026-07-04) ──
    // Comprehensive per-frame log for BOTH PLAY and CALC-capture. Logged AFTER
    // fwTick so `ckpt` reflects a checkpoint created THIS frame (fwTick's capture
    // loop sets fwCkptCreatedThisFrame when it calls createCheckpoint). Diff PLAY
    // vs CALC at the slope-exit frame; the first diverging field = the noclip
    // mechanism. mode: C=cube H=ship B=ball U=ufo V=wave R=robot X=spider G=swing.
    // Written to guccibot_slope.log (Mod save dir) + geode.log. See CALC_SLOPE_EXIT.md.
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
                    "g={} flip={} dash={} mode={} hold={} steps={} ovf={:.4f} rs={} ckpt={} fast={} q={}",
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
                    gb->replay.getCurrentQueuedInput().has_value() ? 1 : 0));
            }
        }
    }

    // Autoclicker
    if (auto* pll = PlayLayer::get()) {
        auto res = Autoclicker::get()->processTick();
        if (res.p1Fire) pll->queueButton(1, res.p1Press, false, 0.0);
        if (res.p2Fire) pll->queueButton(1, res.p2Press, true,  0.0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CCScheduler hook
// ─────────────────────────────────────────────────────────────────────────────

class $modify(GB7CCScheduler, CCScheduler) {
    void update(float dt) override {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_onlyRefresh || !gb->enabled) {
            CCScheduler::update(dt);
            return;
        }
        // 2026-07-03-d: REMOVED the `if (gb->fwAnalyzing) dt = 1/60` override. The
        // [FWDISP] diagnostic proved capture ran at a rigid fixed 1/60 (exactly 4
        // steps/frame) while real playback uses slightly-varying realDt (3-5 steps),
        // and the slope snap at f~1750 is sensitive to that -- fixed-4 missed it,
        // varying hits it. Capture now uses the real realDt like normal playback, so
        // it steps identically and the slope snap fires. Cost: capture is no longer
        // bit-deterministic run-to-run (acceptable -- correct slopes matter more, and
        // the probe phase was already real-time). The original steps=1 timing bug was
        // the calcSsb gate, separately fixed in 2026-07-02-b, so removing this does
        // NOT reintroduce it.
        gb->updater.runUpdates(
            [this](float d){ this->CCScheduler::update(d); }, dt, false);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CCDirector hook
// ─────────────────────────────────────────────────────────────────────────────

class $modify(GB7CCDirector, CCDirector) {
    void drawScene() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) return CCDirector::drawScene();

        gb->scheduler.update(this->getDeltaTime());
        gb->updater.updateAudioSpeedhack();

        auto* pl = PlayLayer::get();

        // v10.3 (render port Stage 1): drive SLRenderer (Silicate FFmpeg
        // renderer) when queued/recording. Mutually exclusive with the legacy
        // TTR renderer below — only one is active at a time.
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

        // 2026-07-04-e: Calculate (fwAnalyzing) uses the RENDERER'S exact regime. The
        // [SLOPE] REND data PROVED the renderer catches the f=1735 collision (REND
        // ys=-5.425, fast=1, dt=sl->getDt()=1/60) while CALC on the slow path (fast=0)
        // missed it (ys=0.127) at the same fixed dt. So CALC feeds sl->getDt() (the
        // renderer's fixed dt) here -- NOT getPhysicsDt() -- so its fast-path step
        // matches the renderer exactly. Combined with useFastLockDelta() returning true
        // during fwAnalyzing, CALC's physics path is bit-identical to the renderer's:
        // runFastLockDelta + sl->getDt(). Verify [SLOPE]: CALC catches f=1735 (ys~-5.4).
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

// ─────────────────────────────────────────────────────────────────────────────
// CCActionManager hook + midhook install
// ─────────────────────────────────────────────────────────────────────────────

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

    // Hook CCActionManager::update
    actionMgrOrig = reinterpret_cast<void(*)(void*,float)>(
        geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET);
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(geode::base::getCocos() + ACTIONMGR_UPDATE_OFFSET),
        &actionMgrHook,
        "CCActionManager::update",
        tulip::hook::TulipConvention::Fastcall);

    // Patch GJBaseGameLayer::resetLevelVariables (don't release buttons)
    // v8.10 self-check: track patch successes instead of discarding results.
    auto p1 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x23B4EA),
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
         0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90});

    // Patch UILayer::handleKeypress
    auto p2 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x4cd95c),
        {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
         0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90});

    // Patch PlayLayer::resetLevel (Silicate 0x3B994C)
    auto p3 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x3B994C),
        {0xeb, 0x5e});

    // Patch PlayLayer::resumeAndRestart
    auto p4 = Mod::get()->patch(
        reinterpret_cast<void*>(geode::base::get() + 0x3BA508),
        {0x90,0x90,0x90,0x90,0x90});

    g_patchAttempts = 4;
    g_patchFailures = (p1.isErr()?1:0)+(p2.isErr()?1:0)+(p3.isErr()?1:0)+(p4.isErr()?1:0);
    if (g_patchFailures)
        geode::log::error("[GucciBot] {} of 4 binary patches FAILED to apply", g_patchFailures);
}
