#pragma once

// The level reader the GPU card and the route atlas share (client.cpp): what
// a game object is to the pathfinder (protocol.hpp's ObjectKind, as a box or
// a circle), and whether it is one of the objects past which a simplified
// model is playing a different game (dual, solo and teleport portals,
// teleport orbs, custom rings, the gravity, rotate-gameplay and teleport
// triggers).

#include "absense/gpu/protocol.hpp"

class GameObject;

namespace absense::gpu {

    // `evenIfOff`: what the object would be once a trigger switches it on
    // (an object that is off now is otherwise nothing).
    bool classifyObject(GameObject* o, Object& out, bool evenIfOff = false);
    bool objectChangesEverything(GameObject* o);

}  // namespace absense::gpu
