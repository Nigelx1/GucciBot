// World::captureLive: the live game's trigger state as a WorldState (trigger
// design step 4). It only reads: every container is walked as it stands and
// copied into plain data, and nothing here can allocate in or write to the
// game. The offsets and element sizes it relies on are pinned in
// world/offsets.hpp against the game's own code.

#include "core/platform.hpp"
#include <Geode/Geode.hpp>

#include <algorithm>
#include <cstring>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/world/def.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

namespace world {

// Every WCmd field up to +0x1b1 sits at the offset of the GroupCommandObject2
// member it is copied from (and back, for the step's fallback through the
// game's own function), so the copy below is the game's command field for
// field. From +0x1b8 on the game has containers and a pointer where WCmd has
// a count, a slot and a Remap; those fields are copied by name.
#define WORLD_CMD_FIELDS(X)                                   \
    X(uid, m_groupCommandUniqueID)                            \
    X(easingType, m_easingType)                               \
    X(easingRate, m_easingRate)                               \
    X(duration, m_duration)                                   \
    X(deltaTime, m_deltaTime)                                 \
    X(targetGroup, m_targetGroupID)                           \
    X(centerGroup, m_centerGroupID)                           \
    X(currentX, m_currentXOffset)                             \
    X(currentY, m_currentYOffset)                             \
    X(deltaX, m_deltaX)                                       \
    X(deltaY, m_deltaY)                                       \
    X(oldDeltaX, m_oldDeltaX)                                 \
    X(oldDeltaY, m_oldDeltaY)                                 \
    X(lockedCurrentX, m_lockedCurrentXOffset)                 \
    X(lockedCurrentY, m_lockedCurrentYOffset)                 \
    X(finished, m_finished)                                   \
    X(disabled, m_disabled)                                   \
    X(finishRelated, m_finishRelated)                         \
    X(lockPlayerX, m_lockToPlayerX)                           \
    X(lockPlayerY, m_lockToPlayerY)                           \
    X(lockCameraX, m_lockToCameraX)                           \
    X(lockCameraY, m_lockToCameraY)                           \
    X(lockedInX, m_lockedInX)                                 \
    X(lockedInY, m_lockedInY)                                 \
    X(modX, m_moveModX)                                       \
    X(modY, m_moveModY)                                       \
    X(rotateValue, m_currentRotateOrTransformValue)           \
    X(rotateDelta, m_currentRotateOrTransformDelta)           \
    X(interpOne1, m_someInterpValue1RelatedOne)               \
    X(interpOne2, m_someInterpValue2RelatedOne)               \
    X(rotationOffset, m_rotationOffset)                       \
    X(lockObjectRotation, m_lockObjectRotation)               \
    X(targetPlayer, m_targetPlayer)                           \
    X(followXMod, m_followXMod)                               \
    X(followYMod, m_followYMod)                               \
    X(commandType, m_commandType)                             \
    X(interp1, m_someInterpValue1)                            \
    X(interp2, m_someInterpValue2)                            \
    X(keyframeRelated, m_keyframeRelated)                     \
    X(targetScaleX, m_targetScaleX)                           \
    X(targetScaleY, m_targetScaleY)                           \
    X(property450, m_transformTriggerProperty450)             \
    X(property451, m_transformTriggerProperty451)             \
    X(interpZero1, m_someInterpValue1RelatedZero)             \
    X(interpZero2, m_someInterpValue2RelatedZero)             \
    X(onlyMove, m_onlyMove)                                   \
    X(transformFlag, m_transformRelatedFalse)                 \
    X(relativeRotation, m_relativeRotation)                   \
    X(interpRelated1, m_someInterpValue1Related)              \
    X(interpRelated2, m_someInterpValue2Related)              \
    X(followYSpeed, m_followYSpeed)                           \
    X(followYDelay, m_followYDelay)                           \
    X(followYOffset, m_followYOffset)                         \
    X(followYMaxSpeed, m_followYMaxSpeed)                     \
    X(triggerUid, m_triggerUniqueID)                          \
    X(controlId, m_controlID)                                 \
    X(deltaX3, m_deltaX_3)                                    \
    X(deltaY3, m_deltaY_3)                                    \
    X(oldDeltaX3, m_oldDeltaX_3)                              \
    X(oldDeltaY3, m_oldDeltaY_3)                              \
    X(delta3Related, m_Delta_3_Related)                       \
    X(unusedDouble, m_unkDoubleMaybeUnused)                   \
    X(actionType1, m_actionType1)                             \
    X(actionType2, m_actionType2)                             \
    X(actionValue1, m_actionValue1)                           \
    X(actionValue2, m_actionValue2)                           \
    X(interpRelatedFalse, m_someInterpValue1RelatedFalse)     \
    X(deltaTimeFloat, m_deltaTimeInFloat)                     \
    X(alreadyUpdated, m_alreadyUpdated)                       \
    X(doUpdate, m_doUpdate)

#define WORLD_CMD_TAIL(X)                                     \
    X(objectRotation, m_gameObjectRotation)                   \
    X(interpRelatedTrue, m_someInterpValue2RelatedTrue)       \
    X(unk204, m_unkInt204)

#ifdef GEODE_IS_WINDOWS // Windows layouts only (core/platform.hpp)
#define WORLD_CMD_OFFSET(field, member) \
    static_assert(offsetof(WCmd, field) == offsetof(GroupCommandObject2, member), "WCmd." #field);
WORLD_CMD_FIELDS(WORLD_CMD_OFFSET)
#undef WORLD_CMD_OFFSET
static_assert(offsetof(WCmd, moveOffsetX) == offsetof(GroupCommandObject2, m_moveOffset));
static_assert(offsetof(GroupCommandObject2, m_keyframes) == 0x1b8 && offsetof(GroupCommandObject2, m_gameObject) == 0x1d8 &&
              offsetof(GroupCommandObject2, m_remapKeys) == 0x1e8);
#endif

using gucci::fillFrom;  // core/platform.hpp

namespace {

Remap pushRemap(std::vector<int>& pool, const gd::vector<int>& keys) {
    Remap r;
    r.begin = (int)pool.size();
    r.count = (int)keys.size();
    pool.insert(pool.end(), keys.begin(), keys.end());
    return r;
}

void markUncertain(std::vector<uint16_t>& out, int group) {
    if (group > 0 && group < kGroupLimit) out.push_back((uint16_t)group);
}

// The imported value, held once with the last import of the same field when
// the two are the same: the many runs begun from one tick of the game, and
// kept starts in a stretch where nothing changes, share one copy.
template <class T>
void importShared(Cow<T>& out, T&& value, Cow<T>& last) {
    if (*last == value) {
        out.share(last);
    } else {
        out.assign(std::move(value));
        last.share(out);
    }
}

}  // namespace

WCmd importCommand(const GroupCommandObject2& c, const WorldDef* def, std::vector<int>& remaps) {
    WCmd w;
#define WORLD_CMD_COPY(field, member) w.field = static_cast<decltype(w.field)>(c.member);
    WORLD_CMD_FIELDS(WORLD_CMD_COPY)
    WORLD_CMD_TAIL(WORLD_CMD_COPY)
#undef WORLD_CMD_COPY
    w.moveOffsetX = c.m_moveOffset.x;
    w.moveOffsetY = c.m_moveOffset.y;
    w.splineX = c.m_splineRelated.x;
    w.splineY = c.m_splineRelated.y;
    w.keyframeCount = (int)c.m_keyframes.size();
    w.objectUid = c.m_gameObject ? c.m_gameObject->m_uniqueID : 0;
    w.objectSlot = def ? def->slotOf(c.m_gameObject) : -1;
    w.remap = pushRemap(remaps, c.m_remapKeys);
    return w;
}

bool commandMatches(const GroupCommandObject2& c, const WCmd& w, const std::vector<int>& remaps) {
    auto bits = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    bool same = true;
#define WORLD_CMD_CHECK(field, member) same = same && bits(w.field, static_cast<decltype(w.field)>(c.member));
    WORLD_CMD_FIELDS(WORLD_CMD_CHECK)
    WORLD_CMD_TAIL(WORLD_CMD_CHECK)
#undef WORLD_CMD_CHECK
    same = same && bits(w.moveOffsetX, c.m_moveOffset.x) && bits(w.moveOffsetY, c.m_moveOffset.y) &&
           bits(w.splineX, c.m_splineRelated.x) && bits(w.splineY, c.m_splineRelated.y) &&
           w.keyframeCount == (int)c.m_keyframes.size() && w.remap.count == (int)c.m_remapKeys.size() &&
           w.remap.begin >= 0 && (std::size_t)(w.remap.begin + w.remap.count) <= remaps.size();
    for (int i = 0; same && i < w.remap.count; i++) {
        same = remaps[(std::size_t)(w.remap.begin + i)] == c.m_remapKeys[(std::size_t)i];
    }
    return same;
}

void World::captureContacts(GJBaseGameLayer* pl, std::vector<std::pair<int, uint32_t>> (&out)[2]) {
    out[0].clear();
    out[1].clear();
    if (!pl) return;
    const GJGameState& gs = pl->m_gameState;
    // The contacts the real players are in: the entries the last real tick's
    // collision pass stamped (processStateTriggers has already dropped the
    // rest). A run's first tick then refuses a multi-activate portal the
    // player is standing in, as the game's next tick does, instead of taking
    // it again. The map is ordered by (object uid, player uid), so each list
    // comes out sorted by uid. Entries under player uid 0 are a touch trigger's
    // that either player fires once between them (m_isSinglePTouch +0x6dc,
    // playerTouchedTrigger 0x217f99), not one player's. Only
    // the stamps of the current command index count: between ticks nothing
    // else is left, and an entry from any other index (a state put back
    // without its map) says nothing about what the player touches now.
    PlayerObject* const players[2] = {pl->m_player1, pl->m_player2};
    const uint32_t now = gs.m_commandIndex;
    for (const auto& [key, stamp] : gs.m_activatedObjectIDs) {
        if (static_cast<uint32_t>(stamp) != now) continue;
        for (int k = 0; k < 2; k++) {
            if (players[k] && key.second != 0 && key.second == players[k]->m_uniqueID) {
                out[k].push_back({key.first, static_cast<uint32_t>(stamp)});
            }
        }
    }
}

WorldState World::captureLive(GJBaseGameLayer* pl, bool withActions) {
    WorldState ws;
    ws.seed = Bot::get()->replaySystem().m_teleportRandomState;
    // The game's own generator, which the random triggers and a spawn's delay
    // spread draw from (world/fire.cpp (aa)): the run takes a copy of the
    // global and moves that on, so it never writes the game and a branch puts
    // it back with the rest of the state.
    ws.randomSeed = off::at<uint64_t>(reinterpret_cast<void*>(geode::base::get()), off::kRandomState);
    if (!pl) return ws;
    const GJGameState& gs = pl->m_gameState;

    captureContacts(pl, ws.contact);

    // The tick's own values.
    ws.tick = 0;
    ws.commandIndex = gs.m_commandIndex;
    ws.timeMod = gs.m_timeModRelated;
    ws.timeMod2 = gs.m_timeModRelated2;
    ws.timeWarp = gs.m_timeWarp;
    ws.queuedTimeWarp = gs.m_queuedTimeWarp;
    ws.appliedTimeWarp = gs.m_timeWarpRelated;
    ws.channel = gs.m_currentChannel;
    // The options trigger's unlink-dual-gravity, which the copies' gravity
    // reads and 2899 changes (world/fire.cpp (ai)).
    ws.unlinkedDual = gs.m_unkBool31;
    // The level's points and running time, which the item triggers read and
    // write (world/fire.cpp (ae)).
    ws.points = gs.m_points;
    ws.levelTime = off::at<double>(pl, off::kLevelTime);
    fillFrom(ws.spawnCursor, gs.m_spawnChannelRelated0);
    std::sort(ws.spawnCursor.begin(), ws.spawnCursor.end());
    fillFrom(ws.goingBack, gs.m_spawnChannelRelated1);
    std::sort(ws.goingBack.begin(), ws.goingBack.end());
    ws.partial = !withActions;

    // What the last import held, field by field (see importShared).
    static Cow<std::vector<int>> lastRemaps;
    static Cow<std::vector<WCmd>> lastCmds;
    static Cow<std::vector<WSpawn>> lastSpawns;
    static Cow<ToggleWords> lastToggles;
    static Cow<std::vector<std::pair<int, int>>> lastTriggeredIds;
    static Cow<std::vector<std::pair<int, int>>> lastItems;
    static Cow<std::vector<std::pair<int, int>>> lastPersistent;
    static Cow<std::vector<WTimer>> lastTimers;
    static Cow<std::vector<WCountListener>> lastCounts;
    static Cow<std::vector<WCollisionListener>> lastCollisions;
    static Cow<std::vector<WTouchListener>> lastTouches;
    static Cow<std::vector<WTimerListener>> lastTimerListeners;
    static Cow<std::vector<WEventListener>> lastEvents;
    static Cow<std::vector<WEventStamp>> lastStamps;
    static Cow<std::vector<int>> lastPersistentTimers;
    static Cow<std::vector<WSequence>> lastSequences;

    std::vector<int> remaps;

    // Event listeners and the index each event last fired at (std::map: in
    // key order already).
    {
        std::vector<WEventListener> events;
        for (const auto& [key, list] : gs.m_unkMapPairGJGameEventIntVectorEventTriggerInstance) {
            for (const EventTriggerInstance& e : list) {
                WEventListener w;
                w.event = (int)key.first;
                w.key = key.second;
                w.targetGroup = e.m_targetID;
                w.triggerUid = e.m_uniqueID;
                w.controlId = e.m_controlID;
                w.inactive = e.m_inactive;
                w.remap = pushRemap(remaps, e.m_remapKeys);
                events.push_back(w);
            }
        }
        importShared(ws.eventListeners, std::move(events), lastEvents);
        std::vector<WEventStamp> stamps;
        for (const auto& [key, index] : gs.m_unkMapPairGJGameEventIntInt) {
            stamps.push_back(WEventStamp{(int)key.first, key.second, index});
        }
        importShared(ws.eventStamps, std::move(stamps), lastStamps);
    }

    const std::shared_ptr<const WorldDef> defHolder = WorldDef::get(pl);
    const WorldDef* def = defHolder.get();
    std::vector<uint16_t>& uncertain = ws.uncertainGroups;
    // Spawns whose triggers fire when the World cannot tell (a kept start's
    // queued spawns), as groups and as single triggers.
    std::vector<int> unknownSpawns;
    std::vector<int> unknownTriggers;

    // What the World does not port yet, running already: dynamic moves and
    // rotates, advanced follows and area effects. What each of them reaches is
    // worked out from the game's own code in world/tierd.cpp - an area effect
    // moves the members of one group, and only a level that targets objects
    // by key, or an object an effect that has gone has still to be put back,
    // leaves a run with nothing it can name.
    markLiveTierDE(pl, def, ws);

    // The game's counter of command uids (GroupCommandObject2::reset reads it
    // at 0x25770c and writes it back at 0x25784e, which World::init checks),
    // read only.
    ws.nextCommandUid = off::at<int>(reinterpret_cast<void*>(geode::base::get()), off::kCommandUidCounter);

    if (GJEffectManager* em = pl->m_effectManager) {
        // Running commands, the finished ones and the paused ones (m_disabled)
        // included. This skipped m_finished commands as erased before the next
        // step reads them, but only m_doUpdate ones are (postMoveActions erases
        // them within the step that set it): a command that finished is still
        // there on the next step, and its node makes moveObjects stamp the
        // last position once more (world/step.cpp (j)). A move without camera
        // locks runs in the World; the groups any other command moves, and the
        // centre it moves them about, are uncertain - and every command's are
        // for a kept start, which leaves the commands out.
        {
            std::vector<WCmd> cmds;
            for (const GroupCommandObject2& c : em->m_unkVector560) {
                if (c.m_doUpdate) continue;
                const bool ported = c.m_commandType == off::kCmdMove && !c.m_lockToCameraX && !c.m_lockToCameraY;
                if (!withActions || !ported) {
                    markUncertain(uncertain, c.m_targetGroupID);
                    markUncertain(uncertain, c.m_centerGroupID);
                }
                if (withActions) cmds.push_back(importCommand(c, def, remaps));
            }
            // A partial import leaves the shared last import alone, so the runs
            // begun around a kept start's capture still share theirs.
            if (withActions) importShared(ws.cmds, std::move(cmds), lastCmds);
        }

        if (!withActions && def) {
            // A kept start leaves the queued spawns out: what the triggers
            // they would spawn do is uncertain, down every spawn and toggle
            // those make in turn (walked below, once the marks are sorted).
            for (const SpawnTriggerAction& a : em->m_spawnTriggerActions) {
                if (a.m_gameObject) {
                    const int slot = def->slotOf(a.m_gameObject);
                    if (slot >= 0) unknownTriggers.push_back(slot);
                } else if (a.m_targetGroupID > 0) {
                    unknownSpawns.push_back(a.m_targetGroupID);
                }
            }
        }
        if (withActions) {
            std::vector<WSpawn> spawns;
            for (const SpawnTriggerAction& a : em->m_spawnTriggerActions) {
                WSpawn w;
                w.finished = a.m_finished;
                w.disabled = a.m_disabled;
                w.duration = a.m_duration;
                w.delta = a.m_deltaTime;
                w.targetGroup = a.m_targetGroupID;
                w.triggerUid = a.m_triggerUniqueID;
                w.controlId = a.m_controlID;
                w.ordered = a.m_spawnOrdered;
                w.objectUid = a.m_gameObject ? a.m_gameObject->m_uniqueID : 0;
                w.objectSlot = def ? def->slotOf(a.m_gameObject) : -1;
                w.remap = pushRemap(remaps, a.m_remapKeys);
                spawns.push_back(w);
            }
            importShared(ws.spawns, std::move(spawns), lastSpawns);
        }

        // The toggle bits, bits 0..9999, as the words toggleGroup reads
        // (0x223c05: the vector<bool>'s word array at EM +0x4f8, 32 bits a
        // word); the last word's unused bits stay clear.
        {
            const std::size_t bitCount = std::min<std::size_t>(em->m_unkVector438.size(), (std::size_t)kGroupLimit);
            const uint32_t* words = off::at<const uint32_t*>(em, off::kToggleBits);
            ToggleWords bits{};
            if (words) {
                for (std::size_t i = 0; i < (bitCount + 31) / 32; i++) {
                    const std::size_t used = std::min<std::size_t>(32, bitCount - i * 32);
                    bits[i] = used == 32 ? words[i] : (words[i] & ((uint32_t{1} << used) - 1));
                }
            }
            importShared(ws.toggleBits, std::move(bits), lastToggles);
        }

        {
            std::vector<std::pair<int, int>> ids;
            fillFrom(ids, em->m_unkMap498);
            importShared(ws.triggeredIds, std::move(ids), lastTriggeredIds);
            std::vector<std::pair<int, int>> items;
            fillFrom(items, em->m_itemCountMap);
            std::sort(items.begin(), items.end());
            importShared(ws.items, std::move(items), lastItems);
            std::vector<std::pair<int, int>> persistent;
            fillFrom(persistent, em->m_persistentItemCountMap);
            std::sort(persistent.begin(), persistent.end());
            importShared(ws.persistentItems, std::move(persistent), lastPersistent);
            // The items a persistent item trigger keeps a timer for (3641).
            std::vector<int> persistentTimers;
            fillFrom(persistentTimers, em->m_persistentTimerItemSet);
            std::sort(persistentTimers.begin(), persistentTimers.end());
            importShared(ws.persistentTimers, std::move(persistentTimers), lastPersistentTimers);
        }

        // Timers in the map's list order: updateTimers 0x2633b4 copies the
        // keys in that order and runs them so.
        if (withActions) {
            std::vector<WTimer> timers;
            for (const auto& [key, t] : em->m_timerItemMap) {
                WTimer w;
                w.key = key;
                w.itemId = t.m_itemID;
                w.time = t.m_time;
                w.active = t.m_active;
                w.timeMod = t.m_timeMod;
                w.ignoreTimeWarp = t.m_ignoreTimeWarp;
                w.targetTime = t.m_targetTime;
                w.stopTimeEnabled = t.m_stopTimeEnabled;
                w.targetGroup = t.m_targetGroupID;
                w.triggerUid = t.m_triggerUniqueID;
                w.controlId = t.m_controlID;
                w.remap = pushRemap(remaps, t.m_remapKeys);
                w.disabled = t.m_disabled;
                timers.push_back(w);
            }
            importShared(ws.timers, std::move(timers), lastTimers);
        }

        // Listeners looked up by item: in item order, each item's list as it
        // stands (updateCountForItem sorts it itself before walking it).
        std::vector<int> keys;
        {
            std::vector<WCountListener> counts;
            keys.reserve(em->m_countTriggerActions.size());
            for (const auto& [item, list] : em->m_countTriggerActions) keys.push_back(item);
            std::sort(keys.begin(), keys.end());
            for (int item : keys) {
                for (const CountTriggerAction& a : em->m_countTriggerActions.at(item)) {
                    WCountListener w;
                    w.item = item;
                    w.disabled = a.m_disabled;
                    w.previousCount = a.m_previousCount;
                    w.targetCount = a.m_targetCount;
                    w.targetGroup = a.m_targetGroupID;
                    w.activateGroup = a.m_activateGroup;
                    w.triggerUid = a.m_triggerUniqueID;
                    w.controlId = a.m_controlID;
                    w.itemId = a.m_itemID;
                    w.multiActivate = a.m_multiActivate;
                    w.remap = pushRemap(remaps, a.m_remapKeys);
                    counts.push_back(w);
                }
            }
            importShared(ws.countListeners, std::move(counts), lastCounts);
        }
        {
            std::vector<WTimerListener> timerListeners;
            keys.clear();
            for (const auto& [item, list] : em->m_unkMap3f8) keys.push_back(item);
            std::sort(keys.begin(), keys.end());
            for (int item : keys) {
                for (const TimerTriggerAction& a : em->m_unkMap3f8.at(item)) {
                    WTimerListener w;
                    w.item = item;
                    w.disabled = a.m_disabled;
                    w.time = a.m_time;
                    w.targetTime = a.m_targetTime;
                    w.targetGroup = a.m_targetGroupID;
                    w.triggerUid = a.m_triggerUniqueID;
                    w.controlId = a.m_controlID;
                    w.itemId = a.m_itemID;
                    w.multiActivate = a.m_multiActivate;
                    w.remap = pushRemap(remaps, a.m_remapKeys);
                    timerListeners.push_back(w);
                }
            }
            importShared(ws.timerListeners, std::move(timerListeners), lastTimerListeners);
        }
        {
            std::vector<WCollisionListener> collisions;
            for (const CollisionTriggerAction& a : em->m_unkVector230) {
                WCollisionListener w;
                w.disabled = a.m_disabled;
                w.blockA = a.m_blockAID;
                w.blockB = a.m_blockBID;
                w.targetGroup = a.m_targetGroupID;
                w.triggerOnExit = a.m_triggerOnExit;
                w.activateGroup = a.m_activateGroup;
                w.triggerUid = a.m_triggerUniqueID;
                w.controlId = a.m_controlID;
                w.remap = pushRemap(remaps, a.m_remapKeys);
                collisions.push_back(w);
            }
            importShared(ws.collisionListeners, std::move(collisions), lastCollisions);
        }
        {
            std::vector<WTouchListener> touches;
            for (const TouchToggleAction& a : em->m_unkVector1e0) {
                WTouchListener w;
                w.disabled = a.m_disabled;
                w.targetGroup = a.m_targetGroupID;
                w.holdMode = a.m_holdMode;
                w.touchType = (int)a.m_touchTriggerType;
                w.touchControl = (int)a.m_touchTriggerControl;
                w.triggerUid = a.m_triggerUniqueID;
                w.controlId = a.m_controlID;
                w.dualMode = a.m_dualMode;
                w.remap = pushRemap(remaps, a.m_remapKeys);
                touches.push_back(w);
            }
            importShared(ws.touchListeners, std::move(touches), lastTouches);
        }
    }
    importShared(ws.remaps, std::move(remaps), lastRemaps);

    // The sequence triggers' own state, which they keep on the object rather
    // than in the effect manager (world/fire.cpp (ab)): every key of both
    // maps, sorted by (slot, key). A key neither map holds is a sequence that
    // has never fired, which is what an empty entry stands for.
    if (def && !def->sequenceDefs.empty()) {
        std::vector<WSequence> sequences;
        for (int di : def->sequenceDefs) {
            const TriggerDef& d = def->triggers[(std::size_t)di];
            if (d.slot < 0 || (std::size_t)d.slot >= def->slots.size()) continue;
            // The cast is of a slot of one read of the level and cannot change
            // while that read stands, and this runs on every keyframe.
            static const WorldDef* castFor = nullptr;
            static std::vector<SequenceTriggerGameObject*> castCache;
            if (castFor != def) {
                castFor = def;
                castCache.assign(def->slots.size(), nullptr);
                for (int dj : def->sequenceDefs) {
                    const TriggerDef& dd = def->triggers[(std::size_t)dj];
                    if (dd.slot < 0 || (std::size_t)dd.slot >= def->slots.size()) continue;
                    castCache[(std::size_t)dd.slot] =
                        geode::cast::typeinfo_cast<SequenceTriggerGameObject*>(def->slots[(std::size_t)dd.slot]);
                }
            }
            auto* o = (std::size_t)d.slot < castCache.size() ? castCache[(std::size_t)d.slot] : nullptr;
            if (!o) continue;
            const auto& times = o->m_sequenceState.m_sequenceTimes;
            const auto& indices = o->m_sequenceState.m_sequenceIndices;
            for (const auto& [key, time] : times) {
                WSequence w;
                w.slot = d.slot;
                w.key = key;
                w.time = time;
                if (auto it = indices.find(key); it != indices.end()) w.index = it->second;
                sequences.push_back(w);
            }
            for (const auto& [key, index] : indices) {
                if (times.find(key) != times.end()) continue;
                WSequence w;
                w.slot = d.slot;
                w.key = key;
                // What the times map answers for a key it does not hold.
                w.time = -1.0f;
                w.index = index;
                sequences.push_back(w);
            }
        }
        std::sort(sequences.begin(), sequences.end(), [](const WSequence& a, const WSequence& b) {
            return std::pair<int, int>{a.slot, a.key} < std::pair<int, int>{b.slot, b.key};
        });
        importShared(ws.sequences, std::move(sequences), lastSequences);
    }

    // The listeners the World does not fire yet - a collision trigger's -
    // toggle or spawn their groups from a path no run follows: those groups
    // are uncertain. A count trigger's are fired (world/fire.cpp (u)), a touch
    // trigger's from the run's own button changes (world::onButton), a time
    // trigger's from the run's timers (world/fire.cpp (ad)) and an event
    // trigger's from the events the copies raise (world::onEvent).
    for (const WCollisionListener& l : *ws.collisionListeners) markUncertain(uncertain, l.targetGroup);
    // A state with no running actions (a kept start) has no timers: the
    // groups they and the triggers waiting on them would spawn are uncertain
    // for as long as it carries none.
    if (!withActions && pl->m_effectManager) {
        for (const auto& [key, t] : pl->m_effectManager->m_timerItemMap) markUncertain(uncertain, t.m_targetGroupID);
        for (const WTimerListener& l : *ws.timerListeners) markUncertain(uncertain, l.targetGroup);
    }

    std::sort(uncertain.begin(), uncertain.end());
    uncertain.erase(std::unique(uncertain.begin(), uncertain.end()), uncertain.end());

    // ... and so is everything the triggers in those groups do when they fire
    // (world/fire.cpp markSpawnUncertain; it keeps the marks sorted itself).
    // Marking the listener's group alone left the moves inside it looking
    // modelled. A group already walked is not walked again.
    if (def) {
        for (const WCollisionListener& l : *ws.collisionListeners) markSpawnUncertain(*def, ws, l.targetGroup);
        if (!withActions && pl->m_effectManager) {
            for (const auto& [key, t] : pl->m_effectManager->m_timerItemMap) {
                markSpawnUncertain(*def, ws, t.m_targetGroupID);
            }
            for (const WTimerListener& l : *ws.timerListeners) markSpawnUncertain(*def, ws, l.targetGroup);
        }
        for (int group : unknownSpawns) markSpawnUncertain(*def, ws, group);
        for (int slot : unknownTriggers) {
            const int di = def->defOfSlot[(std::size_t)slot];
            if (di < 0) continue;
            const TriggerDef& d = def->triggers[(std::size_t)di];
            markFireUncertain(*def, ws, d, d.target, d.center);
        }
    }
    return ws;
}

}  // namespace world

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
