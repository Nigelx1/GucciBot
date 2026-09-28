// The real game's ledger (trigger design step 10, see world/ledger.hpp).
//
// FACTS CHECKED IN GeometryDash.exe 2.2081 (llvm-objdump / capstone) WHILE
// WRITING THIS:
//
// (r1) Every direct call in the image to the six functions the design names:
//      moveObjects 0x22dd50 from 0x22d8c6 and 0x22d957 (processMoveActions
//      0x22d5b0) and from 0x230348, 0x23039f, 0x2304b3, 0x2304da; rotateObject
//      0x22c100 from 0x22c5b5 and 0x2303d4; rotateObjects 0x22c1a0 from
//      0x23046e; moveAreaObject 0x22aab0 from 0x229a9a, 0x22a3e5, 0x22a904 and
//      0x22a9ab; transformAreaObjects 0x229720 from 0x229563 (processAreaActions);
//      toggleGroup 0x223bc0 from 0x223b77 (toggleGroupTriggered) and 0x4a6545
//      (EffectGameObject::triggerObject). So both ways a group is toggled and
//      the whole area-effect path go through a hook.
// (r2) The design assumed those six are the only writers. They are not:
//      processDynamicObjectActions 0x22e3b0 and the transform step 0x22c680
//      call none of them, and moveObjectsSilent is inlined into
//      triggerMoveCommand on Windows. The binary wins, so the ledger does not
//      claim to follow a tick on which one of those could have run: coveredNow
//      below turns pose coverage off for the tick instead, and a start that
//      needs an uncovered tick moves its objects the old way (MovingObjects).
// (r3) frameUpdateMidhook sits at 0x238bAA, past the step at 0x237e9f-0x2383a8:
//      every move of a tick has already happened when afterRealTick runs, and
//      the frame it is given is the number of the tick that just ran. The
//      hooks therefore stamp what they see with that number + 1, kept in
//      Ring::tick.

#include "absense/world/ledger.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <span>
#include <type_traits>
#include <utility>

#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/bot.hpp"
#include "absense/compat/devlog.hpp"
#include "absense/trajectory/trajectory.hpp"  // moverCacheGeneration
#include "absense/world/def.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

namespace world {

namespace {

// One object's pose before the first change the real game made to it on a
// tick. Trivially copyable, so the ring can be moved about with memcpy.
struct Entry {
    int32_t tick = 0;
    int32_t slot = -1;
    Pose before;
};
static_assert(std::is_trivially_copyable_v<Entry>);

// How many entries the 16 MB cap leaves room for.
inline constexpr std::size_t kMaxEntries = kLedgerBytes / sizeof(Entry);
// The ring starts here and doubles up to the cap: a quiet level never pays
// for the whole of it.
inline constexpr std::size_t kFirstEntries = 4096;
// Real ticks whose counters are kept. The pathfinder keeps a start for
// (2 * kBackSeconds + kLookahead) seconds of them, which is 2464 ticks at 240
// TPS and more when the game is driven faster; past this the ring has long
// since wrapped anyway.
inline constexpr std::size_t kMaxMarks = 65536;
// Keyframes the ledger itself holds on to. A start keeps the one it uses
// alive by itself, so this only has to cover the newest and the one the
// shadow check compares with.
inline constexpr std::size_t kKeepKeyframes = 4;

// The whole World state of one real tick, and the inputs of the ticks after
// it. Nothing fires while a keyframe is the newest one (anything that does
// changes the shape and takes a keyframe of its own), so stepping it with
// those inputs only advances what was already running.
struct Keyframe {
    int tick = 0;
    std::shared_ptr<const WorldState> ws;
    std::shared_ptr<std::vector<LockInput>> inputs;
    std::shared_ptr<const WorldDef> def;
    uint64_t shape = 0;
};

// The counters the game stood at on a real tick: what a table rewound to it
// has to carry so the materializer knows the level has moved on since.
struct TickMark {
    int tick = 0;
    uint32_t progress = 0;
    uint32_t commandIndex = 0;
};

uint64_t mixBits(uint64_t h, uint64_t v) {
    h ^= v;
    return h * 1099511628211ull;
}
uint64_t bitsOf(double v) {
    uint64_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}
uint64_t bitsOf(float v) {
    uint32_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

struct Ring {
    // The level this ledger belongs to. A level set up or torn down throws it
    // all away (forgetLedger): the objects an entry names may be gone.
    GJBaseGameLayer* pl = nullptr;
    std::shared_ptr<const WorldDef> def;
    unsigned gen = 0;
    // The level has a move trigger that moves at once instead of making a
    // command (m_isSilent). That move is inlined into triggerMoveCommand on
    // Windows, so no hook sees it and the ledger cannot undo it: such a level
    // keeps no pose coverage at all (fact (r2)).
    bool hasSilentMove = false;
    // A real tick has been seen: the hooks know which tick to stamp with.
    bool ready = false;

    // How many entries this ring may hold. The level's own is the 16 MB cap;
    // the self-test builds a small one so a wrap can be reached.
    std::size_t cap = kMaxEntries;
    std::vector<Entry> entries;
    std::size_t first = 0;  // the oldest entry's place in `entries`
    std::size_t count = 0;
    bool dropped = false;  // the ring has wrapped: the oldest ticks are gone

    // The tick the hooks are recording into (fact (r3)), and the last one
    // afterRealTick was given.
    int tick = 0;
    int lastFrame = -1;
    int startTick = 0;         // the first tick recorded since the level was read
    int lastUncovered = -1;    // the latest tick the ring could not follow
    int lastDrift = -1;        // the latest tick the shadow check found a drift at
    // Whether the tick before the last one was one the ring follows.
    // coveredNow looks at the level after the tick has run, so something that
    // ran during a tick and ended in it is only visible as the tick before
    // it: a tick counts as followed only when both say so.
    bool coveredPrev = true;

    std::vector<uint32_t> seen;  // per slot: the tick + 1 its pose was last taken at
    std::deque<Keyframe> keys;   // oldest first
    std::deque<TickMark> marks;  // one per recorded tick, oldest first

    std::vector<uint32_t> untrusted;  // a bit per group the shadow check caught

    Entry& at(std::size_t i) { return entries[(first + i) % entries.size()]; }
    const Entry& at(std::size_t i) const { return entries[(first + i) % entries.size()]; }

    void grow() {
        const std::size_t want = entries.empty() ? std::min(kFirstEntries, cap) : std::min(entries.size() * 2, cap);
        std::vector<Entry> next(want);
        for (std::size_t i = 0; i < count; i++) next[i] = at(i);
        entries.swap(next);
        first = 0;
    }

    void push(const Entry& e) {
        if (count == entries.size()) {
            if (entries.size() < cap) {
                grow();
            } else {
                // Full at the cap: the oldest entry goes, and with it the
                // promise that the tick it belonged to can be undone.
                entries[first] = e;
                first = (first + 1) % entries.size();
                dropped = true;
                return;
            }
        }
        entries[(first + count) % entries.size()] = e;
        count++;
    }

    // Everything recorded after `frame` is dropped (the game was put back).
    void truncate(int frame) {
        while (count > 0 && at(count - 1).tick > frame) count--;
        std::fill(seen.begin(), seen.end(), 0u);
        while (!keys.empty() && keys.back().tick > frame) keys.pop_back();
        // The keyframe that survives keeps only the inputs of the ticks up to
        // the frame: what it holds past it belongs to a run of the game that
        // is being thrown away. The shorter list is a new one rather than the
        // old one cut down - a start captured past the frame shares the old
        // one, its tick is still a tick that was played, and what it read
        // before the game was put back it must read after, or the same script
        // from the same start would answer two ways.
        if (!keys.empty() && keys.back().inputs) {
            const std::size_t keep = (std::size_t)(frame - keys.back().tick);
            if (keys.back().inputs->size() > keep) {
                keys.back().inputs = std::make_shared<std::vector<LockInput>>(
                    keys.back().inputs->begin(), keys.back().inputs->begin() + (std::ptrdiff_t)keep);
            }
        }
        while (!marks.empty() && marks.back().tick > frame) marks.pop_back();
        if (lastUncovered > frame) lastUncovered = frame;
        if (lastDrift > frame) lastDrift = frame;
        // Nothing past the frame is left to undo, so the frame is as far back
        // as the ring reaches whatever it reached before: a level put back to
        // an earlier numbering (a restart resets the frame) would otherwise
        // keep claiming to start at a tick none of its new ones ever reach.
        if (startTick > frame) startTick = frame;
    }

    // The first tick the ring can still be undone back to.
    int firstCovered() const { return (dropped && count > 0) ? at(0).tick + 1 : startTick; }

    void clear() {
        cap = kMaxEntries;
        pl = nullptr;
        def.reset();
        gen = 0;
        hasSilentMove = false;
        ready = false;
        // The storage goes with it: a level that filled the ring holds 16 MB,
        // and the next may never need it.
        std::vector<Entry>().swap(entries);
        first = 0;
        count = 0;
        dropped = false;
        tick = 0;
        lastFrame = -1;
        startTick = 0;
        lastUncovered = -1;
        lastDrift = -1;
        coveredPrev = true;
        seen.clear();
        keys.clear();
        marks.clear();
        untrusted.clear();
    }
};

Ring g;

// A run of the copies is in progress (or the drawn prediction is): its moves
// are its own and it puts everything it writes back itself.
bool runInProgress() {
    if (currentRun() || current()) return true;
    Bot* bot = Bot::get();
    if (!bot) return true;
    return bot->trajectory().simulating() || bot->trajectory().drawing();
}

// The shape of what is running: everything a keyframe holds except the
// deltas a step of the running actions moves on. While it stays the same,
// nothing has fired and a keyframe stepped forward is the whole state.
uint64_t liveShape(GJBaseGameLayer* pl) {
    uint64_t h = 1469598103934665603ull;
    const GJGameState& gs = pl->m_gameState;
    h = mixBits(h, (uint64_t)(uint32_t)gs.m_currentChannel);
    h = mixBits(h, bitsOf(gs.m_timeModRelated));
    h = mixBits(h, gs.m_timeModRelated2 ? 1 : 0);
    h = mixBits(h, bitsOf(gs.m_timeWarp));
    h = mixBits(h, bitsOf(gs.m_queuedTimeWarp));
    h = mixBits(h, bitsOf(gs.m_timeWarpRelated));
    for (const auto& [k, v] : gs.m_spawnChannelRelated0) h = mixBits(mixBits(h, (uint32_t)k), (uint32_t)v);
    for (const auto& [k, v] : gs.m_spawnChannelRelated1) h = mixBits(mixBits(h, (uint32_t)k), v ? 1 : 0);
    // The kinds the World does not run: their count alone says whether one
    // started or ended, which is a shape change like any other.
    h = mixBits(h, gs.m_dynamicMoveActions.size());
    h = mixBits(h, gs.m_dynamicRotateActions.size());
    h = mixBits(h, gs.m_advanceFollowInstances.size());
    h = mixBits(h, gs.m_moveEffectInstances.size());
    h = mixBits(h, gs.m_rotateEffectInstances.size());
    h = mixBits(h, gs.m_scaleEffectInstances.size());
    h = mixBits(h, gs.m_unkMapPairGJGameEventIntVectorEventTriggerInstance.size());
    h = mixBits(h, gs.m_unkMapPairGJGameEventIntInt.size());
    if (GJEffectManager* em = pl->m_effectManager) {
        h = mixBits(h, em->m_unkVector560.size());
        for (const GroupCommandObject2& c : em->m_unkVector560) {
            h = mixBits(h, (uint32_t)c.m_groupCommandUniqueID);
            h = mixBits(h, (uint32_t)c.m_targetGroupID);
            h = mixBits(h, (uint32_t)c.m_centerGroupID);
            h = mixBits(h, (uint32_t)c.m_commandType);
            h = mixBits(h, (uint32_t)((c.m_finished ? 1 : 0) | (c.m_disabled ? 2 : 0) | (c.m_doUpdate ? 4 : 0)));
            h = mixBits(h, bitsOf(c.m_duration));
        }
        h = mixBits(h, em->m_spawnTriggerActions.size());
        for (const SpawnTriggerAction& a : em->m_spawnTriggerActions) {
            h = mixBits(h, (uint32_t)a.m_triggerUniqueID);
            h = mixBits(h, (uint32_t)a.m_targetGroupID);
            h = mixBits(h, (uint32_t)((a.m_finished ? 1 : 0) | (a.m_disabled ? 2 : 0) | (a.m_spawnOrdered ? 4 : 0)));
            h = mixBits(h, bitsOf(a.m_duration));
        }
        // The toggle bits as toggleGroup reads them (world/capture.cpp).
        const std::size_t bitCount = std::min<std::size_t>(em->m_unkVector438.size(), (std::size_t)kGroupLimit);
        if (const uint32_t* words = off::at<const uint32_t*>(em, off::kToggleBits)) {
            for (std::size_t i = 0; i < (bitCount + 31) / 32; i++) h = mixBits(h, words[i]);
        }
        for (const auto& [item, value] : em->m_itemCountMap) h = mixBits(mixBits(h, (uint32_t)item), (uint32_t)value);
        for (const auto& [item, value] : em->m_persistentItemCountMap) h = mixBits(mixBits(h, (uint32_t)item), (uint32_t)value);
        h = mixBits(h, em->m_timerItemMap.size());
        for (const auto& [key, t] : em->m_timerItemMap) {
            h = mixBits(h, (uint32_t)key);
            h = mixBits(h, (uint32_t)((t.m_active ? 1 : 0) | (t.m_disabled ? 2 : 0) | (t.m_stopTimeEnabled ? 4 : 0)));
            h = mixBits(h, bitsOf(t.m_targetTime));
        }
        h = mixBits(h, em->m_countTriggerActions.size());
        h = mixBits(h, em->m_unkVector230.size());
        h = mixBits(h, em->m_unkVector1e0.size());
        h = mixBits(h, em->m_unkMap3f8.size());
        h = mixBits(h, em->m_unkMap498.size());
    }
    // A group teleport draws from it, and a run carries it: a start whose
    // seed is not the real one answers a different teleport.
    h = mixBits(h, Bot::get()->replaySystem().m_teleportRandomState);
    return h;
}

// Whether the ledger can claim to have seen every move of the tick that just
// ran (fact (r2)): nothing it does not hook was running.
bool coveredNow(GJBaseGameLayer* pl) {
    if (g.hasSilentMove) return false;
    const GJGameState& gs = pl->m_gameState;
    if (!gs.m_dynamicMoveActions.empty() || !gs.m_dynamicRotateActions.empty()) return false;
    if (!gs.m_advanceFollowInstances.empty()) return false;
    if (!gs.m_moveEffectInstances.empty() || !gs.m_rotateEffectInstances.empty() || !gs.m_scaleEffectInstances.empty()) {
        return false;
    }
    // The instances are not the whole of it (world/tierd.cpp (d)):
    // processAreaActions puts back every object no effect touched this tick
    // through resetAreaObjectValues 0x227c30 (0x22909f) and stamps the last
    // position of every object one ever touched (0x2291ae), and neither goes
    // through a hook. Both run on ticks when the instance vectors above are
    // already empty, so a tick with an object still on either of the layer's
    // area lists is not claimed.
    if (pl->m_areaObjectsCount > 0 || pl->m_processedAreaObjectsCount > 0) return false;
    if (GJEffectManager* em = pl->m_effectManager) {
        // Only move commands. The transform step 0x22c680 writes scales
        // through none of the hooks, and which of the other steps do is not
        // settled, so a tick with anything but moves running is not claimed.
        for (const GroupCommandObject2& c : em->m_unkVector560) {
            if (c.m_commandType != off::kCmdMove) return false;
        }
    }
    return true;
}

void markUntrusted(int group) {
    const std::size_t gi = WorldDef::clampGroup(group);
    if (g.untrusted.empty()) g.untrusted.assign((kGroupLimit + 31) / 32, 0u);
    g.untrusted[gi >> 5] |= uint32_t{1} << (gi & 31);
}

// Where every object the ledger saw change after `tick` stood at `tick`,
// newest change first. Walking the ring backwards puts the oldest entry of a
// slot last, and that is the one that holds the pose of `tick`: the ticks in
// between recorded no change for it.
void undoTo(const Ring& r, int tick, std::vector<std::pair<int, Pose>>& out) {
    out.clear();
    if (r.count == 0) return;
    // The oldest entry of each slot wins: walking backwards meets a slot's
    // newest change first and its oldest last, so a later one overwrites the
    // pose already kept. A stamp per slot finds it in one pass - sorting the
    // whole run of changes instead was most of the cost of building a start,
    // and of the shadow check that runs on the game's own thread.
    static std::vector<uint32_t> stamp;  // per slot: the walk it was last seen in
    static std::vector<uint32_t> place;  // and where in `out` its pose sits
    static uint32_t walk = 0;
    if (++walk == 0) {  // wrapped: no stamp may pass for this walk's
        std::fill(stamp.begin(), stamp.end(), 0u);
        walk = 1;
    }
    for (std::size_t n = r.count; n-- > 0;) {
        const Entry& e = r.at(n);
        if (e.tick <= tick) break;
        if (e.slot < 0) continue;
        const std::size_t s = (std::size_t)e.slot;
        if (s >= stamp.size()) {
            stamp.resize(s + 1, 0u);
            place.resize(s + 1, 0u);
        }
        if (stamp[s] == walk) {
            out[place[s]].second = e.before;
            continue;
        }
        stamp[s] = walk;
        place[s] = (uint32_t)out.size();
        out.push_back({e.slot, e.before});
    }
}

// The ticks of a keyframe's inputs a start at `offset` ticks past it uses.
std::span<const LockInput> inputsFor(const std::vector<LockInput>& all, int offset) {
    if (offset <= 0 || (std::size_t)offset > all.size()) return {};
    return std::span<const LockInput>(all.data(), (std::size_t)offset);
}

// A state stepped on by the given ticks, with no copies and nothing of the
// real game touched: the run has no layer, so the fires a step could make
// write only the state (world/fire.cpp guards every use of run.pl).
void stepForward(WorldState& ws, OpLog& log, ObjectCache& cache, const std::shared_ptr<const WorldDef>& def,
                 std::span<const LockInput> inputs) {
    Run run{};
    run.def = def.get();
    run.ws = &ws;
    run.log = &log;
    run.cache = &cache;
    // Making the run current also keeps the hooks quiet: the ledger records
    // nothing while a run of its own is stepping.
    Scope scope(&run);
    for (const LockInput& in : inputs) {
        run.walks = false;
        run.walked = false;
        run.walkAfter = nullptr;
        run.lockRecorded = true;
        run.lockDx = in.dx;
        run.lockDy = in.dy;
        stepTickBegin(run, in.dt, nullptr, 0);
        // processCommands 0x239cf0 (world/step.cpp (a)).
        ws.commandIndex += 2;
        stepTick(run, in.dt);
        endTick(run);
    }
}

// The commands of two states, in the vector's order, field for field.
bool sameCommands(const WorldState& a, const WorldState& b, int& where) {
    if (a.cmds->size() != b.cmds->size()) {
        where = -1;
        return false;
    }
    for (std::size_t i = 0; i < a.cmds->size(); i++) {
        if ((*a.cmds)[i] == (*b.cmds)[i]) continue;
        where = (*b.cmds)[i].targetGroup;
        return false;
    }
    return true;
}

// The shadow check (design step 10): the previous keyframe stepped to this
// tick against the fresh import, and the ledger's objects against the level.
// Only ever run on a keyframe taken because 240 ticks passed - one taken
// because something fired is a tick the step was never meant to predict.
void shadowCheck(const Keyframe& prev, const WorldState& fresh, int nowTick) {
    if (!prev.ws || !prev.inputs || !prev.def || prev.def != g.def) return;
    const int offset = nowTick - prev.tick;
    const std::span<const LockInput> inputs = inputsFor(*prev.inputs, offset);
    if (inputs.empty()) return;

    WorldState pred = *prev.ws;
    OpLog log;
    log.reset((uint32_t)pred.tick);
    ObjectCache cache;
    cache.reset(prev.def);
    // Where the objects stood at the keyframe: the ledger undone back to it.
    // Every slot that moved since is given its base, so the poses the step
    // works out start from the same place the real game did.
    std::vector<std::pair<int, Pose>> base;
    const bool poses = prev.tick >= g.firstCovered() && prev.tick > g.lastUncovered;
    if (poses) {
        undoTo(g, prev.tick, base);
        for (const auto& [slot, pose] : base) cache.setBase(slot, pose);
    }
    stepForward(pred, log, cache, prev.def, inputs);

    int badGroup = 0;
    if (!sameCommands(pred, fresh, badGroup)) {
        char line[192];
        std::snprintf(line, sizeof(line), "ledger: %zu commands predicted, %zu real (group %d) over %d ticks",
                      pred.cmds->size(), fresh.cmds->size(), badGroup, offset);
        noteDrift(line);
        g.lastDrift = nowTick;
        if (badGroup > 0) {
            markUntrusted(badGroup);
        } else {
            // A shape that came apart says nothing about which command is at
            // fault: every group the real ones move stops being trusted.
            for (const WCmd& c : *fresh.cmds) markUntrusted(c.targetGroup);
        }
        return;
    }
    if (!(pred.toggleBits == fresh.toggleBits) || !(pred.items == fresh.items) || pred.spawns->size() != fresh.spawns->size()) {
        noteDrift("ledger: the toggles, items or spawns of a keyframe do not match the step");
        g.lastDrift = nowTick;
        return;
    }
    if (!poses || !prev.def) return;

    // ... and the poses of every object the real game moved in between.
    int bad = 0;
    for (const auto& [slot, was] : base) {
        if (slot < 0 || (std::size_t)slot >= prev.def->slots.size()) continue;
        GameObject* o = prev.def->slots[(std::size_t)slot];
        if (!o) continue;
        const Pose have = cache.pose(log, slot);
        const Pose real = ObjectCache::readLive(o);
        if (std::abs(have.x - real.x) <= 1e-6 && std::abs(have.y - real.y) <= 1e-6) continue;
        bad++;
        const std::span<const uint16_t> groups = prev.def->groupsOf(slot);
        const int group = groups.empty() ? 0 : (int)groups[0];
        for (uint16_t gg : groups) markUntrusted((int)gg);
        if (bad <= 4) {
            char line[224];
            std::snprintf(line, sizeof(line), "group %d uid %d drifts (pred %.6f,%.6f real %.6f,%.6f)", group,
                          o->m_uniqueID, have.x, have.y, real.x, real.y);
            noteDrift(line);
        }
    }
    if (bad > 0) {
        g.lastDrift = nowTick;
        devlog::logf(devlog::Cat::System, "world: ledger shadow check - %d objects drifted over %d ticks", bad, offset);
    }
}

}  // namespace

// ------------------------------------------------------------ the self-test

const char* checkLedger() {
    // A ring of its own, small enough to wrap, filled with a sequence whose
    // answer is known: object `slot` was moved on every tick that is a
    // multiple of slot + 1, and the pose it had before that move says which
    // tick it belonged to. Undoing back to a tick must then give, for every
    // object, the pose of the last move at or before it.
    constexpr int kSlots = 5;
    constexpr int kTicks = 60;
    Ring r;
    r.cap = 64;  // fewer than the 137 entries below: the ring has to wrap
    auto poseOf = [](int slot, int tick) {
        Pose p;
        p.x = slot * 1000.0 + tick;
        p.y = -(double)tick;
        p.marker = (uint32_t)tick;
        return p;
    };
    int pushed = 0;
    for (int tick = 1; tick <= kTicks; tick++) {
        for (int slot = 0; slot < kSlots; slot++) {
            if (tick % (slot + 1) != 0) continue;
            // The pose before the move is the one the object has stood at
            // since its last move.
            int since = tick - (slot + 1);
            if (since < 0) since = 0;
            r.push(Entry{tick, slot, poseOf(slot, since)});
            pushed++;
        }
    }
    if (pushed != 137) return "the ledger self-test did not build the sequence it meant to";
    if (!r.dropped || r.count != r.cap) return "the ledger ring did not wrap at its cap";
    const int oldest = r.at(0).tick;

    std::vector<std::pair<int, Pose>> undo;
    for (int tick = oldest; tick <= kTicks; tick++) {
        undoTo(r, tick, undo);
        for (int slot = 0; slot < kSlots; slot++) {
            // The change the undo has to put back is the first move of the
            // object after `tick`; there is none when its last move was at or
            // before it and nothing follows.
            int next = 0;
            for (int t = tick + 1; t <= kTicks; t++) {
                if (t % (slot + 1) == 0) {
                    next = t;
                    break;
                }
            }
            const auto it = std::find_if(undo.begin(), undo.end(),
                                         [slot](const std::pair<int, Pose>& e) { return e.first == slot; });
            if (next == 0) {
                if (it != undo.end()) return "the ledger undid an object that had not moved since the tick";
                continue;
            }
            if (it == undo.end()) return "the ledger left out an object that moved after the tick";
            int since = next - (slot + 1);
            if (since < 0) since = 0;
            if (!bitEqual(it->second, poseOf(slot, since))) return "the ledger undid an object to the wrong pose";
        }
    }

    // Everything past a frame goes, and what is left still answers the same.
    r.truncate(40);
    if (r.count == 0) return "the ledger truncated everything away";
    if (r.at(r.count - 1).tick > 40) return "the ledger kept an entry past the frame it was put back to";
    undoTo(r, 40, undo);
    if (!undo.empty()) return "the ledger still had changes to undo after the frame it was put back to";
    return nullptr;
}

// ------------------------------------------------------------ the hooks

bool ledgerRecording() {
    if (World::disabled || !g.ready || !g.def) return false;
    if (g.gen != moverCacheGeneration()) return false;
    return !runInProgress();
}

// The pose of one object whose slot is already known. ledgerNoteGroup walks
// the level's own slot lists, so it never has to look one up from the object -
// which reads a cold line off the object and then searches for the slot it
// already came from.
static void ledgerNoteSlot(int slot, GameObject* object) {
    if (!object || slot < 0 || (std::size_t)slot >= g.seen.size()) return;
    const uint32_t stamp = (uint32_t)g.tick + 1u;
    if (g.seen[(std::size_t)slot] == stamp) return;  // already taken this tick
    g.seen[(std::size_t)slot] = stamp;
    g.push(Entry{g.tick, slot, ObjectCache::readLive(object)});
}

void ledgerNoteObject(GameObject* object) {
    if (!object || !g.def) return;
    ledgerNoteSlot(g.def->slotOf(object), object);
}

void ledgerNoteArray(cocos2d::CCArray* objects) {
    if (!objects) return;
    for (unsigned i = 0; i < objects->count(); i++) {
        ledgerNoteObject(static_cast<GameObject*>(objects->objectAtIndex(i)));
    }
}

void ledgerNoteGroup(int group) {
    if (!g.def) return;
    // The arrays toggleGroup walks (0x223c3c): m_groups, and the objects it
    // moves through the other two are the same ones.
    for (int slot : g.def->members(group)) {
        if (slot >= 0 && (std::size_t)slot < g.def->slots.size()) {
            ledgerNoteSlot(slot, g.def->slots[(std::size_t)slot]);
        }
    }
}

// ------------------------------------------------------------ the level

void forgetLedger() { g.clear(); }

bool ledgerUntrustedGroup(int group) {
    if (g.untrusted.empty()) return false;
    const std::size_t gi = WorldDef::clampGroup(group);
    return (g.untrusted[gi >> 5] >> (gi & 31)) & 1u;
}

// ------------------------------------------------------------ the real tick

void World::afterRealTick(GJBaseGameLayer* pl, int frame) {
    if (World::disabled || !pl) return;
    const unsigned gen = moverCacheGeneration();
    // The level as it stands now. WorldDef::get reads it again when the object
    // count changes without the generation moving (the editor deleting objects
    // between playtests), and an entry of the ring names a slot of the read it
    // was taken through: another read starts the ledger over rather than undo
    // into objects that are not there any more.
    std::shared_ptr<const WorldDef> def = WorldDef::get(pl);
    if (!def || World::disabled) return;
    if (g.pl != pl || g.gen != gen || g.def != def) {
        g.clear();
        g.pl = pl;
        g.gen = gen;
        g.def = def;
        g.seen.assign(def->slots.size(), 0u);
        for (const TriggerDef& t : def->triggers) {
            if (t.kind == Kind::Move && t.silent) g.hasSilentMove = true;
        }
    }

    // The step's delta and the lock inputs the game worked out this tick
    // (world/step.cpp (m)), which is all a step of it needs from outside the
    // state. Read, never written.
    LockInput lock;
    lock.dt = Bot::get()->updater().getPhysicsDt() * std::min(pl->m_gameState.m_timeWarp, 1.0f);
    if (GJEffectManager* em = pl->m_effectManager) {
        const float* values = &off::at<float>(em, off::kLockInputs);
        lock.dx = values[0];
        lock.dy = values[1];
    }

    if (!g.ready) {
        g.ready = true;
        g.startTick = frame;
        g.tick = frame + 1;
        g.lastFrame = frame;
    } else if (frame <= g.lastFrame) {
        // The game was put back (the pathfinder rewinding, a practice restore,
        // setFrame): everything recorded past the frame is gone, and the
        // keyframe that survives stops taking inputs - a start that already
        // shares it keeps reading exactly what it read before.
        // A frame that never moves comes in here too: a level played without a
        // PlayLayer to count its ticks (the editor) leaves the frame where it
        // is, so every one of its ticks looks like a put-back. The keyframe of
        // where the game stands is taken again either way, which is what keeps
        // a start made on such a tick standing at the state of that tick.
        g.truncate(frame);
        g.tick = frame + 1;
        g.lastFrame = frame;
        // What the game was put back to does not have to be what it played
        // through the first time (a checkpoint holds less than every field),
        // so nothing before the frame is claimed to be undoable any more.
        g.lastUncovered = frame;
        g.coveredPrev = coveredNow(pl);
        // The frame is being played again from here: it keeps one mark, the
        // one the game now stands at. The counters of the visit before it are
        // not this visit's - a put-back does not have to land on them.
        const TickMark now{frame, pl->m_gameState.m_currentProgress, pl->m_gameState.m_commandIndex};
        if (!g.marks.empty() && g.marks.back().tick == frame) g.marks.back() = now;
        else g.marks.push_back(now);
        // A keyframe of where the game now stands, whatever the shape says:
        // what the game was put back to does not have to be what it played
        // through, and the keyframe of a tick it has played is not a keyframe
        // of this one. A start made from here finds this one (ledgerFillStart
        // takes the newest of a tick).
        WorldState fresh = World::captureLive(pl, true);
        if (World::disabled) return;
        g.keys.push_back(Keyframe{frame, std::make_shared<const WorldState>(std::move(fresh)),
                                  std::make_shared<std::vector<LockInput>>(), g.def, liveShape(pl)});
        while (g.keys.size() > kKeepKeyframes) g.keys.pop_front();
        return;
    } else {
        g.tick = frame + 1;
        g.lastFrame = frame;
    }

    // What the tick stood at, for a table rewound to it.
    g.marks.push_back(TickMark{frame, pl->m_gameState.m_currentProgress, pl->m_gameState.m_commandIndex});
    while (g.marks.size() > kMaxMarks) g.marks.pop_front();
    const bool covered = coveredNow(pl);
    if (!covered || !g.coveredPrev) g.lastUncovered = frame;
    g.coveredPrev = covered;

    // The tick's inputs belong to the keyframe before it: they are what steps
    // that keyframe one tick further.
    if (!g.keys.empty()) g.keys.back().inputs->push_back(lock);

    const uint64_t shape = liveShape(pl);
    const bool changed = g.keys.empty() || shape != g.keys.back().shape;
    const bool due = g.keys.empty() || (frame - g.keys.back().tick) >= kKeyframeEvery;
    if (!changed && !due) return;

    WorldState fresh = World::captureLive(pl, true);
    // captureLive reads the level, and the level check can turn the World off:
    // there is then nothing to keep.
    if (World::disabled) return;
    if (!changed && due && !g.keys.empty()) shadowCheck(g.keys.back(), fresh, frame);
    g.keys.push_back(Keyframe{frame, std::make_shared<const WorldState>(std::move(fresh)),
                              std::make_shared<std::vector<LockInput>>(), g.def, shape});
    while (g.keys.size() > kKeepKeyframes) g.keys.pop_front();
}

// ------------------------------------------------------------ starts

bool ledgerFillStart(GJBaseGameLayer* pl, WorldStart& start, int tick) {
    if (World::disabled || !g.ready || !g.def || g.keys.empty()) return false;
    if (g.gen != moverCacheGeneration() || !pl || g.pl != pl) return false;
    // The newest keyframe at or before the tick. Two can stand at one tick -
    // a put-back takes a keyframe of the tick it lands on, which the game may
    // have played through before - and it is the newest that says where the
    // game stands now (the keys are oldest first, so ties fall to the last).
    const Keyframe* use = nullptr;
    for (const Keyframe& k : g.keys) {
        if (k.tick <= tick && (!use || k.tick >= use->tick)) use = &k;
    }
    if (!use || !use->ws || !use->inputs) return false;
    const int offset = tick - use->tick;
    if (offset < 0 || (std::size_t)offset > use->inputs->size()) return false;
    // The step of those ticks was caught being wrong: the state it would give
    // is not one to start from.
    if (g.lastDrift >= use->tick) return false;
    start.keyframe = use->ws;
    start.keyframeTick = use->tick;
    start.offset = offset;
    start.inputs = use->inputs;
    start.def = use->def;
    start.gen = g.gen;
    // The contacts of this tick, not the keyframe's: the step has no copies to
    // touch anything, so it ages out every contact it was given.
    World::captureContacts(pl, start.contact);
    start.haveContact = true;
    // Whether its objects can be put back where they were is decided when a
    // run first begins from it (ledgerStartObjects).
    start.inexact = true;
    return true;
}

const WorldState& WorldStart::materialize() const {
    if (m_stepped) return *m_stepped;
    if (!keyframe) {
        static const WorldState empty;
        return empty;
    }
    // A start kept before the level was read again is left at its keyframe:
    // the step can ask where an object stands, and the objects that read named
    // may be gone. Such a start has no table either (its slots are another
    // read's), so its runs carry objects the old way and call every killer
    // uncertain - the answer a stale state can be trusted for.
    const bool step = offset > 0 && inputs && def && (std::size_t)offset <= inputs->size() &&
                      gen == moverCacheGeneration();
    if (!step && !haveContact) {
        m_stepped = keyframe;
        return *m_stepped;
    }
    auto stepped = std::make_shared<WorldState>(*keyframe);
    if (step) {
        OpLog log;
        log.reset((uint32_t)stepped->tick);
        ObjectCache cache;
        cache.reset(def);
        stepForward(*stepped, log, cache, def, inputsFor(*inputs, offset));
    }
    if (haveContact) {
        stepped->contact[0] = contact[0];
        stepped->contact[1] = contact[1];
    }
    m_stepped = std::move(stepped);
    return *m_stepped;
}

bool ledgerStartObjects(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& def, const WorldState& ws, int tick,
                        uint32_t liveProgress, uint32_t liveCommandIndex, std::shared_ptr<const BaseTable>& table,
                        std::shared_ptr<const std::vector<Carry>>& carry) {
    if (World::disabled || !g.ready || !pl || !def || def != g.def) return false;
    if (g.gen != moverCacheGeneration() || g.pl != pl) return false;
    if (ws.partial) return false;
    // Every tick since the start has to have been one the ledger follows, its
    // entries have to still be in the ring, and the shadow check must not have
    // caught the step being wrong anywhere after it (design step 10: a start
    // in a drifting interval is inexact). The last tick the ring could not
    // follow is itself out: what coveredNow found running at the end of that
    // tick is what could move without a hook on the tick after it, which is
    // exactly a tick this start would have to undo.
    if (tick < g.firstCovered() || tick <= g.lastUncovered || tick > g.lastFrame) return false;
    if (g.lastDrift >= tick) return false;
    const TickMark* mark = nullptr;
    for (auto it = g.marks.rbegin(); it != g.marks.rend(); ++it) {
        if (it->tick < tick) break;
        if (it->tick == tick) {
            mark = &*it;
            break;
        }
    }
    if (!mark) return false;
    std::shared_ptr<const BaseTable> now = baseTable(pl, def, liveProgress, liveCommandIndex);
    if (!now) return false;
    std::vector<std::pair<int, Pose>> undo;
    undoTo(g, tick, undo);
    std::shared_ptr<const BaseTable> past = rewoundTable(*now, mark->progress, mark->commandIndex, undo);
    if (!past) return false;
    carry = carriedMovers(*past, ws, *def);
    table = std::move(past);
    return true;
}

}  // namespace world
