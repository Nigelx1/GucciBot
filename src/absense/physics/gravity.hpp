#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
// PlayerObject::flipGravity 0x39a1d0 on its own, without the dual linking the
// layer's caller adds (0x212b3f). The game calls it directly where the other
// player's gravity is already being set - copyAttributes and toggleDualMode -
// and it does nothing there when the player is already that way up.
void flipGravityInner(PlayerObject* player, bool gravity);
void flipGravity(PlayerObject* player, bool gravity);
void propellPlayer(PlayerObject* player, float force, bool playBumpEffect,
                   int p2);
void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object);
}  // namespace phys
