// The Calculate page: settings for anticroom's frame-window analyzer
// (src/analysis/ac/), and a Run card for it. Written fresh for the 2026-10
// menu. The grouping, labels and most help texts follow anticroom's own
// Frame Windows tab in his Silicate fork (src/ui/manager.cpp) -- Overlay,
// Sound, Bands, Precision, Measurement, CBF Counting, Releases, Run -- with
// the everyday settings first and his run-speed and debug options under
// Advanced. Credit for those texts is his.
//
// Every control writes straight into SLSettings::get()->frameWindow, which the
// analyzer reads live, and the page saves the lot through fwstore::save() at
// the end of any frame where something changed (analysis/ac/fw_settings_store).
// The slider bounds come from the same table save() clamps with.
//
// The Run card calls the same GucciEngine::analyzeFrameWindows() as the Macro
// page's Calculate button, so both start a run the same way (the pause-menu
// dismissal and the conflict log live there, not here).

#include "core/platform.hpp"
#include "ui/pages.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/fw_settings_store.hpp"
#include "analysis/ac/lstar.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/general.hpp>
#include <imgui.h>

#include <algorithm>
#include <set>
#include <string>

namespace gucci::ui::pages {

    namespace {

        using FW = FrameWindowSettings;

        FW& S() { return SLSettings::get()->frameWindow; }
        FrameWindowAnalyzer& analyzer() { return ::Bot::get()->frameWindow(); }

        // What changed this frame. Settings are saved once at the end of the
        // page; a change to how windows look also redraws the HUD and markers
        // (otherwise the HUD keeps its old bands until the next attempt), and
        // a change to an L* input makes the solved number stale.
        bool g_changed = false;
        bool g_look = false;
        bool g_lstar = false;

        // Page state, not settings.
        bool g_showAdvanced = false;
        bool g_askClear = false;
        bool g_askReset = false;
        std::set<int> g_openBands;

        constexpr const char* kClearId = "##fw-clear";
        constexpr const char* kResetId = "##fw-reset";

        // ---- rows bound to a setting

        bool flag(const char* label, const char* hint, bool FW::* m, bool look = false) {
            if (!kit::SwitchRow(label, hint, &(S().*m)))
                return false;
            g_changed = true;
            g_look |= look;
            return true;
        }

        bool feature(const char* label, const char* hint, bool FW::* m, bool look = false) {
            bool flipped = false;
            bool const open = kit::FeatureBegin(label, hint, &(S().*m), nullptr, &flipped);
            if (flipped) {
                g_changed = true;
                g_look |= look;
            }
            return open;
        }

        bool slider(const char* label, const char* hint, int FW::* m, const char* fmt = "%d", bool look = false) {
            auto const lim = fwstore::limits(m);
            if (!kit::SliderRow(label, hint, &(S().*m), lim.lo, lim.hi, fmt))
                return false;
            g_changed = true;
            g_look |= look;
            return true;
        }

        bool slider(const char* label, const char* hint, float FW::* m, const char* fmt = "%.2f", bool look = false) {
            auto const lim = fwstore::limits(m);
            if (!kit::SliderRow(label, hint, &(S().*m), lim.lo, lim.hi, fmt))
                return false;
            g_changed = true;
            g_look |= look;
            return true;
        }

        bool number(const char* label, const char* hint, double FW::* m, double step, const char* fmt) {
            kit::RowBegin(label, hint);
            bool const changed = kit::InputDouble(label, &(S().*m), step, fmt);
            kit::RowEnd();
            if (changed)
                g_changed = true;
            return changed;
        }

        // ---- L*

        // The in-level L* readout solves for itself whenever the solver is
        // marked dirty, and records each input's frame while it does so it
        // can count them off as the player passes them. A solve started from
        // here would skip that bookkeeping, so when the readout is going to
        // run, the button just marks the solver dirty and lets it.
        bool readoutWillSolve() {
            auto const& s = S();
            return s.enabled && s.lstarEnabled && s.lstarHud && PlayLayer::get();
        }

        void solveLStar() {
            auto* solver = lstar::Solver::get();
            if (readoutWillSolve()) {
                solver->markDirty();
                return;
            }
            auto const& s = S();
            auto& fw = analyzer();
            lstar::Settings ls;
            ls.m_tps = fw.resultsTps();
            ls.m_respawnSeconds = s.lstarRespawn;
            ls.m_targetSeconds = s.lstarTarget > 0.0 ? s.lstarTarget : 86400.0;
            ls.m_nerve = s.lstarNerve;
            ls.m_fatigue = s.lstarFatigue;
            ls.m_cps = s.lstarCps;
            ls.m_useNerve = s.lstarUseNerve;
            ls.m_useFatigue = s.lstarUseFatigue;
            ls.m_useCps = s.lstarUseCps;
            solver->start(fw.precisionInputs(), ls);
        }

        // ---- cards

        void runCard() {
            auto* gb = GucciEngine::get();
            auto& fw = analyzer();
            auto const& actions = gb->replay.m_actionAtom.m_actions;

            kit::BeginCard("Calculate", "Measures how far each input of the loaded macro can move and still pass. "
                                         "The whole frame-window trend started with NaN GD.");

            if (fw.running()) {
                kit::Progress(fw.progress(), fw.status().c_str());
                kit::Gap(0.25f);
                if (kit::Button("Cancel", Tone::Bad))
                    gb->cancelAnalysis();
            } else if (fw.returning()) {
                std::string const back = fmt::format("Back to frame {}", fw.returnFrame());
                kit::Progress(fw.returnProgress(), back.c_str());
                kit::Gap(0.25f);
                if (kit::Button("Stop"))
                    fw.stopReturn();
            } else {
                bool const canRun = PlayLayer::get() && !actions.empty();
                ImGui::BeginDisabled(!canRun);
                if (kit::Button("Calculate", Tone::Accent))
                    gb->analyzeFrameWindows();
                ImGui::EndDisabled();
                if (!fw.results().empty()) {
                    ImGui::SameLine();
                    if (kit::Button("Copy info")) {
                        bool const ok = geode::utils::clipboard::write(fw.describe());
                        gb->fwAcReport = ok ? "Copied the window info." : "Couldn't write to the clipboard.";
                        gb->fwAcOk = ok;
                    }
                    ImGui::SameLine();
                    if (kit::Button("Clear", Tone::Bad))
                        g_askClear = true;
                }
                if (!canRun)
                    kit::Hint("Needs a level open and a macro loaded.");
                else if (!fw.results().empty())
                    kit::Hint("Copy info: the precision and window breakdown in the format NaN uses in video "
                              "descriptions, grouped by your bands.");
                // Mods that step frames themselves throw the numbers off; this
                // was only written to the log.
                if (auto const& conflicts = gb->analyzerConflicts(); !conflicts.empty())
                    kit::Note(conflicts.c_str(), Tone::Warn);
                static bool s_mute = Mod::get()->getSavedValue<bool>("calc_mute", false);
                if (kit::SwitchRow("Mute while it runs", "Silences the music and sounds until it finishes", &s_mute))
                    Mod::get()->setSavedValue("calc_mute", s_mute);
            }

            if (!fw.results().empty())
                kit::Hint(fmt::format("{} measured, {} skipped, {} desynced", fw.measuredCount(), fw.skippedCount(),
                                      fw.desyncCount())
                              .c_str());
            if (!fw.running() && !gb->fwAcReport.empty())
                kit::Note(gb->fwAcReport.c_str(), gb->fwAcOk ? Tone::Muted : Tone::Warn);

            kit::EndCard();
        }

        // What a Calculate preset does (fwstore::applyPreset), for its row's hint.
        const char* presetHint(int preset) {
            switch (preset) {
                case 0:
                    return "Balanced, the default: the strict count. A shift counts only if the macro survives it";
                case 1:
                    return "Accuracy: Recovery Range at 2. A shift that dies still counts if moving the next input up "
                           "to 2 frames saves it. Slightly slower";
                case 2:
                    return "Absolute: Accuracy, and each test runs on past the next input (Check past next input, "
                           "under Advanced). The most thorough and the slowest; experimental";
                default:
                    return "Custom: the settings below match none of the presets. A preset sets only the algorithm, "
                           "the recovery window and Check past next input";
            }
        }

        void measurementCard() {
            auto& s = S();
            kit::BeginCard("Measurement", "How an input is judged.");

            // GucciBot (2026-10-23): presets, after anticroom's antics-counter (beta.4's Accuracy, beta.7's
            // Absolute; he dropped Fast). Read off the settings every frame, so Custom when none matches.
            int preset = fwstore::matchingPreset();
            if (kit::ChoiceRow("Preset", presetHint(preset), &preset, fwstore::kPresetNames, fwstore::kPresetCount))
                fwstore::applyPreset(preset);

            const char* algos[] = {"Time-Based", "Recovery Range"};
            // GucciBot (2026-10-23): the hint said "Stricter", from anticroom's old tooltip, but Recovery Range only
            // ever rescues a shift, so it widens windows; his antics-counter beta.4 changelog says as much.
            if (kit::ChoiceRow("Algorithm",
                               "Time-Based counts a shift only if the macro survives it. Recovery Range also counts "
                               "one that dies if moving the next input up to the recovery window either way saves "
                               "it, so it is more lenient, and slower.",
                               &s.algorithm, algos, 2))
                g_changed = true;
            if (s.algorithm == static_cast<int>(FrameWindowAnalyzer::Algorithm::RecoveryRange))
                slider("Recovery window", "How far the next input can be moved while looking for a recovery",
                       &FW::recoveryRange, "%d frames");

            // GucciBot (2026-10-23): "Sweep range" until now. anticroom renamed it in antics-counter beta.6 so it
            // isn't mistaken for Max Window; it is saved under the same key (fwac_sweep_range) as before.
            slider("Search distance",
                   "How many frames on either side of each input to test. Testing stops at the first death, so a "
                   "wide range is still fast on tight inputs",
                   &FW::sweepRange, "%d frames");
            // GucciBot (2026-10-23): Max Window, from anticroom's antics-counter beta.6 changelog.
            slider("Max window",
                   "Stops counting an input once its window reaches this many frames and marks it N+, so a lenient "
                   "input doesn't take a whole search. Off counts every window out to the search distance",
                   &FW::maxWindow, s.maxWindow == 0 ? "off" : "%d frames");
            // GucciBot (2026-10-23): this said the player "must survive" this long; it only caps each test, as
            // anticroom's own tooltip says.
            slider("Horizon",
                   "The longest a test runs after a shifted input. A test ends Slack frames before the next input, "
                   "so this only cuts it short when that input is far off. 240 is one second at 240 TPS",
                   &FW::maxFrames, "%d ticks");
            slider("Slack",
                   "Ends a test early when the next input is this close, so it isn't judged on frames that belong "
                   "to the next input",
                   &FW::slack, "%d frames");

            flag("Full range sweep",
                 "Keeps testing past the first death. Slower, and survivors past a gap still aren't counted",
                 &FW::fullRangeSweep);
            // GucciBot (2026-10-23): Entry sweep and Dependent pair search both pick their inputs by the Tight
            // threshold whatever else is on (advanceEntryPass, beginDependentPass), and the Sub-tick (CBF) counting
            // card shows that slider only while it counts tight inputs alone. Otherwise the slider shows here,
            // under the two switches, while either is on, and their hints say where it is in each case.
            bool const tightOnCbf = s.subframeProbe && !s.subframeAll;
            char const* const tightWhere =
                tightOnCbf ? ", on the Sub-tick (CBF) counting card" : ", which shows below while this is on";
            std::string const entryHint =
                std::string("Re-tests tight inputs with the previous input placed at each edge of its window and "
                            "reports the span. About 3x the work. Tight means within the Tight threshold") +
                tightWhere;
            flag("Entry sweep", entryHint.c_str(), &FW::entrySweep);
            // GucciBot (2026-10-23): reworked after anticroom's antics-counter beta.6 changelog (framewindow.cpp,
            // above beginDependentPass); the hint says what it does now.
            std::string const dependentHint =
                std::string("Re-measures each tight input with the input just before it moved to the edges of its "
                            "own window, when that input is within the search distance. If that changes its window "
                            "by more than a frame, the marker shows ~N, its average window where it can still be "
                            "hit. Tight means within the Tight threshold") +
                tightWhere;
            flag("Dependent pair search", dependentHint.c_str(), &FW::dependentSearch);
            if (!tightOnCbf && (s.entrySweep || s.dependentSearch))
                slider("Tight threshold",
                       "Entry sweep and Dependent pair search only re-test inputs with a window this tight or "
                       "tighter. The same setting as the Tight threshold on the Sub-tick (CBF) counting card",
                       &FW::tightThreshold, "%d frames");
            if (feature("Suggestions for suspicious clicks",
                        "Calculate's count stays strict. A tight click right after another input is also counted "
                        "Juice's way (his Alignment-Independent method: the input before it moved across its own "
                        "window), and when that comes out wider the Frame Editor flags the click: \"my suggestion is "
                        "x frames\"",
                        &FW::suggestEnabled)) {
                slider("Suspicious at", "A strict window this small or smaller", &FW::suggestBelow,
                       "%d frames or fewer");
                slider("Move the one before", "How far either way the input before it is moved", &FW::suggestZ,
                       "up to %d frames");
                kit::FeatureEnd();
            }
            if (feature("Joint setup sweep",
                        "Resolves back-to-back inputs together instead of judging each against where the macro "
                        "placed the others. Costs extra tests",
                        &FW::jointSetupSweep)) {
                flag("Show setup range", "Reports those inputs as the range their setup can produce, \"Can be from 1 "
                                         "to 4\"",
                     &FW::showSetupRange, true);
                kit::FeatureEnd();
            }

            kit::Section("Releases");
            flag("Ship and swing releases", "Measure releases in ship and swing", &FW::testShipReleases);
            flag("All releases",
                 "Also cube, ball, UFO and spider releases. Holding still affects physics there, just less. Roughly "
                 "doubles the run time",
                 &FW::testAllReleases);
            flag("Orb-aware release skip",
                 "Skips robot releases when the click before touched a non-dash orb, since those can't be retimed "
                 "separately",
                 &FW::orbAwareReleaseSkip);

            kit::EndCard();
        }

        void cbfCard() {
            kit::BeginCard("Sub-tick (CBF) counting",
                           "Reads windows finer than one frame, the way Click Between Frames splits a tick.");
            if (feature("Count sub-tick windows",
                        "Splits the player update into fractions of a tick and reports fractional frames. TPS is "
                        "not changed",
                        &FW::subframeProbe)) {
                auto& s = S();
                auto const hz = fwstore::limits(&FW::cbfInputHz);
                kit::RowBegin("Input rate",
                              "The polling rate to simulate, in Hz. 24000 gives 100 slots per tick at 240 TPS. "
                              "Raising it barely affects speed");
                ImGui::SetNextItemWidth(kit::Avail());
                int64_t const step = 240;
                if (ImGui::InputScalar("##cbfhz", ImGuiDataType_S64, &s.cbfInputHz, &step, nullptr, "%lld")) {
                    s.cbfInputHz = std::clamp(s.cbfInputHz, hz.lo, hz.hi);
                    g_changed = true;
                }
                kit::RowEnd();

                flag("Count every input", "Instead of only tight ones. Each one takes a full sub-tick pass",
                     &FW::subframeAll);
                // GucciBot (2026-10-23): the hint says Entry sweep and Dependent pair search read it too (a copy of
                // this slider shows under their switches on the Measurement card when this one is hidden).
                if (!s.subframeAll)
                    slider("Tight threshold",
                           "Only inputs with a window this tight or tighter are counted. It also picks which inputs "
                           "Entry sweep and Dependent pair search re-test",
                           &FW::tightThreshold, "%d frames");
                flag("Bisect edges",
                     "Binary-searches the edges instead of testing every slot. Much faster, but can't see gaps "
                     "inside the window",
                     &FW::subframeBisect);
                slider("Search stride",
                       "Distance between probes before an edge is bisected. Smaller gaps can be missed; 1 tests every "
                       "slot",
                       &FW::subframeScanPercent, "%d%%");
                // GucciBot (2026-10-23): from anticroom's antics-counter beta.6 changelog ("Small CBF count speed
                // optimization"); the search is ours. Only with bisected edges, so only shown with them.
                if (s.subframeBisect)
                    flag("Faster sub-tick search",
                         "Fewer test runs for each sub-tick input: the search starts at the edges the whole-frame "
                         "count found instead of walking out from the input. Windows come out the same unless one "
                         "has a gap inside it smaller than a frame; this search can step over that gap, so the "
                         "window can come out different, usually wider. Off while the two are being compared. Not "
                         "with Full range sweep",
                         &FW::fastSubtick);
                if (feature("Whole numbers on markers",
                            "Keep the whole-frame count on the markers and show the sub-tick value in the top right "
                            "readout instead",
                            &FW::cbfWholeMarkers, true)) {
                    slider("Readout under", "Only show the readout when the window is this many frames or tighter",
                           &FW::cbfReadoutThreshold, "%d frames", true);
                    kit::FeatureEnd();
                }
                flag("Tick-quantised ground",
                     "Only split a tick if the player was already on the ground going into it, like real CBF. Off "
                     "lets every tick split, which CBF doesn't do",
                     &FW::cbfTickGround);
                kit::FeatureEnd();
            }
            kit::EndCard();
        }

        void overlayCard() {
            auto& s = S();
            kit::BeginCard("Overlay", "What the measured windows look like over the level.");

            if (flag("Show windows",
                     "Markers appear once the player reaches each input. Off hides the markers, the HUD and the L* "
                     "readout",
                     &FW::enabled, true))
                g_lstar = true;
            if (s.enabled) {
                // Default Look (Nigel's 1.7 ask, rebuilt 2026-10-27): NaN GD's look
                // drawn over the player's bands, which stay as they are
                // (analysis/ac/default_look.hpp). Saved under 1.7's keys.
                auto* gb = GucciEngine::get();
                bool lookFlipped = false;
                if (kit::FeatureBegin("Default Look",
                                      "Draws the windows like the counter in NaN GD's videos: a plain ring per input in "
                                      "his colors, the number on its left, and his 9-10 / 7-8 / 5-6 / 4 / 3 / 2 / 1 "
                                      "legend. Your bands, shapes, images, pulses and Circle skin are kept, just not used "
                                      "while it's on",
                                      &gb->fwDefaultLook, nullptr, &lookFlipped)) {
                    if (kit::SwitchRow("Bells", "Each window's own bell instead of the Brrr", &gb->fwDefaultLookBells))
                        Mod::get()->setSavedValue("fw_default_look_bells", gb->fwDefaultLookBells);
                    kit::FeatureEnd();
                }
                if (lookFlipped) {
                    Mod::get()->setSavedValue("fw_default_look", gb->fwDefaultLook);
                    g_look = true;
                }
                flag("Markers", "A marker at each measured input", &FW::showMarkers, true);
                flag("Band HUD", "A running count in the corner of how many inputs of each band have been passed",
                     &FW::showHud, true);
                if (s.showHud)
                    slider("HUD size", nullptr, &FW::hudScale, "%.2fx", true);
                flag("Show desynced", "The ? marker for inputs that could not be counted. Display only",
                     &FW::showDesynced, true);
                flag("Timing readout", "Top right: the last input's window, with its precision in ms",
                     &FW::showTiming, true);
                flag("Rate readout", "Text under the windows giving the window as a Hz number", &FW::showHzReadout,
                     true);
                if (feature("Setup suffixes",
                            "Mark setup-dependent windows: ship and swing get ^, inputs resolved by the joint setup "
                            "sweep get ~",
                            &FW::markSetupVarying, true)) {
                    flag("Include hold modes",
                         "Also mark wave and robot inputs. They depend on setup much less, so off keeps wave "
                         "sections cleaner",
                         &FW::setupHoldModes, true);
                    kit::FeatureEnd();
                }
                slider("Decimals", "Places shown for sub-tick windows on the markers and in the labels",
                       &FW::subframeDecimals, "%d", true);

                kit::Section("Markers");
                if (!s.circleSkin || gb->fwDefaultLook)
                    slider("Marker radius", nullptr, &FW::markerRadius, "%.1f", true);
                slider("Label size", nullptr, &FW::markerScale, "%.2fx", true);
                if (!gb->fwDefaultLook && feature("Circle skin",
                            "The ring grows with the window, so how tight a click is reads at a glance. Replaces the "
                            "fixed radius",
                            &FW::circleSkin, true)) {
                    slider("Base radius", "Size of a 0-frame window", &FW::circleSkinDotRadius, "%.1f", true);
                    slider("Growth per frame", nullptr, &FW::circleSkinRadiusPerFrame, "%.2f", true);
                    slider("Largest radius", nullptr, &FW::circleSkinMaxRadius, "%.0f", true);
                    kit::FeatureEnd();
                }
            }

            kit::Section("Sound");
            if (feature("Play sounds", "A band's sound when the player reaches an input in it. Muted while seeking",
                        &FW::playSounds)) {
                slider("Volume", nullptr, &FW::soundVolume, "%.2f");
                kit::FeatureEnd();
            }

            kit::EndCard();
        }

        std::string bandTitle(FrameWindowTier const& t) {
            if (!t.text.empty())
                return t.text;
            if (t.minWindow == t.maxWindow)
                return fmt::format("{:g} frame{}", t.minWindow, t.minWindow == 1.f ? "" : "s");
            return fmt::format("{:g} - {:g} frames", t.minWindow, t.maxWindow);
        }

        void bandDetails(FrameWindowTier& t) {
            bool changed = false;
            // GucciBot (2026-10-23): decimal edges, after anticroom's
            // antics-counter beta.5. The buttons still step whole frames, so a
            // whole band stays whole unless a decimal is typed.
            kit::RowBegin("Window", "Smallest and largest window in this band, in frames. Whole numbers hold whole "
                                    "windows (a sub-tick 1.3 counts as 1). A decimal edge, like 0.3 to 0.5, holds "
                                    "sub-tick (CBF) values as they're printed: on the markers when Whole numbers on "
                                    "markers is off, and always in the top right readout and Copy info");
            float const half = (kit::Avail() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            changed |= ImGui::InputFloat("##min", &t.minWindow, 1.f, 100.f, "%g");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            changed |= ImGui::InputFloat("##max", &t.maxWindow, 1.f, 100.f, "%g");
            kit::RowEnd();

            kit::RowBegin("Label", "Shown instead of the frame count on the marker and in the HUD. Empty shows the "
                                   "number");
            changed |= kit::InputText("text", &t.text, "auto");
            kit::RowEnd();
            kit::RowBegin("Sound", "An ogg, mp3 or wav. Relative paths look in the mod's config folder, then its "
                                   "resources. Empty plays the bundled clip for the window");
            changed |= kit::InputText("audio", &t.audioPath, "file.ogg");
            kit::RowEnd();
            kit::RowBegin("Image", "A png or jpg drawn in place of the shape, sized by Size below. Relative paths look "
                                   "in the mod's config folder, then its resources. Empty draws the shape");
            changed |= kit::InputText("image", &t.imagePath, "file.png");
            kit::RowEnd();

            changed |= kit::SwitchRow("Pulse the marker", "It grows and settles back as it appears", &t.pulseMarker);
            changed |= kit::SwitchRow("Pulse the label", "Its text grows and settles back as it appears", &t.pulseText);
            if (t.pulseMarker || t.pulseText) {
                changed |= kit::SliderRow("Pulse size", nullptr, &t.pulseScale, 1.f, 3.f, "%.2fx");
                changed |= kit::SliderRow("Fade in", "Seconds to grow", &t.pulseIn, 0.f, 2.f, "%.2f s");
                changed |= kit::SliderRow("Hold", "Seconds at full size", &t.pulseHold, 0.f, 3.f, "%.2f s");
                changed |= kit::SliderRow("Fade out", "Seconds to settle back", &t.pulseOut, 0.f, 3.f, "%.2f s");
            }

            const char* shapes[] = {"Circle", "Star", "Spiral", "Polygon"};
            const char* fills[] = {"Two outlines", "Filled", "One outline"};
            int shape = static_cast<int>(t.style.shape);
            int fill = static_cast<int>(t.style.fill);
            if (kit::DropdownRow("Shape", nullptr, &shape, shapes, 4)) {
                t.style.shape = static_cast<gbshape::Shape>(shape);
                changed = true;
            }
            if (kit::DropdownRow("Fill",
                                 t.style.shape == gbshape::Shape::Circle
                                     ? "A circle stays the plain ring unless it is Filled"
                                     : nullptr,
                                 &fill, fills, 3)) {
                t.style.fill = static_cast<gbshape::Fill>(fill);
                changed = true;
            }

            // Mirrors spawnMarker(): a circle that isn't Filled is drawn as
            // the plain ring and ignores the rest of the style.
            bool const styled = t.style.shape != gbshape::Shape::Circle || t.style.fill == gbshape::Fill::Normal;
            if (!styled && !t.imagePath.empty()) {
                // An image is sized by Size even when the shape is the plain ring.
                changed |= kit::SliderRow("Size", "Multiplies the marker radius", &t.style.sizeScale, 0.1f, 4.f,
                                          "%.2fx");
            } else if (styled) {
                if (t.style.shape == gbshape::Shape::Polygon) {
                    changed |= kit::SliderRow("Sides", nullptr, &t.style.polygonSides, 3, 12);
                    if (t.style.fill != gbshape::Fill::Normal)
                        changed |= kit::SliderRow("Corner rounding", nullptr, &t.style.polygonCornerRadius, 0.f, 1.f);
                }
                if (t.style.fill == gbshape::Fill::Normal && t.style.shape != gbshape::Shape::Spiral)
                    changed |= kit::SwitchRow("No border", "Leave off the dark edge around the fill", &t.style.noBorder);
                changed |= kit::SliderRow(t.style.fill == gbshape::Fill::Normal ? "Border" : "Line width", nullptr,
                                          &t.style.strokeSize, 0.f, 10.f, "%.1f");
                changed |= kit::SliderRow("Size", "Multiplies the marker radius", &t.style.sizeScale, 0.1f, 4.f,
                                          "%.2fx");
            }

            if (changed) {
                g_changed = true;
                g_look = true;
            }
        }

        void bandsCard() {
            auto& tiers = S().tiers;
            kit::BeginCard("Bands", "Colors, labels, shapes, images, pulses and sounds by window size. Hidden bands draw no markers.");
            if (GucciEngine::get()->fwDefaultLook)
                kit::Hint("Default Look is on, so the level uses NaN GD's colors and sounds. These bands are kept, and "
                          "still decide which inputs L* and Copy info count.");

            int removeAt = -1;
            int moveFrom = -1, moveTo = -1;
            for (size_t i = 0; i < tiers.size(); i++) {
                auto& t = tiers[i];
                ImGui::PushID(t.id);
                bool const open = g_openBands.contains(t.id);
                std::string const title = bandTitle(t);
                kit::RowBegin(title.c_str(), nullptr);
                ImVec4 colour(t.color[0], t.color[1], t.color[2], t.color[3]);
                if (kit::Color("##colour", &colour)) {
                    t.color = {colour.x, colour.y, colour.z, colour.w};
                    g_changed = g_look = true;
                }
                ImGui::SameLine(0.f, 12.f);
                if (kit::Switch("##shown", &t.showInHud))
                    g_changed = g_look = true;
                ImGui::SameLine(0.f, 12.f);
                if (kit::Button(open ? "Done" : "Edit")) {
                    if (open)
                        g_openBands.erase(t.id);
                    else
                        g_openBands.insert(t.id);
                }
                // Order matters: a window takes the first band it fits.
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up))
                    moveFrom = static_cast<int>(i), moveTo = static_cast<int>(i) - 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == tiers.size());
                if (ImGui::ArrowButton("##down", ImGuiDir_Down))
                    moveFrom = static_cast<int>(i), moveTo = static_cast<int>(i) + 1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (kit::Button("Remove", Tone::Bad))
                    removeAt = static_cast<int>(i);
                kit::RowEnd();
                if (open) {
                    ImGui::Indent(16.f);
                    bandDetails(t);
                    ImGui::Unindent(16.f);
                    kit::Gap(0.5f);
                }
                ImGui::PopID();
            }
            if (removeAt >= 0) {
                g_openBands.erase(tiers[removeAt].id);
                tiers.erase(tiers.begin() + removeAt);
                g_changed = g_look = true;
            } else if (moveFrom >= 0) {
                std::swap(tiers[moveFrom], tiers[moveTo]);
                g_changed = g_look = true;
            }

            // Every band's sound in one press: the Brrr, or the Bells (what an
            // empty sound plays).
            if (kit::Button("All Brrr")) {
                for (auto& t : tiers)
                    t.audioPath = "fw_default.mp3";
                g_changed = true;
            }
            ImGui::SameLine();
            if (kit::Button("All Bells")) {
                for (auto& t : tiers)
                    t.audioPath.clear();
                g_changed = true;
            }

            kit::Gap();
            // anticroom's Add Band: the next id, starting right after the
            // last band's range.
            if (kit::Button("Add band")) {
                FrameWindowTier nt;
                nt.id = 1;
                for (auto const& x : tiers)
                    nt.id = std::max(nt.id, x.id + 1);
                if (!tiers.empty()) {
                    nt.minWindow = std::min(std::floor(tiers.back().maxWindow) + 1.f,
                                            static_cast<float>(fwstore::kBandWindowMax));
                    nt.maxWindow = nt.minWindow;
                }
                tiers.push_back(nt);
                g_openBands.insert(nt.id);
                g_changed = g_look = true;
            }
            // GucciBot (2026-10-23): anticroom's antics-counter beta.5 has a
            // Sort by Window beside the arrows. Smallest window first, ties
            // by the largest; stable, so equal bands keep their order. Ids go
            // with their bands, so open editors and HUD counts stay right.
            ImGui::SameLine();
            if (kit::Button("Sort by window")) {
                std::stable_sort(tiers.begin(), tiers.end(), [](FrameWindowTier const& a, FrameWindowTier const& b) {
                    if (a.minWindow != b.minWindow)
                        return a.minWindow < b.minWindow;
                    return a.maxWindow < b.maxWindow;
                });
                g_changed = g_look = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smallest window first. A window takes the first band it fits, so where bands "
                                  "overlap this changes which one it lands in.");
            ImGui::SameLine();
            if (kit::Button("Reset bands")) {
                fwstore::resetBands();
                g_openBands.clear();
                g_look = true;
            }
            kit::EndCard();
        }

        void precisionCard() {
            auto& fw = analyzer();
            auto* solver = lstar::Solver::get();

            kit::BeginCard("Precision (L*)",
                           "NaN GD's L* (nandl.pages.dev): the precision needed for a run of these windows to be "
                           "expected within the target time. Higher means more precise, which isn't the same as harder.");

            // Any change in this card (the switches, the inputs, the readout
            // being turned on) leaves the solved number stale.
            bool const changedBefore = g_changed;
            if (feature("L*", nullptr, &FW::lstarEnabled)) {
                if (solver->running()) {
                    kit::Progress(solver->progress() / 100.f,
                                  fmt::format("Solving L* {:.0f}%", solver->progress()).c_str());
                    kit::Gap(0.25f);
                    if (kit::Button("Cancel"))
                        solver->cancel();
                } else {
                    auto const& res = solver->result();
                    if (res.m_ok)
                        kit::Label(fmt::format("L* {:.3f}{}", res.m_value, solver->dirty() ? "  (stale)" : "").c_str(),
                                   solver->dirty() ? Tone::Warn : Tone::Accent);
                    bool const canSolve = !fw.running() && !fw.results().empty();
                    if (canSolve) {
                        if (kit::Button("Solve L*"))
                            solveLStar();
                    } else if (!res.m_ok) {
                        kit::Hint("Measure a macro first.");
                    }
                }

                number("Target time", "Seconds the run should be expected within", &FW::lstarTarget, 60.0, "%.0f s");
                number("Respawn time", "Seconds lost per death", &FW::lstarRespawn, 0.05, "%.2f s");

                // The three penalty terms, each with NaN GD's coefficient as
                // its default (see shim.hpp).
                if (feature("Nerve", "Counts each window as tighter the further into the run (in seconds) it comes",
                            &FW::lstarUseNerve)) {
                    number("Nerve k", nullptr, &FW::lstarNerve, 0.0001, "%.6f");
                    kit::FeatureEnd();
                }
                if (feature("Fatigue", "Counts each window as tighter the more inputs came before it", &FW::lstarUseFatigue)) {
                    number("Fatigue k", nullptr, &FW::lstarFatigue, 0.0001, "%.6f");
                    kit::FeatureEnd();
                }
                if (feature("CPS", "Counts an input as tighter the sooner it follows the one before", &FW::lstarUseCps)) {
                    number("CPS k", nullptr, &FW::lstarCps, 0.001, "%.4f");
                    kit::FeatureEnd();
                }

                if (feature("In-level readout",
                            "Bottom left, the way NaN shows it: how far through the level's precision the player "
                            "is, over the running value",
                            &FW::lstarHud)) {
                    slider("Readout size", nullptr, &FW::lstarHudScale, "%.2fx");
                    kit::FeatureEnd();
                }
                kit::FeatureEnd();
            }
            if (g_changed && !changedBefore)
                g_lstar = true;
            kit::EndCard();
        }

        // Calculate's own count for a mark, in frames: the sub-tick window
        // when it measured one.
        float strictOf(FrameWindowMark const& mk) {
            float const v = mk.cbf && mk.subframe > 0.f ? mk.subframe : static_cast<float>(mk.window);
            return std::round(v * 100.f) / 100.f;
        }

        // A label of `frames`: whole frames as a window, anything else as a
        // sub-tick one.
        void labelWith(FrameWindowAnalyzer& fw, FrameWindowAnalyzer::PlayheadInput const& in, float frames) {
            bool const whole = std::fabs(frames - std::round(frames)) < 0.005f;
            fw.applyLabelTo(in, whole ? static_cast<int>(std::lround(frames)) : 0, whole ? 0.f : frames);
        }

        // GucciBot (2026-10-23): the table below in pages, and a delete for a
        // whole page's windows (anticroom's antics-counter beta.5 changelog).
        // What to delete is kept from the press until the confirm answers,
        // since the table can change in between, with the macro it was for.
        constexpr size_t kTablePage = 50;
        constexpr const char* kDeletePageId = "##fw-delete-page";
        std::vector<FrameWindowAnalyzer::PlayheadInput> g_pageToDelete;
        std::string g_pageToDeleteText;
        std::string g_pageToDeleteMacro;
        bool g_askDeletePage = false;

        // Manual Frame Windows (the old menu's table, Juice's idea), rebuilt
        // 2026-10-21: every input of the macro, its window -- Calculate's, or
        // one typed in -- and Calculate's suggestion where a suspicious click
        // has one. A typed window survives the next run; Clear goes back to
        // Calculate's own count.
        void windowsTable() {
            auto* gb = GucciEngine::get();
            auto& fw = analyzer();
            auto& s = S();
            static bool s_onlySuggested = false;
            static int s_page = 0;
            static int s_drawnPage = -1;

            auto const rows = fw.allInputs(s.labelReleases);
            auto const& results = fw.results();
            auto const suggested = [&](FrameWindowAnalyzer::PlayheadInput const& in) {
                if (in.mark < 0 || in.mark >= static_cast<int>(results.size()))
                    return false;
                auto const& mk = results[static_cast<size_t>(in.mark)];
                return mk.suggestion > strictOf(mk) + 0.01f;
            };

            kit::Section("Every input");
            if (rows.empty()) {
                kit::Hint("Load a macro to see its inputs here.");
                return;
            }
            kit::Hint("Type a window into any row, then Enter (0 clears it). A window you type stays through the next "
                      "Calculate run; Clear goes back to Calculate's own count.");

            int waiting = 0;
            for (auto const& in : rows)
                if (suggested(in))
                    waiting++;
            if (waiting > 0)
                kit::Note(fmt::format("{} suspicious click{} came out wider counted Juice's way: the My suggestion "
                                      "column has {}.",
                                      waiting, waiting == 1 ? "" : "s", waiting == 1 ? "it" : "them")
                              .c_str(),
                          Tone::Accent);
            if (kit::SwitchRow("Only rows with a suggestion", nullptr, &s_onlySuggested))
                s_page = 0;

            std::vector<size_t> shown;
            for (size_t i = 0; i < rows.size(); i++)
                if (!s_onlySuggested || suggested(rows[i]))
                    shown.push_back(i);

            // Pages of the rows shown, so after the filter above.
            size_t const pages = std::max<size_t>(1, (shown.size() + kTablePage - 1) / kTablePage);
            s_page = std::clamp(s_page, 0, static_cast<int>(pages) - 1);
            size_t const first = static_cast<size_t>(s_page) * kTablePage;
            size_t const last = std::min(shown.size(), first + kTablePage);
            if (pages > 1) {
                ImGui::BeginDisabled(s_page == 0);
                if (kit::Button("Prev"))
                    s_page--;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(fmt::format("Page {} of {}", s_page + 1, pages).c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(s_page + 1 >= static_cast<int>(pages));
                if (kit::Button("Next"))
                    s_page++;
                ImGui::EndDisabled();
            }

            // What a row asked for is done after the table is drawn: it changes
            // the results the table is reading.
            enum class Act { None, Set, Clear };
            Act act = Act::None;
            FrameWindowAnalyzer::PlayheadInput actOn;
            float actValue = 0.f;

            ImGuiTableFlags const flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            float const rowH = ImGui::GetFrameHeightWithSpacing();
            float const height = std::min(380.f, rowH * (static_cast<float>(last - first) + 1.5f));
            if (ImGui::BeginTable("##fwtable", 7, flags, ImVec2(0.f, height))) {
                // Another page starts at its top.
                if (s_drawnPage != s_page) {
                    ImGui::SetScrollY(0.f);
                    s_drawnPage = s_page;
                }
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("Frame");
                ImGui::TableSetupColumn("Input");
                ImGui::TableSetupColumn("Window");
                ImGui::TableSetupColumn("From");
                ImGui::TableSetupColumn("My suggestion", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();

                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(last - first));
                while (clip.Step()) {
                    for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                        auto const& in = rows[shown[first + static_cast<size_t>(r)]];
                        FrameWindowMark const* mk =
                            in.mark >= 0 && in.mark < static_cast<int>(results.size())
                                ? &results[static_cast<size_t>(in.mark)]
                                : nullptr;
                        ImGui::PushID(static_cast<int>(in.frame * 4u + (in.player2 ? 2u : 0u) + (in.release ? 1u : 0u)));
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        ImGui::Text("%d", in.number);
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", in.frame);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(
                            fmt::format("{}{}", in.player2 ? "P2 " : "", in.release ? "release" : "press").c_str());

                        ImGui::TableNextColumn();
                        float value = mk ? strictOf(*mk) : 0.f;
                        ImGui::SetNextItemWidth(72.f);
                        ImGui::BeginDisabled(fw.running());
                        ImGui::InputFloat("##w", &value, 0.f, 0.f, "%.2f");
                        if (ImGui::IsItemDeactivatedAfterEdit() && act == Act::None) {
                            act = value > 0.f ? Act::Set : Act::Clear;
                            actOn = in;
                            actValue = value;
                        }
                        ImGui::EndDisabled();

                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", !mk                ? "-"
                                                  : mk->manual       ? "By hand"
                                                  : mk->desynced     ? "Desynced"
                                                                     : "Calculate");

                        ImGui::TableNextColumn();
                        if (mk && suggested(in)) {
                            ImGui::TextColored(look().pal.accent, "%s",
                                               fmt::format("{:g} frames", mk->suggestion).c_str());
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("My suggestion is %g frames (Calculate counts %g): with the input "
                                                  "before it moved across its own window, %.0f%% of %d positions "
                                                  "give it that (Juice's Alignment-Independent method).",
                                                  mk->suggestion, strictOf(*mk), mk->suggestCoverage * 100.f,
                                                  mk->suggestAlignments);
                            ImGui::SameLine();
                            ImGui::BeginDisabled(fw.running());
                            if (ImGui::SmallButton("Use") && act == Act::None) {
                                act = Act::Set;
                                actOn = in;
                                actValue = mk->suggestion;
                            }
                            ImGui::EndDisabled();
                        }

                        ImGui::TableNextColumn();
                        if (mk && mk->manual) {
                            ImGui::BeginDisabled(fw.running());
                            if (ImGui::SmallButton("Clear") && act == Act::None) {
                                act = Act::Clear;
                                actOn = in;
                            }
                            ImGui::EndDisabled();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }

            bool changed = false;
            if (act != Act::None) {
                labelWith(fw, actOn, act == Act::Set ? actValue : 0.f);
                changed = true;
            }
            if (waiting > 1) {
                ImGui::BeginDisabled(fw.running());
                if (kit::Button(fmt::format("Use all {} suggestions", waiting).c_str())) {
                    // One at a time, finding each again: every label can move
                    // the results.
                    struct Want {
                        uint32_t frame;
                        bool player2, release;
                        float value;
                    };
                    std::vector<Want> wants;
                    for (auto const& in : rows)
                        if (suggested(in))
                            wants.push_back({in.frame, in.player2, in.release,
                                             results[static_cast<size_t>(in.mark)].suggestion});
                    for (auto const& w : wants) {
                        int mark = -1;
                        auto const& now = fw.results();
                        for (size_t k = 0; k < now.size(); k++)
                            if (now[k].frame == w.frame && now[k].player2 == w.player2 && now[k].release == w.release) {
                                mark = static_cast<int>(k);
                                break;
                            }
                        labelWith(fw, {true, w.frame, w.player2, w.release, 0, mark}, w.value);
                    }
                    changed = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }

            // Every window on this page, after a confirm (see confirms()).
            std::vector<FrameWindowAnalyzer::PlayheadInput> onPage;
            for (size_t r = first; r < last; r++)
                if (rows[shown[r]].mark >= 0)
                    onPage.push_back(rows[shown[r]]);
            ImGui::BeginDisabled(fw.running() || onPage.empty());
            if (kit::Button("Delete this page's windows", Tone::Bad)) {
                int const a = rows[shown[first]].number;
                int const b = rows[shown[last - 1]].number;
                g_pageToDeleteText = fmt::format(
                    "Every window on this page ({}) goes, typed ones included, here and in the copy saved next to "
                    "the macro. This can't be undone; the next Calculate run measures them again.",
                    a == b ? fmt::format("input {}", a) : fmt::format("inputs {} to {}", a, b));
                g_pageToDelete = std::move(onPage);
                g_pageToDeleteMacro = gb->replayName;
                g_askDeletePage = true;
            }
            ImGui::EndDisabled();

            if (changed) {
                // Labels are results: kept with the macro, and L* moves.
                gb->saveAcFrameWindowResults();
                lstar::Solver::get()->markDirty();
            }
        }

        void labellingSection() {
            auto* gb = GucciEngine::get();
            auto& fw = analyzer();
            auto& s = S();

            kit::Section("At the playhead");
            kit::Hint("Pause on an input in the level and give it a window by hand, or measure just the last few, "
                      "without running a whole Calculate.");

            auto const in = fw.playheadInput();
            if (in.valid) {
                kit::Hint(fmt::format("Input {} at frame {}{}{}", in.number, in.frame, in.player2 ? ", player 2" : "",
                                      in.release ? ", release" : "")
                              .c_str());
                if (in.mark >= 0 && in.mark < static_cast<int>(fw.results().size())) {
                    auto const& mk = fw.results()[static_cast<size_t>(in.mark)];
                    if (mk.suggestion > strictOf(mk) + 0.01f) {
                        kit::Note(fmt::format("Calculate counts {:g}. My suggestion is {:g} frames: with the input "
                                              "before it moved across its own window, {:.0f}% of {} positions give "
                                              "this one {:g} (Juice's Alignment-Independent method).",
                                              strictOf(mk), mk.suggestion, mk.suggestCoverage * 100.f,
                                              mk.suggestAlignments, mk.suggestion)
                                      .c_str(),
                                  Tone::Accent);
                        ImGui::BeginDisabled(fw.running());
                        if (kit::Button("Use my suggestion", Tone::Accent)) {
                            labelWith(fw, in, mk.suggestion);
                            gb->saveAcFrameWindowResults();
                            lstar::Solver::get()->markDirty();
                        }
                        ImGui::EndDisabled();
                    } else {
                        kit::Hint(fmt::format("Calculate counts {:g}.", strictOf(mk)).c_str());
                    }
                }
            } else {
                kit::Hint("No input at or before the playhead.");
            }

            slider("Window", "The window to give it. 0 with no sub-tick value removes its label", &FW::labelWindow,
                   "%d frames");
            slider("Sub-tick window", "A fractional window instead; 0 uses the whole number above", &FW::labelCbf,
                   "%.2f");
            kit::RowBegin("Label it", nullptr);
            ImGui::BeginDisabled(fw.running() || !in.valid);
            if (kit::Button("Apply label")) {
                fw.applyLabel(s.labelWindow, s.labelCbf);
                // Labels are results: keep them with the macro the way a
                // finished run's are, and the L* number no longer matches.
                gb->saveAcFrameWindowResults();
                lstar::Solver::get()->markDirty();
            }
            ImGui::EndDisabled();
            kit::RowEnd();

            slider("Inputs to test", "How many inputs back from the playhead to measure", &FW::labelTestCount, "%d");
            kit::RowBegin("Measure them", "Measures just those inputs, then plays back to where you were");
            ImGui::BeginDisabled(fw.running() || fw.returning() || !in.valid || !PlayLayer::get());
            if (kit::Button("Test")) {
                auto const r = fw.testPlayhead(PlayLayer::get(), s.labelTestCount);
                gb->fwAcReport = r.message;
                gb->fwAcOk = r.ok;
            }
            ImGui::EndDisabled();
            kit::RowEnd();
        }

        // Windows by hand, out where it can be found (it was inside Advanced;
        // Nigel, 2026-10-21, couldn't find it): the suggestions and the
        // playhead labelling.
        void byHandCard() {
            kit::BeginCard("Windows by hand", "Every input's window, Calculate's or yours, and Calculate's "
                                              "suggestions for the suspicious ones.");
            flag("Include releases", "List releases as inputs too, here and at the playhead", &FW::labelReleases);
            windowsTable();
            labellingSection();
            kit::EndCard();
        }

        void advancedCard() {
            auto& s = S();
            kit::BeginCard("Advanced", "Run speed, what the screen does during a run, and debugging.");
            if (kit::FeatureBegin("Show advanced settings", nullptr, &g_showAdvanced)) {
                kit::Section("While it runs");
                if (feature("Analysis visuals", "Hide clutter on screen while the analyzer runs",
                            &FW::analysisVisuals)) {
                    flag("Lock camera",
                         "Keep the camera on the input being measured instead of jumping on every restore",
                         &FW::lockCamera);
                    flag("Hide spawn effects", "The respawn flash and circle, which would otherwise play on every test",
                         &FW::hideSpawnEffects);
                    kit::FeatureEnd();
                }
                // GucciBot (2026-10-23): anticroom's Leg Trail (antics-counter beta.5 and beta.6).
                flag("Leg trail",
                     "Draw each test in the level as it plays: yellow while it runs, green when it makes it, red "
                     "with an X where it dies. The thin white line is the macro's own run",
                     &FW::legTrail);

                kit::Section("Speed");
                if (feature("Turbo",
                            "Give the analyzer a large part of every frame. The game drops to a few FPS while "
                            "counting, but finishes several times faster",
                            &FW::turbo)) {
                    slider("Turbo budget", "Milliseconds spent analysing per frame", &FW::turboBudgetMs, "%d ms");
                    kit::FeatureEnd();
                }
                if (feature("Adaptive budget",
                            "Spend a share of each frame instead of a fixed amount, so levels that already run "
                            "slowly count faster",
                            &FW::adaptiveBudget)) {
                    slider("Budget share", "How much of the time since the last frame to spend analysing",
                           &FW::budgetSharePercent, "%d%%");
                    slider("Max budget", "Cap, so a long stall can't turn into one long freeze", &FW::maxBudgetMs,
                           "%d ms");
                    kit::FeatureEnd();
                }
                slider("Frame budget",
                       s.adaptiveBudget ? "The least the adaptive budget spends per frame"
                                        : "Milliseconds per frame spent analysing. Higher finishes sooner but is "
                                          "choppier",
                       &FW::budgetMs, "%d ms");
                slider("Tick batch",
                       "Physics ticks run per game update while analysing. The ticks are the same; this only skips "
                       "the scheduler and effects between them",
                       &FW::stepBatch, "%d");
                slider("Time cap",
                       "Stops a run that goes on longer than this and keeps what it measured. 0 has no cap",
                       &FW::timeCapMinutes, s.timeCapMinutes == 0 ? "no cap" : "%d min");

                // GucciBot (2026-10-23): from anticroom's antics-counter beta.7 changelog, experimental there too.
                kit::Section("Experimental");
                flag("Check past next input",
                     "Each test runs on to just before the input after the next one, so a death this shift causes "
                     "after the next input isn't missed. Stricter and slower: a death the next input's own timing "
                     "causes counts against this one too",
                     &FW::checkPastNextInput);
                // GucciBot (2026-10-23): from anticroom's antics-counter beta.4 and beta.6 changelogs.
                flag("2-player fixes",
                     "For 2-player macros and dual levels. In a 2-player level a test moves only the measured "
                     "player's clicks, plays the other player's at their own timing with none skipped, and runs on "
                     "to the measured player's next click; in a 1-player level a click a macro stores for both "
                     "players counts once. Their windows can come out wider or narrower: wider where tests skipped "
                     "or moved the other player's clicks, narrower where a test now runs further and meets a later "
                     "death. Off by default while it's compared with the old count",
                     &FW::twoPlayerFixes);

                kit::Section("Debug");
                if (feature("Verbose log",
                            "Log every sample, checkpoint and tested shift to the Geode log and guccibot_fw.log",
                            &FW::verbose)) {
                    flag("Analysis overlay",
                         "Bottom left while counting: stage, current input, shift being tested, window so far, test "
                         "rate and elapsed time",
                         &FW::analysisOverlay);
                    flag("Player state diff",
                         "Compare the player's raw memory before and after each restore and log what changed. Slow; "
                         "for finding state checkpoints don't restore",
                         &FW::statePlayerDiff);
                    kit::FeatureEnd();
                }

                kit::Gap();
                if (kit::Button("Reset all Calculate settings", Tone::Bad))
                    g_askReset = true;
                kit::FeatureEnd();
            }
            kit::EndCard();
        }

        void confirms() {
            if (g_askClear) {
                kit::OpenConfirm(kClearId);
                g_askClear = false;
            }
            if (kit::Confirm(kClearId, "Clear the windows?",
                             "The measured windows for this macro are removed, including the copy saved next to the "
                             "macro file. This can't be undone.",
                             "Clear") == 1) {
                auto& fw = analyzer();
                if (!fw.running()) {
                    fw.clear();
                    fw.clearDisplay();
                    GucciEngine::get()->saveAcFrameWindowResults();
                    GucciEngine::get()->fwAcReport.clear();
                    lstar::Solver::get()->markDirty();
                }
            }

            if (g_askDeletePage) {
                kit::OpenConfirm(kDeletePageId);
                g_askDeletePage = false;
            }
            // GucciBot (2026-10-23): the count is what Delete would take now,
            // by eraseMarks' own rule (one window an input; none once another
            // macro is loaded), so it is the number that goes. It can be fewer
            // than the rows: two rows can show one shared window.
            bool const sameMacro = GucciEngine::get()->replayName == g_pageToDeleteMacro;
            size_t const deleting = sameMacro ? analyzer().countMarks(g_pageToDelete) : 0;
            std::string const deleteTitle = fmt::format("Delete {} window{}?", deleting, deleting == 1 ? "" : "s");
            if (int const answer = kit::Confirm(kDeletePageId, deleteTitle.c_str(), g_pageToDeleteText.c_str(), "Delete");
                answer != 0) {
                // Windows are results: kept with the macro, and L* moves. Not
                // if another macro was loaded while the confirm was up.
                if (answer == 1 && sameMacro && analyzer().eraseMarks(g_pageToDelete) > 0) {
                    GucciEngine::get()->saveAcFrameWindowResults();
                    lstar::Solver::get()->markDirty();
                }
                g_pageToDelete.clear();
            }

            if (g_askReset) {
                kit::OpenConfirm(kResetId);
                g_askReset = false;
            }
            if (kit::Confirm(kResetId, "Reset Calculate settings?",
                             "Every setting on this page goes back to its default, bands included. Measured windows "
                             "are kept.",
                             "Reset") == 1) {
                fwstore::resetAll();
                // Default Look lives on GucciEngine, not in the analyzer's settings.
                auto* gb = GucciEngine::get();
                gb->fwDefaultLook = gb->fwDefaultLookBells = false;
                Mod::get()->setSavedValue("fw_default_look", false);
                Mod::get()->setSavedValue("fw_default_look_bells", false);
                g_openBands.clear();
                g_look = true;
            }
        }

    } // namespace

    void calculate() {
#if !GB_NATIVE_ENGINE
        kit::Note("Calculate is untested on this platform: it has only been checked on Windows so far. If its windows look "
                  "wrong here, please report it.",
                  Tone::Warn);
#endif
        g_changed = g_look = g_lstar = false;

        runCard();
        byHandCard();
        measurementCard();
        cbfCard();
        overlayCard();
        bandsCard();
        precisionCard();
        advancedCard();
        confirms();

        if (g_changed)
            fwstore::save();
        // Redraw from here with the new look. Never mid-run: the analyzer
        // owns the screen then, and everything is redrawn when it finishes.
        auto& fw = analyzer();
        if (g_look && !fw.running())
            fw.clearDisplay();
        if (g_lstar)
            lstar::Solver::get()->markDirty();
    }

} // namespace gucci::ui::pages
