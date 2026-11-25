#pragma once

// GucciBot (2026-11-24, 2.0.1): practice checkpoints GD placed with its own
// code, off Windows (the code is in hook_playlayer.cpp).
//
// A STOPGAP, not tick-exact, behind one switch: GB_GD_CHECKPOINT_POLL in
// core/platform.hpp says what it gets wrong and how to rip it out.
//
// On an iPhone (the Geode iOS app, iOS 27) a checkpoint placed by hand never
// reached GucciBot's checkpoint hooks (UILayer::onCheck,
// PlayLayer::queueCheckpoint, PlayLayer::processCheckpoints, then the frozen
// tick's markCheckpoint and storeCheckpoint): GD kept it in its own list
// (m_checkpointArray), which GucciBot's storeCheckpoint never fills. At the
// next death GucciBot found no checkpoint of its own and took the respawn for
// a restart (frame 0, the recording started over), while GD put the player
// back at its checkpoint. The finished macro held only the last stretch,
// saved as if it started at 0%. The player's HUD showed it: Frame 182 at the
// checkpoint, 0 after the death.
//
// So GucciBot looks at GD's own list right after GD's processCheckpoints and
// after every tick. A checkpoint there that GucciBot didn't save is saved in
// the next frozen tick, between two updates, under that frame: the same
// saveCurrent, at the same point, as the hooks' own checkpoints (saved at
// once, inside GD's update, the first tick after a respawn to it didn't move
// the player in a hidden test, so the rest of the run came out a tick late).
// It stays in GD's list, as GD put it there: one GD takes out again itself
// (its own delete, removing them all, practice mode off) comes off GucciBot's
// list too, and GucciBot's own delete takes one out of GD's. A respawn then
// goes through the normal checkpoint branch. Windows is unchanged, in both files.

#include "core/platform.hpp"

class PlayLayer;
class CheckpointObject;

namespace gucci::gdcheckpoints {

#if GB_GD_CHECKPOINT_POLL
    // After every tick (GBPortableTick, core/engine_updater.cpp).
    void sync(PlayLayer* pl);
    // GucciBot's removeCheckpoint took this one off its own list: off GD's
    // too, if it came from there.
    void forget(PlayLayer* pl, CheckpointObject* cp);
    // A level starting or closing: the last level's are gone with it.
    void clear();
#endif

#if defined(GB_FORCE_PORTABLE)
    // TEST ONLY, GB_FORCE_PORTABLE builds (never shipped): GucciBot's
    // checkpoint hooks step aside so GD does the job with its own code, as its
    // iOS build seems to. 1: the hooks that place one (UILayer::onCheck,
    // processCheckpoints' catch, markCheckpoint, storeCheckpoint; on Windows
    // GD's processCheckpoints calls markCheckpoint as a real function, so the
    // last two have to step aside too). 2: also the ones that take them away
    // (removeCheckpoint, removeAllCheckpoints). Assistant Access's test key
    // test_gd_checkpoints.
    extern int g_bypass;
    inline bool bypassPlacing() {
        return g_bypass >= 1;
    }
    inline bool bypassRemoving() {
        return g_bypass >= 2;
    }
#else
    constexpr bool bypassPlacing() {
        return false;
    }
    constexpr bool bypassRemoving() {
        return false;
    }
#endif

} // namespace gucci::gdcheckpoints
