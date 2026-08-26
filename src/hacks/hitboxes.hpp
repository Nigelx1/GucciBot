#pragma once
#include <Geode/Geode.hpp>
using namespace geode::prelude;

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
