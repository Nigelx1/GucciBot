#pragma once

// The World's per-run state (trigger design steps 3 and 4): what a run of the
// copies carries of the level's triggers and moving objects, and the log of
// what those did to the objects.
//
// WorldState is copied by value into every SimSnapshot and SimStart, and
// compares with a defaulted ==, so a branch and a kept start put it back
// exactly and a check can say so. Its bigger containers are shared until one
// copy changes them (Cow), so a copy costs a few reference counts. Nothing in
// it points into the game: objects are WorldDef slots, remap lists sit in its
// own pool. A run with the World steps it every tick (world/step.cpp) and
// fires the Tier A triggers on it (world/fire.cpp).
//
// OpLog is append-only. Every 16 ticks its open chunk is sealed and shared by
// every copy of the log, so a snapshot copies a few pointers and at most 16
// ticks of ops. ObjectCache works out where an object stands from the ops of
// its groups, with the game's own arithmetic, and knows from the chunks'
// lineage whether what it has cached still belongs to the log it is asked
// about - a branch that was abandoned never leaks into another.

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

class GameObject;

namespace world {

class WorldDef;
struct BaseTable;

// Every group id the game uses is clamped to 0..9999 (getGroup 0x22428a,
// toggleGroup 0x223bea), and the toggle bits are a vector<bool> of 10000
// (GJEffectManager::init 0x25ae57).
inline constexpr int kGroupLimit = 10000;
inline constexpr std::size_t kToggleWords = (kGroupLimit + 31) / 32;

// A range of WorldState::remaps (the m_remapKeys of an action).
struct Remap {
    int begin = 0;
    int count = 0;
    bool operator==(const Remap&) const = default;
};

// A running move, rotate, follow, follow-player-Y, transform or keyframe
// command: GroupCommandObject2 (EM +0x608, 0x208 bytes) without its
// containers. The fields are in the game's order and carry its offsets (up to
// +0x1b1 they are WCmd's own offsets too; world/capture.cpp pins each of those
// to the binding member it is copied from).
// m_keyframes becomes its count (keyframe commands are not ported: their
// groups are uncertain), m_gameObject a WorldDef slot, m_remapKeys a Remap.
struct WCmd {
    int uid = 0;                          // +0x000 m_groupCommandUniqueID
    float moveOffsetX = 0.0f;             // +0x004 m_moveOffset
    float moveOffsetY = 0.0f;             // +0x008
    int easingType = 0;                   // +0x00c
    double easingRate = 0.0;              // +0x010
    double duration = 0.0;                // +0x018
    double deltaTime = 0.0;               // +0x020 (step adds (double)dt)
    int targetGroup = 0;                  // +0x028
    int centerGroup = 0;                  // +0x02c
    double currentX = 0.0;                // +0x030 m_currentXOffset
    double currentY = 0.0;                // +0x038
    double deltaX = 0.0;                  // +0x040
    double deltaY = 0.0;                  // +0x048
    double oldDeltaX = 0.0;               // +0x050
    double oldDeltaY = 0.0;               // +0x058
    double lockedCurrentX = 0.0;          // +0x060
    double lockedCurrentY = 0.0;          // +0x068
    bool finished = false;                // +0x070
    bool disabled = false;                // +0x071 (paused: step returns at once)
    bool finishRelated = false;           // +0x072
    bool lockPlayerX = false;             // +0x073
    bool lockPlayerY = false;             // +0x074
    bool lockCameraX = false;             // +0x075
    bool lockCameraY = false;             // +0x076
    bool lockedInX = false;               // +0x077
    bool lockedInY = false;               // +0x078
    double modX = 0.0;                    // +0x080
    double modY = 0.0;                    // +0x088
    double rotateValue = 0.0;             // +0x090 m_currentRotateOrTransformValue
    double rotateDelta = 0.0;             // +0x098
    double interpOne1 = 0.0;              // +0x0a0
    double interpOne2 = 0.0;              // +0x0a8
    double rotationOffset = 0.0;          // +0x0b0
    bool lockObjectRotation = false;      // +0x0b8
    int targetPlayer = 0;                 // +0x0bc
    double followXMod = 0.0;              // +0x0c0
    double followYMod = 0.0;              // +0x0c8
    int commandType = 0;                  // +0x0d0 (off::kCmdMove ..)
    double interp1 = 0.0;                 // +0x0d8
    double interp2 = 0.0;                 // +0x0e0
    double keyframeRelated = 0.0;         // +0x0e8
    double targetScaleX = 0.0;            // +0x0f0
    double targetScaleY = 0.0;            // +0x0f8
    double property450 = 0.0;             // +0x100
    double property451 = 0.0;             // +0x108
    double interpZero1 = 0.0;             // +0x110
    double interpZero2 = 0.0;             // +0x118
    bool onlyMove = false;                // +0x120
    bool transformFlag = false;           // +0x121 m_transformRelatedFalse
    bool relativeRotation = false;        // +0x122
    double interpRelated1 = 0.0;          // +0x128
    double interpRelated2 = 0.0;          // +0x130
    double followYSpeed = 0.0;            // +0x138
    double followYDelay = 0.0;            // +0x140
    int followYOffset = 0;                // +0x148
    double followYMaxSpeed = 0.0;         // +0x150
    int triggerUid = 0;                   // +0x158
    int controlId = 0;                    // +0x15c
    double deltaX3 = 0.0;                 // +0x160
    double deltaY3 = 0.0;                 // +0x168
    double oldDeltaX3 = 0.0;              // +0x170
    double oldDeltaY3 = 0.0;              // +0x178
    double delta3Related = 0.0;           // +0x180
    double unusedDouble = 0.0;            // +0x188
    int actionType1 = 0;                  // +0x190 (1: x, 2: y, 3/4: rotate value)
    int actionType2 = 0;                  // +0x194
    double actionValue1 = 0.0;            // +0x198
    double actionValue2 = 0.0;            // +0x1a0
    bool interpRelatedFalse = false;      // +0x1a8
    float deltaTimeFloat = 0.0f;          // +0x1ac m_deltaTimeInFloat (the eased time)
    bool alreadyUpdated = false;          // +0x1b0
    bool doUpdate = false;                // +0x1b1
    int keyframeCount = 0;                // +0x1b8 m_keyframes.size()
    float splineX = 0.0f;                 // +0x1d0
    float splineY = 0.0f;                 // +0x1d4
    int objectSlot = -1;                  // +0x1d8 m_gameObject as a WorldDef slot (-1: none or unknown)
    int objectUid = 0;                    //        and its uid (0: none)
    float objectRotation = 0.0f;          // +0x1e0
    Remap remap;                          // +0x1e8 m_remapKeys
    bool interpRelatedTrue = false;       // +0x200
    int unk204 = 0;                       // +0x204

    // Bit for bit: a -0.0 is not a 0.0 and a NaN is itself (world/objects.cpp).
    bool operator==(const WCmd& other) const;
};

// A queued spawn: SpawnTriggerAction (EM +0x5a8, 0x48 bytes). updateSpawnTriggers
// 0x261da0 adds (double)dt to delta while it is not disabled and fires once
// delta >= duration, with delta - duration as the spawn's own delay.
struct WSpawn {
    bool finished = false;   // +0x00
    bool disabled = false;   // +0x01 (paused)
    double duration = 0.0;   // +0x08 the delay
    double delta = 0.0;      // +0x10 time waited so far
    int targetGroup = 0;     // +0x18
    int triggerUid = 0;      // +0x1c
    int controlId = 0;       // +0x20
    bool ordered = false;    // +0x24
    int objectSlot = -1;     // +0x28 m_gameObject: a single trigger to fire instead of a group
    int objectUid = 0;
    Remap remap;             // +0x30

    bool operator==(const WSpawn& other) const;  // bit for bit
};

// A count trigger waiting on an item: CountTriggerAction (0x40 bytes) in
// m_countTriggerActions under its item id.
struct WCountListener {
    int item = 0;  // the map key
    bool disabled = false;
    int previousCount = 0;
    int targetCount = 0;
    int targetGroup = 0;
    bool activateGroup = false;
    int triggerUid = 0;
    int controlId = 0;
    int itemId = 0;
    bool multiActivate = false;
    Remap remap;

    bool operator==(const WCountListener&) const = default;
};

// A collision trigger waiting on two blocks: CollisionTriggerAction (0x38).
struct WCollisionListener {
    bool disabled = false;
    int blockA = 0;  // registerCollisionTrigger keeps the larger id here ...
    int blockB = 0;  // ... and the smaller one here (0x25c4a9-0x25c4b6)
    int targetGroup = 0;
    int triggerOnExit = 0;
    bool activateGroup = false;
    int triggerUid = 0;
    int controlId = 0;
    Remap remap;

    bool operator==(const WCollisionListener&) const = default;
};

// A touch trigger waiting on a button: TouchToggleAction (0x38).
struct WTouchListener {
    bool disabled = false;
    int targetGroup = 0;
    bool holdMode = false;
    int touchType = 0;
    int touchControl = 0;
    int triggerUid = 0;
    int controlId = 0;
    bool dualMode = false;
    Remap remap;

    bool operator==(const WTouchListener&) const = default;
};

// A timer: TimerItem (0x58) in m_timerItemMap. Kept in the map's own list
// order, which is the order updateTimers 0x263340 walks them in.
struct WTimer {
    int key = 0;
    int itemId = 0;
    double time = 0.0;
    bool active = false;
    float timeMod = 0.0f;
    bool ignoreTimeWarp = false;
    double targetTime = 0.0;
    bool stopTimeEnabled = false;
    int targetGroup = 0;
    int triggerUid = 0;
    int controlId = 0;
    Remap remap;
    bool disabled = false;

    bool operator==(const WTimer& other) const;  // bit for bit
};

// A time trigger waiting on a timer: TimerTriggerAction (0x38), under the
// timer's item id.
struct WTimerListener {
    int item = 0;  // the map key
    bool disabled = false;
    float time = 0.0f;
    float targetTime = 0.0f;
    int targetGroup = 0;
    int triggerUid = 0;
    int controlId = 0;
    int itemId = 0;
    bool multiActivate = false;
    Remap remap;

    bool operator==(const WTimerListener& other) const;  // bit for bit
};

// Where a sequence trigger (3607) has got to for one set of remap keys. The
// game keeps this on the object itself - SequenceTriggerGameObject's two maps
// m_sequenceTimes (+0x758) and m_sequenceIndices (+0x798), keyed by the last
// remap key negated - which a run must never write, so the run carries its own
// overlay of them, read from the object the first time it fires one.
struct WSequence {
    int slot = -1;    // the trigger's WorldDef slot
    int key = 0;      // its key in the object's maps (0 without unique remap)
    float time = 0.0f;  // m_sequenceTimes: the level time it last fired at (-1: never)
    int index = 0;      // m_sequenceIndices: how far into the list it has got

    bool operator==(const WSequence& other) const;  // bit for bit
};

// An event trigger waiting on a game event: EventTriggerInstance (0x28) under
// (event, a * 10000 + b) in m_gameState (+0x588).
struct WEventListener {
    int event = 0;
    int key = 0;
    int targetGroup = 0;
    int triggerUid = 0;
    int controlId = 0;
    bool inactive = false;
    Remap remap;

    bool operator==(const WEventListener&) const = default;
};

// The command index an event last fired at (+0x598): gameEventTriggered fires
// an event at most once per command index.
struct WEventStamp {
    int event = 0;
    int key = 0;
    int index = 0;

    bool operator==(const WEventStamp&) const = default;
};

// How far the run's ops can have carried the members of one group: the sums
// of |dx| and |dy| of every move op on it (translate, optimized translate,
// silent place) and the count of every op on it, toggles included. The
// materializer grows a group's envelope by these (world/materialize.cpp) and
// looks at a group again only when its count has moved. Bit for bit on ==: a
// NaN move makes a NaN sum, which is still the same state.
struct GroupReach {
    double x = 0.0;
    double y = 0.0;
    uint32_t ops = 0;

    bool operator==(const GroupReach& other) const;
};

using ToggleWords = std::array<uint32_t, kToggleWords>;

// Every group enabled, as GJEffectManager::init leaves the toggle bits (bits
// 0..9999 set, the unused rest of the last word clear).
constexpr ToggleWords allGroupsEnabled() {
    ToggleWords words{};
    for (int g = 0; g < kGroupLimit; g++) words[(std::size_t)g >> 5] |= uint32_t{1} << (g & 31);
    return words;
}

// A value shared by every state copied from the one that made it, until one
// of them changes it (edit copies it first while it is shared). Copying a
// WorldState into a snapshot or a kept start then costs a few reference
// counts, not the commands and listeners themselves; == compares the values.
// Runs are only stepped on the game's thread, so the use count is exact.
template <class T>
class Cow {
   public:
    Cow() = default;
    explicit Cow(std::shared_ptr<T> shared) : m_p(std::move(shared)) {}

    const T& operator*() const { return m_p ? *m_p : empty(); }
    const T* operator->() const { return &**this; }

    T& edit() {
        if (!m_p) {
            m_p = std::make_shared<T>();
        } else if (m_p.use_count() > 1) {
            m_p = std::make_shared<T>(*m_p);
        }
        return *m_p;
    }
    void assign(T value) { m_p = std::make_shared<T>(std::move(value)); }
    // The same value as `other` from now on, held once.
    void share(const Cow& other) { m_p = other.m_p; }
    bool sharedWith(const Cow& other) const { return m_p == other.m_p; }

    bool operator==(const Cow& other) const { return m_p == other.m_p || **this == *other; }

   private:
    static const T& empty() {
        static const T value{};
        return value;
    }
    std::shared_ptr<T> m_p;
};

// The toggle bits every state starts from: one shared set, never edited in
// place (it is always shared, so edit copies it).
inline Cow<ToggleWords> allGroupsEnabledShared() {
    static const std::shared_ptr<ToggleWords> words = std::make_shared<ToggleWords>(allGroupsEnabled());
    return Cow<ToggleWords>(words);
}

struct WorldState {
    // ------------------------------------------------ the tick
    // World ticks since the state was imported (World::captureLive starts at 0).
    int tick = 0;
    // m_gameState.m_commandIndex: what moveObjects stamps (+0x4dc) and the
    // contact rule compares with. The layer's own field is what a run moves
    // (processCommands' += 2 in Sim::step); the World copies it here at the
    // top of every tick it steps.
    uint32_t commandIndex = 0;
    // The game's counter of command uids (GroupCommandObject2::reset 0x25770c
    // reads and bumps the global at 0x6ba170): a command a run creates takes
    // the uid the game would have given it, from the run's own copy.
    int nextCommandUid = 0;
    float timeMod = 0.0f;           // m_timeModRelated: a speed parked for the next tick
    bool timeMod2 = false;          // m_timeModRelated2
    float timeWarp = 1.0f;          // m_timeWarp (+0x330)
    float queuedTimeWarp = 0.0f;    // m_queuedTimeWarp (+0x334)
    float appliedTimeWarp = 1.0f;   // m_timeWarpRelated (+0x338): what applyTimeWarp last applied
    int channel = 0;                // m_currentChannel
    // m_spawnChannelRelated0 (how far into each channel's spawn list) and
    // m_spawnChannelRelated1 (whether the walk goes back), sorted by channel:
    // the game only ever looks them up by key.
    std::vector<std::pair<int, int>> spawnCursor;
    std::vector<std::pair<int, bool>> goingBack;

    // ------------------------------------------------ running actions
    Cow<std::vector<WCmd>> cmds;      // in the command vector's order (a finished one stays one more step)
    Cow<std::vector<WSpawn>> spawns;  // in the spawn queue's order
    Cow<std::vector<int>> remaps;     // the pool every Remap indexes
    // A kept start's state leaves out the commands, spawns and timers, which
    // change on every tick they run: a copy per tick of the real game would
    // pile up for ten seconds of ticks. The ledger's keyframes (design step
    // 10) are what keep them for a start; until then such a state only holds
    // the rest (the groups those actions move are still in uncertainGroups).
    bool partial = false;

    // ------------------------------------------------ groups
    Cow<ToggleWords> toggleBits = allGroupsEnabledShared();  // EM +0x4f8
    // The net +0x4c0 counter change the run's toggles made to each group's
    // members (toggleGroup 0x223c6c / 0x223ca9), sorted by group. The objects
    // themselves are worked out by ObjectCache from the Toggle ops.
    std::vector<std::pair<int, int8_t>> toggleDelta;
    // The triggers the run fired that only fire once (the design's
    // triggeredOnce): an overlay on the objects' own flags, empty on import.
    std::vector<int> triggeredOnce;
    // storeTriggeredID 0x261ff0's set<pair<int, int>> (EM +0x558), sorted.
    Cow<std::vector<std::pair<int, int>>> triggeredIds;
    // GJBaseGameLayer::canBeActivatedByPlayer's contact map for each copy (0:
    // the copy of player 1, 1: of player 2): object uid -> the command index
    // it was last touched at, sorted by uid. The game keeps it in
    // m_gameState.m_activatedObjectIDs keyed by (object uid, player uid); a
    // copy has its own uid, so the live map never holds a copy's contact.
    std::vector<std::pair<int, uint32_t>> contact[2];

    // ------------------------------------------------ items, timers, listeners
    Cow<std::vector<std::pair<int, int>>> items;            // m_itemCountMap, sorted by item
    Cow<std::vector<std::pair<int, int>>> persistentItems;  // m_persistentItemCountMap, sorted by item
    Cow<std::vector<WTimer>> timers;
    Cow<std::vector<WCountListener>> countListeners;        // by item, each item's list in its own order
    Cow<std::vector<WCollisionListener>> collisionListeners;
    Cow<std::vector<WTouchListener>> touchListeners;
    Cow<std::vector<WTimerListener>> timerListeners;        // by item, each item's list in its own order
    Cow<std::vector<WEventListener>> eventListeners;        // in the map's (event, key) order
    Cow<std::vector<WEventStamp>> eventStamps;
    // m_persistentTimerItemSet (EM +0x3b0): the items a persistent item
    // trigger (3641) keeps across attempts, sorted.
    Cow<std::vector<int>> persistentTimers;
    // The sequence triggers the run has fired, sorted by (slot, key).
    Cow<std::vector<WSequence>> sequences;

    // The replay system's teleport random state as the run has moved it on
    // (a group teleport draws from it, see nextRandom). Only read from
    // ReplaySystem::m_teleportRandomState, never written back.
    uint64_t seed = 0;
    // The game's own generator (the global at 0x6c2e90), which the random
    // triggers and a spawn trigger's delay spread draw from, as the run has
    // moved it on. Taken from the game when the run begins and never written
    // back: the real game draws from it between the run's ticks, so a run only
    // ever guesses - it marks what it picks uncertain (world/fire.cpp (aa)).
    uint64_t randomSeed = 0;

    // ------------------------------------------------ the players
    // The design kept the player control trigger's stops here, per copy.
    // activatePlayerControlTrigger 0x2174e0 keeps no state at all: it releases
    // the buttons and clears the rotation and slide of the players there and
    // then, and the copies carry that in their own fields (which every
    // snapshot already keeps). Left as it is rather than removed: the
    // keyframes of the ledger compare states field for field, and a state
    // written before this stays equal to one written after.
    std::array<uint8_t, 2> playerControl{};
    // m_gameState.m_unkBool31 (the layer's +0x860): the options trigger 2899's
    // "unlink dual gravity", which the copies' gravity reads. Imported from
    // the live game and written back to the layer at the top of every tick, so
    // a branch that fired the trigger and was thrown away leaves nothing
    // behind (Sim::end puts the game state back in any case).
    bool unlinkedDual = false;
    // The level has been ended by an end trigger (3600).
    bool levelEnd = false;
    // The level's points and the time it has been running (the layer's +0x864
    // and +0x3560), which an item trigger reads as item modes 3 and 4 and an
    // item edit trigger writes. A run carries both rather than writing the
    // game: the points are not in the state Sim::begin saves, and the time is
    // not in the game state at all.
    int points = 0;
    double levelTime = 0.0;
    // Set by every fired trigger: the run's shape of commands and listeners
    // changed (the base timeline and the ledger's keyframes look at it).
    bool dirtyShape = false;

    // ------------------------------------------------ what is not modelled
    // Groups something the World does not port yet moves or toggles (sorted,
    // unique): a killer in one is uncertain rather than silently wrong.
    std::vector<uint16_t> uncertainGroups;
    // Groups whose triggers fire when the World cannot tell (sorted, unique):
    // something it does not follow spawns or toggles them, so every group
    // those triggers name is in uncertainGroups too (markSpawnUncertain). A
    // group is walked once; this is the list of the ones already walked.
    std::vector<uint16_t> uncertainSpawns;
    // An area effect is running: it can move any object in its area,
    // whatever its groups, so nothing is certain.
    bool uncertainAny = false;

    // ------------------------------------------------ what the ops reached
    // The groups the run's ops touched, sorted by group (see GroupReach). It
    // is a function of the log, kept here so a snapshot and a start carry it
    // with the log it describes.
    std::vector<std::pair<uint16_t, GroupReach>> reach;

    bool operator==(const WorldState&) const = default;

    bool groupEnabled(int group) const {
        const int g = group < 0 ? 0 : (group > kGroupLimit - 1 ? kGroupLimit - 1 : group);
        return ((*toggleBits)[(std::size_t)g >> 5] >> (g & 31)) & 1u;
    }
};

// ------------------------------------------------------------ the op log

enum class OpKind : uint8_t {
    // moveObjects 0x22dd50 on m_staticGroups[group] with the node's +0x38/+0x40
    // sums, rounded to float once (processMoveActions, world/step.cpp (e)).
    Translate = 0,
    // The same on m_optimizedGroups[group] with the node's +0x90/+0x98 sums.
    TranslateOptimized = 1,
    // rotateObjects / transformObjects: not ported, the objects they touch
    // come out uncertain.
    RotateAbout = 2,
    Scale = 3,
    // toggleGroup 0x223bc0 on m_groups[group]: sign +1 activates, -1 deactivates.
    Toggle = 4,
    // A silent move (triggerMoveCommand 0x21edc1-0x21eea8) on m_groups[group]:
    // the double position moves by (double)dx / dy at once (x only without
    // +0x2c8) and m_lastPosition takes the new position; the move marker is
    // not stamped.
    Place = 5,
};

struct Op {
    uint32_t tick = 0;          // the World tick the op belongs to
    uint32_t commandIndex = 0;  // m_commandIndex while it ran (moveObjects' marker)
    OpKind kind = OpKind::Translate;
    int8_t sign = 0;
    int group = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    double angle = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double sx = 1.0;
    double sy = 1.0;

    bool operator==(const Op&) const = default;
};

// Every field the same bit for bit.
bool bitEqual(const Op& a, const Op& b);

struct Chunk {
    // Unique for the life of the process. Open chunks and sealed ones draw
    // from the same counter, so a key names one chunk and nothing else. (The
    // design checked lineage by chunk pointer and an epoch; a freed chunk's
    // address given out again could pass for it, a key cannot.)
    uint64_t id = 0;
    // A sealed chunk: the key its ops had while they were still open.
    uint64_t fromKey = 0;
    uint32_t firstTick = 0;  // the start of its 16-tick window
    uint32_t lastTick = 0;   // the latest tick of an op in it
    std::vector<Op> ops;
    // Sealed chunks only: (group, op index), sorted, so the ops of a group are
    // a binary search away. (The design's map of vectors, as one array: a
    // sealed chunk is built once and only read.)
    std::vector<std::pair<int, uint32_t>> byGroup;

    std::span<const std::pair<int, uint32_t>> opsOfGroup(int group) const;
};

class OpLog {
   public:
    // A chunk covers the ticks [16k, 16k + 16).
    static constexpr uint32_t kSealTicks = 16;

    OpLog();
    // A copy holds the same ops and shares the sealed chunks, but its open
    // chunk gets a key of its own: whatever is appended to either from here
    // on can never be taken for the other's (ObjectCache checks the key).
    OpLog(const OpLog& other);
    OpLog& operator=(const OpLog& other);
    // A move keeps the key; the moved-from log starts over with a new one.
    OpLog(OpLog&& other) noexcept;
    OpLog& operator=(OpLog&& other) noexcept;

    // The tick about to emit ops: seals the open chunk once the tick has left
    // its window. Ticks never go down within one log.
    void beginTick(uint32_t tick);
    void append(const Op& op);
    // Empty, at `tick`, with a new key.
    void reset(uint32_t tick = 0);

    const std::vector<std::shared_ptr<const Chunk>>& sealed() const { return m_sealed; }
    const Chunk& open() const { return m_open; }
    uint64_t openKey() const { return m_open.id; }
    std::size_t size() const;
    bool empty() const { return size() == 0; }
    // The same ops in the same order, however they are chunked.
    bool sameOps(const OpLog& other) const;

    static uint64_t nextKey();

   private:
    void seal();

    std::vector<std::shared_ptr<const Chunk>> m_sealed;
    Chunk m_open;
};

// ------------------------------------------------------------ real ticks

// What a step of the real game needs from outside its own state to be stepped
// again: the step's delta and the lock inputs GJBaseGameLayer::update worked
// out from player 1 before its update (world/step.cpp (m)). The ledger records
// one of these per real tick (world/ledger.hpp) so a keyframe can be stepped
// forward to the tick a kept start stands at.
struct LockInput {
    float dt = 0.0f;
    float dx = 0.0f;  // EM +0x800
    float dy = 0.0f;  // EM +0x804

    bool operator==(const LockInput&) const = default;
};

// ------------------------------------------------------------ poses

// Where an object stands and what the World's ops left in it.
struct Pose {
    double x = 0.0;             // m_positionX (+0x3b8)
    double y = 0.0;             // m_positionY (+0x3c0)
    float lastX = 0.0f;         // m_lastPosition (+0x4d0)
    float lastY = 0.0f;         // (+0x4d4)
    uint32_t marker = 0;        // +0x4dc: the command index of its last move
    float rot = 0.0f;           // the node's rotation (x)
    float sx = 1.0f;            // the node's scale
    float sy = 1.0f;
    // m_enabledGroupsCounter (+0x4c0) itself rather than the design's int8
    // change to it: toggleGroup moves the int32 by one per array entry, and
    // the disabled flag is its sign (0x223c7c), which a delta alone can't give.
    int32_t counter = 0;
    bool disabled = false;      // m_isGroupDisabled (+0x28e)
    bool noMoveX = false;       // +0x2c8 as the base had it: moveObjects skips the x move
    bool skipLast = false;      // +0x512 as the base had it: moveObjects skips the last position
    bool uncertain = false;     // an op the World does not port touched it
};

// Every field the same bit for bit (a -0.0 is not a 0.0, a NaN is itself).
bool bitEqual(const Pose& a, const Pose& b);

// Where the objects of a run stand, worked out from a base and the run's log.
class ObjectCache {
   public:
    // A new run over `def`: forgets every pose and base.
    void reset(std::shared_ptr<const WorldDef> def);
    const WorldDef* def() const { return m_def.get(); }

    // The base of a slot (a kept start's overlay). Without one, the object's
    // live fields are read the first time the slot is asked about and kept,
    // so the base of a run does not move when the real game ticks between the
    // slices of a paused search. A slot first asked about after the real game
    // has moved it reads where it stands then: an exact base for that, and
    // for a start in the past, is the ledger's overlay (design step 10).
    void setBase(int slot, const Pose& base);
    const Pose& base(int slot);
    // The bases of the slots a table holds come from it (world/materialize.hpp):
    // read at one tick for every object a run can write, so the run's poses do
    // not depend on when a slot is first asked about - the real game moves on
    // between the slices of a paused search, and a kept start stands at a tick
    // the game has left. Set right after reset; a slot set with setBase wins.
    void setBaseTable(std::shared_ptr<const BaseTable> table);

    // The slot after every op of `log`.
    const Pose& pose(const OpLog& log, int slot);
    // The slot after the ops of ticks up to `tick` (nothing is cached).
    Pose poseAt(const OpLog& log, int slot, uint32_t tick);
    // The same worked out from the base alone, never from the cache or a
    // boundary: the second way the self-test and developer builds compare with.
    Pose replayFromBase(const OpLog& log, int slot, uint32_t uptoTick = std::numeric_limits<uint32_t>::max());

    // The fields a pose is made of, read from the object (nothing written).
    static Pose readLive(GameObject* o);
    // One op on one entry of the group's array, as the game does it.
    static void applyOnce(Pose& p, const Op& op);

    std::size_t cachedCount() const { return m_entries.size(); }
    std::size_t boundaryCount() const { return m_boundaries.size(); }

   private:
    struct Entry {
        Pose pose;
        uint64_t key = 0;    // the id of the chunk `chunk` names, or the open key
        uint32_t chunk = 0;  // index into the log's sealed chunks (== their count: the open one)
        uint32_t op = 0;     // ops of that chunk already in `pose`
    };
    struct BoundaryKey {
        int slot = 0;
        uint64_t chunk = 0;
        bool operator==(const BoundaryKey&) const = default;
    };
    struct BoundaryHash {
        std::size_t operator()(const BoundaryKey& k) const noexcept {
            return std::hash<uint64_t>{}(k.chunk * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)k.slot);
        }
    };

    bool resolve(Entry& e, const OpLog& log) const;
    // Replays the ops of `slot` from (chunk, op) onward, stopping before the
    // first op past `uptoTick`; records boundaries when `record`.
    void replay(Pose& p, const OpLog& log, int slot, std::size_t chunk, uint32_t op, uint32_t uptoTick, bool record);
    // The latest recorded boundary a replay of `slot` may start from, for ops
    // up to `uptoTick`: sets `chunk` past it, or leaves the base.
    bool nearestBoundary(const OpLog& log, int slot, uint32_t uptoTick, Pose& p, std::size_t& chunk);
    bool chunkTouches(const Chunk& c, int slot) const;
    int multiplicity(OpKind kind, int group, int slot) const;

    std::shared_ptr<const WorldDef> m_def;
    std::shared_ptr<const BaseTable> m_table;
    std::unordered_map<int, Pose> m_bases;
    std::unordered_map<int, Entry> m_entries;
    std::unordered_map<BoundaryKey, Pose, BoundaryHash> m_boundaries;
    std::vector<uint32_t> m_picks;  // replay's scratch list of op indices
};

}  // namespace world
