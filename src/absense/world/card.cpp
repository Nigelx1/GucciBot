#include "absense/world/card.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include "absense/world/def.hpp"
#include "absense/world/materialize.hpp"
#include "absense/world/state.hpp"
#include "absense/world/world.hpp"

using namespace geode::prelude;

namespace world {

namespace {

// Stepping is a whole decision's worth of ticks, once per slice, and it has
// to stay well inside one frame. Past this the objects are simply carried on
// from where they are by the middle of that many ticks - the batch's own
// PlayerState::moveOrigin counts from the tick they stand at either way, so a
// cap costs some of the centring and nothing else.
constexpr int kMaxAhead = 1024;
// The card only ever hears about what a copy could run into, and a level can
// name tens of thousands of objects in one group: a slice is not worth more
// than this many moved objects (the fallback path stops at 512 movers for the
// same reason).
constexpr std::size_t kMaxObjects = 4096;

// A copy of the live state stepped on by `ahead` ticks, with no copies and
// nothing of the real game touched: the run has no layer, so the fires a step
// could make write only the state (world/fire.cpp guards every use of run.pl).
// The same shape as the ledger's replay of a real tick.
void stepForward(WorldState& ws, OpLog& log, ObjectCache& cache, const std::shared_ptr<const WorldDef>& def, int ahead,
                 float dt) {
    Run run{};
    run.def = def.get();
    run.ws = &ws;
    run.log = &log;
    run.cache = &cache;
    // Making the run current also keeps the hooks quiet: the ledger records
    // nothing while a run of its own is stepping.
    Scope scope(&run);
    for (int i = 0; i < ahead; i++) {
        run.walks = false;
        run.walked = false;
        run.walkAfter = nullptr;
        // No copy of player 1 to take the lock inputs from, and no recorded
        // ones either: a slice looks at the level in front of the player, and
        // a lock-to-player move of it is the player's own business.
        run.lockRecorded = true;
        run.lockDx = 0.0f;
        run.lockDy = 0.0f;
        stepTickBegin(run, dt, nullptr, 0);
        // processCommands 0x239cf0 (world/step.cpp (a)).
        ws.commandIndex += 2;
        stepTick(run, dt);
        endTick(run);
    }
}

// Whether an op of this kind puts an object somewhere else. A toggle only
// moves the group's counter, and a rotate or a scale is not ported (it marks
// the pose uncertain and leaves it where it was), so neither says anything
// the card can carry - and a level whose decoration is one big toggled group
// would otherwise fill the slice's few thousand places with objects that
// never moved.
bool movesObjects(OpKind kind) {
    return kind == OpKind::Translate || kind == OpKind::TranslateOptimized || kind == OpKind::Place;
}

// Every group an op of the log moved something in, once.
std::vector<int> groupsMoved(const OpLog& log) {
    std::vector<int> groups;
    for (const auto& chunk : log.sealed()) {
        for (const Op& op : chunk->ops) {
            if (movesObjects(op.kind)) groups.push_back(op.group);
        }
    }
    for (const Op& op : log.open().ops) {
        if (movesObjects(op.kind)) groups.push_back(op.group);
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    return groups;
}

}  // namespace

CardSlice cardSlice(GJBaseGameLayer* pl, int ahead, float dt) {
    CardSlice out;
    if (!pl || World::disabled) return out;
    if (!(dt > 0.0f)) return out;
    // Never inside a run: its scope is the thread's, and a slice is only ever
    // asked for between runs (the pathfinder's own slices).
    if (currentRun() != nullptr) return out;
    ahead = std::clamp(ahead, 0, kMaxAhead);

    const std::shared_ptr<const WorldDef> def = WorldDef::get(pl);
    if (!def) return out;
    WorldState ws = World::captureLive(pl, true);
    // captureLive turns the World off when it cannot read something: a state
    // from a World that has just been turned off says nothing.
    if (World::disabled || ws.partial) return out;

    const uint32_t progress = pl->m_gameState.m_currentProgress;
    const uint32_t commandIndex = pl->m_gameState.m_commandIndex;
    const std::shared_ptr<const BaseTable> table = baseTable(pl, def, progress, commandIndex);
    if (!table) return out;
    const std::shared_ptr<const std::vector<Carry>> carry = carriedMovers(*table, ws, *def);

    OpLog log;
    ObjectCache cache;
    cache.reset(def);
    cache.setBaseTable(table);
    ws.commandIndex = commandIndex;
    const uint32_t firstTick = (uint32_t)ws.tick;
    stepForward(ws, log, cache, def, ahead, dt);
    const uint32_t lastTick = firstTick + (uint32_t)ahead;

    out.ok = true;
    out.ahead = ahead;

    // What the World follows: everything in a group one of its ops named.
    // Where the object stands after the last step, and how far the step
    // before the last one moved it - the speed the card carries it on at.
    for (const int group : groupsMoved(log)) {
        for (const int slot : def->members(group)) {
            if (out.poses.size() >= kMaxObjects) break;
            GameObject* o = slot >= 0 && slot < (int)def->slots.size() ? def->slots[(std::size_t)slot] : nullptr;
            if (!o) continue;
            const int uid = o->m_uniqueID;
            if (out.poses.count(uid) != 0) continue;
            const Pose now = cache.pose(log, slot);
            CardPose entry;
            entry.x = now.x;
            entry.y = now.y;
            if (ahead > 0) {
                // The last step of all: poseAt takes the ops of every tick up
                // to and including the one it is given, and the last tick
                // stepped is lastTick - 1, so the pose before that tick's own
                // ops is the one at lastTick - 2. (Asking for lastTick - 1
                // gave the pose after every op there is, and every object in
                // the slice went out standing still.)
                const Pose before = ahead >= 2 ? cache.poseAt(log, slot, lastTick - 2) : cache.base(slot);
                entry.dx = (float)(now.x - before.x);
                entry.dy = (float)(now.y - before.y);
            }
            // The same bounds the fallback path takes a step to be one tick of
            // a move by (trajectory.cpp, MovingObjects::begin).
            if (std::fabs(entry.dx) < 0.0005f && std::fabs(entry.dy) < 0.0005f) entry.dx = entry.dy = 0.0f;
            if (std::fabs(entry.dx) > 40.0f || std::fabs(entry.dy) > 40.0f) entry.dx = entry.dy = 0.0f;
            if (entry.dx != 0.0f || entry.dy != 0.0f) out.moves = true;
            out.poses.emplace(uid, entry);
        }
    }

    // What it does not follow is carried on at the step the real game last
    // gave it, exactly as a run carries it (world/materialize.hpp, Carry).
    if (carry) {
        for (const Carry& c : *carry) {
            if (out.poses.size() >= kMaxObjects) break;
            GameObject* o = c.slot >= 0 && c.slot < (int)def->slots.size() ? def->slots[(std::size_t)c.slot] : nullptr;
            if (!o) continue;
            const int uid = o->m_uniqueID;
            if (out.poses.count(uid) != 0) continue;  // an op moved it: the World's word wins
            const Pose* base = table->find(c.slot);
            if (!base) continue;
            CardPose entry;
            entry.x = base->x + (double)c.dx * ahead;
            entry.y = base->y + (double)c.dy * ahead;
            entry.dx = c.dx;
            entry.dy = c.dy;
            if (entry.dx != 0.0f || entry.dy != 0.0f) out.moves = true;
            out.poses.emplace(uid, entry);
        }
    }
    return out;
}

}  // namespace world
