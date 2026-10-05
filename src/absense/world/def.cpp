#include "core/platform.hpp"
#include "absense/world/def.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "absense/compat/devlog.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/ledger.hpp"
#include "absense/world/materialize.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

namespace world {

// ------------------------------------------------------------ kinds

Kind kindOf(int id) {
    switch (id) {
        case 901: return Kind::Move;
        case 1346: return Kind::Rotate;
        case 1347: return Kind::Follow;
        case 1814: return Kind::FollowPlayerY;
        case 2067: return Kind::Scale;
        case 1049: return Kind::Toggle;
        case 1268: return Kind::Spawn;
        case 1616: return Kind::Stop;
        case 1595: return Kind::Touch;
        case 1594: return Kind::ToggleOrb;
        case 3643: return Kind::ToggleBlock;
        case 1611: return Kind::Count;
        case 1811: return Kind::InstantCount;
        case 1817: return Kind::Pickup;
        case 3619: return Kind::ItemEdit;
        case 3620: return Kind::ItemCompare;
        case 3641: return Kind::ItemPersistent;
        case 1815: return Kind::Collision;
        case 1816: return Kind::CollisionBlock;
        case 3609: return Kind::InstantCollision;
        case 3614: return Kind::TimeTrigger;
        case 3615: return Kind::TimeEvent;
        case 3617: return Kind::TimeControl;
        case 3618: return Kind::Reset;
        case 3604: return Kind::Event;
        case 3607: return Kind::Sequence;
        case 1912: return Kind::Random;
        case 2068: return Kind::AdvancedRandom;
        case 1932: return Kind::PlayerControl;
        case 1917: return Kind::Reverse;
        case 1935: return Kind::TimeWarp;
        case 2066: return Kind::Gravity;
        case 2900: return Kind::RotateGameplay;
        case 3022: return Kind::Teleport;
        case 200: case 201: case 202: case 203: case 1334: return Kind::Speed;
        case 2899: return Kind::Options;
        case 3600: return Kind::End;
        case 3016: return Kind::AdvancedFollow;
        case 3660: return Kind::EditAdvancedFollow;
        case 3661: return Kind::RetargetAdvancedFollow;
        case 3033: return Kind::KeyframeAnim;
        case 3032: return Kind::Keyframe;
        case 3006: return Kind::AreaMove;
        case 3007: return Kind::AreaRotate;
        case 3008: return Kind::AreaScale;
        case 3011: return Kind::EditAreaMove;
        case 3012: return Kind::EditAreaRotate;
        case 3013: return Kind::EditAreaScale;
        case 3017: return Kind::EnterMove;
        case 3018: return Kind::EnterRotate;
        case 3019: return Kind::EnterScale;
        case 3020: return Kind::EnterFade;
        case 3021: return Kind::EnterTint;
        case 3024: return Kind::AreaStop;
        case 1913: return Kind::CameraZoom;
        case 1914: return Kind::CameraStatic;
        case 1916: return Kind::CameraOffset;
        case 2062: return Kind::CameraEdge;
        case 2901: return Kind::GameplayOffset;
        case 2925: return Kind::CameraMode;
        default: return Kind::None;
    }
}

Tier tierOf(Kind k) {
    switch (k) {
        // (a): move, toggle, spawn, stop, the counts, speed portals the walk
        // crosses, and gravity / rotate gameplay / teleport through the World.
        case Kind::Move:
        case Kind::Toggle:
        case Kind::Spawn:
        case Kind::Stop:
        case Kind::Count:
        case Kind::InstantCount:
        case Kind::Pickup:
        case Kind::Speed:
        case Kind::Gravity:
        case Kind::RotateGameplay:
        case Kind::Teleport:
            return Tier::A;
        // (b). Rotate and scale work their positions out through live cocos
        // nodes rather than arithmetic (world/objects.cpp), follow and follow
        // player Y keep a node list of their own, and the collision triggers
        // need the player collision blocks, which the game keeps by moving
        // real objects between sections: none of the four is ported yet.
        case Kind::Rotate:
        case Kind::Follow:
        case Kind::FollowPlayerY:
        case Kind::Scale:
        case Kind::ToggleOrb:
        case Kind::ToggleBlock:
        case Kind::Touch:
        case Kind::Collision:
        case Kind::CollisionBlock:
        case Kind::InstantCollision:
            return Tier::B;
        default:
            return Tier::C;
    }
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::None: return "none";
        case Kind::Move: return "move";
        case Kind::Rotate: return "rotate";
        case Kind::Follow: return "follow";
        case Kind::FollowPlayerY: return "follow player Y";
        case Kind::Scale: return "scale";
        case Kind::Toggle: return "toggle";
        case Kind::Spawn: return "spawn";
        case Kind::Stop: return "stop";
        case Kind::Touch: return "touch";
        case Kind::ToggleOrb: return "toggle orb";
        case Kind::ToggleBlock: return "toggle block";
        case Kind::Count: return "count";
        case Kind::InstantCount: return "instant count";
        case Kind::Pickup: return "pickup";
        case Kind::ItemEdit: return "item edit";
        case Kind::ItemCompare: return "item compare";
        case Kind::ItemPersistent: return "persistent item";
        case Kind::Collision: return "collision";
        case Kind::CollisionBlock: return "collision block";
        case Kind::InstantCollision: return "instant collision";
        case Kind::TimeTrigger: return "time";
        case Kind::TimeEvent: return "time event";
        case Kind::TimeControl: return "time control";
        case Kind::Reset: return "reset";
        case Kind::Event: return "event";
        case Kind::Sequence: return "sequence";
        case Kind::Random: return "random";
        case Kind::AdvancedRandom: return "advanced random";
        case Kind::PlayerControl: return "player control";
        case Kind::Reverse: return "reverse";
        case Kind::TimeWarp: return "time warp";
        case Kind::Gravity: return "gravity";
        case Kind::RotateGameplay: return "rotate gameplay";
        case Kind::Teleport: return "teleport";
        case Kind::Speed: return "speed";
        case Kind::Options: return "options";
        case Kind::End: return "end";
        case Kind::AdvancedFollow: return "advanced follow";
        case Kind::EditAdvancedFollow: return "edit advanced follow";
        case Kind::RetargetAdvancedFollow: return "retarget advanced follow";
        case Kind::KeyframeAnim: return "keyframe animation";
        case Kind::Keyframe: return "keyframe";
        case Kind::AreaMove: return "area move";
        case Kind::AreaRotate: return "area rotate";
        case Kind::AreaScale: return "area scale";
        case Kind::EditAreaMove: return "edit area move";
        case Kind::EditAreaRotate: return "edit area rotate";
        case Kind::EditAreaScale: return "edit area scale";
        case Kind::EnterMove: return "enter move";
        case Kind::EnterRotate: return "enter rotate";
        case Kind::EnterScale: return "enter scale";
        case Kind::EnterFade: return "enter fade";
        case Kind::EnterTint: return "enter tint";
        case Kind::AreaStop: return "area stop";
        case Kind::CameraZoom: return "camera zoom";
        case Kind::CameraStatic: return "static camera";
        case Kind::CameraOffset: return "camera offset";
        case Kind::CameraEdge: return "camera edge";
        case Kind::GameplayOffset: return "gameplay offset";
        case Kind::CameraMode: return "camera mode";
    }
    return "?";
}

// Which kinds the World really runs. The tier is the rollout order (design
// step 12); this is what has landed, and a kind joins it only once its port is
// complete. Everything else marks the groups it names uncertain when it fires.
bool kindShipped(Kind k) {
    switch (k) {
        // (a): move, toggle, spawn, stop, the counts and pickups, the speed
        // portals the walk crosses, gravity, rotate gameplay and teleport.
        case Kind::Move:
        case Kind::Toggle:
        case Kind::Spawn:
        case Kind::Stop:
        case Kind::Count:
        case Kind::InstantCount:
        case Kind::Pickup:
        case Kind::Speed:
        case Kind::Gravity:
        case Kind::RotateGameplay:
        case Kind::Teleport:
        // (b): the touch trigger and the two custom rings. A custom ring does
        // what it does from the player's ring path (world::ringActivated), a
        // touch trigger from the run's own button changes (world::onButton).
        case Kind::Touch:
        case Kind::ToggleOrb:
        case Kind::ToggleBlock:
        // (c): the randoms, the sequence, the event trigger, the timers, the
        // item triggers, the player control trigger, reverse, the time warp,
        // the options and the end trigger (world/fire.cpp (aa)-(aj)). The
        // randoms fire from a generator the real game does not share with the
        // run, so what they pick is marked uncertain even though the kind
        // runs; 3618 (reset) is not here - see the note in world::fire.
        case Kind::Random:
        case Kind::AdvancedRandom:
        case Kind::Sequence:
        case Kind::Event:
        case Kind::TimeTrigger:
        case Kind::TimeEvent:
        case Kind::TimeControl:
        case Kind::ItemEdit:
        case Kind::ItemCompare:
        case Kind::ItemPersistent:
        case Kind::PlayerControl:
        case Kind::Reverse:
        case Kind::TimeWarp:
        case Kind::Options:
        case Kind::End:
            return true;
        default:
            return false;
    }
}

// ------------------------------------------------------------ lookups

int WorldDef::slotOfUid(int uid) const {
    if (!m_uidDense.empty()) {
        const int64_t i = (int64_t)uid - m_uidBase;
        if (i < 0 || i >= (int64_t)m_uidDense.size()) return -1;
        return m_uidDense[(std::size_t)i];
    }
    auto it = m_uidSparse.find(uid);
    return it == m_uidSparse.end() ? -1 : it->second;
}

int WorldDef::slotOf(const GameObject* o) const {
    if (!o) return -1;
    const int s = slotOfUid(o->m_uniqueID);
    return (s >= 0 && (std::size_t)s < slots.size() && slots[(std::size_t)s] == o) ? s : -1;
}

int WorldDef::countIn(std::span<const int> list, int slot) {
    auto [a, b] = std::equal_range(list.begin(), list.end(), slot);
    return (int)(b - a);
}

int WorldDef::parentOf(int group) const {
    auto it = std::lower_bound(groupParents.begin(), groupParents.end(), group,
                               [](const std::pair<int, int>& e, int key) { return e.first < key; });
    return (it != groupParents.end() && it->first == group) ? it->second : -1;
}

std::span<const int> WorldDef::spawnList(int channel) const {
    auto it = std::lower_bound(spawnLists.begin(), spawnLists.end(), channel,
                               [](const std::pair<int, std::vector<int>>& e, int key) { return e.first < key; });
    if (it == spawnLists.end() || it->first != channel) return {};
    return it->second;
}

// ------------------------------------------------------------ building

namespace {

// Lists built from (key, value) pairs: grouped by key, each key's values in
// the order they were added.
template <class T>
Lists<T> toLists(std::vector<std::pair<uint32_t, T>>& pairs, std::size_t keys, bool sortValues) {
    Lists<T> out;
    out.offsets.assign(keys + 1, 0);
    for (auto& [k, v] : pairs) out.offsets[k + 1]++;
    for (std::size_t i = 0; i < keys; i++) out.offsets[i + 1] += out.offsets[i];
    out.values.resize(pairs.size());
    std::vector<uint32_t> at(out.offsets.begin(), out.offsets.end() - 1);
    for (auto& [k, v] : pairs) out.values[at[k]++] = v;
    if (sortValues) {
        for (std::size_t i = 0; i < keys; i++) {
            std::sort(out.values.begin() + out.offsets[i], out.values.begin() + out.offsets[i + 1]);
        }
    }
    return out;
}

// The kinds the collision pass drops before it reads anything else
// (physics/collisions.cpp, collisionCheckObjects).
bool collidable(const GameObject* o) {
    switch (o->m_objectType) {
        case GameObjectType::Decoration:
        case GameObjectType::CollisionObject:
        case GameObjectType::SecretCoin:
        case GameObjectType::UserCoin:
        case GameObjectType::Collectible:
        case GameObjectType::EnterEffectObject:
            return false;
        default:
            // The dual and solo portals (286 and 287) were dropped here too
            // while no run simulated them; a run switches on them now, so a
            // moving one has to be put where its run has it.
            return true;
    }
}

bool validGroup(int g) { return g > 0 && g < kGroupLimit; }

void pushChances(WorldDef& w, TriggerDef& d, const gd::vector<ChanceObject>& list) {
    d.chanceBegin = (int)w.chances.size();
    for (const ChanceObject& c : list) w.chances.push_back({c.m_groupID, c.m_oldGroupID, c.m_chance});
    d.chanceCount = (int)w.chances.size() - d.chanceBegin;
}

void fillDef(WorldDef& w, TriggerDef& d, EffectGameObject* e) {
    using namespace geode::cast;
    d.x = e->m_speedStart.x;
    d.y = e->m_speedStart.y;
    d.channel = e->m_channelValue;
    d.touch = e->m_isTouchTriggered;
    d.spawn = e->m_isSpawnTriggered;
    d.multi = e->m_isMultiTriggered;
    d.target = e->m_targetGroupID;
    d.center = e->m_centerGroupID;
    d.control = e->m_controlID;

    // Read from every effect object: the move settings (triggerMoveCommand
    // 0x21ea40 reads them at the offsets world/offsets.hpp pins), the eased
    // kinds' duration and easing, the rotate and follow settings.
    d.moveX = e->m_moveOffset.x;
    d.moveY = e->m_moveOffset.y;
    d.duration = e->m_duration;
    d.easingType = (int)e->m_easingType;
    d.easingRate = e->m_easingRate;
    d.lockPlayerX = e->m_lockToPlayerX;
    d.lockPlayerY = e->m_lockToPlayerY;
    d.lockCameraX = e->m_lockToCameraX;
    d.lockCameraY = e->m_lockToCameraY;
    d.modX = e->m_moveModX;
    d.modY = e->m_moveModY;
    d.useTarget = e->m_useMoveTarget;
    d.targetMode = (int)e->m_moveTargetMode;
    d.targetP1 = e->m_targetPlayer1;
    d.targetP2 = e->m_targetPlayer2;
    d.directionMode = off::at<bool>(e, off::kEffDirectionMode);
    d.directionDistance = e->m_directionModeDistance;
    d.targetModCenter = e->m_targetModCenterID;
    d.dynamic = e->m_isDynamicMode;
    d.silent = e->m_isSilent;
    d.smallStep = e->m_smallStep;
    d.degrees = e->m_rotationDegrees;
    d.times360 = e->m_times360;
    d.lockRotation = e->m_lockObjectRotation;
    d.rotationTarget = e->m_rotationTargetID;
    d.rotationOffset = e->m_rotationOffset;
    d.dynamicEasing = e->m_dynamicModeEasing;
    d.followXMod = e->m_followXMod;
    d.followYMod = e->m_followYMod;
    d.followYSpeed = e->m_followYSpeed;
    d.followYDelay = e->m_followYDelay;
    d.followYOffset = e->m_followYOffset;
    d.followYMaxSpeed = e->m_followYMaxSpeed;
    d.activateGroup = e->m_activateGroup;
    d.spawnOrdered = e->m_spawnOrdered;
    d.itemId = e->m_itemID;
    d.itemId2 = e->m_itemID2;
    d.triggerOnExit = e->m_triggerOnExit;
    d.dynamicBlock = e->m_isDynamicBlock;
    d.touchHold = e->m_touchHoldMode;
    d.touchToggle = (int)e->m_touchToggleMode;
    d.touchPlayer = (int)e->m_touchPlayerMode;
    d.dualMode = e->m_isDualMode;
    d.timeWarp = e->m_timeWarpTimeMod;
    d.gravity = e->m_gravityValue;
    d.followCPP = e->m_followCPP;

    switch (d.kind) {
        case Kind::Scale:
            if (auto* t = typeinfo_cast<TransformTriggerGameObject*>(e)) {
                d.scaleX = t->m_objectScaleX;
                d.scaleY = t->m_objectScaleY;
                d.divideX = t->m_divideX;
                d.divideY = t->m_divideY;
                d.onlyMove = t->m_onlyMove;
                d.relativeRotation = t->m_relativeRotation;
                d.relativeScale = t->m_relativeScale;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Spawn:
            if (auto* s = typeinfo_cast<SpawnTriggerGameObject*>(e)) {
                d.spawnDelay = s->m_spawnDelay;
                d.spawnVariance = s->m_delayRange;
                d.resetRemap = s->m_resetRemap;
                d.remapKey = s->m_remapKey;
                pushChances(w, d, s->m_remapObjects);
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Count:
        case Kind::InstantCount:
        case Kind::Pickup:
            if (auto* c = typeinfo_cast<CountTriggerGameObject*>(e)) {
                d.count = c->m_pickupCount;
                d.mode = c->m_pickupTriggerMode;
                d.countMulti = c->m_multiActivate;
                d.countOverride = c->m_isOverride;
                d.countMultiplier = c->m_pickupTriggerMultiplier;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::ItemEdit:
        case Kind::ItemCompare:
        case Kind::ItemPersistent:
            if (auto* it = typeinfo_cast<ItemTriggerGameObject*>(e)) {
                d.item1Mode = it->m_item1Mode;
                d.item2Mode = it->m_item2Mode;
                d.targetItemMode = it->m_targetItemMode;
                d.mod1 = it->m_mod1;
                d.mod2 = it->m_mod2;
                d.resultType1 = it->m_resultType1;
                d.resultType2 = it->m_resultType2;
                d.resultType3 = it->m_resultType3;
                d.tolerance = it->m_tolerance;
                d.roundType1 = it->m_roundType1;
                d.roundType2 = it->m_roundType2;
                d.signType1 = it->m_signType1;
                d.signType2 = it->m_signType2;
                d.persistent = it->m_persistent;
                d.targetAll = it->m_targetAll;
                d.resetItem = it->m_reset;
                d.timerItem = it->m_timer;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::TimeTrigger:
        case Kind::TimeEvent:
        case Kind::TimeControl:
            if (auto* t = typeinfo_cast<TimerTriggerGameObject*>(e)) {
                d.startTime = t->m_startTime;
                d.targetTime = t->m_targetTime;
                d.stopTimeEnabled = t->m_stopTimeEnabled;
                d.dontOverride = t->m_dontOverride;
                d.ignoreTimeWarp = t->m_ignoreTimeWarp;
                d.timerMod = t->m_timeMod;
                d.startPaused = t->m_startPaused;
                d.timerMulti = t->m_multiActivate;
                d.controlType = t->m_controlType;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Event:
            if (auto* ev = typeinfo_cast<EventLinkTrigger*>(e)) {
                d.listBegin = (int)w.lists.size();
                for (int id : ev->m_eventIDs) w.lists.push_back(id);
                d.listCount = (int)w.lists.size() - d.listBegin;
                d.resetRemap = ev->m_resetRemap;
                d.extraId = ev->m_extraID;
                d.extraId2 = ev->m_extraID2;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Random:
        case Kind::AdvancedRandom:
        case Kind::Sequence:
            if (auto* c = typeinfo_cast<ChanceTriggerGameObject*>(e)) {
                pushChances(w, d, c->m_chanceObjects);
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::PlayerControl:
            if (auto* p = typeinfo_cast<PlayerControlGameObject*>(e)) {
                d.stopJump = p->m_stopJump;
                d.stopMove = p->m_stopMove;
                d.stopRotation = p->m_stopRotation;
                d.stopSlide = p->m_stopSlide;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::RotateGameplay:
            if (auto* r = typeinfo_cast<RotateGameplayGameObject*>(e)) {
                d.moveDirection = r->m_moveDirection;
                d.groundDirection = r->m_groundDirection;
                d.editVelocity = r->m_editVelocity;
                d.overrideVelocity = r->m_overrideVelocity;
                d.velocityModX = r->m_velocityModX;
                d.velocityModY = r->m_velocityModY;
                d.changeChannel = r->m_changeChannel;
                d.channelOnly = r->m_channelOnly;
                d.targetChannel = r->m_targetChannelID;
                d.instantOffset = r->m_instantOffset;
                d.dontSlide = r->m_dontSlide;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Teleport:
            if (auto* t = typeinfo_cast<TeleportPortalObject*>(e)) {
                d.teleportYOffset = t->m_teleportYOffset;
                d.teleportEase = t->m_teleportEase;
                d.staticForceEnabled = t->m_staticForceEnabled;
                d.staticForce = t->m_staticForce;
                d.redirectForceEnabled = t->m_redirectForceEnabled;
                d.redirectForceMod = t->m_redirectForceMod;
                d.redirectForceMin = t->m_redirectForceMin;
                d.redirectForceMax = t->m_redirectForceMax;
                d.saveOffset = t->m_saveOffset;
                d.ignoreX = t->m_ignoreX;
                d.ignoreY = t->m_ignoreY;
                d.gravityMode = t->m_gravityMode;
                d.staticForceAdditive = t->m_staticForceAdditive;
                d.instantCamera = t->m_instantCamera;
                d.snapGround = t->m_snapGround;
                d.redirectDash = t->m_redirectDash;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Stop:
            if (auto* t = typeinfo_cast<TriggerControlGameObject*>(e)) {
                // TriggerControlGameObject::triggerObject 0x4c3320.
                d.controlAction = static_cast<int>(t->m_customTriggerValue);
                d.useControlId = e->m_targetControlID;
            } else {
                d.paramsMissing = true;
            }
            break;
        case Kind::Speed:
            d.hasNoEffects = e->m_hasNoEffects;
            // EffectGameObject::triggerObject 0x4a6896-0x4a68ea: 200 0.7,
            // 202 1.1, 203 1.3, 1334 1.6, anything else 0.9, parked in
            // m_timeModRelated for the next tick.
            switch (d.objectId) {
                case 200: d.speed = 0.7f; break;
                case 202: d.speed = 1.1f; break;
                case 203: d.speed = 1.3f; break;
                case 1334: d.speed = 1.6f; break;
                default: d.speed = 0.9f; break;
            }
            break;
        default:
            break;
    }
}

// The groups an effect object names, clamped the way the game clamps them.
void namedGroups(const WorldDef& w, const TriggerDef* d, EffectGameObject* e, std::vector<int>& out) {
    out.clear();
    auto add = [&](int g) {
        if (validGroup(g)) out.push_back(g);
    };
    add(e->m_targetGroupID);
    add(e->m_centerGroupID);
    add(e->m_targetModCenterID);
    add(e->m_rotationTargetID);
    if (d) {
        for (int i = 0; i < d->chanceCount; i++) {
            const DefChance& c = w.chances[(std::size_t)(d->chanceBegin + i)];
            add(c.group);
            add(c.oldGroup);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

}  // namespace

void WorldDef::setGroups(std::vector<std::vector<int>> members, std::vector<std::vector<int>> staticMembers,
                         std::vector<std::vector<int>> optimizedMembers, int slotCount) {
    auto fill = [](Lists<int>& out, std::vector<std::vector<int>>& in) {
        std::vector<std::pair<uint32_t, int>> pairs;
        for (std::size_t g = 0; g < in.size() && g < (std::size_t)kGroupLimit; g++) {
            for (int s : in[g]) pairs.push_back({(uint32_t)g, s});
        }
        out = toLists(pairs, (std::size_t)kGroupLimit, true);
    };
    fill(m_members, members);
    fill(m_static, staticMembers);
    fill(m_optimized, optimizedMembers);
    finishGroups(slotCount);
}

void WorldDef::finishGroups(int slotCount) {
    // A slot's groups: every array it is in, each group once.
    std::vector<std::pair<uint32_t, uint16_t>> pairs;
    for (const Lists<int>* list : {&m_members, &m_static, &m_optimized}) {
        for (std::size_t g = 0; g < list->size(); g++) {
            for (int s : list->at(g)) {
                if (s < 0 || s >= slotCount) continue;
                pairs.push_back({(uint32_t)s, (uint16_t)g});
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    m_groupsOf = toLists(pairs, (std::size_t)slotCount, false);
}

void WorldDef::build(GJBaseGameLayer* layer) {
    pl = layer;
    const cocos2d::CCArray* objects = layer->m_objects;
    const int n = objects ? (int)objects->count() : 0;
    objectCount = n;
    slots.resize((std::size_t)n);
    flags.assign((std::size_t)n, 0);
    defOfSlot.assign((std::size_t)n, -1);
    int minUid = std::numeric_limits<int>::max();
    int maxUid = std::numeric_limits<int>::min();
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(objects->data->arr[i]);
        slots[(std::size_t)i] = o;
        if (!o) continue;
        minUid = std::min(minUid, o->m_uniqueID);
        maxUid = std::max(maxUid, o->m_uniqueID);
    }
    // The uids come from one counter, so they are usually dense: a table
    // then, a map when they are not.
    m_uidDense.clear();
    m_uidSparse.clear();
    if (n > 0 && minUid <= maxUid && (int64_t)maxUid - minUid < (int64_t)n * 8 + 4096) {
        m_uidBase = minUid;
        m_uidDense.assign((std::size_t)((int64_t)maxUid - minUid + 1), -1);
        for (int i = 0; i < n; i++) {
            if (GameObject* o = slots[(std::size_t)i]) m_uidDense[(std::size_t)(o->m_uniqueID - minUid)] = i;
        }
    } else {
        m_uidSparse.reserve((std::size_t)n);
        for (int i = 0; i < n; i++) {
            if (GameObject* o = slots[(std::size_t)i]) m_uidSparse[o->m_uniqueID] = i;
        }
    }

    // Kinds, flags and trigger settings.
    for (int i = 0; i < n; i++) {
        GameObject* o = slots[(std::size_t)i];
        if (!o) continue;
        uint8_t f = 0;
        if (collidable(o)) f |= kCollidable;
        if (o->m_hasExtendedCollision) f |= kExtended;
        if (o->m_canBeControlled) f |= kControllable;
        if (o->m_classType == GameObjectClassType::Effect) {
            f |= kEffect;
            if (o->isSpawnableTrigger()) f |= kSpawnable;
            const Kind k = kindOf(o->m_objectID);
            if (k != Kind::None) {
                auto* e = static_cast<EffectGameObject*>(o);
                TriggerDef d;
                d.kind = k;
                d.tier = tierOf(k);
                d.objectId = (uint16_t)o->m_objectID;
                d.slot = i;
                d.uid = o->m_uniqueID;
                fillDef(*this, d, e);
                defOfSlot[(std::size_t)i] = (int)triggers.size();
                triggerOf[o] = (int)triggers.size();
                if (k == Kind::Sequence) sequenceDefs.push_back((int)triggers.size());
                triggers.push_back(d);
                f |= kTrigger;
            }
        }
        flags[(std::size_t)i] = f;
    }

    // The group arrays, straight from their slots.
    strayMembers = 0;
    std::vector<std::pair<uint32_t, int>> spawnPairs;
    auto readArrays = [&](const gd::vector<cocos2d::CCArray*>& arrays, Lists<int>& out, bool spawnOrder) {
        std::vector<std::pair<uint32_t, int>> pairs;
        const std::size_t count = std::min<std::size_t>(arrays.size(), (std::size_t)kGroupLimit);
        for (std::size_t g = 0; g < count; g++) {
            const cocos2d::CCArray* arr = arrays[g];
            if (!arr || !arr->data) continue;
            const unsigned num = arr->data->num;
            for (unsigned j = 0; j < num; j++) {
                auto* o = static_cast<GameObject*>(arr->data->arr[j]);
                const int s = slotOf(o);
                if (s < 0) {
                    strayMembers++;
                    continue;
                }
                pairs.push_back({(uint32_t)g, s});
                if (spawnOrder) {
                    // spawnObjectsInOrder 0x21ae8c-0x21aeb8.
                    const bool spawnable = o->m_classType == GameObjectClassType::Effect
                                               ? o->isSpawnableTrigger()
                                               : (o->m_objectID == 2065 && off::at<bool>(o, off::kAnimateOnTrigger));
                    if (spawnable) spawnPairs.push_back({(uint32_t)g, s});
                }
            }
        }
        out = toLists(pairs, (std::size_t)kGroupLimit, true);
    };
    readArrays(layer->m_groups, m_members, true);
    readArrays(layer->m_staticGroups, m_static, false);
    readArrays(layer->m_optimizedGroups, m_optimized, false);
    m_spawnIn = toLists(spawnPairs, (std::size_t)kGroupLimit, false);
    finishGroups(n);

    // The spawn walk's lists, one per channel.
    spawnLists.clear();
    if (cocos2d::CCDictionary* dict = layer->m_spawnObjects) {
        for (cocos2d::CCDictElement* el = dict->m_pElements; el; el = static_cast<cocos2d::CCDictElement*>(el->hh.next)) {
            auto* arr = static_cast<cocos2d::CCArray*>(el->getObject());
            std::vector<int> list;
            if (arr && arr->data) {
                list.reserve(arr->data->num);
                for (unsigned j = 0; j < arr->data->num; j++) {
                    const int s = slotOf(static_cast<GameObject*>(arr->data->arr[j]));
                    list.push_back(s);  // kept even when unknown (-1): the walk's index counts it
                }
            }
            spawnLists.push_back({(int)el->getIntKey(), std::move(list)});
        }
        std::sort(spawnLists.begin(), spawnLists.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    // The remap maps spawn triggers index by their m_remapKey, and the group
    // parents a target move looks for first (both only read).
    enable22Changes = layer->m_enable22Changes;
    remapTables.clear();
    remapTables.reserve(layer->m_spawnRemapTriggers.size());
    for (const auto& table : layer->m_spawnRemapTriggers) {
        std::vector<std::pair<int, int>> pairs;
        gucci::fillFrom(pairs, table);  // core/platform.hpp
        std::sort(pairs.begin(), pairs.end());
        remapTables.push_back(std::move(pairs));
    }
    groupParents.clear();
    if (cocos2d::CCDictionary* dict = layer->m_parentGroupsDict) {
        for (cocos2d::CCDictElement* el = dict->m_pElements; el; el = static_cast<cocos2d::CCDictElement*>(el->hh.next)) {
            const int s = slotOf(static_cast<GameObject*>(el->getObject()));
            if (s >= 0) groupParents.push_back({(int)el->getIntKey(), s});
        }
        std::sort(groupParents.begin(), groupParents.end());
    }

    // Who names each group.
    std::vector<std::pair<uint32_t, int>> targetPairs;
    std::vector<int> named;
    for (int i = 0; i < n; i++) {
        if (!(flags[(std::size_t)i] & kEffect)) continue;
        auto* e = static_cast<EffectGameObject*>(slots[(std::size_t)i]);
        const int di = defOfSlot[(std::size_t)i];
        namedGroups(*this, di >= 0 ? &triggers[(std::size_t)di] : nullptr, e, named);
        for (int g : named) targetPairs.push_back({(uint32_t)g, i});
    }
    m_targeting = toLists(targetPairs, (std::size_t)kGroupLimit, true);
    m_groupModelled.assign((std::size_t)kGroupLimit, 1);
    for (std::size_t g = 0; g < (std::size_t)kGroupLimit; g++) {
        if (m_targeting.at(g).empty()) continue;
        for (int s : m_members.at(g)) flags[(std::size_t)s] |= kReferenced;
        for (int s : m_static.at(g)) flags[(std::size_t)s] |= kReferenced;
        for (int s : m_optimized.at(g)) flags[(std::size_t)s] |= kReferenced;
        // What a killer in the group is worth (world::certaintyOf): an effect
        // object that names it and has no TriggerDef changes nothing physical
        // (colour, pulse, alpha), and any kind the World does not run means
        // the group can end up somewhere the run does not have it.
        for (int s : m_targeting.at(g)) {
            const int di = defOfSlot[(std::size_t)s];
            if (di < 0) continue;
            if (!kindShipped(triggers[(std::size_t)di].kind)) {
                m_groupModelled[g] = 0;
                break;
            }
        }
    }

    // Where each group's collidable members stand now.
    groupBaseBox.assign((std::size_t)kGroupLimit, Box{});
    for (std::size_t g = 0; g < (std::size_t)kGroupLimit; g++) {
        for (int s : m_members.at(g)) {
            if (!(flags[(std::size_t)s] & kCollidable)) continue;
            GameObject* o = slots[(std::size_t)s];
            const float w = o->m_width * std::abs(o->m_scaleX);
            const float h = o->m_height * std::abs(o->m_scaleY);
            // Half the diagonal covers any rotation; a radius covers a circle.
            const float r = std::max({0.5f * std::sqrt(w * w + h * h), o->m_objectRadius, 15.0f});
            const float x = (float)o->m_positionX;
            const float y = (float)o->m_positionY;
            groupBaseBox[g].add(x - r, y - r, x + r, y + r);
        }
    }
}

namespace {
std::shared_ptr<const WorldDef> g_def;
}

void forgetLevel() {
    g_def.reset();
    forgetMaterialDef();
    // The ledger's ring names objects of the level being let go (design step
    // 10): it goes with them rather than be undone into objects that are gone.
    forgetLedger();
}

std::shared_ptr<const WorldDef> WorldDef::get(GJBaseGameLayer* layer) {
    if (!layer || !layer->m_objects) return nullptr;
    // Still loading: optimizeMoveGroups (in setupHasCompleted, right after the
    // progress reaches 1) has not split the groups into their final arrays.
    if (layer->m_loadingProgress < 1.0f) return nullptr;
    const unsigned gen = moverCacheGeneration();
    const int n = (int)layer->m_objects->count();
    if (g_def && g_def->pl == layer && g_def->gen == gen && g_def->objectCount == n) return g_def;
    auto def = std::make_shared<WorldDef>();
    def->gen = gen;
    def->build(layer);
    g_def = def;
    devlog::logf(devlog::Cat::System, "world: level read, %d objects, %zu triggers, %zu spawn channels, %d stray group members",
                 def->objectCount, def->triggers.size(), def->spawnLists.size(), def->strayMembers);
    // The level-load check of what was just read; a failure turns the World
    // off (never the level).
    World::checkDef(*def, layer);
    return g_def;
}

}  // namespace world
