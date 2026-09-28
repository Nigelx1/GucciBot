#pragma once

// The real game's ledger (trigger design step 10): what the level's own ticks
// did to its objects and to its effect manager, kept so a run can begin from a
// tick the game has already left.
//
// Two things are recorded. Hooks on the six functions the game moves, rotates,
// transforms and toggles objects through write the pose an object had before
// the change into a ring buffer, once per object per real tick; undoing that
// ring from now backwards gives where every object stood at an earlier tick.
// And World::afterRealTick, at the end of every real tick, keeps a keyframe of
// the whole World state whenever the shape of the running actions changes (so
// the ticks between two keyframes only ever advance what is already running)
// and otherwise records the tick's lock inputs and delta, which is all a step
// of those ticks needs from outside the state.
//
// A kept start then holds a keyframe and an offset instead of an import of its
// own, and materialize() steps the keyframe there. On every periodic keyframe
// - one taken because 240 ticks passed, not because anything fired - the
// shadow check steps the previous keyframe forward with the recorded inputs
// and compares it with the fresh import, command by command and pose by pose:
// a disagreement is a drift (the pathfinder's m_stats.worldDrift), and the
// groups it was found in stop counting as modelled for the rest of the level.
//
// Nothing here is part of a run: a start takes what it needs when it is
// captured and materializes once, so a run's answer never depends on what the
// real game did after the start was made.

#include <Geode/Geode.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "absense/world/materialize.hpp"
#include "absense/world/state.hpp"

namespace world {

class WorldDef;

// The hard cap on the ring of poses (design step 10). The ledger grows into it
// and then wraps; how many real ticks that covers depends on how much the
// level moves, and coversPoses says which ticks are still there.
inline constexpr std::size_t kLedgerBytes = 16u * 1024u * 1024u;
// A keyframe is taken at least this often even when nothing changes shape.
inline constexpr int kKeyframeEvery = 240;

// ------------------------------------------------------------ the hooks
// hooks/GJBaseGameLayer.cpp calls these before letting the game's own
// moveObjects, rotateObjects, rotateObject, moveAreaObject,
// transformAreaObjects and toggleGroup run.

// Whether the ledger wants to hear about a change now: the World is on, a
// level has been read, and no run of the copies is in progress (a run puts
// everything it writes back itself, and its moves are its own, not the
// level's).
bool ledgerRecording();

// Where these objects stand, before the game changes them.
void ledgerNoteArray(cocos2d::CCArray* objects);
void ledgerNoteObject(GameObject* object);
// The same for every member of a group about to be toggled.
void ledgerNoteGroup(int group);

// ------------------------------------------------------------ the level

// A level is being set up or torn down (world::forgetLevel): the ring, the
// keyframes and the untrusted groups go with it. The objects a kept entry
// names may already be gone, so nothing is written back - a start older than
// this simply has no ledger to stand on.
void forgetLedger();

// ------------------------------------------------------------ starts

// Fills `start` with the latest keyframe at or before `tick`, the offset that
// steps it there, the inputs to step it with and the contacts the players are
// in now. False when the ledger has nothing that reaches the tick, or when the
// shadow check found a drift in between: the caller then keeps the partial
// import it made before.
bool ledgerFillStart(GJBaseGameLayer* pl, WorldStart& start, int tick);

// Where the level's objects stood at `tick` and what a run from there carries,
// from the table of now with the ledger undone back to it. False when the
// ledger does not cover every tick since, and the start's runs then move
// objects the old way.
bool ledgerStartObjects(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& def, const WorldState& ws, int tick,
                        uint32_t liveProgress, uint32_t liveCommandIndex, std::shared_ptr<const BaseTable>& table,
                        std::shared_ptr<const std::vector<Carry>>& carry);

// ------------------------------------------------------------ the self-test

// The ring and its undo on a sequence of its own, small enough to wrap
// (world/ledger.cpp): null when undoing back to every tick gives the pose
// each object really had there, and when a truncation leaves nothing behind.
// Runs at level load from World::init; a failure turns the World off, which
// is exactly today's behaviour for every run.
const char* checkLedger();

// ------------------------------------------------------------ certainty

// A group the shadow check caught the World having somewhere else than the
// real game did: a killer in it is uncertain for the rest of the level,
// whatever the level read said about the kinds that name it.
bool ledgerUntrustedGroup(int group);

}  // namespace world
