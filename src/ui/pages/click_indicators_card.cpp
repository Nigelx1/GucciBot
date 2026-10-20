// The Click Indicators card (trainers/click_indicators.hpp), at the foot of
// both trainer pages with the other indicators.

#include "ui/pages.hpp"
#include "ui/kit.hpp"

#include "trainers/click_indicators.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <string>

using namespace geode::prelude;

namespace gucci::ui::pages {

    namespace {
        // A colour and its transparency, as two rows.
        bool colourRows(const char* label, const char* hint, std::array<float, 3>& rgb, int* transparency) {
            bool changed = false;
            ImGui::PushID(label);
            ImVec4 c(rgb[0], rgb[1], rgb[2], 1.f);
            if (kit::ColorRow(label, hint, &c, false)) {
                rgb = {c.x, c.y, c.z};
                changed = true;
            }
            if (transparency)
                changed |= kit::SliderRow("Transparency", "0 is solid, 100 invisible", transparency, 0, 100, "%d%%");
            ImGui::PopID();
            return changed;
        }
    } // namespace

    void clickIndicators() {
        namespace ci = gucci::clickind;
        auto& s = ci::settings();
        bool changed = false;
        kit::BeginCard("Click Indicators",
                       "Where the macro clicks, in the level and down a lane. It never clicks for you, but it "
                       "counts as a cheat: safe mode keeps progress from saving while it shows.");
        changed |= kit::SwitchRow("Click Indicators",
                                  "In a trainer's level, that trainer's macro; anywhere else, the macro loaded on the "
                                  "Macro page, in its own level. Hidden while a macro plays or records.",
                                  &s.enabled);
        if (s.enabled) {
            if (auto const* what = ci::showing(); what && *what)
                kit::Hint(fmt::format("Showing {}.", what).c_str());

            kit::Section("In the level");
            changed |= kit::SliderRow("Look ahead", "How far ahead presses show", &s.lookAhead, 0.5f, 10.f, "%.1f s");
            changed |= kit::SliderRow("Most on screen", "In the level and in the lane", &s.maxNotes, 1, 60);
            changed |= kit::SliderRow("Timing offset", "Every indicator later (+) or earlier (-)", &s.offsetMs, -2000,
                                      2000, "%d ms");
            changed |= colourRows("Color", nullptr, s.color, &s.transparency);
            changed |= colourRows("Player 2 color", "Two-player macros", s.p2Color, &s.p2Transparency);
            changed |= kit::SliderRow("Tap line thickness", "In level units; a block is 30", &s.lineWidth, 1.f, 20.f,
                                      "%.1f");
            changed |= kit::SliderRow("Tap threshold", "Shorter presses draw as taps, longer ones as holds", &s.tapMs,
                                      5, 300, "%d ms");
            changed |= kit::SwitchRow("Click marker on the edge", "A solid line on the frame each press lands on",
                                      &s.edgeMarker);
            changed |= kit::SwitchRow("Holds drain as you hold",
                                      "Once a press goes by, the bar shrinks with you: what's left is the hold left",
                                      &s.holdTracks);
            changed |= kit::SwitchRow("Badge on the press edge", "A small square level with you, easier to follow",
                                      &s.pressBadge);
            changed |= kit::SwitchRow("Target square on you", "A badge centered in it is perfect timing",
                                      &s.playerSquare);
            if (s.pressBadge || s.playerSquare)
                changed |= kit::SliderRow("Badge size", "On screen, whatever the zoom", &s.badgeSize, 2.f, 20.f,
                                          "%.0f");
            changed |= kit::SwitchRow("Line through you", "An indicator touching it is the frame the macro clicks",
                                      &s.playerLine);
            if (s.playerLine)
                changed |= colourRows("Line color", nullptr, s.playerColor, &s.playerTransparency);

            kit::Section("Rhythm lane");
            changed |= kit::SwitchRow("Rhythm lane", "Presses fall toward a hit line at the edge of the screen",
                                      &s.lane);
            if (s.lane) {
                static const char* const kSides[] = {"Left", "Right", "Middle"};
                changed |= kit::ChoiceRow("Side", "Middle lays it flat across the top", &s.laneSide, kSides, 3);
                if (s.laneSide == 2)
                    changed |= kit::SliderRow("Hit position", "How far in from its edge, as a share of the width",
                                              &s.midLaneHit, 0.08f, 0.5f, "%.2f");
                changed |= kit::SliderRow("Lane window", "Seconds from the far end to the hit line: lower is faster",
                                          &s.laneWindow, 0.3f, 6.f, "%.1f s");
                changed |= kit::SliderRow("Size", nullptr, &s.laneScale, 0.4f, 2.5f, "%.2fx");
                static const char* const kShapes[] = {"Slabs", "Circles"};
                changed |= kit::ChoiceRow("Notes", "Circles: a flat osu!mania lane, gray capsules for holds",
                                          &s.noteShape, kShapes, 2);
                if (s.noteShape == 0)
                    changed |= kit::SliderRow("Perspective", "How hard the lane tapers away", &s.laneDepth, 0.2f, 4.f,
                                              "%.2f");
                changed |= kit::SwitchRow("Upside down", "Notes travel the other way, into the far end",
                                          &s.laneFlip);
                changed |= kit::SliderRow("Lane transparency", nullptr, &s.laneTransparency, 0, 100, "%d%%");
                changed |= kit::SwitchRow("A column per player", "Two-player macros: player 1 left, player 2 right",
                                          &s.p2Lane);
            }

            kit::Section("Scoring");
            changed |= kit::SwitchRow("Score my clicks", "Each click against the macro's press nearest it, beside the lane",
                                      &s.accuracy);
            if (s.accuracy) {
                changed |= kit::SliderRow("Perfect within", nullptr, &s.perfectMs, 1, 100, "%d ms");
                changed |= kit::SliderRow("OK within", "Past it is a miss", &s.okMs, 2, 400, "%d ms");
                auto const t = ci::tally();
                kit::Chip(fmt::format("Perfect {}", t.perfect).c_str(), Tone::Good);
                ImGui::SameLine();
                kit::Chip(fmt::format("OK {}", t.ok).c_str(), Tone::Warn);
                ImGui::SameLine();
                kit::Chip(fmt::format("Miss {}", t.miss).c_str(), Tone::Bad);
                ImGui::SameLine();
                if (kit::Button("Reset"))
                    ci::resetTally();
                kit::Hint("A press nobody answers is a miss; a click with no press near it isn't scored. The tally "
                          "starts over with each attempt.");
            }
        }
        if (changed) {
            s.okMs = std::max(s.okMs, s.perfectMs);
            ci::save();
        }
        kit::EndCard();
    }

} // namespace gucci::ui::pages
