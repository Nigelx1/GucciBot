
#include "gjbasegamelayer.hpp"

#include <Geode/Geode.hpp>

#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/world.hpp"

#include <algorithm>

namespace phys {
cocos2d::CCArray* getGroup(GJBaseGameLayer* pl, int groupID) {
    // i'm too lazy to wait for geode to merge a bindings commit so yea

    if (groupID < 0) groupID = 0;
    if (groupID > 9999) groupID = 9999;

    cocos2d::CCArray* group = pl->m_groups.at(groupID);
    if (!group) {
        group = cocos2d::CCArray::create();
        pl->m_groupDict->setObject(group, groupID);
        pl->m_groups.at(groupID) = group;
    }

    return group;
}

float redirectPlayerForce(PlayerObject* player, float force,
                          float /* forceMod */, float /* forceMin */,
                          float /* forceMax */) {
    // float forceSmaller = force * 0.01745329;
    cocos2d::CCPoint velocityCoords = cocos2d::CCPoint{
        (float)player->m_platformerXVelocity, (float)player->m_yVelocity};

    cocos2d::CCPoint p;

    float idk =
        force * 0.017453292 - atan2f(velocityCoords.y, velocityCoords.x);
    if (idk != 0.0) {
        float v11 = sin(idk);
        float v12 = cos(idk);

        p = cocos2d::CCPoint{
            (float)(velocityCoords.x * v11) - (float)(velocityCoords.y * v12),
            (float)(velocityCoords.x * v12) + (float)(velocityCoords.y * v11)};
    }

    velocityCoords = p;

    // float length = velocityCoords.getLength();
    // if (forceMax > 0.0 && length > forceMax) {

    // }

    if (player->m_isSideways) {
        float x = velocityCoords.x;
        velocityCoords.x = velocityCoords.y;
        velocityCoords.y = x;
    }

    player->m_yVelocity = velocityCoords.y;
    player->m_isAccelerating = true;
    if (player->m_isPlatformer) {
        player->m_platformerXVelocity = velocityCoords.x;
        player->m_affectedByForces = true;
    }

    return player->m_yVelocity;
}

// this function is a nightmare
void teleportPlayer(GJBaseGameLayer* pl, TeleportPortalObject* object,
                    PlayerObject* player) {
    if (!player) {
        player = reinterpret_cast<PlayerObject*>(
            reinterpret_cast<uintptr_t>(pl->m_varianceValues.data()) + 0xe5);
    }
    player->m_wasTeleported = true;
    // A run's movers far from the copy were left behind: the destination and
    // whatever stands there are read at their places for this tick.
    moversCatchUp();

    if (object->m_orangePortal) {
        CCPoint bluePortalStartPos = object->getStartPos();
        CCPoint orangePortalStartPos = object->m_orangePortal->getStartPos();
        object->m_teleportYOffset =
            orangePortalStartPos.y - bluePortalStartPos.y;
    }

    cocos2d::CCPoint playerPos = player->getPosition();
    TeleportPortalObject* destination = object->m_orangePortal;
    if (!destination) {
        int targetGroupID = object->m_targetGroupID;
        if (targetGroupID > 0) {
            // The group's slot, read as it is. getGroup (the game's 0x224280,
            // and the port above) makes and registers an empty array for a
            // group nobody is in, which a run must never do to the real level;
            // a null slot is an empty group, so the pick comes out the same.
            const size_t slot = (size_t)std::min(targetGroupID, 9999);
            cocos2d::CCArray* group = slot < pl->m_groups.size() ? pl->m_groups[slot] : nullptr;
            int groupLength = group ? (int)group->count() : 0;

            float someFloat;

            if (groupLength > 0) {
                if (groupLength == 1) {
                    someFloat = 0.0;
                } else {
                    // The game's pick: the mod's teleportRandomOverride (midhook
                    // at 0x20FEDC) hands teleportPlayer the next value of the
                    // replay system's teleport state, divided by 32767.0
                    // (0x623644). A copy draws from the run's own copy of that
                    // state (the WorldState's seed, or the trajectory's own
                    // while the World is off), which snapshots and kept starts
                    // keep, so the same script always lands at the same target
                    // and the replay state is only ever read.
                    someFloat = world::nextRandom(world::teleportSeed());
                    if (someFloat == 1.0) someFloat = 0.0;
                }

                destination = (TeleportPortalObject*)group->objectAtIndex(
                    (int)(someFloat * groupLength));
            }
        }
    }

    if (destination) {
        CCPoint destinationPos;
        CCPoint portalPos = object->getPosition();
        player->m_lastPortalPos = object->getPosition();
        player->m_lastActivatedPortal = object;
        player->m_fallStartY = 0.0;
        if (object->m_objectID == 0x2eb) {
            float dy = object->m_teleportYOffset;
            portalPos = object->getRealPosition();

            destinationPos =
                cocos2d::CCPoint{player->getPosition().x, portalPos.y + dy};
        } else {
            destinationPos = destination->getPosition();
        }
        if (object->m_saveOffset) {
            portalPos = object->getRealPosition();

            portalPos -= playerPos;
            destinationPos -= portalPos;
        }
        if (object->m_ignoreX) {
            destinationPos.x = playerPos.x;
        }
        if (object->m_ignoreY) {
            destinationPos.y = playerPos.y;
        }
        player->setPosition(destinationPos);
    }

    if (object->m_gravityMode > 0) {
        bool gravity = false;

        if (object->m_gravityMode == 2) {
            gravity = true;
        } else if (object->m_gravityMode == 3) {
            gravity = !player->m_isUpsideDown;
        }

        phys::flipGravity(player, gravity);
    }

    float forceRelated;
    if (destination) {
        float gravityForceMod = 90.0;
        int objectID = object->m_objectID;
        if (objectID == 0x26 || objectID == 0x2eb || objectID == 0x2ed ||
            objectID == 0x810 || objectID == 0xb56) {
            gravityForceMod = 180.0;
        }

        float flipMod = (float)(object->isFlipX() ? 0xb4 : 1);
        forceRelated =
            flipMod + (gravityForceMod - destination->getRotationX());

    } else {
        float flipMod = (float)(object->isFlipX() ? 0xb4 : 1);
        forceRelated = flipMod - object->getRotationX();
    }

    if (object->m_redirectForceEnabled) {
        phys::redirectPlayerForce(
            player, forceRelated, object->m_redirectForceMod,
            object->m_redirectForceMin, object->m_redirectForceMax);
    } else {
        if (object->m_staticForceEnabled) {
            float force = object->m_staticForce;
            if (force == 0.0 && !object->m_staticForceAdditive) {
                player->m_isAccelerating = false;
                player->m_yVelocity = 0.0;
                if (player->m_isPlatformer) {
                    player->m_platformerXVelocity = 0.0;
                    player->m_affectedByForces = false;
                }
            } else {
                float forceMod = forceRelated * 0.01745329;
#ifdef GEODE_IS_WINDOWS
                cocos2d::CCPoint p = cocos2d::ccpForAngle(forceMod);
#else
                cocos2d::CCPoint p = CCPoint{
                    (float)std::cos(((float)forceMod)),
                    (float)std::sin(((float)forceMod)),
                };  // ccpForAngle isn't defined on macOS fsr
#endif

                float length = p.getLength();
                if (length > 0.0) {
                    cocos2d::CCPoint forceVector = p * (force / length);
                    if (player->m_isSideways) {
                        float x = forceVector.x;
                        forceVector.x = forceVector.y;
                        forceVector.y = x;
                    }

                    player->m_isAccelerating = true;
                    double newYVelocity = forceVector.y;
                    if (object->m_staticForceAdditive) {
                        newYVelocity += player->m_yVelocity;
                    }
                    player->m_yVelocity = newYVelocity;
                    if (player->m_isPlatformer) {
                        double newXVelocity = forceVector.x;
                        if (object->m_staticForceAdditive) {
                            newXVelocity += player->m_platformerXVelocity;
                        }
                        player->m_platformerXVelocity = newXVelocity;
                        player->m_affectedByForces = true;
                    }
                }
            }
        }
    }

    if (object->m_snapGround) {
        player->m_lastGroundedPos = player->getPosition();
    }
}
}  // namespace phys