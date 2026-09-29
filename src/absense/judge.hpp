#pragma once

// The real game as the judge of the simulation (GucciBot's, not Absense's).
//
// Absense's look-ahead trusts its simulation: when every idea dies there, the
// spot is a dead end. On Zafari 2 (2026-09-28) the simulation said every route
// through a mini-ship gap died, the pathfinder dove under two black boxes on
// every try, and the real game was never asked about the route over them.
// This is the asking: play a script on the real player from where the game is
// now, one tick at a time, catch the death instead of dying, and put the game
// back exactly as it was. Plus a list of killers the real game has shown the
// simulation to be wrong about, which the simulation's copies then ignore.

#include "absense/trajectory/trajectory.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

class GameObject;
class PlayerObject;

namespace absense::judge {

    struct Result {
        bool ran = false;         // false: nothing to run it on (see error)
        std::string error;
        int survived = 0;         // whole ticks survived; a death is in tick survived + 1
        bool died = false;
        bool complete = false;    // reached the end of the level
        int killerId = 0;         // the object that killed the player (0: none named)
        int killerUid = 0;
        int killerType = 0;
        float killerX = 0.f, killerY = 0.f;
        // Right after the game was put back (diagnostics: the frame should be
        // the one it started at, and no step should be left armed).
        uint32_t startFrame = 0;
        uint32_t frameAfter = 0;
        float xBefore = 0.f, xAfter = 0.f;
        bool stepArmedAfter = false;
        int stepLeftArmed = 0;          // stepped ticks after which the step flag was still set
        bool onlyRefreshAtStart = false;  // the updater was in its refresh-only state
    };

    // Plays `inputs` (one per tick; the last repeats to `ticks`) on the real
    // player, the way the pathfinder commits a tick, recording each tick into
    // `trace`. Nothing is recorded into the macro and nothing is stored for
    // Backwards Stepping; afterwards the game is put back at the state it
    // started from (its frame, its player, its level) and left paused or not
    // as it was. Main thread, outside the game's own update of the tick.
    Result run(std::span<const TickInput> inputs, int ticks, std::vector<TraceSample>* trace);

    // For the hooks: while a run is on, the real player's death and the end of
    // the level are noted here instead of happening.
    bool active();
    void noteDeath(GameObject* object);
    void noteComplete();

    // Killers the simulation is wrong about: a copy that touches one of these
    // (by m_uniqueID) does not die of it.
    void distrust(int uid);
    void clearDistrust();
    bool distrusted(int uid);
    std::vector<int> distrustedList();

}  // namespace absense::judge
