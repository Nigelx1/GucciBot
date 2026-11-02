#pragma once

// The random teleport's pick as plain functions, without Geode, so the
// offline tests can check them (tools/run_teleport_tests.cpp).
//
// GJBaseGameLayer::teleportPlayer (2.2081 Windows, 0x20fe8a-0x20ff0b): a
// portal with no linked exit sends the player to one member of its target
// group. With more than one member GD draws r from a generator of its own,
// takes f = r / 32767.0f (0 when that comes out at exactly 1), and picks
// member (int)(count * f), all in single precision. On Windows the
// teleportRandomOverride midhook (hooks/hook_gjbasegamelayer.cpp) replaces r
// with the next value of the replay system's teleport state, and the state
// keeps only that 15-bit value. Off Windows hooks/portable_teleport.cpp makes
// the same pick from the same state with these, and Absense's look-ahead
// copies draw the same way (world::nextRandom).

#include <cstdint>

namespace gucci::teleportpick {

    // The teleport state after one draw. The drawn value is the new state
    // itself, 0 to 32767: what teleportRandomOverride does to it.
    inline uint64_t nextState(uint64_t state) {
        return static_cast<uint64_t>(static_cast<int>((214013 * state + 2531011) >> 16) & 0x7FFF);
    }

    // The member GD takes out of `count` for the drawn value `r`
    // (teleportPlayer 0x20fee0-0x20ff04: cvtdq2ps, divss by 32767.0f, the
    // compare with 1.0f, mulss by the count, cvttss2si).
    inline int memberIndex(int count, int r) {
        float f = static_cast<float>(r) / 32767.0f;
        if (f == 1.0f)
            f = 0.0f;
        return static_cast<int>(static_cast<float>(count) * f);
    }

} // namespace gucci::teleportpick
