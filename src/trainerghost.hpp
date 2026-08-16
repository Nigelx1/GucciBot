#pragma once

class PlayLayer;

// General-purpose "Trainer" tab counterpart to jupiterghost.hpp -- same
// ghost overlay + synced-music toolset, but scoped to whichever macro the
// user has picked into GucciEngine::trainerMacro instead of the one bundled
// Jupiter macro. Deliberately a parallel, duplicated implementation rather
// than a parameterized/shared one: JupiterGhostOverlay/JupiterMusicSync are
// stateful singletons wired straight into PlayLayer's init/onQuit, and this
// project's own history (see CLAUDE.md) is that stacking risk onto small,
// already-working, game-integration-sensitive hooks is exactly how things
// break. Keeping this fully separate means nothing here can regress JMF.
namespace gbtr {
    // True if pl's level matches trainerMacro's recorded level name. Fails
    // OPEN (returns true) if trainerMacro has no recorded level name at all
    // (common for macros converted from .gdr/.json/legacy .brr, since
    // nothing in this codebase sets BRRMacro::levelName before persisting
    // those formats) -- Stats/Ghost should still work for those macros
    // rather than silently never activating; the GUI shows a caveat badge
    // instead so this isn't silently wrong.
    bool isTrainerLevel(PlayLayer* pl);

    void renderTrainerGhost(PlayLayer* pl);
    // Call from PlayLayer::destroyPlayer/levelComplete when a real attempt
    // (not a bot playback, not an analysis probe) has just ended, so the
    // "own best attempt" ghost can be updated if this run went further.
    void notifyTrainerAttemptEnded();

    void syncTrainerClickBarMusic(bool active, bool paused, double posSec);
    void stopTrainerClickBarMusic();
}
