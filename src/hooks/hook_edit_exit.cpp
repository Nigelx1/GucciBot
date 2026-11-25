// GucciBot (2026-11-23, 2.0.1): GD's Edit exits.
//
// The pause menu's Edit (PauseLayer::onEdit, then goEdit) and the end screen's
// Edit (EndLevelLayer::onEdit, then goEdit) take a level to the editor without
// PlayLayer::onQuit. In GD 2.2081 (Windows binary) PlayLayer::onQuit has five
// callers (the pause menu's and the end screen's exits, setupHasCompleted,
// levelComplete and RetryLevelLayer); neither goEdit is one, both go to
// GJGameLoadingLayer::transitionToLoadingLayer instead. The buttons show on
// your own levels, and Eclipse's Level Edit puts the pause menu's on every
// level. So none of what GucciBot does as a level quits ran: the quit's stop
// for a render, Calculate (and its walk back, 2.0.1) and the pathfinders;
// emptying the practice lists and putting the replay's input position back;
// the Autoclicker's reset; the
// calibration's cancel; stopping the JMF or Trainer music, under which GD's
// own sound stayed muted; and the overlays' nodes, which the level freed (the
// iPhone crash of 2026-11-23).
//
// Silicate 1.1.0 hooks PauseLayer::goEdit for this (src/hooks/PauseLayer.cpp,
// by peony, GPL-3): it unpauses its updater, stops a render and takes its
// trajectory and hitboxes down before GD's goEdit, and empties its practice
// lists and resets the replay's input after it, as its onQuit does. Here it is
// with the whole of GucciBot's quit (hooks/level_exit.hpp and the other
// PlayLayer::onQuit hooks' work), and the same for the end screen's goEdit,
// which Silicate doesn't hook.
//
// The overlays don't depend on this: they find their nodes gone after any
// exit that skips the quit (core/level_nodes.hpp). Here they just let go of
// them sooner.

#include "core/GucciBot.hpp"
#include "hacks/hitborders.hpp"
#include "hacks/hitboxes.hpp"
#include "hooks/level_exit.hpp"
#include "trainers/calibration.hpp"
#include "trainers/jupiterghost.hpp"
#include "trainers/trainerghost.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;
using namespace gucci;

namespace {

    // The quit's work that needs the level still current: before GD's goEdit.
    // Every step is safe to run again.
    void beforeEdit(PlayLayer* pl, char const* which) {
        log::info("[GucciBot] Edit ({}): the level goes to the editor without its quit; doing the quit's "
                  "cleanup",
                  which);
        levelexit::beforeQuit(pl);
        // The other PlayLayer::onQuit hooks' work, each from its own file.
        gbmp::detachMacroPath();
        gbju::leaveLevel();
        gbtr::leaveLevel();
        gbpr::detachPracticeRange();
        HitboxOverlay::get()->destroy();
        HitborderOverlay::get()->destroy();
        CalibrationService::get().cancel();
        // Frame advance off, as Silicate's goEdit does. Frame advance holds
        // the whole cocos scheduler while a level or the editor is open
        // (GucciUpdater::runUpdates), so the editor would open with nothing
        // in it moving; GucciBot's playtest hook only lets go of it once a
        // playtest starts. Last here, where Silicate has it first: stopping a
        // Calculate run or Absense's pathfinder (beforeQuit) puts back the
        // frame advance it found when it started (FrameWindowAnalyzer::finish,
        // AbsensePathfinder::stop), which turned it on again, and stopping
        // Calculate's walk back (stopReturn) leaves it on.
        GucciEngine::get()->updater.setPaused(false);
    }

} // namespace

class $modify(GBEditExitPause, PauseLayer) {
    void goEdit() {
        auto* pl = PlayLayer::get();
        if (pl)
            beforeEdit(pl, "pause menu");
        PauseLayer::goEdit();
        if (pl)
            levelexit::afterQuit();
    }
};

class $modify(GBEditExitEnd, EndLevelLayer) {
    void goEdit() {
        // GD's goEdit sets m_playLayer to null as it ends: read it first.
        auto* pl = m_playLayer ? m_playLayer : PlayLayer::get();
        if (pl)
            beforeEdit(pl, "end screen");
        EndLevelLayer::goEdit();
        if (pl)
            levelexit::afterQuit();
    }
};
