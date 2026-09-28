#include "gravity.hpp"

#include <Geode/Geode.hpp>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/materialize.hpp"

using namespace geode::prelude;

namespace phys {
void flipGravityInner(PlayerObject* player, bool gravity) {
    bool ignoreUpsideDown =
        player->m_isDart && Bot::get()->replaySystem().m_maintainGravity;

    if (!ignoreUpsideDown) {
        player->m_isUpsideDown = gravity;
    }

    player->m_lastFlipTime = player->m_totalTime;
    player->m_collidedBottomMaxY = 0.;
    player->m_collidedTopMinY = 0.;
    player->m_unkA29 = false;
    if (player->m_wasOnSlope || player->m_isOnSlope) {
        player->m_slopeFlipGravityRelated = !player->m_slopeFlipGravityRelated;
    }

    player->m_collisionLogTop->removeAllObjects();
    player->m_collisionLogBottom->removeAllObjects();
    player->m_collisionLogLeft->removeAllObjects();
    player->m_collisionLogRight->removeAllObjects();

    player->m_unk50C = -1;
    player->m_unk510 = -1;
    player->m_lastCollisionBottom = -1;
    player->m_lastCollisionTop = -1;
    player->m_lastCollisionLeft = -1;
    player->m_lastCollisionRight = -1;

    player->m_yVelocity = player->m_yVelocity * 0.5;
    player->m_isOnGround = false;
    if (player->m_isBall) {
        player->m_isRotating = false;
        player->m_isBallRotating = false;
        player->m_isBallRotating2 = false;
        player->m_rotationSpeed = 0.;
        player->runBallRotation2();
    }
}

void flipGravity(PlayerObject* player, bool gravity) {
    if (player->m_isUpsideDown == gravity) return;

    auto& gameState = GJBaseGameLayer::get()->m_gameState;

    flipGravityInner(player, gravity);
    if (Bot::get()->trajectory().isFakePlayer(player) &&
        !gameState.m_unkBool31 && gameState.m_isDualMode &&
        !GJBaseGameLayer::get()->m_levelSettings->m_twoPlayerMode) {
        auto p1 = player;
        auto p2 = Bot::get()->trajectory().getOtherPlayer(player);
        if (!(p1->m_isShip == p2->m_isShip && p1->m_isBall == p2->m_isBall &&
              p1->m_isBird == p2->m_isBird &&
              p1->m_isSpider == p2->m_isSpider &&
              p1->m_isRobot == p2->m_isRobot &&
              p1->m_isSwing == p2->m_isSwing)) {
            return;
        }

        flipGravityInner(p2, !gravity);
    }
}

void propellPlayer(PlayerObject* player, float force, bool, int) {
    player->m_maybeIsBoosted = true;
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    player->m_isOnSlope = false;
    player->m_wasOnSlope = false;

    float gravity = player->m_vehicleSize != 1.0 ? 0.8 : 1.0;
    float gravityCoefficient = player->m_isUpsideDown ? -1 : 1;
    double velocity = force * gravity * gravityCoefficient * 16.0;
    player->setYVelocity(velocity, 0);

    if (player->m_isBall || player->m_isSpider || player->m_isSwing) {
        player->m_yVelocity *= 0.6;
    }

    if (!player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating = false;
        player->m_isBallRotating2 = false;
        player->m_rotationSpeed = 0.;

        if (player->m_isBall) {
            player->runBallRotation(1.0);
        } else {
            player->runNormalRotation(0, 1.0);
        }
    }

    player->m_lastGroundedPos = player->getPosition();
}

void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object) {
    if (player->m_isPlatformer || !player->m_fixRobotJump) {
        player->m_touchedPad = true;
    }

    if (p2 != 0x2c) {
        propellPlayer(player, force, playBumpEffect, p2);
        player->m_isAccelerating = false;
        if (p2 == 0x22) {
            player->m_isAccelerating = true;
            player->m_lastGroundedPos = CCPoint{0.0, 0.0};
        }

        return;
    }

    // Spider pad. Its test looks for the floor or ceiling up or down the
    // screen, so the objects a run with the World moves are all put where the
    // run has them first, not only the ones near the copy (world/materialize.hpp).
    world::catchUpCurrentRun();
    if (!object) {
        player->spiderTestJumpInternal(false);
        return;
    }

    if (!player->m_isSideways) {
        if (player->m_isUpsideDown != object->isFacingDown()) {
            flipGravity(player, !player->m_isUpsideDown);
        }
    } else {
        if (player->m_isUpsideDown != object->isFacingLeft()) {
            flipGravity(player, !player->m_isUpsideDown);
        }
    }

    player->spiderTestJumpInternal(false);
}
}  // namespace phys
