#pragma once

// Absense - where the World has the level's objects while the graphics card
// looks at a decision (design step 13).
//
// The card is told about a slice of the level once and then asked about it
// for a whole decision, so anything on the move has to go out with how fast
// it is moving and the shader carries it on from there. Reading each object's
// own last step (the fallback, as MovingObjects does it) only knows what the
// game did on the tick before. With the World on, a copy of the live state is
// stepped forward with no copies and nothing of the game written, which gives
// the objects where an eased move really has them in the middle of the
// horizon and how fast they are going there - the middle, so the carry either
// way over the horizon is as short as it can be.
//
// Nothing here writes the game or keeps anything: it is a read, like the
// simulation's own import.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <unordered_map>

class GJBaseGameLayer;

namespace world {

// Where one object stands at the slice's tick and how far it moves in one
// tick there.
struct CardPose {
    double x = 0.0;
    double y = 0.0;
    float dx = 0.0f;
    float dy = 0.0f;
};

// What the World says about the objects of a slice.
struct CardSlice {
    // The World stepped and `poses` is what it found. Without it the caller
    // reads the objects' own last step, exactly as it did before the World.
    bool ok = false;
    // Ticks past the game's own tick the poses stand at (the middle of the
    // horizon): the slice's own tick is the game's plus this.
    int ahead = 0;
    // Something in the level is on the move, so the slice goes stale as the
    // game ticks on (the client sends it again when it has drifted far).
    bool moves = false;
    // By object unique id (m_uniqueID), for the objects the World moves or
    // carries. Everything else stands where the level has it.
    std::unordered_map<int, CardPose> poses;

    const CardPose* find(int uid) const {
        const auto it = poses.find(uid);
        return it == poses.end() ? nullptr : &it->second;
    }
};

// A copy of the live World state stepped `ahead` ticks with no copies, and
// where that leaves the objects it moves. Not ok when the World is off, the
// level has no def yet, or the live state does not carry the running actions.
// Never throws, never writes the game, never stops a caller.
CardSlice cardSlice(GJBaseGameLayer* pl, int ahead, float dt);

}  // namespace world
