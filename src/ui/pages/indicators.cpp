// The Indicators page: the Survival Indicator (hacks/indicator.hpp), its click
// cue and the calibration behind it (trainers/calibration.hpp), and the
// accuracy / streak readout. Written fresh on 2026-10-03. Every change is saved
// the moment it is made (indicator::saveSettings, CalibrationService::save).

#include "core/platform.hpp"
#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "core/GucciBot.hpp"
#include "hacks/indicator.hpp"
#include "trainers/calibration.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace gucci::ui::pages {

    namespace {

        const char* styleHint(int style) {
            switch (style) {
                case indicator::Ring: return "A ring around the player; thicker the less room is left";
                case indicator::Classic: return "A light at the top of the screen, with a bar of the room left";
                case indicator::Converge: return "Four corners that close in on the player as the room runs out";
                case indicator::Pulse: return "A disc on the player that beats faster the less room is left";
                default: return nullptr;
            }
        }

        double tickRate() {
            auto const& upd = GucciEngine::get()->updater;
            return upd.m_tps > 0.0 ? upd.m_tps : 240.0;
        }

        void colourRow(const char* label, float* r, float* g, float* b, bool& changed) {
            ImVec4 c(*r, *g, *b, 1.f);
            if (kit::ColorRow(label, nullptr, &c, false)) {
                *r = c.x;
                *g = c.y;
                *b = c.z;
                changed = true;
            }
        }

        // What the indicator says right now, while a level is open.
        void liveLine() {
            auto const& r = indicator::reading();
            if (!PlayLayer::get() || !r.valid) {
                kit::Hint("Open a level to see it work.");
                return;
            }
            kit::RowBegin("Right now", r.held ? "Clicking means letting go" : "Clicking means pressing");
            kit::Chip(r.safe ? "Safe" : "Unsafe", r.safe ? Tone::Good : Tone::Bad);
            ImGui::SameLine();
            kit::Label(fmt::format("click lives {} / {}, doing nothing {} / {}", r.clickTicks, r.lookahead,
                                   r.keepTicks, r.lookahead)
                           .c_str(),
                       Tone::Muted);
            kit::RowEnd();
        }

        void indicatorCard(bool& changed) {
            auto* gb = GucciEngine::get();
            kit::BeginCard("Survival Indicator",
                           "Whether clicking right now survives what is coming. A copy of the player is run ahead "
                           "of it twice every tick: once clicking, once doing nothing.");

            bool flipped = false;
            if (kit::FeatureBegin("Survival indicator",
                                  "Green: a click now lives through the look-ahead. Red: it does not. Player 1 "
                                  "only, and not while a macro plays",
                                  &gb->survivalIndicator, nullptr, &flipped)) {
                char const* const styles[] = {indicator::styleName(indicator::Ring),
                                              indicator::styleName(indicator::Classic),
                                              indicator::styleName(indicator::Converge),
                                              indicator::styleName(indicator::Pulse)};
                changed |= kit::ChoiceRow("Style", styleHint(gb->indicatorStyle), &gb->indicatorStyle, styles,
                                          indicator::StyleCount);

                std::string const lookHint = fmt::format(
                    "How far ahead a click has to live, in ticks ({:.0f} ms at {:g} TPS). The click is held (or let "
                    "go) for all of it, so keep it short for the ship, UFO and wave; the cube wants about a jump",
                    gb->indicatorLookahead * 1000.0 / tickRate(), tickRate());
                changed |= kit::SliderRow("Look-ahead", lookHint.c_str(), &gb->indicatorLookahead,
                                          indicator::kMinLookahead, indicator::kMaxLookahead, "%d ticks");
                changed |= kit::SliderRow("Opacity", nullptr, &gb->indicatorOpacity, 0.05f, 1.f, "%.2f");
                colourRow("Safe colour", &gb->indicatorSafeColorR, &gb->indicatorSafeColorG,
                          &gb->indicatorSafeColorB, changed);
                colourRow("Unsafe colour", &gb->indicatorDangerColorR, &gb->indicatorDangerColorG,
                          &gb->indicatorDangerColorB, changed);
                changed |= kit::SwitchRow("Flash on click",
                                          "Each click you make flashes in the colour the indicator showed then",
                                          &gb->indicatorFlashEnabled);
                kit::Gap();
                liveLine();
                kit::FeatureEnd();
            }
            changed |= flipped;
            kit::EndCard();
        }

        void calibrationRow(int mode, bool levelOpen) {
            auto& cal = CalibrationService::get();
            auto& m = cal.modes[mode];
            std::string hint = m.sampleCount > 0
                                   ? fmt::format("You answer the cue in {:.0f} ms, give or take {:.0f} ({} clicks)",
                                                 m.leadMs, m.jitterMs, m.sampleCount)
                                   : std::string("Not calibrated: the cue plays the moment a click becomes safe");
            ImGui::PushID(mode);
            kit::RowBegin(CalibrationService::gamemodeName(mode), hint.c_str());
            if (kit::Switch("##cue", &m.guideEnabled))
                cal.save();
            ImGui::SameLine(0.f, 12.f);
            ImGui::BeginDisabled(!levelOpen || cal.active);
            if (kit::Button("Calibrate"))
                cal.start(mode);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(m.sampleCount == 0 || (cal.active && cal.calibratingMode == mode));
            if (kit::Button("Reset"))
                cal.resetMode(mode);
            ImGui::EndDisabled();
            kit::RowEnd();
            ImGui::PopID();
        }

        void cueCard(bool& changed) {
            auto* gb = GucciEngine::get();
            auto& cal = CalibrationService::get();
            kit::BeginCard("Click cue",
                           "A click sound when a click is needed and would be safe, pitched higher the less room "
                           "is left. Uses your click pack's click sound and volume.");
            changed |= kit::SwitchRow("Click cue", "Needs the Survival Indicator on", &gb->indicatorSoundEnabled);

            kit::Section("Calibration");
            kit::Paragraph("The cue can come early by the time you take to answer a sound, so your click lands on "
                           "the moment instead of after it. Calibrate a gamemode: with a level open, press "
                           "Calibrate, close the menu, and click as soon as you hear each click "
                           "(noclip or practice mode helps). The switch on each row turns the cue on or off "
                           "in that gamemode.",
                           Tone::Muted);
            bool const levelOpen = PlayLayer::get() != nullptr;
            if (cal.active) {
                std::string const text = fmt::format("{}: {} / {}", CalibrationService::gamemodeName(cal.calibratingMode),
                                                     cal.repsDone, cal.repsTarget);
                kit::Progress(cal.repsTarget > 0 ? static_cast<float>(cal.repsDone) / cal.repsTarget : 0.f,
                              text.c_str());
                if (kit::Button("Cancel calibration", Tone::Bad))
                    cal.cancel();
                kit::Gap();
            } else if (!levelOpen) {
                kit::Hint("Open a level to calibrate. A calibration stops if you leave the level or miss a cue "
                          "for a few seconds.");
            }
            for (int mode = 0; mode < GM_Count; ++mode)
                calibrationRow(mode, levelOpen);
            kit::EndCard();
        }

        void accuracyCard(bool& changed) {
            auto* gb = GucciEngine::get();
            kit::BeginCard("Accuracy",
                           "Your clicks held against what the indicator showed when they came. Only clicks with "
                           "something at stake count: an unsafe click, or a safe one that doing nothing would "
                           "have died without.");
            changed |= kit::SwitchRow("Show in the level", "Accuracy: N%  Streak: N (Best: N), at the top of the screen",
                                      &gb->accuracyHudEnabled);

            std::string const acc =
                gb->accuracyTotalClicks > 0
                    ? fmt::format("{:.0f}% ({} of {})", 100.0 * gb->accuracyGoodClicks / gb->accuracyTotalClicks,
                                  gb->accuracyGoodClicks, gb->accuracyTotalClicks)
                    : std::string("no clicks counted yet");
            kit::RowBegin("This session", "Starts over when a level is opened");
            kit::Label(fmt::format("{}, streak {}", acc, gb->currentStreak).c_str());
            kit::RowEnd();
            kit::RowBegin("Best streak", "Kept between sessions");
            kit::Label(fmt::format("{}", gb->bestStreak).c_str());
            kit::RowEnd();

            if (kit::Button("Start over"))
                indicator::resetStats(false);
            ImGui::SameLine();
            if (kit::Button("Clear best streak", Tone::Bad))
                kit::OpenConfirm("##clearbest");
            if (kit::Confirm("##clearbest", "Clear best streak", "Set the best streak back to 0?", "Clear") == 1)
                indicator::resetStats(true);
            kit::EndCard();
        }

    } // namespace

    void indicators() {
#if !GB_NATIVE_ENGINE
        kit::Note("The Survival Indicator is untested on this platform. Moving objects keep the speed they have now: triggers that start, stop or change them later are not simulated here.", Tone::Warn);
#endif
        bool changed = false;
        indicatorCard(changed);
        cueCard(changed);
        accuracyCard(changed);
        if (changed)
            indicator::saveSettings();
    }

} // namespace gucci::ui::pages
