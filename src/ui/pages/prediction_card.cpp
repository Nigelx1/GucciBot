// The Prediction card (declared in ui/pages.hpp): everything that runs on the
// fork service (analysis/trajectory.*), which asks copies of the player in
// Absense's simulation what happens next. The path preview's look follows
// Absense's own (src/ui/app/pages/prediction.cpp, Absent, GPL-3): a line for
// holding and one for letting go. Every switch and slider writes the same
// saved key the engine loads it from (GucciEngine::loadEngineSettings,
// core/engine_core.cpp), the moment it changes.

#include "ui/pages.hpp"

#include "core/GucciBot.hpp"
#include "ui/kit.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>

using namespace geode::prelude;

namespace gucci::ui::pages {

    namespace {

        // Same limits analysis/trajectory.cpp clamps to.
        constexpr int kMinPathTicks = 10;
        constexpr int kMaxPathTicks = 2000;
        // Same limits replay/subtick_preview.cpp clamps to.
        constexpr int kMinSplits = 2;
        constexpr int kMaxSplits = 524288;

        void saveSwitch(const char* key, bool value) {
            Mod::get()->setSavedValue<bool>(key, value);
        }

    } // namespace

    void predictionCard() {
        auto* gb = GucciEngine::get();
        auto& upd = gb->updater;
        auto* mod = Mod::get();

        kit::BeginCard("Prediction", "Copies of the player run ahead of it through the level to show or test what comes next.");

        // ---- the path preview
        bool changed = false;
        if (kit::FeatureBegin("Path preview", "Lines ahead of the player: green if you hold, red if you let go",
                              &gb->pathPreview, nullptr, &changed)) {
            if (kit::SliderRow("Length", "How far ahead, in ticks; at 240 TPS, 240 is one second", &gb->pathLength,
                               kMinPathTicks, kMaxPathTicks, "%d ticks"))
                mod->setSavedValue<int>("hack_trajectory_len", gb->pathLength);
            if (kit::SwitchRow("Moving objects", "Blocks and hazards that are moving keep moving along the lines. "
                                                 "Off: they stay where they are now",
                               &gb->pathMovingObjects))
                saveSwitch("hack_trajectory_moving", gb->pathMovingObjects);
            if (gb->pathMovingObjects &&
                kit::SliderRow("Move them every", "Places them every this many ticks of a line instead of every "
                                                  "tick: faster, less exact",
                               &gb->pathMoveStepInterval, 1, 30, "%d ticks"))
                mod->setSavedValue<int>("hack_trajectory_move_interval", gb->pathMoveStepInterval);
            kit::FeatureEnd();
        }
        if (changed)
            saveSwitch("hack_trajectory", gb->pathPreview);

        // ---- Frame Extrapolation
        if (kit::SwitchRow("Frame extrapolation",
                           "Above the tick rate, draws the player part of the way to where it will be next tick, "
                           "so motion looks smooth",
                           &upd.m_extrapolateFrames))
            saveSwitch("feat_frame_extrapolation", upd.m_extrapolateFrames);

        // ---- Prevent Death's look-ahead
        if (kit::SwitchRow("Stop before a death",
                           "With Prevent death on (Hacks > Engine): looks four ticks ahead every tick and pauses "
                           "before the player would die, instead of stepping back after",
                           &upd.m_fullGamePrediction))
            saveSwitch("feat_prevent_death_trajectory", upd.m_fullGamePrediction);
        if (upd.m_fullGamePrediction && !upd.m_preventDeath)
            kit::Note("Prevent death is off, so this does nothing yet.", Tone::Warn);

        // ---- Find Best Tick
        kit::Section("Best tick for the next click");
        float threshold = upd.m_acceptablePrediction * 100.f;
        if (kit::SliderRow("Good enough", "Stops at the first tick whose click lives this share of the path length",
                           &threshold, 10.f, 100.f, "%.0f%%")) {
            upd.m_acceptablePrediction = std::clamp(threshold / 100.f, 0.f, 1.f);
            mod->setSavedValue<float>("feat_best_tick_threshold", upd.m_acceptablePrediction);
        }
        bool const canSearch = PlayLayer::get() && upd.m_backwardsStepping;
        kit::RowBegin("Find it", "Steps forward a tick at a time, clicking (or letting go) on each, then steps "
                                 "back to the tick that lived longest");
        ImGui::BeginDisabled(!canSearch);
        if (kit::Button("Find the best tick", Tone::Accent)) {
            // Not from inside the menu's drawing: it steps the game. The next
            // frame runs it before anything draws.
            geode::queueInMainThread([] {
                GucciEngine::get()->updater.findBestFrameCandidate();
            });
        }
        ImGui::EndDisabled();
        kit::RowEnd();
        if (!upd.m_backwardsStepping)
            kit::Hint("Needs Backwards stepping (Hacks > Engine), to step back to the tick it finds.");

        // ---- the sub-tick preview
        kit::Section("Sub-tick preview");
        if (kit::SwitchRow("Step through the tick",
                           "While frame-advancing a sub-tick (CBF) macro or recording, each step forward moves part "
                           "of a tick and shows the hold and release paths from there",
                           &gb->replay.m_subtickPreview))
            saveSwitch("scbf_subtick_preview", gb->replay.m_subtickPreview);
        if (gb->replay.m_subtickPreview) {
            kit::RowBegin("Steps per tick", "How finely a tick is split");
            if (kit::InputInt("##splits", &gb->replay.m_subtickSplits, 100)) {
                gb->replay.m_subtickSplits = std::clamp(gb->replay.m_subtickSplits, kMinSplits, kMaxSplits);
                mod->setSavedValue<int>("scbf_subtick_splits", gb->replay.m_subtickSplits);
            }
            kit::RowEnd();
        }

        kit::EndCard();
    }

} // namespace gucci::ui::pages
