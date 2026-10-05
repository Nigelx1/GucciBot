#include "object.hpp"

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer) {
    if (!isPlatformer) {
        if (object->m_isMultiActivate) {
            return true;
        }
    } else if (!object->m_isNoMultiActivate) {
        // EnhancedGameObject::canMultiActivate (0x1a4a6f): in a platformer
        // level everything fires again unless it is marked no-multi-activate.
        return true;
    }

    return false;
}

void* hasBeenActivatedByPlayerOrig = nullptr;

// A raw Windows address nothing calls any more (see the commented-out call
// below; the check is done on the object's own fields). Off Windows it means
// nothing, so it is not worked out there (GucciBot multiplatform).
#ifdef GEODE_IS_WINDOWS
$execute {
    hasBeenActivatedByPlayerOrig =
        reinterpret_cast<void*>(geode::base::get() + 0x1a1b70);
}
#endif

// this doesn't work in trajectory for some odd reason
bool hasBeenActivatedByPlayer(PlayerObject* player,
                              EnhancedGameObject* object) {
    bool activatedPlatformer =
        phys::activatedPlatformer(object, player->m_isPlatformer);
    if (activatedPlatformer) {
        return false;
    }

    if (!player->m_isSecondPlayer) {
        return object->m_activatedByPlayer1;
    }

    return object->m_activatedByPlayer2;

    // auto func = reinterpret_cast<bool (*)(EnhancedGameObject*,
    // PlayerObject*)>(hasBeenActivatedByPlayerOrig); return func(object,
    // player);
}
}  // namespace phys
