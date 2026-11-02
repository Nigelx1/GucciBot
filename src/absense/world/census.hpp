#pragma once

// GucciBot (2026-11-01, H0 of the heavy three): what a level asks of the
// World, counted once before anything is recorded (gucci_level_census, the
// "[AbsPF] census" line). Which trigger kinds the World does not run and what
// the groups they name hold (hazards, solids, and the portals, pads and orbs
// that change where a copy goes), where in the level that is, how much of it
// depends on the route the player takes, what is running in the live game
// right now, and the section factors that decide how a run writes back the
// objects the real game moved (Materializer::startPending).
//
// Only reads: the level's WorldDef, the object writer's view of it, the
// layer's collision buckets and a capture of the live state, the same reads
// every run makes. Nothing in the game is written.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absense/world/def.hpp"

class GJBaseGameLayer;

namespace world {

// Collidable objects by what they do to a copy, as the collision pass tells
// them apart (physics/collisions.cpp): a hazard kills, a solid (a block, a
// slope, a breakable) is stood on or crashed into, a special (portals, pads,
// orbs, teleports) changes where the copy goes or what it is. A modifier is a
// trigger or a speed portal: the WorldDef files every one of them as
// collidable, and a group full of triggers to spawn would otherwise swamp the
// specials.
struct CensusClasses {
    uint32_t collidable = 0;
    uint32_t hazards = 0;
    uint32_t solids = 0;
    uint32_t specials = 0;
    uint32_t modifiers = 0;
    uint32_t extended = 0;  // of the collidable, with extended collision
};

// One trigger kind present in the level.
struct CensusKind {
    Kind kind = Kind::None;
    std::string ids;  // the object ids of it the level holds, "200,201,1334"
    bool shipped = false;
    Tier tier = Tier::C;
    uint32_t count = 0;
    // How its triggers fire: the spawn walk crossing them (neither flag),
    // spawned (m_isSpawnTriggered), touched (m_isTouchTriggered).
    uint32_t walk = 0;
    uint32_t spawn = 0;
    uint32_t touch = 0;
    float xLo = 0.0f, xHi = 0.0f;  // where they stand (m_speedStart.x)
    uint32_t groups = 0;           // distinct groups they name
    // The distinct members of those groups (every array of each) that are
    // collidable, and where they stand in x.
    CensusClasses reach;
    float reachLo = 0.0f, reachHi = 0.0f;
};

// One bin of x: the collidable objects in groups the World does not model.
struct CensusBin {
    float x = 0.0f;  // the bin's left edge
    uint32_t hazards = 0;
    uint32_t solids = 0;
    uint32_t specials = 0;
};

struct Census {
    // ---- the level
    int objects = 0;
    bool haveAll = false, haveCollidable = false;
    float yAll[2] = {0.0f, 0.0f};         // the lowest and highest object (m_positionY)
    float yCollidable[2] = {0.0f, 0.0f};  // ... of the collidable ones

    // ---- the World's read of it
    int triggers = 0;
    int spawnChannels = 0;
    int strayMembers = 0;
    float factorX = 0.0f;  // the layer's section factors (+0x36a0, +0x36a4)
    float factorY = 0.0f;
    int columns = 0;       // the collision buckets' columns, and the most rows one has
    int rowsMax = 0;
    int writable = 0;      // MaterialDef::slots: what a run may write
    int writableExtended = 0;
    // The way a run from here writes back what the real game moved away from
    // its table: "none (no table)", "columns", "full pass" or "cells".
    const char* pendingPath = "";
    bool tierDEChecked = false;
    bool targetsObjectsByKey = false;

    // ---- per kind: every kind present, the ones the World does not run
    // first, each part ordered by the hazards and solids its groups hold.
    std::vector<CensusKind> kinds;

    // ---- the unmodelled closure: the groups some trigger the World does not
    // run names (WorldDef::groupModelled false) that hold anything
    uint32_t unmodelledGroups = 0;
    CensusClasses unmodelled;
    uint32_t reachesAnyObject = 0;  // triggers whose fire makes a whole run uncertain
    std::vector<CensusBin> unmodelledByX;

    // ---- what depends on the route the player takes
    uint32_t moveLockPlayer = 0;   // move triggers locked to the player (x or y)
    uint32_t moveLockCamera = 0;   // ... to the camera, which a run never moves
    uint32_t dynamicMoves = 0;     // a DynamicObjectAction (world/fire.cpp)
    uint32_t dynamicRotates = 0;   // rotate triggers in dynamic mode
    uint32_t followPlayerY = 0;
    uint32_t touch = 0;
    uint32_t toggleOrbs = 0;
    uint32_t toggleBlocks = 0;
    uint32_t collision = 0;        // collision and instant collision triggers
    uint32_t collisionBlocks = 0;
    uint32_t items = 0;            // count, pickup and item triggers
    uint32_t reset = 0;            // 3618, not shipped
    uint32_t random = 0;           // random and advanced random
    std::vector<float> randomX;    // the first 32 of them, by x

    // ---- objects that change the player or the camera
    uint32_t dual = 0, solo = 0, teleports = 0, mirrors = 0, gravityPortals = 0;
    uint32_t rotateGameplay = 0, reverse = 0, gravityTriggers = 0, speedPortals = 0;
    uint32_t cameraZoom = 0, cameraStatic = 0, cameraOffset = 0, cameraEdge = 0, cameraMode = 0, gameplayOffset = 0;

    // ---- running in the live game now (World::captureLive)
    uint32_t commands[6] = {};  // by command type: move, rotate, follow, follow player Y, transform, keyframe
    uint32_t spawns = 0;
    uint32_t timers = 0;
    uint32_t areaMove = 0, areaRotate = 0, areaScale = 0;
    uint32_t dynamicMoveNow = 0, dynamicRotateNow = 0;
    uint32_t advancedFollows = 0;
    uint32_t areaObjectsToReset = 0;
    uint32_t uncertainGroups = 0;
    bool uncertainAny = false;
};

// The census of the level `def` was read from (the layer's own, which must be
// the one set up), with the unmodelled objects binned `binWidth` units of x
// wide. Only with the World on: everything here reads the game through the
// offsets turning it off says do not hold.
Census takeCensus(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& def, float binWidth);

}  // namespace world
