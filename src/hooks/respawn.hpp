#pragma once

// Custom respawn delay, ported from Absense (hooks/RespawnTime.cpp, by Absent).
//
// After a death GD schedules its own respawn: a delayed action on the PlayLayer
// (tag 0x10) that calls delayedResetLevel about a second later. retime() swaps
// that action for one with our delay -- the way GDH does it -- so the level
// resets exactly once, when we want.
//
// This replaces GucciBot's old Auto Retry, which counted down on its own and
// then called resetLevel() while GD's action was still pending. Whichever came
// second reset the level again, a moment into the new attempt.

class PlayLayer;

namespace gucci::respawn {

    // Call right after the game has handled a real death. No-op if nothing is
    // scheduled (not a real death) or the bot is disabled.
    void retime(PlayLayer* layer, float delaySeconds);

} // namespace gucci::respawn
