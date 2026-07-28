// hook_playlayer.cpp — GucciBot 10.0
// Direct port of Silicate's PlayLayer hook

#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "trajectory.hpp"
#include "hitboxes.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <safetyhook.hpp>

using namespace geode::prelude;

class $modify(GB7PlayLayer, PlayLayer) {

    // ── Checkpoint management ─────────────────────────────────────────────────

    CheckpointObject* markCheckpoint() {
        if (!GucciEngine::get()->enabled) return PlayLayer::markCheckpoint();
        auto* cp = createCheckpoint();
        storeCheckpoint(cp);
        return cp;
    }

    void storeCheckpoint(CheckpointObject* obj) {
        if (!GucciEngine::get()->enabled) return PlayLayer::storeCheckpoint(obj);
        auto* gb = GucciEngine::get();
        obj->retain();
        addToSection(obj->m_physicalCheckpointObject);
        if (this->m_activatedCheckpoint) {
            gb->practiceFix.m_platformerCheckpoints.push_back(std::make_pair(obj, (CheckpointObject*)this->m_activatedCheckpoint));
            return;
        }
        gb->practiceFix.saveCurrent(obj, gb->updater.getFrame());
    }

    void loadFromCheckpoint(CheckpointObject* obj) {
        if (!GucciEngine::get()->enabled) return PlayLayer::loadFromCheckpoint(obj);
        auto& pf = GucciEngine::get()->practiceFix;
        if (pf.m_loadCheckpoint) {
            pf.restorePreviousFrame([this](auto* cp){ this->PlayLayer::loadFromCheckpoint(cp); });
            pf.m_hasDiedNormally = false;
            return;
        }
        if (pf.m_savedCheckpoints.empty()) { PlayLayer::loadFromCheckpoint(obj); return; }
        if (pf.m_shouldLoadPlatformer) {
            PlayLayer::loadFromCheckpoint(pf.m_platformerCheckpoints.back().first);
            return;
        }
        auto cp = pf.m_savedCheckpoints.back();
        pf.clearStoredFrames();
        PlayLayer::loadFromCheckpoint(cp.m_checkpoint);
        pf.applyLatest();
    }

    void removeCheckpoint(bool p0) {
        if (!GucciEngine::get()->enabled) return PlayLayer::removeCheckpoint(p0);
        auto& pf = GucciEngine::get()->practiceFix;
        if (pf.m_savedCheckpoints.empty()) return;
        auto& cp = pf.m_savedCheckpoints.back();
        auto* obj = cp.m_checkpoint->m_physicalCheckpointObject;
        removeObjectFromSection(obj);
        if (obj->m_glowSprite) { obj->m_glowSprite->removeMeAndCleanup(); obj->m_glowSprite = nullptr; }
        obj->removeMeAndCleanup();
        cp.m_checkpoint->release();
        pf.m_savedCheckpoints.pop_back();
    }

    void removeAllCheckpoints() {
        if (!GucciEngine::get()->enabled) return PlayLayer::removeAllCheckpoints();
        if (GucciEngine::get()->updater.m_fullReset) return;
        while (!GucciEngine::get()->practiceFix.m_savedCheckpoints.empty())
            removeCheckpoint(false);
    }

    // ── Init / quit ───────────────────────────────────────────────────────────

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) return PlayLayer::init(level, useReplay, dontCreateObjects);

        gb->practiceFix.clearStoredFrames();
        gb->practiceFix.clearPlatformer(false);

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_clickOnSteps = false;
        m_clickBetweenSteps = false;

        TrajectoryPredictionService::get().updatePreview(this);
        gb->updater.m_frameOnLastAttempt = 0;
        gb->updater.m_lastTfp = 0.0f;
        gb->m_levelLength = m_levelLength;

        return true;
    }

    void onQuit() {
        auto* gb = GucciEngine::get();
        if (gb->renderer.recording) gb->renderer.stop(gb->updater.getFrame());
        TrajectoryPredictionService::get().updatePreview(nullptr);
        PlayLayer::onQuit();
        gb->practiceFix.clearStoredFrames();
        gb->practiceFix.clearPlatformer(true);
        gb->replay.onExit();
        Autoclicker::get()->reset();
    }

    // ── Death / reset helpers ─────────────────────────────────────────────────

    void delayedResetLevel() {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_canDie) gb->updater.m_inputIsDeath = true;
        PlayLayer::delayedResetLevel();
    }

    void fullReset() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) return PlayLayer::fullReset();
        auto& upd = gb->updater;
        if (upd.m_canDie) {
            upd.m_fullReset = true;
            // fake full reset (without clearing checkpoints)
            m_gameState.m_totalTime = 0.0;
            m_gameState.m_levelTime = 0.0;
            m_player1->m_totalTime  = 0.0;
            m_player2->m_totalTime  = 0.0;
            m_attempts = 0; m_jumps = 0;
            m_objectsDeactivated = true;
            m_freezeStartCamera  = true;
            gb->practiceFix.clearPlatformer(true);
            resetLevel();
        } else if (upd.m_expectsDeath) {
            if (auto* ell = getChildByID("EndLevelLayer")) ell->removeFromParent();
            m_gameState.m_totalTime = 0.0; m_gameState.m_levelTime = 0.0;
            m_player1->m_totalTime = 0.0; m_player2->m_totalTime = 0.0;
            m_attempts = 0; m_jumps = 0; m_objectsDeactivated = true; m_freezeStartCamera = true;
            gb->practiceFix.clearPlatformer(true);
            resetLevel();
        } else {
            PlayLayer::fullReset();
            gb->practiceFix.clearPlatformer(true);
            upd.m_frameOnLastAttempt = 0;
        }
    }

    void checkIfResetWasExpected(uint64_t deathFrame) {
        auto* gb = GucciEngine::get();
        while (auto input = gb->replay.getNextInput((uint32_t)deathFrame)) {
            if (input->m_type == gb::ActionType::Death) gb->updater.m_expectsDeath = true;
            else break;
        }
    }

    bool handleResetWithCheckpoints(uint64_t deathFrame) {
        auto* gb  = GucciEngine::get();
        auto& pf  = gb->practiceFix;
        auto& upd = gb->updater;

        if (gb->isRecording())  // 2026-06-24 diagnostic: confirm int-death cause/fix
            log::info("[IDEATH] rec death @{} canDie={} expectsDeath={} inputIsDeath={} fullReset={} hasCK={} stored={}",
                deathFrame, upd.m_canDie, upd.m_expectsDeath, upd.m_inputIsDeath, upd.m_fullReset,
                !pf.m_savedCheckpoints.empty(), pf.m_storedFrames.size());

        // v10.2 (Juice): check for a REGULAR practice checkpoint FIRST. The old
        // code let the m_canDie branch run first and return false for non-
        // platformer levels, so a normal practice checkpoint was never seen — it
        // fell through to "full restart" with the wrong frame offset (deathFrame+1
        // instead of the checkpoint's frame). That's why dying past a checkpoint
        // logged as a full restart and the frame counter shifted (233 -> 107).
        // v10.3 (P1): an INTENTIONAL death (m_canDie armed) must NOT resume at a
        // checkpoint — the feature's whole point is a deliberate full restart from
        // the level beginning. Without `!upd.m_canDie` here, an armed intentional
        // death hit this branch first and resumed at the last checkpoint instead of
        // restarting, which broke recording (it sent you back to the checkpoint,
        // not the level start). Normal deaths (m_canDie false) still take this
        // checkpoint-resume path, so the v10.2 fix above is preserved.
        if (!pf.m_savedCheckpoints.empty() && !pf.m_isBackstep && !upd.m_canDie) {
            // Resume the absolute frame at the checkpoint's frame so getFrame()
            // keeps counting from there instead of restarting at 0.
            upd.m_frameOnLastAttempt = pf.m_savedCheckpoints.back().m_frameOffset;
            m_checkpointArray->addObject(pf.m_savedCheckpoints.back().m_checkpoint);
            return true;
        }
        // Platformer checkpoint (platformer levels use a separate store).
        // 2026-06-24 int-death fix: m_expectsDeath is a PLAYBACK-only concept (a
        // death the macro is going to replay). It must NOT turn a normal RECORDING
        // death into an intentional one — only the user arming m_canDie (the
        // button) should. A lingering m_expectsDeath made every recording death
        // continue the frame counter (deathFrame+1) instead of resetting to 0, so
        // the recording never reset ("actions don't reset") = treated as int-death.
        if (upd.m_canDie || (upd.m_expectsDeath && !gb->isRecording())) {
            if (!pf.m_platformerCheckpoints.empty()) {
                upd.m_frameOnLastAttempt = deathFrame + 1;
                m_checkpointArray->addObject(pf.m_platformerCheckpoints.back().first);
                pf.m_shouldLoadPlatformer = true;
                return true;
            }
            // v10.3 (P1): intentional/expected death with no platformer checkpoint.
            // The LEVEL fully restarts (PlayLayer::resetLevel above already reset it
            // to the start) — but the frame counter must CONTINUE from deathFrame+1,
            // NOT reset to 0. If it reset to 0, the post-death attempt's inputs would
            // be recorded at frames 0.. and collide with the pre-death inputs (same
            // frame numbers); addInputToReplay's frame-order guard would then block
            // them entirely ("stuck at 29, no new inputs after death") and playback's
            // onReset(0) would replay attempt 1 instead of attempt 2. Continuing the
            // counter keeps frames monotonic so attempt 2 records and plays back
            // cleanly. (The platformer path above already did deathFrame+1.)
            upd.m_frameOnLastAttempt = deathFrame + 1;
            return false;
        }
        if (pf.m_loadCheckpoint && !pf.m_storedFrames.empty()) {
            upd.m_frameOnLastAttempt = pf.m_storedFrames.back().frame;
            m_checkpointArray->addObject(pf.m_storedFrames.back().state.m_checkpoint);
            return true;
        }
        upd.m_frameOnLastAttempt = 0;
        pf.clearPlatformer(true);
        return false;
    }

    void updateRandomSeedOnReset() {
        auto* gb  = GucciEngine::get();
        auto& rs  = gb->replay;
        uint64_t& state = *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90);

        if (!gb->updater.m_expectsDeath)
            rs.m_startingSeedThisAttempt = rs.m_startingSeed;

        if (gb->isRecording()) {
            rs.m_startingSeedThisAttempt = state;
            if (gb->practiceFix.m_savedCheckpoints.empty() &&
                !gb->practiceFix.m_loadCheckpoint && !gb->updater.m_canDie) {
                state = 214013 * state + 2531011;
                rs.m_startingSeed = state;
                rs.m_startingSeedThisAttempt = state;
            }
        } else {
            state = rs.m_startingSeedThisAttempt;
        }
    }

    void restoreHoldOnReset(uint64_t deathFrame) {
        auto* gb  = GucciEngine::get();
        auto& rs  = gb->replay;
        auto& upd = gb->updater;

        m_queuedButtons.clear();

        if (gb->isPlaying()) {
            if (gb->practiceFix.m_savedCheckpoints.empty()) {
                m_player1->releaseAllButtons();
                m_player2->releaseAllButtons();
            }
            if (upd.m_expectsDeath) {
                // skip queued inputs at this death frame
                while (auto inp = rs.getCurrentQueuedInput()) {
                    if (inp->m_frame == (uint32_t)deathFrame) rs.advanceInputIndex();
                    else break;
                }
            }
            upd.m_canDie = false; upd.m_inputIsDeath = false; upd.m_expectsDeath = false;
            processQueuedButtons(0.0, true);
            upd.m_tpsOverflow = 0.0;
            return;
        }

        if (gb->isRecording()) {
            if (upd.m_canDie) { m_player1->releaseAllButtons(); m_player2->releaseAllButtons(); return; }

            // v10.2 (Juice): if the player is dying, release held buttons but do
            // NOT record the release as an input. A button released during the
            // death period (die-while-holding) was being captured as a phantom
            // input that then appeared on the next attempt. The death is recorded
            // separately; the death-period release is not a real input.
            if (m_player1->m_isDead || m_player2->m_isDead) {
                m_player1->releaseAllButtons();
                m_player2->releaseAllButtons();
                return;
            }

            bool p1H = m_uiLayer->m_p1Jumping || m_uiLayer->m_p1TouchId != -1;
            bool p2H = m_uiLayer->m_p2Jumping || m_uiLayer->m_p2TouchId != -1;
            if (rs.hasFlippedControls()) std::swap(p1H, p2H);
            p1H |= p2H && !m_levelSettings->m_twoPlayerMode;

            auto check = [&](bool held, int button, bool p2) {
                int idx = p2 ? 2 : 1;
                auto* p = (idx == 1) ? m_player1 : m_player2;
                if (held != (bool)p->m_holdingButtons[button])
                    queueButton(button, held, rs.playerFlipped(p2), 0.0);
            };
            check(p1H, 1, false);
            if (m_levelSettings->m_twoPlayerMode) check(p2H, 1, true);
            rs.m_lastInputs.clear();
        }

        // Idle (not recording, not playing). [ROOT CAUSE 2026-07-01, log-proven]
        // Vanilla PlayLayer::resetLevel() does NOT release a button held through
        // death (postVanilla h1=true), so without intervention the respawn held
        // jump from frame 1 -> instant death -> "in clusters" loop, recovering
        // only when the user clicked jump again. Sync the jump hold to the
        // player's ACTUAL current input — the SAME check() logic the recording
        // branch above uses — so BOTH directions are correct:
        //  - Phantom hold (died holding, then RELEASED): p1H=false but
        //    m_holdingButtons[1] stuck true -> queue a RELEASE.
        //  - Legitimate hold-through-respawn (died holding AND still holding):
        //    p1H=true, vanilla may have cleared m_holdingButtons[1] -> queue a
        //    PRESS so the hold persists into the next attempt (ship/ufo/wave).
        // (History: -07-01-d used releaseAllButtons(), which doesn't touch
        // m_holdingButtons at all; -07-01-e blindly released everything, which
        // fixed the phantom but killed the legitimate hold-through case.)
        if (!gb->isRecording() && !gb->isPlaying()) {
            bool p1H = m_uiLayer->m_p1Jumping || m_uiLayer->m_p1TouchId != -1;
            bool p2H = m_uiLayer->m_p2Jumping || m_uiLayer->m_p2TouchId != -1;
            if (rs.hasFlippedControls()) std::swap(p1H, p2H);
            p1H |= p2H && !m_levelSettings->m_twoPlayerMode;

            auto check = [&](bool held, int button, bool p2) {
                int idx = p2 ? 2 : 1;
                auto* p = (idx == 1) ? m_player1 : m_player2;
                if (held != (bool)p->m_holdingButtons[button])
                    queueButton(button, held, rs.playerFlipped(p2), 0.0);
            };
            check(p1H, 1, false);
            if (m_levelSettings->m_twoPlayerMode) check(p2H, 1, true);
        }
    }

    void addDeathInput(uint64_t deathFrame) {
        auto* gb = GucciEngine::get();
        if (!gb->updater.m_canDie) return;
        auto type = gb->updater.m_inputIsDeath    ? gb::ActionType::Death :
                    gb->updater.m_fullReset        ? gb::ActionType::RestartFull :
                                                     gb::ActionType::Restart;
        gb->replay.m_actionAtom.addAction((uint32_t)deathFrame, type, false, false);
    }

    void intentionalResetDone() {
        auto& upd = GucciEngine::get()->updater;
        upd.m_canDie = false; upd.m_inputIsDeath = false;
        upd.m_expectsDeath = false; upd.m_fullReset = false;
        upd.m_tpsOverflow = 0.0;
    }

    // ── resetLevel — the big one ──────────────────────────────────────────────

    void resetLevel() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) {
            m_player1->releaseAllButtons();
            m_player2->releaseAllButtons();
            return PlayLayer::resetLevel();
        }

        m_practiceMusicSync = true;
        auto& upd = gb->updater;
        upd.m_tpsOverflow  = 0.0;
        upd.m_respawnTimer = gb->hackRespawnInstant ? 0 : 2;  // v8.12 instant respawn
        upd.incrementFrame();
        uint64_t deathFrame = upd.getFrame();

        checkIfResetWasExpected(deathFrame);
        bool hasAddedCheckpoint = handleResetWithCheckpoints(deathFrame);
        upd.resetFrame();

        // Clear hitbox trail
        HitboxOverlay::get()->clearTrail();

        updateRandomSeedOnReset();
        PlayLayer::resetLevel();

        if (hasAddedCheckpoint) {
            gb->practiceFix.m_shouldLoadPlatformer = false;
            m_checkpointArray->removeLastObject();
            upd.m_onlyRefresh = true;
            m_extraDelta = 0.0f;
            CCScheduler::get()->update(0.0f);
            upd.m_onlyRefresh = false;
        }

        // v10.2 (Juice): the TRUE respawn frame is m_frameOnLastAttempt, which
        // handleResetWithCheckpoints already set above: the checkpoint resume
        // frame on a checkpoint death, or 0 on a full restart. (The old code read
        // m_storedFrames here, but it's empty/consumed by now, so it always fell
        // back to 0 — which is why every respawn logged as respawn@0 even when
        // dying mid-level.)
        uint32_t respawnFrame = (uint32_t)upd.m_frameOnLastAttempt;
        gb->replay.onReset(respawnFrame, (uint32_t)deathFrame);
        Autoclicker::get()->reset();
        TrajectoryPredictionService::get().updatePreview(this);

        restoreHoldOnReset(deathFrame);
        addDeathInput(deathFrame);

        if (!upd.m_canDie) {
            gb->replay.m_flipProcessingInputs = true;
            processQueuedButtons(0.0, true);
            gb->replay.m_flipProcessingInputs = false;
        }

        intentionalResetDone();
        gb->practiceFix.m_hasDiedNormally = false;
        gb->practiceFix.m_isBackstep      = false;
        upd.breakLoop();
    }

    // v8.12 MoreHacks: hide the attempt label if requested. m_attemptLabel is a
    // verified PlayLayer binding; postUpdate is a confirmed-hookable PlayLayer
    // method (used in trajectory.cpp) so the label stays hidden as GD updates it.
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto* gb = GucciEngine::get();
        if (gb->hackHideAttempts && m_attemptLabel)
            m_attemptLabel->setVisible(false);
    }

    // ── destroyPlayer — noclip, prevent death ─────────────────────────────────

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto* gb  = GucciEngine::get();
        auto& upd = gb->updater;

        // v8.8 frame-window sweep: while analyzing, a death is a *probe result*,
        // not a real death. Flag it and return before the parent so the live
        // death cascade (respawn, attempt count, effects) never fires — same
        // technique noclip uses below.
        if (gb->fwAnalyzing) {
            if (obj != m_anticheatSpike) {
                gb->fwProbeDied = true;
                // [CAP-DIE] DIAGNOSTIC (2026-07-02-a): mark the exact death frame
                // + x + percent so the capture desync point is visible in the log.
                // REMOVE after diagnosis.
                float xp  = player ? player->m_position.x : -1.f;
                float pct = m_levelLength > 0.f ? xp / m_levelLength * 100.f : -1.f;
                log::info("[CAP-DIE] f={} x={:.1f} pct={:.1f}", upd.getFrame(), xp, pct);
            }
            return;
        }

        // Noclip: intercept before parent
        if (gb->noclipEnabled && obj != m_anticheatSpike) {
            gb->noclipDeathBlocked = true;
            float total = m_levelLength > 0.f ? m_levelLength : 1.f;
            gb->noclipAccuracy = std::clamp((float)(m_gameState.m_levelTime / total), 0.f, 1.f);
            if (gb->noclipThreshold > 0.f && gb->noclipAccuracy >= gb->noclipThreshold)
                gb->noclipEnabled = false;
            if (gb->noclipDeathFlash) {
                player->runAction(CCSequence::create(
                    CCTintTo::create(0.1f,
                        (GLubyte)(gb->noclipDeathColorR * 255),
                        (GLubyte)(gb->noclipDeathColorG * 255),
                        (GLubyte)(gb->noclipDeathColorB * 255)),
                    CCTintTo::create(0.1f, 255, 255, 255), nullptr));
            }
            return;
        }

        // Trajectory ghost players don't die
        // no fake player check in v7

        PlayLayer::destroyPlayer(player, obj);

        // v8.12 MoreHacks: suppress the white death flash if requested.
        if (gb->hackNoSpikeFlash && obj != m_anticheatSpike) {
            if (auto* fl = this->getChildByID("flash"))
                fl->setVisible(false);
        }

        // v8.12 MoreHacks: auto-retry. Uses GucciScheduler-style countdown in
        // the update hook (verified per-frame path) with a one-shot guard,
        // keeping to APIs proven in this codebase.
        if (gb->hackAutoRetry && obj != m_anticheatSpike && !gb->isPlaying()) {
            gb->pendingAutoRetry = std::clamp(gb->hackAutoRetryDelay, 0.05f, 2.f);
        }

        // Prevent death (after calling parent so player dies, then we backstep)
        if (upd.m_preventDeath && obj != m_anticheatSpike) {
            upd.backwardsStep();
            processQueuedButtons(0.0, true);
            if (upd.m_autoFlipOnDeath) {
                bool p2 = (player == m_player2);
                queueButton(1, !player->m_jumpBuffered, p2, 0.0);
                upd.setPaused(false);
            }
        }

        if (!gb->practiceFix.m_loadCheckpoint)
            gb->practiceFix.m_hasDiedNormally = true;
    }

    // ── Level complete / autosave ─────────────────────────────────────────────

    void levelComplete() {
        PlayLayer::levelComplete();
        auto* gb = GucciEngine::get();
        // v8.3: the old logic backed up OR saved — with an existing file it
        // backed up the stale copy and never wrote the fresh completion.
        if (!gb->autosaveAtLevelEnd) return;
        if (!gb->isRecording() || gb->replay.m_actionAtom.empty()) return;
        auto path = gb->replay.getCurrentPath();
        if (gb->replayBackupsEnabled) gb->replay.backupExisting(path);
        gb->replay.save(path);
    }

    void setupHasCompleted() {
        auto* gb = GucciEngine::get();
        // renderer.startIfQueued() not available in TTR renderer
        PlayLayer::setupHasCompleted();
    }

    // ── Misc ──────────────────────────────────────────────────────────────────

    void pauseGame(bool p0) {
        m_gameState.m_pauseCounter = 0;
        PlayLayer::pauseGame(p0);
    }

    void updateAttempts() {
        if (GucciEngine::get()->practiceFix.m_isBackstep) return;
        PlayLayer::updateAttempts();
    }

    void updateVisibility(float dt) {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_extrapolateFrames) dt = CCDirector::get()->getDeltaTime();
        PlayLayer::updateVisibility(dt);
        HitboxOverlay::get()->draw(this);
    }

    // Layout mode: skip non-gameplay objects
    void addObject(GameObject* obj) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled || !gb->layoutMode) return PlayLayer::addObject(obj);
        static const std::unordered_set<int> HIDE_IDS = {
            32,33,1006,1007,29,30,104,105,221,717,718,743,744,899,915,
            2903,2904,2905,2907,2909,2910,2911,2912,2913,2914,2915,2916,
            2917,2919,2920,2921,2922,2923,2924,3029,3030,3031,3606,3612
        };
        if (HIDE_IDS.count(obj->m_objectID)) obj->m_isHide = true;
        else { obj->setOpacity(255); obj->m_isHide = false; }
        PlayLayer::addObject(obj);
    }
};
