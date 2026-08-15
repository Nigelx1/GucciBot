#pragma once

class PlayLayer;

// Draws the macro's recorded ghost (and optionally your own best-attempt
// ghost) in the game world during Jupiter tab practice. Pure rendering --
// never touches player state, camera, or the checkpoint/reset system. See
// GucciEngine's jupiterGhost*/jupiterScrub* fields (GucciBot.hpp) for the
// controls, and jupiterghost.cpp for why this is architected as a strict
// attach/render/detach overlay mirroring PracticeRangeOverlay's pattern.
namespace gbju {
    void renderJupiterGhost(PlayLayer* pl);
    // Call from PlayLayer::destroyPlayer/levelComplete when a real attempt
    // (not a bot playback, not an analysis probe) has just ended, so the
    // "own best attempt" ghost can be updated if this run went further.
    void notifyJupiterAttemptEnded();
}
