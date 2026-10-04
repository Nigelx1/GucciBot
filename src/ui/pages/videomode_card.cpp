// The Video Mode card on the JMF Trainer page: the switch, opacity, the sync
// offset and the Alignment Tool. The overlay and the decoder are the engine's,
// in trainers/videomode.*; this draws their controls. Written fresh for 2.0.

#include "ui/kit.hpp"
#include "ui/look.hpp"

#include "core/GucciBot.hpp"
#include "trainers/videomode.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace gucci::ui::pages {

    void videoModeCard();

    namespace {

        namespace vm = gucci::videomode;

        std::string g_message;
        Tone g_messageTone = Tone::Muted;

        std::string timeText(double sec) {
            if (sec < 0.0)
                sec = 0.0;
            int const m = (int)(sec / 60.0);
            double const s = sec - m * 60.0;
            return m > 0 ? fmt::format("{}:{:06.3f}", m, s) : fmt::format("{:.3f}s", s);
        }

        // Frames in about a second of this video, for the one-second steps.
        int framesPerSecond() {
            double const f = vm::frameSec();
            return f > 0.0 ? std::max(1, (int)std::lround(1.0 / f)) : 30;
        }

        void alignmentTool() {
            auto* gb = GucciEngine::get();
            kit::Section("Alignment Tool");
            kit::Hint("Scrub the video to the frame where the macro's first click happens, then set the offset "
                      "from it in one press.");
            bool on = gb->jupiterVideoAlignToolActive;
            if (kit::SwitchRow("Scrub by hand", "While on, the video holds where you put it instead of following "
                                                "the clock",
                               &on))
                vm::setAlignTool(on);
            if (!gb->jupiterVideoAlignToolActive)
                return;

            kit::RowBegin("Video time", nullptr);
            kit::SliderFloat("scrub", &gb->jupiterVideoAlignScrubSec, 0.f, (float)vm::durationSec(), "%.3f s");
            kit::RowEnd();

            int const second = framesPerSecond();
            kit::RowBegin("Step", "Whole frames from the one on screen");
            if (kit::Button("-1 s##scrub"))
                vm::stepFrames(-second);
            ImGui::SameLine();
            if (kit::Button("-1 frame##scrub"))
                vm::stepFrames(-1);
            ImGui::SameLine();
            if (kit::Button("+1 frame##scrub"))
                vm::stepFrames(1);
            ImGui::SameLine();
            if (kit::Button("+1 s##scrub"))
                vm::stepFrames(second);
            kit::RowEnd();

            double shown = 0.0, first = 0.0;
            bool const haveFrame = vm::shownFrame(shown);
            bool const haveFirst = vm::firstClickSec(first);
            kit::Label(haveFrame ? fmt::format("On screen: the frame at {}", timeText(shown)).c_str()
                                 : "On screen: nothing decoded yet",
                       Tone::Muted);
            kit::Label(haveFirst ? fmt::format("The macro's first press: {}", timeText(first)).c_str()
                                 : "The macro has no clicks, so there's no first press to line up with.",
                       haveFirst ? Tone::Muted : Tone::Warn);

            ImGui::BeginDisabled(!haveFrame || !haveFirst);
            if (kit::Button("Set offset here", Tone::Accent)) {
                if (vm::setOffsetFromShownFrame()) {
                    g_message = fmt::format("Offset set to {:.3f} s. The video follows the clock again.",
                                            gb->jupiterVideoOffsetSec);
                    g_messageTone = Tone::Good;
                }
            }
            ImGui::EndDisabled();
        }

    } // namespace

    void videoModeCard() {
        vm::ensureLoaded();
        auto* gb = GucciEngine::get();

        kit::BeginCard("Video Mode",
                       "The built-in JMF showcase video, full screen behind the menu and on the click bar's clock, "
                       "for reviewing a run against the macro's timing. No level needs to be open.");

        if (kit::SwitchRow("Video Mode", "Plays the showcase video that ships with the mod; nothing to set up",
                           &gb->jupiterVideoModeEnabled)) {
            g_message.clear();
            if (!gb->jupiterVideoModeEnabled)
                gb->jupiterVideoAlignToolActive = false;
        }
        if (!gb->jupiterVideoModeEnabled) {
            kit::EndCard();
            return;
        }

        switch (vm::status()) {
            case vm::Status::Off:
            case vm::Status::Opening:
                kit::Label("Opening the video...", Tone::Muted);
                break;
            case vm::Status::Failed:
                kit::Note(vm::failure().c_str(), Tone::Bad);
                kit::Hint("Switch Video Mode off and on to try again. guccibot_videomode.log in the mod's save "
                          "folder has the details.");
                break;
            case vm::Status::Ready: {
                kit::Chip(fmt::format("{}x{}", vm::width(), vm::height()).c_str(), Tone::Accent);
                ImGui::SameLine();
                kit::Chip(timeText(vm::durationSec()).c_str(), Tone::Muted);
                ImGui::SameLine();
                kit::Chip(fmt::format("{:.0f} fps", vm::frameSec() > 0.0 ? 1.0 / vm::frameSec() : 0.0).c_str(),
                          Tone::Muted);
                UseFont f(Font::Small);
                kit::Label(fmt::format("Clock {}   video {}", timeText(gb->jupiterClickBarPosSec),
                                       timeText(vm::targetSec()))
                               .c_str(),
                           Tone::Muted);
                break;
            }
        }

        if (!gb->jupiterClickBarEnabled)
            kit::Note("The click bar is off, so its clock stands still and so does the video. Turn it on in the "
                      "Click Trainer above.",
                      Tone::Warn);
        else
            kit::Hint("Resume, restart or drag the click bar above to play and skim the video; in the level it "
                      "follows your run.");

        // Shown in percent, kept as 0..1.
        float percent = gb->jupiterVideoOpacity * 100.f;
        if (kit::SliderRow("Opacity", nullptr, &percent, 5.f, 100.f, "%.0f%%")) {
            gb->jupiterVideoOpacity = std::clamp(percent / 100.f, 0.05f, 1.f);
            vm::saveSettings();
        }

        // Wide enough to put any moment of the video at the macro's start.
        float const range = std::max(60.f, (float)std::ceil(vm::durationSec()));
        bool offsetChanged = false;
        kit::RowBegin("Sync offset", "Where the video is when the macro starts. Positive starts it further in.");
        offsetChanged |= kit::SliderFloat("offset", &gb->jupiterVideoOffsetSec, -range, range, "%.3f s");
        kit::RowEnd();
        float const frame = (float)(vm::frameSec() > 0.0 ? vm::frameSec() : 1.0 / 60.0);
        kit::RowBegin("Nudge", nullptr);
        if (kit::Button("-1 s##offset")) {
            gb->jupiterVideoOffsetSec -= 1.f;
            offsetChanged = true;
        }
        ImGui::SameLine();
        if (kit::Button("-1 frame##offset")) {
            gb->jupiterVideoOffsetSec -= frame;
            offsetChanged = true;
        }
        ImGui::SameLine();
        if (kit::Button("+1 frame##offset")) {
            gb->jupiterVideoOffsetSec += frame;
            offsetChanged = true;
        }
        ImGui::SameLine();
        if (kit::Button("+1 s##offset")) {
            gb->jupiterVideoOffsetSec += 1.f;
            offsetChanged = true;
        }
        ImGui::SameLine();
        if (kit::Button("Zero##offset")) {
            gb->jupiterVideoOffsetSec = 0.f;
            offsetChanged = true;
        }
        kit::RowEnd();
        if (offsetChanged) {
            gb->jupiterVideoOffsetSec = std::clamp(gb->jupiterVideoOffsetSec, -600.f, 600.f);
            vm::saveSettings();
        }

        if (vm::status() == vm::Status::Ready)
            alignmentTool();

        if (!g_message.empty())
            kit::Note(g_message.c_str(), g_messageTone);
        kit::EndCard();
    }

} // namespace gucci::ui::pages
