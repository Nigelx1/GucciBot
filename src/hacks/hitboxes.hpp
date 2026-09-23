#pragma once
#include <Geode/Geode.hpp>
using namespace geode::prelude;

namespace gucci {

    // NOT the hitbox implementation. The real one is hitboxes.cpp -- 593 lines
    // of OverlayPainter / VisibleObjectSnapshot / TrailHistory / ShapeExtractor
    // behind HitboxOverlayState, driven by its own $modify hooks on
    // GJBaseGameLayer and PlayLayer reading showHitboxes, hitboxTrail and
    // hitboxOnDeath directly. Show Hitboxes works.
    //
    // This class is a vestigial shim: these methods are deliberately empty and
    // exist only so the older call sites (resetLevel, the editor layer) still
    // compile. Read as-is it looks exactly like a feature that was never
    // implemented -- it fooled me in 2026-09-22 and nearly got the whole thing
    // replaced with a port of Silicate's. Nigel caught it. Open the .cpp.
    class HitboxOverlay {
    public:
        static HitboxOverlay* get() {
            static HitboxOverlay inst;
            return &inst;
        }
        void draw(PlayLayer* pl) {
            (void)pl;
        }
        void clearTrail() {}
        void init(PlayLayer* pl) {
            (void)pl;
        }
        void destroy() {}
    };

} // namespace gucci
