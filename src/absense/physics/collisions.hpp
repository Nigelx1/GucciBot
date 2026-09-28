#pragma once

#include <Geode/Geode.hpp>

#include "absense/compat/bot.hpp"
#include "object.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/world.hpp"

namespace phys {

int checkPlayerCollisions(GJBaseGameLayer* gameLayer, PlayerObject* player);
int collidedWithObjectInternal(PlayerObject* player, float dt,
                               GameObject* object, cocos2d::CCRect* p4,
                               bool p5);
void preSlopeCollision(PlayerObject* player, float dt, GameObject* slope);
void collidedWithSlopeInternal(PlayerObject* player);
void activateForTrajectory(EffectGameObject* obj, PlayerObject* player);
bool copyCanBeActivated(GJBaseGameLayer* pl, PlayerObject* player,
                        EffectGameObject* object);

// In the dual part the game's playerWillSwitchMode / updateDualGround take
// 'the other player' from m_player1 / m_player2 by unique id (0x140213027,
// 0x1402131fc). For a copy that is the real player 1 at the live tick, not its
// partner copy at the simulated tick, so the layer points at the copies while
// one of those calls runs.
struct CopyPlayersScope {
    GJBaseGameLayer* pl;
    PlayerObject* realP1;
    PlayerObject* realP2;
    bool swapped;
    CopyPlayersScope(GJBaseGameLayer* layer, PlayerObject* player)
        : pl(layer), realP1(layer->m_player1), realP2(layer->m_player2), swapped(false) {
        auto* traj = Bot::get()->trajectory().unsafeInner();
        if (!traj || !traj->isFakePlayer(player) || !pl->m_gameState.m_isDualMode) return;
        pl->m_player1 = traj->m_fakePlayer1;
        pl->m_player2 = traj->m_fakePlayer2;
        swapped = true;
    }
    ~CopyPlayersScope() {
        if (!swapped) return;
        pl->m_player1 = realP1;
        pl->m_player2 = realP2;
    }
    CopyPlayersScope(const CopyPlayersScope&) = delete;
    CopyPlayersScope& operator=(const CopyPlayersScope&) = delete;
};

inline bool activatingPortal(GJBaseGameLayer* pl, PlayerObject* player,
                             EffectGameObject* portal) {
    if (world::canActivate(pl, player, portal)) {
        if (Bot::get()->trajectory().playerHasActivated(player, portal)) {
            return false;
        }

        {
            CopyPlayersScope copies(pl, player);
            pl->playerWillSwitchMode(player, portal);
        }

        bool isTheSame =
            ((portal->m_objectType == GameObjectType::ShipPortal) &&
             player->m_isShip) ||
            ((portal->m_objectType == GameObjectType::BallPortal) &&
             player->m_isBall) ||
            ((portal->m_objectType == GameObjectType::UfoPortal) &&
             player->m_isBird) ||
            ((portal->m_objectType == GameObjectType::WavePortal) &&
             player->m_isDart) ||
            ((portal->m_objectType == GameObjectType::SpiderPortal) &&
             player->m_isSpider) ||
            ((portal->m_objectType == GameObjectType::SwingPortal) &&
             player->m_isSwing) ||
            ((portal->m_objectType == GameObjectType::RobotPortal) &&
             player->m_isRobot) ||
            ((portal->m_objectType == GameObjectType::CubePortal) &&
             (!(player->m_isShip || player->m_isBall || player->m_isBird ||
                player->m_isDart || player->m_isSpider || player->m_isSwing)));

        cocos2d::CCPoint position = player->getPosition();
        // if (portal->m_objectType == GameObjectType::CubePortal) {
        player->switchedToMode(portal->m_objectType);
        // }

        switch (portal->m_objectType) {
            case GameObjectType::ShipPortal:
                player->toggleFlyMode(true, true);
                break;
            case GameObjectType::BallPortal:
                player->toggleRollMode(true, true);
                break;
            case GameObjectType::UfoPortal:
                player->toggleBirdMode(true, true);
                break;
            case GameObjectType::WavePortal:
                player->toggleDartMode(true, true);
                break;
            case GameObjectType::SpiderPortal:
                player->toggleSpiderMode(true, true);
                break;
            case GameObjectType::SwingPortal:
                player->toggleSwingMode(true, true);
                break;
            case GameObjectType::RobotPortal:
                player->toggleRobotMode(true, true);
                break;
            default:
                break;
        }

        player->setPosition(position);
        player->m_iconSprite->setPosition(position);
        player->m_vehicleSprite->setPosition(position);

        player->m_lastPortalPos = portal->getPosition();
        player->m_lastActivatedPortal = portal;
        activateForTrajectory(portal, player);
        Bot::get()->trajectory().noteFakeModeFromPortal(player, portal->m_objectType);

        return !isTheSame;
    }

    return false;
}

void bumpPlayerFromGJBGL(GJBaseGameLayer* pl, PlayerObject* player,
                         EffectGameObject* object);

// int handleRotatedCollisionInternal(
//     PlayerObject* player,
//     float dt,
//     GameObject* object,
//     cocos2d::CCRect* rect,
//     bool p3,
//     bool p4,
//     bool isSlope
// ) {
//     cocos2d::CCPoint p1 = player->getPosition();
//     player->rotateGameplayObject(object);
//     for (auto& obj : player->m_maybeRotatedObjectsMap) {
//         player->rotateGameplayObject(obj.second);
//     }

//     int ret = 0;
//     if (isSlope) {
//         player->collidedWithSlopeInternal(dt, object, p4);
//     } else {
//         ret = player->collidedWithObjectInternal(dt, object, *rect, p3);
//     }

//     cocos2d::CCPoint newPos = {
//         player->getPosition().y - p1.y + p1.x,
//         p1.y - player->getPosition().x + p1.x,
//     };
//     player->setPosition(newPos);
//     player->unrotateGameplayObject(object);
//     for (auto& obj : player->m_maybeRotatedObjectsMap) {
//         player->unrotateGameplayObject(obj.second);
//     }

//     return ret;
// }

void collisionCheckObjects(GJBaseGameLayer* pl, PlayerObject* player,
                           gd::vector<GameObject*>* objects, int objectCount,
                           float dt);

void triggerObject(EffectGameObject* obj, GJBaseGameLayer* pl,
                   PlayerObject* player);
void checkSpawnObjects(GJBaseGameLayer* pl, PlayerObject* player);
}  // namespace phys
