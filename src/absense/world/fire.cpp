// Firing triggers (trigger design step 6) and the spawn walk (step 7): what
// EffectGameObject::triggerObject and the functions it calls do, on a run's
// WorldState and OpLog instead of the game. Only the Tier A kinds run here:
// move, toggle, spawn, stop, count / instant count / pickup, speed portals,
// gravity, rotate gameplay and teleport. Every other kind that can change what
// a copy meets marks the groups it names uncertain.
//
// FACTS - GeometryDash.exe 2.2081, read with capstone (see world/step.cpp for
// the step's own table).
//
// (n) EffectGameObject::triggerObject 0x4a5f30(this, layer, int uniqueID,
//     vector<int>* remapKeys) switches on m_objectID: 200/201/202/203/1334
//     (0x4a6896) park 0.7/0.9/1.1/1.3/1.6 in m_timeModRelated and
//     m_hasNoEffects (+0x41c) in m_timeModRelated2 (the shine effect and
//     vtable+0x438, a bare ret in PlayLayer, change nothing a run reads); 901
//     calls triggerMoveCommand 0x21ea40; 1049 calls toggleGroup(target,
//     m_activateGroup) 0x223bc0 - no spawn; 2066 (0x4a6e1c) sets m_gravityMod
//     (+0xb84) of m_player1 unless m_targetPlayer2 and of m_player2 unless
//     m_targetPlayer1, or with m_followCPP only of the player whose uid is the
//     uniqueID argument (1 or 2; the spawn walk passes 0: nobody); 2900 calls
//     the layer's rotateGameplay 0x2181c0 (the channel, then
//     PlayerObject::rotateGameplay on player 1 and, in the dual part, player
//     2); 3022 calls teleportPlayer(layer, this, nullptr), which teleports
//     m_player1. 1268, 1611/1811/1817 and 1616 are the overrides of
//     SpawnTriggerGameObject 0x4b91f0, CountTriggerGameObject 0x4bb210 and
//     TriggerControlGameObject 0x4c3320.
//
// (o) triggerMoveCommand 0x21ea40: returns when (int)trunc(offset.x /
//     max(1, duration)) + 999999 > 1999998 (unsigned), or a player or camera
//     lock is on with |mod| > 99999 on its axis. A dynamic move with a target
//     or direction mode becomes a DynamicObjectAction (not ported). With a
//     target or direction mode the offset is getRealPosition(b) -
//     getRealPosition(a): a = the object of m_targetModCenterID (or the
//     target when that is 0), b = player 1 / player 2 (dual) / the centre
//     group's object; m_enable22Changes picks them with tryGetObject (a random
//     member when the group has no parent and several objects) and
//     tryGetMainObject otherwise (the parent, or the only member). Direction
//     mode normalizes the offset and scales it by +0x60c; target mode 1 drops
//     y and 2 drops x; the locks are cleared. A silent move (+0x611) moves
//     getGroup(target)'s objects at once (OpKind::Place); anything else is
//     createMoveCommand 0x25c700: nothing for a zero offset with no lock or a
//     group <= 0; a fresh command (reset: the game's uid counter) with the
//     target, offset, easing, locks, (double)duration, (double)rate, the mods
//     (0 becomes 1.0), m_lockedInX/Y = player or camera lock, and actions: x
//     when the offset has one and x is not locked in, then y (second action
//     when x took the first).
//
// (p) SpawnTriggerGameObject::triggerObject 0x4b91f0: delay = m_spawnDelay,
//     spread by m_delayRange when that is not 0 - max(0, (r * 2 - 1) * range +
//     delay) with r from an MSVC rand() written inline on the global 0x6c2e90
//     (not fast_rand), unless the layer has +0x309c set and +0x880 is not 1:
//     a state the World cannot follow, so the group's spawn is uncertain; d =
//     (float)((double)delay - m_currentDelay). d <= 0 spawns now with -d
//     through spawnGroup; otherwise (a NaN included: comiss / jb at 0x4b92be)
//     GJEffectManager::spawnGroup 0x261cb0
//     queues it (a group <= 0 is dropped). The keys are the trigger's own
//     m_remapKeys, which updateRemapKeys 0x4b90f0 built from the parent's:
//     with m_remapKey > 0, [key] + (reset ? [] : parent) and then -uid when
//     the parent had none or reset is on; with no key, the parent's unless
//     reset. m_currentDelay goes back to 0.
//
// (q) GJBaseGameLayer::spawnGroup 0x21ab80(group, ordered, delay, keys, uid,
//     control): nothing when the group's toggle bit is off; (group,
//     m_enable22Changes ? uid : 0, -keys.back() or 0) spawns once per step
//     (m_spawnTuples); ordered goes to spawnObjectsInOrder 0x21ae00, anything
//     else calls spawnObject on every member of getGroup(group) in array
//     order. spawnObjectsInOrder walks the spawnable members: the first spawns
//     now with delay 0 and gives x0 = getPosition().x; each later one is
//     d = (float)((double)(x - x0) / 311.58010864257812 - delay) away and
//     spawns now when d <= 0, or is queued (a NaN too, 0x21aef4) as an entry
//     naming the object with that delay, the keys, uid and control.
//
// (r) spawnObject 0x21b030: a non-effect object only animates; a group
//     disabled object, a non-multi-triggered one already activated
//     (hasBeenActivated: either player flag), one not spawn triggered or not
//     spawnable does nothing. Otherwise triggerActivated (both flags when not
//     multi-triggered), and triggerObject(layer, 0, keys) with applyRemap
//     0x21b1c0 put on the object for the call: for each key but the last, in
//     order, the key's map in m_spawnRemapTriggers turns target, centre, item,
//     item 2, control id and animation id (and 901's target-mode centre) into
//     their new groups. A spawn trigger also gets updateRemapKeys(keys) and
//     m_currentDelay = delay first.
//
// (s) toggleGroupTriggered 0x223b50 (the delegate's slot 0): toggleGroup, and
//     when it activates, spawnGroup(group, false, 0, keys, uid, control).
//
// (t) CountTriggerGameObject::triggerObject 0x4bb210: 1611 registers a
//     CountTriggerAction under its item in EM +0x208 (previous count =
//     countForItem now, target = m_pickupCount, the group, activate, uid,
//     control, item, multi activate, the keys). 1811 compares countForItem
//     with m_pickupCount by m_pickupTriggerMode (0 ==, 1 >, 2 <) and calls
//     toggleGroupTriggered when it holds. 1817 by mode: 1 multiplies the count
//     by +0x74c, 2 divides it (nothing when that is 0), each through roundf
//     and a truncation; anything else sets m_pickupCount (override, +0x749) or
//     adds it to m_itemCountMap[clamped item]; then updateCountForItem.
//
// (u) updateCountForItem 0x2624b0: the clamped item's count is set (erased
//     at 0) and a persistent copy too when there is one; its listeners are
//     sorted by target count (descending when the count fell), std::sort being
//     an insertion sort up to 32 of them (0x26ca28), and walked: a paused one,
//     or one whose previous count is the new count, is passed; otherwise the
//     previous count becomes the new one and it fires when the count crossed
//     its target (up from below to at or above, down from above to at or
//     below). A fired listener that is not multi activate is erased first,
//     and the keys the delegate is handed are then the ones of the listener
//     that moved into its place (none when it was the last).
//
// (v) The stop trigger: controlTriggersWithControlID 0x21e720 when
//     m_targetControlID, controlTriggersInGroup 0x21e190 otherwise.
//     controlTriggersInGroup walks getGroup(target) and, for each object with
//     m_canBeControlled, calls controlActionsForTrigger 0x25e3b0 (after the
//     dynamic moves and rotates of a dynamic 901 / 1346, and instead of it for
//     camera, gradient, area, advanced follow, audio and event link kinds).
//     controlActionsForTrigger, by the trigger's uid: move, rotate, follow,
//     transform, advanced follow and keyframe commands get m_finishRelated
//     (stop), m_disabled (pause) or not (resume); spawn entries, count, touch
//     and collision listeners are erased (stop) or paused / resumed.
//     controlActionsForControlID 0x25da50 does the same by control id to all
//     of them at once.
//
// (w) checkSpawnObjects 0x21a8f0: see world/step.cpp (d). The position is
//     taken once before the loop, m_player1's m_isSideways again for every
//     entry (0x21aa47), and whether an entry is group disabled from the run's
//     toggle bits (groupDisabled below). The position is
//     m_player1's getPosition() outside a platformer; in a platformer it is
//     posForTime(m_levelTime), which goes through LevelTools::posForTimeInternal
//     0x32cba0 - a function that writes game globals (0x6ba198, 0x6c2c76/77) a
//     run must not touch, so a platformer run keeps the copies' own walk.
//
// (x) GJBaseGameLayer::activateCustomRing is inlined in PlayerObject::ringJump
//     at 0x398e4b-0x398f96, reached only for a ring whose getType (vtable+0x660)
//     is 0x24 (GameObjectType::CustomRing) and only once the ring has really
//     fired - after the checks, the ring set, m_touchingRings->removeObject and
//     activatedByPlayer. With m_isSpawnOnly (RingObject +0x741) it calls the
//     trigger delegate's slot 1, spawnGroup(m_targetGroupID, false, 0.0, {},
//     m_uniqueID, m_controlID) (0x398eb8); otherwise slot 0,
//     toggleGroupTriggered(m_targetGroupID, m_activateGroup, {}, m_uniqueID,
//     m_controlID) (0x398f3e). Both ids the object setup gives that type
//     (0x191b8b and 0x191c66) are rings: 1594 and 3643. The design called 3643
//     a toggle block fired from the collision pass; in the binary it is a
//     second custom ring and goes through this same path, and no other orb or
//     pad fires anything at all - bumpPlayer 0x2179d0 and playerTouchedRing
//     0x217e40 have no trigger call, so "orbs and pads with a target group"
//     is only ever this.
//
// (y) GJEffectManager::playerButton 0x262190(EM, bool down, bool isPlayer2)
//     walks m_touchToggleActions (EM +0x1f0, entries of 0x38 bytes, the vector
//     re-read every step) and for each entry: a disabled one (+0x00) is
//     passed; a dual-mode one (+0x1c) is passed while isPlayer2; the touch
//     control (+0x10) is 0 for both players, 1 for player 2 only and 2 for
//     player 1 only, and anything else passes; a release (down false) is
//     passed unless the entry holds (+0x08). Then group = +0x04 and activate =
//     the group's toggle bit is clear. Holding: the toggle mode (+0x0c) 0
//     keeps that, 1 makes activate = down and anything else activate = !down.
//     Not holding: mode 0 keeps it, anything else makes activate = (mode == 1).
//     It then calls the trigger delegate's toggleGroupTriggered(group,
//     activate, the entry's keys, +0x14, +0x18); with no delegate it writes the
//     bit itself. GJBaseGameLayer::handleButton 0x2338e0 calls it once per
//     button change, after the players' own handling and only while
//     m_player1->m_isDead is clear (0x233aa3-0x233ac2).
//
// (z) GJBaseGameLayer::playerTouchedTrigger 0x217f50(layer, player, object):
//     object 3640 first gets vtable+0x678(m_commandIndex); an object that is
//     not touch triggered (+0x5d0) returns there. The key is (m_uniqueID,
//     m_isSinglePTouch (+0x6dc) ? 0 : player->m_uniqueID). A multi-triggered
//     object (+0x690) whose key is not in m_gameState.m_activatedObjectIDs
//     calls removeTriggeredID(key) first, and then stamps the key with
//     m_commandIndex either way. The fire itself is once per key: nothing
//     happens while the key is in the effect manager's triggered set (+0x558),
//     otherwise storeTriggeredID(key), updateRemapKeys for a spawn trigger and
//     triggerObject(layer, player->m_uniqueID, nullptr). The port's collision
//     pass has already asked canActivate (once per contact) before it gets
//     here, which is the same gate the contact-map half of this is: what is
//     left is the triggered set.
//
// The Tier C kinds (design step 12c). Every one of them ends in the trigger
// delegate's spawnGroup (slot 1), so what they pick is a group to spawn.
//
// (aa) The randoms. 1912 is a case of the base triggerObject (0x4a7225): r =
//     ((seed >> 16) & 0x7fff) / 32767.0f * 100.0f from the game's own
//     generator, an inlined rand() on the global at 0x6c2e90 (seed = seed *
//     0x343fd + 0x269ec3, kept whole in 64 bits); the group is
//     m_centerGroupID when m_duration - the chance, read at +0x5bc - is below
//     r (a NaN chance goes there too), m_targetGroupID otherwise. 2068 is
//     RandTriggerGameObject::triggerObject 0x4b41e0, which passes every other
//     id to the base: total = the sum of the chances, i = (int)roundf(r /
//     32767.0f * (float)total), and the group is the first chance object whose
//     running sum reaches i (0 when the list is empty or the sum never does).
//     Both spawn with a delay of -0.0, the trigger's own uid and control id.
//     A run cannot follow the game's generator - the real game draws from it
//     for its own spawn delays and particles between the run's ticks - so it
//     carries a copy of the global taken when the run began (WorldState::
//     randomSeed) and marks every group in the pick uncertain: the fire is the
//     same on every branch of the run, and a killer in one of those groups is
//     not trusted. (The design said to draw from ws.seed, the replay system's
//     teleport state; the binary's own generator is a plain global the mod can
//     read, so the run starts from the value the game would draw next.)
//
// (ab) SequenceTriggerGameObject::triggerObject 0x4b4870 keeps its state on
//     the object itself: m_sequenceState's two maps, m_sequenceTimes (+0x758)
//     and m_sequenceIndices (+0x798), under the key -keys.back() when
//     m_uniqueRemap (+0x7ec) is set and the fire carries keys, 0 otherwise.
//     time = the key's entry or -1.0f; elapsed = (float)(m_gameState.
//     m_unkDouble3 - time); with m_minInt (+0x7d8) > 0, a time of 0 or more
//     and m_minInt > elapsed it does nothing. Then, with an index above 0,
//     m_reset (+0x7e4) > 0, a time of 0 or more and elapsed > m_reset:
//     m_resetMode (+0x7e0) 0 starts again at 0, anything else drops
//     floorf(elapsed / m_reset) steps (never below 0). The time becomes the
//     clock, the index goes up one, and past the total m_sequenceMode (+0x7dc)
//     0 stops there, 1 starts again at 1 and 2 stays at the total. The group is
//     the first chance object whose running sum reaches the index. A run keeps
//     its own overlay of the two maps (WorldState::sequences), read from the
//     object the first time it fires one, so nothing writes the level.
//
// (ac) EventLinkTrigger::triggerObject 0x4b88a0 -> activateEventTrigger
//     0x232110: key = m_extraID (+0x754) * 10000 + m_extraID2; for each id in
//     m_eventIDs (+0x740, a set), m_resetRemap (+0x750) erases every listener
//     of m_targetGroupID under (id, key), and otherwise appends one
//     {target, uid, control, false, keys}. gameEventTriggered 0x231ff0(event,
//     material, playerID) fires them: key = material * 10000 + playerID, and
//     unless the event's stamp (+0x598) already holds the command index it
//     stamps it and spawns the group of every listener under that key that is
//     not inactive, with the listener's own keys, uid and control. It then
//     runs itself again with playerID 0, so a listener of "either player"
//     fires too.
//
// (ad) The timers. TimerTriggerGameObject::triggerObject 0x4bf1e0 sends 3614,
//     3615 and 3617 to activateTimerTrigger 0x234e60 and everything else to
//     the base. 3614 starts a timer (startTimer 0x262f50) on m_itemID: a new
//     one takes the start and target times rounded through float, m_active =
//     !m_startPaused and the trigger's group, uid, control and keys; an
//     existing one keeps its own time unless m_dontOverride is off and takes
//     the rest. 3615 appends a TimerTriggerAction under m_itemID {time 0,
//     (float)m_targetTime, group, uid, control, item, m_multiActivate, keys}.
//     3617 sets m_active of the timer of m_itemID (nothing when there is none)
//     from m_controlType: 0 starts it, 1 pauses it.
//     updateTimers 0x263340(EM, dt) walks the timer map in its own order:
//     a paused or disabled timer only serves its listeners; the rest add
//     (double)(dt * m_timeMod) to m_time and, with m_stopTimeEnabled, stop at
//     m_targetTime once the step crosses it - the time becomes the target,
//     m_active goes off and the group is spawned. Then, for each listener of
//     the timer's item in order: it fires when the timer has passed the
//     listener's target time since the listener's own stored time and the
//     timer is running that way (m_timeMod > 0 going up, < 0 going down); a
//     listener that is not multi-activate is erased when it fires, and any
//     other keeps the timer's time. The fire spawns with the timer's keys, uid
//     and control, not the listener's. The layer calls it with the step's dt
//     and only while m_playerDied is clear. The bindings give it a second
//     float (a time warp); the call at 0x238360 sets no such register and the
//     function reads none, so the step's dt is all there is, and
//     m_ignoreTimeWarp is read by nothing in it.
//
// (ae) The item triggers. ItemTriggerGameObject::triggerObject 0x4bfcf0 sends
//     3619 to activateItemEditTrigger 0x234250, 3620 to
//     activateItemCompareTrigger 0x234630 and 3641 to
//     activatePersistentItemTrigger 0x234a40. getItemValue 0x2341c0(mode,
//     item) reads: 1 the item count, 2 the timer's time, 3 the level's points
//     (+0x864), 4 the layer's running time (+0x3560), 5 the attempt count
//     (+0x3084), anything else 0. The edit trigger works out a value from item
//     1 (or item 2 when only that one is in play), the other item and its own
//     mod - roundf(m_mod1 * 1000) / 1000 - through m_resultType2 (the two
//     items), m_resultType3 (the mod, clamped to at least 3: multiply or
//     divide), the rounding and sign of m_roundType1 / m_signType1, then
//     m_resultType1 against the target item's own value and m_roundType2 /
//     m_signType2, and writes it by m_targetItemMode: 1 the item count, 2 the
//     timer, 3 the level's points. The compare trigger does the same to two
//     values and spawns m_targetGroupID when the comparison holds and
//     m_centerGroupID when it does not. The persistent trigger adds the item
//     (or every persistent one with m_targetAll) to the persistent item map or
//     the persistent timer set, or takes it out, and resets it with m_reset.
//
// (af) GJBaseGameLayer::activatePlayerControlTrigger 0x2174e0 works on player
//     1 when m_targetPlayer1 is set, on player 2 when m_targetPlayer2 is, and
//     on both when neither is. m_stopJump (+0x740) releases the jump button
//     (PlayerObject::releaseButton 0x3981d0, inlined); m_stopMove (+0x741)
//     releases left and right, in a platformer level only; m_stopRotation
//     (+0x742) clears the player's +0x720, +0x728 and +0x668; m_stopSlide
//     (+0x743) clears +0x952 and +0xb94. It keeps no state of its own.
//
// (ag) reverseDirection 0x218160 (the layer's vtable +0x4d8, what 1917 calls):
//     each player that is not in a platformer gets doReversePlayer(!
//     m_isGoingLeft), player 2 only in the dual part.
//
// (ah) The time warp. 1935 (0x4a736e) only parks m_timeWarpTimeMod in
//     m_queuedTimeWarp (+0x334). At the end of an update with a queued warp
//     above 0 (0x238bb0) the layer runs updateTimeWarp 0x236150: v =
//     min(warp, 2), then 0.1 when that is below it; m_queuedTimeWarp = 0,
//     m_timeWarp = v, and applyTimeWarp (vtable +0x4f8) when v is exactly 1.
//     applyTimeWarp 0x2361a0 only moves m_timeWarpRelated (+0x338) and the
//     audio pitch. The steps themselves get shorter or longer through the
//     warp, which is where a run's dt comes from (world::stepDt).
//
// (ai) processOptionsTrigger 0x223d50 reads GameOptionsTrigger's settings
//     (0 leaves the option alone, 1 turns it on, anything else off):
//     m_unlinkDualGravity -> the layer's +0x860, which the dual gravity of the
//     copies reads; m_disableP1Controls / m_disableP2Controls ->
//     PlayerObject::disablePlayerControls 0x39f3b0 / enablePlayerControls
//     0x39f500; m_boostSlide -> both players' m_decreaseBoostSlide;
//     m_editRespawnTime with m_respawnTime -> the layer's +0x518 (clamped to
//     1..10). The rest - the streak blend, the ground, middleground, attempt
//     and player visibility, the death audio - changes nothing a run reads.
//
// (aj) EndTriggerGameObject::triggerObject 0x4bccd0 calls the layer's vtable
//     +0x458, PlayLayer::activatePlatformerEndTrigger 0x3ac2d0: nothing while
//     player 1 is dead or the level is already ending, otherwise it spawns
//     m_targetGroupID (when above 0) and the level ends.

#include <Geode/Geode.hpp>
#include <Geode/binding/RingObject.hpp>

#include <algorithm>
#include <cmath>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif
#include <cstdint>

#include "absense/compat/bot.hpp"
#include "absense/physics/gjbasegamelayer.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/world/def.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

namespace world {

namespace {

// Deeper than any chain a level means to build: spawns that fire each other
// with no delay and are multi-triggered would recurse for ever in the game
// too, but the once-per-step spawn set stops a group spawning twice; this only
// keeps a run from blowing the stack on something the model gets wrong.
constexpr int kMaxDepth = 64;

struct Deeper {
    Run& run;
    explicit Deeper(Run& r) : run(r) { run.depth++; }
    ~Deeper() { run.depth--; }
    Deeper(const Deeper&) = delete;
    Deeper& operator=(const Deeper&) = delete;
};

int clampGroup(int g) { return (int)WorldDef::clampGroup(g); }

// applyRemap 0x21b1c0 on one field, fact (r).
int remapped(Run& run, std::span<const int> keys, int value) {
    if (keys.size() <= 1) return value;
    const auto& tables = run.def->remapTables;
    for (std::size_t i = 0; i + 1 < keys.size(); i++) {
        const int key = keys[i];
        if (key < 0 || (std::size_t)key >= tables.size()) {
            // The game indexes the table with no check: nothing it does
            // then is known.
            run.ws->uncertainAny = true;
            continue;
        }
        const auto& table = tables[(std::size_t)key];
        auto it = std::lower_bound(table.begin(), table.end(), value,
                                   [](const std::pair<int, int>& e, int k) { return e.first < k; });
        if (it != table.end() && it->first == value) value = it->second;
    }
    return value;
}

template <class T>
T& lookupInsert(std::vector<std::pair<int, T>>& map, int key) {
    auto it = std::lower_bound(map.begin(), map.end(), key,
                               [](const std::pair<int, T>& e, int k) { return e.first < k; });
    if (it == map.end() || it->first != key) it = map.insert(it, {key, T{}});
    return it->second;
}

bool sortedContains(const std::vector<int>& v, int x) { return std::binary_search(v.begin(), v.end(), x); }

void sortedInsert(std::vector<int>& v, int x) {
    auto it = std::lower_bound(v.begin(), v.end(), x);
    if (it == v.end() || *it != x) v.insert(it, x);
}

// cvttss2si: INT_MIN for a NaN or a value out of range, where a C++ cast is
// undefined.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
int truncFloat(float f) { return _mm_cvtt_ss2si(_mm_set_ss(f)); }
#else
// Off x86: the same answer cvttss2si gives, INT_MIN for NaN or out of range.
int truncFloat(float f) {
    return (f != f || f >= 2147483648.0f || f < -2147483648.0f) ? INT32_MIN : static_cast<int>(f);
}
#endif

GameObject* objectOf(const Run& run, int slot) {
    if (slot < 0 || (std::size_t)slot >= run.def->slots.size()) return nullptr;
    return run.def->slots[(std::size_t)slot];
}

// m_isGroupDisabled (+0x28e) as the run has it. toggleGroup 0x223bc0 does
// nothing when the group's bit already has the value and otherwise moves the
// counter (+0x4c0) of every entry of m_groups[group] by one, the flag being its
// sign; the only other writers in a level, object setup and a checkpoint's
// load (0x3b7e1e, one decrement per entry of every group that is off), keep
// the same count. So an object is disabled exactly while one of the groups it
// is listed in is off, and the run's toggle bits say which are. This read the
// object's counter through ObjectCache, whose base is the live object: a kept
// start in the past or a start carried ahead then counted the toggles the real
// game had made since as well as the run's own (a group switched off twice
// stayed off when the run switched it back on), and the answer changed with the
// real game between the slices of a paused search.
bool groupDisabled(const Run& run, int slot) {
    for (uint16_t g : run.def->groupsOf(slot)) {
        if (!run.ws->groupEnabled(g) && WorldDef::countIn(run.def->members(g), slot) > 0) return true;
    }
    return false;
}

// ------------------------------------------------------------ targets

// tryGetMainObject 0x224520: the group's parent, or its only member.
int mainObject(const Run& run, int group) {
    if (group <= 0) return -1;
    const int parent = run.def->parentOf(group);
    if (parent >= 0) return parent;
    const auto members = run.def->members(group);
    return members.size() == 1 ? members[0] : -1;
}

// ------------------------------------------------------------ commands

void createMoveCommand(Run& run, cocos2d::CCPoint offset, int group, const TriggerDef& d, bool lockPX, bool lockPY,
                       bool lockCX, bool lockCY, int control) {
    // createMoveCommand 0x25c700, fact (o).
    if (offset.x == 0.0f && offset.y == 0.0f && !lockPX && !lockPY && !lockCX && !lockCY) return;
    if (group <= 0) return;
    WorldState& ws = *run.ws;
    WCmd c = freshCommand(ws.nextCommandUid++);
    c.triggerUid = d.uid;
    c.controlId = control;
    c.targetGroup = group;
    c.commandType = 0;
    c.moveOffsetX = offset.x;
    c.moveOffsetY = offset.y;
    c.easingType = d.easingType;
    c.lockPlayerX = lockPX;
    c.lockPlayerY = lockPY;
    c.lockCameraX = lockCX;
    c.lockCameraY = lockCY;
    c.duration = (double)d.duration;
    c.easingRate = (double)d.easingRate;
    const double modX = (double)d.modX;
    const double modY = (double)d.modY;
    c.modX = modX == 0.0 ? 1.0 : modX;
    c.modY = modY == 0.0 ? 1.0 : modY;
    c.lockedInX = lockPX || lockCX;
    c.lockedInY = lockPY || lockCY;
    if (offset.x == 0.0f && offset.y == 0.0f && !c.lockedInX && !c.lockedInY) {
        c.finished = true;
        c.finishRelated = true;
    } else {
        if (offset.x != 0.0f && !c.lockedInX) {
            c.actionType1 = 1;
            c.actionValue1 = (double)offset.x;
        }
        if (offset.y != 0.0f && !c.lockedInY) {
            if (c.actionType1 == 0) {
                c.actionType1 = 2;
                c.actionValue1 = (double)offset.y;
            } else {
                c.actionType2 = 2;
                c.actionValue2 = (double)offset.y;
            }
        }
    }
    ws.cmds.edit().push_back(c);
}

void triggerMoveCommand(Run& run, const TriggerDef& d, std::span<const int> keys) {
    // triggerMoveCommand 0x21ea40, fact (o).
    WorldState& ws = *run.ws;
    const int target = remapped(run, keys, d.target);
    const int center = remapped(run, keys, d.center);
    const int modCenter = remapped(run, keys, d.targetModCenter);
    const int control = remapped(run, keys, d.control);
    cocos2d::CCPoint offset{d.moveX, d.moveY};
    const float span = 1.0f > d.duration ? 1.0f : d.duration;  // maxss
    if ((uint32_t)(truncFloat(offset.x / span) + 999999) > 1999998u) return;
    bool lockPX = d.lockPlayerX, lockPY = d.lockPlayerY, lockCX = d.lockCameraX, lockCY = d.lockCameraY;
    if ((lockPX || lockCX) && std::fabs(d.modX) > 99999.0f) return;
    if ((lockPY || lockCY) && std::fabs(d.modY) > 99999.0f) return;
    if (d.dynamic && (d.useTarget || d.directionMode)) {
        // A DynamicObjectAction: not ported. The action the trigger makes
        // takes its objects out of a temporary group when the level names
        // objects rather than groups, and then the group it names is not what
        // it moves (world/tierd.cpp (g)).
        markUncertain(ws, target);
        if (targetsObjectsByKey(*run.def)) ws.uncertainAny = true;
        return;
    }
    if (d.useTarget || d.directionMode) {
        const int aGroup = modCenter > 0 ? modCenter : target;
        int a = mainObject(run, aGroup);
        if (run.def->enable22Changes && a < 0 && !run.def->members(aGroup).empty()) {
            // tryGetObject's random member (fast_rand): not something a run can follow.
            markUncertain(ws, target);
            return;
        }
        cocos2d::CCPoint pa{}, pb{};
        bool haveA = a >= 0, haveB = false;
        int bUid = 0;
        PlayerObject* player = nullptr;
        if (d.targetP1) {
            player = run.player1;
            haveB = player != nullptr;
            if (!player) markUncertain(ws, target);
        } else if (d.targetP2) {
            player = (run.pl && run.pl->m_gameState.m_isDualMode) ? run.player2 : run.player1;
            haveB = player != nullptr;
            if (!player) markUncertain(ws, target);
        } else {
            int b = mainObject(run, center);
            if (run.def->enable22Changes && b < 0 && !run.def->members(center).empty()) {
                markUncertain(ws, target);
                return;
            }
            if (b >= 0) {
                haveB = true;
                const Pose& pose = run.cache->pose(*run.log, b);
                pb = cocos2d::CCPoint{(float)pose.x, (float)pose.y};  // getRealPosition 0x197b20
                if (GameObject* o = objectOf(run, b)) bUid = o->m_uniqueID;
            }
        }
        if (player) {
            pb = player->getPosition();  // PlayerObject::getRealPosition 0x39dc20
            bUid = player->m_uniqueID;
        }
        int aUid = 0;
        if (haveA) {
            const Pose& pose = run.cache->pose(*run.log, a);
            pa = cocos2d::CCPoint{(float)pose.x, (float)pose.y};
            if (GameObject* o = objectOf(run, a)) aUid = o->m_uniqueID;
        }
        if (haveA && haveB && aUid != bUid) {
            offset = pb - pa;
        } else {
            offset = cocos2d::CCPointZero;
        }
        // libcocos2d's own ccpNormalize (the import the game calls at 0x21ed6a),
        // not the header's inline CCPoint::normalize, so the length and the
        // division are the DLL's.
        if (d.directionMode) offset = cocos2d::ccpNormalize(offset) * d.directionDistance;
        lockPX = lockPY = lockCX = lockCY = false;
        if (d.useTarget) {
            if (d.targetMode == 1) offset.y = 0.0f;
            else if (d.targetMode == 2) offset.x = 0.0f;
        }
    }
    if (d.silent) {
        if (run.log && !run.def->members(clampGroup(target)).empty()) {
            Op op;
            op.tick = (uint32_t)ws.tick;
            op.commandIndex = ws.commandIndex;
            op.kind = OpKind::Place;
            op.group = clampGroup(target);
            op.dx = offset.x;
            op.dy = offset.y;
            emitOp(run, op);
        }
        return;
    }
    createMoveCommand(run, offset, target, d, lockPX, lockPY, lockCX, lockCY, control);
}

// ------------------------------------------------------------ spawns

// SpawnTriggerGameObject::updateRemapKeys 0x4b90f0, fact (p): the keys a spawn
// trigger fires with, built from the keys its own fire carries.
std::vector<int> spawnKeys(const Run& run, const TriggerDef& d, std::span<const int> parent) {
    std::vector<int> out;
    if (d.remapKey > 0) {
        out.push_back(d.remapKey);
        if (!d.resetRemap) out.insert(out.end(), parent.begin(), parent.end());
        if (!parent.empty() && !d.resetRemap) return out;
        out.push_back(-d.uid);
        return out;
    }
    if (!d.resetRemap) return std::vector<int>(parent.begin(), parent.end());
    // No remap of its own and reset on: the list is left as it was, and no
    // path the game takes ever sets it then - the object's own list is it.
    if (auto* s = geode::cast::typeinfo_cast<SpawnTriggerGameObject*>(objectOf(run, d.slot))) {
        return std::vector<int>(s->m_remapKeys.begin(), s->m_remapKeys.end());
    }
    return out;
}

void fireSpawn(Run& run, const TriggerDef& d, std::span<const int> parentKeys, double currentDelay) {
    // SpawnTriggerGameObject::triggerObject 0x4b91f0, fact (p).
    WorldState& ws = *run.ws;
    const int target = remapped(run, parentKeys, d.target);
    const int control = remapped(run, parentKeys, d.control);
    const std::vector<int> keys = spawnKeys(run, d, parentKeys);
    float delay = d.spawnDelay;
    if (d.spawnVariance != 0.0f) {
        // The spread comes from a global random state of the game's (0x6c2e90):
        // when the triggers of the group fire is not known, nor what they do.
        markSpawnUncertain(*run.def, ws, target);
    }
    const float wait = (float)((double)delay - currentDelay);
    // comiss 0, d / jb queue (0x4b92be): a NaN delay is queued like a positive one.
    if (wait <= 0.0f) {
        spawnGroup(run, target, d.spawnOrdered, (double)(-wait), keys, d.uid, control);
        return;
    }
    // GJEffectManager::spawnGroup 0x261cb0.
    if (target <= 0) return;
    WSpawn s;
    s.duration = (double)wait;
    s.targetGroup = target;
    s.ordered = d.spawnOrdered;
    s.triggerUid = d.uid;
    s.controlId = control;
    s.remap = pushKeys(ws, keys);
    ws.spawns.edit().push_back(s);
}

void spawnObjectsInOrder(Run& run, int group, double delay, std::span<const int> keys, int uid, int control) {
    // spawnObjectsInOrder 0x21ae00, fact (q).
    WorldState& ws = *run.ws;
    bool first = true;
    float x0 = 0.0f;
    for (int slot : run.def->spawnTriggeredIn(group)) {
        GameObject* o = objectOf(run, slot);
        if (!o) continue;
        // getPosition() is the node's. A trigger the run's own ops moved is
        // where they left it; any other stands where the game has it.
        float x = o->getPositionX();
        const Pose& pose = run.cache->pose(*run.log, slot);
        const Pose& base = run.cache->base(slot);
        if (pose.x != base.x) {
            x = (float)pose.x;
            markUncertain(ws, group);
        }
        if (first) {
            first = false;
            x0 = x;
            spawnObject(run, slot, 0.0, keys);
            continue;
        }
        const float wait = (float)((double)(x - x0) / 311.58010864257812 - delay);
        // comiss 0, d / jb queue (0x21aef4): a NaN is queued, as a later one is.
        if (wait <= 0.0f) {
            spawnObject(run, slot, 0.0, keys);
            continue;
        }
        WSpawn s;
        s.duration = (double)wait;
        s.triggerUid = uid;
        s.controlId = control;
        s.objectSlot = slot;
        s.objectUid = o->m_uniqueID;
        s.remap = pushKeys(ws, keys);
        ws.spawns.edit().push_back(s);
    }
}

// ------------------------------------------------------------ control

void controlCommand(WCmd& c, int action) {
    if (action == 0) c.finishRelated = true;
    else if (action == 1) c.disabled = true;
    else if (action == 2) c.disabled = false;
}

// Erase on stop, pause or resume otherwise, for every entry `match` picks.
template <class T, class Match>
void controlList(Cow<std::vector<T>>& list, int action, Match match) {
    if (std::none_of(list->begin(), list->end(), match)) return;
    std::vector<T>& v = list.edit();
    if (action == 0) {
        std::erase_if(v, match);
        return;
    }
    for (T& e : v) {
        if (!match(e)) continue;
        if (action == 1) e.disabled = true;
        else if (action == 2) e.disabled = false;
    }
}

bool isCommandKind(int id) {
    // controlActionsForTrigger 0x25e787's ids: move, rotate, follow, follow
    // player Y (0x716 at 0x25e77e - it was missing here), transform, advanced
    // follow, keyframe animation.
    return id == 901 || id == 1346 || id == 1347 || id == 1814 || id == 2067 || id == 3016 || id == 3033;
}

void controlActionsForTrigger(Run& run, GameObject* o, int action) {
    // controlActionsForTrigger 0x25e3b0, fact (v).
    WorldState& ws = *run.ws;
    const int uid = o->m_uniqueID;
    if (uid <= 0) return;
    const int id = o->m_objectID;
    if (isCommandKind(id)) {
        if (std::any_of(ws.cmds->begin(), ws.cmds->end(), [uid](const WCmd& c) { return c.triggerUid == uid; })) {
            for (WCmd& c : ws.cmds.edit()) {
                if (c.triggerUid == uid) controlCommand(c, action);
            }
        }
        return;
    }
    switch (id) {
        case 1268:
            controlList(ws.spawns, action, [uid](const WSpawn& s) { return s.triggerUid == uid; });
            break;
        case 1611:
            controlList(ws.countListeners, action, [uid](const WCountListener& l) { return l.triggerUid == uid; });
            break;
        case 1595:
            controlList(ws.touchListeners, action, [uid](const WTouchListener& l) { return l.triggerUid == uid; });
            break;
        case 1815:
            controlList(ws.collisionListeners, action, [uid](const WCollisionListener& l) { return l.triggerUid == uid; });
            break;
        case 3614:
            // 0x25ee30: the timers the trigger started.
            controlList(ws.timers, action, [uid](const WTimer& t) { return t.triggerUid == uid; });
            break;
        case 3615:
            // 0x25ecd8: the triggers waiting on a timer.
            controlList(ws.timerListeners, action, [uid](const WTimerListener& l) { return l.triggerUid == uid; });
            break;
        default:
            // Colour, pulse and alpha actions change nothing physical; the
            // on-death kinds change nothing a run reads.
            break;
    }
}

void controlTriggersInGroup(Run& run, int group, int action) {
    // controlTriggersInGroup 0x21e190, fact (v).
    for (int slot : run.def->members(group)) {
        if (!(run.def->flags[(std::size_t)slot] & kControllable)) continue;
        GameObject* o = objectOf(run, slot);
        if (!o) continue;
        const int id = o->m_objectID;
        if ((id == 901 || id == 1346) && static_cast<EffectGameObject*>(o)->m_isDynamicMode) {
            // Its DynamicObjectActions (not ported, their groups uncertain -
            // and the whole run where the level names objects rather than
            // groups, as at the fire above).
            markUncertain(*run.ws, static_cast<EffectGameObject*>(o)->m_targetGroupID);
            if (targetsObjectsByKey(*run.def)) run.ws->uncertainAny = true;
        }
        if (id == 1615 || id == 1913 || id == 1916 || id == 2015 || id == 2903 || id == 3602 || id == 3604 ||
            (id >= 2904 && id <= 3016)) {
            // Labels, camera, gradients, audio, event links, area effects and
            // advanced follows: handled by the layer, nothing the World runs.
            continue;
        }
        controlActionsForTrigger(run, o, action);
    }
}

void controlTriggersWithControlId(Run& run, int control, int action) {
    // controlTriggersWithControlID 0x21e720 -> controlActionsForControlID 0x25da50.
    WorldState& ws = *run.ws;
    if (std::any_of(ws.cmds->begin(), ws.cmds->end(), [control](const WCmd& c) { return c.controlId == control; })) {
        for (WCmd& c : ws.cmds.edit()) {
            if (c.controlId == control) controlCommand(c, action);
        }
    }
    controlList(ws.countListeners, action, [control](const WCountListener& l) { return l.controlId == control; });
    controlList(ws.touchListeners, action, [control](const WTouchListener& l) { return l.controlId == control; });
    controlList(ws.collisionListeners, action, [control](const WCollisionListener& l) { return l.controlId == control; });
    controlList(ws.spawns, action, [control](const WSpawn& s) { return s.controlId == control; });
    // The timers and the triggers waiting on them, which this path reaches as
    // well (0x25e1f4 and 0x25e248).
    controlList(ws.timers, action, [control](const WTimer& t) { return t.controlId == control; });
    controlList(ws.timerListeners, action, [control](const WTimerListener& l) { return l.controlId == control; });
}

// ------------------------------------------------------------ items

void toggleGroupTriggered(Run& run, int group, bool activate, std::span<const int> keys, int uid, int control) {
    // toggleGroupTriggered 0x223b50, fact (s).
    toggleGroup(run, group, activate);
    if (activate) spawnGroup(run, group, false, 0.0, keys, uid, control);
}

void fireCount(Run& run, const TriggerDef& d, std::span<const int> keys) {
    // CountTriggerGameObject::triggerObject 0x4bb210, fact (t).
    WorldState& ws = *run.ws;
    const int item = remapped(run, keys, d.itemId);
    const int target = remapped(run, keys, d.target);
    const int control = remapped(run, keys, d.control);
    switch (d.kind) {
        case Kind::Count: {
            WCountListener w;
            w.item = item;
            w.previousCount = countForItem(ws, item);
            w.targetCount = d.count;
            w.targetGroup = target;
            w.activateGroup = d.activateGroup;
            w.triggerUid = d.uid;
            w.controlId = control;
            w.itemId = item;
            w.multiActivate = d.countMulti;
            w.remap = pushKeys(ws, keys);
            std::vector<WCountListener>& list = ws.countListeners.edit();
            // Appended to its item's list, which the World keeps as one run
            // of a vector sorted by item.
            auto at = std::upper_bound(list.begin(), list.end(), item,
                                       [](int k, const WCountListener& l) { return k < l.item; });
            list.insert(at, w);
            break;
        }
        case Kind::InstantCount: {
            const int now = countForItem(ws, item);
            bool hit = false;
            if (d.mode == 0) hit = now == d.count;
            else if (d.mode == 1) hit = now > d.count;
            else if (d.mode == 2) hit = now < d.count;
            if (hit) toggleGroupTriggered(run, target, d.activateGroup, keys, d.uid, control);
            break;
        }
        case Kind::Pickup: {
            if (d.mode == 1) {
                const float v = (float)countForItem(ws, item) * d.countMultiplier;
                updateCountForItem(run, item, truncFloat(std::roundf(v)));
            } else if (d.mode == 2) {
                if (!(d.countMultiplier == 0.0f)) {
                    const float v = (float)countForItem(ws, item) / d.countMultiplier;
                    updateCountForItem(run, item, truncFloat(std::roundf(v)));
                }
            } else if (d.countOverride) {
                updateCountForItem(run, item, d.count);
            } else {
                const int key = clampGroup(item);
                updateCountForItem(run, key, countForItem(ws, key) + d.count);
            }
            break;
        }
        default:
            break;
    }
}

// The touch trigger 1595, fact (y): its own case of triggerObject
// (0x4a6968-0x4a6a8e) only appends a TouchToggleAction, field for field, with
// the keys the fire carries. What it does happens on the next button change.
void fireTouch(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    WTouchListener w;
    w.targetGroup = remapped(run, keys, d.target);
    w.holdMode = d.touchHold;
    w.touchType = d.touchToggle;
    w.touchControl = d.touchPlayer;
    w.triggerUid = d.uid;
    w.controlId = remapped(run, keys, d.control);
    w.dualMode = d.dualMode;
    w.remap = pushKeys(ws, keys);
    ws.touchListeners.edit().push_back(w);
}

// ------------------------------------------------------------ the players' part

PlayerObject* copyForUid(Run& run, int uid) {
    // The uid the game compares with (1 for player 1, 2 for player 2).
    if (uid == 1) return run.player1;
    if (uid == 2) return run.player2;
    return nullptr;
}

void fireRotateGameplay(Run& run, const TriggerDef& d) {
    // GJBaseGameLayer::rotateGameplay 0x2181c0 (the camera parts left out).
    WorldState& ws = *run.ws;
    if (d.changeChannel) {
        switchChannel(ws, d.targetChannel, (unsigned)(d.groundDirection - 2) <= 1u);
        // The collision pass's channel filter reads the layer's.
        if (run.pl) run.pl->m_gameState.m_currentChannel = ws.channel;
    }
    if (d.channelOnly) return;
    auto rotate = [&](PlayerObject* p) {
        if (!p) return;
        p->rotateGameplay(d.moveDirection, d.groundDirection, d.editVelocity, d.velocityModX, d.velocityModY,
                          d.overrideVelocity, d.dontSlide);
    };
    rotate(run.player1);
    if (run.pl && run.pl->m_gameState.m_isDualMode) rotate(run.player2);
}

// ------------------------------------------------------------ the randoms

// The game's own generator, fact (aa), on the run's copy of it. The product
// and the sum are taken in 64 bits, as the binary's imul / add on the global
// are, and the value is the 15 bits above the low word.
float nextTriggerRandom(WorldState& ws) {
    ws.randomSeed = ws.randomSeed * 0x343fdull + 0x269ec3ull;
    const int value = (int)((ws.randomSeed >> 16) & 0x7fff);
    return (float)value / 32767.0f;
}

// Every group a random pick could have named: the run took one of them from a
// generator the real game does not share, so none of them is followed.
void markRandomUncertain(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    if (d.kind == Kind::Random) {
        markSpawnUncertain(*run.def, ws, remapped(run, keys, d.target));
        markSpawnUncertain(*run.def, ws, remapped(run, keys, d.center));
        return;
    }
    for (int i = 0; i < d.chanceCount; i++) {
        markSpawnUncertain(*run.def, ws, run.def->chances[(std::size_t)(d.chanceBegin + i)].group);
    }
}

void fireRandom(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    int group = 0;
    if (d.kind == Kind::AdvancedRandom) {
        // RandTriggerGameObject::triggerObject 0x4b41e0.
        int total = 0;
        for (int i = 0; i < d.chanceCount; i++) total += run.def->chances[(std::size_t)(d.chanceBegin + i)].chance;
        const float pick = nextTriggerRandom(ws) * (float)total;
        const int target = truncFloat(std::roundf(pick));
        int sum = 0;
        for (int i = 0; i < d.chanceCount; i++) {
            const DefChance& c = run.def->chances[(std::size_t)(d.chanceBegin + i)];
            sum += c.chance;
            if (sum >= target) {
                group = c.group;
                break;
            }
        }
    } else {
        // The base's 1912 case 0x4a7225: the chance sits in m_duration, and
        // the comparison is a comiss / jb - a NaN chance takes the centre
        // group with everything else below the roll.
        const float roll = nextTriggerRandom(ws) * 100.0f;
        group = !(d.duration >= roll) ? remapped(run, keys, d.center) : remapped(run, keys, d.target);
    }
    markRandomUncertain(run, d, keys);
    spawnGroup(run, group, false, -0.0, keys, d.uid, remapped(run, keys, d.control));
}

// ------------------------------------------------------------ the sequence

// Where the run has the sequence trigger `d` for `key`, fact (ab). The run's
// state holds every key both of the object's maps had when it was imported
// (World::captureLive), so a key that is not there is one neither map held: a
// time of -1 (what the times map answers for a key it does not hold) and the
// index 0 the indices map's operator[] would put there.
WSequence& sequenceState(Run& run, const TriggerDef& d, int key) {
    WorldState& ws = *run.ws;
    std::vector<WSequence>& list = ws.sequences.edit();
    const std::pair<int, int> want{d.slot, key};
    auto it = std::lower_bound(list.begin(), list.end(), want, [](const WSequence& e, const std::pair<int, int>& k) {
        return std::pair<int, int>{e.slot, e.key} < k;
    });
    if (it != list.end() && it->slot == d.slot && it->key == key) return *it;
    WSequence w;
    w.slot = d.slot;
    w.key = key;
    w.time = -1.0f;
    w.index = 0;
    return *list.insert(it, w);
}

void fireSequence(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    auto* o = geode::cast::typeinfo_cast<SequenceTriggerGameObject*>(objectOf(run, d.slot));
    // A sequence is worked out from the level's clock (m_unkDouble3). Without
    // the class the settings live on, or without a layer to read the clock
    // from - the ledger replaying a keyframe has neither - what it would spawn
    // is unknown, and its groups say so.
    if (!o || !run.pl) {
        markFireUncertain(*run.def, ws, d, remapped(run, keys, d.target), remapped(run, keys, d.center));
        for (int i = 0; i < d.chanceCount; i++) {
            markSpawnUncertain(*run.def, ws, run.def->chances[(std::size_t)(d.chanceBegin + i)].group);
        }
        return;
    }
    // 0x4b48b0: the key is the last remap key negated, and only with unique
    // remap on.
    const int key = (o->m_uniqueRemap && !keys.empty()) ? -keys.back() : 0;
    const double clock = run.pl->m_gameState.m_unkDouble3;
    float time;
    int index;
    {
        const WSequence& state = sequenceState(run, d, key);
        time = state.time;
        index = state.index;
    }
    const float elapsed = (float)(clock - (double)time);
    const float minInterval = o->m_minInt;
    if (minInterval > 0.0f && time >= 0.0f && minInterval > elapsed) return;
    if (index > 0) {
        const float reset = o->m_reset;
        if (reset > 0.0f && time >= 0.0f && elapsed > reset) {
            if (o->m_resetMode == 0) {
                index = 0;
            } else {
                // maxss with 0 as the destination: a NaN gives the value,
                // not the zero std::max would.
                const float left = (float)index - std::floorf(elapsed / reset);
                index = truncFloat(0.0f > left ? 0.0f : left);
            }
        }
    }
    int total = 0;
    for (int i = 0; i < d.chanceCount; i++) total += run.def->chances[(std::size_t)(d.chanceBegin + i)].chance;
    index++;
    if (index > total) {
        // 0x4b4a7e: mode 0 stops there and does not even write the index back.
        if (o->m_sequenceMode == 0) {
            WSequence& state = sequenceState(run, d, key);
            state.time = (float)clock;
            return;
        }
        index = o->m_sequenceMode == 1 ? 1 : (o->m_sequenceMode == 2 ? total : index);
    }
    int group = 0;
    int sum = 0;
    for (int i = 0; i < d.chanceCount; i++) {
        const DefChance& c = run.def->chances[(std::size_t)(d.chanceBegin + i)];
        sum += c.chance;
        if (sum >= index) {
            group = c.group;
            break;
        }
    }
    {
        WSequence& state = sequenceState(run, d, key);
        state.time = (float)clock;
        state.index = index;
    }
    spawnGroup(run, group, false, -0.0, keys, d.uid, remapped(run, keys, d.control));
}

// ------------------------------------------------------------ the event trigger

// activateEventTrigger 0x232110, fact (ac).
void fireEvent(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    const int key = d.extraId * 10000 + d.extraId2;
    const int target = remapped(run, keys, d.target);
    const int control = remapped(run, keys, d.control);
    for (int i = 0; i < d.listCount; i++) {
        const int event = run.def->lists[(std::size_t)(d.listBegin + i)];
        std::vector<WEventListener>& list = ws.eventListeners.edit();
        if (d.resetRemap) {
            std::erase_if(list, [event, key, target](const WEventListener& l) {
                return l.event == event && l.key == key && l.targetGroup == target;
            });
            continue;
        }
        WEventListener w;
        w.event = event;
        w.key = key;
        w.targetGroup = target;
        w.triggerUid = d.uid;
        w.controlId = control;
        w.inactive = false;
        w.remap = pushKeys(ws, keys);
        // The map is kept in (event, key) order, each pair's own list in the
        // order the listeners were added.
        auto at = std::upper_bound(list.begin(), list.end(), std::pair<int, int>{event, key},
                                   [](const std::pair<int, int>& k, const WEventListener& l) {
                                       return k < std::pair<int, int>{l.event, l.key};
                                   });
        list.insert(at, w);
    }
}

// ------------------------------------------------------------ timers and items

// GJEffectManager::timerExists 0x263110 looks the item up as it is; the game
// only clamps it in updateTimer and timeForItem (0x2631bd, 0x263942), so a
// timer of an item above 9999 exists and still reads as 0.
bool timerExists(const WorldState& ws, int item) {
    return std::any_of(ws.timers->begin(), ws.timers->end(), [item](const WTimer& t) { return t.key == item; });
}

double timeForItem(const WorldState& ws, int item) {
    const int key = clampGroup(item);
    for (const WTimer& t : *ws.timers) {
        if (t.key == key) return t.time;
    }
    return 0.0;
}

// GJEffectManager::startTimer 0x262f50, fact (ad).
void startTimer(Run& run, int item, double startTime, double targetTime, bool stopTimeEnabled, bool active,
                bool dontOverride, float timeMod, bool ignoreTimeWarp, int targetGroup, std::span<const int> keys,
                int uid, int control) {
    WorldState& ws = *run.ws;
    // The item is the map's key as it stands: startTimer does not clamp it.
    const int key = item;
    {
        const std::vector<WTimer>& timers = *ws.timers;
        auto it = std::find_if(timers.begin(), timers.end(), [key](const WTimer& t) { return t.key == key; });
        if (it != timers.end()) {
            // Where it is, not an iterator to it: edit() hands back a fresh
            // copy of the list while a snapshot still shares it, and an
            // iterator into the one it left behind writes the snapshot's timer
            // rather than the run's - the run would then step a timer it never
            // started, and a branch put back would bring one back changed.
            const std::size_t at = (std::size_t)(it - timers.begin());
            WTimer& w = ws.timers.edit()[at];
            w.active = active;
            w.timeMod = timeMod;
            w.ignoreTimeWarp = ignoreTimeWarp;
            w.targetTime = targetTime;
            w.stopTimeEnabled = stopTimeEnabled;
            w.targetGroup = targetGroup;
            w.triggerUid = uid;
            w.controlId = control;
            w.disabled = false;
            // 0x2630e2: the entry keeps the time it has unless the trigger says
            // to override it. Its item id and keys are left alone as well.
            if (!dontOverride) w.time = startTime;
            return;
        }
    }
    const Remap remap = pushKeys(ws, keys);
    std::vector<WTimer>& timers = ws.timers.edit();
    WTimer w;
    w.key = key;
    w.itemId = key;
    // 0x263016 and 0x26303d: a new entry takes both times through a float.
    w.time = (double)(float)startTime;
    w.active = active;
    w.timeMod = timeMod;
    w.ignoreTimeWarp = ignoreTimeWarp;
    w.targetTime = (double)(float)targetTime;
    w.stopTimeEnabled = stopTimeEnabled;
    w.targetGroup = targetGroup;
    w.triggerUid = uid;
    w.controlId = control;
    w.remap = remap;
    w.disabled = false;
    // The game's map puts a key whose bucket is empty at the head of its list,
    // which is the order updateTimers walks. Timers of one level rarely share
    // a bucket, so the head is where a new one goes.
    timers.insert(timers.begin(), w);
}

// GJEffectManager::updateTimer 0x2631b0: the value clamped as minsd / maxsd
// leave it (a NaN passes through both).
void updateTimerValue(Run& run, int item, double value) {
    WorldState& ws = *run.ws;
    // 0x2631bd: this one clamps the item before it looks it up.
    const int key = clampGroup(item);
    double v = (-9999999.0 > value) ? -9999999.0 : value;
    v = (9999999.0 < v) ? 9999999.0 : v;
    std::vector<WTimer>& timers = ws.timers.edit();
    auto it = std::find_if(timers.begin(), timers.end(), [key](const WTimer& t) { return t.key == key; });
    if (it != timers.end()) {
        it->time = v;
        return;
    }
    startTimer(run, key, v, 0.0, false, false, true, 1.0f, false, 0, {}, 0, 0);
}

// TimerTriggerGameObject::triggerObject 0x4bf1e0 -> activateTimerTrigger
// 0x234e60, fact (ad).
void fireTimer(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    const int item = remapped(run, keys, d.itemId);
    const int target = remapped(run, keys, d.target);
    const int control = remapped(run, keys, d.control);
    switch (d.kind) {
        case Kind::TimeTrigger:
            startTimer(run, item, d.startTime, d.targetTime, d.stopTimeEnabled, !d.startPaused, d.dontOverride,
                       d.timerMod, d.ignoreTimeWarp, target, keys, d.uid, control);
            return;
        case Kind::TimeEvent: {
            WTimerListener w;
            w.item = item;
            w.time = 0.0f;
            w.targetTime = (float)d.targetTime;
            w.targetGroup = target;
            w.triggerUid = d.uid;
            w.controlId = control;
            w.itemId = item;
            w.multiActivate = d.timerMulti;
            w.remap = pushKeys(ws, keys);
            std::vector<WTimerListener>& list = ws.timerListeners.edit();
            auto at = std::upper_bound(list.begin(), list.end(), w.item,
                                       [](int k, const WTimerListener& l) { return k < l.item; });
            list.insert(at, w);
            return;
        }
        case Kind::TimeControl: {
            // 0x234ea8: control type 0 starts the timer of the item, 1 pauses
            // it; a timer that is not there is left alone. The item is looked
            // up as it stands, like startTimer's.
            if (d.controlType != 0 && d.controlType != 1) return;
            const int key = item;
            const auto& timers = *ws.timers;
            auto it = std::find_if(timers.begin(), timers.end(), [key](const WTimer& t) { return t.key == key; });
            if (it == timers.end()) return;
            ws.timers.edit()[(std::size_t)(it - timers.begin())].active = d.controlType == 0;
            return;
        }
        default:
            return;
    }
}

// GJBaseGameLayer::getItemValue 0x2341c0, fact (ae). The level's points, its
// running time and its attempt count belong to the real game: a run reads them
// where they stand and never writes them (an item edit with target mode 3 is
// the one thing it leaves out, see fireItemEdit).
double getItemValue(Run& run, int mode, int item) {
    const WorldState& ws = *run.ws;
    switch (mode) {
        case 1: return (double)countForItem(ws, item);
        case 2: return timeForItem(ws, item);
        case 3: return (double)ws.points;
        case 4: return ws.levelTime;
        // The attempt count never changes inside a run: read where it stands.
        case 5: return run.pl ? (double)off::at<int>(run.pl, off::kAttemptCount) : 0.0;
        default: return 0.0;
    }
}

// cvttsd2si, as the item triggers take their result to an int.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
int truncDouble(double d) { return _mm_cvttsd_si32(_mm_set_sd(d)); }
#else
// Off x86: the same answer cvttsd2si gives, INT_MIN for NaN or out of range.
int truncDouble(double d) {
    return (d != d || d >= 2147483648.0 || d < -2147483648.0) ? INT32_MIN : static_cast<int>(d);
}
#endif

// roundf(x * 1000) / 1000: the mods and the tolerance of an item trigger
// (0x234288, 0x23471b).
float itemMod(float value) { return std::roundf(value * 1000.0f) / 1000.0f; }

// The arithmetic both item triggers share: `op` on a and b (0 leaves a alone).
double itemCombine(int op, double a, double b) {
    switch (op) {
        case 1: return a + b;
        case 2: return a - b;
        case 3: return a * b;
        case 4: return b == 0.0 ? 0.0 : a / b;
        default: return 0.0;
    }
}

double itemRound(int type, double v) {
    switch (type) {
        case 1: return std::round(v);
        case 2: return std::floor(v);
        case 3: return std::ceil(v);
        default: return v;
    }
}

double itemSign(int type, double v) {
    switch (type) {
        case 1: return std::fabs(v);
        case 2: return -std::fabs(v);
        default: return v;
    }
}

// Whether an item takes part at all (0x234343): a mode of 0 never does, and
// otherwise only an item above 0 or a mode of 3 or 4 (the level's own values).
bool itemInPlay(int mode, int item) {
    return mode != 0 && (item > 0 || (unsigned)(mode - 3) <= 1u);
}

// GJBaseGameLayer::activateItemEditTrigger 0x234250, fact (ae).
void fireItemEdit(Run& run, const TriggerDef& d, std::span<const int> keys) {
    const int item1 = remapped(run, keys, d.itemId);
    const int item2 = remapped(run, keys, d.itemId2);
    const int targetItem = remapped(run, keys, d.target);
    // 0x2342dc-0x23431a: each of these is clamped before anything else.
    const int op2 = d.resultType2 <= 0 ? 1 : d.resultType2;
    const int op3 = d.resultType3 < 3 ? 3 : d.resultType3;
    const int modeT = d.targetItemMode <= 0 ? 1 : d.targetItemMode;
    const float mod = itemMod(d.mod1);
    if (!(targetItem > 0 || modeT == 3)) return;
    bool haveFirst = itemInPlay(d.item1Mode, item1);
    bool haveSecond = itemInPlay(d.item2Mode, item2);
    int modeA = d.item1Mode;
    int itemA = item1;
    if (!haveFirst && haveSecond) {
        // 0x234394: with only the second item in play it becomes the first.
        modeA = d.item2Mode;
        itemA = item2;
        haveFirst = true;
        haveSecond = false;
    }
    double a = getItemValue(run, modeA, itemA);
    const double b = getItemValue(run, d.item2Mode, item2);
    const double c = getItemValue(run, modeT, targetItem);
    if (haveSecond) a = itemCombine(op2, a, b);
    a = haveFirst ? itemCombine(op3, a, (double)mod) : (double)mod;
    a = itemRound(d.roundType1, a);
    a = itemSign(d.signType1, a);
    // 0x2344ed: the result against what the target item holds now. Modes 2 and
    // 4 keep the target's value on the left.
    switch (d.resultType1) {
        case 0: break;
        case 1: a = a + c; break;
        case 2: a = c - a; break;
        case 3: a = a * c; break;
        case 4: a = a == 0.0 ? 0.0 : c / a; break;
        default: a = 0.0; break;
    }
    a = itemRound(d.roundType2, a);
    a = itemSign(d.signType2, a);
    if (modeT == 1) {
        const int value = truncDouble(a);
        updateCountForItem(run, targetItem, value);
    } else if (modeT == 2) {
        updateTimerValue(run, targetItem, a);
    } else if (modeT == 3) {
        // 0x2345a7: the level's points. The run keeps its own, so a later item
        // read of mode 3 gives what the run has done and the real game's
        // counter is left alone.
        run.ws->points = truncDouble(a);
    }
}

// GJBaseGameLayer::activateItemCompareTrigger 0x234630, fact (ae).
void fireItemCompare(Run& run, const TriggerDef& d, std::span<const int> keys) {
    const int item1 = remapped(run, keys, d.itemId);
    const int item2 = remapped(run, keys, d.itemId2);
    const int mode1 = d.item1Mode <= 0 ? 1 : d.item1Mode;
    const int op1 = d.resultType1 <= 0 ? 3 : d.resultType1;
    const int op2 = d.resultType2 <= 0 ? 3 : d.resultType2;
    const double mod1 = (double)itemMod(d.mod1);
    const double mod2 = (double)itemMod(d.mod2);
    const double tolerance = (double)itemMod(d.tolerance);
    double a = getItemValue(run, mode1, item1);
    a = itemCombine(op1, a, mod1);
    a = itemRound(d.roundType1, a);
    a = itemSign(d.signType1, a);
    double b = mod2;
    if (itemInPlay(d.item2Mode, item2)) b = itemCombine(op2, getItemValue(run, d.item2Mode, item2), mod2);
    b = itemRound(d.roundType2, b);
    b = itemSign(d.signType2, b);
    // 0x2348fb: six comparisons, anything else never holds.
    bool holds = false;
    switch (d.resultType3) {
        case 0: holds = tolerance >= std::fabs(a - b); break;
        case 1: holds = tolerance + a > b; break;
        case 2: holds = tolerance + a >= b; break;
        case 3: holds = b > a - tolerance; break;
        case 4: holds = b >= a - tolerance; break;
        case 5: holds = std::fabs(a - b) > tolerance; break;
        default: break;
    }
    const int group = holds ? remapped(run, keys, d.target) : remapped(run, keys, d.center);
    spawnGroup(run, group, false, 0.0, keys, d.uid, remapped(run, keys, d.control));
}

// GJBaseGameLayer::activatePersistentItemTrigger 0x234a40, fact (ae).
void firePersistentItem(Run& run, const TriggerDef& d, std::span<const int> keys) {
    WorldState& ws = *run.ws;
    // 0x234b94 and 0x234d08 key the two persistent containers on the item as
    // it stands; only the count and timer functions they call clamp it.
    const int item = remapped(run, keys, d.itemId);
    if (d.timerItem) {
        if (d.targetAll) {
            if (d.resetItem) {
                const std::vector<int> items = *ws.persistentTimers;
                for (int id : items) {
                    if (timerExists(ws, id)) updateTimerValue(run, id, 0.0);
                }
            }
            if (!d.persistent) ws.persistentTimers.edit().clear();
            return;
        }
        std::vector<int>& set = ws.persistentTimers.edit();
        auto it = std::lower_bound(set.begin(), set.end(), item);
        const bool there = it != set.end() && *it == item;
        if (!d.persistent) {
            if (there) set.erase(it);
        } else if (!there) {
            set.insert(it, item);
        }
        if (d.resetItem && timerExists(ws, item)) updateTimerValue(run, item, 0.0);
        return;
    }
    if (d.targetAll) {
        if (d.resetItem) {
            const std::vector<std::pair<int, int>> items = *ws.persistentItems;
            for (const auto& [id, count] : items) updateCountForItem(run, id, 0);
        }
        if (!d.persistent) ws.persistentItems.edit().clear();
        return;
    }
    {
        std::vector<std::pair<int, int>>& map = ws.persistentItems.edit();
        auto it = std::lower_bound(map.begin(), map.end(), item,
                                   [](const std::pair<int, int>& e, int k) { return e.first < k; });
        const bool there = it != map.end() && it->first == item;
        if (!d.persistent) {
            if (there) map.erase(it);
        } else {
            // 0x234d4a: a persistent item starts from the count it has now.
            const int count = countForItem(ws, item);
            if (there) it->second = count;
            else map.insert(it, {item, count});
        }
    }
    if (d.resetItem) updateCountForItem(run, item, 0);
}

// ------------------------------------------------------------ the players' options

// GJBaseGameLayer::activatePlayerControlTrigger 0x2174e0, fact (af). The
// streak points and the dash the game's releaseButton places and stops are
// the copy's own and are left to it; what it clears here is the button state
// the next tick's input reads.
void firePlayerControl(Run& run, const TriggerDef& d) {
    const bool platformer = run.pl && run.pl->m_isPlatformer;
    auto stop = [&](PlayerObject* p) {
        if (!p) return;
        if (d.stopJump) p->releaseButton(PlayerButton::Jump);
        if (d.stopMove && platformer) {
            p->releaseButton(PlayerButton::Left);
            p->releaseButton(PlayerButton::Right);
        }
        if (d.stopRotation) {
            // 0x2176a0: the word at +0x728 is both rotation flags.
            p->m_isRotating = false;
            p->m_isBallRotating2 = false;
            p->m_isBallRotating = false;
            p->m_rotationSpeed = 0.0f;
        }
        if (d.stopSlide) {
            p->m_isAccelerating = false;
            p->m_affectedByForces = false;
        }
    };
    if (!d.targetP1 && !d.targetP2) {
        stop(run.player1);
        stop(run.player2);
        return;
    }
    if (d.targetP1) stop(run.player1);
    if (d.targetP2) stop(run.player2);
}

// PlayerObject::disablePlayerControls 0x39f3b0 / enablePlayerControls
// 0x39f500 on the copy - the game's own, which is what the option calls. They
// only ever touch the player they are given, and everything they change is in
// the copy's checkpoint, so a snapshot puts it all back. A port of them here
// let go of the buttons the copy held but never pressed them again when the
// option turned controls back on, which is most of what enablePlayerControls
// does (0x39f539 and 0x39f666 push a held left, right or jump again).
void setControlsDisabled(PlayerObject* p, bool disabled) {
    if (!p) return;
    if (disabled) p->disablePlayerControls();
    else p->enablePlayerControls();
}

// GJBaseGameLayer::processOptionsTrigger 0x223d50, fact (ai): only the
// settings a run can feel.
void fireOptions(Run& run, const TriggerDef& d) {
    WorldState& ws = *run.ws;
    auto* o = geode::cast::typeinfo_cast<GameOptionsTrigger*>(objectOf(run, d.slot));
    if (!o) return;
    auto setting = [](GameOptionsSetting s) { return (int)s; };
    if (const int v = setting(o->m_unlinkDualGravity); v != 0) {
        ws.unlinkedDual = v == 1;
        if (run.pl) run.pl->m_gameState.m_unkBool31 = ws.unlinkedDual;
    }
    if (const int v = setting(o->m_disableP1Controls); v != 0) setControlsDisabled(run.player1, v == 1);
    if (const int v = setting(o->m_disableP2Controls); v != 0) setControlsDisabled(run.player2, v == 1);
    if (const int v = setting(o->m_boostSlide); v != 0) {
        const bool on = v == 1;
        if (run.player1) run.player1->m_decreaseBoostSlide = on;
        if (run.player2) run.player2->m_decreaseBoostSlide = on;
    }
    // Left alone: the streak blend, the ground, middleground, attempt and
    // player visibility and the death audio, none of which a run reads, and
    // the respawn time - a run ends at the death it is looking for, so the
    // wait after one changes nothing it can see.
}

}  // namespace

int countForItem(const WorldState& ws, int item) {
    const int key = clampGroup(item);
    const auto& items = *ws.items;
    auto it = std::lower_bound(items.begin(), items.end(), key,
                               [](const std::pair<int, int>& e, int k) { return e.first < k; });
    return (it != items.end() && it->first == key) ? it->second : 0;
}

void updateCountForItem(Run& run, int item, int value) {
    // updateCountForItem 0x2624b0, fact (u).
    WorldState& ws = *run.ws;
    const int key = clampGroup(item);
    const int old = countForItem(ws, key);
    {
        std::vector<std::pair<int, int>>& items = ws.items.edit();
        auto it = std::lower_bound(items.begin(), items.end(), key,
                                   [](const std::pair<int, int>& e, int k) { return e.first < k; });
        const bool there = it != items.end() && it->first == key;
        if (value == 0) {
            if (there) items.erase(it);
        } else if (there) {
            it->second = value;
        } else {
            items.insert(it, {key, value});
        }
    }
    {
        const auto& persistent = *ws.persistentItems;
        auto it = std::lower_bound(persistent.begin(), persistent.end(), key,
                                   [](const std::pair<int, int>& e, int k) { return e.first < k; });
        if (it != persistent.end() && it->first == key) {
            const std::size_t at = (std::size_t)(it - persistent.begin());
            ws.persistentItems.edit()[at].second = value;
        }
    }
    auto range = [&ws, key]() {
        const auto& list = *ws.countListeners;
        auto lo = std::lower_bound(list.begin(), list.end(), key, [](const WCountListener& l, int k) { return l.item < k; });
        auto hi = std::upper_bound(lo, list.end(), key, [](int k, const WCountListener& l) { return k < l.item; });
        return std::pair<std::size_t, std::size_t>((std::size_t)(lo - list.begin()), (std::size_t)(hi - list.begin()));
    };
    auto [begin, end] = range();
    if (begin == end) return;
    {
        std::vector<WCountListener>& list = ws.countListeners.edit();
        // std::sort of up to 32 is an insertion sort, which keeps equal ones
        // in order: stable_sort gives the same. More than that is the game's
        // introsort, whose order among equal targets is its own: any of the
        // listeners can fire in another order (this marked the first one's
        // group alone).
        if (end - begin > 32) {
            const std::vector<WCountListener> waiting(list.begin() + (std::ptrdiff_t)begin,
                                                      list.begin() + (std::ptrdiff_t)end);
            for (const WCountListener& l : waiting) markSpawnUncertain(*run.def, ws, l.targetGroup);
        }
        if (old > value) {
            std::stable_sort(list.begin() + (std::ptrdiff_t)begin, list.begin() + (std::ptrdiff_t)end,
                             [](const WCountListener& a, const WCountListener& b) { return b.targetCount < a.targetCount; });
        } else {
            std::stable_sort(list.begin() + (std::ptrdiff_t)begin, list.begin() + (std::ptrdiff_t)end,
                             [](const WCountListener& a, const WCountListener& b) { return a.targetCount < b.targetCount; });
        }
    }
    std::size_t i = 0;
    for (;;) {
        std::tie(begin, end) = range();
        if (begin + i >= end) break;
        WCountListener& a = ws.countListeners.edit()[begin + i];
        if (a.disabled || a.previousCount == value) {
            i++;
            continue;
        }
        const int previous = a.previousCount;
        const int target = a.targetCount;
        a.previousCount = value;
        const bool crossed = previous < target ? value >= target : (previous > target && value <= target);
        if (!crossed) {
            i++;
            continue;
        }
        const int group = a.targetGroup;
        const bool activate = a.activateGroup;
        const int uid = a.triggerUid;
        const int control = a.controlId;
        const bool erase = !a.multiActivate;
        std::vector<int> keys;
        if (erase) {
            std::vector<WCountListener>& list = ws.countListeners.edit();
            list.erase(list.begin() + (std::ptrdiff_t)(begin + i));
            // The game copies the keys from the slot after the erase: the
            // next listener's, or none when this one was the last.
            if (begin + i < end - 1) keys = keysOf(ws, list[begin + i].remap);
        } else {
            keys = keysOf(ws, a.remap);
        }
        toggleGroupTriggered(run, group, activate, keys, uid, control);
        if (!erase) i++;
    }
}

void updateTimers(Run& run, float dt) {
    // GJEffectManager::updateTimers 0x263340, fact (ad). Every lookup is made
    // again after a fire: a spawn can start a timer, stop one or add and erase
    // listeners, and the game's own walk reads the containers as they stand.
    WorldState& ws = *run.ws;
    if (ws.timers->empty() && ws.timerListeners->empty()) return;
    // The keys as they were at the top: a timer a fire starts waits for the
    // next tick (0x2633b4 copies them first). A list of its own rather than a
    // kept one: a spawn below can reach this again.
    std::vector<int> keys;
    keys.reserve(ws.timers->size());
    for (const WTimer& t : *ws.timers) keys.push_back(t.key);

    auto timerAt = [&ws](int key) -> int {
        const auto& timers = *ws.timers;
        for (std::size_t i = 0; i < timers.size(); i++) {
            if (timers[i].key == key) return (int)i;
        }
        return -1;
    };
    // The run of listeners of one item, which is kept sorted by item.
    auto listenerRange = [&ws](int item) {
        const auto& list = *ws.timerListeners;
        auto lo = std::lower_bound(list.begin(), list.end(), item,
                                   [](const WTimerListener& l, int k) { return l.item < k; });
        auto hi = std::upper_bound(lo, list.end(), item, [](int k, const WTimerListener& l) { return k < l.item; });
        return std::pair<std::size_t, std::size_t>((std::size_t)(lo - list.begin()), (std::size_t)(hi - list.begin()));
    };

    for (int key : keys) {
        int idx = timerAt(key);
        if (idx < 0) continue;
        WTimer timer = (*ws.timers)[(std::size_t)idx];
        if (timer.active && !timer.disabled) {
            const double old = timer.time;
            const double next = (double)(dt * timer.timeMod) + old;
            bool stopped = false;
            {
                WTimer& w = ws.timers.edit()[(std::size_t)idx];
                w.time = next;
                if (w.stopTimeEnabled) {
                    const double target = w.targetTime;
                    // 0x263515: the step crossed the target, either way round.
                    const bool crossed = (target > old && next >= target) || (old > target && target >= next);
                    if (crossed) {
                        w.time = target;
                        w.active = false;
                        stopped = true;
                    }
                }
                timer = w;
            }
            if (stopped && timer.targetGroup > 0) {
                spawnGroup(run, timer.targetGroup, false, 0.0, keysOf(ws, timer.remap), timer.triggerUid,
                           timer.controlId);
                if (const int now = timerAt(key); now >= 0) timer = (*ws.timers)[(std::size_t)now];
            }
        }
        // The listeners of the timer's item: a fire takes the timer's keys,
        // uid and control, not the listener's (0x2637f1).
        std::size_t i = 0;
        for (;;) {
            auto [begin, end] = listenerRange(timer.itemId);
            if (begin + i >= end) break;
            const WTimerListener l = (*ws.timerListeners)[begin + i];
            bool fires = false;
            if (l.targetTime > l.time) {
                fires = timer.time >= (double)l.targetTime && timer.timeMod > 0.0f;
            }
            if (!fires && l.time > l.targetTime) {
                fires = (double)l.targetTime >= timer.time && 0.0f > timer.timeMod;
            }
            if (l.disabled || !fires) {
                ws.timerListeners.edit()[begin + i].time = (float)timer.time;
                i++;
                continue;
            }
            const bool erase = !l.multiActivate;
            if (erase) {
                std::vector<WTimerListener>& list = ws.timerListeners.edit();
                list.erase(list.begin() + (std::ptrdiff_t)(begin + i));
            }
            spawnGroup(run, l.targetGroup, false, 0.0, keysOf(ws, timer.remap), timer.triggerUid, timer.controlId);
            if (const int now = timerAt(key); now >= 0) timer = (*ws.timers)[(std::size_t)now];
            if (!erase) {
                auto [b2, e2] = listenerRange(timer.itemId);
                if (b2 + i < e2) ws.timerListeners.edit()[b2 + i].time = (float)timer.time;
                i++;
            }
        }
    }
}

void toggleGroup(Run& run, int group, bool activate) {
    // toggleGroup 0x223bc0.
    WorldState& ws = *run.ws;
    const int g = clampGroup(group);
    if (ws.groupEnabled(g) == activate) return;
    if (run.log && run.def && !run.def->members(g).empty()) {
        Op op;
        op.tick = (uint32_t)ws.tick;
        op.commandIndex = ws.commandIndex;
        op.kind = OpKind::Toggle;
        op.sign = activate ? 1 : -1;
        op.group = g;
        emitOp(run, op);
    }
    ToggleWords& bits = ws.toggleBits.edit();
    if (activate) bits[(std::size_t)g >> 5] |= uint32_t{1} << (g & 31);
    else bits[(std::size_t)g >> 5] &= ~(uint32_t{1} << (g & 31));
    int8_t& delta = lookupInsert(ws.toggleDelta, g);
    delta = (int8_t)std::clamp(delta + (activate ? 1 : -1), -127, 127);
}

void spawnGroup(Run& run, int group, bool ordered, double delay, std::span<const int> keys, int uid, int control) {
    // spawnGroup 0x21ab80, fact (q).
    WorldState& ws = *run.ws;
    if (!ws.groupEnabled(clampGroup(group))) return;
    const std::array<int, 3> tuple{group, run.def->enable22Changes ? uid : 0, keys.empty() ? 0 : -keys.back()};
    if (std::find(run.spawnTuples.begin(), run.spawnTuples.end(), tuple) != run.spawnTuples.end()) return;
    run.spawnTuples.push_back(tuple);
    if (run.depth >= kMaxDepth) {
        markUncertain(ws, group);
        return;
    }
    Deeper deeper(run);
    if (ordered) {
        spawnObjectsInOrder(run, clampGroup(group), delay, keys, uid, control);
        return;
    }
    // The members spawnObject can do anything with, in the array's order (a
    // copy: a spawn can grow nothing here, but the span is the WorldDef's).
    for (int slot : run.def->spawnTriggeredIn(clampGroup(group))) spawnObject(run, slot, delay, keys);
}

void spawnObject(Run& run, int slot, double delay, std::span<const int> keys) {
    // spawnObject 0x21b030, fact (r).
    WorldState& ws = *run.ws;
    GameObject* o = objectOf(run, slot);
    if (!o || o->m_classType != GameObjectClassType::Effect) return;  // (animationTriggered: nothing physical)
    if (groupDisabled(run, slot)) return;
    const int di = run.def->defOfSlot[(std::size_t)slot];
    if (di < 0) return;  // an effect object with nothing physical to fire
    const TriggerDef& d = run.def->triggers[(std::size_t)di];
    auto* e = static_cast<EnhancedGameObject*>(o);
    if (!d.multi && (e->m_activatedByPlayer1 || e->m_activatedByPlayer2 || sortedContains(ws.triggeredOnce, d.uid))) return;
    if (!d.spawn) return;
    if (!(run.def->flags[(std::size_t)slot] & kSpawnable)) return;
    // triggerActivated 0x4a8790: both flags, which the run keeps as its own.
    if (!d.multi) sortedInsert(ws.triggeredOnce, d.uid);
    fire(run, di, keys, 0, delay);
}

void fire(Run& run, int defIdx, std::span<const int> keys, int playerUid, double currentDelay) {
    WorldState& ws = *run.ws;
    if (defIdx < 0 || (std::size_t)defIdx >= run.def->triggers.size()) return;
    const TriggerDef& d = run.def->triggers[(std::size_t)defIdx];
    ws.dirtyShape = true;
    if (d.paramsMissing) {
        markFireUncertain(*run.def, ws, d, remapped(run, keys, d.target), remapped(run, keys, d.center));
        return;
    }
    switch (d.kind) {
        case Kind::Speed:
            if (run.pl) {
                run.pl->m_gameState.m_timeModRelated = d.speed;
                run.pl->m_gameState.m_timeModRelated2 = d.hasNoEffects;
            }
            ws.timeMod = d.speed;
            ws.timeMod2 = d.hasNoEffects;
            return;
        case Kind::Gravity:
            if (d.followCPP) {
                if (PlayerObject* p = copyForUid(run, playerUid)) p->m_gravityMod = d.gravity;
                return;
            }
            if (!d.targetP2 && run.player1) run.player1->m_gravityMod = d.gravity;
            if (!d.targetP1 && run.player2) run.player2->m_gravityMod = d.gravity;
            return;
        case Kind::RotateGameplay:
            fireRotateGameplay(run, d);
            return;
        case Kind::Teleport:
            if (run.player1) {
                auto* portal = static_cast<TeleportPortalObject*>(objectOf(run, d.slot));
                if (remapped(run, keys, d.target) != d.target) {
                    // The game teleports to the remapped group; the copy's
                    // teleport reads the object, which a run never rewrites.
                    markUncertain(ws, remapped(run, keys, d.target));
                }
                if (portal && run.pl) phys::teleportPlayer(run.pl, portal, run.player1);
            }
            return;
        case Kind::Move:
            triggerMoveCommand(run, d, keys);
            return;
        case Kind::Toggle:
            toggleGroup(run, remapped(run, keys, d.target), d.activateGroup);
            return;
        case Kind::Spawn:
            fireSpawn(run, d, keys, currentDelay);
            return;
        case Kind::Stop: {
            const int target = remapped(run, keys, d.target);
            if (d.useControlId) controlTriggersWithControlId(run, target, d.controlAction);
            else controlTriggersInGroup(run, target, d.controlAction);
            return;
        }
        case Kind::Count:
        case Kind::InstantCount:
        case Kind::Pickup:
            fireCount(run, d, keys);
            return;
        case Kind::Touch:
            fireTouch(run, d, keys);
            return;
        case Kind::ToggleOrb:
        case Kind::ToggleBlock:
            // A custom ring does nothing when a trigger fires it: what it does
            // is in the ring path of the player (ringActivated, fact (x)).
            return;
        case Kind::Random:
        case Kind::AdvancedRandom:
            fireRandom(run, d, keys);
            return;
        case Kind::Sequence:
            fireSequence(run, d, keys);
            return;
        case Kind::Event:
            fireEvent(run, d, keys);
            return;
        case Kind::TimeTrigger:
        case Kind::TimeEvent:
        case Kind::TimeControl:
            fireTimer(run, d, keys);
            return;
        case Kind::ItemEdit:
            fireItemEdit(run, d, keys);
            return;
        case Kind::ItemCompare:
            fireItemCompare(run, d, keys);
            return;
        case Kind::ItemPersistent:
            firePersistentItem(run, d, keys);
            return;
        case Kind::PlayerControl:
            firePlayerControl(run, d);
            return;
        case Kind::Reverse: {
            // reverseDirection 0x218160, fact (ag).
            auto reverse = [](PlayerObject* p) {
                if (p && !p->m_isPlatformer) p->doReversePlayer(!p->m_isGoingLeft);
            };
            reverse(run.player1);
            if (run.pl && run.pl->m_gameState.m_isDualMode) reverse(run.player2);
            return;
        }
        case Kind::TimeWarp:
            // 0x4a736e: only parked. The end of the tick applies it (fact (ah)).
            ws.queuedTimeWarp = d.timeWarp;
            return;
        case Kind::Options:
            fireOptions(run, d);
            return;
        case Kind::End:
            // PlayLayer::activatePlatformerEndTrigger 0x3ac2d0, fact (aj).
            if (run.player1Dead || ws.levelEnd) return;
            if (remapped(run, keys, d.target) > 0) {
                spawnGroup(run, remapped(run, keys, d.target), false, 0.0, keys, d.uid,
                           remapped(run, keys, d.control));
            }
            ws.levelEnd = true;
            return;
        case Kind::Reset:
            // 3618 (0x4a7ea2) walks its group and, for every collectible,
            // breakable and 2063 in it, forgets the triggered ids of both
            // players and calls resetObject - which puts the whole object back
            // the way the level had it: its position, scale, rotation,
            // animation frame and activated flags. The World follows where
            // objects stand as a log of moves, not as whole-object state, and
            // a run may not write the level, so the reset is not ported: its
            // group stays uncertain, exactly as it was before this work.
            [[fallthrough]];
        default:
            // Not run by the World (yet): what it names is uncertain, and so is
            // everything down the spawns and toggles it makes.
            markFireUncertain(*run.def, ws, d, remapped(run, keys, d.target), remapped(run, keys, d.center));
            markUncertain(ws, remapped(run, keys, d.targetModCenter));
            return;
    }
}

void touchedTrigger(Run& run, PlayerObject* copy, EffectGameObject* object) {
    if (!run.def || !run.ws || !object) return;
    const int slot = run.def->slotOf(object);
    const int di = slot >= 0 ? run.def->defOfSlot[(std::size_t)slot] : -1;
    if (di < 0) return;
    const TriggerDef& d = run.def->triggers[(std::size_t)di];
    switch (d.kind) {
        case Kind::Speed:
        case Kind::Gravity:
        case Kind::RotateGameplay:
        case Kind::Teleport:
            // The copy's own collision pass has already taken these
            // (phys::triggerObject): firing them again here would take them twice.
            return;
        default:
            break;
    }
    // (The copy's collision pass has already remembered it as fired through
    // phys::triggerObject's activateForTrajectory, which is what makes the
    // spawn walk pass a touch-triggered entry rather than wait there.)
    WorldState& ws = *run.ws;
    // playerTouchedTrigger 0x217f50, fact (z). A copy's m_uniqueID is not the
    // game's 1 or 2, so which copy it is stands in for the player's id.
    const int copyUid = copyIndex(copy) == 1 ? 2 : 1;
    // The key the triggered set holds is the player's id only for a trigger
    // that is not single-player-touch; the fire itself always gets the
    // player's own id (0x217fa2 builds the key, 0x21811c reads m_uniqueID
    // again for the call), so a single-player-touch gravity trigger with
    // "follow CP" still knows which copy touched it.
    const std::pair<int, int> key{d.uid, object->m_isSinglePTouch ? 0 : copyUid};
    auto find = [&ws, key]() {
        const auto& ids = *ws.triggeredIds;
        auto it = std::lower_bound(ids.begin(), ids.end(), key);
        return std::pair<std::size_t, bool>((std::size_t)(it - ids.begin()), it != ids.end() && *it == key);
    };
    auto [at, there] = find();
    // removeTriggeredID: a multi-triggered object drops its key whenever the
    // contact is new, and the collision pass only gets here on a new contact
    // (canActivate). So the key never stops one of those firing again.
    if (there && d.multi) {
        std::vector<std::pair<int, int>>& ids = ws.triggeredIds.edit();
        ids.erase(ids.begin() + (std::ptrdiff_t)at);
        there = false;
    }
    if (there) return;  // it has already fired for this player
    {
        // storeTriggeredID 0x261ff0.
        std::vector<std::pair<int, int>>& ids = ws.triggeredIds.edit();
        ids.insert(ids.begin() + (std::ptrdiff_t)at, key);
    }
    // triggerObject(layer, player->m_uniqueID, nullptr): no remap keys of its
    // own, and the uid is what a gravity trigger with "follow CP" reads.
    fire(run, di, std::span<const int>{}, copyUid);
}

void ringActivated(RingObject* ring) {
    // activateCustomRing, fact (x). The game reads the spawn-only flag off the
    // ring without asking what class it is (0x398e84), and every object the
    // collision pass calls a ring is one: the call takes a RingObject so the
    // flag is read the same way, with no cast of its own.
    Run* run = currentRun();
    if (!run || !run->ws || !run->def || !ring) return;
    WorldState& ws = *run->ws;
    ws.dirtyShape = true;
    const int group = ring->m_targetGroupID;
    const int uid = ring->m_uniqueID;
    const int control = ring->m_controlID;
    if (ring->m_isSpawnOnly) {
        spawnGroup(*run, group, false, 0.0, std::span<const int>{}, uid, control);
        return;
    }
    toggleGroupTriggered(*run, group, ring->m_activateGroup, std::span<const int>{}, uid, control);
}

void onButton(Run& run, PlayerObject* copy, bool down) {
    // GJEffectManager::playerButton 0x262190, fact (y).
    if (!run.ws || !run.def) return;
    if (run.ws->touchListeners->empty()) return;
    // handleButton 0x233aa3: nothing at all while the copy of player 1 is dead.
    if (run.player1Dead) return;
    WorldState& ws = *run.ws;
    const bool isP2 = copyIndex(copy) == 1;
    // By index, with the size read again every time: a toggle can spawn a
    // trigger that registers another touch listener, or a stop that erases one.
    for (std::size_t i = 0; i < ws.touchListeners->size(); i++) {
        const WTouchListener e = (*ws.touchListeners)[i];  // by value: the vector can move under the fire
        if (e.disabled) continue;
        if (e.dualMode && isP2) continue;
        if (e.touchControl == 1) {
            if (!isP2) continue;
        } else if (e.touchControl == 2) {
            if (isP2) continue;
        } else if (e.touchControl != 0) {
            continue;
        }
        if (!down && !e.holdMode) continue;
        ws.dirtyShape = true;
        bool activate = !ws.groupEnabled(e.targetGroup);
        if (e.touchType != 0) activate = e.holdMode ? (e.touchType == 1 ? down : !down) : (e.touchType == 1);
        toggleGroupTriggered(run, e.targetGroup, activate, keysOf(ws, e.remap), e.triggerUid, e.controlId);
    }
}

void onEvent(int event, int material, int playerUid) {
    Run* run = currentRun();
    if (!run || !run->ws || !run->def) return;
    WorldState& ws = *run->ws;
    // GJBaseGameLayer::gameEventTriggered 0x231ff0, fact (ac). A copy's uid is
    // not the game's 1 or 2, so the player the event names is which copy it
    // is; an event of no player keeps its 0.
    int player = playerUid;
    if (player != 0) {
        if (run->player1 && player == run->player1->m_uniqueID) player = 1;
        else if (run->player2 && player == run->player2->m_uniqueID) player = 2;
    }
    const int key = material * 10000 + player;
    {
        std::vector<WEventStamp>& stamps = ws.eventStamps.edit();
        auto at = std::lower_bound(stamps.begin(), stamps.end(), std::pair<int, int>{event, key},
                                   [](const WEventStamp& e, const std::pair<int, int>& k) {
                                       return std::pair<int, int>{e.event, e.key} < k;
                                   });
        // 0x232030: the game's operator[] puts an entry of 0 there whether
        // the event fires or not, and an entry that already holds the command
        // index stops it.
        if (at == stamps.end() || at->event != event || at->key != key) {
            at = stamps.insert(at, WEventStamp{event, key, 0});
        }
        if (at->index == (int)ws.commandIndex) {
            // Only the second call, for the listeners of either player, is left.
            if (player != 0) onEvent(event, material, 0);
            return;
        }
        at->index = (int)ws.commandIndex;
    }
    // The listeners under (event, key), in the order they were added. The game
    // takes the vector of that one key and walks from its begin to the end it
    // had when it started (0x232073), so a listener an earlier fire appends
    // waits for the next event. The run keeps every key's listeners in one
    // list, so the run of this key is looked up again on every step - a fire
    // can add one under another key and move this one along - and the walk
    // stops at the count the key started with. Scanning the whole list instead
    // fired a listener twice when a fire inserted before it, and cost a pass
    // over every listener of the level for each event a copy raised.
    auto listenerRange = [&ws](int ev, int k) {
        const auto& list = *ws.eventListeners;
        const std::pair<int, int> want{ev, k};
        auto lo = std::lower_bound(list.begin(), list.end(), want,
                                   [](const WEventListener& l, const std::pair<int, int>& q) {
                                       return std::pair<int, int>{l.event, l.key} < q;
                                   });
        auto hi = std::upper_bound(lo, list.end(), want, [](const std::pair<int, int>& q, const WEventListener& l) {
            return q < std::pair<int, int>{l.event, l.key};
        });
        return std::pair<std::size_t, std::size_t>((std::size_t)(lo - list.begin()), (std::size_t)(hi - list.begin()));
    };
    const auto [firstAt, lastAt] = listenerRange(event, key);
    for (std::size_t i = 0; i < lastAt - firstAt; i++) {
        const auto [lo, hi] = listenerRange(event, key);
        if (lo + i >= hi) break;
        const WEventListener l = (*ws.eventListeners)[lo + i];
        if (l.inactive) continue;
        spawnGroup(*run, l.targetGroup, false, 0.0, keysOf(ws, l.remap), l.triggerUid, l.controlId);
    }
    if (player != 0) onEvent(event, material, 0);
}

// ------------------------------------------------------------ what cannot be followed

namespace {

// Kinds whose fire toggles or spawns groups (toggleGroupTriggered 0x223b50
// spawns what it switches on) or changes the items that count, instant count,
// item compare and time triggers read: when the World cannot follow such a
// fire, the triggers in those groups fire when it does not know.
bool spawnsOrToggles(Kind k) {
    switch (k) {
        case Kind::Toggle:
        case Kind::Spawn:
        case Kind::Count:
        case Kind::InstantCount:
        case Kind::Pickup:
        case Kind::Touch:
        case Kind::ToggleOrb:
        case Kind::ToggleBlock:
        case Kind::ItemEdit:
        case Kind::ItemCompare:
        case Kind::ItemPersistent:
        case Kind::Collision:
        case Kind::InstantCollision:
        case Kind::TimeTrigger:
        case Kind::TimeEvent:
        case Kind::TimeControl:
        case Kind::Reset:
        case Kind::Event:
        case Kind::Sequence:
        case Kind::Random:
        case Kind::AdvancedRandom:
            return true;
        default:
            return false;
    }
}

bool changesItems(Kind k) {
    return k == Kind::Pickup || k == Kind::ItemEdit || k == Kind::ItemPersistent || k == Kind::TimeControl ||
           k == Kind::Reset;
}

bool readsItems(Kind k) {
    return k == Kind::Count || k == Kind::InstantCount || k == Kind::ItemCompare || k == Kind::TimeEvent;
}

// The walk markSpawnUncertain and markFireUncertain share: a work list of
// groups, each walked once per state (WorldState::uncertainSpawns).
struct UncertainWalk {
    const WorldDef& def;
    WorldState& ws;
    std::vector<int> todo;
    std::vector<int> items;  // the items already looked up in this walk

    void group(int g) {
        if (g <= 0) return;
        markUncertain(ws, g);
        const uint16_t c = (uint16_t)WorldDef::clampGroup(g);
        auto it = std::lower_bound(ws.uncertainSpawns.begin(), ws.uncertainSpawns.end(), c);
        if (it != ws.uncertainSpawns.end() && *it == c) return;
        ws.uncertainSpawns.insert(it, c);
        todo.push_back(c);
    }

    // Everything that reads an item whose count is not known.
    void item(int itemId) {
        const int key = (int)WorldDef::clampGroup(itemId);
        if (std::find(items.begin(), items.end(), key) != items.end()) return;
        items.push_back(key);
        for (const WCountListener& l : *ws.countListeners) {
            if (l.item == key) group(l.targetGroup);
        }
        for (const WTimerListener& l : *ws.timerListeners) {
            if (l.item == key) group(l.targetGroup);
        }
        for (const TriggerDef& t : def.triggers) {
            if (!readsItems(t.kind)) continue;
            if ((int)WorldDef::clampGroup(t.itemId) == key || (int)WorldDef::clampGroup(t.itemId2) == key) {
                group(t.target);
                group(t.center);
            }
        }
    }

    void trigger(const TriggerDef& d, int target, int center) {
        markUncertain(ws, target);
        markUncertain(ws, center);
        markUncertain(ws, d.targetModCenter);
        markUncertain(ws, d.rotationTarget);
        // What the kind reaches beyond the groups it names (world/tierd.cpp):
        // an area effect moves the members of the group it names, but an
        // enter effect works on a channel and a time warp on everything.
        if (fireReachesAnyObject(def, d.kind)) ws.uncertainAny = true;
        // A keyframe animation whose trigger names no target group animates
        // the group each of its keyframe objects names instead.
        if (d.kind == Kind::KeyframeAnim) markKeyframeTargets(def, ws, d);
        if (!spawnsOrToggles(d.kind)) return;
        group(target);
        group(center);
        for (int i = 0; i < d.chanceCount; i++) {
            const std::size_t at = (std::size_t)(d.chanceBegin + i);
            if (at >= def.chances.size()) break;
            group(def.chances[at].group);
            group(def.chances[at].oldGroup);
        }
        if (changesItems(d.kind)) {
            item(d.itemId);
            item(d.itemId2);
        }
    }

    void finish() {
        while (!todo.empty()) {
            const int g = todo.back();
            todo.pop_back();
            // Every trigger in the group: the ones a spawn fires, and the ones
            // the spawn walk passes without firing while the group is off
            // (isSpawnableTrigger 0x1a26b0 leaves some of those out).
            for (int slot : def.members(g)) {
                if (slot < 0 || (std::size_t)slot >= def.defOfSlot.size()) continue;
                if (!(def.flags[(std::size_t)slot] & kTrigger)) continue;
                const int di = def.defOfSlot[(std::size_t)slot];
                if (di < 0) continue;
                const TriggerDef& t = def.triggers[(std::size_t)di];
                trigger(t, t.target, t.center);
            }
        }
    }
};

}  // namespace

void markSpawnUncertain(const WorldDef& def, WorldState& ws, int group) {
    if (group <= 0) return;
    UncertainWalk walk{def, ws, {}, {}};
    walk.group(group);
    walk.finish();
}

void markFireUncertain(const WorldDef& def, WorldState& ws, const TriggerDef& trigger, int target, int center) {
    UncertainWalk walk{def, ws, {}, {}};
    walk.trigger(trigger, target, center);
    walk.finish();
}

void switchChannel(WorldState& ws, int channel, bool goingBack) {
    ws.channel = channel;
    lookupInsert(ws.goingBack, channel) = goingBack;
}

bool walkModelled(const Run& run) {
    return run.player1 && run.def && run.pl && run.ws && run.log && run.cache && !run.player1->m_isPlatformer &&
           !run.pl->m_isPlatformer;
}

bool checkSpawnObjects(Run& run) {
    // checkSpawnObjects 0x21a8f0, facts (d) in world/step.cpp and (w).
    if (!walkModelled(run)) return false;
    PlayerObject* p1 = run.player1;
    WorldState& ws = *run.ws;
    // The position once, before the loop (0x21a93a); m_isSideways again for
    // every entry (0x21aa47), below.
    const cocos2d::CCPoint pos = p1->getPosition();
    Trajectory* traj = Bot::get()->trajectory().unsafeInner();
    // Every entry the walk passes moves the cursor on, so this ends; the
    // bound only guards a list the model got wrong.
    for (int guard = 0; guard < 1 << 20; guard++) {
        const int channel = ws.channel;
        const std::span<const int> list = run.def->spawnList(channel);
        const int cursor = lookupInsert(ws.spawnCursor, channel);
        const bool back = lookupInsert(ws.goingBack, channel);
        if (cursor < 0 || (std::size_t)cursor >= list.size()) return true;
        const int slot = list[(std::size_t)cursor];
        auto* e = static_cast<EffectGameObject*>(objectOf(run, slot));
        if (!e) return true;  // an entry the level read does not hold: the walk cannot go past it
        bool passed = false;
        if (e->m_isTouchTriggered) {
            // Waits there until touched: by the real player, or by a copy
            // in this run (its touch fires it through the collision pass).
            const bool touched = e->m_activated ||
                                 (traj && ((run.player1 && traj->copyFired(run.player1, e)) ||
                                           (run.player2 && traj->copyFired(run.player2, e))));
            if (!touched) return true;
            passed = true;
        } else {
            const cocos2d::CCPoint start = e->m_speedStart;
            // Read on every entry: a rotate-gameplay trigger this walk has just
            // fired can have turned the player sideways (it was read once
            // before the loop).
            const bool sideways = p1->m_isSideways;
            bool reached;
            if (sideways) reached = back ? start.y >= pos.y : pos.y >= start.y;
            else reached = back ? start.x >= pos.x : pos.x >= start.x;
            if (!reached) return true;
        }
        if (!passed && !groupDisabled(run, slot)) {
            // triggerObject(layer, 0, nullptr); a spawn trigger's own keys come
            // from updateRemapKeys with an empty list, which fireSpawn builds
            // from the (empty) keys it is handed.
            const int di = run.def->defOfSlot[(std::size_t)slot];
            if (di >= 0) fire(run, di, std::span<const int>{}, 0);
        }
        // The channel of this iteration's top, whatever the fire switched to.
        lookupInsert(ws.spawnCursor, channel)++;
    }
    return true;
}

}  // namespace world
