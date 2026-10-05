// Absense's game hooks for its trajectory COPIES -- the copies of the player
// its look-ahead runs through the real level. Ported from Absense's
// hooks/GJBaseGameLayer.cpp, PlayerObject.cpp and EnhancedGameObject.cpp.
//
// Every hook here acts only on Absense's copies (Bot::trajectory().
// isFakePlayer) or while a copy is being run, and hands everything else to
// the game untouched, so the real player and GucciBot's own trajectory
// preview (whose players are separate objects) never reach this code. That
// is what lets these live in their own $modify classes next to GucciBot's.
// The hooks GucciBot already had for these same functions carry the
// equivalent checks inline (hooks/hook_*.cpp), since there the order
// matters.
//
// The ledger hooks (moveObjects ... toggleGroup) are the World's record of
// where the real game put its objects each tick, so a look-ahead that starts
// from a kept tick can put them back. world::ledgerRecording() is off unless
// the World is recording, which is only while Absense's pathfinder runs.

#include "absense/compat/bot.hpp"
#include "absense/physics/collisions.hpp"
#include "absense/physics/gjbasegamelayer.hpp"
#include "absense/physics/object.hpp"
#include "absense/physics/player.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/ledger.hpp"
#include "absense/world/world.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

class $modify(AbsGJBaseGameLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int length,
                               float p3) {
        if (!Bot::get()->trajectory().isFakePlayer(player))
            return GJBaseGameLayer::collisionCheckObjects(player, objects, length, p3);
        phys::collisionCheckObjects(this, player, objects, length, p3);
    }

    void teleportPlayer(TeleportPortalObject* obj, PlayerObject* player) {
        if (Bot::get()->trajectory().isFakePlayer(player))
            phys::teleportPlayer(this, obj, player);
        else
            GJBaseGameLayer::teleportPlayer(obj, player);
    }

    void flipGravity(PlayerObject* player, bool gravity, bool unk) {
        if (Bot::get()->trajectory().isFakePlayer(player))
            phys::flipGravity(player, gravity);
        else
            GJBaseGameLayer::flipGravity(player, gravity, unk);
    }

    void moveObjects(cocos2d::CCArray* objects, double dx, double dy, bool lockPlayerY) {
        if (world::ledgerRecording()) world::ledgerNoteArray(objects);
        GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
    }

    void rotateObjects(cocos2d::CCArray* objects, float rotation, cocos2d::CCPoint position,
                       cocos2d::CCPoint offset, bool finished, bool unused) {
        if (world::ledgerRecording()) world::ledgerNoteArray(objects);
        GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
    }

    void rotateObject(GameObject* object, float rotation) {
        if (world::ledgerRecording()) world::ledgerNoteObject(object);
        GJBaseGameLayer::rotateObject(object, rotation);
    }

#ifdef GEODE_IS_WINDOWS
    // Inlined into its callers on the other platforms: nothing to hook.
    void moveAreaObject(GameObject* object, float dx, float dy) {
        if (world::ledgerRecording()) world::ledgerNoteObject(object);
        GJBaseGameLayer::moveAreaObject(object, dx, dy);
    }
#endif

    void transformAreaObjects(GameObject* object, cocos2d::CCArray* objects, float scaleX, float scaleY,
                              bool reset) {
        if (world::ledgerRecording()) {
            world::ledgerNoteObject(object);
            world::ledgerNoteArray(objects);
        }
        GJBaseGameLayer::transformAreaObjects(object, objects, scaleX, scaleY, reset);
    }

    void toggleGroup(int id, bool activate) {
        if (world::ledgerRecording()) world::ledgerNoteGroup(id);
        GJBaseGameLayer::toggleGroup(id, activate);
    }
};

class $modify(AbsPlayerObject, PlayerObject) {
    void playSpiderDashEffect(cocos2d::CCPoint p0, cocos2d::CCPoint p1) {
        auto& t = Bot::get()->trajectory();
        if (!t.drawing() && !t.simulating())
            PlayerObject::playSpiderDashEffect(p0, p1);
    }

    void ringJump(RingObject* ring, bool unk) {
        if (Bot::get()->trajectory().isFakePlayer(this))
            phys::ringJump(this, ring);
        else
            PlayerObject::ringJump(ring, unk);
    }

    void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
        if (Bot::get()->trajectory().isFakePlayer(this))
            phys::bumpPlayer(this, force, objectType, playEffect, object);
        else
            PlayerObject::bumpPlayer(force, objectType, playEffect, object);
    }

    void propellPlayer(float force, bool dontPlayEffect, int objectType) {
        if (Bot::get()->trajectory().isFakePlayer(this))
            phys::propellPlayer(this, force, dontPlayEffect, objectType);
        else
            PlayerObject::propellPlayer(force, dontPlayEffect, objectType);
    }

    void startDashing(DashRingObject* obj) {
        if (Bot::get()->trajectory().isFakePlayer(this))
            phys::startDashing(this, obj);
        else
            PlayerObject::startDashing(obj);
    }

    void stopDashing() {
        if (Bot::get()->trajectory().isFakePlayer(this))
            phys::stopDashing(this);
        else
            PlayerObject::stopDashing();
    }
};

class $modify(AbsEnhancedGameObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        if (Bot::get()->trajectory().isFakePlayer(player))
            phys::activateForTrajectory((EffectGameObject*)this, player);
        else
            EnhancedGameObject::activatedByPlayer(player);
    }
};
