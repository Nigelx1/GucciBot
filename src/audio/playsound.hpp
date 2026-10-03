#pragma once

namespace gucci {

    // Click sounds (audio/clicks.hpp), rebuilt from scratch on 2026-10-02
    // after the ToastyReplay-derived ones were removed. Called on every button
    // event the game handles, the player's and a playing macro's alike; plays
    // a click or release for jump (button 1) when click sounds are on, and
    // otherwise does nothing. Defined in audio/clicks.cpp.
    void triggerClickAudio(bool p2, int button, bool pressed);

} // namespace gucci
