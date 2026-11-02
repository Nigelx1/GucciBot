// The level census (world/census.hpp). GucciBot (2026-11-01, H0 of the heavy
// three).

#include "absense/world/census.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "absense/world/materialize.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

namespace world {

namespace {

enum class Class : uint8_t { Hazard, Solid, Special, Modifier };

// What the collision pass does with a collidable object (physics/collisions.cpp,
// collisionCheckObjects): a hazard kills, a solid, slope or breakable is stood
// on or crashed into, a modifier is a trigger or a speed portal, and every
// other kind (portals, pads, orbs, teleports) acts on the copy.
Class classOf(const GameObject* o) {
    switch (o->m_objectType) {
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            return Class::Hazard;
        case GameObjectType::Solid:
        case GameObjectType::Slope:
        case GameObjectType::Breakable:
            return Class::Solid;
        case GameObjectType::Modifier:
            return Class::Modifier;
        default:
            return Class::Special;
    }
}

void tally(CensusClasses& c, Class k, bool extended) {
    c.collidable++;
    switch (k) {
        case Class::Hazard: c.hazards++; break;
        case Class::Solid: c.solids++; break;
        case Class::Special: c.specials++; break;
        case Class::Modifier: c.modifiers++; break;
    }
    if (extended) c.extended++;
}

void widen(bool& have, float (&range)[2], float v) {
    if (!have) {
        range[0] = range[1] = v;
        have = true;
        return;
    }
    range[0] = std::min(range[0], v);
    range[1] = std::max(range[1], v);
}

// An MSVC vector's element count, laid out as the collision buckets are (RawVec).
std::size_t entries(const RawVec& v, std::size_t elem) {
    return v.first && v.last >= v.first ? (std::size_t)(v.last - v.first) / elem : 0;
}

constexpr int kKinds = (int)Kind::CameraMode + 1;

}  // namespace

Census takeCensus(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& defHolder, float binWidth) {
    Census c;
    if (!pl || !defHolder) return c;
    const WorldDef& def = *defHolder;
    const std::size_t n = def.slots.size();
    if (!(binWidth >= 1.0f)) binWidth = 1000.0f;  // (a NaN too)
    c.objects = def.objectCount;
    c.triggers = (int)def.triggers.size();
    c.spawnChannels = (int)def.spawnLists.size();
    c.strayMembers = def.strayMembers;

    // ---- the level's extents, and the objects that change the player
    for (std::size_t s = 0; s < n; s++) {
        const GameObject* o = def.slots[s];
        if (!o) continue;
        const float y = (float)o->m_positionY;
        if (std::isfinite(y)) {
            widen(c.haveAll, c.yAll, y);
            if (def.flags[s] & kCollidable) widen(c.haveCollidable, c.yCollidable, y);
        }
        switch (o->m_objectType) {
            case GameObjectType::DualPortal: c.dual++; break;
            case GameObjectType::SoloPortal: c.solo++; break;
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal: c.mirrors++; break;
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::GravityTogglePortal: c.gravityPortals++; break;
            default: break;
        }
        const int di = def.defOfSlot[s];
        if (o->m_objectType == GameObjectType::TeleportPortal ||
            (di >= 0 && def.triggers[(std::size_t)di].kind == Kind::Teleport)) {
            c.teleports++;
        }
    }

    // ---- the sections: the factors as the layer has them (what
    // Materializer::begin reads), and the collision buckets' shape
    c.factorX = off::at<float>(pl, off::kSectionW);
    c.factorY = off::at<float>(pl, off::kSectionH);
    const SectionGrid grid = SectionGrid::of(pl);
    if (grid.buckets) {
        const std::size_t cols = entries(*grid.buckets, sizeof(void*));
        c.columns = (int)cols;
        for (std::size_t x = 0; x < cols; x++) {
            const RawVec* col = *reinterpret_cast<RawVec* const*>(grid.buckets->first + x * sizeof(void*));
            if (col) c.rowsMax = std::max(c.rowsMax, (int)entries(*col, sizeof(void*)));
        }
    }
    if (const std::shared_ptr<const MaterialDef> md = materialDef(defHolder)) {
        c.writable = (int)md->slots.size();
        for (int s : md->slots) {
            if (def.flags[(std::size_t)s] & kExtended) c.writableExtended++;
        }
    }
    // Materializer::startPending's choice, for a run from a table of another
    // tick (the first look of a run whose objects the real game has moved).
    if (c.writable == 0) {
        c.pendingPath = "none (no table)";
    } else if ((double)c.factorY > 0.0) {
        c.pendingPath = g_pendingCells ? "cells" : "full pass";
    } else {
        c.pendingPath = "columns";
    }
    c.tierDEChecked = checkTierDE();
    c.targetsObjectsByKey = world::targetsObjectsByKey(def);

    // ---- per kind, and what depends on the route
    struct Acc {
        uint32_t count = 0, walk = 0, spawn = 0, touch = 0;
        float xLo = 0.0f, xHi = 0.0f;
        std::vector<int> ids;
        std::vector<int> groups;
    };
    std::vector<Acc> acc((std::size_t)kKinds);
    std::vector<int> named;
    for (const TriggerDef& d : def.triggers) {
        const int k = (int)d.kind;
        if (k <= 0 || k >= kKinds) continue;
        Acc& a = acc[(std::size_t)k];
        if (a.count == 0) {
            a.xLo = a.xHi = d.x;
        } else {
            a.xLo = std::min(a.xLo, d.x);
            a.xHi = std::max(a.xHi, d.x);
        }
        a.count++;
        if (d.touch) a.touch++;
        if (d.spawn) a.spawn++;
        if (!d.touch && !d.spawn) a.walk++;
        a.ids.push_back((int)d.objectId);
        def.namedGroupsOf(d.slot, named);
        a.groups.insert(a.groups.end(), named.begin(), named.end());
        if (fireReachesAnyObject(def, d.kind)) c.reachesAnyObject++;

        switch (d.kind) {
            case Kind::Move:
                if (d.lockPlayerX || d.lockPlayerY) c.moveLockPlayer++;
                if (d.lockCameraX || d.lockCameraY) c.moveLockCamera++;
                // world/fire.cpp: the trigger makes a DynamicObjectAction.
                if (d.dynamic && (d.useTarget || d.directionMode)) c.dynamicMoves++;
                break;
            case Kind::Rotate:
                if (d.dynamic) c.dynamicRotates++;
                break;
            case Kind::FollowPlayerY: c.followPlayerY++; break;
            case Kind::Touch: c.touch++; break;
            case Kind::ToggleOrb: c.toggleOrbs++; break;
            case Kind::ToggleBlock: c.toggleBlocks++; break;
            case Kind::Collision:
            case Kind::InstantCollision: c.collision++; break;
            case Kind::CollisionBlock: c.collisionBlocks++; break;
            case Kind::Count:
            case Kind::InstantCount:
            case Kind::Pickup:
            case Kind::ItemEdit:
            case Kind::ItemCompare:
            case Kind::ItemPersistent: c.items++; break;
            case Kind::Reset: c.reset++; break;
            case Kind::Random:
            case Kind::AdvancedRandom:
                c.random++;
                c.randomX.push_back(d.x);
                break;
            case Kind::RotateGameplay: c.rotateGameplay++; break;
            case Kind::Reverse: c.reverse++; break;
            case Kind::Gravity: c.gravityTriggers++; break;
            case Kind::Speed: c.speedPortals++; break;
            case Kind::CameraZoom: c.cameraZoom++; break;
            case Kind::CameraStatic: c.cameraStatic++; break;
            case Kind::CameraOffset: c.cameraOffset++; break;
            case Kind::CameraEdge: c.cameraEdge++; break;
            case Kind::CameraMode: c.cameraMode++; break;
            case Kind::GameplayOffset: c.gameplayOffset++; break;
            default: break;
        }
    }
    std::sort(c.randomX.begin(), c.randomX.end());
    if (c.randomX.size() > 32) c.randomX.resize(32);

    // Each object of a group's three arrays once per walk: a stamp per slot,
    // a new mark per walk.
    std::vector<uint32_t> stamp(n, 0);
    uint32_t mark = 0;
    const auto walkGroup = [&](int g, auto&& fn) {
        for (auto list : {def.members(g), def.staticMembers(g), def.optimizedMembers(g)}) {
            for (int s : list) {
                if (s < 0 || (std::size_t)s >= n || stamp[(std::size_t)s] == mark) continue;
                stamp[(std::size_t)s] = mark;
                fn((std::size_t)s);
            }
        }
    };

    for (int k = 1; k < kKinds; k++) {
        Acc& a = acc[(std::size_t)k];
        if (a.count == 0) continue;
        std::sort(a.ids.begin(), a.ids.end());
        a.ids.erase(std::unique(a.ids.begin(), a.ids.end()), a.ids.end());
        std::sort(a.groups.begin(), a.groups.end());
        a.groups.erase(std::unique(a.groups.begin(), a.groups.end()), a.groups.end());
        CensusKind out;
        out.kind = (Kind)k;
        for (std::size_t i = 0; i < a.ids.size(); i++) out.ids += (i ? "," : "") + std::to_string(a.ids[i]);
        out.shipped = kindShipped(out.kind);
        out.tier = tierOf(out.kind);
        out.count = a.count;
        out.walk = a.walk;
        out.spawn = a.spawn;
        out.touch = a.touch;
        out.xLo = a.xLo;
        out.xHi = a.xHi;
        out.groups = (uint32_t)a.groups.size();
        mark++;
        bool haveX = false;
        float reachX[2] = {0.0f, 0.0f};
        for (int g : a.groups) {
            walkGroup(g, [&](std::size_t s) {
                const GameObject* o = def.slots[s];
                if (!o || !(def.flags[s] & kCollidable)) return;
                tally(out.reach, classOf(o), (def.flags[s] & kExtended) != 0);
                const float x = (float)o->m_positionX;
                if (std::isfinite(x)) widen(haveX, reachX, x);
            });
        }
        out.reachLo = reachX[0];
        out.reachHi = reachX[1];
        c.kinds.push_back(std::move(out));
    }
    std::sort(c.kinds.begin(), c.kinds.end(), [](const CensusKind& a, const CensusKind& b) {
        if (a.shipped != b.shipped) return !a.shipped;
        const uint32_t ka = a.reach.hazards + a.reach.solids, kb = b.reach.hazards + b.reach.solids;
        if (ka != kb) return ka > kb;
        if (a.reach.specials != b.reach.specials) return a.reach.specials > b.reach.specials;
        if (a.count != b.count) return a.count > b.count;
        return (int)a.kind < (int)b.kind;
    });

    // ---- the unmodelled closure: every group a kind the World does not run
    // names, and what it holds, each object once
    mark++;
    std::map<int64_t, CensusBin> bins;
    for (int g = 1; g < kGroupLimit; g++) {
        if (def.groupModelled(g)) continue;
        if (def.members(g).empty() && def.staticMembers(g).empty() && def.optimizedMembers(g).empty()) continue;
        c.unmodelledGroups++;
        walkGroup(g, [&](std::size_t s) {
            const GameObject* o = def.slots[s];
            if (!o || !(def.flags[s] & kCollidable)) return;
            const Class k = classOf(o);
            tally(c.unmodelled, k, (def.flags[s] & kExtended) != 0);
            const double x = o->m_positionX;
            if (k == Class::Modifier || !std::isfinite(x)) return;
            const int64_t b = (int64_t)std::floor(x / (double)binWidth);
            CensusBin& bin = bins[b];
            bin.x = (float)((double)b * (double)binWidth);
            if (k == Class::Hazard) bin.hazards++;
            else if (k == Class::Solid) bin.solids++;
            else bin.specials++;
        });
    }
    for (const auto& [b, bin] : bins) c.unmodelledByX.push_back(bin);

    // ---- running in the live game now: the state a run from here begins
    // from, with its running actions (its first read of the level runs the
    // level check, as any run's does)
    const WorldState ws = World::captureLive(pl, true);
    for (const WCmd& cmd : *ws.cmds) {
        if (cmd.commandType >= 0 && cmd.commandType < 6) c.commands[cmd.commandType]++;
    }
    c.spawns = (uint32_t)ws.spawns->size();
    c.timers = (uint32_t)ws.timers->size();
    c.uncertainGroups = (uint32_t)ws.uncertainGroups.size();
    c.uncertainAny = ws.uncertainAny;
    const GJGameState& gs = pl->m_gameState;
    c.areaMove = (uint32_t)gs.m_moveEffectInstances.size();
    c.areaRotate = (uint32_t)gs.m_rotateEffectInstances.size();
    c.areaScale = (uint32_t)gs.m_scaleEffectInstances.size();
    c.dynamicMoveNow = (uint32_t)gs.m_dynamicMoveActions.size();
    c.dynamicRotateNow = (uint32_t)gs.m_dynamicRotateActions.size();
    c.advancedFollows = (uint32_t)gs.m_advanceFollowInstances.size();
    c.areaObjectsToReset = (uint32_t)std::max(0, pl->m_areaObjectsCount + pl->m_processedAreaObjectsCount);
    return c;
}

}  // namespace world
