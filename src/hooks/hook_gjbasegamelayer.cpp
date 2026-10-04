#include "core/GucciBot.hpp"
#include "analysis/trajectory.hpp"
#include "audio/playsound.hpp"
#include "trainers/calibration.hpp"
#include "hacks/indicator.hpp"
#include "hooks/util_midhook.hpp"
#include "trainers/jupiterghost.hpp"
#include "trainers/trainerghost.hpp"
#include "render/renderer.hpp"
#include <safetyhook.hpp>

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/binding/GJGroundLayer.hpp>

#include "analysis/ac/cbf.hpp"
#include "analysis/ac/framewindow.hpp"
#include "trailbuf/trailbuf.hpp"
#include "replay/scbf_input.hpp"
#include "absense/compat/bot.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/world.hpp"

using namespace geode::prelude;

using namespace gucci;

// Congregation slope diagnostic, see engine_updater.cpp.
namespace gucci::slopediag {
    extern int g_checkCalls;
    extern int g_postCalls;
    void log(std::string const& line);
}

static void shakeRandomOverride(SafetyHookContext& ctx) {
    uint64_t& state = GucciEngine::get()->replay.m_shakeRandomState;
    state = (int)((214013 * state + 2531011) >> 16) & 0x7FFF;
    ctx.rax = (uintptr_t)state;
    // Skip the rand() call this sits on, as Silicate does. Without this the
    // call still ran after the hook: it overwrote rax with GD's own rand()
    // (so shake was never seeded) and advanced GD's global random state as a
    // side effect.
    ctx.rip += 6;
}

// ENGINE_AUDIT §1.4. The teleport trigger's random pick, from the macro's
// seeded teleport state instead of GD's rand(). m_teleportRandomState was
// stored and restored with checkpoints and reseeded every attempt, but the
// hook that reads it was never installed. This one sits after the call (no
// rip skip) -- Silicate's placement.
static void teleportRandomOverride(SafetyHookContext& ctx) {
    uint64_t& state = GucciEngine::get()->replay.m_teleportRandomState;
    state = (int)((214013 * state + 2531011) >> 16) & 0x7FFF;
    ctx.rax = (uintptr_t)state;
}

static void overrideCheckpointPlacement(SafetyHookContext& ctx) {
    ctx.rip += 5;
    auto* pl = PlayLayer::get();
    if (!pl)
        return;
    // anticroom: while Calculate walks you back after a Test, it places your
    // checkpoints itself at the frames they were at. One from the key in the
    // middle of that would throw off its count and land somewhere you never
    // put one.
    if (::Bot::get()->frameWindow().returning())
        return;
    pl->queueCheckpoint();
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

        // Frame Extrapolation moves the players to an in-between point for
        // drawing. loadActualState puts them back before real physics runs --
        // but only if no level reset happened in between. A reset rebuilds the
        // player from scratch; restoring the pre-reset rotation, game state
        // and ground on top of it would drag the old attempt into the new one.
        bool m_undoPending = false;
        uint32_t m_undoResets = 0;

        // Input FPS: presses queued on a tick that is not an input tick wait
        // here for the next one.
        std::vector<PlayerButtonCommand> m_heldInputs;
        uint32_t m_heldAt = 0;
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
        bool const undo = m_fields->m_undoPending &&
                          m_fields->m_undoResets == GucciEngine::get()->updater.m_resetCount;
        m_fields->m_undoPending = false;
        if (!undo)
            return;
        m_gameState = m_fields->m_lastGameState;
        m_fields->m_lastP1.load(m_player1);
        m_fields->m_lastP2.load(m_player2);
        m_fields->m_lastGround.load(m_groundLayer);
        m_fields->m_lastGround2.load(m_groundLayer2);
    }

    bool shouldExtrapolate() const {
        return GucciEngine::get()->updater.estimatedStepCount == 0;
    }

    // Frame Extrapolation, as Silicate does it. On a drawn frame where no
    // physics step is due, predict each player one step ahead and draw them at
    // the in-between point, so motion stays smooth when the frame rate is
    // above the tick rate.
    //
    // Before 2026-09-26 this computed that in-between point and threw it away
    // -- `(void)framePos;` -- so the setting only refreshed the camera. The
    // position half of the feature was never ported.
    //
    // Only the DRAWN position moves (setPosition). The physics position,
    // m_position, is untouched, and loadActualState restores rotation, game
    // state and ground before the next real step.
    void extrapolateVisualUpdates(float dt) {
        auto& upd = GucciEngine::get()->updater;
        m_player1->setRotation(m_fields->m_lastP1.m_rotation);
        m_player2->setRotation(m_fields->m_lastP2.m_rotation);

        auto* pl = PlayLayer::get();
        auto& traj = TrajectoryPredictionService::get();
        cocos2d::CCPoint next1, next2;
        float rot1 = 0.f, rot2 = 0.f;
        // Hold or release to match what the real player is doing, so the
        // prediction follows the same input the next real step will see.
        bool const have1 =
            pl && traj.predictStep(pl, m_player1, m_player1->m_jumpBuffered, next1, rot1);
        bool const have2 =
            pl && traj.predictStep(pl, m_player2, m_player2->m_jumpBuffered, next2, rot2);

        // How far into the next tick this drawn frame is: 0.5 is halfway.
        float framePos = std::clamp(
            static_cast<float>(upd.m_tpsOverflow / upd.getPhysicsDt()), 0.f, 1.f);
        if (!std::isfinite(framePos))
            framePos = 0.f;

        m_fields->m_undoPending = true;
        m_fields->m_undoResets = upd.m_resetCount;

        if (have1) {
            m_player1->setPosition(next1 * framePos + m_player1->m_position * (1.f - framePos));
            m_player1->setRotation(rot1 * framePos +
                                   m_fields->m_lastP1.m_rotation * (1.f - framePos));
        }
        if (have2) {
            m_player2->setPosition(next2 * framePos + m_player2->m_position * (1.f - framePos));
            m_player2->setRotation(rot2 * framePos +
                                   m_fields->m_lastP2.m_rotation * (1.f - framePos));
        }

        // updateCamera only writes game-state variables, so loadActualState
        // puts it back.
        updateCamera(dt * 60.0f);
        updateVisibility(dt);

        if (m_player1->m_isDart && m_player1->m_waveTrail)
            m_player1->m_waveTrail->setPosition(m_player1->m_position);
        if (m_player2->m_isDart && m_player2->m_waveTrail)
            m_player2->m_waveTrail->setPosition(m_player2->m_position);

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

        // Diagnostic: the analyzer's legs are supposed to step exactly as the
        // capture pass did. This prints what the step count actually is per
        // frame while a run is in progress, and whether the post-reset clamp
        // is the thing changing it -- the capture never resets, so it never
        // meets the clamp, while every leg starts with one.
        int const stepsBeforeClamp = upd.estimatedStepCount;

        if (upd.m_respawnTimer > 0) {
            upd.m_respawnTimer--;
            upd.totalStepCount = std::min(upd.totalStepCount, 1);
            upd.estimatedStepCount = std::min(upd.estimatedStepCount, 1);
        }

        if (gb->analyzerOwnsRun() && stepsBeforeClamp != upd.estimatedStepCount)
            gucci::fwEngineLog(fmt::format(
                "[fw][steps] frame={} steps {} -> {} (respawn clamp, {} left)",
                upd.getFrame(), stepsBeforeClamp, upd.estimatedStepCount,
                upd.m_respawnTimer));

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

        // The legacy TTR recording hook lived here. SLRenderer drives its own
        // capture from CCDirector::drawScene, so this only ever ran against a
        // renderer that could not start. Removed with it in 2.0.

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

    }

    void addInputToReplay(PlayerButtonCommand cmd) {
        auto* gb = GucciEngine::get();
        if (cmd.m_isPlayer2 && !m_levelSettings->m_twoPlayerMode)
            cmd.m_isPlayer2 = false;
        auto& atom = gb->replay.m_actionAtom;

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

    // Silicate's trail buffer snapshots the hitbox either side of the
    // collision pass, so a trail records where the player was pushed to as
    // well as where it moved. Real players only -- never a trajectory fork.
    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        bool const real = player && (player == m_player1 || player == m_player2);
        if (real)
            ::Bot::get()->trailBuffer().saveCollision(this, player);

        // Congregation slope diagnostic, see engine_updater.cpp. GD copies
        // m_isOnSlope into m_wasOnSlope at the top of this pass and calls
        // postCollision at the end -- unless the pass returns early.
        bool const p1 = player && player == m_player1 && PlayLayer::get();
        bool const wasOn = p1 && player->m_isOnSlope;
        int const postBefore = gucci::slopediag::g_postCalls;
        if (p1)
            gucci::slopediag::g_checkCalls++;

        int const result = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);

        if (p1 && wasOn && !player->m_isOnSlope) {
            auto* gb = GucciEngine::get();
            gucci::slopediag::log(fmt::format(
                "  {}-CC f={} left slope: ret={} ignoreDamage={} dt={:.4f} postCalled={} "
                "is={} was={} et={:.3f}",
                gb->analyzerOwnsRun() ? "CALC" : "PLAY", gb->updater.getFrame(), result,
                ignoreDamage ? 1 : 0, dt, gucci::slopediag::g_postCalls - postBefore,
                player->m_isOnSlope ? 1 : 0, player->m_wasOnSlope ? 1 : 0,
                player->m_slopeEndTime));
        }

        if (real)
            ::Bot::get()->trailBuffer().saveCollision(this, player);
        return result;
    }

    void saveQueuedButtons() {
        for (auto& cmd : m_queuedButtons)
            addInputToReplay(cmd);
    }

    // anticroom's Input FPS. Holds this tick's presses until the next tick on
    // which a frame would start at the configured rate, then lets them through
    // together. If the frame went BACKWARDS the held presses belonged to an
    // attempt that no longer exists, so they are dropped rather than replayed
    // into the new one.
    void holdUntilFrame() {
        auto& upd = GucciEngine::get()->updater;
        auto& held = m_fields->m_heldInputs;
        uint32_t const frame = upd.getFrame();

        if (frame < m_fields->m_heldAt)
            held.clear();
        m_fields->m_heldAt = frame;

        held.insert(held.end(), m_queuedButtons.begin(), m_queuedButtons.end());
        m_queuedButtons.clear();

        if (!upd.isInputTick(frame))
            return;
        m_queuedButtons.insert(m_queuedButtons.end(), held.begin(), held.end());
        held.clear();
    }

    // anticroom's SCBF. Each live press waits in the recorder until the tick it
    // really arrived in; a press that landed inside that tick is handed to the
    // CBF engine to fire that far into the step, and recorded with the offset.
    // One split point per tick: every press due this tick splits at the first
    // one's offset, as in his.
    void releaseLiveInputs() {
        auto* gb = GucciEngine::get();
        auto& live = scbf::LiveRecorder::get();
        auto& actions = gb->replay.m_actionAtom;
        auto* eng = cbf::Engine::get();
        uint32_t const frame = gb->updater.getFrame();

        live.defer(m_queuedButtons, frame);

        double split = 0.0;
        for (auto const& due : live.takeDue(frame)) {
            if (due.offset <= 0.0) {
                m_queuedButtons.push_back(due.cmd);
                live.noteAligned();
                continue;
            }

            if (split <= 0.0)
                split = due.offset;
            eng->arm(frame, split);
            if (!eng->capture(frame,
                              static_cast<int>(due.cmd.m_button),
                              due.cmd.m_isPush,
                              due.cmd.m_isPlayer2)) {
                m_queuedButtons.push_back(due.cmd);
                live.noteAligned();
                continue;
            }

            size_t const before = actions.length();
            this->addInputToReplay(due.cmd);
            if (actions.length() > before)
                scbf::setOffset(actions.m_actions.back(), split);
            live.notePlaced();
        }
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
        //
        // anticroom's gate: only a tick that is actually being split may take
        // the input -- one this input's own sub-tick offset arms (SCBF), or
        // one the analyzer arms. Outside those, stale engine state can't
        // swallow a normal playback input.
        //
        // Keyed by action.m_frame, as the analyzer arms it. Silicate keys by
        // the updater frame; GucciBot looks inputs up one frame ahead
        // (lookupFrame below), so the two differ by one here -- but arm and
        // capture only have to agree with each other, and the split fires on
        // the next physics step either way.
        {
            bool const flipped2 = gb->replay.playerFlipped(action.m_player2);
            auto* eng = cbf::Engine::get();
            double const offset = scbf::offsetOf(action);
            if (offset > 0.0 && !eng->isArmed(action.m_frame))
                eng->arm(action.m_frame, offset);
            bool const splits = offset > 0.0 || ::Bot::get()->frameWindow().armsTicks();
            if (splits && eng->capture(action.m_frame, button, action.m_holding, flipped2))
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
                // The Survival Indicator's flash and accuracy (hacks/indicator.hpp).
                if (gb->survivalIndicator)
                    indicator::onRealClick(!player1, pressed);
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
        // GucciBot records a press twice over -- here, and from the queue in
        // processQueuedButtons -- and normally the second copy is swallowed
        // because both land on the same frame. Two cases break that, and in
        // both the queue path has to be the only one:
        //   - Input FPS holds queued presses until the next input tick. A copy
        //     recorded here lands on the raw tick instead, so the macro gets
        //     the press twice on two different frames.
        //   - A press fired by CBF mid-step is the replay's own input being
        //     split into the tick, not a new one (anticroom's guard).
        //   - CBF Recording (SCBF) holds each press until the tick it really
        //     arrived in and records it there with its offset. A copy recorded
        //     here would land on the arrival frame, with no offset.
        // Silicate only records from the queue unless its alternate-hook
        // setting is on, which is why it never had the first problem.
        if (!gb->isRecording() || gb->updater.inputFpsActive() ||
            cbf::Engine::get()->m_midStep || scbf::LiveRecorder::get().recording()) {
            return GJBaseGameLayer::handleButton(pressed, button, player1);
        }
        addInputToReplay({.m_button = (PlayerButton)button,
                          .m_isPush = pressed,
                          .m_isPlayer2 = !player1,
                          .m_step = 0});
        GJBaseGameLayer::handleButton(pressed, button, player1);
    }

    // Deterministic object variance, ported from Silicate 2026-09-22.
    //
    // GD gives objects driven by move/rotate/scale/advance-follow triggers a
    // per-object "variance index" that randomises their motion. Left alone it
    // is not reproducible, so a macro replays against objects that are not
    // where they were when it was recorded.
    //
    // GucciBot already carried the whole apparatus for fixing this -- the
    // macro header stores an rngSeed, m_startingSeed and
    // m_startingSeedThisAttempt are maintained on reset, and checkpoints save
    // and restore m_varianceValues -- and then never applied any of it to the
    // objects, because this hook was not ported. The seed was being written to
    // every macro file and read back and used for nothing.
    //
    // Derives each object's index from the seed and the object's own immutable
    // properties, so it is stable across attempts and identical for the same
    // macro every time.
    void processMoveActionsStep(float dt, bool visibleFrame) {
        auto* gb = GucciEngine::get();

        auto const hashObject = [&](GameObject* object, int group, int index) {
            return (int)((gb->replay.m_startingSeed *
                          (int)(object->m_objectID + 5541) *
                          (int)(std::fabs(object->m_startPosition.x) + 1.0) *
                          (int)(std::fabs(object->m_startPosition.y) + 1.0) *
                          ((int)object->m_zLayer + 2137) *
                          (object->m_groupCount + 6969) *
                          ((int)object->m_isObjectBlack * 420 + 14) *
                          ((int)(std::fabs(object->m_startRotationX) * 6.7 + 1.0) *
                           (index + 67)) *
                          (group + 2137)) %
                         1777);
        };

        auto const seedGroup = [&](int group) {
            CCArray* objects = this->getGroup(group);
            if (!objects)
                return;
            for (int i = 0; i < (int)objects->count(); i++) {
                if (auto* object = static_cast<GameObject*>(objects->objectAtIndex(i)))
                    object->m_varianceIndex = hashObject(object, group, i);
            }
        };

        for (auto& a : m_gameState.m_advanceFollowInstances)
            seedGroup(a.m_group);
        for (auto& a : m_gameState.m_rotateEffectInstances)
            seedGroup(a.m_targetID);
        for (auto& a : m_gameState.m_scaleEffectInstances)
            seedGroup(a.m_targetID);
        for (auto& a : m_gameState.m_moveEffectInstances)
            seedGroup(a.m_targetID);

        GJBaseGameLayer::processMoveActionsStep(dt, visibleFrame);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled)
            return GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);

        gb->practiceFix.updatePlatformerInputs(m_queuedButtons);
        // Absense's pathfinder in realtime: this tick's planned input. A no-op
        // unless it is driving the game.
        ::Bot::get()->pathfinder().liveInput();

        // anticroom: an input the CBF engine took but never got to split --
        // the tick it was armed for didn't run the split -- would otherwise
        // never fire at all. Fire it on the tick edge instead.
        if (auto* eng = cbf::Engine::get(); eng->hasPending())
            eng->flushOrphaned();

        if (gb->replay.m_ignoreInputs && gb->isPlaying())
            m_queuedButtons.clear();

        if (gb->isRecording()) {
            if (gb->replay.m_maintainGravity) {
                performMaintainGravity();
                return;
            }
            auto& live = scbf::LiveRecorder::get();
            if (gb->updater.inputFpsActive())
                holdUntilFrame();
            else if (live.splitting())
                releaseLiveInputs();
            else if (live.recording())
                live.passThrough(m_queuedButtons);
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
        // A copy of the player in Absense's look-ahead landing or jumping runs
        // the game's own code, which reports the event to the level -- its
        // event triggers would spawn real groups. The run fires its own event
        // listeners instead (world::onEvent). This slot held `if (false)`
        // until the look-ahead it is about existed here (2026-09-28).
        if (auto& t = ::Bot::get()->trajectory(); t.drawing() || t.simulating()) {
            world::onEvent((int)event, p1, p2);
            return;
        }
        if (event == GJGameEvent::CheckpointRespawn &&
            !GucciEngine::get()->practiceFix.m_shouldLoadPlatformer)
            return;
        GJBaseGameLayer::gameEventTriggered(event, p1, p2);
    }

    void destroyObject(GameObject* obj) {
        auto* gb = GucciEngine::get();
        // A copy in Absense's look-ahead broke the block: it is only hidden
        // from the copies and put back when the run is undone, never
        // destroyed in the real level or filed as a practice break. (Was
        // `if (false)` until that look-ahead existed here.)
        if (auto& t = ::Bot::get()->trajectory(); t.drawing())
            return;
        else if (t.simulating()) {
            t.simBreak(obj);
            return;
        }
        if (m_isPracticeMode)
            gb->practiceFix.registerBrokenObject(obj);
        GJBaseGameLayer::destroyObject(obj);
    }

    void toggleFlipped(bool flipped, bool noEffects) {
        auto* gb = GucciEngine::get();
        // "Only Recording" was saved and loaded but never read, so No Mirror
        // applied during playback and renders too (queued since 1.7.2).
        if (gb->noMirrorEffect && (!gb->noMirrorRecordingOnly || gb->isRecording())) {
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

// ENGINE_AUDIT §1.5: Silicate's GJEffectManager hook. Layout Mode hid
// decoration and colours through the GJBaseGameLayer hooks above, but pulse
// and opacity triggers kept running, so objects still flashed and faded.
class $modify(GB7GJEffectManager, GJEffectManager) {
    void updateEffects(float dt) {
        auto* gb = GucciEngine::get();
        if (gb->enabled && gb->layoutMode && !LevelEditorLayer::get()) {
            m_pulseEffectMap.clear();
            m_pulseEffectVector.clear();
            m_opacityEffectMap.clear();
        }
        GJEffectManager::updateEffects(dt);
    }
};

$execute {
    util_midhook(geode::base::get() + 0x23E173, "shakeRandom1", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1A1, "shakeRandom2", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1CB, "shakeRandom3", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x23E1E9, "shakeRandom4", shakeRandomOverride);
    util_midhook(geode::base::get() + 0x20FEDC, "teleportRandomOverride", teleportRandomOverride);
    util_midhook(geode::base::get() + 0x3A3657, "checkpointPlacement", overrideCheckpointPlacement);
}
