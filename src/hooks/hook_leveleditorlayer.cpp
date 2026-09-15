#include "core/GucciBot.hpp"
#include "analysis/trajectory.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/hitboxes.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

using namespace geode::prelude;
using namespace gucci;

// Editor playtests never go through PlayLayer, so none of the reset work in
// hook_playlayer.cpp's resetLevel runs when one starts: the frame and death
// state, the replay's input position and any held button all carried over from
// whatever happened before. Silicate has had this hook for a long time
// (src/hooks/LevelEditorLayer.cpp); GucciBot's updater was ported from Silicate
// but this file never came with it. This is that hook, mapped onto GucciBot's
// names.
class $modify(GB7LevelEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        auto* gb = GucciEngine::get();
        if (!gb->enabled) {
            LevelEditorLayer::onPlaytest();
            return;
        }

        auto& upd = gb->updater;
        upd.m_tpsOverflow = 0.0;
        upd.m_frameOnLastAttempt = 0;
        upd.setPaused(false);
        upd.resetFrame();

        // Editor song preview keeps playing into the playtest otherwise.
        // onPlayback toggles it off; stopPlayback does not.
        if (auto* ui = EditorUI::get(); ui && ui->m_isPlayingMusic)
            ui->onPlayback(nullptr);

        m_playbackActive = false;

        LevelEditorLayer::onPlaytest();

        // Same as a full restart in a normal level: playback rewinds to the
        // first input, recording starts over from frame 0.
        gb->replay.onReset(0, 0);
        Autoclicker::get()->reset();
        TrajectoryPredictionService::get().markDirty();
        HitboxOverlay::get()->clearTrail();

        // Release jump for both players so a button held going into the
        // playtest does not carry over. These are not inputs, so they must
        // not end up in a recording.
        gb->suppressInputCapture = true;
        handleButton(false, (int)PlayerButton::Jump, true);
        handleButton(false, (int)PlayerButton::Jump, false);
        gb->suppressInputCapture = false;

        upd.m_canDie = false;
        upd.m_inputIsDeath = false;
        upd.m_expectsDeath = false;
    }
};
