// Tier D and E (design step 12d-e): the kinds the World does not run yet -
// keyframe animations, the camera, the area and enter effects, the dynamic
// moves and rotates and the advanced follow - and exactly what each of them
// can reach. None of them is ported: each one needs whole-object state the
// World's log of ops does not carry (the notes below say what, and where the
// game does it). What this file does instead is name the groups they move, as
// narrowly as the game's own code allows, so a run marks those uncertain
// rather than being silently wrong about them - and marks everything when it
// cannot name them.
//
// FACT TABLE - GeometryDash.exe 2.2081 (Windows), read with capstone.
// Addresses are offsets from the image base.
//
// (a) Area effects 3006-3010: triggerAreaEffect 0x2271c0 switches on the
//     object id (0x40c) - 3006 .. 3010 - and tail calls addAreaEffect
//     0x227480 with the instance vector for the kind: move +0x658, rotate
//     +0x670, scale +0x688, fade +0x6a0, tint +0x6b8 (the layer's, which are
//     m_gameState.m_moveEffectInstances and the four after it). Anything
//     outside 3006..3015 is not an area trigger at all. addAreaEffect either
//     finds the instance of the same (object, m_targetGroupID, m_centerGroupID,
//     m_controlID) and reloads it (0x2274bc-0x2274e4), or makes one whose
//     +0xac / +0xb0 are the trigger's m_targetGroupID (+0x5c8) and
//     m_centerGroupID (+0x5cc) and whose +0xc0 comes from m_targetGroups
//     (+0xf80) - the level's map of "target this object, not its group" keys.
//
// (b) processAreaEffects 0x2283e0 walks one of those vectors and, per
//     instance, takes the objects it moves from
//     m_targetGroupIndex (+0xc0) > 0 ? m_targetGroupsArray (+0xf78) at that
//     index : m_groups (+0xf18) [m_targetID clamped to 0..9999]
//     (0x228b45-0x228b8f). So an area effect reaches the members of one
//     group, not everything near it: the sections it works out before that
//     (0x228855-0x2289d4) are the effect's own area, written back onto the
//     trigger, not a set of objects. m_centerID (or -1 / -2 for the players,
//     0x228773-0x2287c6) only says where the effect is measured from.
//     A paused instance (+0xbc) is skipped whole (0x2285b0).
//
// (c) The move an area effect makes is real: moveAreaObject 0x22aab0 adds its
//     step to m_positionY (+0x3c0) and, unless +0x2c8 is set, to m_positionX
//     (+0x3b8), keeps the running offset in +0x2a0 / +0x2a4 and calls
//     updateObjectSection (vtable+0x490). Its rotate and scale do the same
//     through rotateAreaObjects 0x22a100 and transformAreaObjects 0x229720.
//     Why it is not ported: the effect's per-object state lives on the
//     objects (+0x2a0 / +0x2a4, +0x2a8, +0x2b8 / +0x2bc, +0x488 / +0x48c,
//     +0x4e0) and in the layer's two lists of touched objects, and a run
//     would have to write and put back every one of them; the value per
//     object also comes from getAreaObjectValue 0x228070 and
//     getEasedAreaValue 0x228260 over the effect's own animation map.
//
// (d) processAreaActions 0x228fc0 runs the scale, rotate and move vectors in
//     that order, and then puts back every object that no effect touched this
//     tick: for each object of m_areaObjects (+0xe28, count +0xee4) whose
//     +0x4e0 is below the command index it calls resetAreaObjectValues
//     0x227c30 and updateObjectSection, and the two lists are then swapped
//     (+0xe40, count +0xee8). So an object an effect moved is still moved
//     back by the game after the effect itself is gone: a run that begins in
//     that window has to count those objects as moved too, whatever is
//     running now.
//
// (e) Enter and exit effects 3017-3021: triggerObject 0x4a7686-0x4a772f maps
//     the object id to a code (-15 for 3017..3021, -3..-14 for the preset
//     ids) and writes it into m_enterChannelMap / m_exitChannelMap (+0x628 /
//     +0x640) under the trigger's channel, then adds the instance to
//     m_enterEffectInstanceVectors / m_exitEffectInstanceVectors (+0x5a8 /
//     +0x5e8) under the same channel - not to the area vectors of (a). They
//     are not group-addressed: an object takes its channel's effect as it
//     enters the screen. updateEnterEffects 0x20e960 only steps the instances
//     (EnterEffectInstance::updateTransitions), and where the game applies
//     one to an object has not been found yet, so nothing here claims they
//     are harmless: firing one still makes everything uncertain, as before.
//     The stop 3024 goes the same way round - stopCustomEnterEffect 0x20e760
//     twice, for the enter and the exit side of the trigger's mode (+0x740),
//     at 0x4a77f5-0x4a7835 - so it is channel-addressed too.
//
// (f) Advanced follow 3016 / 3660 / 3661: triggerObject 0x4a73fa-0x4a7494
//     keys an instance on (trigger, m_targetGroupID, the followed group,
//     m_controlID) - so the group an instance moves, m_group (+0x08), is the
//     trigger's own m_targetGroupID, and +0x0c is what it follows
//     (m_centerGroupID, or -1 / -2 / -3 for the players).
//     processAdvancedFollowActions
//     0x22f0e0 steps each AdvancedFollowInstance (0x20 bytes, m_gameObject
//     +0x00, m_group +0x08) at dt * 240 and drops the ones marked at +0x1e;
//     processAdvancedFollowAction 0x22f300 moves
//     m_targetGroups (+0xf80) [ (10 * (object+0x738 + 10) + object+0x739) *
//     1000000 + m_group ] > 0 ? m_targetGroupsArray (+0xf78) at that index :
//     m_groups [m_group clamped] (0x22f393-0x22f4d5). Why it is not ported:
//     it keeps a GameObjectPhysics per object in the game state's own map
//     (getGameObjectPhysics 0x205660, which inserts), stamped with the
//     command index and swept every step (0x22f220-0x22f2c3) - per-object
//     state in the live game that a run must not make or write.
//
// (g) Dynamic moves and rotates: processDynamicObjectActions 0x22e280 walks
//     m_dynamicMoveActions (+0x730) or m_dynamicRotateActions (+0x748), 0x60
//     bytes each, and works the step out again from where the target and the
//     centre stand. A move trigger 901 or rotate 1346 with m_isDynamicMode
//     and a target makes one instead of a command (world/fire.cpp). Not
//     ported: the step reads the target's and the centre's live positions
//     through getMoveTargetDelta and the players, and re-targets every tick.
//     The trigger makes the action at 0x22eac0 (its only caller is
//     triggerObject 0x4a7b92), and that picks the objects the same way (b)
//     and (f) do: the key in m_targetGroups (+0xf80) first, and its array out
//     of m_targetGroupsArray (+0xf78) when it finds one (0x22ec30). So a
//     dynamic action's own m_targetGroupID names its objects only on a level
//     that does not name objects rather than groups - which is the one
//     condition targetsObjectsByKey below stands for, shared by (b), (f) and
//     here.
//
// (h) Keyframe animations 3033 with keyframe objects 3032:
//     playKeyframeAnimation 0x217bc0 walks getGroup(m_animationID (+0x684)),
//     and for each object 3032 whose m_keyframeGroup (+0x750) it has not seen
//     yet calls createKeyframeCommand 0x25cd10 with the keyframe array
//     m_keyframeStore (+0x3530) holds for that group, the centre
//     tryGetMainObject(m_centerGroupID), and the target
//     m_targetGroupID ? : the keyframe object's own m_targetGroupID
//     (0x217d4e-0x217d5e). Not ported: a keyframe command carries a vector of
//     0x1c0-byte entries (m_keyframes +0x1b8) built by 0x25cd10 out of every
//     keyframe object's position, rotation and scale and two splines per
//     entry (KeyframeObject::setupSpline 0x2c0e00), and prepareMoveActions'
//     keyframe case 0x25fc0b walks them, evaluates both splines (0x4c6400)
//     and writes back into the entries (the "fired" byte +0x1d at 0x25ff2e).
//     A killer of a group a keyframe animation moves is already not modelled
//     whatever a run does: object 3032 is an effect object that names its
//     target group, so WorldDef::groupModelled says no for it.
//
// (i) The camera: GJBaseGameLayer::updateCamera 0x23bcf0 is what a move
//     command locked to the camera reads (EM +0x808 / +0x80c, world/step.cpp
//     (m)) and what moves the bounds a flying copy is held inside
//     (checkCollisions 0x213c12 takes m_maxGameplayY +0x36a8 - level-wide,
//     updateMaxGameplayY 0x23aec0 sets it once - but getMinPortalY 0x213690
//     and getMaxPortalY 0x213770 divide by the camera zoom
//     m_gameState.m_cameraZoom and add the camera's own y). Not ported: the
//     copies run the game's own checkCollisions, so they take the camera
//     where the real game left it, and a run that models the camera would
//     have to write those fields for every simulated tick and put them back.
//     A move command with a camera lock already marks its group uncertain
//     (world/step.cpp).

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "absense/compat/devlog.hpp"
#include "absense/world/def.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

namespace world {

// The bindings and the fact table have to agree about every field read below;
// where they did not, the constant would win and a raw accessor would take the
// member's place (design step 1).
static_assert(sizeof(EnterEffectInstance) == off::kAreaInstanceSize);
static_assert(offsetof(EnterEffectInstance, m_gameObject) == 0xa0);
static_assert(offsetof(EnterEffectInstance, m_targetID) == 0xac);
static_assert(offsetof(EnterEffectInstance, m_centerID) == 0xb0);
static_assert(offsetof(EnterEffectInstance, m_paused) == 0xbc);
static_assert(offsetof(EnterEffectInstance, m_targetGroupIndex) == 0xc0);
static_assert(offsetof(EnterEffectInstance, m_controlID) == 0xe0);
static_assert(sizeof(AdvancedFollowInstance) == off::kAdvancedFollowSize);
static_assert(offsetof(AdvancedFollowInstance, m_gameObject) == 0x00);
static_assert(offsetof(AdvancedFollowInstance, m_group) == 0x08);
static_assert(offsetof(GJBaseGameLayer, m_areaObjects) == 0xe28);
static_assert(offsetof(GJBaseGameLayer, m_processedAreaObjects) == 0xe40);
static_assert(offsetof(GJBaseGameLayer, m_areaObjectsCount) == 0xee4);
static_assert(offsetof(GJBaseGameLayer, m_processedAreaObjectsCount) == 0xee8);
static_assert(offsetof(GJBaseGameLayer, m_targetGroupsArray) == 0xf78);
static_assert(offsetof(EffectGameObject, m_animationID) == 0x684);
static_assert(offsetof(EffectGameObject, m_targetGroupID) == 0x5c8);

namespace {

// The game's own instructions at the places the table above reads. A run only
// takes the narrow answer while these hold; if the game is built differently
// every area effect makes everything uncertain, which is what it did before
// this file. (A mismatch does not turn the World off: falling back here is
// exactly how the World behaved until now, and turning it off would put every
// run back on MovingObjects, which is worse.)

// processAreaEffects 0x228b45: mov eax, [r15+0xc0] ; test ; jle ; mov rcx, [rdi+0xf78]
const uint8_t kAreaTempGroup[] = {0x41, 0x8b, 0x87, 0xc0, 0x00, 0x00, 0x00, 0x85, 0xc0,
                                  0x7e, 0x1b, 0x48, 0x8b, 0x8f, 0x78, 0x0f, 0x00, 0x00};
// processAreaEffects 0x228b6b: mov eax, [r15+0xac] ; clamp to 0..9999 ; mov rax, [rdi+0xf18]
const uint8_t kAreaTargetGroup[] = {0x41, 0x8b, 0x87, 0xac, 0x00, 0x00, 0x00, 0xb9, 0x0f, 0x27, 0x00, 0x00,
                                    0x3b, 0xc1, 0x0f, 0x4f, 0xc1, 0x85, 0xc0, 0x41, 0x0f, 0x48, 0xc5, 0x8b,
                                    0xd8, 0x48, 0x8b, 0x87, 0x18, 0x0f, 0x00, 0x00, 0x4c, 0x8b, 0x2c, 0xd8};
// addAreaEffect 0x2274bc: the instance's +0xac / +0xb0 against the trigger's
// m_targetGroupID (+0x5c8) and m_centerGroupID (+0x5cc)
const uint8_t kAreaInstanceKey[] = {0x8b, 0x87, 0xc8, 0x05, 0x00, 0x00, 0x39, 0x81, 0xac, 0x00,
                                    0x00, 0x00, 0x75, 0x1c, 0x8b, 0x87, 0xcc, 0x05, 0x00, 0x00,
                                    0x39, 0x81, 0xb0, 0x00, 0x00, 0x00, 0x75, 0x0e};
// processAreaActions 0x229050: lea rsi, [rbx+0xe40] ; cmp [rbx+0xee8], ebp
const uint8_t kAreaProcessedList[] = {0x48, 0x8d, 0xb3, 0x40, 0x0e, 0x00, 0x00, 0x44, 0x8b, 0xfd, 0x39,
                                      0xab, 0xe8, 0x0e, 0x00, 0x00, 0x0f, 0x8e, 0x86, 0x00, 0x00, 0x00};
// processAreaActions 0x2290ec: mov eax, [rbx+0xee4] ; lea rdi, [rbx+0xee8]
const uint8_t kAreaCounts[] = {0x8b, 0x83, 0xe4, 0x0e, 0x00, 0x00, 0x48, 0x8d, 0xbb, 0xe8, 0x0e, 0x00, 0x00};
// processAreaActions 0x229120: mov rax, [rbx+0xe28] ; mov rdx, [r14+rax]
const uint8_t kAreaList[] = {0x48, 0x8b, 0x83, 0x28, 0x0e, 0x00, 0x00, 0x49, 0x8b, 0x14, 0x06};
// processAdvancedFollowAction 0x22f4a6: mov rcx, [r15+0xf78] ; lea edx, [rsi-1]
const uint8_t kFollowTempGroup[] = {0x49, 0x8b, 0x8f, 0x78, 0x0f, 0x00, 0x00, 0x8d, 0x56, 0xff, 0x41, 0x03, 0xd6};
// processAdvancedFollowAction 0x22f4be: mov eax, [rbx+8] ; clamp ; mov rax, [r15+0xf18]
const uint8_t kFollowGroup[] = {0x8b, 0x43, 0x08, 0x3b, 0xc1, 0x0f, 0x4f, 0xc1, 0x85, 0xc0,
                                0x41, 0x0f, 0x48, 0xc4, 0x8b, 0xf0, 0x49, 0x8b, 0x87, 0x18,
                                0x0f, 0x00, 0x00};
// the dynamic action a trigger makes, 0x22ec30: mov rcx, [rbx+0xf78] ; lea edx, [r15+rdi]
const uint8_t kDynamicTempGroup[] = {0x48, 0x8b, 0x8b, 0x78, 0x0f, 0x00, 0x00, 0x41, 0x8d, 0x14, 0x3f};
// playKeyframeAnimation 0x217be8: mov edx, [rdx+0x684] ; call getGroup
const uint8_t kKeyframeGroup[] = {0x8b, 0x92, 0x84, 0x06, 0x00, 0x00, 0xe8, 0x8d, 0xc6, 0x00, 0x00};
// playKeyframeAnimation 0x217c6c: cmp dword [rsi+0x40c], 0xbd8 ; jne ; mov edx, [rsi+0x750]
const uint8_t kKeyframeObject[] = {0x81, 0xbe, 0x0c, 0x04, 0x00, 0x00, 0xd8, 0x0b, 0x00, 0x00, 0x0f,
                                   0x85, 0x6d, 0x01, 0x00, 0x00, 0x8b, 0x96, 0x50, 0x07, 0x00, 0x00};
// playKeyframeAnimation 0x217d4e: mov edx, [rbx+0x5c8] ; test ; jnz ; mov edx, [rsi+0x5c8]
const uint8_t kKeyframeTarget[] = {0x8b, 0x93, 0xc8, 0x05, 0x00, 0x00, 0x85, 0xd2,
                                   0x75, 0x06, 0x8b, 0x96, 0xc8, 0x05, 0x00, 0x00};

struct Site {
    uintptr_t rva;
    const uint8_t* bytes;
    std::size_t size;
    const char* what;
};

#define TIER_SITE(rva, bytes, what) Site{rva, bytes, sizeof(bytes), what}
const Site kSites[] = {
    TIER_SITE(0x228b45, kAreaTempGroup, "processAreaEffects target group index +0xc0"),
    TIER_SITE(0x228b6b, kAreaTargetGroup, "processAreaEffects target group +0xac"),
    TIER_SITE(0x2274bc, kAreaInstanceKey, "addAreaEffect instance target / centre"),
    TIER_SITE(0x229050, kAreaProcessedList, "processAreaActions processed list +0xe40"),
    TIER_SITE(0x2290ec, kAreaCounts, "processAreaActions counts +0xee4 / +0xee8"),
    TIER_SITE(0x229120, kAreaList, "processAreaActions list +0xe28"),
    TIER_SITE(0x22f4a6, kFollowTempGroup, "processAdvancedFollowAction group array +0xf78"),
    TIER_SITE(0x22f4be, kFollowGroup, "processAdvancedFollowAction group +0x08"),
    TIER_SITE(0x22ec30, kDynamicTempGroup, "dynamic action group array +0xf78"),
    TIER_SITE(0x217be8, kKeyframeGroup, "playKeyframeAnimation animation group +0x684"),
    TIER_SITE(0x217c6c, kKeyframeObject, "playKeyframeAnimation keyframe object 3032"),
    TIER_SITE(0x217d4e, kKeyframeTarget, "playKeyframeAnimation target group +0x5c8"),
};
#undef TIER_SITE

// The game's code does not change while it runs: checked once per process, and
// logged the first time either way so a level that answers widely says why.
bool sitesOk() {
    static bool done = false;
    static bool ok = false;
    if (done) return ok;
    done = true;
    const uintptr_t base = geode::base::get();
    const char* failed = nullptr;
    for (const Site& s : kSites) {
        if (std::memcmp(reinterpret_cast<const void*>(base + s.rva), s.bytes, s.size) != 0) {
            failed = s.what;
            break;
        }
    }
    ok = failed == nullptr;
    if (ok) {
        devlog::logf(devlog::Cat::System, "world: tier D/E reach checked, %d sites", (int)std::size(kSites));
    } else {
        geode::log::warn("World: an area effect now makes every object uncertain ({} differs)", failed);
        devlog::logf(devlog::Cat::System, "world: tier D/E reach off - %s", failed);
    }
    return ok;
}

// Whether the level ever targets an object rather than its whole group: the
// game keeps those in m_targetGroupsArray, and an area effect, an advanced
// follow or a dynamic action that uses one moves objects the World cannot
// name. The map of keys (+0xf80) and the arrays they stand for (+0xf78) are
// built together, once, when the level is read (0x224e70 clears both and
// 0x225df1 fills them side by side), so the array standing empty is the same
// answer as the map standing empty, and it is an answer that does not change
// while the level runs.
bool usesTargetGroupKeys(GJBaseGameLayer* pl) {
    if (!pl) return true;
    cocos2d::CCArray* groups = pl->m_targetGroupsArray;
    return groups != nullptr && groups->count() > 0;
}

// Fact (d): the objects the area effects moved and the game has still to put
// back. Worked out once per real tick - a decision makes thousands of runs
// from one tick of the game and every one of them begins with a captureLive -
// and false when the two lists do not read back as the game's code writes
// them, which makes the caller mark everything.
struct TouchedCache {
    const GJBaseGameLayer* pl = nullptr;
    unsigned gen = 0;
    unsigned progress = 0;
    int counts[2] = {-1, -1};
    std::vector<uint16_t> groups;
    bool any = false;
    bool valid = false;
};

bool markAreaTouched(GJBaseGameLayer* pl, const WorldDef& def, WorldState& ws) {
    const int counts[2] = {pl->m_areaObjectsCount, pl->m_processedAreaObjectsCount};
    if (counts[0] <= 0 && counts[1] <= 0) return true;
    static TouchedCache cache;
    const unsigned progress = pl->m_gameState.m_currentProgress;
    if (!(cache.valid && cache.pl == pl && cache.gen == def.gen && cache.progress == progress &&
          cache.counts[0] == counts[0] && cache.counts[1] == counts[1])) {
        cache = TouchedCache{};
        cache.pl = pl;
        cache.gen = def.gen;
        cache.progress = progress;
        cache.counts[0] = counts[0];
        cache.counts[1] = counts[1];
        const gd::vector<GameObject*>* lists[2] = {&pl->m_areaObjects, &pl->m_processedAreaObjects};
        for (int which = 0; which < 2; which++) {
            const gd::vector<GameObject*>& list = *lists[which];
            const int count = counts[which];
            if (count < 0 || (std::size_t)count > list.size()) return false;
            for (int i = 0; i < count; i++) {
                GameObject* o = list[(std::size_t)i];
                const int slot = o ? def.slotOf(o) : -1;
                if (slot < 0) {
                    // An object the level read does not hold: nothing names it.
                    cache.any = true;
                    continue;
                }
                const std::span<const uint16_t> groups = def.groupsOf(slot);
                bool named = false;
                for (const uint16_t g : groups) {
                    if (g == 0) continue;  // group 0 is not a group a trigger can name
                    cache.groups.push_back(g);
                    named = true;
                }
                // Nothing names it, so nothing can mark it: the whole run is
                // uncertain while the game has still to put it back.
                if (!named) cache.any = true;
            }
        }
        std::sort(cache.groups.begin(), cache.groups.end());
        cache.groups.erase(std::unique(cache.groups.begin(), cache.groups.end()), cache.groups.end());
        cache.valid = true;
    }
    if (cache.any) ws.uncertainAny = true;
    ws.uncertainGroups.insert(ws.uncertainGroups.end(), cache.groups.begin(), cache.groups.end());
    return true;
}

}  // namespace

bool checkTierDE() { return sitesOk(); }

bool targetsObjectsByKey(const WorldDef& def) { return !sitesOk() || usesTargetGroupKeys(def.pl); }

bool fireReachesAnyObject(const WorldDef& def, Kind kind) {
    switch (kind) {
        // Fact (e): an enter or exit effect is set on a channel, and any
        // object can be on one. The World does not model where the game
        // applies one, so a fire makes everything uncertain.
        case Kind::EnterMove:
        case Kind::EnterRotate:
        case Kind::EnterScale:
        case Kind::AreaStop:
        // A time warp changes how fast every command and spawn runs.
        case Kind::TimeWarp:
            return true;
        // Facts (a), (b) and (d): an area effect moves the members of the
        // group it names, and an edit of one reaches the objects of the
        // instances it edits - which the state already holds, and whose own
        // groups are marked where they are made. Only a level that targets
        // objects by key can put objects the World cannot name inside one.
        case Kind::AreaMove:
        case Kind::AreaRotate:
        case Kind::AreaScale:
        case Kind::EditAreaMove:
        case Kind::EditAreaRotate:
        case Kind::EditAreaScale:
        // Fact (f): an advanced follow moves the members of the group its
        // trigger names, and the instance works the array out again on every
        // step - so a level with keys can put objects the World cannot name
        // inside one just as an area effect can. The live side of this is in
        // markLiveTierDE; a fire has to say the same, or a follow that starts
        // inside a run would name a group it does not move.
        case Kind::AdvancedFollow:
        case Kind::EditAdvancedFollow:
        case Kind::RetargetAdvancedFollow:
            return targetsObjectsByKey(def);
        default:
            return false;
    }
}

void markKeyframeTargets(const WorldDef& def, WorldState& ws, const TriggerDef& trigger) {
    // Fact (h): the animation's keyframe objects are the 3032s of the group
    // m_animationID names, and each of them animates the trigger's target
    // group, or - when the trigger names none - its own. Both are read raw off
    // the objects: playKeyframeAnimation takes the trigger's m_targetGroupID
    // at 0x217d4e and hands it to createKeyframeCommand, which uses it as it
    // stands (0x25cd7a), so a remap the caller applied names another group
    // than the one the animation moves. The caller marks the remapped one;
    // the raw one is marked here.
    markUncertain(ws, trigger.target);
    if (trigger.slot < 0 || (std::size_t)trigger.slot >= def.slots.size() || !sitesOk()) {
        ws.uncertainAny = true;
        return;
    }
    GameObject* o = def.slots[(std::size_t)trigger.slot];
    if (!o || !(def.flags[(std::size_t)trigger.slot] & kEffect)) {
        ws.uncertainAny = true;
        return;
    }
    const int animation = static_cast<EffectGameObject*>(o)->m_animationID;
    for (const int slot : def.members(animation)) {
        if (slot < 0 || (std::size_t)slot >= def.slots.size()) continue;
        if (!(def.flags[(std::size_t)slot] & kEffect)) continue;
        GameObject* k = def.slots[(std::size_t)slot];
        if (!k || k->m_objectID != 3032) continue;
        markUncertain(ws, static_cast<EffectGameObject*>(k)->m_targetGroupID);
    }
}

void markLiveTierDE(GJBaseGameLayer* pl, const WorldDef* def, WorldState& ws) {
    if (!pl) return;
    GJGameState& gs = pl->m_gameState;
    std::vector<uint16_t>& uncertain = ws.uncertainGroups;
    // captureLive sorts and uniques the list itself once every mark is in.
    const auto mark = [&uncertain](int group) {
        if (group > 0 && group < kGroupLimit) uncertain.push_back((uint16_t)group);
    };
    const bool exact = sitesOk();
    // The one condition the narrow answers of (b), (f) and (g) all rest on: a
    // level that names objects rather than groups moves them out of a
    // temporary group nothing in the level names.
    const bool keyed = !exact || usesTargetGroupKeys(pl);

    // Fact (g): a dynamic move or rotate carries its target group and the
    // centre it works its step out from.
    for (const DynamicObjectAction& a : gs.m_dynamicMoveActions) {
        mark(a.m_targetGroupID);
        mark(a.m_centerGroupID);
        if (keyed) ws.uncertainAny = true;
    }
    for (const DynamicObjectAction& a : gs.m_dynamicRotateActions) {
        mark(a.m_targetGroupID);
        mark(a.m_centerGroupID);
        if (keyed) ws.uncertainAny = true;
    }

    // Fact (f): an advanced follow moves the members of its own group, unless
    // the level targets objects by key - the index it would use is worked out
    // again on every step from a map the World does not read, so a level that
    // has any makes everything uncertain while one runs.
    for (const AdvancedFollowInstance& a : gs.m_advanceFollowInstances) {
        mark(a.m_group);
        // The same group as m_group unless a remap moved the one the fire
        // used; what it follows only ever gets read, so it is not marked.
        if (AdvancedFollowTriggerObject* o = a.m_gameObject) mark(o->m_targetGroupID);
        if (keyed) ws.uncertainAny = true;
    }

    // Facts (a) and (b): a running area effect moves the members of the group
    // its instance names, about the centre it names. A paused one is still
    // marked: what resumes it is not always something the World follows.
    for (const gd::vector<EnterEffectInstance>* instances :
         {&gs.m_moveEffectInstances, &gs.m_rotateEffectInstances, &gs.m_scaleEffectInstances}) {
        for (const EnterEffectInstance& a : *instances) {
            mark(a.m_targetID);
            mark(a.m_centerID);
            // A temporary group (the "target this object" keys): its members
            // are not something the World can name.
            if (!exact || a.m_targetGroupIndex > 0) ws.uncertainAny = true;
        }
    }

    // Fact (d): and the objects an effect that has already gone left moved.
    if (!exact || !def) {
        if (pl->m_areaObjectsCount > 0 || pl->m_processedAreaObjectsCount > 0) ws.uncertainAny = true;
    } else if (!markAreaTouched(pl, *def, ws)) {
        ws.uncertainAny = true;
    }
}

}  // namespace world

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
