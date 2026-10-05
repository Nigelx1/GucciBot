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

            kit::BeginCard("Calculate", "Measures how far each input of the loaded macro can move and still pass.");

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
            }

            if (!fw.results().empty())
                kit::Hint(fmt::format("{} measured, {} skipped, {} desynced", fw.measuredCount(), fw.skippedCount(),
                                      fw.desyncCount())
                              .c_str());
            if (!fw.running() && !gb->fwAcReport.empty())
                kit::Note(gb->fwAcReport.c_str(), gb->fwAcOk ? Tone::Muted : Tone::Warn);

            kit::EndCard();
        }

        void measurementCard() {
            auto& s = S();
            kit::BeginCard("Measurement", "How an input is judged.");

            const char* algos[] = {"Time-Based", "Recovery Range"};
            if (kit::ChoiceRow("Algorithm",
                               "Recovery Range requires the next input to still be able to recover before a shift "
                               "counts. Stricter and slower.",
                               &s.algorithm, algos, 2))
                g_changed = true;
            if (s.algorithm == static_cast<int>(FrameWindowAnalyzer::Algorithm::RecoveryRange))
                slider("Recovery window", "How far the next input can be moved while looking for a recovery",
                       &FW::recoveryRange, "%d frames");

            slider("Sweep range",
                   "How many frames on either side of each input to test. Testing stops at the first death, so a "
                   "wide range is still fast on tight inputs",
                   &FW::sweepRange, "%d frames");
            slider("Horizon",
                   "How many ticks the player must survive after a shifted input. 240 is one second at 240 TPS",
                   &FW::maxFrames, "%d ticks");
            slider("Slack",
                   "Ends a test early when the next input is this close, so it isn't judged on frames that belong "
                   "to the next input",
                   &FW::slack, "%d frames");

            flag("Full range sweep",
                 "Keeps testing past the first death. Slower, and survivors past a gap still aren't counted",
                 &FW::fullRangeSweep);
            flag("Entry sweep",
                 "Re-tests tight inputs with the previous input placed at each edge of its window and reports the "
                 "span. About 3x the work",
                 &FW::entrySweep);
            flag("Dependent pair search",
                 "Re-measures each input with the previous one moved across its own window. If the timing carries "
                 "over, the window becomes the average and shows as ~N",
                 &FW::dependentSearch);
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
                if (!s.subframeAll)
                    slider("Tight threshold", "Only inputs with a window this tight or tighter are counted",
                           &FW::tightThreshold, "%d frames");
                flag("Bisect edges",
                     "Binary-searches the edges instead of testing every slot. Much faster, but can't see gaps "
                     "inside the window",
                     &FW::subframeBisect);
                slider("Search stride",
                       "Distance between probes before an edge is bisected. Smaller gaps can be missed; 1 tests every "
                       "slot",
                       &FW::subframeScanPercent, "%d%%");
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
                         "Also mark UFO, wave and robot inputs. They depend on setup much less, so off keeps wave "
                         "sections cleaner",
                         &FW::setupHoldModes, true);
                    kit::FeatureEnd();
                }
                slider("Decimals", "Places shown for sub-tick windows on the markers and in the labels",
                       &FW::subframeDecimals, "%d", true);

                kit::Section("Markers");
                if (!s.circleSkin)
                    slider("Marker radius", nullptr, &FW::markerRadius, "%.1f", true);
                slider("Label size", nullptr, &FW::markerScale, "%.2fx", true);
                if (feature("Circle skin",
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
                return fmt::format("{} frame{}", t.minWindow, t.minWindow == 1 ? "" : "s");
            return fmt::format("{} - {} frames", t.minWindow, t.maxWindow);
        }

        void bandDetails(FrameWindowTier& t) {
            bool changed = false;
            kit::RowBegin("Window", "Smallest and largest window in this band, in frames");
            float const half = (kit::Avail() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            changed |= ImGui::InputInt("##min", &t.minWindow);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(half);
            changed |= ImGui::InputInt("##max", &t.maxWindow);
            kit::RowEnd();

            kit::RowBegin("Label", "Shown instead of the frame count on the marker and in the HUD. Empty shows the "
                                   "number");
            changed |= kit::InputText("text", &t.text, "auto");
            kit::RowEnd();
            kit::RowBegin("Sound", "An ogg, mp3 or wav. Relative paths look in the mod's config folder, then its "
                                   "resources. Empty plays the bundled clip for the window");
            changed |= kit::InputText("audio", &t.audioPath, "file.ogg");
            kit::RowEnd();

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
            if (styled) {
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
            kit::BeginCard("Bands", "Colours, labels, shapes and sounds by window size. Hidden bands draw no markers.");

            int removeAt = -1;
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
                    nt.minWindow = std::min(tiers.back().maxWindow + 1, fwstore::kBandWindowMax);
                    nt.maxWindow = nt.minWindow;
                }
                tiers.push_back(nt);
                g_openBands.insert(nt.id);
                g_changed = g_look = true;
            }
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
                           "NaN's L*: the precision needed for a run of these windows to be expected within the "
                           "target time. Higher is harder.");

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

        void labellingSection() {
            auto* gb = GucciEngine::get();
            auto& fw = analyzer();
            auto& s = S();

            kit::Section("Playhead labelling");
            kit::Hint("Work on the input at or before the playhead without running a whole Calculate.");
            flag("Include releases", "Count releases as inputs when finding the one under the playhead",
                 &FW::labelReleases);

            auto const in = fw.playheadInput();
            if (in.valid)
                kit::Hint(fmt::format("Input {} at frame {}{}{}", in.number, in.frame, in.player2 ? ", player 2" : "",
                                      in.release ? ", release" : "")
                              .c_str());
            else
                kit::Hint("No input at or before the playhead.");

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

        void advancedCard() {
            auto& s = S();
            kit::BeginCard("Advanced", "Run speed, what the screen does during a run, labelling by hand, and "
                                       "debugging.");
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

                labellingSection();

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

            if (g_askReset) {
                kit::OpenConfirm(kResetId);
                g_askReset = false;
            }
            if (kit::Confirm(kResetId, "Reset Calculate settings?",
                             "Every setting on this page goes back to its default, bands included. Measured windows "
                             "are kept.",
                             "Reset") == 1) {
                fwstore::resetAll();
                g_openBands.clear();
                g_look = true;
            }
        }

    } // namespace

    void calculate() {
#if !GB_NATIVE_ENGINE
        kit::Note("Calculate is Windows-only for now: it steps the game in batches through the Windows engine.", Tone::Warn);
#endif
        g_changed = g_look = g_lstar = false;

        runCard();
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
