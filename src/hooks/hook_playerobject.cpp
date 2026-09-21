#include "core/GucciBot.hpp"
#include "analysis/ac/cbf.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/trajectory.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
using namespace geode::prelude;

using namespace gucci;

class $modify(GB7PlayerObject, PlayerObject) {
    // Splits one physics tick into sub-steps so an input can land BETWEEN
    // them, which is what makes sub-tick (CBF) frame windows mean anything.
    // Ported from Silicate 2026-09-20. Without it the CBF engine still armed
    // and the analyzer still reported sub-tick numbers, but no tick was ever
    // actually split -- every "sub-tick" leg was really a whole-frame leg, so
    // the fractions it printed were measuring nothing.
    //
    // Only player 1 drives this; player 2 is stepped from inside the same
    // loop, and m_p2Handled tells the p2 update to stand down for that tick.
    bool cbfSplitUpdate(float stepDelta) {
        auto* eng = cbf::Engine::get();
        auto* pl = PlayLayer::get();
        if (!pl || this != pl->m_player1)
            return false;
        if (eng->m_midStep)
            return false;

        eng->m_p2Handled = false;

        if (!eng->beginTick()) {
            eng->m_p1Split = false;
            eng->m_p2Split = false;
            return false;
        }

        PlayerObject* p2 = pl->m_player2;
        bool const isDual = pl->m_gameState.m_isDualMode;

        bool const p1StartedOnGround = this->m_isOnGround;
        bool const p2StartedOnGround = p2 && p2->m_isOnGround;

        bool const tickGround = SLSettings::get()->frameWindow.cbfTickGround;

        bool const p1NotBuffering = cbf::canSplit(this, tickGround);
        bool const p2NotBuffering = p2 && cbf::canSplit(p2, tickGround);

        eng->m_p1Pos = this->getPosition();
        eng->m_p2Pos = p2 ? p2->getPosition() : cocos2d::CCPoint{};

        eng->m_p1Split = p1NotBuffering;
        eng->m_p2Split = p2NotBuffering && isDual;

        eng->m_midStep = true;
        eng->m_shipRotAccum = 0.f;
        eng->m_shipRotAccumP2 = 0.f;
        eng->m_shipRotHeld = true;

        bool firstLoop = true;
        cbf::Step step;

        do {
            step = eng->pop();
            float const substepDelta = stepDelta * static_cast<float>(step.deltaFactor);
            eng->m_rotationDelta = substepDelta;

            if (eng->m_p1Split) {
                PlayerObject::update(substepDelta);
                if (!step.endStep) {
                    if (tickGround && firstLoop &&
                        ((this->m_yVelocity < 0) ^ this->m_isUpsideDown))
                        this->m_isOnGround = p1StartedOnGround;

                    if (!this->m_isOnSlope || this->m_isDart)
                        pl->checkCollisions(this, 0.0f, true);
                    else
                        pl->checkCollisions(this, stepDelta, true);

                    PlayerObject::updateRotation(substepDelta);

                    cbf::resetCollisionLog(this);
                }
            } else if (step.endStep) {
                PlayerObject::update(stepDelta);
            }

            if (eng->m_p2Split && p2) {
                p2->update(substepDelta);
                if (!step.endStep) {
                    if (tickGround && firstLoop &&
                        ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown))
                        p2->m_isOnGround = p2StartedOnGround;

                    if (!p2->m_isOnSlope || p2->m_isDart)
                        pl->checkCollisions(p2, 0.0f, true);
                    else
                        pl->checkCollisions(p2, stepDelta, true);

                    p2->updateRotation(substepDelta);
                    cbf::resetCollisionLog(p2);
                }
            } else if (step.endStep && p2) {
                p2->update(stepDelta);
            }

            firstLoop = false;

            if (!step.endStep)
                eng->fire();
        } while (!step.endStep && !eng->exhausted());

        eng->m_shipRotHeld = false;
        if (eng->m_shipRotAccum != 0.f) {
            PlayerObject::updateShipRotation(eng->m_shipRotAccum);
            eng->m_shipRotAccum = 0.f;
        }
        if (p2 && eng->m_shipRotAccumP2 != 0.f) {
            p2->updateShipRotation(eng->m_shipRotAccumP2);
            eng->m_shipRotAccumP2 = 0.f;
        }

        eng->m_midStep = false;
        eng->m_p2Handled = p2 != nullptr;
        eng->endTick();
        return true;
    }

    void update(float dt) {
        auto& upd = GucciEngine::get()->updater;
        auto& traj = TrajectoryPredictionService::get();
        bool const real = !traj.ownsPreviewPlayer(this);

        if (real)
            upd.m_lastPlayerX = upd.m_currentPlayerX;

        // Player 2 was already stepped inside player 1's split loop this tick.
        if (auto* eng = cbf::Engine::get(); eng->m_p2Handled && !eng->m_midStep) {
            if (auto* pl = PlayLayer::get(); pl && this == pl->m_player2) {
                eng->m_p2Handled = false;
                return;
            }
        }

        // Simulated players are never split: a fork exists to answer one
        // question about physics, and splitting its ticks would change the
        // physics it is being asked about.
        if (real && this->cbfSplitUpdate(dt)) {
            upd.m_currentPlayerX = this->getPositionX();
            return;
        }

        PlayerObject::update(dt);

        if (real)
            upd.m_currentPlayerX = this->getPositionX();
    }

    // While a tick is split, rotation must advance by the SUB-step delta and
    // measure from the position the tick started at -- otherwise the player
    // visibly spins by a full tick's worth on every sub-step.
    void updateRotation(float t) {
        auto* eng = cbf::Engine::get();
        auto* pl = PlayLayer::get();

        if (pl && !eng->m_midStep && eng->m_p1Split && this == pl->m_player1) {
            PlayerObject::updateRotation(eng->m_rotationDelta);
            this->m_lastPosition = eng->m_p1Pos;
            return;
        }
        if (pl && !eng->m_midStep && eng->m_p2Split && this == pl->m_player2) {
            PlayerObject::updateRotation(eng->m_rotationDelta);
            this->m_lastPosition = eng->m_p2Pos;
            return;
        }

        PlayerObject::updateRotation(t);
    }

    // Ship rotation accumulates across the sub-steps and is applied once at
    // the end of the tick, so a split tick rotates the ship exactly as far as
    // an unsplit one would.
    void updateShipRotation(float t) {
        auto* eng = cbf::Engine::get();
        if (eng->m_shipRotHeld) {
            auto* pl = PlayLayer::get();
            if (pl && this == pl->m_player2)
                eng->m_shipRotAccumP2 += t;
            else
                eng->m_shipRotAccum += t;
            return;
        }

        PlayerObject::updateShipRotation(t);
    }

    void playDeathEffect() {
        auto& upd = GucciEngine::get()->updater;
        if (upd.m_preventDeath || upd.m_predicting)
            return;
        PlayerObject::playDeathEffect();
    }

    void playSpawnEffect() {
        if (GucciEngine::get()->practiceFix.m_loadCheckpoint)
            return;
        if (::Bot::get()->frameWindow().hideSpawnEffects())
            return;
        PlayerObject::playSpawnEffect();
    }

    void spawnCircle() {
        if (::Bot::get()->frameWindow().hideSpawnEffects())
            return;
        PlayerObject::spawnCircle();
    }

    void releaseAllButtons() {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_canDie || gb->isPlaying())
            PlayerObject::releaseAllButtons();
    }
};
