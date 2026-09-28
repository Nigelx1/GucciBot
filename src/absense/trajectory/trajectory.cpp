#include "trajectory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/compat/devlog.hpp"
#include "absense/compat/bot.hpp"
#include "ccTypes.h"
#include "absense/compat/bot.hpp"
#include "absense/physics/collisions.hpp"
#include "absense/physics/gravity.hpp"
#include "absense/physics/object.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/settings.hpp"
#include "absense/world/def.hpp"
#include "absense/world/ledger.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/materialize.hpp"

#ifdef ABSENSE_PROTECT
// #include "VMProtect/VMProtectSDK.h"  (not used in GucciBot)
#endif

using namespace geode::prelude;

// constexpr std::array<int, 15> INTERACTABLE_IDS = {747,  748,  35,   67,   84,
//                                                   140,  141,  1022, 1332,
//                                                   1333, 1330, 1704, 1751,
//                                                   3005, 3004};

cocos2d::ccColor4F toCocosColor(float colors[4]) {
    return cocos2d::ccColor4F{colors[0], colors[1], colors[2], colors[3]};
}

cocos2d::ccColor4F toCocosColorNegative(float colors[4]) {
    return cocos2d::ccColor4F{1.0f - colors[0], 1.0f - colors[1],
                              1.0f - colors[2], colors[3]};
}

static cocos2d::CCRect usingWidth(const cocos2d::CCRect& old,
                                  const float width) {
    cocos2d::CCRect rect = old;
    rect.origin.x += width;
    rect.origin.y += width;
    rect.size.width -= width * 2.0;
    rect.size.height -= width * 2.0;
    return rect;
}

int Trajectory::getPredictionLength() {
    auto bot = Bot::get();
    auto pl = GJBaseGameLayer::get();
    if (!pl) return 0;

    return m_state->m_length->inner() *
           std::max(bot->updater().getTps(), 240.0) /
           pl->m_gameState.m_timeWarp;
}

bool Trajectory::iterate(GJBaseGameLayer* pl, PlayerObject* player, int mode,
                         float* colors, bool&, int& stepCount,
                         PredictionConfig config) {
    cocos2d::CCPoint position = player->getPosition();

    auto& updater = Bot::get()->updater();
    // The run's own time warp with the World on (a time warp trigger 1935 it
    // fired moved it), the live game's without it; one game step is
    // getModifiedDelta long: physicsDt * min(timeWarp, 1).
    const float timeWarp = world::stepTimeWarp();
    const float physicsDt = world::stepDt();

    // The layer's clock and a parked speed belong to the tick, not to a copy:
    // the game runs them once per step for both players (world/step.cpp (g),
    // update 0x238016-0x238064). Run here they ran once per copy - twice a tick
    // in the dual part, and in the drawn prediction the second copy took a
    // speed the first one had just parked a tick early (Sim::step holds a
    // parked speed back between its copies). A run with the World (Sim::step,
    // the drawn prediction) runs them once per tick (world::advanceClock and
    // world::applyParkedSpeed); this is the path a run without it keeps. The
    // run is asked (world::current), not World::disabled, so a level check
    // turning the World off between two slices of a paused search leaves the
    // search on the clock it began with.
    const bool clockPerCopy = world::current() == nullptr;
    if (clockPerCopy) {
        pl->m_gameState.m_totalTime += physicsDt;
        pl->m_gameState.m_unkDouble3 += physicsDt / timeWarp;
        pl->m_gameState.m_currentProgress++;
        // pl->m_gameState.m_unkUint2++;
        pl->m_gameState.m_unkUint5 += (int)roundf(timeWarp * 1000.0f);
    }

    player->m_totalTime += physicsDt;

    if (clockPerCopy) {
        float playerSpeed = *(float*)(&pl->m_gameState.m_timeModRelated);
        if (playerSpeed != 0.0) {
            pl->m_gameState.m_timeModRelated = 0;
            pl->m_gameState.m_timeModRelated2 = 0;

            player->updateTimeMod(playerSpeed, true);
        }
    }

    // GJBaseGameLayer::update calls this right after the tick's input
    // (0x14023809e, false): it drops only the orbs the previous collision pass
    // did not touch and then forgets those touches. So a press still fires an
    // orb touched on either of the last two passes - anywhere in a contact,
    // and on the tick after leaving it - while an orb left behind drops out a
    // tick later. Emptying the set every tick instead kept a normal orb
    // pressable only on the first tick or two of its contact, which is why
    // the human's mid-contact press never fired in the copies. Sim::step
    // presses before this, as processCommands does. SavedPlayerCheckpoint
    // carries both sets (checkpoint/checkpoint.cpp:202-207, 472-477), so the
    // same script still gives the same answer.
    if (player->m_touchingRings) player->resetTouchedRings(false);

    // The game's own per-tick reset (inlined in GJBaseGameLayer::update): empties
    // the logs AND moves last tick's bottom/top IDs into m_unk50C/m_unk510 and
    // resets m_lastCollision* to -1 (storeCollision and the landing test read them).
    player->resetCollisionLog(false);

    if (((m_deadP1 && (mode & TrajectoryMode::Player1) != 0) ||
         (m_deadP2 && (mode & TrajectoryMode::Player2) != 0))) {
        if (stepCount > 1 && !config.m_silent) {
            emitHitbox(player);
        }
        return true;
    }

    // pl->m_effectManager->prepareMoveActions(m_delta / 60.0f, false);
    // pl->processMoveActionsStep(m_delta / 60.0f, true);
    // pl->m_effectManager->postMoveActions();

    if (!m_actions.empty()) {
        for (auto& action : m_actions) {
            if (action.m_delay == 0) {
                // geode::log::info("executing action at pseudo-frame {}",
                // pl->m_gameState.m_currentProgress - 1);
                action.m_func();
                action.m_executed = true;
                continue;
            }

            action.m_delay--;
        }

        std::erase_if(m_actions, [](auto& a) { return a.m_executed; });
    }

    // The step the copy's own update and collision pass take. A run with the
    // World works it out from the run's own time warp, exactly as Sim::begin
    // works m_delta out from the warp the run started under: a 1935 the run
    // fired changed the warp for the clock and the commands above, and the
    // copy has to move at the same rate or the level would run slower around a
    // player who does not. Until one fires this is m_delta to the bit. Every
    // other run keeps m_delta, the rate it began at.
    const float worldDelta = (float)(updater.getPhysicsDt() * std::min(timeWarp, 1.0f) * 60.0f);
    float delta = config.m_overridenTPS == 0.0
                      ? (world::current() ? worldDelta : m_delta)
                      : (1.0 / config.m_overridenTPS) * 60.0;
    // float delta = m_delta;

    player->m_playEffects = false;
    player->update(delta);

    if (pl->checkCollisions(player, delta, false) == 1) {
        this->hasDied(player);
    }
    if (!m_phantoms.empty() && hitsPhantom(player)) {
        this->hasDied(player);
    }

    // The game walks the spawn lists once per step, for player 1, after both
    // players' collision passes (world/fire.cpp (w)): a run whose World walks
    // them does it then (Sim::step, the drawn prediction), and this per-copy
    // walk is what a run without it keeps.
    if (world::Run* worldRun = world::currentRun(); worldRun && worldRun->walks) {
        if (player == worldRun->walkAfter && !worldRun->walked) {
            worldRun->walked = true;
            world::checkSpawnObjects(*worldRun);
        }
    } else {
        phys::checkSpawnObjects(pl, player);
    }

    // GJBaseGameLayer::update: rotation after the collision pass and the spawn
    // check (the collision pass tests rotated hazards with last tick's rotation;
    // m_shipRotation is the post-collision position updateShipRotation reads).
    player->m_unkUnused3 = player->getRotation();
    player->updateRotation(delta);
    player->m_shipRotation = player->getPosition();

    if (!config.m_silent) {
        tickSegment(player, position, player->getPosition(), colors);
    }

    // for (int i = 0; i < pl->m_objects->count(); i++) {
    //     GameObject* object = (GameObject*)pl->m_objects->objectAtIndex(i);
    //     auto fillCol = toCocosColor(colors);
    //     fillCol.a = 0.2f;
    //     m_node->drawRect(object->getObjectRect(), fillCol, 0.5f,
    //     toCocosColor(colors));
    // }

    stepCount++;

    return false;  // continue iteration
}

static void pressJump(PlayerObject* p);
static void releaseJump(PlayerObject* p);
static void displayMovingStep(float dt);

// Whether the drawn lines take their click kind as their input: the player's
// own play. A playing replay drives every kind with its inputs instead, and so
// does the autoclicker while it is on. runPrediction and drawLines both ask
// this, so a line drawn once for every kind (drawLines) is always one whose
// kind makes no difference to it.
static bool manualPredictionInput() {
    return !Bot::get()->isPlaying() && !Bot::get()->autoclicker().m_enabled->inner();
}

TrajectoryPlayerData Trajectory::runPrediction(GJBaseGameLayer* pl,
                                               PlayerObject* player,
                                               PlayerObject* other, int mode,
                                               float* colors, bool both,
                                               PredictionConfig config) {
    auto bot = Bot::get();
    bool hasPerformedClick = false;
    bool hasPerformedClickOther = false;

    const int iterations = m_state->m_length->inner() *
                           std::max(bot->updater().getTps(), 240.0) /
                           pl->m_gameState.m_timeWarp;

    int playerMask = (player == m_fakePlayer1) ? TrajectoryMode::Player1
                                               : TrajectoryMode::Player2;

    std::array<PlayerObject*, 2> players = {player, other};
    int count = (both && pl->m_gameState.m_isDualMode) ? 2 : 1;

    auto& ac = Bot::get()->autoclicker();
    bool acEnabled = ac.m_enabled->inner();
    bool isP2 = player == m_fakePlayer2;
    AbsAutoclicker::PlayerSettings state = isP2 ? ac.m_player2 : ac.m_player1;

    // The autoclicker only acts while recording; otherwise the path shows
    // what the mode's input would do - unless a replay is playing, in which
    // case its inputs drive the simulation instead.
    const bool acDriven = !Bot::get()->isPlaying() && acEnabled;
    const bool manualInput = manualPredictionInput();  // !isPlaying() && !acDriven
    // Tap and DoubleHeld end their first tick with the button still down and
    // let go at the top of the second, the way Sim::step's apply lets go on a
    // tick whose input is up after a held one. Only the kind's own input does
    // this: a replay or the autoclicker drives the copies otherwise.
    const int clickKind = mode & Trajectory::CLICK_MASK;
    const bool releaseNextTick =
        manualInput && (clickKind == TrajectoryMode::Tap || clickKind == TrajectoryMode::DoubleHeld);

    // A rebuild of update() draws a run of per-tick segments as one segment
    // (SegmentRun) - only in an opaque colour: a translucent line is darker
    // where its segments overlap, and every tick's segment overlaps the last
    // ones, so fewer of them would draw it lighter. A recorded line is drawn in
    // the colour update() says (drawLines). Every other caller of simulate()
    // draws one segment a tick, as before.
    m_runs[0].active = false;
    m_runs[1].active = false;
    m_runColors = colors;
    m_runWidth = m_state->m_width->inner();
    m_mergeRuns = m_drawing && m_mergeEps > 0.0f && (m_recordOps ? m_recordMerge : colors[3] == 1.0f);

    for (int i = 0; i < count; i++) {
        PlayerObject* plr = players[i];
        CCPoint position = plr->getPosition();
        if (acDriven) {
            if (state.isClicking()) {
                plr->pushButton(PlayerButton::Jump);
            } else {
                plr->releaseButton(PlayerButton::Jump);
            }
        }

        if (manualInput) {
            // Whether this copy's jump is down already: simulate() gave it the
            // real player's buttons (syncButtons), and this is the same test
            // AbsensePathfinder::playerHeld and the MCP tools start a Sim from. The
            // clicks below follow Sim::step's apply(): a click from a button
            // that is down lets go first, and keeping it down is no input.
            const auto heldIt = plr->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            const bool down = heldIt != plr->m_holdingButtons.end() && heldIt->second;
            switch (mode & Trajectory::CLICK_MASK) {
                case TrajectoryMode::Hold: {
                    // The game presses once per press (GJBaseGameLayer::
                    // handleButton 0x2338e0), never again for a button that
                    // stays down. A second pushButton here re-armed the orb
                    // buffer (0x397fbc) and fired an orb the copy was touching
                    // (0x398067), one a held button never takes.
                    if (!down) plr->pushButton(PlayerButton::Jump);
                    break;
                }
                case TrajectoryMode::Tap: {
                    // Pressed now, let go on the next tick (see the loop).
                    if (down) releaseJump(plr);
                    pressJump(plr);
                    break;
                }
                case TrajectoryMode::Swift: {
                    if (down) releaseJump(plr);
                    pressJump(plr);
                    releaseJump(plr);
                    break;
                }
                case TrajectoryMode::Double: {
                    // Two clicks inside the same tick, both let go in it.
                    if (down) releaseJump(plr);
                    pressJump(plr);
                    releaseJump(plr);
                    pressJump(plr);
                    releaseJump(plr);
                    break;
                }
                case TrajectoryMode::DoubleHeld: {
                    // Two clicks inside the same tick, the second kept down and
                    // let go on the next tick (see the loop). Each press is a
                    // pushButton of its own, so each re-arms the orb buffer as
                    // each queued press does in the game, and the tick's
                    // physics run with the button down.
                    if (down) releaseJump(plr);
                    pressJump(plr);
                    releaseJump(plr);
                    pressJump(plr);
                    break;
                }
                case TrajectoryMode::Release: {
                    plr->releaseButton(PlayerButton::Jump);
                    plr->m_jumpBuffered = false;
                    break;
                }
            }

            if (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode) {
                switch (mode & Trajectory::DIRECTION_MASK) {
                    case TrajectoryMode::Left: {
                        plr->releaseButton(PlayerButton::Right);
                        plr->pushButton(PlayerButton::Left);
                        break;
                    }
                    case TrajectoryMode::Right: {
                        plr->releaseButton(PlayerButton::Left);
                        plr->pushButton(PlayerButton::Right);
                        break;
                    }
                    default: {
                        plr->releaseButton(PlayerButton::Left);
                        plr->releaseButton(PlayerButton::Right);
                        break;
                    }
                }
            }
        }

        float width = m_state->m_width->inner() / pl->m_gameState.m_cameraZoom;
        emitSegment(position, plr->getPosition(), width, plr == m_fakePlayer1, colors);
    }

    bool breakIterationP1 = false;
    bool breakIterationP2 = false;

    int stepCountP1 = 0;
    int stepCountP2 = 0;

    int trajectoryInputIndex = Bot::get()->replaySystem().getInputIndex();
    const std::vector<slc::Action>& inputs =
        Bot::get()->replaySystem().m_actionAtom.m_actions;


    std::chrono::steady_clock::time_point loopStart;
    int64_t drawBeforeLoop = 0;
    if (m_prof.on) {
        loopStart = std::chrono::steady_clock::now();
        drawBeforeLoop = m_prof.drawNs;
    }

    int predCount = 0;
    for (int i = 0; i < iterations && i <= config.m_maxLength; i++) {
        uint64_t frame = Bot::get()->updater().getFrame() + i;
        // The real tick this drawn tick produces, as a run's step sets it:
        // a phantom spot only counts around the moment it happened at.
        m_simRealTick = (int)frame + 1;
        // update 0x237fb3 empties the set a spawnGroup keeps for a step at the
        // very top of it. The prediction takes its inputs there rather than
        // inside processCommands, and a touch trigger one of them fires spawns
        // through that set, so it is emptied before them as well as in
        // stepTickBegin below.
        if (world::Run* tickRun = world::currentRun()) {
            tickRun->spawnTuples.clear();
            tickRun->player1Dead = m_deadP1;  // see world::onButton
        }
        if (Bot::get()->isPlaying()) {
            while (trajectoryInputIndex < (int)inputs.size() &&
                   inputs[trajectoryInputIndex].m_frame <= frame) {
                const auto& input = inputs[trajectoryInputIndex];
                PlayerObject* p =
                    (input.m_player2) ? m_fakePlayer2 : m_fakePlayer1;
                PlayerObject* other =
                    (input.m_player2) ? m_fakePlayer1 : m_fakePlayer2;
                // Each of these is a GJBaseGameLayer::handleButton too: the
                // prediction's own World hands it to its touch listeners. The
                // prediction takes its inputs at the top of the tick rather
                // than inside processCommands, so what they toggle is stamped
                // with the index of the tick before - nothing a pose is worked
                // out from reads a toggle's stamp (ObjectCache::applyOnce).
                world::Run* buttonRun = world::currentRun();
                auto button = [buttonRun](PlayerObject* who, bool down) {
                    if (buttonRun) world::onButton(*buttonRun, who, down);
                };
                if (input.m_holding) {
                    p->pushButton(static_cast<PlayerButton>(input.m_type));
                    if (both && pl->m_gameState.m_isDualMode) {
                        other->pushButton(
                            static_cast<PlayerButton>(input.m_type));
                    }
                    button(p, true);
                } else {
                    p->releaseButton(static_cast<PlayerButton>(input.m_type));
                    if (both && pl->m_gameState.m_isDualMode) {
                        other->releaseButton(
                            static_cast<PlayerButton>(input.m_type));
                    }
                    button(p, false);
                }

                trajectoryInputIndex++;
                if (trajectoryInputIndex >= (int)inputs.size()) {
                    break;
                }
            }
        }

        if (acDriven) {
            ac.update(player, state, isP2, frame, false);
        }
        displayMovingStep(bot->updater().getPhysicsDt());
        // The press Tap and DoubleHeld kept down over the first tick is let go
        // at the top of the second, before its physics, as the game handles a
        // release queued for that tick.
        if (releaseNextTick && i == 1) {
            for (int k = 0; k < count; k++) releaseJump(players[k]);
        }

        const bool tickOther = both && pl->m_gameState.m_isDualMode && !breakIterationP2;
        // The prediction's own World run, when it steps the level's triggers.
        world::Run* displayRun = world::currentRun();
        const bool worldTick = world::current() && (!breakIterationP1 || tickOther);
        if (worldTick) {
            // Once per tick for the copies this tick runs, as a Sim::step does
            // (see iterate): a speed parked on the last tick, the command index
            // the contact rule stamps with (processCommands 0x239cf0, without
            // it every contact of the prediction looked current for ever), and
            // the clock. With the triggers stepped, the queued spawns come
            // before the parked speed and the move step after the index, as in
            // Sim::step (the tick's input came first above: the prediction keeps
            // its replay and autoclicker inputs at the top of the tick).
            PlayerObject* ticking[2] = {nullptr, nullptr};
            int n = 0;
            if (!breakIterationP1) ticking[n++] = player;
            if (tickOther) ticking[n++] = other;
            if (displayRun) {
                const float dt = world::stepDt();
                displayRun->walks = world::walkModelled(*displayRun);
                displayRun->walked = false;
                displayRun->walkAfter = tickOther ? other : player;
                world::stepTickBegin(*displayRun, dt, ticking, n);
                pl->m_gameState.m_commandIndex += 2;
                displayRun->ws->commandIndex = pl->m_gameState.m_commandIndex;
                world::stepTick(*displayRun, dt);
            } else {
                world::applyParkedSpeed(pl, ticking, n);
                pl->m_gameState.m_commandIndex += 2;
            }
            world::advanceClock(pl);
        }
        // The objects each copy can meet on the tick, where the prediction's
        // log has them (a prediction whose World puts them in place).
        world::Materializer* displayObjects = displayRun ? displayRun->objects : nullptr;
        if (!breakIterationP1) {
            if (displayObjects) displayObjects->materializeNear(*displayRun, player);
            breakIterationP1 =
                this->iterate(pl, player, mode | playerMask, colors,
                              hasPerformedClick, stepCountP1, config);
            predCount++;
        }
        if (both && pl->m_gameState.m_isDualMode && !breakIterationP2) {
            if (displayObjects) displayObjects->materializeNear(*displayRun, other);
            breakIterationP2 =
                this->iterate(pl, other, mode | TrajectoryMode::Player2, colors,
                              hasPerformedClickOther, stepCountP2, config);
        }
        // GJBaseGameLayer::update 0x238990: the contacts no copy touched on
        // this tick are forgotten (a prediction with the World only), after the
        // spawn walk of a prediction that steps the triggers.
        if (displayRun && worldTick) {
            if (displayRun->walks && !displayRun->walked) world::checkSpawnObjects(*displayRun);
            world::endTick(*displayRun);
        } else if (world::WorldState* ws = world::current()) {
            world::ageContacts(*ws, pl->m_gameState.m_commandIndex);
        }
    }
    m_simRealTick = -1;  // the drawn path is done: no tick to measure a spot against
    if (m_prof.on) {
        m_prof.loopNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - loopStart)
                            .count();
        m_prof.loopDrawNs = m_prof.drawNs - drawBeforeLoop;
        m_prof.ticks[0] = stepCountP1;
        m_prof.ticks[1] = stepCountP2;
    }
    // The runs still open end where the line ends.
    flushRuns();
    m_mergeRuns = false;

    return {
        .position = player->getPosition(),
        .hitbox = player->getObjectRect(),
        .innerHitbox = player->getObjectRect(0.3, 0.3),
        .rotation = player->getRotationX(),
        .p1 = (mode & TrajectoryMode::Player1) != 0,
        .holding = (mode & TrajectoryMode::Hold) != 0,
        .score = predCount,
    };
}


// The copy's button map is not part of copyAttributes or the checkpoint,
// and GD's pushButton / releaseButton look at it: a stale "held" from an
// earlier run makes a press silently do nothing. Keep it the real one's.
static void syncButtons(PlayerObject* fake, PlayerObject* real) {
    fake->m_holdingButtons = real->m_holdingButtons;
}

// Press / release with the map put in the state the call expects first, so
// the call always takes effect.
static void pressJump(PlayerObject* p) {
    p->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = false;
    p->pushButton(PlayerButton::Jump);
}
static void releaseJump(PlayerObject* p) {
    p->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = true;
    p->releaseButton(PlayerButton::Jump);
}

// ------------------------------------------------------------ modes

int Trajectory::modeOf(const PlayerObject* p) {
    if (p->m_isShip) return kModeShip;
    if (p->m_isBall) return kModeBall;
    if (p->m_isBird) return kModeBird;
    if (p->m_isDart) return kModeDart;
    if (p->m_isRobot) return kModeRobot;
    if (p->m_isSpider) return kModeSpider;
    if (p->m_isSwing) return kModeSwing;
    return kModeCube;
}

int Trajectory::modeOf(const SavedPlayerCheckpoint& c) {
    if (c.m_isShip) return kModeShip;
    if (c.m_isBall) return kModeBall;
    if (c.m_isBird) return kModeBird;
    if (c.m_isDart) return kModeDart;
    if (c.m_isRobot) return kModeRobot;
    if (c.m_isSpider) return kModeSpider;
    if (c.m_isSwing) return kModeSwing;
    return kModeCube;
}

// The game's toggles return early when the flag already says so; the flag
// is put the other way first so the switch always happens.
static void toggleMode(PlayerObject* p, int mode, bool on) {
    switch (mode) {
        case Trajectory::kModeShip:
            p->m_isShip = !on;
            p->toggleFlyMode(on, true);
            break;
        case Trajectory::kModeBall:
            p->m_isBall = !on;
            p->toggleRollMode(on, true);
            break;
        case Trajectory::kModeBird:
            p->m_isBird = !on;
            p->toggleBirdMode(on, true);
            break;
        case Trajectory::kModeDart:
            p->m_isDart = !on;
            p->toggleDartMode(on, true);
            break;
        case Trajectory::kModeRobot:
            p->m_isRobot = !on;
            p->toggleRobotMode(on, true);
            break;
        case Trajectory::kModeSpider:
            p->m_isSpider = !on;
            p->toggleSpiderMode(on, true);
            break;
        case Trajectory::kModeSwing:
            p->m_isSwing = !on;
            p->toggleSwingMode(on, true);
            break;
        default:
            break;
    }
}

void Trajectory::setFakeMode(PlayerObject* fake, int mode) {
    const int idx = fake == m_fakePlayer2 ? 1 : 0;
    const int cur = m_fakeNodeMode[idx];
    if (cur == mode) return;
    // Through the cube, the way the game itself changes between kinds.
    if (cur != kModeCube) toggleMode(fake, cur, false);
    if (mode != kModeCube) toggleMode(fake, mode, true);
    m_fakeNodeMode[idx] = mode;
}

void Trajectory::noteFakeMode(PlayerObject* fake, int mode) {
    if (!isFakePlayer(fake)) return;
    m_fakeNodeMode[fake == m_fakePlayer2 ? 1 : 0] = mode;
}

// ------------------------------------------------------------ rings

static unsigned simTickOf(PlayerObject* p) {
    return p->m_gameLayer ? p->m_gameLayer->m_gameState.m_currentProgress : 0u;
}

void Trajectory::noteRingContact(PlayerObject* p, GameObject* ring) {
    if (!isFakePlayer(p) || !ring) return;
    m_ringTouch[p == m_fakePlayer2 ? 1 : 0][(uintptr_t)ring] = simTickOf(p);
}

bool Trajectory::ringUsedThisContact(PlayerObject* p, GameObject* ring) {
    if (!isFakePlayer(p) || !ring) return false;
    const int i = p == m_fakePlayer2 ? 1 : 0;
    auto& used = m_ringUsed[i];
    auto it = used.find((uintptr_t)ring);
    if (it == used.end()) return false;
    // Still in contact when the ring was touched on the previous tick or
    // this one (a press comes before the tick's own collision pass).
    auto& touch = m_ringTouch[i];
    auto jt = touch.find((uintptr_t)ring);
    if (jt != touch.end() && jt->second + 1 >= simTickOf(p)) return true;
    used.erase(it);
    return false;
}

// The groups a trigger that moves things targets, for the moving-object scan
// (sampleMovers): a run without the World carries their members at the speed
// of their last real tick, as every run did before the World.
void Trajectory::scanMovedGroups(GJBaseGameLayer* pl) {
    const int n = pl && pl->m_objects ? pl->m_objects->count() : 0;
    if (pl == m_movedFor && n == m_movedCount) return;
    m_movedFor = pl;
    m_movedCount = n;
    m_movedGroups.clear();
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!o) continue;
        bool moves = false;  // the trigger moves its target, not only switches it
        switch (o->m_objectID) {
            case 901:   // move
            case 1346:  // rotate
            case 1347:  // follow
            case 1814:  // follow player Y
            case 2067:  // scale
            case 3032:  // keyframe
            case 3033:  // keyframe animation
            case 3016:  // advanced follow
            case 3660:  // edit advanced follow
            case 3661:  // re-target advanced follow
            case 3006: case 3007: case 3008: case 3009: case 3010: case 3011:  // area effects
                moves = true;
                break;
            default:
                continue;
        }
        auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o);
        if (moves && e && e->m_targetGroupID > 0) m_movedGroups.insert(e->m_targetGroupID);
    }
}

void Trajectory::noteKiller(PlayerObject* p, GameObject* object) {
    if (!isFakePlayer(p)) return;
    if (m_killer.valid) return;  // the first hit of a tick is the one
    m_killer.valid = true;
    m_killer.tick = (int)simTickOf(p);
    m_killer.playerRect = p->getObjectRect();
    m_killer.playerInner = p->getObjectRect(0.3f, 0.3f);
    m_killer.playerRot = p->getRotation();
    if (object) {
        m_killer.id = object->m_objectID;
        m_killer.uid = object->m_uniqueID;
        m_killer.type = (int)object->m_objectType;
        m_killer.x = object->getPositionX();
        m_killer.y = object->getPositionY();
        m_killer.rot = object->getRotation();
        m_killer.outerOb = object->m_shouldUseOuterOb;
        m_killer.rect = object->getObjectRect();
        // What the run knows about it: nothing in the level names a group of
        // it and it has not moved (Static), the World runs every trigger that
        // can reach it and the run follows its objects (Modelled), or it may
        // stand somewhere else in the real game (Uncertain). The run asking is
        // the current one, so the answer is about this run's model.
        m_killer.certainty = world::certaintyOf(GJBaseGameLayer::get(), object);
        m_killer.movable = m_killer.certainty == world::Certainty::Uncertain;
    }
}

void Trajectory::noteRingUsed(PlayerObject* p, GameObject* ring) {
    if (!isFakePlayer(p) || !ring) return;
    m_ringUsed[p == m_fakePlayer2 ? 1 : 0][(uintptr_t)ring] = simTickOf(p);
}

// The copy carries the player's own ring bookkeeping (the rings it is on,
// the ones fired during this contact); the once-per-contact rule of the
// simulation starts out from it, so a decision made while the player sits
// on an orb it has already fired does not count on firing it again.
void Trajectory::seedRingContacts(PlayerObject* copy) {
    if (!isFakePlayer(copy) || !copy->m_touchingRings) return;
    if (copy->m_ringRelatedSet.empty()) return;
    const unsigned n = copy->m_touchingRings->count();
    for (unsigned i = 0; i < n; i++) {
        auto* ring = static_cast<GameObject*>(copy->m_touchingRings->objectAtIndex(i));
        if (!ring || !copy->m_ringRelatedSet.contains(ring->m_uniqueID)) continue;
        noteRingContact(copy, ring);
        noteRingUsed(copy, ring);
    }
}

// ------------------------------------------------------------ branching

// One player's part of a snapshot.
struct SimPlayerSnap {
    SavedPlayerCheckpoint player;
    cocos2d::CCPoint position;
    float rotation = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float spriteWidthScale = 1.0f;
    float spriteHeightScale = 1.0f;
    gd::map<int, bool> buttons;
    // The copy's m_lastPosition: the lock inputs of a move locked to the player
    // read it before the copy's own update rewrites it (world/step.cpp (m)), so
    // a run with the World puts it back with the rest of the copy.
    cocos2d::CCPoint lastPosition;
};

// The real player only changes when the real game runs a tick or is put
// back; without this every run of a decision made from the live state
// copies the same checkpoint (with its maps, sets and vectors) again -
// hundreds of times per decision. It is deliberately NOT keyed on a tick
// number: the search goes back and plays the same tick again with different
// inputs, so the same number can mean two different states. Never freed: two
// objects for the life of the process, so the containers inside them are not
// destroyed at unload.
namespace {
SavedPlayerCheckpoint* g_realCheckpoint[2] = {nullptr, nullptr};
PlayerObject* g_realCheckpointFor[2] = {nullptr, nullptr};
}  // namespace

// The scan of the moving objects below is cached across the runs of one tick.
// Its entries are raw objects of one level, so a level being set up or torn
// down throws the cache away rather than leaving it to be recognised by a
// layer address the game has handed out again.
static unsigned g_moverCacheGen = 0;  // bumped whenever a level is set up or torn down
// The checkpoints above are of one level as well, and they are recognised by
// a player address the game can hand out again, so the same call clears them.
void moverCacheInvalidate() {
    g_moverCacheGen++;
    g_realCheckpointFor[0] = nullptr;
    g_realCheckpointFor[1] = nullptr;
    world::forgetLevel();  // it is rebuilt from the next level on its first use anyway
}
unsigned moverCacheGeneration() { return g_moverCacheGen; }

// The kinds a copy can collide with. The collision pass drops these before it
// reads anything else about them (physics/collisions.cpp), so carrying them
// along can never change a copy - and moving scenery used up the 512 mover
// places before the blocks and spikes further down the object list.
static bool carriedKind(const GameObject* o) {
    switch (o->m_objectType) {
        case GameObjectType::Decoration:
        case GameObjectType::CollisionObject:
        case GameObjectType::SecretCoin:
        case GameObjectType::UserCoin:
        case GameObjectType::Collectible:
        case GameObjectType::EnterEffectObject:
            return false;
        default:
            break;
    }
    return true;
}

// Where the objects a move trigger can move were on the last two real ticks.
// An ordinary move, follow or rotate trigger runs inside the effect manager
// and shows up in neither m_moveEffectInstances (the area effects) nor
// m_dynamicMoveActions, so the scan below used to see nothing at all on a
// level built from them: every block stood frozen where the decision tick
// found it. m_lastPosition alone cannot say an object is moving now - the
// game may only refresh it on the ticks the object moves, and a block whose
// move has ended would be carried on forever - so an object counts as moving
// only when its own position a real tick ago says so as well. Taken from
// Trajectory::realStateChanged, which the game's tick and every restore
// already call; a gap or a jump in the tick count starts the samples over.
namespace {
struct MoverSamples {
    GJBaseGameLayer* layer = nullptr;
    int objectCount = -1;
    unsigned gen = ~0u;
    std::vector<GameObject*> objects;             // the objects of the moved groups, in m_objects order
    std::unordered_set<GameObject*> objectSet;    // the same, to look up
    std::vector<cocos2d::CCPoint> at[2];          // [1] the newest sample, [0] the one a tick before it
    unsigned progress[2] = {0u, 0u};
    bool valid[2] = {false, false};
};
MoverSamples g_moverSamples;
}  // namespace

static void sampleMovers(GJBaseGameLayer* pl, const std::unordered_set<int>& movedGroups) {
    MoverSamples& ms = g_moverSamples;
    const int n = (int)pl->m_objects->count();
    if (ms.layer != pl || ms.objectCount != n || ms.gen != g_moverCacheGen) {
        ms.layer = pl;
        ms.objectCount = n;
        ms.gen = g_moverCacheGen;
        ms.objects.clear();
        ms.objectSet.clear();
        ms.valid[0] = ms.valid[1] = false;
        if (!movedGroups.empty()) {
            for (int i = 0; i < n; i++) {
                auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (!o || o->m_groupCount <= 0 || !o->m_groups || !carriedKind(o)) continue;
                for (int g = 0; g < o->m_groupCount && g < 10; g++) {
                    if (movedGroups.count((int)(*o->m_groups)[(size_t)g]) != 0) {
                        ms.objects.push_back(o);
                        ms.objectSet.insert(o);
                        break;
                    }
                }
            }
        }
    }
    if (ms.objects.empty()) return;
    const unsigned now = pl->m_gameState.m_currentProgress;
    if (ms.valid[1] && ms.progress[1] + 1 == now) {
        std::swap(ms.at[0], ms.at[1]);
        ms.progress[0] = ms.progress[1];
        ms.valid[0] = true;
    } else if (!(ms.valid[1] && ms.progress[1] == now)) {
        ms.valid[0] = false;  // a gap or a jump: nothing a tick old to compare with
    }
    // (The same tick again - another call after the tick, or a restore to it -
    // takes the newest sample again over the old one.)
    ms.at[1].resize(ms.objects.size());
    for (size_t i = 0; i < ms.objects.size(); i++) ms.at[1][i] = ms.objects[i]->getPosition();
    ms.progress[1] = now;
    ms.valid[1] = true;
}

// Objects on the move - a move trigger already running when a run starts -
// move during the run through the game's own step, so a wall that slides
// into the copy's way is seen (frozen where they were, the copies died two
// ticks later than the player, or a corridor looked shut that opens). Only
// the objects of the groups the running moves target are touched, their
// positions go into every snapshot, and they are put back at the end.
struct MovingObjects {
    // An object that is moving right now, and how fast it was going on the
    // tick the run started.
    struct Mover {
        GameObject* o = nullptr;
        cocos2d::CCPoint base;   // where it was when the run began
        cocos2d::CCPoint step;   // how far it travels in one tick
        double px = 0.0, py = 0.0;
        // Half the object's width, read once when it is scanned. place() only
        // ever slides an object along, and sliding does not change how wide it
        // is - but place() also marks its outline stale, so asking for it every
        // tick made the game work the whole outline out again, for every one of
        // up to five hundred movers, on every tick of every run.
        float halfWidth = 0.0f;
    };
    GJBaseGameLayer* pl = nullptr;
    std::vector<Mover> movers;
    int at = 0;  // ticks into the run
    bool active = false;
    int baseTick = 0;  // the real tick the bases were scanned at
    int origin = 0;    // (the tick a run starts from) - baseTick
    // Where the copies are (x), set by the run before every placement: a
    // mover far from both copies is left where it is, since placing and
    // re-sectioning hundreds of them level-wide on every simulated tick is
    // most of a run. NAN (the prediction's) places every one.
    float nearX[2] = {NAN, NAN};

    // Which objects are moving, and how fast. The game's own move step is
    // deliberately not used: calling it advances state inside the effect
    // manager that undoing a branch cannot put back, and the search would
    // then get a different answer every time it ran the same script (every
    // steered and searched idea lied about how long it lasted). A moving
    // object is carried along its own speed instead, which is a pure
    // function of how many ticks in we are - so a branch is exact.
    void begin(GJBaseGameLayer* layer) {
        pl = layer;
        movers.clear();
        at = 0;
        origin = 0;
        baseTick = (int)Bot::get()->updater().getFrame();
        active = false;
        if (!pl || !pl->m_objects) return;
        // The area effects and the dynamic-mode moves, as before: every object
        // of the level whose position changed on the last tick.
        const bool areaMoves = !pl->m_gameState.m_moveEffectInstances.empty() || !pl->m_gameState.m_dynamicMoveActions.empty();
        // The objects of the groups a move trigger targets, judged against where
        // they were a real tick ago (see MoverSamples): which of the two samples
        // is from the tick before this one, or -1 when neither is.
        const MoverSamples& ms = g_moverSamples;
        const unsigned progressNow = pl->m_gameState.m_currentProgress;
        int older = -1;
        if (ms.layer == pl && ms.gen == g_moverCacheGen && ms.objectCount == (int)pl->m_objects->count() && !ms.objects.empty()) {
            if (ms.valid[1] && ms.progress[1] + 1 == progressNow) older = 1;
            else if (ms.valid[0] && ms.valid[1] && ms.progress[1] == progressNow && ms.progress[0] + 1 == progressNow) older = 0;
        }
        if (!areaMoves && older < 0) return;
        // Scanning every object of the level is most of a run on a big one
        // (Acu: tens of thousands of objects, seven hundred runs a decision).
        // The objects only move on real ticks, so one scan serves every run
        // made from the same tick of the game.
        static GJBaseGameLayer* cachedFor = nullptr;
        static unsigned int cachedProgress = ~0u;
        static unsigned cachedGen = ~0u;
        static bool cachedSampled = false;
        static std::vector<Mover> cached;
        bool hit = cachedGen == g_moverCacheGen && cachedFor == pl && cachedProgress == pl->m_gameState.m_currentProgress &&
                   cachedSampled == (older >= 0);
        // The same tick of the game again - but the objects must still be where they were
        // scanned (a restarted attempt, a loaded state or another point of the tick moves them).
        for (size_t i = 0; hit && i < cached.size(); i++) {
            const cocos2d::CCPoint now = cached[i].o->getPosition();
            if (now.x != cached[i].base.x || now.y != cached[i].base.y) hit = false;
        }
        if (hit) {
            movers = cached;
        } else {
            if (older >= 0) {
                const std::vector<cocos2d::CCPoint>& was = ms.at[older];
                for (size_t i = 0; i < ms.objects.size() && i < was.size() && movers.size() < 512; i++) {
                    GameObject* o = ms.objects[i];
                    if (o->m_isGroupDisabled) continue;
                    const cocos2d::CCPoint now = o->getPosition();
                    const cocos2d::CCPoint moved = now - was[i];
                    // Not moving over the last real tick: never carried on, whatever
                    // m_lastPosition still holds from a move that has ended.
                    if (std::abs(moved.x) < 0.0005f && std::abs(moved.y) < 0.0005f) continue;
                    const cocos2d::CCPoint step = now - o->m_lastPosition;
                    // ... and the game's own last step has to agree, or it is not one tick of a move.
                    if (std::abs(step.x - moved.x) > 0.01f || std::abs(step.y - moved.y) > 0.01f) continue;
                    if (std::abs(step.x) > 40.0f || std::abs(step.y) > 40.0f) continue;  // teleported, not moving
                    movers.push_back(Mover{o, now, step, o->m_positionX, o->m_positionY,
                                           o->getObjectRect().size.width * 0.5f});
                }
            }
            if (areaMoves) {
                const int n = pl->m_objects->count();
                for (int i = 0; i < n && movers.size() < 512; i++) {
                    auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                    if (!o || o->m_isGroupDisabled) continue;
                    // Only what a copy can touch (see carriedKind): a moving background
                    // filled the 512 places with decoration, each one placed and
                    // re-sectioned on every simulated tick, and a moving spike further
                    // down the object list was left frozen.
                    if (!carriedKind(o)) continue;
                    if (older >= 0 && ms.objectSet.count(o) != 0) continue;  // judged by its samples above
                    const cocos2d::CCPoint now = o->getPosition();
                    const cocos2d::CCPoint step = now - o->m_lastPosition;
                    if (std::abs(step.x) < 0.0005f && std::abs(step.y) < 0.0005f) continue;  // standing still
                    if (std::abs(step.x) > 40.0f || std::abs(step.y) > 40.0f) continue;      // teleported, not moving
                    movers.push_back(Mover{o, now, step, o->m_positionX, o->m_positionY,
                                           o->getObjectRect().size.width * 0.5f});
                }
            }
            cached = movers;
            cachedFor = pl;
            cachedProgress = pl->m_gameState.m_currentProgress;
            cachedGen = g_moverCacheGen;
            cachedSampled = older >= 0;
        }
        active = !movers.empty();
        resume();  // where they really are now is where they go back to
    }

    void place(int tick) {
        if (!active) return;
        at = tick;
        // A run from a kept start stands at another tick than the bases were
        // scanned at, so the objects are carried that far as well (origin).
        const int k = tick + origin;
        for (const Mover& m : movers) {
            const cocos2d::CCPoint want{m.base.x + m.step.x * (float)k, m.base.y + m.step.y * (float)k};
            // Far from both copies where it should be and where it stands: left
            // alone. Its place is a pure function of k, so it is put exactly
            // where it belongs on the first tick a copy comes near (or before a
            // copy teleports, when every one is: see moversCatchUp).
            if (!std::isnan(nearX[0])) {
                const float r = 800.0f + m.halfWidth;
                const cocos2d::CCPoint now0 = m.o->getPosition();
                auto farAway = [&](float x) { return std::abs(x - nearX[0]) > r && std::abs(x - nearX[1]) > r; };
                if (farAway(want.x) && farAway(now0.x)) continue;
            }
            // Always one tick behind where it is placed, as the game's own move
            // step leaves it (the slope code reads it), on straight runs and
            // after a rewind alike.
            m.o->m_lastPosition = cocos2d::CCPoint{want.x - m.step.x, want.y - m.step.y};
            const cocos2d::CCPoint now = m.o->getPosition();
            if (now.x == want.x && now.y == want.y) continue;
            m.o->m_positionX = m.px + (double)m.step.x * k;
            m.o->m_positionY = m.py + (double)m.step.y * k;
            m.o->setPosition(want);
            m.o->setObjectRectDirty(true);
            m.o->setOrientedRectDirty(true);
            pl->updateObjectSection(m.o);
        }
    }

    void step(float) {
        if (!active) return;
        place(at + 1);
    }
    int mark() const { return at; }
    void rewind(int tick) { place(tick); }

    // A search runs in slices with real ticks in between (in realtime the
    // game is playing meanwhile), so the objects' real positions move on.
    // A slice puts them back where they really are when it ends - not
    // where the run began - and carries its own placement on from the same
    // bases next slice. Re-basing on the moved objects instead made the
    // same script last a different number of ticks in the next slice.
    std::vector<cocos2d::CCPoint> real;
    std::vector<std::pair<double, double>> realP;
    std::vector<cocos2d::CCPoint> realLast;
    void resume() {
        if (!active) return;
        real.resize(movers.size());
        realP.resize(movers.size());
        realLast.resize(movers.size());
        for (size_t i = 0; i < movers.size(); i++) {
            real[i] = movers[i].o->getPosition();
            realP[i] = {movers[i].o->m_positionX, movers[i].o->m_positionY};
            realLast[i] = movers[i].o->m_lastPosition;
        }
    }
    void suspend() {
        if (!active) return;
        for (size_t i = 0; i < movers.size() && i < real.size(); i++) {
            Mover& m = movers[i];
            if (i < realLast.size()) m.o->m_lastPosition = realLast[i];
            const cocos2d::CCPoint now = m.o->getPosition();
            if (now.x == real[i].x && now.y == real[i].y) continue;
            m.o->m_positionX = realP[i].first;
            m.o->m_positionY = realP[i].second;
            m.o->setPosition(real[i]);
            m.o->setObjectRectDirty(true);
            m.o->setOrientedRectDirty(true);
            pl->updateObjectSection(m.o);
        }
    }
    void end() {
        if (active) suspend();
        active = false;
        movers.clear();
        real.clear();
        realP.clear();
        realLast.clear();
    }
};

static MovingObjects g_displayMoving;  // the prediction's (the runs have their own)
static void displayMovingStep(float dt) { g_displayMoving.step(dt); }

// The moving objects of the run whose tick is in progress (set by Sim::step).
static MovingObjects* g_runMovers = nullptr;

// A copy is about to teleport: the movers left where they were because they
// were far from it (see MovingObjects::nearX) may be where it lands, or be the
// portal or group object it lands on, so every one goes where it belongs at the
// run's tick first - as every one was before movers far away were skipped.
void moversCatchUp() {
    // A run whose objects the World puts in place: every object its log has
    // moved goes where it belongs, wherever the window of the copies is.
    if (world::Run* run = world::currentRun(); run && run->objects && run->objects->active()) {
        run->objects->catchUpAll(*run);
        return;
    }
    if (!g_runMovers) return;
    MovingObjects& mv = *g_runMovers;
    const float kept[2] = {mv.nearX[0], mv.nearX[1]};
    mv.nearX[0] = mv.nearX[1] = NAN;
    mv.place(mv.at);
    mv.nearX[0] = kept[0];
    mv.nearX[1] = kept[1];
}

// The game state a portal writes and the collision pass reads back. A copy
// taking a portal runs the game's playerWillSwitchMode -> updateDualGround,
// which sets the flying kinds' floor and ceiling base, free mode and
// m_lastActivatedPortal1/2. checkCollisions reads the bounds through
// getMinPortalY / getMaxPortalY (with the camera zoom and offset) for the
// flying kinds, the ball, the spider and all of dual. Plain data: GJGameState
// from its first field to its first map, and m_lastActivatedPortal1 up to
// m_tweenActions.
struct PortalState {
    std::array<uint8_t, 0x1a0> head{};
    std::array<uint8_t, 0x28> portals{};
    // The portal kind updateDualGround 0x2131d0 animated the ground for last.
    // It sits past both runs of bytes above, and every portal a copy crosses
    // writes it through playerWillSwitchMode, so an abandoned branch used to
    // leave its own portal's kind behind for the branch that replaced it.
    uint32_t groundMode = 0;
    static size_t headBytes(const GJGameState& gs) {
        return std::min<size_t>(0x1a0, (size_t)((const char*)&gs.m_spawnChannelRelated0 - (const char*)&gs.m_cameraZoom));
    }
    static size_t portalBytes(const GJGameState& gs) {
        return std::min<size_t>(0x28, (size_t)((const char*)&gs.m_tweenActions - (const char*)&gs.m_lastActivatedPortal1));
    }
    void grab(GJBaseGameLayer* pl) {
        const GJGameState& gs = pl->m_gameState;
        std::memcpy(head.data(), &gs.m_cameraZoom, headBytes(gs));
        std::memcpy(portals.data(), &gs.m_lastActivatedPortal1, portalBytes(gs));
        groundMode = world::off::at<uint32_t>(pl, world::off::kDualGroundMode);
    }
    void put(GJBaseGameLayer* pl) const {
        GJGameState& gs = pl->m_gameState;
        std::memcpy(&gs.m_cameraZoom, head.data(), headBytes(gs));
        std::memcpy(&gs.m_lastActivatedPortal1, portals.data(), portalBytes(gs));
        world::off::at<uint32_t>(pl, world::off::kDualGroundMode) = groundMode;
    }
};

// Everything a simulated tick touches, so a branch can be undone. Only what
// a simulated tick changes in the game state is kept (the whole state holds
// maps and is copied once per run, not per branch).
struct Trajectory::SimSnapshot {
    SimPlayerSnap p[2];
    int movingAt = 0;  // how many ticks the moving objects have been carried
    double totalTime = 0.0;
    double unkDouble3 = 0.0;
    unsigned int currentProgress = 0;
    int unkUint5 = 0;
    float timeMod = 0.0f;
    bool timeMod2 = false;
    PortalState portal;  // the flying bounds, free mode and last portals of this tick
    // The contact map of pads, portals and triggers (m_activatedObjectIDs,
    // stamped with m_commandIndex) - a branch must not keep another's contact.
    unsigned int commandIndex = 0;
    gd::map<std::pair<int, int>, int> activatedIDs;
    // The channel a rotate-gameplay trigger switched to, and which spawn
    // channels are on: a simulated tick writes both, and the tick's spawn
    // list is read from them, so a branch has to put them back too.
    int currentChannel = 0;
    gd::unordered_map<int, bool> spawnChannel1;
    gd::unordered_map<int, int> spawnChannel0;  // how far into each channel's spawn list
    // The dual flag of the run's own tick: the collision and gravity rules
    // read it, and a search that pauses between slices would otherwise come
    // back on whatever the live game has now.
    bool dualMode = false;
    // The copies the run has, and which player each one stands for: a dual or
    // solo portal a copy crossed changes both inside the tick (see enterDual),
    // so a branch put back has to be told how many players its own tick had.
    int count = 0;
    PlayerObject* order[2] = {nullptr, nullptr};
    int masks[2] = {0, 0};
    bool separate = false;
    uint64_t teleportRand = 0;  // the run's teleport random state (phys::teleportPlayer draws from it)
    std::unordered_set<uintptr_t> activated1;
    std::unordered_set<uintptr_t> activated2;
    std::unordered_map<uintptr_t, unsigned> ringTouch[2];
    std::unordered_map<uintptr_t, unsigned> ringUsed[2];
    std::vector<TrajectoryAction> actions;
    bool dead1 = false;
    bool dead2 = false;
    bool down = false;
    bool downOther = false;
    bool otherDead = false;
    int otherSurvived = 0;
    std::vector<SimBroken> simBroken;  // the blocks the run had broken
    // What the World keeps for the run (its contacts, teleport seed, running
    // commands, spawns, toggles, items and listeners), what its triggers did to
    // the objects, and how many ticks the run had stepped.
    world::WorldState world;
    world::OpLog log;
    unsigned int ticks = 0;
};

// The players' state at some tick of the real path, kept so a run can
// begin from it later (see useStart).
struct Trajectory::SimStart {
    bool p1 = true;
    int count = 0;
    SimPlayerSnap p[2];
    double totalTime = 0.0;
    double unkDouble3 = 0.0;
    unsigned int currentProgress = 0;
    int unkUint5 = 0;
    float timeMod = 0.0f;
    bool timeMod2 = false;
    PortalState portal;      // the flying bounds, free mode and last portals of that tick
    int currentChannel = 0;  // the spawn channel of that tick (see SimSnapshot)
    gd::unordered_map<int, bool> spawnChannel1;
    gd::unordered_map<int, int> spawnChannel0;  // the index into each channel's spawn list at that tick
    bool dualMode = false;  // the game was in the dual part at that tick
    uint64_t teleportRand = 0;  // the replay system's teleport random state at that tick
    int moveTick = 0;     // the real tick this start stands at
    bool held = false;    // jump button down
    bool flying = false;  // ship / UFO / wave / swing
    float x = 0.0f;       // where the first player is
    bool held2 = false;   // the other player's button and kind (count == 2)
    bool flying2 = false;
    // What the run that carried this start here activated (advanceStart), the rings it fired included.
    std::unordered_set<uintptr_t> activated1;
    std::unordered_set<uintptr_t> activated2;
    // The World at that tick (design step 9): the state a run from the start
    // begins from instead of the live game's, the log of what the plan that
    // carried the start here did to the objects (advanceStart), and the table
    // of where the objects stood when that log begins. Null for a start kept
    // while the World was off: a run from it goes without the World rather
    // than start from nothing. A start kept from the game holds no running
    // actions (WorldState::partial) and so no table: its runs move objects by
    // the old path (MovingObjects), unless its first run comes while the game
    // still stands at its tick (see Sim::begin).
    std::shared_ptr<world::WorldStart> world;
};

std::shared_ptr<Trajectory::SimStart> Trajectory::captureStart(GJBaseGameLayer* pl, bool p1) {
    if (!pl) return nullptr;
    PlayerObject* real = p1 ? pl->m_player1 : pl->m_player2;
    if (!real) return nullptr;
    auto s = std::make_shared<SimStart>();
    auto grab = [](PlayerObject* r, SimPlayerSnap& out) {
        out.player = SavedPlayerCheckpoint::create(r);
        out.position = r->m_position;  // the real player's m_position is the one that counts
        out.rotation = r->getRotation();
        out.scaleX = r->getScaleX();
        out.scaleY = r->getScaleY();
        out.spriteWidthScale = r->m_spriteWidthScale;
        out.spriteHeightScale = r->m_spriteHeightScale;
        out.buttons = r->m_holdingButtons;
        out.lastPosition = r->m_lastPosition;
    };
    s->p1 = p1;
    grab(real, s->p[0]);
    s->count = 1;
    if (simulatesBoth(pl, p1)) {
        grab(p1 ? pl->m_player2 : pl->m_player1, s->p[1]);
        s->count = 2;
    }
    s->totalTime = pl->m_gameState.m_totalTime;
    s->unkDouble3 = pl->m_gameState.m_unkDouble3;
    s->currentProgress = pl->m_gameState.m_currentProgress;
    s->dualMode = pl->m_gameState.m_isDualMode;
    s->moveTick = (int)Bot::get()->updater().getFrame();
    s->unkUint5 = pl->m_gameState.m_unkUint5;
    s->portal.grab(pl);
    s->timeMod = pl->m_gameState.m_timeModRelated;
    s->timeMod2 = pl->m_gameState.m_timeModRelated2;
    s->currentChannel = pl->m_gameState.m_currentChannel;
    s->spawnChannel1 = pl->m_gameState.m_spawnChannelRelated1;
    s->teleportRand = Bot::get()->replaySystem().m_teleportRandomState;
    s->spawnChannel0 = pl->m_gameState.m_spawnChannelRelated0;
    // The contacts the players are in and the teleport seed of this tick: the
    // live contact map has moved on by the time a run starts from here. A
    // start is kept for every real tick, so its World leaves out what changes
    // on every tick (see WorldState::partial).
    if (!world::World::disabled) {
        auto kept = std::make_shared<world::WorldStart>();
        // The ledger's keyframe of this tick and the ticks that step it there
        // (design step 10): the whole state, running actions included, at the
        // cost of a few reference counts. Without one - the ledger has not
        // reached this tick, or the shadow check caught a drift since its
        // keyframe - the partial import of before, which leaves the running
        // actions out and marks what they move uncertain.
        if (!world::ledgerFillStart(pl, *kept, s->moveTick)) {
            kept->keyframe = std::make_shared<const world::WorldState>(world::World::captureLive(pl, false));
            kept->keyframeTick = s->moveTick;
            kept->offset = 0;
            kept->inexact = true;
        }
        // (Its first read of the level runs the level check, which can turn
        // the World off: then the state is not one to start from.)
        if (!world::World::disabled) s->world = std::move(kept);
    }
    {
        auto it = real->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        s->held = it != real->m_holdingButtons.end() && it->second;
    }
    s->flying = real->m_isShip || real->m_isBird || real->m_isDart || real->m_isSwing;
    s->x = real->m_position.x;
    if (s->count == 2) {
        PlayerObject* o = p1 ? pl->m_player2 : pl->m_player1;
        auto it = o->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        s->held2 = it != o->m_holdingButtons.end() && it->second;
        s->flying2 = o->m_isShip || o->m_isBird || o->m_isDart || o->m_isSwing;
    }
    return s;
}

void Trajectory::addPhantom(const cocos2d::CCRect& rect, int realTick) {
    for (auto& p : m_phantoms) {
        if (std::abs(p.rect.origin.x - rect.origin.x) < 0.5f && std::abs(p.rect.origin.y - rect.origin.y) < 0.5f) {
            // The same spot again: it is the later moment that matters now.
            p.realTick = realTick;
            return;
        }
    }
    m_phantoms.push_back(Phantom{rect, realTick});
    if (m_phantoms.size() > 256) m_phantoms.erase(m_phantoms.begin());
}

void Trajectory::dropPhantomsBefore(float x) {
    std::erase_if(m_phantoms, [x](const Phantom& p) { return p.rect.getMaxX() < x; });
}

bool Trajectory::hitsPhantom(PlayerObject* p) const {
    // The player's inner hitbox (the one hazards use), where it is now.
    const cocos2d::CCRect inner = p->getObjectRect(0.3f, 0.3f);
    // Whatever killed the player there moves, or the simulation would have
    // seen it: a copy only meets the spot around the moment it happened at.
    // Outside a run (no real tick to measure against) every spot counts, as
    // they all did before.
    for (const Phantom& ph : m_phantoms) {
        if (m_simRealTick >= 0 && std::abs(m_simRealTick - ph.realTick) > m_phantomWindow) continue;
        if (inner.intersectsRect(ph.rect)) return true;
    }
    return false;
}

bool Trajectory::startHeld(const SimStart& s) { return s.held; }
bool Trajectory::startFlying(const SimStart& s) { return s.flying; }
float Trajectory::startX(const SimStart& s) { return s.x; }
int Trajectory::startCount(const SimStart& s) { return s.count; }

Trajectory::StartState Trajectory::startState(const SimStart& s, bool second) {
    StartState out;
    const SimPlayerSnap& p = s.p[(second && s.count == 2) ? 1 : 0];
    const SavedPlayerCheckpoint& c = p.player;
    out.x = p.position.x;
    out.y = p.position.y;
    out.yVelocity = (float)c.m_yVelocity;
    out.gravity = (float)c.m_gravity;
    out.gravityMod = c.m_gravityMod;
    out.yStart = (float)c.m_yStart;
    // A speed portal touched on the start's last tick is parked, and it is the
    // speed from the next tick on.
    out.playerSpeed = s.timeMod != 0.0f ? s.timeMod : c.m_playerSpeed;
    out.speedMultiplier = (float)c.m_speedMultiplier;
    out.vehicleSize = c.m_vehicleSize;
    out.mode = modeOf(c);
    out.upsideDown = c.m_isUpsideDown;
    out.onGround = c.m_isOnGround;
    out.held = second ? s.held2 : s.held;
    return out;
}

Trajectory::StartState Trajectory::liveState(GJBaseGameLayer* pl, bool p1) {
    StartState out;
    PlayerObject* p = pl ? (p1 ? pl->m_player1 : pl->m_player2) : nullptr;
    if (!p) return out;
    out.x = p->m_position.x;
    out.y = p->m_position.y;
    out.yVelocity = (float)p->m_yVelocity;
    out.gravity = (float)p->m_gravity;
    out.gravityMod = p->m_gravityMod;
    out.yStart = (float)p->m_yStart;
    out.playerSpeed = pl->m_gameState.m_timeModRelated != 0.0f ? pl->m_gameState.m_timeModRelated : p->m_playerSpeed;
    out.speedMultiplier = (float)p->m_speedMultiplier;
    out.vehicleSize = p->m_vehicleSize;
    out.mode = modeOf(p);
    out.upsideDown = p->m_isUpsideDown;
    out.onGround = p->m_isOnGround;
    auto it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
    out.held = it != p->m_holdingButtons.end() && it->second;
    return out;
}
bool Trajectory::startHeld2(const SimStart& s) { return s.held2; }
bool Trajectory::startFlying2(const SimStart& s) { return s.flying2; }

// Objects the real player has activated since the kept start (the ones
// ahead of it: the player only moves forward) count as untouched while the
// start is in use, or a run from it would pass a portal without effect.
static bool activatableType(GameObjectType t) {
    switch (t) {
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::BallPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::Modifier:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderPortal:
        case GameObjectType::RedJumpPad:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::Special:
        case GameObjectType::SwingPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::SpiderOrb:
        case GameObjectType::SpiderPad:
        case GameObjectType::EnterEffectObject:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

// The kinds the collision pass treats as rings (physics/collisions.cpp): they
// are activated by firing them, never by being touched.
static bool ringType(GameObjectType t) {
    switch (t) {
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::DropRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

void Trajectory::useStart(std::shared_ptr<SimStart> start) {
    GJBaseGameLayer* pl = GJBaseGameLayer::get();
    // Put back what the previous start cleared (the objects are gone with
    // the level, and after a reset the game owns every flag again - see
    // forgetClearedFlags).
    if (pl && pl == m_clearedFor) {
        for (const ClearedFlag& c : m_clearedFlags) {
            c.object->m_activatedByPlayer1 = c.p1;
            c.object->m_activatedByPlayer2 = c.p2;
        }
    }
    m_clearedFlags.clear();
    m_clearedFor = nullptr;
    m_start = std::move(start);
    if (!m_start) return;
    if (!pl || !pl->m_objects) return;
    m_clearedFor = pl;
    const float from = m_start->x;
    // The objects that can be activated at all, found once per level (like the
    // mover cache): every stop of a repair, and every decision made ahead twice
    // (advanceStart and the start itself), read the kind of every object of the
    // level - a cache miss each - to keep a few hundred of them.
    static GJBaseGameLayer* activatableFor = nullptr;
    static int activatableCount = -1;
    static unsigned activatableGen = ~0u;
    static std::vector<EnhancedGameObject*> activatable;
    const int n = pl->m_objects->count();
    if (activatableFor != pl || activatableCount != n || activatableGen != g_moverCacheGen) {
        activatableFor = pl;
        activatableCount = n;
        activatableGen = g_moverCacheGen;
        activatable.clear();
        for (int i = 0; i < n; i++) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!o || !activatableType(o->m_objectType)) continue;
            if (auto* ce = geode::cast::typeinfo_cast<EnhancedGameObject*>(o)) activatable.push_back(ce);
        }
    }
    for (EnhancedGameObject* e : activatable) {
        GameObject* obj = e;
        // Only a flag that is set can need clearing: the cheap test first.
        if (!e->m_activatedByPlayer1 && !e->m_activatedByPlayer2) continue;
        const bool ring = ringType(obj->m_objectType);
        // A ring a start's player is on and has not fired was not activated by
        // that player at the start's tick, whichever side of its centre the
        // player stands: the collision pass skips an activated object before it
        // touches it (collisionCheckObjects 0x140214a8c), so a ring in the
        // player's touched or touching set was unused then, and firing it takes
        // it out of m_touchingRings and puts it into m_ringRelatedSet. A flag on
        // it comes from a press the game made since - and it is the very orb a
        // repair stop inside the contact has to be able to take again. Per
        // player: in the dual part the other one may have fired it before.
        unsigned onUnfired = 0;  // bit k: the start's player k
        if (ring) {
            const auto* key = static_cast<cocos2d::CCObject*>(obj);
            for (int k = 0; k < m_start->count; k++) {
                const SavedPlayerCheckpoint& c = m_start->p[k].player;
                if (c.m_ringRelatedSet.contains(obj->m_uniqueID)) continue;
                if (c.m_touchedRings.contains(obj->m_uniqueID) ||
                    std::find(c.m_touchingRings.begin(), c.m_touchingRings.end(), key) != c.m_touchingRings.end())
                    onUnfired |= 1u << k;
            }
        }
        if (!onUnfired && obj->getPositionX() <= from) continue;
        // Ahead of the start, only what its player is already on - or a ring it
        // has fired during the contact it is in - was activated at or before its
        // tick; anything else flagged there was activated in the ticks the game
        // has played since, and counts as untouched for a run from the start.
        bool before = false;
        for (int k = 0; k < m_start->count && !before; k++) {
            if (onUnfired & (1u << k)) continue;  // unused for this player at the start's tick
            const SimPlayerSnap& sp = m_start->p[k];
            if (sp.player.m_ringRelatedSet.contains(obj->m_uniqueID)) { before = true; break; }
            // The other player of a dual start: a ring it has already passed keeps its flag.
            if (onUnfired && obj->getPositionX() <= sp.position.x) { before = true; break; }
            // Nor a ring this player has not reached yet: the +6 slack below is for
            // what the player is on, and a ring the game fired a tick or two after
            // the start (the stop just before the contact) sits exactly inside it.
            if (ring && !sp.player.m_isGoingLeft && !sp.player.m_isSideways &&
                obj->getObjectRect().getMinX() > sp.position.x + 15.0f * std::max(0.6f, sp.player.m_vehicleSize))
                continue;
            const float half = 15.0f * std::max(0.6f, sp.player.m_vehicleSize) + 6.0f;
            if (obj->m_objectRadius > 0.0f) {
                const float dx = obj->getPositionX() - sp.position.x;
                const float dy = obj->getPositionY() - sp.position.y;
                const float r = obj->m_objectRadius + half;
                before = dx * dx + dy * dy <= r * r;
            } else {
                const cocos2d::CCRect box(sp.position.x - half, sp.position.y - half, half * 2.0f, half * 2.0f);
                before = obj->getObjectRect().intersectsRect(box);
            }
        }
        if (onUnfired) {
            // Only the flags of the start's players: the one on it unfired loses
            // its flag, the other keeps its own unless nothing says it was used
            // before. One ClearedFlag per object, so the restore order stays right.
            m_clearedFlags.push_back({e, e->m_activatedByPlayer1, e->m_activatedByPlayer2});
            for (int k = 0; k < m_start->count; k++) {
                if (!(onUnfired & (1u << k)) && before) continue;
                if ((k == 0) == m_start->p1) e->m_activatedByPlayer1 = false;
                else e->m_activatedByPlayer2 = false;
            }
            continue;
        }
        if (before) continue;
        m_clearedFlags.push_back({e, e->m_activatedByPlayer1, e->m_activatedByPlayer2});
        e->m_activatedByPlayer1 = false;
        e->m_activatedByPlayer2 = false;
    }
}

// The cache itself sits up with the mover cache, so that the one call a
// level's setup and teardown already make throws both of them away.
void Trajectory::realStateChanged() {
    g_realCheckpointFor[0] = nullptr;
    g_realCheckpointFor[1] = nullptr;
    // The objects' table of the last tick, and every kept table's comparison
    // with the objects, are of a real state that has just changed.
    world::noteRealStateChanged();
    // How far either side of its own real tick a phantom spot still counts:
    // a thirtieth of a second, which is about as long as whatever moved into
    // the player stays in the same place, and never fewer than two ticks.
    m_phantomWindow = std::max(2, (int)std::lround(Bot::get()->updater().getTps() / 30.0));
    // The real objects have moved on too: where the moved groups stand now is
    // what the next tick's scan compares with (see MoverSamples).
    GJBaseGameLayer* pl = GJBaseGameLayer::get();
    if (!pl || !pl->m_objects || !pl->m_player1) return;
    // The level's own check that a run through branches answers the same as a
    // run straight, once the level is loaded and nothing else owns the copies.
    checkWorldBranches(pl);
    scanMovedGroups(pl);
    sampleMovers(pl, m_movedGroups);
}

// The clone a dual portal starts the copy of the other player from (see
// Sim::enterDual). One object for the life of the process, like the real
// players' checkpoints above: the containers inside it are not destroyed at
// unload.
namespace {
SavedPlayerCheckpoint* g_dualClone = nullptr;
}  // namespace

// A scripted run of the copies: the copy of player 1 (or 2), plus the copy
// of the other player in dual mode, when one input drives both. Every step
// applies the tick's input to each copy and runs it; a death of either one
// ends the run.
struct Trajectory::Sim {
    // The run whose tick is in progress, for the copies' own code to reach
    // back into (a dual or solo portal changes how many copies the run has).
    // Null outside a tick, so the drawn prediction - which runs the same
    // physics without a Sim - keeps the copies it began with, as before.
    static inline Sim* current = nullptr;
    struct Current {
        Sim* was;
        explicit Current(Sim* s) : was(current) { current = s; }
        ~Current() { current = was; }
        Current(const Current&) = delete;
        Current& operator=(const Current&) = delete;
    };

    Trajectory* t = nullptr;
    GJBaseGameLayer* pl = nullptr;
    PlayerObject* players[2] = {nullptr, nullptr};
    int masks[2] = {0, 0};
    int count = 0;
    bool down = false;
    float levelEnd = 0.0f;
    GJGameState savedState;
    PredictionConfig config;
    int simulated = 0;
    // Two-player mode: the other copy (players[1]) follows a script of its
    // own, indexed by the tick since the start of the run (the tick count
    // lives in the game state, so a restore puts it back too), and only the
    // driven copy's death ends the run unless eitherDeath is set.
    bool separate = false;
    std::span<const TickInput> other;
    bool downOther = false;
    bool eitherDeath = false;
    bool otherDead = false;
    int otherSurvived = 0;
    MovingObjects mv;
    // The World's state of this run (contacts, teleport seed): current while a
    // tick runs, kept by every snapshot.
    world::WorldState ws;
    // Whether this run has the World, decided once in begin() and kept for
    // the run's life, slices of a paused search included (see World::disabled).
    bool worldOn = false;
    // Whether it also steps the level's triggers (the World is on and the
    // level has been read), and what that works on: the level's triggers, the
    // log of what they did to objects (kept by every snapshot), where those
    // objects stand, and the run as world/step.cpp takes it.
    bool worldStep = false;
    std::shared_ptr<const world::WorldDef> def;
    world::OpLog log;
    world::ObjectCache cache;
    world::Run run;
    // Whether the World puts the objects the log moves in place near the
    // copies (design step 8): a run with the World whose state holds the
    // running actions and whose table says where the objects stood. Every
    // other run keeps MovingObjects (`mv`), the path from before the World.
    bool worldObjects = false;
    world::Materializer objects;
    // Ticks this run has stepped (see tickIndex).
    unsigned int ticks = 0;
    // The real tick the run's first step leaves (see begin).
    int realTickBase = 0;

    bool begin(Trajectory* traj, GJBaseGameLayer* layer, bool p1, bool buttonDown, std::span<const TickInput> otherScript = {},
               bool otherButtonDown = false, bool eitherDeathEnds = false) {
        t = traj;
        pl = layer;
        if (!pl || !t->m_fakePlayer1 || !t->m_fakePlayer2) return false;
        PlayerObject* realPlayer = p1 ? pl->m_player1 : pl->m_player2;
        if (!realPlayer) return false;
        auto& updater = Bot::get()->updater();
        t->scanMovedGroups(pl);
        savedState = pl->m_gameState;
        mv.begin(pl);
        config.m_bypassConfig = true;
        config.m_silent = true;
        levelEnd = pl->m_levelLength;
        down = buttonDown;
        other = otherScript;
        downOther = otherButtonDown;
        eitherDeath = eitherDeathEnds;
        otherDead = false;
        otherSurvived = 0;
        count = 0;
        players[0] = players[1] = nullptr;
        masks[0] = masks[1] = 0;
        // A kept start (useStart) stands in for the real players. The
        // pathfinder keeps every start as player 1's; a run for player 2 (the
        // second pass of a two-player decision) takes it too when it holds both
        // copies - its snaps are the other way round.
        SimStart* from = (t->m_start && t->m_start->count > 0 && (t->m_start->p1 == p1 || t->m_start->count == 2))
                             ? t->m_start.get()
                             : nullptr;
        const int own = (from && from->p1 != p1) ? 1 : 0;  // the driven copy's snap
        // Which real tick the run starts at: a kept start's own, or the tick
        // the game stands at. Every simulated tick is measured from it, which
        // is how a phantom spot knows whether the run is passing through the
        // moment it happened at (hitsPhantom).
        realTickBase = from ? from->moveTick : (int)updater.getFrame();
        // Each copy's m_lastPosition to start from (a run with the World puts it in place).
        cocos2d::CCPoint lastPositions[2];
        auto add = [&](PlayerObject* fake, PlayerObject* real, int mask, SimPlayerSnap* snap) {
            lastPositions[count] = snap ? snap->lastPosition : real->m_lastPosition;
            fake->copyAttributes(real);
            // copyAttributes runs the game's own kind toggles with the real
            // player's flags (GD 2.2081 0x1403a04a0), so the node is the real
            // player's kind now, whatever the last run left it as. Without this
            // setFakeMode below saw its bookkeeping already at the start's kind
            // (the previous run ended in it), did nothing, and the copy ran with
            // one kind's flags on the other kind's node and hitbox.
            t->noteFakeMode(fake, modeOf(real));
            fake->m_maybeReducedEffects = true;
            if (snap) {
                t->setFakeMode(fake, modeOf(snap->player));
                snap->player.apply(fake);
                fake->setPosition(snap->position);
                fake->setRotation(snap->rotation);
                if (fake->getScaleX() != snap->scaleX) fake->setScaleX(snap->scaleX);
                if (fake->getScaleY() != snap->scaleY) fake->setScaleY(snap->scaleY);
                fake->m_spriteWidthScale = snap->spriteWidthScale;
                fake->m_spriteHeightScale = snap->spriteHeightScale;
                fake->m_holdingButtons = snap->buttons;
            } else {
                t->setFakeMode(fake, modeOf(real));
                const int slot = real == pl->m_player2 ? 1 : 0;
                if (!g_realCheckpoint[slot]) g_realCheckpoint[slot] = new SavedPlayerCheckpoint();
                if (g_realCheckpointFor[slot] != real) {
                    g_realCheckpoint[slot]->capture(real);
                    g_realCheckpointFor[slot] = real;
                }
                g_realCheckpoint[slot]->apply(fake);
                fake->setPosition(real->m_position);
                fake->setRotation(real->getRotation());
                if (fake->getScaleX() != real->getScaleX()) fake->setScaleX(real->getScaleX());
                if (fake->getScaleY() != real->getScaleY()) fake->setScaleY(real->getScaleY());
                fake->m_spriteWidthScale = real->m_spriteWidthScale;
                fake->m_spriteHeightScale = real->m_spriteHeightScale;
                syncButtons(fake, real);
            }
            players[count] = fake;
            masks[count] = mask;
            count++;
        };
        add(p1 ? t->m_fakePlayer1 : t->m_fakePlayer2, realPlayer, p1 ? TrajectoryMode::Player1 : TrajectoryMode::Player2,
            from ? &from->p[own] : nullptr);
        if (from ? from->count == 2 : t->simulatesBoth(pl, p1)) {
            add(p1 ? t->m_fakePlayer2 : t->m_fakePlayer1, p1 ? pl->m_player2 : pl->m_player1,
                p1 ? TrajectoryMode::Player2 : TrajectoryMode::Player1, from ? &from->p[1 - own] : nullptr);
        }
        if (from) {
            // The clock and the speed of that tick (put back by end()).
            pl->m_gameState.m_totalTime = from->totalTime;
            pl->m_gameState.m_unkDouble3 = from->unkDouble3;
            pl->m_gameState.m_currentProgress = from->currentProgress;
            pl->m_gameState.m_unkUint5 = from->unkUint5;
            from->portal.put(pl);
            pl->m_gameState.m_timeModRelated = from->timeMod;
            pl->m_gameState.m_timeModRelated2 = from->timeMod2;
            pl->m_gameState.m_currentChannel = from->currentChannel;
            pl->m_gameState.m_spawnChannelRelated1 = from->spawnChannel1;
            // ... and how far into the spawn list the game was then: the triggers crossed
            // since (gravity, rotate gameplay, teleport) are ahead of this start again.
            pl->m_gameState.m_spawnChannelRelated0 = from->spawnChannel0;
            // The dual part as it was at that tick: the copies run the
            // collision and gravity rules through this flag, and a start kept
            // inside a dual section is used long after the game has left it.
            pl->m_gameState.m_isDualMode = from->dualMode;
        }
        // The run's step takes the time warp the run starts under: a kept start's
        // portal state above has just put that start's warp back, and the clock
        // (iterate) reads it too, so the copy and the clock move at the same rate.
        t->m_delta = updater.getPhysicsDt() * std::min(pl->m_gameState.m_timeWarp, 1.0f) * 60.0f;
        t->m_deadP1 = false;
        t->m_deadP2 = false;
        t->clearRingContacts();
        t->clearKiller();
        // What another run left behind: a search paused between slices keeps
        // its activated objects (they come back with its own restore), and a
        // run that never reached end() keeps them too - this run would skip
        // every one of those objects.
        t->deactivateAllRemembered();
        if (from) {
            // A start carried along a plan: what that plan already used stays used.
            t->m_activatedObjectsP1 = from->activated1;
            t->m_activatedObjectsP2 = from->activated2;
        }
        // The next group teleport draws what the game would draw next from the
        // run's start (read only: the replay system's state is never written).
        t->m_teleportRand = from ? from->teleportRand : Bot::get()->replaySystem().m_teleportRandomState;
        // The World's state the run starts from: the kept start's, or the live
        // game's (its contacts and the replay system's teleport seed, read only).
        ws = world::WorldState{};
        worldOn = false;
        worldObjects = false;
        world::WorldStart* fromWorld = from ? from->world.get() : nullptr;
        // Whether a run has been made from this start before: what a start
        // settles once, it settles on its first run (see WorldStart::taken).
        const bool startTaken = fromWorld && fromWorld->taken;
        if (!world::World::disabled && (!from || fromWorld)) {
            if (fromWorld && !fromWorld->taken && fromWorld->inexact && !fromWorld->log && fromWorld->keyframe &&
                from->moveTick == (int)updater.getFrame() && savedState.m_currentProgress == from->currentProgress &&
                savedState.m_commandIndex == fromWorld->keyframe->commandIndex) {
                // A kept start whose first run comes while the game still stands
                // at its tick (a decision made ahead from the tick being played):
                // the game still holds that tick's running actions and objects,
                // so the start takes them now - for this run and every later run
                // from it, which then all begin from the same state whatever the
                // game does meanwhile.
                keepFullWorld(*fromWorld);
            }
            // Only on its first run (WorldStart::taken): a start already run
            // from while the game stood elsewhere keeps what those runs had,
            // even when a restore brings the game back to its tick.
            if (fromWorld) fromWorld->taken = true;
            ws = fromWorld ? fromWorld->materialize() : world::World::captureLive(pl);
            // captureLive's first read of a level runs the level check, which
            // can turn the World off: the run then goes without it.
            worldOn = !world::World::disabled;
            if (worldOn) {
                // The start's contacts at the game's index, which this run
                // counts on from (see restampContacts).
                world::restampContacts(ws, pl->m_gameState.m_commandIndex);
            } else {
                ws = world::WorldState{};
            }
        }
        // The level's triggers: a run with the World steps them from the state
        // above, with the log of what they do to objects (a start carried along
        // a plan brings the plan's).
        worldStep = false;
        def.reset();
        run = world::Run{};
        log.reset(0);
        if (worldOn) {
            def = world::WorldDef::get(pl);
            if (def) {
                worldStep = true;
                if (fromWorld && fromWorld->log) log = *fromWorld->log;
                else log.reset((uint32_t)ws.tick);
                cache.reset(def);
                // Where the objects the log moves stand before it: the table
                // of this tick for a run from the live game, the one a start
                // brings (read at the tick its log begins) for a start. A state
                // without the running actions has no table: what they move is
                // not in the log.
                std::shared_ptr<const world::BaseTable> table;
                std::shared_ptr<const std::vector<world::Carry>> carry;
                if (!ws.partial) {
                    if (fromWorld) {
                        // A start at a tick the game has left: where its
                        // objects stood then, from the ledger undone back to
                        // it (design step 10). Only on its first run, like
                        // every other thing a start settles once, and only
                        // while its keyframe is of this read of the level -
                        // the state of another read says nothing about where
                        // these objects stand.
                        if (fromWorld->inexact && !fromWorld->log && from && !startTaken &&
                            fromWorld->def == def) {
                            keepLedgerWorld(*fromWorld, from->moveTick, ws, def);
                        }
                        // (Only a table of this read of the level: its slots are this one's.)
                        if (!fromWorld->inexact && fromWorld->base && fromWorld->base->md &&
                            fromWorld->base->md->def == def) {
                            table = fromWorld->base;
                            carry = fromWorld->carry;
                        }
                    } else {
                        table = world::baseTable(pl, def, savedState.m_currentProgress, savedState.m_commandIndex);
                        if (table) carry = world::carriedMovers(*table, ws, *def);
                    }
                }
                if (table && carry) {
                    worldObjects = true;
                    cache.setBaseTable(table);
                    // Which tick of this run the table stands at: a start
                    // stepped out of a keyframe begins at the keyframe's
                    // offset and a start carried ahead where its plan left
                    // off, and a carried mover is one step further on for
                    // every tick past the table's own.
                    objects.begin(pl, table, carry, savedState.m_currentProgress, savedState.m_commandIndex,
                                  fromWorld ? fromWorld->baseTick : (int)ws.tick);
                }
                ws.commandIndex = pl->m_gameState.m_commandIndex;
                run.pl = pl;
                run.def = def.get();
                run.ws = &ws;
                run.log = &log;
                run.cache = &cache;
                run.objects = worldObjects ? &objects : nullptr;
                for (int i = 0; i < count; i++) {
                    if (players[i] == t->m_fakePlayer1) run.player1 = players[i];
                    else if (players[i] == t->m_fakePlayer2) run.player2 = players[i];
                    players[i]->m_lastPosition = lastPositions[i];
                }
                // The walk and the collision pass's channel filter read the run's channel.
                pl->m_gameState.m_currentChannel = ws.channel;
            }
        }
        if (worldObjects) {
            // The World moves the objects: the old carry lets go of the ones it
            // scanned (it has placed none yet, so nothing is written back).
            mv.end();
        } else {
            objects.end();
            // A kept start stands at another tick than the game: the moving objects
            // are carried from where they are now to where they are at that tick, so
            // every run from the start meets them in the same place whatever tick the
            // game has reached by then.
            mv.origin = from ? from->moveTick - mv.baseTick : 0;
            mv.nearX[0] = players[0]->getPositionX();
            mv.nearX[1] = count == 2 ? players[1]->getPositionX() : mv.nearX[0];
            if (mv.origin != 0) mv.place(0);
        }
        ticks = 0;
        t->m_actions.clear();
        // Blocks another run broke (a paused search gets its own back with its
        // restore); outside a tick they are already visible again.
        t->m_simBroken.clear();
        for (int i = 0; i < count; i++) t->seedRingContacts(players[i]);
        // Two copies means the run starts in the dual part (live, or the kept
        // start was captured there); whether the game is in it right now is not
        // the question - a probe looks at a tick the game has left.
        syncSeparate();
        return true;
    }

    // A kept start takes the World of the tick the game still stands at: its
    // running actions, the table of where the objects stand and what it
    // carries (see begin). Left as it was when any of it cannot be read.
    void keepFullWorld(world::WorldStart& start) {
        world::WorldState full = world::World::captureLive(pl, true);
        if (world::World::disabled) return;
        std::shared_ptr<const world::WorldDef> fullDef = world::WorldDef::get(pl);
        if (!fullDef) return;
        std::shared_ptr<const world::BaseTable> table =
            world::baseTable(pl, fullDef, savedState.m_currentProgress, savedState.m_commandIndex);
        if (!table) return;
        start.carry = world::carriedMovers(*table, full, *fullDef);
        start.base = std::move(table);
        start.keyframe = std::make_shared<const world::WorldState>(std::move(full));
        // The new keyframe stands at the start's own tick, so the ticks the
        // ledger would have stepped it through are behind it: stepping them
        // again would carry the state past the start.
        start.keyframeTick = realTickBase;
        start.offset = 0;
        start.baseTick = 0;  // the state is of this tick and so is the table
        start.inputs.reset();
        start.def.reset();
        start.gen = 0;
        start.inexact = false;
    }

    // A start at a tick the real game has already left takes its objects from
    // the ledger (design step 10): the table of now with every change the game
    // made since undone. Left as it was when the ledger does not reach the
    // tick or something it cannot follow moved an object in between, and the
    // start's runs then carry objects the old way, as they did before.
    void keepLedgerWorld(world::WorldStart& start, int tick, const world::WorldState& ws,
                         const std::shared_ptr<const world::WorldDef>& def) {
        if (!def || ws.partial) return;
        std::shared_ptr<const world::BaseTable> table;
        std::shared_ptr<const std::vector<world::Carry>> carry;
        if (!world::ledgerStartObjects(pl, def, ws, tick, savedState.m_currentProgress, savedState.m_commandIndex,
                                       table, carry)) {
            return;
        }
        start.base = std::move(table);
        start.carry = std::move(carry);
        // The table is of the start's own tick, which the stepped state calls
        // tick `offset`: a run from here carries a mover one step on its first
        // tick, not offset + 1 of them.
        start.baseTick = (int)ws.tick;
        start.inexact = false;
    }

    // Ticks since the start of the run: the run counts its own. It used to be
    // worked out from the game's progress divided by the copies for a run
    // without the World (whose iterate() moves the clock once per copy), which
    // a dual or solo portal halfway through a run makes nonsense of - the
    // ticks before it moved the progress by one and the ticks after it by two.
    unsigned int tickIndex() const { return ticks; }
    TickInput otherAt(unsigned int idx) const {
        if (other.empty()) return TickInput{0, downOther};
        return other[std::min<size_t>((size_t)idx, other.size() - 1)];
    }

    // Which copy the World's step hands the tick's parked speed and lock
    // inputs to, after the run has gained or lost one.
    void syncRunPlayers() {
        if (!worldStep) return;
        run.player1 = nullptr;
        run.player2 = nullptr;
        for (int i = 0; i < count; i++) {
            if (players[i] == t->m_fakePlayer1) run.player1 = players[i];
            else if (players[i] == t->m_fakePlayer2) run.player2 = players[i];
        }
    }

    // Two-player mode gives the second copy a script of its own; one copy
    // never has one.
    void syncSeparate() {
        separate = count == 2 && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
    }

    // GJBaseGameLayer::toggleDualMode 0x2168d0, the branch that turns the dual
    // part on (0x216979). The copy of the other player joins the run inside
    // the collision pass that crossed the portal, where the game puts it: the
    // dual flag is read again after player 1's pass (update 0x238517), so the
    // new player gets its own update and collision pass in the same step.
    void enterDual(PlayerObject* by, GameObject* portal) {
        if (count != 1 || !t->m_fakePlayer1 || !t->m_fakePlayer2) return;
        PlayerObject* lead = players[0];
        PlayerObject* partner = lead == t->m_fakePlayer1 ? t->m_fakePlayer2 : t->m_fakePlayer1;
        if (!partner || partner == lead) return;
        pl->m_gameState.m_isDualMode = true;  // 0x2168f4
        // Where the partner starts from. The game's player 2 has been standing
        // still since the level was read (its update is behind the same dual
        // flag), so it carries its own leftovers into the dual part; a copy
        // takes the crossing copy's instead. The real player 2 is live in the
        // dual part a run from before the portal is trying to reach, so reading
        // it would answer the same script one way on one real tick and another
        // way on the next - which is the one thing a run may not do. What
        // toggleDualMode really decides - where the new player stands, its
        // kind, its gravity and its speed - is the game's own, below.
        if (!g_dualClone) g_dualClone = new SavedPlayerCheckpoint();
        g_dualClone->capture(lead);
        // 0x2169fb: copyAttributes first, as the game has it - it runs the
        // game's own kind toggles, so the partner's node is the crossing
        // copy's kind whatever the last run left it as (see begin()).
        partner->copyAttributes(lead);
        t->noteFakeMode(partner, modeOf(lead));
        t->setFakeMode(partner, modeOf(lead));
        // Which player a copy is stays its own: the checkpoint carries that
        // flag with everything else, and the game reads it for the icon and
        // for the player a landing or a jump is reported under.
        const bool partnerIsSecond = partner->m_isSecondPlayer;
        g_dualClone->apply(partner);
        partner->m_isSecondPlayer = partnerIsSecond;
        // 0x216956-0x216966: both players are told the level is in the dual part.
        lead->m_unkA99 = true;
        partner->m_unkA99 = true;
        partner->setPosition(lead->getPosition());
        partner->setRotation(lead->getRotation());
        if (partner->getScaleX() != lead->getScaleX()) partner->setScaleX(lead->getScaleX());
        if (partner->getScaleY() != lead->getScaleY()) partner->setScaleY(lead->getScaleY());
        partner->m_spriteWidthScale = lead->m_spriteWidthScale;
        partner->m_spriteHeightScale = lead->m_spriteHeightScale;
        partner->m_holdingButtons = lead->m_holdingButtons;
        partner->m_lastPosition = lead->m_lastPosition;
        partner->m_maybeReducedEffects = true;
        // A player the game has just put into the level is standing on nothing:
        // the orbs the crossing copy is on are for the partner's own collision
        // pass to find this tick, and one the crossing copy has already fired
        // is still there for the new player to fire.
        if (partner->m_touchingRings) partner->m_touchingRings->removeAllObjects();
        partner->m_touchedRings.clear();
        partner->m_ringRelatedSet.clear();
        {
            const int pi = partner == t->m_fakePlayer2 ? 1 : 0;
            t->m_ringTouch[pi].clear();
            t->m_ringUsed[pi].clear();
        }
        // 0x216a06-0x216a5b: the gravity and the y speed the dual part gives
        // the new player - mirrored unless the level has unlinked dual gravity
        // - and it is not standing on anything.
        const bool unlinked = pl->m_gameState.m_unkBool31;
        const bool up = unlinked ? lead->m_isUpsideDown : !lead->m_isUpsideDown;
        if (partner->m_isUpsideDown != up) phys::flipGravityInner(partner, up);
        partner->m_yVelocity = unlinked ? lead->m_yVelocity : -lead->m_yVelocity;
        partner->m_isOnGround = false;
        partner->m_isOnGround2 = false;
        players[1] = partner;
        masks[1] = lead == t->m_fakePlayer1 ? TrajectoryMode::Player2 : TrajectoryMode::Player1;
        count = 2;
        syncSeparate();
        syncRunPlayers();
        // The spawn walk goes after the last copy that ticks in the step
        // (update 0x23861f, after both players' collision passes), which is
        // the copy that has just joined.
        if (worldStep && run.walks && !run.walked) run.walkAfter = partner;
        // 0x216b1e: the portal the dual part was entered by, which the ground
        // is worked out from while the level is in it (updateDualGround
        // 0x2131aa). Then playerWillSwitchMode (0x216b33), with the copies in
        // the layer's player slots and the flag the dual portal case holds
        // over the call (0x21588d).
        if (portal) pl->m_gameState.m_lastActivatedPortal2 = portal;
        t->m_fakePlayer2->m_isBeingSpawnedByDualPortal = true;
        {
            phys::CopyPlayersScope copies(pl, by);
            pl->playerWillSwitchMode(by, portal);
        }
        t->m_fakePlayer2->m_isBeingSpawnedByDualPortal = false;
    }

    // ... and the branch that turns it off (0x216b3d).
    void leaveDual(PlayerObject* by, GameObject* portal) {
        if (count != 2 || !t->m_fakePlayer1 || !t->m_fakePlayer2) return;
        PlayerObject* p1copy = t->m_fakePlayer1;
        PlayerObject* p2copy = t->m_fakePlayer2;
        pl->m_gameState.m_isDualMode = false;  // 0x2168f4
        p1copy->m_unkA99 = false;              // 0x216956-0x216966
        p2copy->m_unkA99 = false;
        // 0x216be1: the player that stays is player 1, and when it was player 2
        // that crossed the portal, player 1 carries on from player 2's state.
        if (by == p2copy) {
            p1copy->copyAttributes(p2copy);
            t->noteFakeMode(p1copy, modeOf(p2copy));
            p1copy->m_holdingButtons = p2copy->m_holdingButtons;
            p1copy->m_lastPosition = p2copy->m_lastPosition;
        }
        p2copy->releaseAllButtons();  // 0x216c1f
        p2copy->setVisible(false);
        // The level keeps the copy of player 1, so the run goes on with it. A
        // run driven by player 2 (the second pass of a two-player decision) has
        // it in the other slot: it moves into the driven one, because the copy
        // that pass was following is the one the level has just taken away.
        if (players[0] == p2copy) {
            std::swap(players[0], players[1]);
            std::swap(masks[0], masks[1]);
        }
        count = 1;
        syncSeparate();
        syncRunPlayers();
        // The copy the spawn walk was waiting for may be the one just dropped,
        // and it will not tick again: the walk goes with the copy whose tick
        // this is (the crossing one). When that is the dropped copy itself,
        // its own tick still runs to the end and takes the walk with it.
        if (worldStep && run.walks && !run.walked && run.walkAfter != by) run.walkAfter = by;
        // 0x216d06-0x216d8d: the portal the dual part was entered by is
        // forgotten, and an effect portal hands the level its camera settings
        // (the same ones playerWillSwitchMode takes on the way in).
        pl->m_gameState.m_lastActivatedPortal2 = nullptr;
        if (portal && portal->m_classType == GameObjectClassType::Effect) {
            auto* e = static_cast<EffectGameObject*>(portal);
            world::off::at<bool>(pl, world::off::kCameraFreeMode) = e->m_cameraIsFreeMode;
            world::off::at<bool>(pl, world::off::kCameraGridSnap) = e->m_cameraDisableGridSnap;
            if (e->m_cameraEditCameraSettings) {
                world::off::at<float>(pl, world::off::kCameraEasing) =
                    std::min(40.0f, std::max(1.0f, e->m_cameraEasingValue));
                world::off::at<float>(pl, world::off::kCameraPadding) =
                    std::min(1.0f, std::max(0.0f, e->m_cameraPaddingValue));
            }
        }
        // 0x216d95-0x216df8: the ground goes back to the kind the copy of
        // player 1 is in now (the game's own type numbers).
        PlayerObject* stay = players[0];
        int mode = 6;  // CubePortal
        if (stay->m_isShip || stay->m_isBird || stay->m_isDart || stay->m_isSwing) mode = 5;  // ShipPortal
        else if (stay->m_isBall) mode = 16;                                                   // BallPortal
        else if (stay->m_isSpider) mode = 33;                                                 // SpiderPortal
        {
            phys::CopyPlayersScope copies(pl, by);
            pl->updateDualGround(by, mode, false, 0.0f);
        }
    }

    // The tick's input on every copy (the other copy's own script with
    // separate controls), then the tick; true when the run is over.
    bool step(const TickInput& in) {
        // A copy's tick runs the game's own code, which reports to the level
        // (event triggers on landing or jumping, a breakable block destroyed,
        // the jump count). The hooks drop those while simulating() is true. The
        // blocks this run has broken are hidden for the tick only, so the real
        // game ticking between the slices of a paused search still meets them.
        struct SimulatedTick {
            Trajectory* t;
            bool was;
            explicit SimulatedTick(Trajectory* traj) : t(traj), was(traj->m_simulating) {
                t->m_simulating = true;
                for (SimBroken& b : t->m_simBroken) {
                    b.disabled = b.object->m_isDisabled;
                    b.disabled2 = b.object->m_isDisabled2;
                    b.object->m_isDisabled = true;
                    b.object->m_isDisabled2 = true;
                }
            }
            ~SimulatedTick() {
                for (SimBroken& b : t->m_simBroken) {
                    b.object->m_isDisabled = b.disabled;
                    b.object->m_isDisabled2 = b.disabled2;
                }
                t->m_simulating = was;
            }
        } simulatedTick(t);
        // This run's movers for a teleport in the tick (see moversCatchUp).
        struct RunMovers {
            MovingObjects* was;
            explicit RunMovers(MovingObjects* m) : was(g_runMovers) { g_runMovers = m; }
            ~RunMovers() { g_runMovers = was; }
        } runMovers(&mv);
        // A dual or solo portal a copy crosses reaches this run (dualPortal).
        Current currentSim(this);
        // The contact rule, group teleports and clock of the copies read this
        // run's World state (none for a run without the World), and the
        // triggers the copies fire its whole run.
        world::Scope worldScope(worldStep ? &run : nullptr, worldOn ? &ws : nullptr);
        const unsigned int idx = tickIndex();
        // The real tick this simulated tick produces: what a phantom spot's
        // window is measured against (hitsPhantom).
        t->m_simRealTick = realTickBase + (int)idx + 1;
        // Each button change of the copy is also a GJBaseGameLayer::handleButton,
        // which hands it to the run's touch listeners (world::onButton); a run
        // without the World has none to hand it to.
        auto apply = [this](PlayerObject* p, const TickInput& ti, bool d, bool ownInput) {
            auto button = [this, ownInput](PlayerObject* who, bool down) {
                // One GJBaseGameLayer::handleButton per input, not per copy:
                // a dual level's two copies take the same press, and the game
                // hands that press to the touch listeners once.
                if (worldStep && ownInput) world::onButton(run, who, down);
            };
            for (uint8_t k = 0; k < ti.presses; k++) {
                if (d) {
                    releaseJump(p);
                    button(p, false);
                }
                pressJump(p);
                button(p, true);
                d = true;
            }
            if (d != ti.held) {
                if (ti.held) {
                    pressJump(p);
                    button(p, true);
                } else {
                    releaseJump(p);
                    button(p, false);
                }
            }
        };
        const TickInput in2 = separate ? otherAt(idx) : in;
        if (worldStep) {
            // GJBaseGameLayer::update's order (world/step.cpp (h)): the queued
            // spawns and the parked speed; processCommands - the command index,
            // then the tick's input; then the lock inputs and the move step.
            const float dt = world::stepDt();
            run.walks = world::walkModelled(run);
            // What the run's touch listeners are told about (world::onButton):
            // the copy of player 1 having died in this run, not the real
            // player's m_isDead, which the copies never write.
            run.player1Dead = t->m_deadP1;
            world::stepTickBegin(run, dt, players, count);
            pl->m_gameState.m_commandIndex += 2;  // processCommands 0x239cf0 (world/step.cpp (a))
            // Before the input, not after it: processQueuedButtons runs inside
            // processCommands, past the index, so what a touch trigger toggles
            // this tick is stamped with the tick's own index.
            ws.commandIndex = pl->m_gameState.m_commandIndex;
            apply(players[0], in, down, true);
            // The other copy's presses are an input of their own only when the
            // run drives it from a script of its own.
            if (count == 2) apply(players[1], in2, separate ? downOther : down, separate);
            world::stepTick(run, dt);
        } else {
            // GJBaseGameLayer::update (0x140238016-0x140238064) applies a queued speed change once,
            // at the top of the tick, to player 1 and - in the dual part - player 2.
            world::applyParkedSpeed(pl, players, count);
            apply(players[0], in, down, false);
            if (count == 2) apply(players[1], in2, separate ? downOther : down, false);
        }
        down = in.held;
        if (separate) downOther = in2.held;
        mv.nearX[0] = players[0]->getPositionX();
        mv.nearX[1] = count == 2 ? players[1]->getPositionX() : mv.nearX[0];
        mv.step(Bot::get()->updater().getPhysicsDt());
        static float colors[4] = {0, 0, 0, 0};
        bool deadOwn = false;
        bool deadOther = false;
        if (!worldStep) pl->m_gameState.m_commandIndex += 2;  // processCommands 0x239cf0 (world/step.cpp (a))
        // The clock, once for the tick (a run without the World moves it in each copy's iterate).
        if (worldOn) world::advanceClock(pl);
        float queuedMod = 0.0f;
        bool queuedMod2 = false;
        if (worldStep) {
            // The walk goes with the last copy whose tick runs past its update.
            run.walked = false;
            run.walkAfter = nullptr;
            for (int i = 0; i < count; i++) {
                const bool dead = (t->m_deadP1 && (masks[i] & TrajectoryMode::Player1) != 0) ||
                                  (t->m_deadP2 && (masks[i] & TrajectoryMode::Player2) != 0);
                if (!dead) run.walkAfter = players[i];
            }
        }
        for (int i = 0; i < count; i++) {
            bool held = false;
            int stepCount = 0;
            // The objects this copy can meet on the tick stand where the run's
            // log has them (materializeNear), before its update and collision
            // pass; a dead copy's tick reads none.
            if (worldObjects && !((t->m_deadP1 && (masks[i] & TrajectoryMode::Player1) != 0) ||
                                  (t->m_deadP2 && (masks[i] & TrajectoryMode::Player2) != 0))) {
                objects.materializeNear(run, players[i]);
            }
            // iterate() only reports a death on the call after the one that
            // hit; a tick that hits something is not a survived tick.
            if (t->iterate(pl, players[i], masks[i], colors, held, stepCount, config)) {
                if (i == 0) deadOwn = true;
                else deadOther = true;
            }
            // A speed portal this copy touched belongs to the top of the next tick
            // (above); the other copy's iterate() must not take it this tick.
            if (pl->m_gameState.m_timeModRelated != 0.0f) {
                queuedMod = pl->m_gameState.m_timeModRelated;
                queuedMod2 = pl->m_gameState.m_timeModRelated2;
                pl->m_gameState.m_timeModRelated = 0;
                pl->m_gameState.m_timeModRelated2 = false;
            }
        }
        if (queuedMod != 0.0f) {  // put back so snapshot() keeps it and the next step applies it to every copy
            pl->m_gameState.m_timeModRelated = queuedMod;
            pl->m_gameState.m_timeModRelated2 = queuedMod2;
        }
        // The spawn walk, when no copy's tick got as far as it (world/fire.cpp (w)).
        if (worldStep && run.walks && !run.walked) world::checkSpawnObjects(run);
        // GJBaseGameLayer::update 0x238990: forget the pads, portals and
        // triggers no copy touched on this tick (multi-activate contact rule).
        pl->m_gameState.processStateTriggers();
        // ... and the same for the copies' own contacts, which the World keeps
        // out of the live map.
        if (worldStep) world::endTick(run);
        else if (worldOn) world::ageContacts(ws, pl->m_gameState.m_commandIndex);
        if ((t->m_deadP1 && masks[0] == TrajectoryMode::Player1) || (t->m_deadP2 && masks[0] == TrajectoryMode::Player2)) deadOwn = true;
        if (count == 2 && ((t->m_deadP1 && masks[1] == TrajectoryMode::Player1) || (t->m_deadP2 && masks[1] == TrajectoryMode::Player2)))
            deadOther = true;
        if (deadOther && !otherDead) {
            otherDead = true;
            otherSurvived = (int)idx;
        }
        simulated++;
        ticks++;
        if (!separate) return deadOwn || deadOther;  // one input for both: either death ends it
        return deadOwn || (eitherDeath && deadOther);
    }

    bool deadPlayer2() const { return count == 2 && t->m_deadP2 && !t->m_deadP1; }
    bool complete() const { return levelEnd > 0.0f && players[0]->getPositionX() >= levelEnd; }
    cocos2d::CCPoint pos(int i = 0) const { return players[i]->getPosition(); }

    // The copies move through their node positions. A copy's m_position is
    // only written when it is set up (the game itself writes it after a
    // collision pass the simulation does not run), so the node position is
    // what a snapshot keeps; taking m_position here would put the copy back
    // at the start of the run on every restore.
    void snapshot(SimSnapshot& out) const {
        for (int i = 0; i < count; i++) {
            PlayerObject* p = players[i];
            SimPlayerSnap& s = out.p[i];
            s.player = SavedPlayerCheckpoint::create(p);  // GucciBot's checkpoint has no in-place capture  // into the storage the last snapshot here left, not a fresh checkpoint
            s.position = p->getPosition();
            s.rotation = p->getRotation();
            s.scaleX = p->getScaleX();
            s.scaleY = p->getScaleY();
            s.spriteWidthScale = p->m_spriteWidthScale;
            s.spriteHeightScale = p->m_spriteHeightScale;
            if (worldStep) s.lastPosition = p->m_lastPosition;
            // (the buttons are in s.player, and apply() puts them back)
        }
        out.totalTime = pl->m_gameState.m_totalTime;
        out.unkDouble3 = pl->m_gameState.m_unkDouble3;
        out.currentProgress = pl->m_gameState.m_currentProgress;
        out.unkUint5 = pl->m_gameState.m_unkUint5;
        out.portal.grab(pl);
        out.timeMod = pl->m_gameState.m_timeModRelated;
        out.timeMod2 = pl->m_gameState.m_timeModRelated2;
        out.commandIndex = pl->m_gameState.m_commandIndex;
        out.activatedIDs = pl->m_gameState.m_activatedObjectIDs;
        out.currentChannel = pl->m_gameState.m_currentChannel;
        out.spawnChannel1 = pl->m_gameState.m_spawnChannelRelated1;
        out.spawnChannel0 = pl->m_gameState.m_spawnChannelRelated0;
        out.dualMode = pl->m_gameState.m_isDualMode;
        out.count = count;
        for (int i = 0; i < 2; i++) {
            out.order[i] = players[i];
            out.masks[i] = masks[i];
        }
        out.separate = separate;
        out.simBroken = t->m_simBroken;
        out.teleportRand = t->m_teleportRand;
        out.activated1 = t->m_activatedObjectsP1;
        out.activated2 = t->m_activatedObjectsP2;
        for (int i = 0; i < 2; i++) {
            out.ringTouch[i] = t->m_ringTouch[i];
            out.ringUsed[i] = t->m_ringUsed[i];
        }
        out.actions = t->m_actions;
        out.dead1 = t->m_deadP1;
        out.dead2 = t->m_deadP2;
        out.down = down;
        out.downOther = downOther;
        out.otherDead = otherDead;
        out.otherSurvived = otherSurvived;
        out.movingAt = mv.mark();
        out.world = ws;
        if (worldStep) out.log = log;
        out.ticks = ticks;
    }

    void restore(SimSnapshot& in) {
        // How many copies the tick being put back had, and which player each
        // one stood for: a dual or solo portal the abandoned branch crossed
        // moved both (enterDual / leaveDual). A copy the branch had and this
        // tick does not is hidden again; its own state is put back the next
        // time a portal brings it in, from the copy that crosses it.
        if (in.count > 0) {
            for (int i = in.count; i < count; i++) {
                if (players[i]) players[i]->setVisible(false);
            }
            for (int i = 0; i < 2; i++) {
                players[i] = in.order[i];
                masks[i] = in.masks[i];
            }
            count = in.count;
            separate = in.separate;
            syncRunPlayers();
        }
        for (int i = 0; i < count; i++) {
            PlayerObject* p = players[i];
            SimPlayerSnap& s = in.p[i];
            t->setFakeMode(p, modeOf(s.player));
            s.player.apply(p);
            // (apply put the node at the copy's stale m_position; the node
            // position is the one that counts, and m_position stays what a
            // plain run leaves it, so a branched path and a straight one
            // start from the same state.)
            p->setPosition(s.position);
            p->setRotation(s.rotation);
            if (p->getScaleX() != s.scaleX) p->setScaleX(s.scaleX);
            if (p->getScaleY() != s.scaleY) p->setScaleY(s.scaleY);
            p->m_spriteWidthScale = s.spriteWidthScale;
            p->m_spriteHeightScale = s.spriteHeightScale;
            if (worldStep) p->m_lastPosition = s.lastPosition;
        }
        pl->m_gameState.m_totalTime = in.totalTime;
        pl->m_gameState.m_unkDouble3 = in.unkDouble3;
        pl->m_gameState.m_currentProgress = in.currentProgress;
        pl->m_gameState.m_unkUint5 = in.unkUint5;
        in.portal.put(pl);
        pl->m_gameState.m_timeModRelated = in.timeMod;
        pl->m_gameState.m_timeModRelated2 = in.timeMod2;
        pl->m_gameState.m_commandIndex = in.commandIndex;
        pl->m_gameState.m_activatedObjectIDs = in.activatedIDs;
        pl->m_gameState.m_currentChannel = in.currentChannel;
        pl->m_gameState.m_spawnChannelRelated1 = in.spawnChannel1;
        pl->m_gameState.m_spawnChannelRelated0 = in.spawnChannel0;
        pl->m_gameState.m_isDualMode = in.dualMode;
        t->m_simBroken = in.simBroken;  // hidden again only inside the next tick
        t->m_teleportRand = in.teleportRand;
        t->m_activatedObjectsP1 = in.activated1;
        t->m_activatedObjectsP2 = in.activated2;
        for (int i = 0; i < 2; i++) {
            t->m_ringTouch[i] = in.ringTouch[i];
            t->m_ringUsed[i] = in.ringUsed[i];
        }
        t->m_actions = in.actions;
        t->m_deadP1 = in.dead1;
        t->m_deadP2 = in.dead2;
        down = in.down;
        downOther = in.downOther;
        otherDead = in.otherDead;
        otherSurvived = in.otherSurvived;
        ws = in.world;
        if (worldStep) log = in.log;  // a new key: nothing the abandoned branch appended is taken for this log's
        ticks = in.ticks;
        // What the abandoned branch wrote stays until the next materializeNear
        // looks at it again, before any collision pass reads it.
        if (worldObjects) objects.invalidate();
        mv.nearX[0] = players[0]->getPositionX();
        mv.nearX[1] = count == 2 ? players[1]->getPositionX() : mv.nearX[0];
        mv.rewind(in.movingAt);
    }

    void fill(RunResult& r) const {
        const cocos2d::CCPoint p = pos();
        r.x = p.x;
        r.y = p.y;
        r.goingLeft = players[0]->m_isGoingLeft;
        r.minY = std::min(r.minY, p.y);
        r.maxY = std::max(r.maxY, p.y);
        r.otherDied = otherDead;
        r.otherSurvived = otherSurvived;
    }

    void end() {
        for (int i = 0; i < count; i++) players[i]->setVisible(false);
        t->deactivateAllRemembered();
        t->clearRingContacts();
        t->m_actions.clear();
        // World::restoreWritten: every object field and bucket the run wrote.
        objects.end();
        mv.end();
        t->m_simBroken.clear();
        t->m_simRealTick = -1;  // no run's tick to measure a phantom spot against
        pl->m_gameState = std::move(savedState);  // never read again after this
        // A copy the run picked up at a dual portal goes out of sight with it.
        if (t->m_fakePlayer1) t->m_fakePlayer1->setVisible(false);
        if (t->m_fakePlayer2) t->m_fakePlayer2->setVisible(false);
    }
};

void Trajectory::dualPortal(GJBaseGameLayer* pl, PlayerObject* copy, EffectGameObject* portal, bool toDual) {
    Sim* s = Sim::current;
    if (!s || s->pl != pl || !isFakePlayer(copy)) return;
    if (toDual) s->enterDual(copy, portal);
    else s->leaveDual(copy, portal);
}

bool Trajectory::simulatesBoth(GJBaseGameLayer* pl, bool p1) const {
    (void)p1;
    if (!pl || !pl->m_player1 || !pl->m_player2) return false;
    return pl->m_gameState.m_isDualMode;
}

bool Trajectory::separateControls(GJBaseGameLayer* pl) const {
    return pl && pl->m_gameState.m_isDualMode && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
}

// The copy is sitting on an orb the game re-fires on every new press (a black
// orb and friends - m_isMultiActivate; see physics/player.cpp:208). The ring
// is taken out of m_touchingRings when it fires (physics/player.cpp:257) and
// put back by the next tick's collision pass (physics/collisions.cpp:542), so
// the answer is false on the tick after a press: ask it to start a spam, not
// to decide whether one carries on (that is what spamPeriod remembers).
static bool onMultiActivateRing(PlayerObject* p) {
    if (!p || !p->m_touchingRings) return false;
    const unsigned n = p->m_touchingRings->count();
    for (unsigned i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(p->m_touchingRings->objectAtIndex(i));
        auto* e = geode::cast::typeinfo_cast<EnhancedGameObject*>(o);
        if (e && e->m_isMultiActivate) return true;
    }
    return false;
}

// The kinds a hold gives at most one impulse to: the UFO flaps once per
// press, the ball and the spider flip once. Riding a multi-activate orb in
// one of these means pressing again and again - hold and release alone
// cannot do it, however the look-ahead is steered.
static bool spamsOrbs(const PlayerObject* p) {
    return p && (p->m_isBird || p->m_isBall || p->m_isSpider);
}

// Ticks per press worth trying for that spam, in the order they are worth
// trying. The unit measured on the 1000 TPS corridor is a press every other
// tick, so the raw 2 comes first whatever the tick rate; the rate-scaled
// ones follow, for an orb that wants a fire rate rather than a tick (at 240
// TPS they collapse back to 2, 3 and 4).
static int orbSpamCadences(int out[4]) {
    const double scale = std::max(1.0, Bot::get()->updater().getTps() / 240.0);
    const double wanted[4] = {2.0, 2.0 * scale, 3.0 * scale, 4.0 * scale};
    int n = 0;
    for (const double w : wanted) {
        const int p = std::max(2, (int)std::round(w));
        bool seen = false;
        for (int i = 0; i < n; i++) seen = seen || out[i] == p;
        if (!seen) out[n++] = p;
    }
    return n;
}

// One tick of a spam of that cadence, `since` ticks after it started: the
// unit is a press, a release and a press inside one tick, let go on the next,
// then quiet until the cadence comes round again.
static TickInput orbSpamTick(int since, int period) {
    return (period > 0 && since % period == 0) ? TickInput{2, true} : TickInput{0, false};
}

RunResult Trajectory::steer(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int lookahead,
                            std::vector<TickInput>& script, std::span<const TickInput> other, bool otherDown) {
    RunResult result;
    script.clear();
    if (ticks <= 0) return result;
    Sim s;
    if (!s.begin(this, pl, p1, buttonDown, other, otherDown, false)) return result;
    lookahead = std::max(1, lookahead);

    int guaranteed = 0;  // ticks the current input is known to survive
    // When nothing lasts the whole look-ahead the choice is kept until it is
    // this close to hitting something (a quarter of the look-ahead: the
    // long look-aheads switch early, the short ones late).
    const int margin = std::max(1, lookahead / 4);
    bool committed = false;  // riding a choice that does not last the look-ahead
    // Riding the orb spam (see probeSpam below): the tick it started at and
    // its cadence in ticks per press. spamPeriod == 0 is the flag for "not
    // spamming" - spamFrom 0 is a real start, the first tick of the look.
    int spamFrom = 0;
    int spamPeriod = 0;
    result.minY = result.maxY = s.players[0]->getPositionY();
    script.reserve((size_t)ticks);

    // How many of `n` ticks the player survives when `first` is applied now
    // and its held state kept afterwards; the state is put back after.
    // With `keepAt` > 0 the state `keepAt` ticks in is kept as well (see its use).
    SimSnapshot before;
    SimSnapshot halfway;
    int halfwayAt = 0;  // 0: the last probe kept no state
    float halfwayMinY = 0.0f, halfwayMaxY = 0.0f;
    auto probe = [&](const TickInput& first, int n, int keepAt = 0) {
        s.snapshot(before);
        halfwayAt = 0;
        float lo = result.minY, hi = result.maxY;
        int survived = 0;
        for (int i = 0; i < n; i++) {
            const TickInput in = i == 0 ? first : TickInput{0, first.held};
            if (s.step(in)) break;
            survived++;
            if (s.complete()) {
                survived = n;
                halfwayAt = 0;  // the level ends inside the look: no shortcut
                break;
            }
            if (survived <= keepAt) {
                const float y = s.pos().y;
                lo = std::min(lo, y);
                hi = std::max(hi, y);
                if (survived == keepAt) {
                    s.snapshot(halfway);
                    halfwayAt = keepAt;
                    halfwayMinY = lo;
                    halfwayMaxY = hi;
                }
            }
        }
        s.restore(before);
        return survived;
    };

    // The same for the orb spam at a cadence: the unit pressed over and over
    // from here, not one unit and then its held state. A corridor ridden on a
    // multi-activate orb only survives while the spam keeps going, so one
    // unit scored on its own would always look like a death - which is why
    // steering has never been able to take it.
    auto probeSpam = [&](int period, int n) {
        s.snapshot(before);
        int survived = 0;
        for (int i = 0; i < n; i++) {
            if (s.step(orbSpamTick(i, period))) break;
            survived++;
            if (s.complete()) {
                survived = n;
                break;
            }
        }
        s.restore(before);
        return survived;
    };

    for (int i = 0; i < ticks; i++) {
        // The copy the run is driven by, read again every tick: a solo portal
        // a run crosses takes one of the two copies out of the level and hands
        // the run to the other (Sim::leaveDual), so holding on to the one the
        // run began with would ask a copy that has left what kind it is.
        PlayerObject* const player = s.players[0];
        // Riding the orb spam: its own ticks, not "carry on". Carrying on is
        // a hold, and a hold is one flap and one shot of the orb - the ticks
        // between the decision points are what keeps the corridor going.
        TickInput chosen = spamPeriod > 0 ? orbSpamTick(i - spamFrom, spamPeriod) : TickInput{0, s.down};
        // The cube kinds jump with a tap; the flying kinds steer with hold
        // and release, where a tap is just a tiny nudge.
        const bool flying = player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;
        if (guaranteed <= (committed ? 0 : lookahead / 2)) {
            // Asking again: the spam is one of the answers below, not the
            // default, so it stops here unless it wins again. A spam that is
            // already running stands in for the ring probe: the ring is out
            // of m_touchingRings on the tick after a press, and the re-ask
            // lands on exactly such a tick for half the look-aheads.
            const int wasSpamming = spamPeriod;
            spamPeriod = 0;
            chosen = TickInput{0, s.down};
            // Keeping the input for the whole look is what most asks come to, and
            // then the next lookahead - lookahead / 2 ticks step exactly that input
            // again: the probe's own state that far in is taken instead.
            const int ahead = lookahead - lookahead / 2;
            const int keep = probe(chosen, lookahead, (ahead >= 8 && i + ahead <= ticks) ? ahead : 0);
            committed = false;
            if (keep >= lookahead && halfwayAt > 0) {
                s.restore(halfway);
                for (int k = 0; k < halfwayAt; k++) script.push_back(chosen);
                result.minY = halfwayMinY;
                result.maxY = halfwayMaxY;
                s.fill(result);
                i += halfwayAt - 1;
                result.survived = i + 1;
                guaranteed = lookahead - halfwayAt;
                continue;
            }
            if (keep >= lookahead) {
                guaranteed = lookahead;
            } else {
                // Keeping this input dies soon: switch, or tap.
                const TickInput toggle = s.down ? TickInput{0, false} : TickInput{1, true};
                const int switched = probe(toggle, lookahead);
                int best = keep;
                if (switched > best || (switched == best && switched >= lookahead)) {
                    chosen = toggle;
                    best = switched;
                }
                if (!flying && best < lookahead) {
                    const TickInput tap{1, false};
                    const int tapped = probe(tap, lookahead);
                    if (tapped > best) {
                        chosen = tap;
                        best = tapped;
                    }
                }
                // Sitting on an orb that fires again on every press, in a
                // kind a hold gives one impulse to (UFO, ball, spider): the
                // spam, at each cadence, scored as the repeating thing it is.
                // Hold and release cannot make the press-release-press unit,
                // so without this a black orb corridor is a corridor no steer
                // can ride - and the steers are what solve long corridors.
                if (best < lookahead && spamsOrbs(player) && (wasSpamming > 0 || onMultiActivateRing(player))) {
                    int cadences[4];
                    const int nc = orbSpamCadences(cadences);
                    for (int ci = 0; ci < nc && best < lookahead; ci++) {
                        const int period = cadences[ci];
                        const int spammed = probeSpam(period, lookahead);
                        if (spammed > best) {
                            chosen = orbSpamTick(0, period);
                            best = spammed;
                            spamPeriod = period;
                            spamFrom = i;
                        }
                    }
                }
                if (best >= lookahead) {
                    guaranteed = lookahead;
                } else {
                    // Nothing lasts the whole look-ahead (a corridor narrower
                    // than it, a spike further on): ride the choice until it
                    // is about to hit something instead of asking again next
                    // tick. Asking every tick is what made a wave hover on
                    // the spot - a switch every tick - through any corridor
                    // narrower than its look-ahead.
                    // (In a spot tighter than the margin itself, half of
                    // what is left: still a few ticks between switches.)
                    guaranteed = std::max(0, best - std::min(margin, std::max(1, best / 2)));
                    committed = true;
                }
            }
        }
        const bool dead = s.step(chosen);
        script.push_back(chosen);
        guaranteed = std::max(0, guaranteed - 1);
        s.fill(result);
        if (dead) {
            result.died = true;
            result.dualDeath = s.deadPlayer2();
            break;
        }
        result.survived = i + 1;
        if (s.complete()) {
            result.complete = true;
            break;
        }
    }
    result.simulated = s.simulated;
    s.end();
    return result;
}

// The inputs worth trying instead of "carry on" at a branch point, for the
// kind of player the copy is right now. An alternative may span two ticks:
// the orb spam unit is a press, a release and a press inside one tick, let
// go on the next. With a cadence it does not stop there - the way forward
// keeps pressing at that cadence (see SearchState::forwardTick). One unit
// per branch meant one stack frame per two ticks of corridor: about 150 of
// them before the budget ran out, a quarter of the 585 a 390 unit black orb
// corridor takes at 1000 TPS.
struct Alternative {
    TickInput first;
    bool thenRelease = false;
    int spamPeriod = 0;  // >0: keep pressing every so many ticks from here on
};

static int alternativesFor(PlayerObject* p, bool down, Alternative out[8], bool ringHidden) {
    int n = 0;
    const bool onRing = p->m_touchingRings && p->m_touchingRings->count() > 0;
    // On an orb that fires again on every press, in a kind a hold gives one
    // impulse to: the sustained spam at each cadence, tried first. The single
    // unit and the tap are still behind it for a one-off drop.
    // The ring is out of m_touchingRings on the tick after a press and only
    // goes back in the next collision pass, so onMultiActivateRing cannot see
    // it at a branch that lands there: `ringHidden` stands in for the probe.
    int cadences[4];
    const int nc = (spamsOrbs(p) && (ringHidden || (onRing && onMultiActivateRing(p)))) ? orbSpamCadences(cadences) : 0;
    for (int i = 0; i < nc; i++) out[n++] = {TickInput{2, true}, true, cadences[i]};
    if (p->m_isBird) {
        // The UFO flaps once per press.
        out[n++] = {TickInput{1, false}};
        if (onRing) out[n++] = {TickInput{2, true}, true};
        return n;
    }
    if (p->m_isShip || p->m_isDart || p->m_isSwing) {
        out[n++] = {down ? TickInput{0, false} : TickInput{1, true}};
        // Held on an orb: letting go never fires it; a fresh press that keeps
        // the hold (release and press inside the tick) does.
        if (down && onRing) out[n++] = {TickInput{1, true}};
        return n;
    }
    if (down) {
        out[n++] = {TickInput{0, false}};
        if (onRing) out[n++] = {TickInput{1, true}};  // the same re-press for the cube, robot, ball and spider
        return n;
    }
    out[n++] = {TickInput{1, false}};  // a tap
    if (p->m_isRobot || !(p->m_isBall || p->m_isSpider)) {
        out[n++] = {TickInput{1, true}};  // a hold (the robot jumps higher, the cube jumps again on landing)
    }
    if (onRing) out[n++] = {TickInput{2, true}, true};
    return n;
}

// The search, kept between slices (see beginSearch / stepSearch).
struct Trajectory::SearchState {
    Sim s;
    int ticks = 0;
    int budget = 0;
    int maxBack = 128;
    int tickScale = 1;  // ticks per 240-TPS tick: the branch offsets step in those
    int fine = 8;       // 240-TPS ticks before a death where every tick is a branch point (more at a widened spot)
    static constexpr int kSnapEvery = 4;  // a snapshot every few ticks; the rest is re-run

    std::vector<TickInput> path;      // the path so far
    std::vector<SimSnapshot> snaps;   // every kSnapEvery ticks along it
    int pathLength = 0;               // ticks of `path` that are valid
    int bestLength = -1;
    bool bestComplete = false;
    std::vector<TickInput> best;
    bool startDown = false;
    std::vector<TickInput> base;  // followed on the way forward instead of "carry on" (the last plan's verified tail)
    // Per tick of `path`: a press on that tick lands on a ring. Laid down as
    // the path is (noteRing), so the offset walk can tell without a rewind.
    std::vector<uint8_t> onRing;

    bool onRingAt(int at) const {
        return at >= 0 && at < (int)onRing.size() && onRing[(size_t)at] != 0;
    }
    // The copy's ring set after the tick just stepped: a press on the tick
    // after it is the press that would fire the orb.
    void noteRing(int at) {
        if (at < 0 || at >= (int)onRing.size()) return;
        PlayerObject* p = s.players[0];
        onRing[(size_t)at] = (p && p->m_touchingRings && p->m_touchingRings->count() > 0) ? 1u : 0u;
    }

    struct Frame {
        int deathTick;   // the tick that died with "carry on"
        int offset;      // branch at deathTick - offset (grows)
        int alt;         // next alternative at that tick
        int branchTick;  // where this frame branched last (its children may not branch before it)
        int altCount = -1;  // alternatives the first rewind at this offset counted (-1: none yet)
    };
    std::vector<Frame> stack;

    enum Stage { Forward, Branch, Over };
    Stage stage = Forward;
    int forwardIndex = 0;
    // The orb spam a branch started (see alternativesFor): the tick it began
    // at and its cadence in ticks per press, carried by the way forward.
    // spamPeriod == 0 is "not spamming"; a branch settles both.
    int spamFrom = 0;
    int spamPeriod = 0;
    RunResult result;
    SimSnapshot resume;   // the state at the end of a slice
    bool paused = false;

    void noteBest(int length) {
        if (length > bestLength) {
            bestLength = length;
            best.assign(path.begin(), path.begin() + length);
        }
    }

    void takeSnapshotAt(int i) {
        if (i % kSnapEvery == 0) s.snapshot(snaps[(size_t)(i / kSnapEvery)]);
    }

    // Puts the state back at the start of tick `t` (the nearest snapshot,
    // then the path's own inputs up to t).
    void rewindTo(int t) {
        const int k = t / kSnapEvery;
        s.restore(snaps[(size_t)k]);
        for (int i = k * kSnapEvery; i < t; i++) (void)s.step(path[(size_t)i]);
    }

    void finish(int lengthNow) {
        noteBest(lengthNow);
        stage = Over;
    }

    // One forward tick of "carry on".
    void forwardTick() {
        const int i = forwardIndex;
        if (i >= ticks) {
            finish(pathLength);
            return;
        }
        takeSnapshotAt(i);
        // Riding a spam a branch started: its own ticks. Otherwise the last
        // plan's verified tail where there is one, and "carry on" after that.
        const TickInput in = spamPeriod > 0          ? orbSpamTick(i - spamFrom, spamPeriod)
                             : i < (int)base.size() ? base[(size_t)i]
                                                    : TickInput{0, s.down};
        path[(size_t)i] = in;
        pathLength = i + 1;
        const bool dead = s.step(in);
        noteRing(i + 1);
        s.fill(result);
        if (dead) {
            if (stack.empty()) result.dualDeath = s.deadPlayer2();
            noteBest(i);
            stack.push_back(Frame{i, 1, 0, -1});
            stage = Branch;
            return;
        }
        if (s.complete()) {
            bestComplete = true;
            finish(pathLength);
            return;
        }
        if (s.simulated >= budget) {
            finish(pathLength);
            return;
        }
        forwardIndex = i + 1;
    }

    // One branch attempt (or bookkeeping towards the next one).
    void branchStep() {
        if (stack.empty()) {
            stage = Over;
            return;
        }
        Frame& f = stack.back();
        const int minTick = stack.size() >= 2 ? stack[stack.size() - 2].branchTick + 1 : 0;
        if (f.offset > maxBack || f.deathTick - f.offset < minTick) {
            stack.pop_back();
            return;
        }
        // Every tick for the last few before the death, then growing steps:
        // a branch that has to be exact is close to the death, a far one is
        // a matter of roughly when.
        // The thresholds and the steps are 240-TPS ticks. At 1000 TPS the
        // coarse part of the walk moved 32 ticks (32 ms) at a time across a
        // maxBack of 512, which is half as many milliseconds per branch point
        // as at 240 TPS and nearly twice as many points out of the budget.
        const int u = tickScale;
        // Finely while the copy is on a ring (see the stride further down). The way past an orb is a
        // press on one exact tick; the walk below steps 8 ticks at a time at
        // 2000 TPS even in its fine part, so of the 240 ticks a contact lasts
        // there it tried four. The flags come from the forward run, so this
        // costs no rewind to find out.
        const bool ringHere = onRingAt(f.deathTick - f.offset);
        if (!ringHere && f.offset > fine * u && f.alt == 0) {
            const int step = (f.offset > 64 * u ? 32 : f.offset > 32 * u ? 16 : f.offset > 16 * u ? 8 : 4) * u;
            const int rem = f.offset % step;
            if (rem != 0) {
                int want = f.offset + (step - rem);
                // ... and the coarse walk never steps over a contact: it stops
                // at the first tick of one it would have jumped past.
                for (int o = f.offset + 1; o <= want; o++) {
                    if (onRingAt(f.deathTick - o)) {
                        want = o;
                        break;
                    }
                }
                f.offset = want;
                return;
            }
        }
        const int t = f.deathTick - f.offset;
        // Every alternative at this offset tried: on to the next offset without a
        // rewind first. The list at a tick can only shrink once one of its
        // alternatives has run (that branch sets spamFrom to this tick, which can
        // only turn ringHidden off; nothing else it reads changes), so the count
        // the first rewind here made is the most there can be.
        if (f.altCount >= 0 && f.alt >= f.altCount) {
            f.alt = 0;
            f.altCount = -1;
            f.offset += ringHere ? (f.offset <= fine * u ? 1 : (std::max(1, u / 2) | 1)) : (f.offset < fine * u ? u : 1);  // the stride below
            return;
        }
        rewindTo(t);
        Alternative alts[8];
        const bool ringHidden = (spamPeriod > 0 && t > spamFrom) || (t > 0 && path[(size_t)(t - 1)].presses > 0);
        const int n = alternativesFor(s.players[0], s.down, alts, ringHidden);
        if (f.altCount < 0) f.altCount = n;
        if (f.alt >= n) {
            f.alt = 0;
            f.altCount = -1;
            // The fine part of the walk (every tick for the last few before
            // the death at 240 TPS) steps one 240-TPS tick, not one tick of a
            // rate four times as fast. On a ring: about half a 240-TPS tick, and
            // odd, so a two-tick orb spam still gets starts on both parities (1
            // tick below 960 TPS, 3 at 1000, 5 at 2000). A single tick there spent
            // the whole budget of a repair stop inside the nearest contact at
            // 2000 TPS, 420-odd ticks each with its alternatives and their runs.
            // That saving is kept only further back: within fine*u ticks of the
            // death (a zone that grows as the spot widens) every ring tick is
            // branched, since an orb's one-tick press window there is the way past.
            f.offset += ringHere ? (f.offset <= fine * u ? 1 : (std::max(1, u / 2) | 1)) : (f.offset < fine * u ? u : 1);
            return;
        }
        const Alternative alt = alts[f.alt++];
        // The branch settles what the way forward does from here: a spam
        // alternative keeps pressing at its cadence, and any other one stops
        // a spam an earlier branch started (the ticks it already wrote stay
        // in the path - "spam this far, then something else").
        spamPeriod = alt.spamPeriod;
        spamFrom = t;
        f.branchTick = t;
        result.branches++;
        path[(size_t)t] = alt.first;
        pathLength = t + 1;
        takeSnapshotAt(t);
        bool dead = s.step(alt.first);
        noteRing(t + 1);
        s.fill(result);
        int next = t + 1;
        if (!dead && alt.thenRelease && next < ticks && !s.complete()) {
            // The second tick of the unit: let go.
            const TickInput rel{0, false};
            path[(size_t)next] = rel;
            takeSnapshotAt(next);
            dead = s.step(rel);
            noteRing(next + 1);
            s.fill(result);
            pathLength = next + 1;
            next++;
        }
        if (dead) {
            noteBest(pathLength - 1);
            if (s.simulated >= budget) stage = Over;
            return;  // that alternative dies at once: the next one
        }
        if (s.simulated >= budget) {
            finish(pathLength);
            return;
        }
        forwardIndex = next;
        stage = Forward;
    }
};

void Trajectory::dropSearch() {
    delete m_search;
    m_search = nullptr;
}

bool Trajectory::beginSearch(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget,
                             std::span<const TickInput> other, bool otherDown, std::span<const TickInput> base, int widen) {
    dropSearch();
    if (ticks <= 0) return false;
    auto* st = new SearchState();
    if (!st->s.begin(this, pl, p1, buttonDown, other, otherDown, false)) {
        delete st;
        return false;
    }
    st->ticks = ticks;
    st->budget = std::max(budget, ticks * 2);
    st->startDown = buttonDown;
    st->base.assign(base.begin(), base.end());
    const double tps = Bot::get()->updater().getTps();
    const int scale = (int)std::max(1.0, std::round(tps / 240.0));
    st->maxBack = 128 * scale * (1 + std::clamp(widen, 0, 3));  // how far before a death a branch may start (further at a widened spot)
    st->fine = 8 * (1 + std::clamp(widen, 0, 3));               // every tick a branch point this close to a death
    st->tickScale = scale;      // ... and the offsets it steps through are 240-TPS ticks
    st->path.assign((size_t)ticks, TickInput{});
    st->onRing.assign((size_t)ticks + 1, 0);
    st->snaps.resize((size_t)(ticks / SearchState::kSnapEvery + 2));
    st->result.minY = st->result.maxY = st->s.players[0]->getPositionY();
    st->noteRing(0);  // whether the copy starts a run already on an orb
    m_search = st;
    return true;
}

bool Trajectory::stepSearch(int maxTicks, std::chrono::steady_clock::time_point until) {
    SearchState* st = m_search;
    if (!st) return true;
    if (st->stage == SearchState::Over) return true;
    if (st->paused) {
        // Back where the last slice left off (the copies and the game state
        // may have been used for other things in between).
        st->s.savedState = st->s.pl->m_gameState;
        st->s.mv.resume();
        // The real game has stepped since the slice was suspended: what it moved
        // goes back where the run's table has it before the next collision pass.
        if (st->s.worldObjects) {
            st->s.objects.resume(st->s.savedState.m_currentProgress, st->s.savedState.m_commandIndex);
        }
        st->s.restore(st->resume);
        // The PlayerObject::update hook wrote the real tick's dt in between:
        // the step goes back to the paused run's own, under its time warp, so
        // a script lasts as long in every slice.
        m_delta = Bot::get()->updater().getPhysicsDt() * std::min(st->s.pl->m_gameState.m_timeWarp, 1.0f) * 60.0f;
        st->paused = false;
    }
    int start = st->s.simulated;
    for (;;) {
        while (st->stage != SearchState::Over && st->s.simulated - start < maxTicks) {
            if (st->stage == SearchState::Forward) st->forwardTick();
            else st->branchStep();
        }
        // Given a deadline, the clock is looked at every maxTicks and the search
        // goes on in place; it is paused (below) only once the time is up.
        if (st->stage == SearchState::Over || until == std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() >= until)
            break;
        start = st->s.simulated;
    }
    if (st->stage != SearchState::Over) {
        st->s.snapshot(st->resume);
        // The real game ticks before the next slice: everything the slice wrote
        // is put back (World::restoreWritten).
        st->s.objects.restoreWritten();
        st->s.mv.suspend();
        // Refilled from the game state on resume, and in finishSearch.
        st->s.pl->m_gameState = std::move(st->s.savedState);
        st->paused = true;
        return false;
    }
    return true;
}

RunResult Trajectory::finishSearch(std::vector<TickInput>& script) {
    script.clear();
    SearchState* st = m_search;
    if (!st) return {};
    if (st->paused) {
        st->s.savedState = st->s.pl->m_gameState;
        st->s.mv.resume();
        st->paused = false;
    }
    if (st->stage == SearchState::Forward) st->finish(st->pathLength);  // the last tick run survived
    st->stage = SearchState::Over;
    RunResult result = st->result;
    result.simulated = st->s.simulated;
    if (st->bestLength < 0) st->bestLength = 0;
    result.survived = st->bestLength;
    result.complete = st->bestComplete && st->bestLength == st->pathLength;
    result.died = !result.complete && st->bestLength < st->ticks;
    if (!result.died) result.dualDeath = false;
    script = st->best;
    if (script.empty()) script.push_back(TickInput{0, st->startDown});
    st->s.end();
    dropSearch();
    return result;
}

RunResult Trajectory::search(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget,
                             std::vector<TickInput>& script, std::span<const TickInput> other, bool otherDown) {
    script.clear();
    if (!beginSearch(pl, p1, ticks, buttonDown, budget, other, otherDown)) return {};
    while (!stepSearch(1 << 30)) {
    }
    return finishSearch(script);
}

Trajectory::HitboxCheck Trajectory::checkHitboxes(GJBaseGameLayer* pl, bool p1) {
    HitboxCheck out;
    if (!pl || !m_fakePlayer1 || !m_fakePlayer2) return out;
    PlayerObject* player = p1 ? m_fakePlayer1 : m_fakePlayer2;
    PlayerObject* realPlayer = p1 ? pl->m_player1 : pl->m_player2;
    if (!realPlayer) return out;
    GJGameState state = pl->m_gameState;
    player->copyAttributes(realPlayer);
    player->m_maybeReducedEffects = true;
    setFakeMode(player, modeOf(realPlayer));
    SavedPlayerCheckpoint checkpoint = SavedPlayerCheckpoint::create(realPlayer);
    checkpoint.apply(player);
    player->setPosition(realPlayer->m_position);
    player->setRotation(realPlayer->getRotation());
    if (player->getScaleX() != realPlayer->getScaleX()) player->setScaleX(realPlayer->getScaleX());
    if (player->getScaleY() != realPlayer->getScaleY()) player->setScaleY(realPlayer->getScaleY());
    syncButtons(player, realPlayer);
    out.real = realPlayer->getObjectRect().size;
    out.realInner = realPlayer->getObjectRect(0.3f, 0.3f).size;
    out.fake = player->getObjectRect().size;
    out.fakeInner = player->getObjectRect(0.3f, 0.3f).size;
    out.realScale = realPlayer->getScale();
    out.fakeScale = player->getScale();
    out.realDart = realPlayer->m_isDart;
    out.fakeDart = player->m_isDart;
    out.realSize = realPlayer->m_vehicleSize;
    out.fakeSize = player->m_vehicleSize;
    player->setVisible(false);
    pl->m_gameState = state;
    return out;
}

RunResult Trajectory::run(GJBaseGameLayer* pl, bool p1,
                          std::span<const TickInput> inputs, int ticks, bool buttonDown,
                          std::vector<TraceSample>* trace, std::span<const TickInput> other, bool otherDown,
                          bool eitherDeath) {
    RunResult result;
    if (ticks <= 0) return result;
    Sim s;
    if (!s.begin(this, pl, p1, buttonDown, other, otherDown, eitherDeath)) return result;
    result.minY = result.maxY = s.players[0]->getPositionY();
    for (int i = 0; i < ticks; i++) {
        const TickInput in = inputs.empty() ? TickInput{} : inputs[std::min<size_t>((size_t)i, inputs.size() - 1)];
        const bool dead = s.step(in);
        s.fill(result);
        if (trace) {
            PlayerObject* player = s.players[0];
            const cocos2d::CCPoint pos = player->getPosition();
            TraceSample sample;
            sample.tick = i + 1;
            sample.x = pos.x;
            sample.y = pos.y;
            sample.yVel = static_cast<float>(player->m_yVelocity);
            sample.xVel = static_cast<float>(player->m_platformerXVelocity);
            sample.rotation = player->getRotation();
            sample.onGround = player->m_isOnGround;
            sample.held = s.down;
            sample.dead = dead;
            if (s.count == 2) {
                sample.dual = true;
                const cocos2d::CCPoint pos2 = s.players[1]->getPosition();
                sample.x2 = pos2.x;
                sample.y2 = pos2.y;
            }
            trace->push_back(sample);
        }
        if (dead) {
            result.died = true;
            result.dualDeath = s.deadPlayer2();
            break;
        }
        result.survived = i + 1;
        if (s.complete()) {
            result.complete = true;
            break;
        }
    }
    result.simulated = s.simulated;
    s.end();
    return result;
}

// ------------------------------------------------ the branch check (design step 15)

namespace {

constexpr int kCheckTicks = 64;        // as far down the level as the check goes
constexpr int kCheckSnapEvery = 7;     // a snapshot, a branch and a restore this often
constexpr int kCheckBranchTicks = 5;   // ticks the thrown-away branch runs

uint64_t mixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0x100000001b3ull;
}
uint64_t mixFloat(uint64_t h, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));  // the bits, so a -0.0 is not a 0.0 and a NaN is itself
    return mixHash(h, bits);
}
uint64_t mixDouble(uint64_t h, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return mixHash(h, bits);
}

// The script the check runs: no input at all, so the copy gets as far down the
// level as the level lets it and the check covers as many ticks as it can.
TickInput checkScriptAt(int) { return TickInput{}; }
// What a branch does instead: presses, so it takes the orbs and pads it meets,
// fires their triggers and leaves the objects somewhere the straight run never
// has them. All of it is thrown away by the restore.
TickInput checkBranchAt(int i) {
    TickInput in;
    in.held = (i % 3) != 0;
    if (i % 2 == 0) in.presses = 1;
    return in;
}

}  // namespace

void Trajectory::checkWorldBranches(GJBaseGameLayer* pl) {
    if (m_branchCheckGen == moverCacheGeneration()) return;  // once per read of the level
    // Nothing to check while the World is off: every run is the old path then.
    if (world::World::disabled) return;
    // Not in the middle of somebody else's run: a paused search owns the
    // copies between its slices, and a start stands in for the players.
    if (m_search || m_start || m_simulating || m_drawing) return;
    if (!pl || !pl->m_objects || !pl->m_player1 || !m_fakePlayer1 || !m_fakePlayer2) return;
    if (pl->m_loadingProgress < 1.0f) return;  // the group arrays are not final yet
    if (pl->m_player1->m_isDead || pl->m_playerDied) return;
    // Only on a tick the game has just played forward. The other callers of
    // realStateChanged are a restore and a rewind, where the level is halfway
    // between two states and the copies have no business running.
    const uint64_t frame = Bot::get()->updater().getFrame();
    const bool stepped = m_branchCheckFrame != 0 && frame == m_branchCheckFrame + 1;
    m_branchCheckFrame = frame;
    if (!stepped) return;
    // Reading the level runs the check of what was read, which can turn the
    // World off; then there is nothing here to check either.
    if (!world::WorldDef::get(pl) || world::World::disabled) return;
    m_branchCheckGen = moverCacheGeneration();

    // The check's own runs must leave the trajectory as they found it: the
    // pathfinder reads the killer of the run it has just made, and the
    // teleport state is the real game's.
    const Killer keptKiller = m_killer;
    const uint64_t keptRand = m_teleportRand;

    // One tick of a run as a number: where the copies are, what the World
    // state holds, and where the objects the run's ops have moved stand.
    auto sample = [](Sim& s) {
        uint64_t h = 0xcbf29ce484222325ull;
        // How many copies the run has and which part of the level they are in:
        // a dual or solo portal a branch crossed moves both (enterDual), so a
        // restore that did not put them back would show up here.
        h = mixHash(h, (uint64_t)s.count);
        h = mixHash(h, s.pl->m_gameState.m_isDualMode ? 1u : 0u);
        for (int i = 0; i < s.count; i++) {
            PlayerObject* p = s.players[i];
            const cocos2d::CCPoint pos = p->getPosition();
            h = mixFloat(h, pos.x);
            h = mixFloat(h, pos.y);
            h = mixFloat(h, p->getRotation());
            h = mixDouble(h, p->m_yVelocity);
            h = mixHash(h, p->m_isOnGround ? 1u : 0u);
        }
        h = mixHash(h, s.ticks);
        h = mixHash(h, s.ws.tick);
        h = mixHash(h, s.ws.commandIndex);
        h = mixHash(h, s.ws.cmds->size());
        h = mixHash(h, s.ws.spawns->size());
        h = mixHash(h, s.ws.seed);
        h = mixHash(h, s.ws.uncertainGroups.size());
        h = mixHash(h, s.log.size());
        if (!s.worldStep || !s.def) return h;
        // The poses the materializer writes, worked out the way a run works
        // them out - through the cache, so an entry an abandoned branch left
        // behind shows up here.
        int taken = 0;
        for (const auto& entry : s.ws.reach) {
            for (const int slot : s.def->members((int)entry.first)) {
                const world::Pose& p = s.cache.pose(s.log, slot);
                h = mixDouble(h, p.x);
                h = mixDouble(h, p.y);
                h = mixFloat(h, p.lastX);
                h = mixFloat(h, p.lastY);
                h = mixHash(h, p.marker);
                h = mixFloat(h, p.rot);
                h = mixFloat(h, p.sx);
                h = mixFloat(h, p.sy);
                h = mixHash(h, (uint64_t)(uint32_t)p.counter);
                h = mixHash(h, (p.disabled ? 1u : 0u) | (p.uncertain ? 2u : 0u));
                if (++taken >= 128) return h;
            }
        }
        return h;
    };

    std::vector<uint64_t> straight;
    std::vector<uint64_t> branched;
    int straightSurvived = 0, branchedSurvived = 0;
    bool straightDied = false, branchedDied = false;
    bool worldStep = false;
    int branches = 0;
    world::WorldState straightWs, branchedWs;
    world::OpLog straightLog, branchedLog;

    {
        Sim s;
        if (!s.begin(this, pl, true, false)) {
            m_killer = keptKiller;
            m_teleportRand = keptRand;
            return;
        }
        worldStep = s.worldStep;
        for (int i = 0; i < kCheckTicks; i++) {
            const bool dead = s.step(checkScriptAt(i));
            straight.push_back(sample(s));
            if (dead) {
                straightDied = true;
                break;
            }
            straightSurvived = i + 1;
            if (s.complete()) break;
        }
        straightWs = s.ws;
        straightLog = s.log;
        s.end();
    }
    {
        Sim s;
        if (!s.begin(this, pl, true, false)) {
            m_killer = keptKiller;
            m_teleportRand = keptRand;
            return;
        }
        SimSnapshot snap;
        for (int i = 0; i < kCheckTicks; i++) {
            if (i % kCheckSnapEvery == 0) {
                // A branch of the kind the search tries and throws away: its
                // own inputs, its own triggers, its own ops, undone again.
                s.snapshot(snap);
                branches++;
                for (int k = 0; k < kCheckBranchTicks; k++) {
                    const bool dead = s.step(checkBranchAt(i + k));
                    (void)sample(s);  // a branch reads poses too: its cache entries must not leak
                    if (dead) break;
                }
                s.restore(snap);
            }
            const bool dead = s.step(checkScriptAt(i));
            branched.push_back(sample(s));
            if (dead) {
                branchedDied = true;
                break;
            }
            branchedSurvived = i + 1;
            if (s.complete()) break;
        }
        branchedWs = s.ws;
        branchedLog = s.log;
        s.end();
    }

    m_killer = keptKiller;
    m_teleportRand = keptRand;

    const char* bad = nullptr;
    char detail[192] = {};
    if (straightDied != branchedDied || straightSurvived != branchedSurvived) {
        bad = "a run through branches lasts a different number of ticks";
        std::snprintf(detail, sizeof(detail), "straight %d ticks (died %d), branched %d ticks (died %d)",
                      straightSurvived, straightDied ? 1 : 0, branchedSurvived, branchedDied ? 1 : 0);
    } else if (straight.size() != branched.size()) {
        bad = "a run through branches has a different number of ticks";
    } else {
        for (std::size_t i = 0; i < straight.size(); i++) {
            if (straight[i] == branched[i]) continue;
            bad = "a tick of a run through branches comes out different";
            std::snprintf(detail, sizeof(detail), "tick %d of %d", (int)(i + 1), (int)straight.size());
            break;
        }
    }
    if (!bad && !(straightWs == branchedWs)) bad = "the World state after branches is different";
    if (!bad && worldStep && !straightLog.sameOps(branchedLog)) bad = "the ops after branches are different";

    if (bad) {
        // A run whose answer depends on what the search did before it is the
        // one thing the pathfinder cannot live with, so the World goes off and
        // every run behaves as it did before it existed.
        world::noteDrift(bad);
        world::World::turnOff(bad);
        devlog::logf(devlog::Cat::System, "world: branch check failed - %s (%s)", bad, detail);
        return;
    }
    devlog::logf(devlog::Cat::System,
                 "world: branch check passed - %d ticks straight and through %d abandoned branches come out the "
                 "same, tick for tick%s",
                 (int)straight.size(), branches, worldStep ? "" : " (the World does not step this level's triggers)");
}

std::shared_ptr<Trajectory::SimStart> Trajectory::advanceStart(GJBaseGameLayer* pl, std::shared_ptr<SimStart> from,
                                                              std::span<const TickInput> inputs,
                                                              std::span<const TickInput> inputs2) {
    if (!pl || !from || inputs.empty()) return nullptr;
    std::shared_ptr<SimStart> previous = m_start;
    const bool swapped = from != previous;  // already in use: nothing to clear or put back
    if (swapped) useStart(from);
    std::shared_ptr<SimStart> out;
    Sim s;
    const bool p1 = from->p1;
    if (s.begin(this, pl, p1, from->held, inputs2, from->held2, true)) {
        // How far the lead copy got in each direction. Which copy leads is
        // read again every tick: a solo portal the plan crosses takes one of
        // the two out of the level and hands the run to the other
        // (Sim::leaveDual), and the box has to follow the copy that carries on.
        cocos2d::CCPoint lo = s.players[0]->getPosition(), hi = lo;
        bool dead = false;
        int stepped = 0;  // the ticks actually run: the new start stands that much later
        for (size_t i = 0; i < inputs.size(); i++) {
            if (s.step(inputs[i])) {
                dead = true;
                break;
            }
            stepped++;
            {
                const cocos2d::CCPoint q = s.players[0]->getPosition();
                lo.x = std::min(lo.x, q.x);
                lo.y = std::min(lo.y, q.y);
                hi.x = std::max(hi.x, q.x);
                hi.y = std::max(hi.y, q.y);
            }
            if (s.complete()) break;
        }
        if (!dead) {
            out = std::make_shared<SimStart>();
            auto grab = [](PlayerObject* r, SimPlayerSnap& o) {
                o.player = SavedPlayerCheckpoint::create(r);
                o.position = r->getPosition();
                o.player.m_position = o.position;  // a copy's m_position is stale by design (see snapshot());
                                                   // captureStart keeps the two the same and so must this
                o.rotation = r->getRotation();
                o.scaleX = r->getScaleX();
                o.scaleY = r->getScaleY();
                o.spriteWidthScale = r->m_spriteWidthScale;
                o.spriteHeightScale = r->m_spriteHeightScale;
                o.buttons = r->m_holdingButtons;
                o.lastPosition = r->m_lastPosition;
            };
            // Which player the lead copy stands for. It is the one the run was
            // driven by, unless a solo portal took that one out of the level
            // (leaveDual): the copy of player 1 is then the one the start
            // carries on with.
            out->p1 = s.players[0] == m_fakePlayer1;
            out->count = s.count;
            for (int i = 0; i < s.count; i++) grab(s.players[i], out->p[i]);
            // The clock as the run left it (end() puts the game's back).
            out->totalTime = pl->m_gameState.m_totalTime;
            out->unkDouble3 = pl->m_gameState.m_unkDouble3;
            out->currentProgress = pl->m_gameState.m_currentProgress;
            out->unkUint5 = pl->m_gameState.m_unkUint5;
            out->portal.grab(pl);
            out->timeMod = pl->m_gameState.m_timeModRelated;
            out->timeMod2 = pl->m_gameState.m_timeModRelated2;
            out->currentChannel = pl->m_gameState.m_currentChannel;
            out->spawnChannel1 = pl->m_gameState.m_spawnChannelRelated1;
            out->teleportRand = m_teleportRand;  // as the run left it
            if (s.worldOn) {
                // The World as the run left it (design step 9): its final state,
                // its log, and the table that log starts from - the start's own,
                // so a run from the new start puts the objects where the plan
                // leaves them, not where the real game has played them to since.
                auto carried = std::make_shared<world::WorldStart>();
                carried->keyframe = std::make_shared<const world::WorldState>(s.ws);
                carried->keyframeTick = from->moveTick + stepped;
                if (s.worldStep) carried->log = std::make_shared<const world::OpLog>(s.log);
                if (s.worldObjects) {
                    carried->base = s.objects.table();
                    carried->carry = s.objects.carry();
                    // The plan's own base tick travels with its table: the new
                    // start's runs go on counting the carry from it.
                    carried->baseTick = s.objects.baseTick();
                    carried->inexact = false;
                }
                out->world = std::move(carried);
            }
            out->spawnChannel0 = pl->m_gameState.m_spawnChannelRelated0;
            if (s.worldStep && s.run.walks) {
                // The run's own walk moved the spawn cursors as the game's does:
                // the start takes them as they are (a run from it without the
                // World reads these too).
                out->spawnChannel0.clear();
                for (const auto& [channel, cursor] : s.ws.spawnCursor) out->spawnChannel0[channel] = cursor;
                out->spawnChannel1.clear();
                for (const auto& [channel, back] : s.ws.goingBack) out->spawnChannel1[channel] = back;
                out->currentChannel = s.ws.channel;
            } else {
                // The simulation never moves the game's index into the spawn list, so
                // the triggers this run went past are moved over here, as the game's
                // walk does, or every run from the new start fires them again on its
                // first tick (a teleport trigger throws the copy back). Measured against
                // the furthest the lead copy got, and never short of the last entry the
                // run fired: a teleport can throw the copy back over its own trigger.
                // (Before end(), which clears what the run activated.)
                const int ch = pl->m_gameState.m_currentChannel;
                auto* list = pl->m_spawnObjects ? static_cast<cocos2d::CCArray*>(pl->m_spawnObjects->objectForKey(ch)) : nullptr;
                if (list) {
                    const auto bk = pl->m_gameState.m_spawnChannelRelated1.find(ch);
                    const bool goingBack = bk != pl->m_gameState.m_spawnChannelRelated1.end() && bk->second;
                    const bool side = s.players[0]->m_isSideways;
                    const float pv = goingBack ? (side ? lo.y : lo.x) : (side ? hi.y : hi.x);
                    const unsigned n = list->count();
                    const auto it = out->spawnChannel0.find(ch);
                    int idx = it == out->spawnChannel0.end() ? 0 : std::max(0, it->second);
                    int fired = -1;
                    if (!m_activatedObjectsP1.empty() || !m_activatedObjectsP2.empty()) {
                        for (unsigned i = (unsigned)idx; i < n; i++) {
                            const uintptr_t key = (uintptr_t)list->objectAtIndex(i);
                            if (m_activatedObjectsP1.contains(key) || m_activatedObjectsP2.contains(key)) fired = (int)i;
                        }
                    }
                    while ((unsigned)idx < n) {
                        auto* o = static_cast<EffectGameObject*>(list->objectAtIndex((unsigned)idx));
                        const float ov = side ? o->m_speedStart.y : o->m_speedStart.x;
                        if (idx > fired && (goingBack ? ov < pv : ov > pv)) break;
                        idx++;
                    }
                    out->spawnChannel0[ch] = idx;
                }
            }
            // The dual part as the run left it: a dual or solo portal the plan
            // crossed changed it (enterDual / leaveDual), and end() has not put
            // the game's own back yet.
            out->dualMode = pl->m_gameState.m_isDualMode;
            out->moveTick = from->moveTick + stepped;
            out->held = s.down;
            out->held2 = s.downOther;
            // A pad, portal or trigger the plan already used must not fire again for a
            // run from the new start (the real player has not reached it, so its own
            // flag is still off). Rings go in too: the list only holds the rings the run
            // fired (physics/player.cpp ringJump marks one when it fires, never on a
            // touch), so a ring only touched stays pressable, and a fired one is skipped
            // by the collision pass just as it was in the run - without leaning on
            // m_ringRelatedSet surviving the per-tick resetTouchedRings. A multi-activate
            // ring is not skipped either way (playerHasActivated).
            auto carry = [](const std::unordered_set<uintptr_t>& src, std::unordered_set<uintptr_t>& dst) {
                dst.insert(src.begin(), src.end());
            };
            carry(m_activatedObjectsP1, out->activated1);
            carry(m_activatedObjectsP2, out->activated2);
            PlayerObject* f = s.players[0];
            out->flying = f->m_isShip || f->m_isBird || f->m_isDart || f->m_isSwing;
            out->x = f->getPositionX();
            if (s.count == 2) {
                PlayerObject* o = s.players[1];
                out->flying2 = o->m_isShip || o->m_isBird || o->m_isDart || o->m_isSwing;
            }
        }
        s.end();
    }
    if (swapped) useStart(previous);
    return out;
}

bool Trajectory::wants(GJBaseGameLayer* pl, bool p1, int mode) {
    auto realPlayer = p1 ? pl->m_player1 : pl->m_player2;

    bool isLeft = (mode & TrajectoryMode::Left) != 0;
    bool isRight = (mode & TrajectoryMode::Right) != 0;
    bool isDirectionless = !(isLeft || isRight);
    bool holdingDirection = (isLeft && realPlayer->m_holdingLeft) ||
                            (isRight && realPlayer->m_holdingRight) ||
                            (isDirectionless && !realPlayer->m_holdingLeft &&
                             !realPlayer->m_holdingRight);

    // Whether a path is drawn is a matter of its click kind alone; in a
    // platformer level the path follows the direction the player holds (the
    // one standing still when none is held).
    const auto& kind = SLSettings::get()->trajectory.categories[mode & CLICK_MASK];
    return kind.enabled && (!pl->m_isPlatformer || holdingDirection);
}

// A line's name in the log: its click kind, and its direction in a platformer level.
static std::string lineName(int mode) {
    using Mode = SLSettings::TrajectorySettings::Mode;
    std::string name;
    switch (mode & Trajectory::CLICK_MASK) {
        case Mode::Hold: name = "Hold"; break;
        case Mode::Swift: name = "Swift"; break;
        case Mode::Release: name = "Release"; break;
        case Mode::Double: name = "Double"; break;
        case Mode::Tap: name = "Tap"; break;
        case Mode::DoubleHeld: name = "Double held"; break;
        default: name = "?"; break;
    }
    if (mode & Mode::Left) name += " left";
    if (mode & Mode::Right) name += " right";
    return name;
}

TrajectoryPlayerData Trajectory::simulate(GJBaseGameLayer* pl, bool p1,
                                          int mode, bool clickBothPlayers,
                                          PredictionConfig config) {
    // VMProtectBeginMutation("TrajectorySimulation");

    auto player = p1 ? m_fakePlayer1 : m_fakePlayer2;
    auto realPlayer = p1 ? pl->m_player1 : pl->m_player2;
    auto otherPlayer = p1 ? m_fakePlayer2 : m_fakePlayer1;
    auto otherRealPlayer = p1 ? pl->m_player2 : pl->m_player1;

    const bool wanted = wants(pl, p1, mode);
    if (!wanted && !config.m_bypassConfig) {
        return {};
    }

    // What this line cost (devlog Cat::Trajectory only), logged once all it
    // set up is gone again: it is made before any of that, so it is
    // destroyed after all of it.
    struct LineTimer {
        Trajectory* t = nullptr;  // null: not measured
        bool p1 = true;
        int mode = 0;
        std::chrono::steady_clock::time_point start;
        ~LineTimer() {
            if (!t) return;
            auto& prof = t->m_prof;
            const int64_t totalNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            const int64_t simNs = prof.loopNs - prof.loopDrawNs;
            const int64_t setupNs = totalNs - prof.loopNs - (prof.drawNs - prof.loopDrawNs);
            const char* what = "drawn";
            if (!t->m_drawing) what = "not a rebuild";  // the death prevention, the extrapolation, the best-frame search
            else if (t->m_recordOps == &t->m_lineOps) what = "recorded";
            else if (t->m_recordOps) what = "recording check";
            devlog::logf(devlog::Cat::Trajectory,
                         "line %s P%d (%s): ticks %d+%d, %d segments, %d hitboxes; setup %.0f us, "
                         "simulation %.0f us, drawing %.0f us",
                         lineName(mode).c_str(), p1 ? 1 : 2, what, prof.ticks[0], prof.ticks[1], prof.segments,
                         prof.hitboxes, setupNs / 1000.0, simNs / 1000.0, prof.drawNs / 1000.0);
            if (t->m_drawing) {
                auto& r = t->m_rebuildProf;
                r.lines++;
                if (!t->m_recordOps) r.segments += prof.segments;  // a recording's are counted where it is drawn
                r.setupNs += setupNs;
                r.simNs += simNs;
                r.drawNs += prof.drawNs;
            }
            prof.on = false;
        }
    } lineTimer;
    m_prof = LineProfile{};
    m_prof.on = devlog::on(devlog::Cat::Trajectory);
    if (m_prof.on) {
        lineTimer.t = this;
        lineTimer.p1 = p1;
        lineTimer.mode = mode;
        lineTimer.start = std::chrono::steady_clock::now();
    }

    GJGameState state = pl->m_gameState;
    // std::vector<SavedObjectStateRef> savedObjects;
    // std::vector<SavedActiveObjectState> savedActiveObjects;
    // std::vector<SavedSpecialObjectState> savedSpecialObjects;
    // ((PlayLayer*)pl)->saveDynamicSaveObjects(savedObjects);
    // ((PlayLayer*)pl)
    //     ->saveActiveSaveObjects(savedActiveObjects, savedSpecialObjects);

    player->copyAttributes(realPlayer);
    // copyAttributes runs the game's own kind toggles with the real player's
    // flags (GD 2.2081 0x1403a04a0), so the node is the real player's kind now,
    // whatever the last run left it as; Sim::begin notes it the same way.
    // Without this, setFakeMode below started from the kind the last run
    // (another line, the death prevention, the frame extrapolation or a Sim)
    // had left in the bookkeeping and ran the game's toggles on top of what
    // copyAttributes had set up, so two lines of one rebuild could start from
    // copies set up differently.
    noteFakeMode(player, modeOf(realPlayer));
    player->m_maybeReducedEffects = true;
    setFakeMode(player, modeOf(realPlayer));
    SavedPlayerCheckpoint checkpoint =
        SavedPlayerCheckpoint::create(realPlayer);
    checkpoint.apply(player);
    // player->setPosition(realPlayer->getPosition());
    // player->setRotation(realPlayer->getRotation());
    player->setPosition(realPlayer->m_position);
    player->setRotation(realPlayer->getRotation());
    if (player->getScaleX() != realPlayer->getScaleX()) player->setScaleX(realPlayer->getScaleX());
    if (player->getScaleY() != realPlayer->getScaleY()) player->setScaleY(realPlayer->getScaleY());
    syncButtons(player, realPlayer);

    if (clickBothPlayers) {
        otherPlayer->copyAttributes(otherRealPlayer);
        noteFakeMode(otherPlayer, modeOf(otherRealPlayer));  // (see above)
        otherPlayer->m_maybeReducedEffects = true;
        setFakeMode(otherPlayer, modeOf(otherRealPlayer));
        SavedPlayerCheckpoint checkpoint =
            SavedPlayerCheckpoint::create(otherRealPlayer);
        checkpoint.apply(otherPlayer);

        otherPlayer->setPosition(otherRealPlayer->getPosition());
        otherPlayer->setRotation(otherRealPlayer->getRotation());
        syncButtons(otherPlayer, otherRealPlayer);
    }

    m_deadP1 = false;
    m_deadP2 = false;
    // The once-per-contact ring rule starts fresh for every prediction (it
    // kept the contacts of the previous one, made at a later simulated tick,
    // so an orb the last prediction fired counted as used for this one too:
    // the click path over an orb showed no jump from the second frame on).
    clearRingContacts();
    clearKiller();
    deactivateAllRemembered();  // a paused search's activations are not this prediction's
    seedRingContacts(player);
    if (clickBothPlayers) seedRingContacts(otherPlayer);

    float* colors = SLSettings::get()->trajectory.categories[mode & CLICK_MASK].colors.data();

    // The prediction's own World state, started from the live game like a run
    // (the contacts the players are in, the teleport seed), so its contact rule
    // and group teleports never touch the live contact map or another run's.
    // Without the World (or when the capture's level check has just turned it
    // off) no state is current, and the prediction takes the old paths.
    world::WorldState displayWorld;
    bool displayWorldOn = false;
    if (!world::World::disabled) {
        displayWorld = world::World::captureLive(pl);
        displayWorldOn = !world::World::disabled;
    }
    // ... and it steps the level's triggers like a run (world/step.cpp), with a
    // log and object cache of its own, for the copies it draws.
    world::OpLog displayLog;
    world::ObjectCache displayCache;
    world::Run displayRun;
    std::shared_ptr<const world::WorldDef> displayDef;
    if (displayWorldOn) displayDef = world::WorldDef::get(pl);
    const bool displayStep = displayWorldOn && displayDef != nullptr;
    if (displayStep) {
        displayCache.reset(displayDef);
        displayWorld.commandIndex = pl->m_gameState.m_commandIndex;
        displayRun.pl = pl;
        displayRun.def = displayDef.get();
        displayRun.ws = &displayWorld;
        displayRun.log = &displayLog;
        displayRun.cache = &displayCache;
        displayRun.player1 = p1 ? player : (clickBothPlayers ? otherPlayer : nullptr);
        displayRun.player2 = !p1 ? player : (clickBothPlayers ? otherPlayer : nullptr);
        player->m_lastPosition = realPlayer->m_lastPosition;
        if (clickBothPlayers) otherPlayer->m_lastPosition = otherRealPlayer->m_lastPosition;
        pl->m_gameState.m_currentChannel = displayWorld.channel;
    }
    // The drawn prediction's own objects (design step 8): the table of this
    // tick and the prediction's log put them in place near its copies, and
    // everything is put back when it is drawn. Without them it keeps the old
    // carry of the moving objects.
    world::Materializer displayObjects;
    if (displayStep && !displayWorld.partial) {
        auto table = world::baseTable(pl, displayDef, state.m_currentProgress, state.m_commandIndex);
        if (table) {
            displayCache.setBaseTable(table);
            displayObjects.begin(pl, table, world::carriedMovers(*table, displayWorld, *displayDef),
                                 state.m_currentProgress, state.m_commandIndex, (int)displayWorld.tick);
            displayRun.objects = &displayObjects;
        }
    }
    world::Scope displayScope(displayStep ? &displayRun : nullptr, displayWorldOn ? &displayWorld : nullptr);

    if (!displayObjects.active()) g_displayMoving.begin(pl);
    auto predicted = this->runPrediction(pl, player, otherPlayer, mode, colors,
                                         clickBothPlayers, config);
    displayObjects.end();
    g_displayMoving.end();

    player->setVisible(false);
    otherPlayer->setVisible(false);

    clearRingContacts();
    deactivateAllRemembered();
    // for (int i = 0; i < pl->m_objects->count(); i++) {
    //     GameObject* object = (GameObject*)pl->m_objects->objectAtIndex(i);

    //     CCPoint pos = object->getRealPosition();

    //     object->resetObject();
    //     // if (object->m_outerSectionIndex >= 0) {
    //     //     if (object->m_positionX > 0.0) {

    //     //     }
    //     // }

    //     object->m_isDirty = true;
    //     object->setObjectRectDirty(true);
    //     object->setOrientedRectDirty(true);
    // }

    pl->m_gameState = state;

    // reinterpret_cast<void(*)(GJBaseGameLayer*)>(reinterpret_cast<void*>(geode::base::get()
    // + 0x398310))(pl);
    // for (auto& obj : savedObjects) {
    //     GameObject* o = obj.m_gameObject;
    //     o->resetObject();
    //     o->m_positionX = obj.m_positionX;
    //     o->m_positionY = obj.m_positionY;
    //     o->m_rotationXOffset = obj.m_rotationXOffset;
    //     o->m_rotationYOffset = obj.m_rotationYOffset;

    //     float addToCustomScaleX = obj.m_addToCustomScaleX;
    //     o->m_scaleXOffset = o->m_scaleXOffset + addToCustomScaleX;
    //     o->m_scaleX = o->m_scaleX + addToCustomScaleX;

    //     o->m_isDirty = true;
    //     o->m_isObjectRectDirty = true;

    //     float addToCustomScaleY = obj.m_addToCustomScaleY;
    //     o->m_scaleYOffset = o->m_scaleYOffset + addToCustomScaleY;
    //     o->m_scaleY = o->m_scaleY + addToCustomScaleY;

    //     o->m_unk4C4 = -1;
    //     o->m_unk4CC = -1;

    //     auto pos = o->getRealPosition();
    //     o->setPosition(pos);
    //     pos = o->getRealPosition();
    //     o->m_lastPosition = pos;
    //     o->setObjectRectDirty(true);
    //     o->setOrientedRectDirty(true);
    //     pl->updateObjectSection(o);
    // }

    // for (auto& obj : savedActiveObjects) {
    //     EffectGameObject* o = (EffectGameObject*)obj.m_gameObject;
    //     o->resetObject();
    //     o->m_activatedByPlayer1 = obj.m_activatedByPlayer1;
    //     o->m_activatedByPlayer2 = obj.m_activatedByPlayer2;
    // }

    // VMProtectEnd();
    return predicted;
}

inline void drawRect(CCDrawNode* node, CCRect& rect, ccColor4F color,
                     float width) {
    // std::array<CCPoint, 4> vertices = {CCPoint{rect.getMinX(),
    // rect.getMinY()},
    //                                    CCPoint{rect.getMaxX(),
    //                                    rect.getMinY()},
    //                                    CCPoint{rect.getMaxX(),
    //                                    rect.getMaxY()},
    //                                    CCPoint{rect.getMinX(),
    //                                    rect.getMaxY()}};

    // node->drawPolygon(vertices.data(), vertices.size(), {0.0, 0.0, 0.0, 0.0},
    //                   width, color);

    node->drawRect(rect, {0.0, 0.0, 0.0, 0.0}, width, color);
}

static void drawRotatedRect(CCDrawNode* node, CCRect& rect, float angle,
                            ccColor4F color, float width) {
    std::array<CCPoint, 4> vertices = {CCPoint{rect.getMinX(), rect.getMinY()},
                                       CCPoint{rect.getMaxX(), rect.getMinY()},
                                       CCPoint{rect.getMaxX(), rect.getMaxY()},
                                       CCPoint{rect.getMinX(), rect.getMaxY()}};

    for (auto& vertex : vertices) {
        vertex = vertex.rotateByAngle({rect.getMidX(), rect.getMidY()},
                                      -CC_DEGREES_TO_RADIANS(angle));
    }

    node->drawPolygon(vertices.data(), vertices.size(), {0.0, 0.0, 0.0, 0.0},
                      width, color);
}

static uint64_t packPlayerFlags(PlayerObject* p) {
    uint64_t flags = 0;
    int bit = 0;
    auto set = [&](bool v) {
        if (v) flags |= (uint64_t(1) << bit);
        bit++;
    };
    set(p->m_isShip);
    set(p->m_isBird);
    set(p->m_isDart);
    set(p->m_isSwing);
    set(p->m_isBall);
    set(p->m_isSpider);
    set(p->m_isRobot);
    set(p->m_isUpsideDown);
    set(p->m_isDead);
    set(p->m_isOnGround2);
    set(p->m_isSideways);
    set(p->m_isDashing);
    set(p->m_isAccelerating);
    set(p->m_holdingLeft);
    set(p->m_holdingRight);
    set(p->m_jumpBuffered);
    set(p->m_isPlatformer);
    set(p->m_isGoingLeft);
    set(p->m_maybeIsBoosted);
    return flags;
}

// thank you clanker
static size_t hashTrajectoryCategories() {
    size_t h = 0;
    for (const auto& [key, state] : SLSettings::get()->trajectory.categories) {
        size_t e = std::hash<int>{}(key);
        e = e * 31 + (state.enabled ? 1u : 0u);
        for (const float c : state.colors) {
            e = e * 31 + std::hash<float>{}(c);
        }
        // order-independent combine (keys are unique)
        h ^= e + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

Trajectory::Signature Trajectory::computeSignature(GJBaseGameLayer* pl) {
    Signature s;
    s.frame = Bot::get()->updater().getFrame();

    if (PlayerObject* p = pl->m_player1) {
        const cocos2d::CCPoint pos = p->getPosition();
        s.p1[0] = pos.x;
        s.p1[1] = pos.y;
        s.p1[2] = p->getRotation();
        s.p1[3] = p->m_yVelocity;
        s.p1[4] = p->m_platformerXVelocity;
        s.p1[5] = p->m_gravityMod;
        s.p1[6] = p->m_vehicleSize;
        s.p1flags = packPlayerFlags(p);
    }
    if (PlayerObject* p = pl->m_player2) {
        const cocos2d::CCPoint pos = p->getPosition();
        s.p2[0] = pos.x;
        s.p2[1] = pos.y;
        s.p2[2] = p->getRotation();
        s.p2[3] = p->m_yVelocity;
        s.p2[4] = p->m_platformerXVelocity;
        s.p2[5] = p->m_gravityMod;
        s.p2[6] = p->m_vehicleSize;
        s.p2flags = packPlayerFlags(p);
    }

    s.timeWarp = pl->m_gameState.m_timeWarp;
    s.cameraZoom = pl->m_gameState.m_cameraZoom;
    s.width = m_state->m_width->inner();
    s.length = m_state->m_length->inner();

    uint32_t b = 0;
    int bit = 0;
    auto setb = [&](bool v) {
        if (v) b |= (uint32_t(1) << bit);
        bit++;
    };
    setb(m_state->m_enabled->inner());
    setb(pl->m_gameState.m_isDualMode);
    setb(pl->m_isPlatformer);
    setb(pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode);
    setb(m_p1Holding);
    setb(m_p2Holding);
    setb(pl->m_player1 != nullptr);
    setb(pl->m_player2 != nullptr);
    s.boolPack = b;

    s.categoriesHash = hashTrajectoryCategories();
    return s;
}

void Trajectory::update(GJBaseGameLayer* pl) {
    if (!pl) return;

    this->m_fakePlayer1->setVisible(false);
    this->m_fakePlayer2->setVisible(false);

    if (auto lel = LevelEditorLayer::get();
        lel && lel->m_playbackMode == PlaybackMode::Not) {
        return;
    }

    // While the pathfinder plays, the copies are its. When the game is
    // paused underneath it (the pause menu) they are free, and the paths
    // from where the player stands can be shown - unless a kept start is in
    // use, which would put objects ahead in another state.
    const bool pathfinding = Bot::get()->pathfinder().drivesGame();
    auto* playLayer = PlayLayer::get();
    const bool pausedUnderneath = playLayer && playLayer->m_isPaused && !usingStart();
    if (!m_state->m_enabled->inner() || (pathfinding && !pausedUnderneath)) {
        m_node->setVisible(false);
        return;
    }

    // The level settings and players only exist once the level has been set
    // up; a frame that runs before that (the loading screen, a level being
    // torn down) has nothing to predict yet.
    if (!pl->m_levelSettings || !pl->m_player1) {
        m_node->setVisible(false);
        return;
    }

    m_node->setVisible(true);

    Signature signature = computeSignature(pl);
    bool needsRebuild = !m_calculated || !(signature == m_lastSignature);

    // The merge tolerance (mergeTolerance) was fixed for the scale the last
    // rebuild drew at. The signature does not see a zoom that leaves the
    // game's camera zoom alone (the editor's zoom in a paused playtest, a mod
    // scaling the PlayLayer in the pause menu, a resized window). Once the
    // lines are drawn at twice that scale or more, a merged segment could
    // stray from the per-tick path by a visible amount, so they are rebuilt.
    // Zooming out only makes the tolerance finer than it needs to be. With no
    // tolerance (nothing was merged) there is nothing to redo.
    if (!needsRebuild && m_calculated && m_mergeEps > 0.0f && !(mergeTolerance() >= 0.5f * m_mergeEps)) {
        needsRebuild = true;
    }

    // The refresh limit ("Updates per second" on the Trajectory card; 0, the
    // default, leaves every rebuild to the signature alone). While the level
    // runs, a rebuild asked for sooner than 1 / refreshRate seconds after the
    // last one is skipped: the lines drawn last stay on screen, and the
    // signature is not stored, so the first frame the limit lets through
    // rebuilds from wherever the game is by then. Everything else rebuilds at
    // once. A first build or a dropped cache has nothing on screen worth
    // keeping, and a jump back in time (a respawn, a restart, a step back)
    // leaves the old lines pointing away from where the player is now. The
    // pause menu, the frame stepper and a paused playtest are where the lines
    // are studied, and a game that stands still saves nothing by waiting. A
    // video being rendered runs at the video's pace, not the screen's, so a
    // limit counted in wall clock time would make its line move unevenly.
    // The rate is the one the slider shows (updatesPerSecond).
    if (const int refreshRate = SLSettings::get()->trajectory.updatesPerSecond(); needsRebuild && refreshRate > 0) {
        // One trajectory exists at a time, and a new one starts uncalculated,
        // so a single time for all of them is enough.
        static std::chrono::steady_clock::time_point lastRebuild;
        const auto now = std::chrono::steady_clock::now();
        auto* editor = LevelEditorLayer::get();
        const bool running = !(playLayer && playLayer->m_isPaused) && !Bot::get()->updater().isPaused() &&
                             !(editor && editor->m_playbackMode == PlaybackMode::Paused) &&
                             !Renderer::get()->isRecording();
        const bool forward = signature.frame >= m_lastSignature.frame;
        if (m_calculated && running && forward &&
            std::chrono::duration<double>(now - lastRebuild).count() < 1.0 / refreshRate) {
            needsRebuild = false;
            // The step length a rebuild sets is kept current all the same:
            // the death prevention and the frame extrapolation run short
            // lines of their own with it between rebuilds (without the
            // World), and what they find must not depend on this limit.
            m_delta = Bot::get()->updater().getPhysicsDt() * std::min(pl->m_gameState.m_timeWarp, 1.0f) * 60.0f;
        } else {
            lastRebuild = now;
        }
    }

    // What the rebuild cost (devlog Cat::Trajectory only; see simulate()).
    const bool profile = needsRebuild && devlog::on(devlog::Cat::Trajectory);
    std::chrono::steady_clock::time_point rebuildStart;

    if (needsRebuild) {
        m_drawing = true;
        if (profile) {
            rebuildStart = std::chrono::steady_clock::now();
            m_rebuildProf = RebuildProfile{};
        }

        // m_node->setZOrder(999);
        m_node->clear();

        auto bot = Bot::get();
        m_lastFrame = bot->updater().getFrame();
        m_delta = bot->updater().getPhysicsDt() * std::min(pl->m_gameState.m_timeWarp, 1.0f) * 60.0f;
        // How far a merged segment may stray at the zoom this rebuild draws at.
        m_mergeEps = mergeTolerance();

        m_fakePlayer1->setVisible(false);
        if (pl->m_player2) {
            m_fakePlayer2->setVisible(false);
        }
    }

    if (needsRebuild && pl->m_player1) {
        TrajectoryPlayerData hold{};
        TrajectoryPlayerData release{};
        drawLines(pl, true, !pl->m_levelSettings->m_twoPlayerMode, &hold, &release);
        if (devlog::on(devlog::Cat::Trajectory)) {
            const int length = this->getPredictionLength();
            devlog::logf(devlog::Cat::Trajectory,
                         "rebuilt: click survives %d/%d ticks -> (%.2f, %.2f), "
                         "no click survives %d/%d ticks -> (%.2f, %.2f)",
                         hold.score, length, hold.position.x, hold.position.y,
                         release.score, length, release.position.x,
                         release.position.y);
        }

        m_fakePlayer1->setVisible(false);
        if (pl->m_player2) {
            m_fakePlayer2->setVisible(false);
        }
    }
    if (needsRebuild && pl->m_player2 && pl->m_gameState.m_isDualMode &&
        pl->m_levelSettings->m_twoPlayerMode) {
        drawLines(pl, false, false, nullptr, nullptr);
    }

    if (needsRebuild) {
        if (profile) {
            const auto& r = m_rebuildProf;
            const double totalUs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - rebuildStart)
                                       .count() /
                                   1000.0;
            // The node uploads its whole buffer (capacity, 20 bytes a vertex)
            // every frame after a rebuild, outside this measurement.
            devlog::logf(devlog::Cat::Trajectory,
                         "rebuild (%s, %.0f TPS, length %.2f): %d lines simulated, %d drawn from a recording, "
                         "%d segments drawn; %.0f us: setup %.0f, simulation %.0f, drawing %.0f, "
                         "recordings drawn %.0f, recording check %.0f; node %d of %u vertices (%.2f MB), "
                         "merge tolerance %.4f units",
                         LevelEditorLayer::get() ? "editor" : "level", Bot::get()->updater().getTps(),
                         m_state->m_length->inner(), r.lines, r.replayed, r.segments, totalUs, r.setupNs / 1000.0,
                         r.simNs / 1000.0, r.drawNs / 1000.0, r.replayNs / 1000.0, r.checkNs / 1000.0,
                         (int)m_node->m_nBufferCount, m_node->m_uBufferCapacity,
                         m_node->m_uBufferCapacity * 20.0 / (1024.0 * 1024.0), m_mergeEps);
        }
        m_calculated = true;
        m_lastSignature = signature;
        m_drawing = false;
    }

    auto& updater = Bot::get()->updater();
    if (gucci::GucciEngine::get()->layoutMode && !LevelEditorLayer::get()) {
        constexpr std::array<int, 5> colors = {1000, 1001, 1009, 1013, 1014};
        for (const int color : colors) {
            if (ColorAction* action =
                    pl->m_effectManager->getColorAction(color)) {
                switch (color) {
                    case 1000: {
                        auto& col = SLSettings::get()->layoutBgColor;
                        action->m_color = {
                            static_cast<GLubyte>(col[0] * 255),
                            static_cast<GLubyte>(col[1] * 255),
                            static_cast<GLubyte>(col[2] * 255),
                        };
                        break;
                    }
                    case 1001:
                    case 1009: {
                        auto& col = SLSettings::get()->layoutGroundColor;
                        action->m_color = {
                            static_cast<GLubyte>(col[0] * 255),
                            static_cast<GLubyte>(col[1] * 255),
                            static_cast<GLubyte>(col[2] * 255),
                        };
                        break;
                    }
                    default: {
                        action->m_color = {40, 125, 255};
                    }
                }
            }
        }
    }
}

void Trajectory::drawHitbox(PlayerObject* player) {
    float width = m_state->m_width->inner() /
                  GJBaseGameLayer::get()->m_gameState.m_cameraZoom;
    CCRect rect = usingWidth(player->getObjectRect(), width);
    CCRect scaled = usingWidth(player->getObjectRect(0.3, 0.3), width);
    drawHitboxRects(rect, scaled, player->getRotation(), width);
}

void Trajectory::drawHitboxRects(CCRect& rect, CCRect& scaled, float rotation, float width) {
#define CC_COLOR(color_type)                \
    *reinterpret_cast<cocos2d::ccColor4F*>( \
        settings.categories[color_type].colors.data())
    auto& settings = SLSettings::get()->hitboxes;
    using Type = SLSettings::HitboxSettings::Type;

    drawRotatedRect(m_node, rect, rotation,
                    CC_COLOR(Type::PlayerRotated), width);
    drawRect(m_node, rect, CC_COLOR(Type::Player), width);
    drawRect(m_node, scaled, CC_COLOR(Type::PlayerInner), width);
#undef CC_COLOR
}

// ------------------------------------------------------------ drawing the lines

namespace {
// Times the drawing for a line's profile (simulate(), devlog Cat::Trajectory
// only). Only the outermost timer counts, so a segment that a tick's merging
// draws is not counted twice. Nothing but a flag test while the log is off.
template <class Profile>
struct DrawTimer {
    Profile& prof;
    bool outer = false;
    std::chrono::steady_clock::time_point start;
    explicit DrawTimer(Profile& p) : prof(p) {
        if (!prof.on) return;
        outer = prof.depth++ == 0;
        if (outer) start = std::chrono::steady_clock::now();
    }
    ~DrawTimer() {
        if (!prof.on) return;
        prof.depth--;
        if (outer) {
            prof.drawNs +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        }
    }
    DrawTimer(const DrawTimer&) = delete;
    DrawTimer& operator=(const DrawTimer&) = delete;
};
}  // namespace

// Whether a run outside a rebuild (the death prevention's short lines, the
// frame extrapolation, the best-frame search) draws nothing. Without a refresh
// limit, every frame such a run draws in is a frame the game moved in, and its
// rebuild clears the node before anything is shown. With a limit, a frame
// whose rebuild is skipped keeps the node as the last rebuild left it, and
// what those runs draw would pile up on it until the next one. They only want
// what the simulation returns, and drawing never feeds back into it.
static bool dropsDrawsOutsideRebuild(bool drawing, bool recording) {
    return !drawing && !recording && SLSettings::get()->trajectory.updatesPerSecond() > 0;
}

// One draw call of a line: drawn, or kept while update() records the line.
void Trajectory::emitSegment(const CCPoint& a, const CCPoint& b, float width, bool firstCopy, float* colors) {
    // Before the timer and the count, so a profile only counts what is drawn.
    if (dropsDrawsOutsideRebuild(m_drawing, m_recordOps != nullptr)) return;
    DrawTimer timer(m_prof);
    if (m_prof.on) m_prof.segments++;
    if (m_recordOps) {
        DrawOp op;
        op.firstCopy = firstCopy;
        op.width = width;
        op.a = a;
        op.b = b;
        m_recordOps->push_back(op);
    } else {
        m_node->drawSegment(a, b, width, firstCopy ? toCocosColor(colors) : toCocosColorNegative(colors));
    }
}

// A copy's segment for one tick (iterate): drawn as it is, or added to the
// copy's run while a rebuild merges them.
void Trajectory::tickSegment(PlayerObject* copy, const CCPoint& from, const CCPoint& to, float* colors) {
    DrawTimer timer(m_prof);
    if (!m_mergeRuns) {
        emitSegment(from, to, (float)m_state->m_width->inner(), copy == m_fakePlayer1, colors);
        return;
    }
    addToRun(copy == m_fakePlayer2 ? 1 : 0, from, to);
}

// A dead copy's hitbox (iterate).
void Trajectory::emitHitbox(PlayerObject* copy) {
    // (No runs are merged outside a rebuild, so there is nothing to flush.)
    if (dropsDrawsOutsideRebuild(m_drawing, m_recordOps != nullptr)) return;
    DrawTimer timer(m_prof);
    if (m_prof.on) m_prof.hitboxes++;
    // The runs still being merged hold older ticks than the hitbox, so they
    // are drawn first, as their segments were one tick at a time.
    flushRuns();
    if (m_recordOps) {
        // What drawHitbox would draw, worked out the same way.
        DrawOp op;
        op.hitbox = true;
        op.firstCopy = copy == m_fakePlayer1;
        op.width = m_state->m_width->inner() / GJBaseGameLayer::get()->m_gameState.m_cameraZoom;
        op.rect = usingWidth(copy->getObjectRect(), op.width);
        op.inner = usingWidth(copy->getObjectRect(0.3, 0.3), op.width);
        op.rotation = copy->getRotation();
        m_recordOps->push_back(op);
    } else {
        drawHitbox(copy);
    }
}

void Trajectory::SegmentRun::start(const CCPoint& from, const CCPoint& to, float eps) {
    active = true;
    a = from;
    p = to;
    minX = std::min(from.x, to.x);
    maxX = std::max(from.x, to.x);
    minY = std::min(from.y, to.y);
    maxY = std::max(from.y, to.y);
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    reach = std::sqrt(dx * dx + dy * dy);
    coned = false;
    if (reach > eps) narrow(dx, dy, reach, eps);
}

// Whether the one segment from a to `to` still stands for every tick of the
// run with `to` added; if so, `to` is the run's last point now.
bool Trajectory::SegmentRun::extend(const CCPoint& to, float eps) {
    const float dx = to.x - a.x;
    const float dy = to.y - a.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    // The path has to keep moving away from the start. One that turns back
    // (a platformer turn, a jump straight up and down on the spot, reverse
    // gameplay) would lose what lies beyond the point it turned at: the
    // segment only reaches as far as its last point.
    if (!(d >= reach)) return false;
    if (d > eps) {
        // ... and every point so far must lie within eps of the segment: its
        // direction has to be inside all of their bounds. A point within eps
        // of a is within eps of any segment from a, and bounds nothing.
        if (coned && (lo.x * dy - lo.y * dx < 0.0f || dx * hi.y - dy * hi.x < 0.0f)) return false;
        narrow(dx, dy, d, eps);
    }
    p = to;
    reach = d;
    minX = std::min(minX, to.x);
    maxX = std::max(maxX, to.x);
    minY = std::min(minY, to.y);
    maxY = std::max(maxY, to.y);
    return true;
}

// A point (dx, dy) from a, d away, is a point of the run from now on: a
// segment from a passes within eps of it when its direction is within
// asin(eps / d) of the point's own, and the bounds narrow to that.
void Trajectory::SegmentRun::narrow(float dx, float dy, float d, float eps) {
    const float s = eps / d;
    const float c = std::sqrt(1.0f - s * s);
    const float ux = dx / d;
    const float uy = dy / d;
    const CCPoint l{ux * c + uy * s, uy * c - ux * s};  // turned clockwise by that angle
    const CCPoint h{ux * c - uy * s, uy * c + ux * s};  // and counter-clockwise
    if (!coned) {
        lo = l;
        hi = h;
        coned = true;
        return;
    }
    if (lo.x * l.y - lo.y * l.x > 0.0f) lo = l;  // l is counter-clockwise of lo: the tighter bound
    if (h.x * hi.y - h.y * hi.x > 0.0f) hi = h;  // h is clockwise of hi
}

// Whether a segment from `from` to `to` comes within `pad` of the run's points.
bool Trajectory::SegmentRun::meets(const CCPoint& from, const CCPoint& to, float pad) const {
    return std::min(from.x, to.x) - pad <= maxX && std::max(from.x, to.x) + pad >= minX &&
           std::min(from.y, to.y) - pad <= maxY && std::max(from.y, to.y) + pad >= minY;
}

void Trajectory::addToRun(int i, const CCPoint& from, const CCPoint& to) {
    // Two copies (the dual part) draw in two colours, and where their lines
    // cross the one drawn later is on top: tick by tick, the copy that passed
    // there later. A run is drawn only when it ends, so the other copy's run
    // is drawn first whenever this tick's segment comes near it. Two runs in
    // progress then never share a pixel, and their order changes nothing.
    SegmentRun& other = m_runs[1 - i];
    if (other.active && other.meets(from, to, 2.0f * (m_runWidth + m_mergeEps) + 1.0f)) flushRun(1 - i);
    SegmentRun& run = m_runs[i];
    if (run.active) {
        // Only a path that goes on from where the last tick ended: a gap
        // between two ticks (a teleport) stays a gap.
        if (from.x == run.p.x && from.y == run.p.y && run.extend(to, m_mergeEps)) return;
        flushRun(i);
    }
    run.start(from, to, m_mergeEps);
}

void Trajectory::flushRun(int i) {
    SegmentRun& run = m_runs[i];
    if (!run.active) return;
    run.active = false;
    emitSegment(run.a, run.p, m_runWidth, i == 0, m_runColors);
}

void Trajectory::flushRuns() {
    flushRun(0);
    flushRun(1);
}

// A tenth of a screen pixel in the level units the node draws in: how far a
// merged segment may pass from the per-tick path it stands for (SegmentRun).
// It comes from the node's own transform, which carries the camera zoom (the
// editor's zoom in a playtest), and from the window's pixels per point. 0 when
// there is nothing to work it out from, and then nothing is merged.
float Trajectory::mergeTolerance() {
    auto* director = CCDirector::get();
    auto* view = director ? director->getOpenGLView() : nullptr;
    if (!view || !m_node->getParent()) return 0.0f;
    const CCSize points = director->getWinSize();
    const CCSize pixels = view->getFrameSize();
    if (!(points.width > 0.0f && points.height > 0.0f)) return 0.0f;
    const float perPoint = std::max(pixels.width / points.width, pixels.height / points.height);
    const CCAffineTransform t = m_node->nodeToWorldTransform();
    const float perUnit =
        perPoint * std::max(std::sqrt(t.a * t.a + t.b * t.b), std::sqrt(t.c * t.c + t.d * t.d));
    if (!(perUnit > 0.0f) || !std::isfinite(perUnit)) return 0.0f;
    return 0.1f / perUnit;
}

void Trajectory::drawOps(const std::vector<DrawOp>& ops, float* colors, bool hitboxesOnly) {
    for (const DrawOp& op : ops) {
        if (op.hitbox) {
            CCRect rect = op.rect;
            CCRect inner = op.inner;
            drawHitboxRects(rect, inner, op.rotation, op.width);
        } else if (!hitboxesOnly) {
            m_node->drawSegment(op.a, op.b, op.width,
                                op.firstCopy ? toCocosColor(colors) : toCocosColorNegative(colors));
        }
    }
}

void Trajectory::drawLines(GJBaseGameLayer* pl, bool p1, bool clickBothPlayers, TrajectoryPlayerData* hold,
                           TrajectoryPlayerData* release) {
    using Mode = SLSettings::TrajectorySettings::Mode;
    // One line per click kind, and in a platformer level Hold, Swift and
    // Release going left and going right as well (simulate() draws the ones
    // whose direction the player holds): six click kinds and six directional
    // lines at most, which the array has to hold. A new kind goes last, so
    // every existing line keeps its place in the draw order.
    int modes[12];
    int count = 0;
    for (int m : {Mode::Hold, Mode::Swift, Mode::Release, Mode::Double, Mode::Tap, Mode::DoubleHeld}) {
        modes[count++] = m;
    }
    if (pl->m_isPlatformer) {
        for (int m : {Mode::Hold | Mode::Left, Mode::Swift | Mode::Left, Mode::Release | Mode::Left,
                      Mode::Hold | Mode::Right, Mode::Swift | Mode::Right, Mode::Release | Mode::Right}) {
            modes[count++] = m;
        }
    }
    auto keep = [&](int mode, const TrajectoryPlayerData& data) {
        if (mode == Mode::Hold && hold) *hold = data;
        if (mode == Mode::Release && release) *release = data;
    };

    if (manualPredictionInput()) {
        // The player's own play: each kind is an input of its own.
        for (int i = 0; i < count; i++) keep(modes[i], simulate(pl, p1, modes[i], clickBothPlayers));
        return;
    }

    // A replay or the autoclicker drives the copies instead
    // (manualPredictionInput), so every kind runs the very same inputs from
    // the very same start, and its kind only colours the line (and names it in
    // what simulate() returns). The first line the player wants is simulated
    // once with its draw calls kept, and every wanted kind is drawn from them,
    // in its own colour and in the order the lines have always been drawn in.
    // A recording only ever stands for the lines of the player it was made
    // for: in a two-player level player 2's lines are recorded on their own.
    int wanted[12];  // at most every line in modes
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (wants(pl, p1, modes[i])) wanted[n++] = modes[i];
    }
    if (n == 0) return;
    if (n == 1) {
        // Nothing to draw twice: the line is drawn as it is simulated.
        keep(wanted[0], simulate(pl, p1, wanted[0], clickBothPlayers));
        return;
    }
    const bool profile = devlog::on(devlog::Cat::Trajectory);
    auto& categories = SLSettings::get()->trajectory.categories;
    // Every kind's line lies on the same pixels, so an opaque kind drawn last
    // covers every line below it completely: those need no segments at all
    // (the node's shader has no soft edge: a pixel is the line's colour or
    // untouched). Their hitboxes are still drawn, in order: the hitbox
    // colours can be translucent, and each line drew its own.
    const bool opaqueTop = categories[wanted[n - 1] & CLICK_MASK].colors[3] == 1.0f;
    m_lineOps.clear();
    m_recordOps = &m_lineOps;
    m_recordMerge = opaqueTop;  // only the top line's segments are drawn, and only when it is opaque
    const TrajectoryPlayerData line = simulate(pl, p1, wanted[0], clickBothPlayers);
    m_recordOps = nullptr;

    // The recording stands for lines that are no longer simulated. Now and
    // then while the log is on, the top one is simulated as well and its
    // draw calls compared with the recording: any difference is state one
    // line leaves behind for the next that none of them should see.
    if (profile && m_recordedGroups++ % 64 == 0) {
        const auto t0 = std::chrono::steady_clock::now();
        const RebuildProfile kept = m_rebuildProf;
        std::vector<DrawOp> again;
        m_recordOps = &again;
        simulate(pl, p1, wanted[n - 1], clickBothPlayers);
        m_recordOps = nullptr;
        m_rebuildProf = kept;
        auto bits = [](float f) {
            uint32_t u = 0;
            std::memcpy(&u, &f, sizeof(u));
            return u;
        };
        auto same = [&](const DrawOp& x, const DrawOp& y) {
            return x.hitbox == y.hitbox && x.firstCopy == y.firstCopy && bits(x.width) == bits(y.width) &&
                   bits(x.a.x) == bits(y.a.x) && bits(x.a.y) == bits(y.a.y) && bits(x.b.x) == bits(y.b.x) &&
                   bits(x.b.y) == bits(y.b.y) && bits(x.rect.origin.x) == bits(y.rect.origin.x) &&
                   bits(x.rect.origin.y) == bits(y.rect.origin.y) &&
                   bits(x.rect.size.width) == bits(y.rect.size.width) &&
                   bits(x.rect.size.height) == bits(y.rect.size.height) &&
                   bits(x.inner.origin.x) == bits(y.inner.origin.x) &&
                   bits(x.inner.origin.y) == bits(y.inner.origin.y) &&
                   bits(x.inner.size.width) == bits(y.inner.size.width) &&
                   bits(x.inner.size.height) == bits(y.inner.size.height) && bits(x.rotation) == bits(y.rotation);
        };
        size_t k = 0;
        while (k < m_lineOps.size() && k < again.size() && same(m_lineOps[k], again[k])) k++;
        if (k == m_lineOps.size() && k == again.size()) {
            devlog::logf(devlog::Cat::Trajectory, "recording check P%d: %s simulated again, %zu draw calls identical",
                         p1 ? 1 : 2, lineName(wanted[n - 1]).c_str(), again.size());
        } else {
            const DrawOp none{};
            const DrawOp& x = k < m_lineOps.size() ? m_lineOps[k] : none;
            const DrawOp& y = k < again.size() ? again[k] : none;
            devlog::logf(devlog::Cat::Trajectory,
                         "recording check P%d: %s simulated again DIFFERS from the recorded %s at draw call %zu "
                         "(%zu recorded, %zu now): (%.3f, %.3f)-(%.3f, %.3f) against (%.3f, %.3f)-(%.3f, %.3f)",
                         p1 ? 1 : 2, lineName(wanted[n - 1]).c_str(), lineName(wanted[0]).c_str(), k,
                         m_lineOps.size(), again.size(), x.a.x, x.a.y, x.b.x, x.b.y, y.a.x, y.a.y, y.b.x, y.b.y);
        }
        m_rebuildProf.checkNs +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    std::chrono::steady_clock::time_point t0;
    if (profile) t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < n; k++) {
        const bool hitboxesOnly = opaqueTop && k < n - 1;
        drawOps(m_lineOps, categories[wanted[k] & CLICK_MASK].colors.data(), hitboxesOnly);
        // What simulate() would have returned for this kind.
        TrajectoryPlayerData data = line;
        data.p1 = (wanted[k] & Mode::Player1) != 0;
        data.holding = (wanted[k] & Mode::Hold) != 0;
        keep(wanted[k], data);
    }
    if (profile) {
        m_rebuildProf.replayNs +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        m_rebuildProf.replayed += n - 1;
        int segments = 0;
        for (const DrawOp& op : m_lineOps) segments += op.hitbox ? 0 : 1;
        m_rebuildProf.segments += segments * (opaqueTop ? 1 : n);
    }
}

bool Trajectory::playerHasActivated(PlayerObject* player,
                                    EnhancedGameObject* object) {
    if (!object) {
        return false;
    }

    // The game's canMultiActivate rule (phys::activatedPlatformer): in a
    // platformer level everything not marked no-multi-activate fires again
    // after the copy leaves it, so the contact guard decides, not this set.
    if (phys::activatedPlatformer(object, player->m_isPlatformer)) {
        return false;
    }

    if (player == m_fakePlayer1) {
        return m_activatedObjectsP1.contains((uintptr_t)object);
    } else if (player == m_fakePlayer2) {
        return m_activatedObjectsP2.contains((uintptr_t)object);
    }

    return false;
}

bool Trajectory::realPlayerHasActivated(PlayerObject* player,
                                        EnhancedGameObject* object) {
    if (!object) {
        return false;
    }

    auto pl = GJBaseGameLayer::get();
    if (!isFakePlayer(player)) {
        return phys::hasBeenActivatedByPlayer(player, object);
    }

    PlayerObject* realPlayer =
        player == m_fakePlayer1 ? pl->m_player1 : pl->m_player2;

    return phys::hasBeenActivatedByPlayer(realPlayer, object);
}

static void* RingObject_spawnCircle_orig = nullptr;

void RingObject_spawnCircle(RingObject* self) {
    auto func =
        reinterpret_cast<void (*)(RingObject*)>(RingObject_spawnCircle_orig);

    auto bot = Bot::get();

    if (!bot->trajectory().drawing()) {
        func(self);
    }
}

// #ifdef GEODE_IS_WINDOWS
// $execute {
//     RingObject_spawnCircle_orig =
//         reinterpret_cast<void*>(geode::base::get() + 0x4896d0);

//     (void)Mod::get()->hook(RingObject_spawnCircle_orig,
//     &RingObject_spawnCircle,
//                            "RingObject::spawnCircle",
//                            tulip::hook::TulipConvention::Default);
// }
// #endif
