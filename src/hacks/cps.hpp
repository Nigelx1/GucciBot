#pragma once

// Clicks per second: jump presses in the last second of level time, per
// player, and the most there have been since the attempt started. Ported
// from Silicate's CPS counter (silicate/src/assist/cps.cpp, by peony,
// GPL-3.0 - GucciBot's licence too), for the HUD's CPS readouts (issue #28).
//
// One difference: Silicate drops presses older than a second from its tick
// loop, a Windows-only midhook here; this drops them whenever the count is
// read or added to, which comes out the same and works on every platform.
//
// It counts the player's own jump presses while idle or recording, and the
// macro's while playing, like Silicate. A reset starts it over.

namespace gucci::cps {

    // A jump press, now, by player 1 or player 2.
    void push(bool player2);
    // Presses in the last second, and the most in any second this attempt.
    int now(bool player2);
    int most(bool player2);
    // A new attempt.
    void clear();

} // namespace gucci::cps
