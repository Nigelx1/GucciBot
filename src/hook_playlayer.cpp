#include "GucciBot.hpp"
#include "autoclicker.hpp"
#include "trajectory.hpp"
#include "hitboxes.hpp"
#include "jupiterghost.hpp"
#include "trainerghost.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <safetyhook.hpp>

using namespace geode::prelude;

class $modify(GB7PlayLayer, PlayLayer) {

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
        // Juice's testing (2026-08-19): a checkpoint placed on what the bot
        // reads as frame N actually happens on frame N+1 -- storeCheckpoint
        // is native GD's own checkpoint-touch callback, fired before this
        // tick's own frame increment (frameUpdateMidhook). Confirmed the
        // same +1 applies to input clicks too (see addInputToReplay).
        gb->practiceFix.saveCurrent(obj, gb->updater.getFrame() + 1);
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
        // Nigel: Calculate should save no matter what it stops for. fwTick()
        // only cancels (and saves) when it notices PlayLayer is already gone
        // -- but that check runs from frameUpdateMidhook, which itself bails
        // before fwTick() if GJBaseGameLayer is already gone too (a full exit
        // to menu can tear both down before another tick ever runs). Catch it
        // here instead, while the layer is still valid enough for
        // cancelAnalysis()'s own PlayLayer::get() check to work normally.
        if (gb->fwAnalyzing) gb->cancelAnalysis();
        if (gb->renderer.recording) gb->renderer.stop(gb->updater.getFrame());
        TrajectoryPredictionService::get().updatePreview(nullptr);
        PlayLayer::onQuit();
        gb->practiceFix.clearStoredFrames();
        gb->practiceFix.clearPlatformer(true);
        gb->replay.onExit();
        Autoclicker::get()->reset();
    }

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

        if (gb->isRecording())
            log::info("[IDEATH] rec death @{} canDie={} expectsDeath={} inputIsDeath={} fullReset={} hasCK={} stored={}",
                deathFrame, upd.m_canDie, upd.m_expectsDeath, upd.m_inputIsDeath, upd.m_fullReset,
                !pf.m_savedCheckpoints.empty(), pf.m_storedFrames.size());

                                                                                                                if (!pf.m_savedCheckpoints.empty() && !pf.m_isBackstep && !upd.m_canDie) {
                                    upd.m_frameOnLastAttempt = pf.m_savedCheckpoints.back().m_frameOffset;
            m_checkpointArray->addObject(pf.m_savedCheckpoints.back().m_checkpoint);
            return true;
        }
                                                                if (upd.m_canDie || (upd.m_expectsDeath && !gb->isRecording())) {
            if (!pf.m_platformerCheckpoints.empty()) {
                upd.m_frameOnLastAttempt = deathFrame + 1;
                m_checkpointArray->addObject(pf.m_platformerCheckpoints.back().first);
                pf.m_shouldLoadPlatformer = true;
                return true;
            }
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

                                                                        if (m_player1->m_isDead || m_player2->m_isDead) {
                // Juice: dying mid-click left one stray input carried into the
                // restart, self-correcting only on a second restart. First
                // attempt at a fix (queueButton-ing a recorded release here,
                // based on live m_holdingButtons state) didn't hold up in
                // Juice's retest -- most likely because GD's own death
                // handling already clears m_holdingButtons before this runs,
                // making that check read false and the fix a silent no-op.
                // Replaced with Juice's own algorithm instead, which doesn't
                // depend on live state at all: see onReset() (engine_core.cpp)
                // for the actual fix -- it inspects the RECORDED macro data
                // directly (was the last action before the checkpoint clip a
                // press with no matching release?) and cleans that up, plus
                // suppresses whatever release comes in next in case the
                // physical button is still down through the reset. This
                // branch just releases the live buttons now, same as always.
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

    void resetLevel() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) {
            m_player1->releaseAllButtons();
            m_player2->releaseAllButtons();
            return PlayLayer::resetLevel();
        }

        // Bot-triggered resets (Calculate starting/restarting a probe run, a
        // recording/playback restart, etc.) call resetLevel() directly rather
        // than going through the player pressing Retry off the end screen --
        // vanilla never has to tear down EndLevelLayer here because vanilla
        // never calls this while it's still showing. If the level was just
        // completed (endscreen up) and something bot-side resets it, the
        // endscreen was staying on screen on top of the level actually
        // playing behind it (Nigel's report). Same one-liner already used in
        // fullReset()'s m_expectsDeath branch below.
        if (auto* ell = getChildByID("EndLevelLayer")) ell->removeFromParent();

        m_practiceMusicSync = true;
        auto& upd = gb->updater;
        upd.m_tpsOverflow  = 0.0;
        upd.m_respawnTimer = gb->hackRespawnInstant ? 0 : 2;
        upd.incrementFrame();
        if (upd.m_logFrameIncrements)
            logFrameIncrement("resetLevel", upd.getFrame());
        uint64_t deathFrame = upd.getFrame();

        checkIfResetWasExpected(deathFrame);
        bool hasAddedCheckpoint = handleResetWithCheckpoints(deathFrame);
        upd.resetFrame();

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

                void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto* gb = GucciEngine::get();
        if (gb->hackHideAttempts && m_attemptLabel)
            m_attemptLabel->setVisible(false);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto* gb  = GucciEngine::get();
        auto& upd = gb->updater;

                                        if (gb->fwAnalyzing) {
            if (obj != m_anticheatSpike) {
                gb->fwProbeDied = true;
                                                                float xp  = player ? player->m_position.x : -1.f;
                float pct = m_levelLength > 0.f ? xp / m_levelLength * 100.f : -1.f;
                log::info("[CAP-DIE] f={} x={:.1f} pct={:.1f}", upd.getFrame(), xp, pct);
            }
            return;
        }

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

        PlayLayer::destroyPlayer(player, obj);

                // Jupiter tab: attempt/PB tracker + death heatmap. Session-only
        // bookkeeping, deliberately placed after both early-returns above so
        // analysis probes and noclip'd "deaths" (which don't actually end the
        // attempt) don't get counted -- only real deaths do. Also gated to the
        // Jupiter level itself and to real (non-bot) play, so dying on an
        // unrelated level or the bot finishing a playback pass doesn't
        // corrupt these Jupiter-only session stats.
        if (obj != m_anticheatSpike && !gb->isPlaying() && gbju::isJupiterLevel(this)) {
            gb->jupiterAttemptCount++;
            float xp = player ? player->m_position.x : -1.f;
            if (m_levelLength > 0.f && xp >= 0.f) {
                float pct = std::clamp(xp / m_levelLength * 100.f, 0.f, 100.f);
                gb->jupiterDeathPcts.push_back(pct);
                if (pct > gb->jupiterSessionBestPct) gb->jupiterSessionBestPct = pct;
            }
            gbju::notifyJupiterAttemptEnded();
        }

        // Trainer tab: same tracker, scoped to whichever macro's loaded there.
        if (obj != m_anticheatSpike && !gb->isPlaying() && gbtr::isTrainerLevel(this)) {
            gb->trainerAttemptCount++;
            float xp = player ? player->m_position.x : -1.f;
            if (m_levelLength > 0.f && xp >= 0.f) {
                float pct = std::clamp(xp / m_levelLength * 100.f, 0.f, 100.f);
                gb->trainerDeathPcts.push_back(pct);
                if (pct > gb->trainerSessionBestPct) gb->trainerSessionBestPct = pct;
            }
            gbtr::notifyTrainerAttemptEnded();
        }

                if (gb->hackNoSpikeFlash && obj != m_anticheatSpike) {
            if (auto* fl = this->getChildByID("flash"))
                fl->setVisible(false);
        }

                                if (gb->hackAutoRetry && obj != m_anticheatSpike && !gb->isPlaying()) {
            gb->pendingAutoRetry = std::clamp(gb->hackAutoRetryDelay, 0.05f, 2.f);
        }

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

    void levelComplete() {
        PlayLayer::levelComplete();
        auto* gb = GucciEngine::get();
        // Same Jupiter/real-play scoping as the destroyPlayer tracker above --
        // otherwise completing ANY level (or the bot finishing ANY macro)
        // stomps the Jupiter tab's session-best to 100%.
        if (!gb->isPlaying() && gbju::isJupiterLevel(this)) {
            gb->jupiterSessionBestPct = 100.f;
            gbju::notifyJupiterAttemptEnded();
        }
        if (!gb->isPlaying() && gbtr::isTrainerLevel(this)) {
            gb->trainerSessionBestPct = 100.f;
            gbtr::notifyTrainerAttemptEnded();
        }
                        if (!gb->autosaveAtLevelEnd) return;
        if (!gb->isRecording() || gb->replay.m_actionAtom.empty()) return;
        auto path = gb->replay.getCurrentPath();
        if (gb->replayBackupsEnabled) gb->replay.backupExisting(path);
        gb->replay.save(path);
    }

    void setupHasCompleted() {
        auto* gb = GucciEngine::get();
                PlayLayer::setupHasCompleted();
    }

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
