#pragma once

// Noclip accuracy: the share of this attempt's ticks on which noclip did not
// have to block a death. Written fresh on 2026-10-03. The number it replaced
// was the level time divided by the level's length, taken once at a blocked
// death - a time over a distance - and nothing drew it.
//
// A tick counts once however many hazards it touched, and for either player.
// The attempt is GucciBot's (a reset, a respawn at a checkpoint, a new level
// start it over), so its ticks are the frame counter's since the attempt began
// (GucciUpdater::m_frame).
//
// GucciEngine::noclipThreshold ("Die below" on the Hacks page) is a floor: a
// hit that would take the attempt under it is not blocked, and the player dies
// as if noclip were off. Zero turns that off. The number is drawn by the
// indicator's readouts (hacks/indicator.cpp) while noclipAccuracyVisible is on.

namespace gucci::noclipacc {

    // hook_playlayer.cpp, where noclip is about to block a real player's death.
    // True: block it (the tick is counted as a hit). False: the hit would take
    // the attempt under the "die below" line, so let the death happen.
    bool onBlockedDeath();

    // 0..1 for this attempt so far (1 before the first tick), also kept in
    // GucciEngine::noclipAccuracy.
    float accuracy();
    // Ticks this attempt on which a death was blocked.
    int hits();

} // namespace gucci::noclipacc
