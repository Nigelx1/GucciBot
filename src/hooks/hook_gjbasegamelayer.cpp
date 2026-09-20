#include "core/GucciBot.hpp"
#include "analysis/trajectory.hpp"
#include "audio/playsound.hpp"
#include "trainers/calibration.hpp"
#include "hooks/util_midhook.hpp"
#include "trainers/jupiterghost.hpp"
#include "trainers/trainerghost.hpp"
#include "render/renderer.hpp"
#include <safetyhook.hpp>

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGroundLayer.hpp>

#include "analysis/ac/cbf.hpp"
#include "analysis/ac/framewindow.hpp"

using namespace geode::prelude;

using namespace gucci;

static void shakeRandomOverride(SafetyHookContext& ctx) {
    uint64_t& state = GucciEngine::get()->replay.m_shakeRandomState;
    state = (int)((214013 * state + 2531011) >> 16) & 0x7FFF;
    ctx.rax = (uintptr_t)state;
}

static void overrideCheckpointPlacement(SafetyHookContext& ctx) {
    ctx.rip += 5;
    PlayLayer::get()->queueCheckpoint();
}

class $modify(GB7GJBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        struct PlayerState {
            float m_rotation = 0.f;
            void save(PlayerObject* p) {
                if (p)
                    m_rotation = p->getRotation();
            }
            void load(PlayerObject* p) {
                if (p)
                    p->setRotation(m_rotation);
            }
        };
        struct GroundState {
            cocos2d::CCPoint p1, p2;
            void save(GJGroundLayer* g) {
                if (!g || !g->m_ground1Sprite)
                    return;
                p1 = g->m_ground1Sprite->getPosition();
                if (g->m_ground2Sprite)
                    p2 = g->m_ground2Sprite->getPosition();
            }
            void load(GJGroundLayer* g) {
                if (!g || !g->m_ground1Sprite)
                    return;
                g->m_ground1Sprite->setPosition(p1);
                if (g->m_ground2Sprite)
                    g->m_ground2Sprite->setPosition(p2);
            }
        };
        GJGameState m_lastGameState;
        PlayerState m_lastP1, m_lastP2;
        GroundState m_lastGround, m_lastGround2;
    };

    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("GJBaseGameLayer::handleButton", Priority::Last);
    }

    void storeActualState() {
        m_fields->m_lastP1.save(m_player1);
        m_fields->m_lastP2.save(m_player2);
        m_fields->m_lastGameState = m_gameState;
        m_fields->m_lastGround.save(m_groundLayer);
        m_fields->m_lastGround2.save(m_groundLayer2);
    }

    void loadActualState() {
        m_gameState = m_fields->m_lastGameState;
        m_fields->m_lastP1.load(m_player1);
        m_fields->m_lastP2.load(m_player2);
        m_fields->m_lastGround.load(m_groundLayer);
        m_fields->m_lastGround2.load(m_groundLayer2);
    }

    bool shouldExtrapolate() const {
        return GucciEngine::get()->updater.estimatedStepCount == 0;
    }

    void extrapolateVisualUpdates(float dt) {
        auto& upd = GucciEngine::get()->updater;
        m_player1->setRotation(m_fields->m_lastP1.m_rotation);
        m_player2->setRotation(m_fields->m_lastP2.m_rotation);
        float framePos = (float)(upd.m_tpsOverflow / upd.getPhysicsDt());
        (void)framePos;
        updateCamera(dt * 60.0f);
        updateVisibility(dt);
        CCLayer::update(dt);
    }

    void update(float dt) {
        auto* gb = GucciEngine::get();
        auto& upd = gb->updater;

        if (upd.m_onlyRefresh || !gb->enabled) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // Lock delta only drives stepping inside a real level, where
        // runSlowLockDelta hands this update exactly one step at a time.
        // Anywhere else -- the editor -- the step count comes from real time,
        // right here.
        bool lockDeltaActive = upd.isLockDelta() && PlayLayer::get();
        if (!lockDeltaActive)
            upd.calculateSteps(dt, (float)upd.getPhysicsDt());

        if (upd.m_respawnTimer > 0) {
            upd.m_respawnTimer--;
            upd.totalStepCount = std::min(upd.totalStepCount, 1);
            upd.estimatedStepCount = std::min(upd.estimatedStepCount, 1);
        }

        if (upd.m_extrapolateFrames && upd.getFrame() > upd.m_frameOnLastAttempt) {
            if (shouldExtrapolate()) {
                extrapolateVisualUpdates(dt);
            } else {
                loadActualState();
                GJBaseGameLayer::update(dt);
                storeActualState();
            }
        } else {
            // This used to test the raw lock-delta setting, which is true in
            // the editor too. So in an editor playtest GD's update ran on every
            // drawn frame, including frames where real time said zero steps
            // were due -- and the step-count midhook still produces one step
            // for those. At 360 FPS that is 360 steps a second instead of 240:
            // exactly the 1.5x Nigel measured. Only real-level lock delta may
            // force the call now. The editor's song playback preview runs on
            // GD's own timing, so it keeps the old every-frame behaviour.
            auto* lel = LevelEditorLayer::get();
            bool editorPlayback = lel && lel->m_playbackActive;
            if (lockDeltaActive || upd.estimatedStepCount != 0 || editorPlayback) {
                GJBaseGameLayer::update(dt);
                storeActualState();
            }
        }

        if (gb->renderer.recording) {
            if (auto* rpl = PlayLayer::get(); rpl && rpl->m_player1)
                gb->renderer.handleRecording(rpl, (int)upd.getFrame());
        }

        // Ghosts, ranges and debug overlays are decoration. During a run they
        // are drawn hundreds of times a second over a level that is being
        // restarted constantly, and none of it is being looked at. Analyzer
        // gets the frame budget.
        if (auto* fpl = PlayLayer::get(); fpl && !gb->analyzerOwnsRun()) {
            gbpf::renderAgencyDebug(fpl);
            gbpr::renderPracticeRange(fpl);
            gbju::renderJupiterGhost(fpl);
            gbtr::renderTrainerGhost(fpl);
        }

        // Auto-retry calls resetLevel(). Doing that in the middle of a leg
        // would throw away the run the analyzer is measuring.
        if (gb->pendingAutoRetry > 0.0f && !gb->analyzerOwnsRun()) {
            gb->pendingAutoRetry -= dt;
            if (gb->pendingAutoRetry <= 0.0f) {
                gb->pendingAutoRetry = 0.0f;
                if (gb->hackAutoRetry && !gb->isPlaying()) {
                    if (auto* rpl = PlayLayer::get())
                        rpl->resetLevel();
                }
            }
        }
    }

    void addInputToReplay(PlayerButtonCommand cmd) {
        auto* gb = GucciEngine::get();
        if (cmd.m_isPlayer2 && !m_levelSettings->m_twoPlayerMode)
            cmd.m_isPlayer2 = false;
        auto& atom = gb->replay.m_actionAtom;

        bool finalPlayer2 = gb->replay.playerFlipped(cmd.m_isPlayer2);
        int suppressIdx = finalPlayer2 ? 1 : 0;
        if (gb->replay.m_suppressNextRelease[suppressIdx]) {
            gb->replay.m_suppressNextRelease[suppressIdx] = false;
            if (!cmd.m_isPush) {
                log::info("[GucciBot] Recording: suppressed orphan release for player{} "
                          "(Juice's died-mid-click cleanup)",
                          suppressIdx + 1);
                return;
            }
        }

        uint32_t f = gb->updater.getFrame() + 1;
        if (atom.length() > 0 && atom.m_actions.back().m_frame > f)
            return;
        bool added = atom.addAction(f,
                                    static_cast<gb::ActionType>(cmd.m_button),
                                    cmd.m_isPush,
                                    gb->replay.playerFlipped(cmd.m_isPlayer2));
        if (gb->isRecording() && added)
            log::info("[GucciBot] Recording: input @ frame {} ({}, btn {}, p{})",
                      f,
                      cmd.m_isPush ? "press" : "release",
                      (int)cmd.m_button,
                      cmd.m_isPlayer2 ? 2 : 1);
    }

    void saveQueuedButtons() {
        for (auto& cmd : m_queuedButtons)
            addInputToReplay(cmd);
    }

    void processReplayAction(gb::Action& action) {
        auto* gb = GucciEngine::get();
        auto& upd = gb->updater;

        if (action.m_type == gb::ActionType::Death) {
            upd.m_expectsDeath = true;
            gb->replay.m_startingSeedThisAttempt =
                *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90);
            return;
        }
        if (action.m_type == gb::ActionType::RestartFull) {
            upd.m_expectsDeath = true;
            ((PlayLayer*)this)->fullReset();
            return;
        }
        if (action.m_type == gb::ActionType::Restart) {
            upd.m_expectsDeath = true;
            ((PlayLayer*)this)->resetLevel();
            return;
        }
        if (action.m_type == gb::ActionType::TPS) {
            upd.setTps(action.m_tps);
            upd.estimatedStepCount = 0;
            upd.totalStepCount = 0;
            return;
        }

        int button = (int)action.m_type;
        if (button < 1 || button > 3)
            return;

        // Tell anticroom's analyzer the moment a press is actually applied,
        // and in what state the player was when it landed -- notably whether
        // the input could be split within the tick or had to be buffered,
        // which is what his cube CBF investigation hangs on. Inert unless his
        // analyzer is running, which GucciBot's own Calculate never makes it.
        // Placed exactly where Silicate places it: after the button filter,
        // before the input is handed onward.
        if (auto& acfw = ::Bot::get()->frameWindow(); acfw.running()) {
            bool const flipped = gb->replay.playerFlipped(action.m_player2);
            auto* actor = flipped ? m_player2 : m_player1;
            acfw.notePress(action.m_frame,
                           action.m_player2,
                           cbf::canSplit(actor,
                                         SLSettings::get()->frameWindow.cbfTickGround),
                           actor && actor->m_isOnGround,
                           actor ? actor->m_yVelocity : 0.0);
        }

        // Hand the input to the CBF engine. If it is armed for this frame, it
        // holds the input back and fires it between physics sub-steps instead
        // of at the tick boundary -- which is the whole point of a sub-tick
        // window. Returning true means "taken, do not queue it normally".
        {
            bool const flipped2 = gb->replay.playerFlipped(action.m_player2);
            if (cbf::Engine::get()->capture(
                    action.m_frame, button, action.m_holding, flipped2))
                return;
        }

        if (gb->fwSampling && action.m_holding) {
            auto* sp = action.m_player2 ? m_player2 : m_player1;
            if (sp)
                gb->fwClickSamples.push_back({action.m_frame,
                                              sp->m_position.x,
                                              sp->m_position.y,
                                              action.m_player2,
                                              false,
                                              false,
                                              false,
                                              action.m_type});
        }

        // GucciBot's tier sounds were driven from here off its own fwMarks.
        // anticroom's analyzer plays its tier sounds itself, from its render
        // path, so there is nothing to drive from the action dispatch now.

        queueButton(button, action.m_holding, gb->replay.playerFlipped(action.m_player2), 0.0);
    }

    void performMaintainGravity() {
        auto* gb = GucciEngine::get();
        m_queuedButtons.clear();

        bool p1J = m_uiLayer->m_p1Jumping || m_uiLayer->m_p1TouchId != -1;
        bool p2J = m_uiLayer->m_p2Jumping || m_uiLayer->m_p2TouchId != -1;
        if (gb->replay.hasFlippedControls())
            std::swap(p1J, p2J);
        if (gb->replay.m_mirrorInputs) {
            p1J |= p2J ^ gb->replay.m_mirrorInverted;
            p2J |= p1J ^ gb->replay.m_mirrorInverted;
        }

        auto* p1 = m_player1;
        if (PlayLayer::get()->m_levelEndAnimationStarted)
            return;

        while (p1->m_isUpsideDown == (p1J == p1->m_jumpBuffered) && !p1->m_controlsDisabled) {
            addInputToReplay({.m_button = (PlayerButton)1,
                              .m_isPush = (bool)(p1J ^ p1->m_isUpsideDown),
                              .m_isPlayer2 = gb->replay.playerFlipped(false),
                              .m_step = 0});
            handleButton(p1J ^ p1->m_isUpsideDown, 1, gb->replay.playerFlipped(true));
        }
    }

    void requeueInverted() {
        auto* gb = GucciEngine::get();
        for (auto& cmd : m_queuedButtons) {
            int key =
                (int)cmd.m_button + ((cmd.m_isPlayer2 && m_levelSettings->m_twoPlayerMode) ? 4 : 0);
            gb::Action a;
            a.m_frame = gb->replay.m_forceNextInput ? std::numeric_limits<uint32_t>::max()
                                                    : gb->updater.getFrame();
            a.m_type = static_cast<gb::ActionType>(cmd.m_button);
            a.m_holding = cmd.m_isPush;
            a.m_player2 = cmd.m_isPlayer2 && m_levelSettings->m_twoPlayerMode;
            gb->replay.m_lastInputs.insert_or_assign(key, a);

            if (!gb->replay.m_mirrorInputs)
                continue;
            bool inv = gb->replay.m_mirrorInverted;
            queueButton((int)cmd.m_button,
                        cmd.m_isPush ^ inv,
                        gb->replay.playerFlipped(!cmd.m_isPlayer2),
                        0.0);
        }
    }

    void handleButton(bool pressed, int button, bool player1) {
        auto* gb = GucciEngine::get();
        // GucciBot's own button events (editor playtest start) are not the
        // player's: no click sound, no scoring, and never recorded.
        if (gb->suppressInputCapture)
            return GJBaseGameLayer::handleButton(pressed, button, player1);
        if (button == 1) {
            triggerClickAudio(!player1, button, pressed);
            if (!gb->isPlaying()) {
                if (gb->survivalIndicator) {
                    TrajectoryPredictionService::get().onRealClick(!player1, pressed);
                }
                if (pressed) {
                    CalibrationService::get().onRealClick();
                }
                // Real-time click scoring for the Jupiter/Trainer Click
                // Trainer pages -- timed off the actual input event, not
                // polled from GUI draw code, so the number shown is the real
                // error and not an estimate of it (matches the frame the
                // click genuinely arrived in).
                double tps = gb->updater.m_tps > 0.0 ? gb->updater.m_tps : 240.0;
                double clickTimeSec = (double)gb->updater.getFrame() / tps;
                if (gb->jupiterClickBarPageVisible && !gb->jupiterMacro.clickIntervalsSec.empty()) {
                    gb->scoreRealClick(
                        gb->jupiterMacro.clickIntervalsSec, gb->jupiterClickScore, clickTimeSec, pressed, tps);
                }
                if (gb->trainerClickBarPageVisible && !gb->trainerMacro.clickIntervalsSec.empty()) {
                    gb->scoreRealClick(
                        gb->trainerMacro.clickIntervalsSec, gb->trainerClickScore, clickTimeSec, pressed, tps);
                }
            }
        }
        if (!gb->isRecording()) {
            return GJBaseGameLayer::handleButton(pressed, button, player1);
        }
        addInputToReplay({.m_button = (PlayerButton)button,
                          .m_isPush = pressed,
                          .m_isPlayer2 = !player1,
                          .m_step = 0});
        GJBaseGameLayer::handleButton(pressed, button, player1);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled)
            return GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        gb->practiceFix.updatePlatformerInputs(nullptr);

        if (gb->replay.m_ignoreInputs && gb->isPlaying())
            m_queuedButtons.clear();

        if (gb->isRecording()) {
            if (gb->replay.m_maintainGravity) {
                performMaintainGravity();
                return;
            }
            requeueInverted();
            saveQueuedButtons();
        } else if (gb->isPlaying()) {
            uint32_t frame = gb->updater.getFrame();
            // The unshifted branch exists for GucciBot's own analyzer (Juice's
            // design, commit 77b0dad): store the true frame everywhere and
            // compensate only where normal playback applies a queued input.
            //
            // anticroom's analyzer is not that analyzer. It replays GucciBot
            // macros through the ordinary update path, so it needs the same
            // compensation ordinary playback needs. Without it the capture
            // pass -- a plain replay from frame 0, no restores involved --
            // died at frame 193 on a macro that plays fine normally, while
            // with it the same pass reached 3943. Capture and legs were both
            // unshifted, so they agreed with each other and desyncs looked
            // low, but both were running a macro that was not the real one.
            // That is the "counts everything, numbers aren't right" symptom.
            //
            // Pathfinder keeps the unshifted branch: it is GucciBot-native and
            // was built against it.
            bool const acRun = gb->analyzerOwnsRun();
            uint32_t lookupFrame = (gb->fwAnalyzing && !acRun) ? frame : frame + 1;
            while (auto input = gb->replay.getNextInput(lookupFrame)) {
                // Nigel's real test (2026-09-06): a Pathfinder result "calculated
                // correctly" but every one of its inputs failed to fire on normal
                // playback (fwAnalyzing == false) -- died at the exact same frame
                // as with no macro at all. [CAP-IN] only ever covered the
                // fwAnalyzing branch, so normal playback had zero visibility into
                // whether getNextInput was even matching anything. Logging both
                // branches (tagged separately) until this is actually diagnosed
                // with real data instead of another guess.
                log::info("[{}] f={} lookupFrame={} inputFrame={} idx={}/{} hold={} p2={}",
                          gb->fwAnalyzing ? "CAP-IN" : "PLAY-IN",
                          frame,
                          lookupFrame,
                          input->m_frame,
                          gb->replay.m_inputIndex,
                          gb->replay.m_actionAtom.m_actions.size(),
                          input->m_holding ? 1 : 0,
                          input->m_player2 ? 1 : 0);
                processReplayAction(input.value());
            }
        }

        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    void updateCamera(float dt) {
        auto& upd = GucciEngine::get()->updater;
        upd.m_lastCameraPos = upd.m_currentCameraPos;
        GJBaseGameLayer::updateCamera(dt);

        // Holds the camera on the click being measured instead of letting it
        // fly around as the analyzer restarts the level hundreds of times.
        // This is what the "Lock Camera" setting does -- without it that
        // setting saved, loaded and changed nothing at all.
        if (auto& acfw = ::Bot::get()->frameWindow();
            acfw.cameraLocked() && m_objectLayer) {
            auto const win = cocos2d::CCDirector::sharedDirector()->getWinSize();
            auto const target = acfw.cameraPoint();
            float const sc = m_objectLayer->getScale();
            m_objectLayer->setPosition(
                {win.width / 2.f - target.x * sc, m_objectLayer->getPositionY()});
        }

        upd.m_currentCameraPos = m_objectLayer->getPosition();
    }

    double getModifiedDelta(float dt) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled)
            return GJBaseGameLayer::getModifiedDelta(dt);

        float modDelta = GJBaseGameLayer::getModifiedDelta(dt);

        if (auto* lel = LevelEditorLayer::get(); lel && lel->m_playbackActive)
            return modDelta;

        auto& upd = gb->updater;
        if (upd.m_onlyRefresh)
            return 0.0f;

        float tw = m_gameState.m_timeWarp;
        if (tw <= 0.0f)
            tw = 1.0f;
        return (float)upd.getPhysicsDt() * std::fmin(tw, 1.0f);
    }

    void gameEventTriggered(GJGameEvent event, int p1, int p2) {
        if (false)
            return;
        if (event == GJGameEvent::CheckpointRespawn &&
            !GucciEngine::get()->practiceFix.m_shouldLoadPlatformer)
            return;
        GJBaseGameLayer::gameEventTriggered(event, p1, p2);
    }

    void destroyObject(GameObject* obj) {
        auto* gb = GucciEngine::get();
        if (false)
            return;
        if (m_isPracticeMode)
            gb->practiceFix.registerBrokenObject(obj);
        GJBaseGameLayer::destroyObject(obj);
    }

    void toggleFlipped(bool flipped, bool noEffects) {
        if (GucciEngine::get()->noMirrorEffect) {
            m_gameState.m_unkBool10 = flipped;
            return;
        }
        GJBaseGameLayer::toggleFlipped(flipped, noEffects);
    }

    void createBackground(int bg) {
        auto* gb = GucciEngine::get();
        if (gb->enabled && !LevelEditorLayer::get() && gb->layoutMode)
            bg = 13;
        GJBaseGameLayer::createBackground(bg);
    }
    void createMiddleground(int mg) {
        auto* gb = GucciEngine::get();
        if (gb->enabled && !LevelEditorLayer::get() && gb->layoutMode) {
            if (m_middleground) {
                m_middleground->removeFromParent();
                m_middleground = nullptr;
            }
            return;
        }
        GJBaseGameLayer::createMiddleground(mg);
    }
    void createGroundLayer(int g, int lt) {
        auto* gb = GucciEngine::get();
        if (gb->enabled && !LevelEditorLayer::get() && gb->layoutMode) {
            g = 18;
            lt = 2;
        }
        GJBaseGameLayer::createGroundLayer(g, lt);
    }
    void updateColor(cocos2d::ccColor3B& col,
                     float ft,
                     int cid,
                     bool bl,
                     float op,
                     cocos2d::ccHSVValue& hsv,
                     int cid2,
                     bool co,
                     EffectGameObject* eo,
                     int u1,
                     int u2) {
        if (!LevelEditorLayer::get() && GucciEngine::get()->enabled &&
            GucciEngine::get()->layoutMode)
            return;
        GJBaseGameLayer::updateColor(col, ft, cid, bl, op, hsv, cid2, co, eo, u1, u2);
    }
    void triggerGradientCommand(GradientTriggerObject* obj) {
        if (!LevelEditorLayer::get() && GucciEngine::get()->enabled &&
            GucciEngine::get()->layoutMode)
            return;
        GJBaseGameLayer::triggerGradientCommand(obj);
    }
};

$execute {
    util_midhook(geode::base::get() + 0x23E173, "shakeRandom1", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1A1, "shakeRandom2", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1CB, "shakeRandom3", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1E9, "shakeRandom4", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x3A3657, "checkpointPlacement", overrideCheckpointPlacement);
}
