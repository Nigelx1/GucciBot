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

    // Synced music, click-bar-preview path: called from
    // MenuInterface::drawJupiterClickTrainerPage every frame that page is
    // open, so the music is audible while previewing the click bar too, not
    // just during live gameplay on the level itself (that's the OTHER path,
    // driven by renderJupiterGhost's per-gameplay-frame hook -- it takes
    // priority automatically if a real Jupiter attempt is live at the same
    // time). posSec is the click bar's own transport position.
    void syncClickBarMusic(bool active, bool paused, double posSec);
    // Explicit stop, called when navigating away from the Click Trainer page
    // (the Back button) so the click-bar-preview music doesn't keep playing
    // once nothing is rendering position updates for it anymore.
    void stopClickBarMusic();
}
