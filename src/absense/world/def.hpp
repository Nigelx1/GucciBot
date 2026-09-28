#pragma once

// WorldDef: what the level's objects and triggers are, read once per level
// (trigger design step 3). It is built the first time a run asks for it after
// the level has finished loading, and thrown away with the other per-level
// caches (moverCacheInvalidate). Everything in it is read-only afterwards.
//
// The group arrays are read straight from their slots in m_groups,
// m_staticGroups and m_optimizedGroups: a null slot is an empty group. The
// game's getGroup (0x224280) makes and registers an array for a group nobody
// is in, which the World must never do to the real level.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absense/world/state.hpp"

namespace world {

// Every trigger kind that can change what a copy collides with or where it
// goes. Anything else (colour, pulse, alpha, shader, SFX and song, particles,
// background, area fade and tint 3009/3010/3014/3015) is not physical and
// has no TriggerDef.
enum class Kind : uint8_t {
    None,
    Move,                    // 901
    Rotate,                  // 1346
    Follow,                  // 1347
    FollowPlayerY,           // 1814
    Scale,                   // 2067 (transform)
    Toggle,                  // 1049
    Spawn,                   // 1268
    Stop,                    // 1616
    Touch,                   // 1595
    ToggleOrb,               // 1594
    ToggleBlock,             // 3643
    Count,                   // 1611
    InstantCount,            // 1811
    Pickup,                  // 1817
    ItemEdit,                // 3619
    ItemCompare,             // 3620
    ItemPersistent,          // 3641
    Collision,               // 1815
    CollisionBlock,          // 1816
    InstantCollision,        // 3609
    TimeTrigger,             // 3614
    TimeEvent,               // 3615
    TimeControl,             // 3617
    Reset,                   // 3618
    Event,                   // 3604
    Sequence,                // 3607
    Random,                  // 1912
    AdvancedRandom,          // 2068
    PlayerControl,           // 1932
    Reverse,                 // 1917
    TimeWarp,                // 1935
    Gravity,                 // 2066
    RotateGameplay,          // 2900
    Teleport,                // 3022
    Speed,                   // 200-203, 1334
    Options,                 // 2899
    End,                     // 3600
    AdvancedFollow,          // 3016
    EditAdvancedFollow,      // 3660
    RetargetAdvancedFollow,  // 3661
    KeyframeAnim,            // 3033
    Keyframe,                // 3032
    AreaMove,                // 3006
    AreaRotate,              // 3007
    AreaScale,               // 3008
    EditAreaMove,            // 3011
    EditAreaRotate,          // 3012
    EditAreaScale,           // 3013
    EnterMove,               // 3017
    EnterRotate,             // 3018
    EnterScale,              // 3019
    EnterFade,               // 3020
    EnterTint,               // 3021
    AreaStop,                // 3024
    CameraZoom,              // 1913
    CameraStatic,            // 1914
    CameraOffset,            // 1916
    CameraEdge,              // 2062
    GameplayOffset,          // 2901
    CameraMode,              // 2925
};

// The rollout group a kind ships in (design step 12): A first, then B, then
// the rest. Until a kind has shipped it is uncertain whatever its tier.
enum class Tier : uint8_t { A, B, C };

Kind kindOf(int objectId);
Tier tierOf(Kind kind);
const char* kindName(Kind kind);
// Whether the World runs the kind: the Tier A kinds (world/step.cpp and
// world/fire.cpp). Every other kind's groups stay uncertain when it fires or
// runs.
bool kindShipped(Kind kind);

class WorldDef;

// The Tier D and E kinds - the area and enter effects, the advanced follow,
// the dynamic moves and rotates, the keyframe animations and the camera - are
// not run by the World, and world/tierd.cpp is where what each of them can
// reach is worked out from the game's own code.

// Level load: checks the game's own code at the places the reach below is
// read from, and logs what it found. A failure does not turn the World off -
// every area effect then makes every object of the run uncertain, which is
// what it did before this was worked out, and turning the World off would put
// every run back on MovingObjects instead, which is worse.
bool checkTierDE();

// Whether a fire of `kind` reaches objects the groups it names do not cover,
// so the whole run is uncertain rather than those groups.
bool fireReachesAnyObject(const WorldDef& def, Kind kind);

// Whether the level names objects rather than whole groups anywhere (the
// game's m_targetGroups keys). An area effect, an advanced follow and a
// dynamic move or rotate all take their objects out of the temporary group a
// key stands for when there is one, and nothing in the level names that
// group's members - so what they move on such a level cannot be marked, only
// the whole run. False on every level that has no keys, which is most of them.
bool targetsObjectsByKey(const WorldDef& def);

// The groups a keyframe animation trigger (3033) animates: its keyframe
// objects (3032) each animate the trigger's target group, or their own when
// the trigger names none.
struct TriggerDef;
void markKeyframeTargets(const WorldDef& def, WorldState& ws, const TriggerDef& trigger);

// What the Tier D and E kinds running in the live game right now can move, as
// the state a run starting from here carries it (World::captureLive): the
// groups go on ws.uncertainGroups unsorted, which captureLive sorts itself.
void markLiveTierDE(GJBaseGameLayer* pl, const WorldDef* def, WorldState& ws);

// A (group, old group, chance) entry of a spawn trigger's remap list or a
// random / sequence trigger's list (ChanceObject), as the object has it.
struct DefChance {
    int group = 0;
    int oldGroup = 0;
    int chance = 0;
};

struct Box {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = -1.0f;
    float maxY = -1.0f;
    bool empty() const { return maxX < minX; }
    void add(float x0, float y0, float x1, float y1) {
        if (empty()) {
            minX = x0, minY = y0, maxX = x1, maxY = y1;
            return;
        }
        if (x0 < minX) minX = x0;
        if (y0 < minY) minY = y0;
        if (x1 > maxX) maxX = x1;
        if (y1 > maxY) maxY = y1;
    }
};

// One trigger of the level and every setting the World reads from it. The
// fields are what the object holds; a spawn's remap is applied when it fires,
// never here.
struct TriggerDef {
    Kind kind = Kind::None;
    Tier tier = Tier::C;
    uint16_t objectId = 0;
    int slot = -1;            // the object's WorldDef slot
    int uid = 0;              // m_uniqueID
    float x = 0.0f;           // m_speedStart: where the spawn walk passes it
    float y = 0.0f;
    int channel = 0;          // m_channelValue
    bool touch = false;       // m_isTouchTriggered
    bool spawn = false;       // m_isSpawnTriggered
    bool multi = false;       // m_isMultiTriggered
    int target = 0;           // m_targetGroupID
    int center = 0;           // m_centerGroupID
    int control = 0;          // m_controlID
    // A subclass the kind's settings are read from was not what the object is.
    bool paramsMissing = false;

    // move 901 (triggerMoveCommand 0x21ea40), and the duration and easing of
    // every eased kind
    float moveX = 0.0f;
    float moveY = 0.0f;
    float duration = 0.0f;
    int easingType = 0;
    float easingRate = 0.0f;
    bool lockPlayerX = false;
    bool lockPlayerY = false;
    bool lockCameraX = false;
    bool lockCameraY = false;
    float modX = 0.0f;
    float modY = 0.0f;
    bool useTarget = false;
    int targetMode = 0;
    bool targetP1 = false;
    bool targetP2 = false;
    bool directionMode = false;
    float directionDistance = 0.0f;
    int targetModCenter = 0;
    bool dynamic = false;
    bool silent = false;
    bool smallStep = false;

    // rotate 1346
    float degrees = 0.0f;
    int times360 = 0;
    bool lockRotation = false;
    int rotationTarget = 0;
    float rotationOffset = 0.0f;
    int dynamicEasing = 0;

    // follow 1347, follow player Y 1814
    float followXMod = 0.0f;
    float followYMod = 0.0f;
    float followYSpeed = 0.0f;
    float followYDelay = 0.0f;
    int followYOffset = 0;
    float followYMaxSpeed = 0.0f;

    // scale 2067 (TransformTriggerGameObject)
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    bool divideX = false;
    bool divideY = false;
    bool onlyMove = false;
    bool relativeRotation = false;
    bool relativeScale = false;

    // toggles, counts, collisions, touches
    bool activateGroup = false;

    // spawn 1268 (SpawnTriggerGameObject)
    float spawnDelay = 0.0f;
    float spawnVariance = 0.0f;
    bool spawnOrdered = false;
    bool resetRemap = false;
    // m_remapKey (+0x758): its remap list's index in m_spawnRemapTriggers
    // (generateSpawnRemap 0x21dfb0), 0 when it has none.
    int remapKey = 0;
    int chanceBegin = 0;       // its remap list, or a random / sequence list, in WorldDef::chances
    int chanceCount = 0;

    // counts, pickups, items, timers
    int itemId = 0;
    int itemId2 = 0;
    int count = 0;             // CountTriggerGameObject::m_pickupCount
    int mode = 0;              // m_pickupTriggerMode
    bool countMulti = false;   // CountTriggerGameObject::m_multiActivate
    bool countOverride = false;
    float countMultiplier = 0.0f;
    int item1Mode = 0;         // ItemTriggerGameObject
    int item2Mode = 0;
    int targetItemMode = 0;
    float mod1 = 0.0f;
    float mod2 = 0.0f;
    int resultType1 = 0;
    int resultType2 = 0;
    int resultType3 = 0;
    float tolerance = 0.0f;
    int roundType1 = 0;
    int roundType2 = 0;
    int signType1 = 0;
    int signType2 = 0;
    bool persistent = false;
    bool targetAll = false;
    bool resetItem = false;
    bool timerItem = false;
    double startTime = 0.0;    // TimerTriggerGameObject
    double targetTime = 0.0;
    bool stopTimeEnabled = false;
    bool dontOverride = false;
    bool ignoreTimeWarp = false;
    float timerMod = 0.0f;
    bool startPaused = false;
    bool timerMulti = false;
    int controlType = 0;

    // collisions 1815 / 3609 (blocks A and B), collision block 1816 (its id)
    int blockA = 0;
    int blockB = 0;
    bool triggerOnExit = false;
    bool dynamicBlock = false;

    // touch 1595
    bool touchHold = false;
    int touchToggle = 0;
    int touchPlayer = 0;
    bool dualMode = false;

    // event 3604 (its ids in WorldDef::lists)
    int listBegin = 0;
    int listCount = 0;
    int extraId = 0;
    int extraId2 = 0;

    // player control 1932
    bool stopJump = false;
    bool stopMove = false;
    bool stopRotation = false;
    bool stopSlide = false;

    // stop 1616 (TriggerControlGameObject): m_customTriggerValue (+0x760: 0
    // stop, 1 pause, 2 resume) and m_targetControlID (+0x69c: the target is a
    // control id, not a group)
    int controlAction = 0;
    bool useControlId = false;

    // time warp 1935, gravity 2066, speed portals
    float timeWarp = 1.0f;
    float gravity = 0.0f;
    bool followCPP = false;
    float speed = 0.0f;
    bool hasNoEffects = false;  // GameObject::m_hasNoEffects (+0x41c): what a speed portal parks in m_timeModRelated2

    // rotate gameplay 2900
    int moveDirection = 0;
    int groundDirection = 0;
    bool editVelocity = false;
    bool overrideVelocity = false;
    float velocityModX = 0.0f;
    float velocityModY = 0.0f;
    bool changeChannel = false;
    bool channelOnly = false;
    int targetChannel = 0;
    bool instantOffset = false;
    bool dontSlide = false;

    // teleport 3022 (TeleportPortalObject)
    float teleportYOffset = 0.0f;
    bool teleportEase = false;
    bool staticForceEnabled = false;
    float staticForce = 0.0f;
    bool redirectForceEnabled = false;
    float redirectForceMod = 0.0f;
    float redirectForceMin = 0.0f;
    float redirectForceMax = 0.0f;
    bool saveOffset = false;
    bool ignoreX = false;
    bool ignoreY = false;
    int gravityMode = 0;
    bool staticForceAdditive = false;
    bool instantCamera = false;
    bool snapGround = false;
    bool redirectDash = false;
};

// Per-slot flags.
enum SlotFlag : uint8_t {
    // A copy can collide with it: the kinds the collision pass drops before
    // anything else (physics/collisions.cpp, the skip list) are not.
    kCollidable = 1 << 0,
    // m_hasExtendedCollision (+0x280): in the calculated list, not the buckets.
    kExtended = 1 << 1,
    // A member of a group some effect object names (targetingGroup): what
    // moves or toggles may read or change it.
    kReferenced = 1 << 2,
    // An effect object (m_classType 1).
    kEffect = 1 << 3,
    // Has a TriggerDef.
    kTrigger = 1 << 4,
    // GameObject::isSpawnableTrigger 0x1a26b0 says yes (it only looks at the id).
    kSpawnable = 1 << 5,
    // m_canBeControlled (+0x4f0): controlTriggersInGroup 0x21e20c skips anything else.
    kControllable = 1 << 6,
};

// A flat array of lists: list i is values[offsets[i] .. offsets[i + 1]).
template <class T>
struct Lists {
    std::vector<uint32_t> offsets;
    std::vector<T> values;
    std::span<const T> at(std::size_t i) const {
        if (i + 1 >= offsets.size()) return {};
        return std::span<const T>(values.data() + offsets[i], offsets[i + 1] - offsets[i]);
    }
    std::size_t size() const { return offsets.empty() ? 0 : offsets.size() - 1; }
};

class WorldDef {
   public:
    // The level's WorldDef, built on the first call once the layer has
    // finished loading (null before that, or without a layer). Kept until the
    // per-level caches are thrown away or the object count changes (the editor).
    static std::shared_ptr<const WorldDef> get(GJBaseGameLayer* pl);

    GJBaseGameLayer* pl = nullptr;
    unsigned gen = 0;
    int objectCount = 0;

    // m_objects, in its order: a slot is an index into it.
    std::vector<GameObject*> slots;
    std::vector<uint8_t> flags;
    std::vector<int> defOfSlot;  // the TriggerDef of a slot, or -1

    int slotOfUid(int uid) const;
    // The slot of an object of this level (-1 for anything else).
    int slotOf(const GameObject* o) const;

    // The distinct groups a slot is in, from the group arrays themselves.
    std::span<const uint16_t> groupsOf(int slot) const { return m_groupsOf.at((std::size_t)slot); }
    // A group's array as slots, sorted, an object in it twice listed twice:
    // m_groups (+0xf18), m_staticGroups (+0xf30) and m_optimizedGroups (+0xf48).
    std::span<const int> members(int group) const { return m_members.at(clampGroup(group)); }
    std::span<const int> staticMembers(int group) const { return m_static.at(clampGroup(group)); }
    std::span<const int> optimizedMembers(int group) const { return m_optimized.at(clampGroup(group)); }
    // How many times `slot` is in `list` (sorted).
    static int countIn(std::span<const int> list, int slot);

    std::vector<TriggerDef> triggers;
    std::unordered_map<const GameObject*, int> triggerOf;

    // m_spawnObjects: each channel's list (slots, in the list's order), sorted
    // by channel.
    std::vector<std::pair<int, std::vector<int>>> spawnLists;
    std::span<const int> spawnList(int channel) const;
    // The members of a group that spawnObjectsInOrder 0x21ae00 counts, in the
    // group array's order: effect objects isSpawnableTrigger says yes to, and
    // object 2065 with animate-on-trigger (+0x599). The design had this list
    // x-sorted; spawnObjectsInOrder walks m_groups as it stands (no sort of
    // its own, 0x21ae4d-0x21afe9), so the array's order is the game's.
    std::span<const int> spawnTriggeredIn(int group) const { return m_spawnIn.at(clampGroup(group)); }
    // Every effect object that names a group - as its target, centre, target
    // mode centre, rotation target, or in its random / sequence / remap list -
    // as slots. Whether one is physical is its defOfSlot.
    std::span<const int> targetingGroup(int group) const { return m_targeting.at(clampGroup(group)); }
    // Whether every physical trigger that names the group is a kind the World
    // runs (a group nothing names counts too). Worked out once with the rest
    // of the level, because a dying run asks it about its killer's groups and
    // the pathfinder makes thousands of dying runs a decision.
    bool groupModelled(int group) const {
        const std::size_t g = clampGroup(group);
        // A def that was not built from a level (the self-test's own) has no
        // answer: the careful one.
        return g < m_groupModelled.size() && m_groupModelled[g] != 0;
    }
    // Where each group's collidable members stood when the WorldDef was built
    // (empty for a group with none). Only a bound to grow: objects move after.
    std::vector<Box> groupBaseBox;

    std::vector<DefChance> chances;  // pools TriggerDef ranges index
    std::vector<int> lists;
    // The sequence triggers (3607) of the level, as trigger indices. They keep
    // their state on the object itself, so a run has to import it with the
    // rest (World::captureLive); the list is here to save walking every
    // trigger of the level for the levels - most of them - that have none.
    std::vector<int> sequenceDefs;

    // m_spawnRemapTriggers (+0x8f8): one old -> new group map per remap key,
    // each as sorted pairs. applyRemap 0x21b1c0 looks every remapped field up
    // in the maps of an action's keys in turn.
    std::vector<std::vector<std::pair<int, int>>> remapTables;
    // m_enable22Changes (+0x887): spawnGroup keys its once-per-step set on the
    // trigger's uid, and a target move picks among the group's objects.
    bool enable22Changes = false;
    // m_parentGroupsDict (+0xf60): the parent object of a group, as (group,
    // slot) sorted by group. tryGetMainObject 0x224520 asks it first.
    std::vector<std::pair<int, int>> groupParents;
    int parentOf(int group) const;

    // Group array members that are not in m_objects (none are expected).
    int strayMembers = 0;

    static std::size_t clampGroup(int group) {
        return (std::size_t)(group < 0 ? 0 : (group > kGroupLimit - 1 ? kGroupLimit - 1 : group));
    }

    // Builds from the level (world/def.cpp).
    void build(GJBaseGameLayer* layer);
    // Sets a group array directly (the self-test's own levels).
    void setGroups(std::vector<std::vector<int>> members, std::vector<std::vector<int>> staticMembers,
                   std::vector<std::vector<int>> optimizedMembers, int slotCount);

   private:
    void finishGroups(int slotCount);

    std::vector<int> m_uidDense;  // uid - m_uidBase -> slot, when the uids are dense enough
    int m_uidBase = 0;
    std::unordered_map<int, int> m_uidSparse;

    Lists<int> m_members;
    Lists<int> m_static;
    Lists<int> m_optimized;
    Lists<uint16_t> m_groupsOf;
    Lists<int> m_spawnIn;
    Lists<int> m_targeting;
    std::vector<uint8_t> m_groupModelled;
};

}  // namespace world
