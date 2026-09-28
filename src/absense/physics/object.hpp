#pragma once

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer);

// this doesn't work in trajectory for some odd reason
bool hasBeenActivatedByPlayer(PlayerObject* player, EnhancedGameObject* object);
}  // namespace phys
