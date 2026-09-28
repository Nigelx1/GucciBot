#pragma once

// The World: the mod's own model of what triggers and moving objects do to a
// run of the copies (see absense-tools/trigger_design_plan.md). Everything it
// keeps per run is a WorldState and an OpLog, copied by value into every
// SimSnapshot and SimStart, so a branch and a kept start put them back exactly
// and the same script from the same start always gives the same result.
//
// What runs use of it: the multi-activate contact map of the copies, the
// teleport-group random state, and the step of the Tier A trigger kinds (move,
// toggle, spawn, stop, the counts and pickups, speed portals, gravity, rotate
// gameplay and teleport triggers): each simulated tick steps the running
// commands and queued spawns, and the spawn walk fires what the copy of player
// 1 crosses. What those do to the level's objects goes into the OpLog, and the
// materializer (world/materialize.hpp) writes it into the objects and collision
// buckets near the copies and puts every byte back when the run ends or its
// slice is suspended - for a run whose state holds the running actions; any
// other run keeps MovingObjects. What they do to the copies (speed, gravity,
// rotation, teleports) is applied to the copies. When the offsets World::init
// checks at level load do not hold, World::disabled is set and every run
// behaves exactly as it did before the World existed.

#include <Geode/Geode.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "absense/world/state.hpp"

namespace world {

class WorldDef;
struct TriggerDef;
class Materializer;

// A run of the World in progress: what the step, the triggers it fires and the
// spawn walk work on. Everything in it that outlives a tick is the run's
// WorldState and OpLog (kept by every snapshot and start); the rest is either
// read-only (the level's WorldDef, the layer) or scratch that the top of every
// tick clears.
struct Run {
    GJBaseGameLayer* pl = nullptr;
    const WorldDef* def = nullptr;
    WorldState* ws = nullptr;
    OpLog* log = nullptr;
    // Where the run's objects stand (bases read from the live objects the
    // first time they are asked about).
    ObjectCache* cache = nullptr;
    // The copies standing in for m_player1 and m_player2 this run (null when
    // the run does not simulate that player).
    PlayerObject* player1 = nullptr;
    PlayerObject* player2 = nullptr;
    // Whether the copy of player 1 has died in the run: handleButton hands
    // nothing to the touch listeners while m_player1 is dead (0x233aa3). The
    // copies carry their death in the run rather than in the player's own
    // m_isDead (which stays whatever the real player had), so the run sets
    // this from its own deaths before each tick's input; a branch that puts
    // those deaths back answers the same.
    bool player1Dead = false;
    // GJBaseGameLayer::m_spawnTuples (+0x10b8): the (group, uid, last remap
    // key) a spawnGroup already spawned this tick. update empties it at the
    // top of every step (0x237fb3), so it never outlives a tick.
    std::vector<std::array<int, 3>> spawnTuples;
    // How deep spawnObject -> triggerObject -> spawnGroup has recursed now.
    int depth = 0;
    // The World's spawn walk replaces the copies' own this tick (set by the
    // run before its copies tick, see walkModelled). The game walks after both
    // players' collision passes and before either turns (update 0x23861f,
    // 0x23866d): the walk runs inside the tick of `walkAfter`, the last copy
    // that ticks, right where the copies' own walk was; `walked` says it did
    // (a tick whose copies are all dead walks after them instead).
    bool walks = false;
    PlayerObject* walkAfter = nullptr;
    bool walked = false;
    // What puts the run's objects where its log has them near the copies
    // (world/materialize.hpp), or null for a run whose objects move by the old
    // path (MovingObjects): a copy's teleport asks it to catch every object up.
    Materializer* objects = nullptr;
    // The tick's lock inputs, for a run with no copy of player 1 to take them
    // from: the ledger's replay of a real tick hands over what the game worked
    // out then (world/ledger.cpp). Scratch of one tick, set before each
    // stepTick, so it never has to be kept by a snapshot.
    bool lockRecorded = false;
    float lockDx = 0.0f;
    float lockDy = 0.0f;
};

// Appends an op to the run's log and adds it to the reach of its group
// (WorldState::reach). Every op a run emits goes through here.
void emitOp(Run& run, const Op& op);

// Whether the World's spawn walk is modelled for the run: it follows the copy
// of player 1 outside a platformer level (world/fire.cpp (w)). Otherwise each
// copy keeps phys::checkSpawnObjects, as before the World.
bool walkModelled(const Run& run);

// A rotate-gameplay trigger's channel switch, from wherever it fires (the
// copies' touch path included): the run's channel and whether its walk goes
// back (GJBaseGameLayer::rotateGameplay 0x2181df-0x218217).
void switchChannel(WorldState& ws, int channel, bool goingBack);

// The run whose tick is in progress (Sim::step, the drawn prediction), or null.
Run* currentRun();

class World {
   public:
    // Set by init when the offsets it checks do not hold: every run then
    // behaves exactly as before the World, and the old paths stay in charge.
    // Off until init has checked the code once: nothing turns it on unchecked.
    // A run decides once, when it begins, whether it has the World (Sim::begin,
    // the drawn prediction), and keeps that for its whole life - a level check
    // that turns the World off halfway through a paused search must not switch
    // the search's clock and tick count between two of its slices.
    static inline bool disabled = true;
    // Why the World is off, for the log and the diagnostics (empty when on).
    static inline const char* disabledReason = "not checked yet";

    // Level load: checks the game's code and a live object against the
    // offsets the World relies on, and ObjectCache against a level of its own,
    // logs the result and sets disabled when anything is off. Never throws and
    // never stops the level.
    static void init(GJBaseGameLayer* pl);

    // The check of a WorldDef just read from the level (its first use after
    // loading): every slot's uid leads back to it, the group arrays read back
    // the same, importing the live state twice gives the same state, and each
    // running command copies over field for field. Sets disabled on a failure.
    static void checkDef(const WorldDef& def, GJBaseGameLayer* pl);

    // The World's state of the live game right now, as a run starting from it
    // begins (world/capture.cpp): the contacts the real players are in, the
    // running commands and spawns, the toggle bits, items, timers, listeners,
    // the spawn channels, the parked speed, the time warp, the command index
    // and the teleport seed. Only reads the game. Without withActions (a
    // kept start) the commands, spawns and timers are left out and the state
    // is marked partial; the groups they move are still uncertain.
    static WorldState captureLive(GJBaseGameLayer* pl, bool withActions = true);

    // Just the contact part of that, for a caller that has a state of another
    // tick and needs the contacts of this one (world/ledger.cpp): the entries
    // of m_activatedObjectIDs stamped with the game's current command index,
    // per real player, sorted by object uid.
    static void captureContacts(GJBaseGameLayer* pl, std::vector<std::pair<int, uint32_t>> (&out)[2]);

    // The real game has finished a tick (bot/updater.cpp frameUpdateMidhook,
    // right before the trajectory is told): the ledger takes a keyframe when
    // the shape of the running actions has changed or 240 ticks have passed,
    // and records the tick's lock inputs otherwise. A frame below the last one
    // is a rewind (the pathfinder going back, a practice restore, setFrame):
    // everything after it is dropped. Never throws and never stops the game;
    // anything it cannot read turns the World off instead.
    static void afterRealTick(GJBaseGameLayer* pl, int frame);

    // Turns the World off from outside world/selftest.cpp (a check that needs
    // the level and the copies, see Trajectory::checkWorldBranches), with the
    // same log lines every other failure writes.
    static void turnOff(const char* why);
};

// ------------------------------------------------------------ certainty

// How well the World follows an object (design step 13). A run that dies asks
// this about what killed it: the pathfinder trusts a death at a Static or a
// Modelled object, and only an Uncertain one leaves a phantom spot behind.
enum class Certainty : uint8_t {
    // Nothing in the level names a group of it and it stands where the level
    // put it: no run and no trigger can move it.
    Static,
    // Every trigger that can reach it is a kind the World runs, none of its
    // groups is uncertain, and the run follows where its objects stand.
    Modelled,
    // Something that can move it is not modelled: the run may have it in the
    // wrong place.
    Uncertain,
};
const char* certaintyName(Certainty certainty);

// The certainty of `object` for the run whose tick is in progress, or - asked
// outside a run, about what the real game killed the player with - for the
// level alone, where only Static and Uncertain can come out.
Certainty certaintyOf(GJBaseGameLayer* pl, GameObject* object);

// How many times the World has been found to disagree with the real game or
// with itself since the process started (the pathfinder shows the count of its
// own run as m_stats.worldDrift). The ledger's shadow check (design step 10)
// is the other caller once it lands.
uint64_t drifts();
void noteDrift(const char* what);

// A running command of the game as a WCmd (its remap keys go into the pool it is handed),
// and whether a WCmd holds exactly what the command does, bit for bit.
WCmd importCommand(const GroupCommandObject2& command, const WorldDef* def, std::vector<int>& remaps);
bool commandMatches(const GroupCommandObject2& command, const WCmd& w, const std::vector<int>& remaps);

// A level is being set up or torn down (moverCacheInvalidate): the WorldDef
// read from the last one is let go rather than held until the next is read.
void forgetLevel();

// The state of the run whose tick is in progress (Sim::step, or the drawn
// prediction), or null outside one and for a run begun without the World: the
// copies' contact rule, group teleports and clock ask this, not
// World::disabled, so a run stays on the path it began on.
WorldState* current();

// Makes `ws` (or a whole run and its state) current for its lifetime (nested
// scopes put the outer ones back).
struct Scope {
    WorldState* was;
    Run* wasRun;
    explicit Scope(WorldState* ws);
    explicit Scope(Run* run);
    // The run when there is one, the state alone otherwise.
    Scope(Run* run, WorldState* ws);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// GJBaseGameLayer::canBeActivatedByPlayer 0x2178c0 for a copy, on the run's
// own contact map: every touch stamps (object, copy) with the command index; a
// multi-activate object (EnhancedGameObject::canMultiActivate: m_isMultiActivate
// in a classic level, !m_isNoMultiActivate in a platformer) the copy was still
// touching is refused; anything else is refused once its player's activated
// flag is set. With no current state (a run without the World) it is
// phys::copyCanBeActivated, as before.
bool canActivate(GJBaseGameLayer* pl, PlayerObject* copy, EffectGameObject* object);

// GJGameState::processStateTriggers 0x2054f0 on the run's contacts: every entry
// not stamped on this tick (index below `commandIndex`) is forgotten.
void ageContacts(WorldState& ws, uint32_t commandIndex);

// A run begins at the game's command index, not the one its state was taken
// at: every contact it carries is stamped with `commandIndex`. Only whether an
// entry is there counts (the rule asks that, and ageing drops what the next
// tick does not stamp again), so this never changes an answer - but a start
// carried ahead by a plan (advanceStart) holds stamps up to twice the plan's
// ticks past the game's index, which ageContacts' "below the index" would
// never drop, and the copy stayed "in" every multi-activate portal or pad it
// stood on at the end of the plan for the whole run.
void restampContacts(WorldState& ws, uint32_t commandIndex);

// The teleport group pick in GJBaseGameLayer::teleportPlayer (0x20febb-0x20ff04)
// as the game runs it under the mod: teleportRandomOverride (midhook at
// 0x20FEDC) replaces rand() with the next value of the replay system's state,
// and the state keeps only that 15-bit value. Returns value / 32767.
float nextRandom(uint64_t& seed);

// The state a copy's group teleport draws from: the current run's seed for a
// run with the World, the trajectory's own copy of the replay state otherwise.
uint64_t& teleportSeed();

// The copy index a contact is kept under (1 for the copy of player 2).
int copyIndex(PlayerObject* copy);

// ------------------------------------------------------------ once per tick
// The parts of GJBaseGameLayer::update's step that belong to the tick, not to
// a player (world/step.cpp). The copies' iterate() ran them once per copy;
// with the World on they run once per simulated tick.

// A speed a portal parked in m_timeModRelated, taken by every copy at the top
// of the tick (update 0x238016-0x238064) and cleared.
void applyParkedSpeed(GJBaseGameLayer* pl, PlayerObject* const* copies, int count);

// The clock of one simulated tick (see the fact table in world/step.cpp).
void advanceClock(GJBaseGameLayer* pl);

// The time warp a simulated tick runs at, and how long its step is in seconds
// (the physics step shortened by a warp above 1). A run with the World takes
// the warp from its own state, which a time warp trigger 1935 it fired has
// moved (world/fire.cpp (ah)); every other run takes the live game's, as
// before. Every simulated tick's dt comes from here, so one script from one
// start always gets the same steps.
float stepTimeWarp();
float stepDt();

// ------------------------------------------------------------ the step port
// A simulated tick with the World runs, in the order of GJBaseGameLayer::update
// (fact table (h) in world/step.cpp):
//   stepTickBegin   updateSpawnTriggers, the parked speed to the copies
//   (the run)       processCommands: the command index, then the tick's input
//   stepTick        the lock inputs, timers, prepareMoveActions,
//                   processMoveActionsStep, postMoveActions
//   (the run)       the clock and each copy's own tick
//   checkSpawnObjects  the spawn walk, once, for the copy of player 1
//   endTick         the contacts no copy touched are forgotten; tick + 1
// `dt` is the step's float delta in seconds.
void stepTickBegin(Run& run, float dt, PlayerObject* const* copies, int count);
void stepTick(Run& run, float dt);
// False when the walk is not modelled for this run (a platformer level, whose
// walk goes by the level time through a game function that writes globals):
// the caller keeps the copies' own walk then.
bool checkSpawnObjects(Run& run);
void endTick(Run& run);

// What a tick with nothing running does to the state: the step is skipped
// (the idle fast path) and only the index and the tick move on.
bool idle(const WorldState& ws);

// ------------------------------------------------------------ firing (world/fire.cpp)
// EffectGameObject::triggerObject 0x4a5f30 (and the overrides of the Tier A
// classes) for the TriggerDef `defIdx`, with the remap keys the fire carries
// (applyRemap 0x21b1c0 is applied to the fields it reads, never to the object).
// `playerUid` is triggerObject's second argument (0 from the spawn walk and
// spawns); `currentDelay` is a spawn trigger's m_currentDelay (what spawnObject
// leaves in it). Kinds the World does not run mark their groups uncertain.
void fire(Run& run, int defIdx, std::span<const int> keys, int playerUid = 0, double currentDelay = 0.0);
// GJBaseGameLayer::toggleGroup 0x223bc0: a Toggle op and the run's bit.
void toggleGroup(Run& run, int group, bool activate);
// GJBaseGameLayer::spawnGroup 0x21ab80 and spawnObject 0x21b030.
void spawnGroup(Run& run, int group, bool ordered, double delay, std::span<const int> keys, int uid, int control);
void spawnObject(Run& run, int slot, double delay, std::span<const int> keys);
// GJEffectManager::updateCountForItem 0x2624b0 and countForItem 0x2623f0 on the run's items.
void updateCountForItem(Run& run, int item, int value);
int countForItem(const WorldState& ws, int item);
// GJEffectManager::updateTimers 0x263340 on the run's timers and the triggers
// waiting on them (world/fire.cpp (ad)): one step of every running timer, and
// the spawns the timers and their listeners make when they pass their times.
void updateTimers(Run& run, float dt);

// Marks a group (clamped the game's way; 0 and below are nothing) uncertain.
void markUncertain(WorldState& ws, int group);
// Marks a group whose spawn or toggle the World cannot follow: the group, and
// - since its triggers then fire when the World does not know - every group
// they name, down every spawn, toggle and item change they make in turn. A
// spread spawn delay, a kind not ported yet that spawns or toggles, a touched
// trigger and a listener nothing fires all end here; marking only the group
// they name left the moves of the triggers inside it looking modelled.
void markSpawnUncertain(const WorldDef& def, WorldState& ws, int group);
// The same for one trigger whose fire the World cannot follow, with its target
// and centre as the fire has them (remapped).
void markFireUncertain(const WorldDef& def, WorldState& ws, const TriggerDef& trigger, int target, int center);

// A copy touched a touch-triggered trigger: GJBaseGameLayer::playerTouchedTrigger
// 0x217f50 on the run's state (world/fire.cpp (z)). The copies' collision pass
// applies the speed portals, gravity, rotate-gameplay and teleport triggers to
// the copy itself (phys::triggerObject); everything else the touch fires goes
// through World::fire here, and a kind the World does not run marks the groups
// it names uncertain as before.
void touchedTrigger(Run& run, PlayerObject* copy, EffectGameObject* object);

// A copy used a custom ring - the toggle orb 1594 and the toggle block 3643,
// the two ids the game sets up as GameObjectType::CustomRing: what
// GJBaseGameLayer::activateCustomRing does, inlined in PlayerObject::ringJump
// (world/fire.cpp (x)). physics/player.cpp calls it where the game does, on
// the one path where the ring really fires. Does nothing outside a run with
// the World; the ring's groups are then uncertain as they were.
void ringActivated(RingObject* ring);

// A copy pressed or let go of the jump button: GJEffectManager::playerButton
// 0x262190 on the run's touch listeners, which is what the touch trigger 1595
// registers (world/fire.cpp (y)). Called once per button change of each copy,
// right after the tick's input, as GJBaseGameLayer::handleButton does.
void onButton(Run& run, PlayerObject* copy, bool down);

// A copy's own code reported a game event to the level (the hook on
// GJBaseGameLayer::gameEventTriggered, which keeps the copies out of the real
// effect manager): GJBaseGameLayer::gameEventTriggered 0x231ff0 on the run's
// own event listeners and stamps (world/fire.cpp (ac)). `material` and
// `playerUid` are the game's other two arguments; a copy's uid stands in for
// the player it is. Does nothing outside a run with the World.
void onEvent(int event, int material, int playerUid);

// The remap keys of a Remap as a list of their own (the pool can move while a
// fire appends to it).
std::vector<int> keysOf(const WorldState& ws, const Remap& remap);
Remap pushKeys(WorldState& ws, std::span<const int> keys);

// The Tier B check (world/selftest.cpp) found the transcribed command step
// not bit-identical with GroupCommandObject2::step: commands are stepped
// through the game's own function on a scratch object instead.
extern bool g_commandStepThroughGame;
// One GroupCommandObject2::step 0x257900 of a command, transcribed (or
// through the game, see above).
void stepCommand(WCmd& command, float dt);
void stepCommandTranscribed(WCmd& command, float dt);
void stepCommandThroughGame(WCmd& command, float dt);
// A command as GroupCommandObject2::reset 0x257700 leaves it, with `uid`.
WCmd freshCommand(int uid);

}  // namespace world
