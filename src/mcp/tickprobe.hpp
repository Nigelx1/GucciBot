#pragma once

// gucci_tick_probe: player 1's settled state at every tick, so two runs of the
// same inputs -- Absense's pathfinder playing a level, and a replay of what it
// recorded -- can be compared tick by tick. Recorded from frameUpdateMidhook
// only while the tool has armed it; a tick played again after going back
// overwrites the old one, so the store holds the path as it finally went.

#include <cstdint>
#include <map>

namespace gucci::tickprobe {

    struct Sample {
        float x = 0.f;
        float y = 0.f;
        float yVel = 0.f;
        float rot = 0.f;
        bool held = false;
        bool onGround = false;
        char mode = 'C';
    };

    inline bool armed = false;
    inline std::map<uint32_t, Sample> current;
    inline std::map<uint32_t, Sample> saved;

}  // namespace gucci::tickprobe
