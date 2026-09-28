#include "collisions.hpp"

#include <Geode/Enums.hpp>
#include <Geode/Geode.hpp>

#include "absense/compat/bot.hpp"
#include "gravity.hpp"
#include "player.hpp"
#include "absense/trajectory/trajectory.hpp"

namespace phys {
int checkPlayerCollisions(GJBaseGameLayer* gameLayer, PlayerObject* player) {
    // bool wasJustTeleported = player->m_wasTeleported;
    player->m_wasTeleported = false;
    player->m_ringJumpRelated = false;
    player->m_collidedTopMinY = 0.0;
    player->m_collidedBottomMaxY = 0.0;
    player->m_collidedLeftMaxX = 0.0;
    player->m_collidedRightMinX = 0.0;

    player->m_wasOnSlope = player->m_isOnSlope;
    player->m_isOnSlope = false;

    bool isOnGround = player->m_isOnGround2;
    player->m_isOnGround4 = isOnGround;
    if (isOnGround && !player->m_platformerMovingLeft &&
        !player->m_platformerMovingRight && player->m_maybeSlidingTime > 0) {
        player->m_maybeSlidingTime = 0;
        player->m_maybeSlidingStartTime = player->m_totalTime;
    }

    if (player->m_unk669) {
        player->m_currentPotentialSlope = nullptr;
    }
    [[maybe_unused]] GameObject* addedSlope;
    player->m_unk669 = true;
    player->m_potentialSlopeMap.clear();
    GameObject* currentSlope = player->m_currentPotentialSlope;
    if (currentSlope) {
        player->m_potentialSlopeMap.insert(
            {currentSlope->m_uniqueID, currentSlope});
        addedSlope = currentSlope;
    }

    currentSlope = player->m_currentSlope;
    if (currentSlope) {
        player->m_potentialSlopeMap.insert(
            {currentSlope->m_uniqueID, currentSlope});
        addedSlope = currentSlope;
    }

    float someValue = 0.0;
    float vehicleSize = player->m_vehicleSize;
    float unkAngle = player->m_unkAngle1;
    if (vehicleSize != 1.0) {
        someValue = (1.0 - vehicleSize) * unkAngle * 0.5;
    }
    float unkAngleHalved = unkAngle * 0.5;
    float angleTransformed = (unkAngleHalved + 90.0) - someValue;

    bool groundExists =
        !gameLayer->m_gameState.m_isFreeMode &&
        (player->m_isShip || player->m_isBird || player->m_isDart ||
         player->m_isSwing || player->m_isBall || player->m_isSpider ||
         gameLayer->m_gameState.m_isDualMode);

    // bool isOutOfBounds = player->m_isOutOfBounds;
    player->m_isOutOfBounds = false;

    [[maybe_unused]] bool exceededBounds = false;

    if (gameLayer->m_isPlatformer) {
        auto playerPosition = player->getPosition();
        if (playerPosition.x < -30.0) {
            player->m_platformerXVelocity = 0.0;
            player->setPosition({-30.0, playerPosition.y});
        }
    } else if (player->m_isGoingLeft) {
        exceededBounds = player->getPosition().x < -30.0;
    }

    if (angleTransformed <= player->getPosition().y ||
        (groundExists &&
         (player->m_isShip || player->m_isBird || player->m_isDart ||
          player->m_isSwing || player->m_isBall || player->m_isSpider ||
          gameLayer->m_gameState.m_isDualMode))) {
        if (player->getPosition().y >
            (float)(someValue + gameLayer->m_maxGameplayY)) {
            exceededBounds = true;
        }
    } else if (player->m_isUpsideDown) {
        if (player->m_lastFlipTime != 0 &&
            player->m_totalTime - player->m_lastFlipTime < 0.1) {
            auto playerPosition = player->getPosition();
            player->setPosition({-1.0842022e-19, playerPosition.x});
            player->hitGround(nullptr, true);
            player->updateCollide(PlayerCollisionDirection::Top, nullptr);
            player->m_isOnGround2 = false;
            return 0;
        }
        exceededBounds = true;
    } else if (!player->m_maybeIsBoosted) {
        // i don't know
    }

    if (!groundExists || player->m_wasTeleported) {
        // some goto
    }

    // TODO
    return 0;
}

int collidedWithObjectInternal(PlayerObject* player, float, GameObject* object,
                               CCRect* p4, bool) {
    [[maybe_unused]] bool holdingLeft = player->m_holdingLeft;
    [[maybe_unused]] bool holdingRight = player->m_holdingLeft;
    if (player->m_leftPressedFirst) {
        holdingRight = false;
    } else {
        holdingLeft = false;
    }

    GameObject* someObj = nullptr;
    cocos2d::CCRect playerRect;
    // cocos2d::CCRect objectRect =
    //     object ? object->getObjectRect() : cocos2d::CCRect{0, 0, 0, 0};
    if (p4->equals(cocos2d::CCRect{0.0, 0.0, 0.0, 0.0})) {
        someObj = object;
        playerRect = player->getObjectRect();
        if (object) {
            *p4 = object->getObjectRect();
            playerRect = object->getObjectRect();  // yea idk either
        }
    } else {
        someObj = nullptr;
        playerRect = player->getObjectRect();
    }

    float unk1 = 5.0;
    if (!player->m_isPlatformer || player->m_wasOnSlope ||
        player->m_isOnSlope) {
        unk1 = 10.0;
    }
    unk1 = player->m_stateScale > 0 ? 15.0 : unk1;

    double unk2 = player->m_isUpsideDown ? -unk1 : unk1;
    if ((player->m_isShip || player->m_isBird || player->m_isDart ||
         player->m_isSwing) &&
        !player->m_isPlatformer) {
        unk2 = player->m_isUpsideDown ? -6.0 : 6.0;
    }
    if (player->m_wasOnSlope) {
        unk2 = unk2 + (player->m_isUpsideDown ? -1.0 : 1.0) * player->unk_584;
    }

    // bool idk = false;
    if (someObj) {
        // idk = someObj->m
    }

    return 0;
}

void preSlopeCollision(PlayerObject* player, float dt, GameObject* slope) {
    if (slope->m_uniqueID == player->m_collidingWithSlopeId) return;

    cocos2d::CCRect playerRect = player->getObjectRect();
    cocos2d::CCRect slopeRect = slope->getObjectRect();

    int slopeDir = slope->m_slopeDirection;

    [[maybe_unused]] bool someBool = false;

    bool unk1 = false;
    if ((slopeDir < 5) || slopeDir == 6) {
        unk1 = true;
    }

    [[maybe_unused]] bool unk2 = false;
    if ((slopeDir < 7) && ((0x6aU >> (slopeDir & 0x1f) & 1) != 0)) {
        unk2 = true;
    }

    float unk3 = 5.0;
    if (!player->m_isPlatformer ||
        (!player->m_wasOnSlope && !player->m_isOnSlope)) {
        unk3 = 0.0;
    }

    cocos2d::CCPoint pos = slope->getRealPosition();
    cocos2d::CCPoint diff = pos - slope->m_lastPosition;

    if (player->m_isSideways) {
        diff.x = diff.y;
    }

    float playerSpeed = player->m_playerSpeed * player->m_speedMultiplier * dt;
    float slopePosX = slope->getPosition().x;
    int unk4 = 0;
    if (unk1) {
        if ((!player->m_isGoingLeft || player->m_isPlatformer ||
             diff.x < playerSpeed) &&
            slopePosX < slopeRect.origin.x) {
            unk4 = (!player->m_isGoingLeft || player->m_isPlatformer) ? 0 : 1;
            unk3 += unk4;
            float heightDiff = slopeRect.size.height - (unk3 * 2);
            playerSpeed = slopeRect.origin.x;
            cocos2d::CCRect newPlayerRect = {
                playerSpeed, unk3 + slopeRect.origin.y, 1.0, heightDiff};
            someBool = true;
            if (newPlayerRect.intersectsRect(playerRect)) {
                player->collidedWithObjectInternal(dt, slope, newPlayerRect,
                                                   false);
            }
        }
    } else if (player->m_isGoingLeft || player->m_isPlatformer ||
               (playerSpeed < diff.x)) {
        playerSpeed = slopeRect.getMaxX();
        if (playerSpeed < slopePosX) {
            unk4 = (!player->m_isGoingLeft && !player->m_isPlatformer) ? 1 : 0;
            playerSpeed = (slopeRect.size.width + slopeRect.origin.x) - 1.0;
            unk3 += unk4;
            float heightDiff = slopeRect.size.height - (unk3 * 2);
            cocos2d::CCRect newPlayerRect = {
                playerSpeed, unk3 + slopeRect.origin.y, 1.0, heightDiff};
            someBool = true;
            if (newPlayerRect.intersectsRect(playerRect)) {
                player->collidedWithObjectInternal(dt, slope, newPlayerRect,
                                                   false);
            }
        }
    }
}

void collidedWithSlopeInternal(PlayerObject*) {}

void activateForTrajectory(EffectGameObject* obj, PlayerObject* player) {
    auto bot = Bot::get();

    bot->trajectory().rememberActivatedObject(obj, player);
}

// GJBaseGameLayer::canBeActivatedByPlayer (0x2178c0) for a copy. The game
// stamps the (object, player) contact map, refuses a multi-activate object the
// player is still touching, and otherwise asks hasBeenActivatedByPlayer
// (0x1a4af0) - which picks the flag by m_uniqueID == 1. A copy's id never is,
// so the game's own call read player 2's flag for the copy of player 1; here
// the flag is picked by which copy it is.
// This is the path a run without the World keeps: the copies stamp the live
// contact map, and nothing ages their entries in the drawn prediction. With
// the World, world::canActivate applies the same rule to the run's own
// WorldState.
bool copyCanBeActivated(GJBaseGameLayer* pl, PlayerObject* player,
                        EffectGameObject* object) {
    auto& ids = pl->m_gameState.m_activatedObjectIDs;
    const std::pair<int, int> key{object->m_uniqueID, player->m_uniqueID};
    const bool multi = object->canMultiActivate(player->m_isPlatformer);
    const bool touching = multi && ids.contains(key);
    ids[key] = static_cast<int>(pl->m_gameState.m_commandIndex);
    if (touching) return false;
    if (multi) return true;
    const bool isP2 = Bot::get()->trajectory().unsafeInner()->m_fakePlayer2 == player;
    return !(isP2 ? object->m_activatedByPlayer2 : object->m_activatedByPlayer1);
}

void bumpPlayerFromGJBGL(GJBaseGameLayer* pl, PlayerObject* player,
                         EffectGameObject* object) {
    if (world::canActivate(pl, player, object)) {
        // cocos2d::CCPoint p = cocos2d::CCPoint{0, -10};
        cocos2d::CCPoint objPos = object->getPosition();

        player->m_lastPortalPos = objPos;
        activateForTrajectory(object, player);
        float force = 1.0;

        if (object->m_objectType == GameObjectType::PinkJumpPad) {
            if (player->m_isShip) {
                force = 0.35;
            } else if (player->m_isBird) {
                force = 0.4;
            } else if (player->m_isBall) {
                force = 0.7;
            } else if (player->m_isSpider) {
                force = 0.7;
            } else {
                force = 0.65;
            }
        } else if (object->m_objectType == GameObjectType::RedJumpPad) {
            if (player->m_isShip) {
                if (player->m_vehicleSize >= 1.0) {
                    force = 0.63;
                } else {
                    force = 0.95;
                }
            } else if (player->m_isBird) {
                if (player->m_vehicleSize >= 1.0) {
                    force = 0.6;
                } else {
                    force = 0.98;
                }
            } else {
                force = 1.25;
            }
        }
        player->m_lastActivatedPortal = object;
        // GJBaseGameLayer::bumpPlayer 0x217b29-0x217b3f: a pad with Reverse
        // turns the player round before the throw.
        if (object->m_isReverse) {
            player->reversePlayer(object);
        }
        phys::bumpPlayer(player, force, static_cast<int>(object->m_objectType),
                         false, object);
    }
}

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

static void* g_PlayerObject_getOrientedBox = nullptr;
static void* g_PlayerObject_updateOrientedBox = nullptr;

$execute {
    g_PlayerObject_getOrientedBox =
        reinterpret_cast<void*>(geode::base::get() + 0x38a8c0);
    g_PlayerObject_updateOrientedBox =
        reinterpret_cast<void*>(geode::base::get() + 0x19e5f0);
}

void collisionCheckObjects(GJBaseGameLayer* pl, PlayerObject* player,
                           gd::vector<GameObject*>* objects, int objectCount,
                           float dt) {
    if (objectCount <= 0) return;

    CCRect playerRect = player->getObjectRect();

    [[maybe_unused]] float playerMinX = playerRect.getMinX();
    [[maybe_unused]] float playerMaxX = playerRect.getMaxX();
    [[maybe_unused]] float playerMinY = playerRect.getMinY();
    [[maybe_unused]] float playerMaxY = playerRect.getMaxY();

    for (int i = 0; i < objectCount; i++) {
        GameObject* object = objects->at(i);

        if (!object) continue;

        if (object->m_objectType == GameObjectType::Decoration ||
            object->m_objectType == GameObjectType::CollisionObject ||
            object->m_objectType == GameObjectType::SecretCoin ||
            object->m_objectType == GameObjectType::UserCoin ||
            object->m_objectType == GameObjectType::Collectible ||
            object->m_objectType == GameObjectType::EnterEffectObject ||
            object->m_isGroupDisabled || object->m_isDisabled)
            continue;

        if (object->m_objectType == GameObjectType::Solid ||
            object->m_objectType == GameObjectType::Breakable) {
            if (pl->m_solidCollisionObjectsCount <
                pl->m_solidCollisionObjectsIndex) {
                pl->m_solidCollisionObjects.at(
                    pl->m_solidCollisionObjectsCount) = object;
                pl->m_solidCollisionObjectsCount++;
            } else {
                pl->m_solidCollisionObjects.push_back(object);
                pl->m_solidCollisionObjectsCount++;
                pl->m_solidCollisionObjectsIndex++;
            }

            continue;
        }

        if (object == pl->m_anticheatSpike) continue;

        if (object->m_objectType == GameObjectType::Hazard ||
            object->m_objectType == GameObjectType::AnimatedHazard) {
            if (pl->m_hazardCollisionObjectsCount <
                pl->m_hazardCollisionObjectsIndex) {
                pl->m_hazardCollisionObjects.at(
                    pl->m_hazardCollisionObjectsCount) = object;
                pl->m_hazardCollisionObjectsCount++;
            } else {
                pl->m_hazardCollisionObjects.push_back(object);
                pl->m_hazardCollisionObjectsCount++;
                pl->m_hazardCollisionObjectsIndex++;
            }

            continue;
        }

        auto bot = Bot::get();
        EffectGameObject* obj = (EffectGameObject*)object;
        if (!obj) continue;

        cocos2d::CCRect rect;
        if (object->m_objectType == GameObjectType::Slope) {
            rect = object->getObjectRect(2.0, 2.0);
        } else {
            rect = object->getObjectRect();
        }

        // Overlap first: a few compares on a rect the object keeps, where the
        // activation checks are a hash lookup and a call - and nearly every
        // object of the sections around the player misses it.
        if (object->m_objectRadius <= 0.0) {
            if (!playerRect.intersectsRect(rect)) continue;
        } else if (!pl->playerCircleCollision(player, object)) {
            continue;
        }

        if ((bot->trajectory().playerHasActivated(player, obj) ||
             bot->trajectory().realPlayerHasActivated(player, obj)) &&
            (object->m_objectType != GameObjectType::Slope))
            continue;

        bool overlaps = true;

        if (object->m_shouldUseOuterOb &&
            (!pl->m_levelSettings->m_fixRadiusCollision ||
             object->m_objectRadius <= 0.0)) {
            OBB2D* box = object->getOrientedBox();
            player->updateOrientedBox();
            OBB2D* playerBox = ((GameObject*)(player))->getOrientedBox();
            // 0x214bb0 / 0x214bbf: a one-way test only tries the object's own
            // axes, so both boxes' axes have to fail to separate them.
            overlaps = box->overlaps1Way(playerBox) && playerBox->overlaps1Way(box);
        }

        if (object->m_objectType == GameObjectType::Slope) {
            rect = object->getObjectRect();
        }

        if (!overlaps) continue;

        // 0x214c01-0x214c20: while a rotate-gameplay trigger has switched
        // channel, effect objects (m_classType 1, +0x4e8) of another channel
        // (+0x700) are not touched. It sits where the game has it, after the
        // overlap test and before the kind switch. The channel read here is
        // the run's own: a copy's rotate-gameplay trigger writes it, and every
        // snapshot, kept start and end() puts it back.
        if (pl->m_gameState.m_currentChannel != 0 &&
            object->m_classType == GameObjectClassType::Effect &&
            obj->m_channelValue != 0 &&
            obj->m_channelValue != pl->m_gameState.m_currentChannel) {
            continue;
        }

        switch (object->m_objectType) {
            case GameObjectType::InverseGravityPortal:
                // Once per contact, as canBeActivatedByPlayer lets it: a portal
                // that fires again (multi-activate, or any in a platformer level)
                // is not taken again while the copy is still in it.
                if (!world::canActivate(pl, player, obj)) break;
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, true);
                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();
                break;
            case GameObjectType::NormalGravityPortal:
                if (!world::canActivate(pl, player, obj)) break;  // once per contact (see above)
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, false);
                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();
                break;
            case GameObjectType::GravityTogglePortal:
                if (!world::canActivate(pl, player, obj)) break;  // once per contact, or it flips back every tick
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, !player->m_isUpsideDown);
                break;
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
                // 0x215840 / 0x2157b6: the contact rule first, the portal is
                // remembered on the player, and then toggleDualMode brings the
                // other player in or takes it out. The run switches there and
                // then, because the game reads the dual flag again after
                // player 1's collision pass (update 0x238517) and gives the new
                // player its own tick in the same step. The event that follows
                // it (0x2158dc / 0x215836) goes to the run's own listeners: the
                // hook that keeps the copies out of the real effect manager
                // hands it to world::onEvent.
                if (!world::canActivate(pl, player, obj)) break;
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                bot->trajectory().dualPortal(pl, player, obj,
                                             object->m_objectType == GameObjectType::DualPortal);
                pl->gameEventTriggered(object->m_objectType == GameObjectType::DualPortal
                                           ? GJGameEvent::PortalDualOn
                                           : GJGameEvent::PortalDualOff,
                                       0, 0);
                break;
            case GameObjectType::TeleportPortal:
                if (world::canActivate(pl, player, (EffectGameObject*)object)) {
                    phys::teleportPlayer(pl, (TeleportPortalObject*)object,
                                         player);
                    activateForTrajectory(obj, player);
                    // 0x21557f -> 0x215e78: the rest of the pass tests the
                    // player where it was teleported to.
                    playerRect = player->getObjectRect();
                }
                break;
            case GameObjectType::Slope:
                if (!player->m_isSideways) {
                    player->collidedWithSlopeInternal(dt, object, false);
                } else {
                    cocos2d::CCRect emptyRect =
                        cocos2d::CCRect{0.0, 0.0, 0.0, 0.0};

                    player->handleRotatedCollisionInternal(
                        dt, object, emptyRect, false, false, true);
                }

                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();
                // 0x215e78: the rest of the pass tests the player where the
                // slope put it.
                playerRect = player->getObjectRect();

                break;
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
                if (!player->m_touchingRings->containsObject(object)) {
                    player->m_touchingRings->addObject(object);
                }
                player->m_touchedRings.insert(object->m_uniqueID);
                bot->trajectory().noteRingContact(player, object);

                // GJBaseGameLayer::playerTouchedRing (0x217e40): fired on contact
                // unless the player flies or the ring claims the touch
                // (m_claimTouch, +0x740). It only counts as used once it really
                // fires - ringJump marks it - so a press later in the contact
                // still takes it.
                if (!player->m_isShip && !player->m_isBird &&
                    !player->m_isDart && !player->m_isSwing &&
                    !((RingObject*)object)->m_claimTouch) {
                    phys::ringJump(player, (RingObject*)object);
                }
                // 0x215a50: after a teleport orb, fired or not, the rest of the
                // pass tests the player where it now is.
                if (object->m_objectType == GameObjectType::TeleportOrb) {
                    playerRect = player->getObjectRect();
                }
                break;
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
                phys::bumpPlayerFromGJBGL(pl, player, obj);
                break;
            case GameObjectType::GravityPad: {
                bool isFacingDown = false;
                if (player->m_isSideways) {
                    isFacingDown = object->isFacingLeft();
                } else {
                    isFacingDown = object->isFacingDown();
                }

                // gravBumpPlayer asks the orientation first and only then
                // canBeActivatedByPlayer, so a pad met with gravity the wrong way
                // round leaves no contact stamp and still fires once flipped.
                if (player->m_isUpsideDown == isFacingDown && world::canActivate(pl, player, obj)) {
                    if (obj->m_isReverse) {
                        player->reversePlayer(obj);
                    }

                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::propellPlayer(player, 0.8, false, 10);
                    phys::flipGravity(player, !isFacingDown);
                    player->m_padRingRelated = true;
                }
                break;
            }
            case GameObjectType::MiniSizePortal:
                if (world::canActivate(pl, player, obj)) {
                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::togglePlayerScale(player, true);
                }
                break;
            case GameObjectType::RegularSizePortal:
                if (world::canActivate(pl, player, obj)) {
                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::togglePlayerScale(player, false);
                }
                break;
            case GameObjectType::Special:
                if (object->m_objectID == 0x743) {
                    player->m_stateHitHead = 2;
                } else if (object->m_objectID == 0x6db) {
                    player->m_stateDartSlide = 2;
                } else if (object->m_objectID == 0x715) {
                    player->m_stateNoAutoJump = 2;
                } else if (object->m_objectID == 0x725) {
                    // collisionCheckObjects 0x215a87-0x215aa1: only a dashing
                    // player is stopped, and nothing else is set. The only write
                    // of the head-hit state is the 1859 case (0x215ac6); Geode's
                    // reconstructed touchedObject falls through, the game does not.
                    if (player->m_isDashing) {
                        phys::stopDashing(player);
                        player->m_jumpBuffered = false;
                    }
                } else if (object->m_objectID == 0xb32) {
                    player->m_stateFlipGravity = 2;
                } else if (object->m_objectID == 2069 ||
                           object->m_objectID == 3645) {  // 0x815 / 0xe3d, compared at 0x215ad6-0x215aea
                    player->m_stateForce = 2;
                    ForceBlockGameObject* forceBlock =
                        (ForceBlockGameObject*)object;
                    int forceID = forceBlock->m_forceID;
                    if (forceID > 0) {
                        if (player->m_jumpPadRelated.contains(forceID)) {
                            if (player->m_jumpPadRelated.at(forceID)) {
                                break;
                            }
                        }
                        player->m_jumpPadRelated.insert({forceID, true});
                    }

                    // void* fnPtr = reinterpret_cast<void*>(geode::base::get()
                    // + 0x4a9370); CCPoint force =
                    // (reinterpret_cast<CCPoint(*)(ForceBlockGameObject*,
                    // CCPoint)>(fnPtr))(forceBlock, player->getPosition());
                    CCPoint force = forceBlock->calculateForceToTarget(player);
                    player->m_stateForceVector += force;
                    // 0x215b7b-0x215be8: the summed force is capped at a length
                    // of 9999, so overlapping strong force blocks push no further
                    // than that.
                    const float forceLength =
                        sqrtf(player->m_stateForceVector.x * player->m_stateForceVector.x +
                              player->m_stateForceVector.y * player->m_stateForceVector.y);
                    if (forceLength > 9999.0f) {
                        player->m_stateForceVector =
                            player->m_stateForceVector * (9999.0f / forceLength);
                    }
                }
                break;
            case GameObjectType::CubePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::RobotPortal:
                // 0x215290 / 0x2150d1: a platformer level ignores wave and
                // swing portals.
                if (pl->m_isPlatformer &&
                    (object->m_objectType == GameObjectType::WavePortal ||
                     object->m_objectType == GameObjectType::SwingPortal)) {
                    break;
                }
                activatingPortal(pl, player, obj);
                break;
            case GameObjectType::EnterEffectObject:
            case GameObjectType::Modifier:
                // Once per contact as well: a speed portal or other modifier
                // that fires again is not taken on every tick the copy is in it.
                if (world::canActivate(pl, player, obj)) {
                    // The game's playerTouchedTrigger 0x217f50 never calls
                    // activatedByPlayer: it returns at once for an object that
                    // is not touch triggered (+0x5d0), and a touch trigger that
                    // fires is marked by triggerObject below. The call (the
                    // EnhancedGameObject hook turns it into the copy's activated
                    // list) filed every modifier a copy's hitbox brushed as
                    // fired whether it fired or not, and one the spawn walk
                    // lists as well was then skipped when the copy crossed it
                    // (copyFired). Kept only for a run without the World (it
                    // asks the run, not World::disabled: see world::current).
                    if (!world::current()) obj->activatedByPlayer(player);

                    if (obj->m_isTouchTriggered) {
                        // Bot::get()->trajectory().scheduleAction(
                        //     [player, pl, obj]() {
                        phys::triggerObject(obj, pl, player);
                        // }, 1);
                        if (world::Run* run = world::currentRun()) world::touchedTrigger(*run, player, obj);
                    }
                }
                break;
            default:
                break;
        }
    }
}

void triggerObject(EffectGameObject* object, GJBaseGameLayer* pl,
                   PlayerObject* player) {
    auto bot = Bot::get();
    switch (object->m_objectID) {
        case 200:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 0.7;
            break;
        case 201:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 0.9;
            break;
        case 202:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.1;
            break;
        case 203:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.3;
            break;
        case 1334:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.6;
            break;
        case 2066: {
            if (object->m_followCPP) {
                break;
            }

            bool isP2 =
                bot->trajectory().unsafeInner()->m_fakePlayer2 == player;
            if (!object->m_targetPlayer2 && !isP2) {
                player->m_gravityMod = object->m_gravityValue;
            }
            if (object->m_targetPlayer2 && isP2) {
                player->m_gravityMod = object->m_gravityValue;
            }
            break;
        }
        case 2900: {
            RotateGameplayGameObject* rotateObj =
                (RotateGameplayGameObject*)object;

            if (rotateObj->m_changeChannel) {
                pl->m_gameState.m_currentChannel = rotateObj->m_targetChannelID;
                pl->m_gameState
                    .m_spawnChannelRelated1[rotateObj->m_targetChannelID] =
                    rotateObj->m_groundDirection == 2 ||
                    rotateObj->m_groundDirection == 3;
                // A run with the World walks the spawn lists from its own
                // channel, which a touched trigger switches as well.
                if (world::WorldState* ws = world::current()) {
                    world::switchChannel(*ws, rotateObj->m_targetChannelID,
                                         rotateObj->m_groundDirection == 2 || rotateObj->m_groundDirection == 3);
                }
            }

            if (rotateObj->m_channelOnly) break;

            auto rotate = [rotateObj](PlayerObject* p) {
                p->rotateGameplay(
                    rotateObj->m_moveDirection, rotateObj->m_groundDirection,
                    rotateObj->m_editVelocity, rotateObj->m_velocityModX,
                    rotateObj->m_velocityModY, rotateObj->m_overrideVelocity,
                    rotateObj->m_dontSlide);
            };

            rotate(player);

            if (pl->m_gameState.m_isDualMode &&
                bot->trajectory().isFakePlayer(player)) {
                PlayerObject* other = bot->trajectory().getOtherPlayer(player);
                if (other && other != player &&
                    !bot->trajectory().playerHasActivated(other, object) &&
                    !bot->trajectory().realPlayerHasActivated(other, object)) {
                    rotate(other);
                    phys::activateForTrajectory(object, other);
                }
            }

            break;
        }
        case 3022: {
            phys::teleportPlayer(pl, (TeleportPortalObject*)object, player);
            break;
        }
        // case 901: {
        //     // geode::log::info("Triggering move command for object ID 901");
        //     pl->triggerMoveCommand(object);
        //     break;
        // }
        default: {
            break;
        }
    }

    phys::activateForTrajectory(object, player);
}

void checkSpawnObjects(GJBaseGameLayer* pl, PlayerObject* player) {
    CCPoint position = player->getPosition();

    cocos2d::CCArray* objects =
        (cocos2d::CCArray*)pl->m_spawnObjects->objectForKey(
            pl->m_gameState.m_currentChannel);
    if (!objects) {
        return;
    }

    const int channel = pl->m_gameState.m_currentChannel;
    const auto& related0 = pl->m_gameState.m_spawnChannelRelated0;
    const auto& related1 = pl->m_gameState.m_spawnChannelRelated1;

    auto startIt = related0.find(channel);
    auto backIt = related1.find(channel);

    int startingIndex = startIt == related0.end() ? 0 : startIt->second;
    bool goingBack = backIt == related1.end() ? false : backIt->second;

    for (int i = startingIndex; static_cast<unsigned int>(i) < objects->count();
         i++) {
        SpawnTriggerGameObject* object =
            (SpawnTriggerGameObject*)objects->objectAtIndex(i);
        // The list is in the order the break below relies on, so past the
        // player is past it for every kind, not only for the four simulated
        // here. With the kind filtered first, every colour, alpha, pulse or
        // spawn trigger up to the next of those four beyond the player - to the
        // end of the list when there is none - was walked on every simulated
        // tick of every copy.
        if (!player->m_isSideways && !goingBack &&
            object->m_speedStart.x > position.x)
            break;
        if (object->m_objectID != 2066 && object->m_objectID != 2900 &&
            object->m_objectID != 3022 && object->m_objectID != 901) {
            continue;
        }

        CCPoint objectPos = object->m_speedStart;

        if (player->m_isSideways) {
            if (goingBack) {
                if (objectPos.y < position.y) break;
            } else {
                if (objectPos.y > position.y) break;
            }
        } else {
            if (goingBack) {
                if (objectPos.x < position.x) break;
            } else {
                if (objectPos.x > position.x) break;
            }
        }

        if (object->m_isGroupDisabled) continue;
        auto bot = Bot::get();
        // The game walks this list once (its index moves past what it fired),
        // so a trigger the copy fired stays fired for the run whatever its
        // multi-activate or platformer rule: in a platformer level
        // playerHasActivated lets everything fire again, and a gravity,
        // rotation or teleport trigger behind the copy fired on every tick.
        if (bot->trajectory().copyFired(player, object) ||
            bot->trajectory().realPlayerHasActivated(player, object))
            continue;

        if (!object->m_isTouchTriggered) {
            phys::triggerObject(object, pl, player);
        }
    }
}
}  // namespace phys
