// The World's per-tick step: the parts of GJBaseGameLayer::update that belong
// to the simulated tick rather than to one copy.
//
// FACT TABLE - GeometryDash.exe 2.2081 (Windows), read with llvm-objdump and
// capstone. Addresses are offsets from the image base. The step port (design
// step 5) and the spawn walk (step 7) are transcribed against this table.
//
// (a) The bool update passes to processCommands (the call at 0x238090) is
//     movzx r8d, byte [layer+0x3798]: whether this step is half of a split
//     step. update clears it at the top of every step (0x237d42) and sets it
//     only when m_clickBetweenSteps (+0x3799, game variable "0177") is on and
//     buttonIsRelevant 0x231e80 finds a queued button inside the step; the
//     step's deltas are then halved (0x237dfb-0x237e2a). processCommands
//     0x239c60 copies m_commandIndex to m_unkUint4 (+0x3ec), and when dt > 0
//     adds 1 to m_commandIndex for a half step and 2 otherwise (0x239cc6 /
//     0x239cf0), adds round(timeWarp * 1000) (halved for a half step) to
//     m_unkUint6 (+0x3f4), and - unless +0x324a is set - adds 1 or 2 to
//     m_currentProgress (+0x3e8, 0x239d14-0x239d26). The mod's PlayLayer::init
//     hook turns m_clickBetweenSteps off, so in a level the bot plays the flag
//     is always 0: the command index goes up by 2 per step, as Sim::step has it.
//
// (b) addToSection 0x226500: sx = (int)trunc((double)m_sectionXFactor *
//     m_positionX) - the double position, not the node's - with m_positionX
//     <= 0 giving 0 and m_positionX >= 1e7 giving (double)(factor * 1e7f);
//     sy the same with m_positionY and m_sectionYFactor. Both factors
//     are 0.01f (GJBaseGameLayer::init 0x206fc8): a section is 100 units, and
//     +0x36a0 / +0x36a4 are not widths. The outer vectors grow to sx + 1, the
//     inner to sy + 1 (their count and dirty vectors with them), and a null
//     bucket is allocated as an empty 0x18-byte vector (operator new 0x4d0770).
//     In a bucket: bucket[count] = object when count < size, push_back
//     otherwise; the object's slot = count; count++.
//
// (c) Every object goes into m_sections (+0x3598)[sx][sy] (counts +0x3640,
//     slot +0x270), with +0x278 / +0x27c = sx / sy. Then by class:
//     - objectID 1816 (collision block): extended (+0x280) -> the list +0x3600
//       (count +0x3618), otherwise m_collisionBlockSections +0x35c8[sx][sy]
//       (counts +0x3670); slot +0x274, no dirty bit.
//     - m_objectType 7 (decoration): nothing more.
//     - extended (+0x280) -> m_calcNonEffectObjects +0x35e0 (count +0x35f8),
//       slot +0x274.
//     - anything else -> m_nonEffectObjects +0x35b0[sx][sy] (counts +0x3658),
//       bit sy set in the uint32 words of +0x3688[sx] (bts, 0x226d23), slot
//       +0x274. These are the buckets checkCollisions sorts by m_uniqueID.
//
// (d) checkSpawnObjects 0x21a8f0 runs once per step, after both players'
//     collision passes (0x23861f), never per player:
//     - position: m_player1->m_isPlatformer ? posForTime((float)m_levelTime)
//       (vtable+0x4c0, PlayLayer::posForTime 0x3b3660) : m_player1's position.
//       Player 2 is never looked at.
//     - loop: list = m_spawnObjects->objectForKey(m_currentChannel) (an empty
//       array when there is none); i = m_spawnChannelRelated0[channel] and
//       back = m_spawnChannelRelated1[channel] (operator[]: both insert a
//       zero entry); i >= count -> applyTimeWarp (vtable+0x4f8) and return.
//     - an entry with m_isTouchTriggered (+0x5d0): not m_activated (+0x5b3)
//       -> return, the walk waits there until it is touched; activated ->
//       passed without firing.
//     - any other entry: the position test first, before any kind filter. A
//       platformer layer (+0x309e) fires when pos.x >= m_speedStart.x;
//       otherwise sideways (m_player1 +0x9c3): back ? start.y >= pos.y :
//       pos.y >= start.y, else back ? start.x >= pos.x : pos.x >= start.x.
//       Failing it returns.
//     - m_isGroupDisabled (+0x28e) -> passed without firing; otherwise a spawn
//       trigger (1268) gets updateRemapKeys with an empty list and every kind
//       fires through triggerObject (vtable+0x468)(layer, 0, nullptr).
//     - m_spawnChannelRelated0[channel]++ (0x21ab5a), and the loop goes on
//       from the top, where the channel and its list are read again. The
//       increment keys on the channel saved at the top of this iteration
//       (0x21a98b, a local), not one read after the fire: a rotate-gameplay
//       trigger that switches channel moves the old channel's cursor past
//       itself, and the walk carries on in the new channel from that one's
//       own cursor. The cursor moves past every entry it passes, fired or not.
//       Failing the position test, or running out of entries, calls
//       applyTimeWarp (vtable+0x4f8) before returning (0x21aa1f).
//
// (e) processMoveActions 0x22d5b0:
//     1. For each local offset in EM+0x7d8 ({CCMoveCNode*, CCPoint, object}):
//        angle = -(object+0x2a8) * 0.01745329f, the offset rotated by it and
//        scaled by (+0x2b8 + 1.0) and (+0x2bc + 1.0) in double, back to float;
//        the float difference from the old offset is added (as a double) to
//        the node's +0x38/+0x40 and +0x90/+0x98, +0xd1 = 0; then the vector is
//        emptied.
//     2. For each CCMoveCNode* in EM+0x790 with +0xd0 == 0: group = its
//        vtable+0x38 clamped to 0..9999; array = m_staticGroups[group] (made
//        and put into the +0xf08 dictionary when null: an allocation a run
//        must never make); dx = (float)node+0x38, dy = (float)node+0x40; when
//        the array exists and dx != 0 || dy != 0 || node+0x78:
//        moveObjects(array, (double)dx, (double)dy). Then the same for
//        m_optimizedGroups[group] (made into +0xf10) with (float)node+0x90 /
//        +0x98. So a step's move is rounded to float once, and the optimized
//        groups are moved too, by their own sums (the design said static only).
//     optimizeMoveGroups 0x230a20 only takes objects out of the optimized set:
//     from the group ids the triggers of the level name it calls
//     moveObjectToStaticGroup 0x231410, which clears +0x512 and adds the object
//     to the static groups, and it never sets the flag. +0x512 is set at
//     object setup (0x1a0a45-0x1a0a51) to m_objectType == 7 and nothing else,
//     and addToSection never puts type 7 into a collision bucket: no object a
//     copy can collide with is ever in m_optimizedGroups, so no group has to be
//     permanently Tier C for this.
//
// (f) moveObjects 0x22dd50: m_movedCount (+0x3724) += the array count first
//     (0x22ddb1, before the loop). Per object: when +0x512 == 0 and +0x4dc !=
//     m_commandIndex: m_lastPosition = (float)m_positionX / (float)m_positionY,
//     +0x4dc = m_commandIndex, word +0x368 = 0x101 (rect and oriented box
//     dirty); then m_positionX += dx when dx != 0 and +0x2c8 == 0, m_positionY
//     += dy when dy != 0; word +0x351 = 0x101 (position dirty); vtable+0x490 =
//     updateObjectSection 0x227f50. After the loop vtable+0x498 =
//     updateDisabledObjectsLastPos(CCArray*): a bare ret (0x3be80) in both the
//     GJBaseGameLayer and the PlayLayer vtables; only LevelEditorLayer's
//     (0x2dfbe0) does anything, stamping the marker of optimized objects. A run
//     in a level never needs it.
//
// (g) The step's clock (not in the design's table; read while checking (a)).
//     At the top of each step (0x237e9f), before updateSpawnTriggers, the
//     parked speed and processCommands: m_totalTime (+0x3c8) += the step's dt
//     and m_unkDouble3 (+0x3d8) += dt / timeWarp. m_currentProgress and
//     m_unkUint6 move in processCommands (a). m_levelTime (+0x3d0) += dt /
//     timeWarp after both collision passes, just before checkSpawnObjects
//     (0x2385f9). m_unkUint5 (+0x3f0) catches up with m_unkUint6 once it is
//     1000 behind (0x2386fc). The copies keep the mod's own clock - one
//     m_currentProgress and round(warp * 1000) of m_unkUint5 per tick - because
//     the ring contact rule and the tick counts are built on one per tick and
//     nothing a copy runs reads the rest; with the World on it runs once per
//     simulated tick instead of once per copy.
//
// (h) The order of a step (update 0x237e9f-0x2383a8): the clock; with no half
//     step, m_spawnTuples (+0x10b8) and m_destroyObjectValues (+0x3010) are
//     emptied; updateSpawnTriggers(EM, dt) 0x238011; the parked speed to
//     m_player1 and, in the dual part (+0x422), m_player2; applyTimeWarp
//     (vtable+0x4f8); processCommands 0x238090, which moves the command index
//     and then calls processQueuedButtons 0x239d51 - the tick's input comes
//     after the index, the parked speed and the queued spawns, not before
//     them as the design had it; resetTouchedRings; the lock inputs
//     0x238208-0x238344 (below); updateTimers 0x238360 unless m_playerDied
//     (+0x324a); prepareMoveActions(EM, dt, !lastStep) 0x238378;
//     processMoveActionsStep(layer, dt, lastStep) 0x238388; postMoveActions
//     0x238394; vtable+0x410 (PlayLayer::updateVerifyDamage, nothing a run
//     needs); updateCollisionBlocks 0x2383a8. lastStep (r15b, 0x237e42) is
//     true only on the last step of an update call: the bot's updater runs
//     many steps per call, so the real game's frame grouping is not a thing a
//     run can know. A simulated tick counts as a last step; see (j) for the
//     only thing that changes.
//
// (i) GroupCommandObject2::step 0x257900(this, float dt): nothing while
//     m_disabled (+0x71); m_deltaTime (+0x20) = (double)dt + m_deltaTime even
//     once m_finished (+0x70); then nothing more when finished. m_alreadyUpdated
//     (+0x1b0, which reset 0x257845 sets) is cleared instead of adding dt to
//     m_deltaTimeInFloat (+0x1ac) once. Without an action (+0x190 == 0) the
//     command finishes when m_deltaTime >= m_duration unless the duration is
//     -1.0. Otherwise updateAction(+0x190, (float)+0x198), updateAction(+0x194,
//     (float)+0x1a0) when that is set, and it finishes when m_deltaTimeInFloat
//     > 0 and (double)m_deltaTimeInFloat >= m_duration.
//     updateAction 0x2579d0(this, int type, float value): t = (double)
//     m_deltaTimeInFloat / maxsd(m_duration, 1.1920928955078125e-07), clamped
//     to [0, 1] (a NaN stays); eased = GameToolbox::getEasedValue((float)t,
//     m_easingType, (float)m_easingRate) (0x68b70, pure: math calls only);
//     v = value - (1.0f - eased) * value in float. Type 1 unless m_lockedInX
//     (+0x77): d = (double)v - m_currentXOffset; m_currentXOffset = v;
//     m_deltaX (+0x40) += d; m_deltaX_3 (+0x160) += d. Type 2 the same with
//     +0x78 / +0x38 / +0x48 / +0x168. Types 3 and 4 on +0x90 / +0x98.
//     Checked offline as well: stepCommandTranscribed below, compiled on its
//     own with clang-cl, against 0x257900 run from a private copy of the
//     image (only the CRT math imports resolved) - 36,960 commands over 22
//     easing types, 7 rates (0 and -1 among them), 8 durations (-1 and 1e-9),
//     5 step lengths (0 among them) and 6 action shapes, 129.5 million steps,
//     every field the same bit for bit.
//
// (j) prepareMoveActions 0x25f3f0(EM, float dt, bool intermediate): the move
//     nodes of the last step are put back in their pool with their sums and
//     flags cleared (0x2606b0) and EM +0x790 is emptied. Then for each command
//     in order (the size read again every time), a paused one is skipped
//     whole; m_commandType (+0xd0) picks the case. Move (0x25f5f5):
//     getMoveCommandNode 0x25efd0 finds or makes the node of the target group
//     (a new one is appended to EM +0x790: node order is first-use order).
//     With m_finishRelated (+0x72): node +0x90 += m_deltaX_3, +0x98 +=
//     m_deltaY_3, node +0x78 = 1 (moveObjects runs even for a zero move).
//     Otherwise step(dt); x = m_deltaX, y = m_deltaY, x3 / y3 = intermediate ?
//     0 : m_deltaX_3 / m_deltaY_3; a player lock X (+0x73) makes x = (double)
//     EM+0x800 * m_moveModX, a camera lock X (+0x75) the same with EM+0x808,
//     and adds x to x3 (to m_deltaX_3 itself when intermediate); the same for
//     Y with +0x74 / EM+0x804 and +0x76 / EM+0x80c (the camera Y lock rounds
//     through float first, 0x25f6f4, then adds the same way, addsd 0x25f6ff -
//     camera locks are not ported). A player Y lock also sets node +0x1c = 13,
//     which processMoveActions does not read. m_lockedCurrentX/Y (+0x60/+0x68) += x / y, node +0x38/+0x40
//     += x / y, node +0x90/+0x98 += x3 / y3. Every command that was not
//     skipped then ends with (0x260586): m_oldDeltaX/Y = m_deltaX/Y,
//     m_oldDeltaX_3/Y_3 = m_deltaX_3/Y_3, m_deltaX/Y and +0x98 = 0; unless
//     intermediate (and always after the m_finishRelated path) m_deltaX_3/Y_3
//     and +0x180 = 0; m_finishRelated ? m_doUpdate (+0x1b1) = 1 : m_finished ?
//     m_finishRelated = 1. A command that finished is therefore still there
//     on the next step - its node flag makes moveObjects stamp the last
//     position once more - and only then erased; world/capture.cpp took it
//     for gone. The optimized sums are the only thing intermediate changes,
//     and the optimized groups hold decoration alone (fact (e)).
//
// (k) postMoveActions 0x260800: every command with m_doUpdate is erased (in
//     place); a move or follow one first adds its current (or locked) offset
//     to EM +0x620, the offsets a level save stores - nothing a run reads.
//
// (l) updateSpawnTriggers 0x261da0(EM, float dt): over the queue's size at
//     the start (entries a spawn appends wait for the next step): a finished
//     entry fires; a paused one is skipped; otherwise m_deltaTime = (double)dt
//     + m_deltaTime and it fires once m_deltaTime >= m_duration. A fire copies
//     the remap keys and calls, through the layer's trigger delegate (+0x198),
//     spawnObject(m_gameObject, delta - duration, keys) when the entry names an
//     object, spawnGroup(group, ordered, delta - duration, keys, uid, control)
//     otherwise. Then every finished entry is erased, the rest keeping their
//     order.
//
// (m) The lock inputs (0x238208-0x238344), taken from m_player1 before its
//     update: EM+0x800 = getPosition().x - m_lastPosition.x; EM+0x804 the same
//     in y, zeroed when |dy - m_yVelocityRelated3 (+0xab4)| > dt * 16; both
//     zero while m_playerDied. EM+0x808/+0x80c are the camera's (not ported),
//     EM+0x810/+0x814 the players' rotation change (rotate triggers).

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/world/def.hpp"
#include "absense/world/world.hpp"

namespace world {

void applyParkedSpeed(GJBaseGameLayer* pl, PlayerObject* const* copies, int count) {
    const float speed = pl->m_gameState.m_timeModRelated;
    if (speed == 0.0f) return;
    pl->m_gameState.m_timeModRelated = 0;
    pl->m_gameState.m_timeModRelated2 = false;
    // The game passes the parked m_timeModRelated2 (0x238026); it only decides
    // whether the speed change plays its effect (updateTimeMod 0x3a0d89), which
    // a copy never shows.
    for (int i = 0; i < count; i++) {
        if (copies[i]) copies[i]->updateTimeMod(speed, true);
    }
}

float stepTimeWarp() {
    // A run with the World runs at the warp its own state has: a time warp
    // trigger it fired changed it for the run and for nobody else (world/
    // fire.cpp (ah)). Every other run, and the game itself, keep the live one.
    if (const WorldState* ws = current()) return ws->timeWarp;
    return Bot::get()->updater().getTimeWarp();
}

float stepDt() {
    // One game step is getModifiedDelta long: physicsDt * min(timeWarp, 1).
    return Bot::get()->updater().getPhysicsDt() * std::min(stepTimeWarp(), 1.0f);
}

void advanceClock(GJBaseGameLayer* pl) {
    const float timeWarp = stepTimeWarp();
    const float physicsDt = stepDt();
    pl->m_gameState.m_totalTime += physicsDt;
    pl->m_gameState.m_unkDouble3 += physicsDt / timeWarp;
    pl->m_gameState.m_currentProgress++;
    pl->m_gameState.m_unkUint5 += (int)roundf(timeWarp * 1000.0f);
}

// ------------------------------------------------------------ commands

bool g_commandStepThroughGame = false;

WCmd freshCommand(int uid) {
    // GroupCommandObject2::reset 0x257700: everything zero but these.
    WCmd c;
    c.uid = uid;
    c.modX = 1.0;
    c.modY = 1.0;
    c.followXMod = 1.0;
    c.followYMod = 1.0;
    c.interp1 = 1.0;
    c.interp2 = 1.0;
    c.alreadyUpdated = true;  // the word 1 at +0x1b0: m_alreadyUpdated set, m_doUpdate clear
    return c;
}

namespace {

// GroupCommandObject2::updateAction 0x2579d0, fact (i). Every operation is the
// binary's, in its order and precision.
void updateAction(WCmd& c, int type, float value) {
    const double span = c.duration > 1.1920928955078125e-07 ? c.duration : 1.1920928955078125e-07;  // maxsd
    const double t0 = (double)c.deltaTimeFloat / span;
    const double t = t0 > 1.0 ? 1.0 : (0.0 > t0 ? 0.0 : t0);
    const float eased = GameToolbox::getEasedValue((float)t, c.easingType, (float)c.easingRate);
    const float v = value - (1.0f - eased) * value;
    switch (type) {
        case 1:
            if (c.lockedInX) return;
            {
                const double now = (double)v;
                const double d = now - c.currentX;
                c.currentX = now;
                c.deltaX = d + c.deltaX;
                c.deltaX3 = d + c.deltaX3;
            }
            return;
        case 2:
            if (c.lockedInY) return;
            {
                const double now = (double)v;
                const double d = now - c.currentY;
                c.currentY = now;
                c.deltaY = d + c.deltaY;
                c.deltaY3 = d + c.deltaY3;
            }
            return;
        case 3:
        case 4: {
            const double now = (double)v;
            const double d = now - c.rotateValue;
            c.rotateValue = now;
            c.rotateDelta = d + c.rotateDelta;
            return;
        }
        default:
            return;
    }
}

}  // namespace

void stepCommandTranscribed(WCmd& c, float dt) {
    // GroupCommandObject2::step 0x257900, fact (i).
    if (c.disabled) return;
    const double total = (double)dt + c.deltaTime;
    c.deltaTime = total;
    if (c.finished) return;
    if (c.alreadyUpdated) {
        c.alreadyUpdated = false;
    } else {
        c.deltaTimeFloat = dt + c.deltaTimeFloat;
    }
    if (c.actionType1 == 0) {
        if (!(total >= c.duration)) return;  // comisd + jb: a NaN waits too
        if (c.duration == -1.0) return;
        c.finished = true;
        return;
    }
    updateAction(c, c.actionType1, (float)c.actionValue1);
    if (c.actionType2 != 0) updateAction(c, c.actionType2, (float)c.actionValue2);
    if (c.deltaTimeFloat > 0.0f && (double)c.deltaTimeFloat >= c.duration) c.finished = true;
}

namespace {
// The game's own step runs on this: a GroupCommandObject2 the mod owns, never
// constructed (the constructor would bump the game's uid counter), its
// containers left as all-zero, which is an empty MSVC vector. step and
// updateAction only read and write the fields below +0x1b2.
constexpr std::size_t kCommandStepBytes = 0x1b2;
struct ScratchCommand {
    alignas(16) unsigned char bytes[0x208];
};
}  // namespace

void stepCommandThroughGame(WCmd& c, float dt) {
    static ScratchCommand scratch{};
    // WCmd's fields up to +0x1b1 sit at the offsets of the command's (pinned
    // in world/capture.cpp), so the bytes go over as they are.
    std::memcpy(scratch.bytes, static_cast<void*>(&c), kCommandStepBytes);
    reinterpret_cast<GroupCommandObject2*>(scratch.bytes)->step(dt);
    std::memcpy(static_cast<void*>(&c), scratch.bytes, kCommandStepBytes);
}

void stepCommand(WCmd& c, float dt) {
    if (g_commandStepThroughGame) stepCommandThroughGame(c, dt);
    else stepCommandTranscribed(c, dt);
}

// ------------------------------------------------------------ helpers

void markUncertain(WorldState& ws, int group) {
    if (group <= 0) return;
    const uint16_t g = (uint16_t)WorldDef::clampGroup(group);
    auto it = std::lower_bound(ws.uncertainGroups.begin(), ws.uncertainGroups.end(), g);
    if (it == ws.uncertainGroups.end() || *it != g) ws.uncertainGroups.insert(it, g);
}

void emitOp(Run& run, const Op& op) {
    if (!run.log) return;
    run.log->append(op);
    WorldState& ws = *run.ws;
    // Under the group the log keeps it under (append clamps it the game's way).
    const uint16_t g = (uint16_t)WorldDef::clampGroup(op.group);
    auto it = std::lower_bound(ws.reach.begin(), ws.reach.end(), g,
                               [](const std::pair<uint16_t, GroupReach>& e, uint16_t key) { return e.first < key; });
    if (it == ws.reach.end() || it->first != g) it = ws.reach.insert(it, {g, GroupReach{}});
    GroupReach& r = it->second;
    r.ops++;
    switch (op.kind) {
        case OpKind::Translate:
        case OpKind::TranslateOptimized:
        case OpKind::Place:
            // What moveObjects and the silent move add: the float step as a double.
            r.x += std::fabs((double)op.dx);
            r.y += std::fabs((double)op.dy);
            break;
        default:
            break;
    }
}

std::vector<int> keysOf(const WorldState& ws, const Remap& remap) {
    const std::vector<int>& pool = *ws.remaps;
    if (remap.count <= 0 || remap.begin < 0 || (std::size_t)(remap.begin + remap.count) > pool.size()) return {};
    return std::vector<int>(pool.begin() + remap.begin, pool.begin() + remap.begin + remap.count);
}

Remap pushKeys(WorldState& ws, std::span<const int> keys) {
    Remap r;
    if (keys.empty()) return r;
    std::vector<int>& pool = ws.remaps.edit();
    r.begin = (int)pool.size();
    r.count = (int)keys.size();
    pool.insert(pool.end(), keys.begin(), keys.end());
    return r;
}

bool idle(const WorldState& ws) {
    return ws.cmds->empty() && ws.spawns->empty() && ws.timers->empty() && ws.queuedTimeWarp <= 0.0f;
}

// ------------------------------------------------------------ the tick

namespace {

// updateSpawnTriggers 0x261da0, fact (l).
void updateSpawnTriggers(Run& run, float dt) {
    WorldState& ws = *run.ws;
    const std::size_t n = ws.spawns->size();
    if (n == 0) return;
    for (std::size_t i = 0; i < n; i++) {
        // Read again every time: a fire can append to the queue, or a stop
        // it fires erase from it (the game walks the moved entries too).
        if (i >= ws.spawns->size()) break;
        // A paused entry is passed without a write: the queue a snapshot
        // shares is only copied when an entry really moves on.
        if (const WSpawn& peek = (*ws.spawns)[i]; !peek.finished && peek.disabled) continue;
        {
            WSpawn& a = ws.spawns.edit()[i];
            if (!a.finished) {
                if (a.disabled) continue;
                a.delta = (double)dt + a.delta;
                a.finished = a.delta >= a.duration;
                if (!a.finished) continue;
            }
        }
        const WSpawn a = (*ws.spawns)[i];
        const double remain = a.delta - a.duration;
        const std::vector<int> keys = keysOf(ws, a.remap);
        if (a.objectUid != 0) {
            if (a.objectSlot >= 0) {
                spawnObject(run, a.objectSlot, remain, keys);
            }
            // (An object the level read does not hold: nothing it does is known.)
            else {
                ws.uncertainAny = true;
            }
        } else {
            spawnGroup(run, a.targetGroup, a.ordered, remain, keys, a.triggerUid, a.controlId);
        }
    }
    if (std::any_of(ws.spawns->begin(), ws.spawns->end(), [](const WSpawn& s) { return s.finished; })) {
        std::erase_if(ws.spawns.edit(), [](const WSpawn& s) { return s.finished; });
    }
}

// A move node of one step: the sums getMoveCommandNode's node gathers (fact (j)).
struct MoveNode {
    int group = 0;
    double sx = 0.0, sy = 0.0;  // node +0x38 / +0x40: the static groups
    double ox = 0.0, oy = 0.0;  // node +0x90 / +0x98: the optimized groups
    bool force = false;         // node +0x78
};

MoveNode& nodeFor(std::vector<MoveNode>& nodes, int group) {
    for (MoveNode& n : nodes) {
        if (n.group == group) return n;
    }
    nodes.push_back(MoveNode{group});
    return nodes.back();
}

// The loop tail of prepareMoveActions (0x260586), fact (j).
void commandTail(WCmd& c, bool keepThirds) {
    c.oldDeltaX = c.deltaX;
    c.oldDeltaY = c.deltaY;
    c.oldDeltaX3 = c.deltaX3;
    c.oldDeltaY3 = c.deltaY3;
    c.deltaX = 0.0;
    c.deltaY = 0.0;
    c.rotateDelta = 0.0;
    if (!keepThirds) {
        c.deltaX3 = 0.0;
        c.deltaY3 = 0.0;
        c.delta3Related = 0.0;
    }
    if (c.finishRelated) c.doUpdate = true;
    else if (c.finished) c.finishRelated = true;
}

}  // namespace

void stepTickBegin(Run& run, float dt, PlayerObject* const* copies, int count) {
    WorldState& ws = *run.ws;
    // (h): the set spawnGroup keeps for a step is emptied at its top.
    run.spawnTuples.clear();
    if (run.log) run.log->beginTick((uint32_t)ws.tick);
    // The one option of 2899 the copies read is a field of the game (world/
    // fire.cpp (ai)). The run carries it in its own state and puts it back at
    // the top of every tick, so a branch that fired the trigger and was thrown
    // away cannot leave it behind; Sim::end restores the game state in any case.
    if (run.pl) run.pl->m_gameState.m_unkBool31 = ws.unlinkedDual;
    // The level's running time, at the top of the step as update 0x237ed9 has
    // it, and only while the level has not ended and the player is alive. The
    // run keeps it rather than writing the layer's own (world/fire.cpp (ae)
    // reads it as an item). The step works the delta out once and adds the
    // same one to m_unkDouble3 and to this (0x237eb7 and 0x237edd), so it is
    // worked out here exactly as world::advanceClock works the clock's out -
    // a float divide - and the two never drift apart.
    if (!ws.levelEnd && !run.player1Dead && ws.timeWarp != 0.0f) {
        ws.levelTime += (double)(dt / ws.timeWarp);
    }
    updateSpawnTriggers(run, dt);
    if (run.pl) {
        applyParkedSpeed(run.pl, copies, count);
        ws.timeMod = run.pl->m_gameState.m_timeModRelated;
        ws.timeMod2 = run.pl->m_gameState.m_timeModRelated2;
    }
}

void stepTick(Run& run, float dt) {
    WorldState& ws = *run.ws;
    // processCommands has just moved the layer's index: the run copies it
    // into ws.commandIndex before this (so the check in world/selftest.cpp can
    // step a state with no layer at all).

    // The lock inputs, fact (m), from the copy of player 1 - or, for a step
    // that has no copies at all (the ledger replaying a real tick), the values
    // the real game worked out on that tick. Worked out here, before the
    // timers, because that is where the step has them (0x2381da-0x238344, with
    // updateTimers only at 0x238360): a group a timer spawns can teleport the
    // copy, and the tick's locks are the ones from before it did.
    float lockDx = 0.0f;
    float lockDy = 0.0f;
    if (run.lockRecorded) {
        lockDx = run.lockDx;
        lockDy = run.lockDy;
    } else if (PlayerObject* p1 = run.player1) {
        const cocos2d::CCPoint pos = p1->getPosition();
        float dy = pos.y - p1->m_lastPosition.y;
        if ((double)std::fabs(dy - p1->m_yVelocityRelated3) > (double)dt * 16.0) dy = 0.0f;
        lockDy = dy;
        lockDx = pos.x - p1->m_lastPosition.x;
    }

    // updateTimers, fact (h): before the move step, so a command a timer's
    // spawn starts is stepped on this tick. The game skips it while
    // m_playerDied is set - a run whose copy of player 1 has died has ended,
    // so a simulated tick always runs it.
    updateTimers(run, dt);
    // The idle fast path: no command to step, no node, no op. Paused commands
    // are skipped whole (0x25f5cf), and the ones not ported stay as they are:
    // neither writes, so the commands a snapshot shares are not copied for them.
    bool anyMove = false;
    for (const WCmd& c : *ws.cmds) {
        if (c.disabled) continue;
        if (c.commandType != 0) {
            // Rotate, follow, follow player Y, transform and keyframe commands
            // are not ported: they are left as they are, and what they move
            // is uncertain.
            markUncertain(ws, c.targetGroup);
            markUncertain(ws, c.centerGroup);
            continue;
        }
        anyMove = true;
    }
    if (!anyMove) return;

    // prepareMoveActions, fact (j): a simulated tick is a last step, so the
    // optimized sums go out on every tick (intermediate is false).
    constexpr bool intermediate = false;
    // The step's nodes, in a list kept from tick to tick: this runs for every
    // tick of the hundreds of runs a decision makes, and a vector made here
    // was an allocation each time. stepTick never runs inside itself.
    static std::vector<MoveNode> nodes;
    nodes.clear();
    std::vector<WCmd>& cmds = ws.cmds.edit();
    for (std::size_t i = 0; i < cmds.size(); i++) {
        WCmd& c = cmds[i];
        if (c.disabled || c.commandType != 0) continue;  // (marked above)
        MoveNode& node = nodeFor(nodes, c.targetGroup);
        if (c.finishRelated) {
            node.ox = c.deltaX3 + node.ox;
            node.oy = c.deltaY3 + node.oy;
            node.force = true;
            commandTail(c, false);
            continue;
        }
        stepCommand(c, dt);
        double x = c.deltaX;
        double y = c.deltaY;
        double x3 = intermediate ? 0.0 : c.deltaX3;
        double y3 = intermediate ? 0.0 : c.deltaY3;
        if (c.lockPlayerX || c.lockCameraX) {
            float input = lockDx;
            if (!c.lockPlayerX) {
                // Locked to the camera (Tier C): its motion is not modelled.
                input = 0.0f;
                markUncertain(ws, c.targetGroup);
            } else if (!run.player1) {
                markUncertain(ws, c.targetGroup);
            }
            x = (double)input * c.modX;
            if (!intermediate) x3 = x3 + x;
            else c.deltaX3 = x + c.deltaX3;
        }
        if (c.lockPlayerY) {
            if (!run.player1) markUncertain(ws, c.targetGroup);
            y = (double)lockDy * c.modY;
            if (!intermediate) y3 = y3 + y;
            else c.deltaY3 = y + c.deltaY3;
        } else if (c.lockCameraY) {
            // The camera input is not modelled: taken as zero. The game rounds
            // this one through float and then adds it like the others, to the
            // third sum (addsd 0x25f6ff) or to m_deltaY_3 when intermediate;
            // the add was left out here (and called an addps), which a NaN
            // mod tells apart.
            markUncertain(ws, c.targetGroup);
            y = (double)(float)(0.0 * c.modY);
            if (!intermediate) y3 = y3 + y;
            else c.deltaY3 = y + c.deltaY3;
        }
        c.lockedCurrentX = x + c.lockedCurrentX;
        c.lockedCurrentY = y + c.lockedCurrentY;
        node.sx = x + node.sx;
        node.sy = y + node.sy;
        node.ox = x3 + node.ox;
        node.oy = y3 + node.oy;
        commandTail(c, intermediate);
    }

    // processMoveActionsStep 0x22b5d0 -> processMoveActions 0x22d5b0, fact (e):
    // each node in the order it was made moves its static groups by its
    // sums rounded to float once, then its optimized groups by theirs. The
    // local-offset pre-loop only has entries from rotate commands, which
    // are not ported (their groups are uncertain already).
    if (run.log && run.def) {
        for (const MoveNode& n : nodes) {
            const int group = (int)WorldDef::clampGroup(n.group);
            const float fx = (float)n.sx;
            const float fy = (float)n.sy;
            if (!run.def->staticMembers(group).empty() && (fx != 0.0f || fy != 0.0f || n.force)) {
                Op op;
                op.tick = (uint32_t)ws.tick;
                op.commandIndex = ws.commandIndex;
                op.kind = OpKind::Translate;
                op.group = group;
                op.dx = fx;
                op.dy = fy;
                emitOp(run, op);
            }
            const float ox = (float)n.ox;
            const float oy = (float)n.oy;
            if (!run.def->optimizedMembers(group).empty() && (ox != 0.0f || oy != 0.0f || n.force)) {
                Op op;
                op.tick = (uint32_t)ws.tick;
                op.commandIndex = ws.commandIndex;
                op.kind = OpKind::TranslateOptimized;
                op.group = group;
                op.dx = ox;
                op.dy = oy;
                emitOp(run, op);
            }
        }
    }

    // postMoveActions 0x260800, fact (k).
    std::erase_if(cmds, [](const WCmd& c) { return c.doUpdate; });
}

void endTick(Run& run) {
    WorldState& ws = *run.ws;
    // A queued time warp, fact (ah): the game applies it at the end of an
    // update (0x238bb0), which is the end of a simulated tick here - the bot's
    // updater runs the steps itself, so a tick is the smallest thing a run has
    // to hang it on. updateTimeWarp 0x236150 clamps it to 2 and then to at
    // least 0.1; applyTimeWarp only moves m_timeWarpRelated and the audio.
    if (ws.queuedTimeWarp > 0.0f) {
        float warp = std::min(ws.queuedTimeWarp, 2.0f);
        if (0.1f > warp) warp = 0.1f;
        ws.queuedTimeWarp = 0.0f;
        ws.timeWarp = warp;
        // The design read updateTimeWarp's own tail call to applyTimeWarp as
        // "when the warp is exactly 1". The constant it compares against at
        // 0x236176 is 64, which the clamp above can never reach, so that call
        // is dead; the caller makes the call itself, right after and with the
        // warp that was just written (0x238bd8). So it always follows.
        ws.appliedTimeWarp = warp;
    }
    // GJBaseGameLayer::update 0x238990: the contacts no copy touched on this
    // tick are forgotten.
    ageContacts(ws, ws.commandIndex);
    if (run.pl) {
        ws.timeMod = run.pl->m_gameState.m_timeModRelated;
        ws.timeMod2 = run.pl->m_gameState.m_timeModRelated2;
    }
    ws.tick++;
}

}  // namespace world
