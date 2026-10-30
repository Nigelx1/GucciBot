// The Video Mode cards: JMF's on the JMF Trainer page (the bundled showcase
// video) and the Trainer's on the Trainer page (a video you pick). The switch,
// opacity, the sync offset and the Alignment Tool; the last three are drawn
// over the video while it plays (videoModeSettings). The overlay and the
// decoder are the engine's, in trainers/videomode.*; this draws their
// controls. Written fresh for 2.0; the Trainer's since 2026-10-19.

#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/pages.hpp"

#include "core/GucciBot.hpp"
#include "trainers/videomode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

namespace gucci::ui::pages {

    namespace {

        namespace vm = gucci::videomode;
        namespace fs = std::filesystem;
        namespace gfile = geode::utils::file;

        // The last note, and the card it belongs to.
        std::string g_message;
        Tone g_messageTone = Tone::Muted;
        vm::Source g_messageFor = vm::Source::None;

        void say(vm::Source on, std::string text, Tone tone) {
            g_message = std::move(text);
            g_messageTone = tone;
            g_messageFor = on;
        }

        void showMessage(vm::Source card) {
            if (!g_message.empty() && g_messageFor == card)
                kit::Note(g_message.c_str(), g_messageTone);
        }

        // The file name of a UTF-8 path. '/' and '\' never occur inside a
        // UTF-8 sequence, so a byte search is safe.
        std::string fileName(std::string const& path) {
            auto const cut = path.find_last_of("/\\");
            return cut == std::string::npos ? path : path.substr(cut + 1);
        }

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

        // What differs between the two cards.
        struct Binding {
            vm::Source source;
            bool& enabled;
            bool& otherEnabled;  // the other Video Mode, switched off when this one goes on
            float& offset;
            float& opacity;
            double& clock;
            bool& barEnabled;
        };

        Binding jmfBinding() {
            auto* gb = GucciEngine::get();
            return {vm::Source::Jmf,          gb->jupiterVideoModeEnabled, gb->trainerVideoModeEnabled,
                    gb->jupiterVideoOffsetSec, gb->jupiterVideoOpacity,  gb->jupiterClickBarPosSec,
                    gb->jupiterClickBarEnabled};
        }

        Binding trainerBinding() {
            auto* gb = GucciEngine::get();
            return {vm::Source::Trainer,       gb->trainerVideoModeEnabled, gb->jupiterVideoModeEnabled,
                    gb->trainerVideoOffsetSec, gb->trainerVideoOpacity,  gb->trainerClickBarPosSec,
                    gb->trainerClickBarEnabled};
        }

        void alignmentTool(Binding const& b) {
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
                if (vm::setOffsetFromShownFrame())
                    say(b.source, fmt::format("Offset set to {:.3f} s. The video follows the clock again.", b.offset),
                        Tone::Good);
            }
            ImGui::EndDisabled();
        }

        // Everything under the switch, once this card's Video Mode is the one on.
        // With Video Mode on, the video and its click bar take the menu's
        // place, so this is drawn over the video (videoModeSettings, below).
        void cardBody(Binding const& b) {
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
                    kit::Label(fmt::format("Clock {}   video {}", timeText(b.clock), timeText(vm::targetSec())).c_str(),
                               Tone::Muted);
                    break;
                }
            }

            if (!b.barEnabled)
                kit::Note("The click bar is off, so its clock stands still and so does the video. Exit Video Mode "
                          "and turn it on in the Click Trainer.",
                          Tone::Warn);
            else
                kit::Hint("Resume, restart or drag the click bar to play and skim the video; in the level it "
                          "follows your run.");

            // Shown in percent, kept as 0..1.
            float percent = b.opacity * 100.f;
            if (kit::SliderRow("Opacity", nullptr, &percent, 5.f, 100.f, "%.0f%%")) {
                b.opacity = std::clamp(percent / 100.f, 0.05f, 1.f);
                vm::saveSettings();
            }

            // Wide enough to put any moment of the video at the macro's start.
            float const range = std::max(60.f, (float)std::ceil(vm::durationSec()));
            bool offsetChanged = false;
            kit::RowBegin("Sync offset", "Where the video is when the macro starts. Positive starts it further in.");
            offsetChanged |= kit::SliderFloat("offset", &b.offset, -range, range, "%.3f s");
            kit::RowEnd();
            float const frame = (float)(vm::frameSec() > 0.0 ? vm::frameSec() : 1.0 / 60.0);
            kit::RowBegin("Nudge", nullptr);
            if (kit::Button("-1 s##offset")) {
                b.offset -= 1.f;
                offsetChanged = true;
            }
            ImGui::SameLine();
            if (kit::Button("-1 frame##offset")) {
                b.offset -= frame;
                offsetChanged = true;
            }
            ImGui::SameLine();
            if (kit::Button("+1 frame##offset")) {
                b.offset += frame;
                offsetChanged = true;
            }
            ImGui::SameLine();
            if (kit::Button("+1 s##offset")) {
                b.offset += 1.f;
                offsetChanged = true;
            }
            ImGui::SameLine();
            if (kit::Button("Zero##offset")) {
                b.offset = 0.f;
                offsetChanged = true;
            }
            kit::RowEnd();
            if (offsetChanged) {
                b.offset = std::clamp(b.offset, -600.f, 600.f);
                vm::saveSettings();
            }

            if (vm::status() == vm::Status::Ready)
                alignmentTool(b);
        }

        // The switch: one Video Mode at a time.
        void videoSwitch(Binding const& b, const char* hint, bool canTurnOn) {
            auto* gb = GucciEngine::get();
            ImGui::BeginDisabled(!canTurnOn && !b.enabled);
            if (kit::SwitchRow("Video Mode", hint, &b.enabled)) {
                g_message.clear();
                gb->jupiterVideoAlignToolActive = false;  // the tool is shared by both Video Modes
                if (b.enabled)
                    b.otherEnabled = false;
            }
            ImGui::EndDisabled();
        }

        // ---- the Trainer's video picker
        //
        // The rules the BIG BRRRR track picker set (pages/themes.cpp), after
        // issue #1: the pick is awaited only from an arc coroutine on Geode's
        // arc runtime, never from a geode::Task; nothing ever cancels a pending
        // pick (the dialog's thread points into the coroutine's frame), so the
        // spawned handle is dropped at once and detaches; the result reaches
        // the page by value through a main-thread call; and not
        // async::spawn/TaskHolder, which this MSVC can't instantiate (C1001).

        struct PickedVideo {
            bool ready = false;
            std::optional<fs::path> path;  // nullopt: the dialog was cancelled
            std::string error;
        };
        PickedVideo s_picked;
        bool s_pickPending = false;

        arc::Future<int> pickVideoTask() {
            gfile::FilePickOptions options;
            options.filters.push_back({"Videos", {"*.mp4", "*.mkv", "*.mov", "*.webm", "*.avi"}});
            gfile::PickResult result = co_await gfile::pick(gfile::PickMode::OpenFile, std::move(options));
            PickedVideo got;
            got.ready = true;
            if (result.isOk())
                got.path = std::move(result).unwrap();
            else
                got.error = std::move(result).unwrapErr();
            geode::queueInMainThread([got = std::move(got)]() mutable {
                s_picked = std::move(got);
                s_pickPending = false;
            });
            co_return 0;
        }

        void pickVideo() {
            if (s_pickPending)
                return;
            s_pickPending = true;
            // The handle is a temporary: dropping it detaches the task.
            (void)geode::async::runtime().spawn(pickVideoTask());
        }

        void takePickedVideo() {
            if (!s_picked.ready)
                return;
            PickedVideo got = std::move(s_picked);
            s_picked = PickedVideo{};
            if (!got.error.empty()) {
                say(vm::Source::Trainer, "The file picker failed: " + got.error, Tone::Bad);
                return;
            }
            if (!got.path)
                return;
            auto* gb = GucciEngine::get();
            gb->trainerVideoPath = geode::utils::string::pathToString(*got.path);
            gb->jupiterVideoAlignToolActive = false;
            vm::saveSettings();
            say(vm::Source::Trainer, "Video chosen. Video Mode plays it on the Trainer's click bar.", Tone::Good);
        }

    } // namespace

    void videoModeCard() {
        vm::ensureLoaded();
        Binding const b = jmfBinding();

        kit::BeginCard("Video Mode",
                       "The built-in JMF showcase video, full screen in place of the menu with the click bar over it, "
                       "on the bar's clock, for reviewing a run against the macro's timing. No level needs to be "
                       "open.");
        videoSwitch(b, "Plays the showcase video that ships with the mod; nothing to set up", true);
        if (b.enabled && vm::source() == vm::Source::Jmf)
            cardBody(b);
        showMessage(vm::Source::Jmf);
        kit::EndCard();
    }

    void trainerVideoModeCard() {
        vm::ensureLoaded();
        takePickedVideo();
        auto* gb = GucciEngine::get();
        Binding const b = trainerBinding();

        kit::BeginCard("Video Mode",
                       "A video of your own, full screen in place of the menu with the click bar over it, on the bar's "
                       "clock, for reviewing a run against the macro's timing. No level needs to be open.");
        bool const haveVideo = !gb->trainerVideoPath.empty();
        kit::RowBegin("Video", haveVideo ? nullptr : "Pick one to start");
        if (haveVideo) {
            std::string const name = fileName(gb->trainerVideoPath);
            kit::Label(name.c_str(), Tone::Muted);
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(s_pickPending);
        if (kit::Button(s_pickPending ? "Choosing..." : haveVideo ? "Change" : "Choose a video"))
            pickVideo();
        ImGui::EndDisabled();
        kit::RowEnd();
        videoSwitch(b, "Plays the video you chose", haveVideo);
        if (b.enabled && vm::source() == vm::Source::Trainer)
            cardBody(b);
        showMessage(vm::Source::Trainer);
        kit::EndCard();
    }

    // While Video Mode is on, the video and its click bar take the menu's
    // place (ui/shell.cpp), so the card above can't be seen and its settings
    // couldn't be reached. They sit over the top of the video instead, in a
    // small window that starts folded each time Video Mode comes on.
    void videoModeSettings(bool jmf) {
        vm::ensureLoaded();
        Binding const b = jmf ? jmfBinding() : trainerBinding();
        if (!b.enabled || vm::source() != b.source)
            return;
        if (jmf)
            jupiterLookPush();
        ImGuiIO const& io = ImGui::GetIO();
        float const w = std::min(io.DisplaySize.x - 40.f, 560.f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 20.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, FLT_MAX));
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_Appearing);
        if (ImGui::Begin(jmf ? "Video settings##jmfvideosettings" : "Video settings##trainervideosettings", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
            cardBody(b);
            showMessage(b.source);
        }
        ImGui::End();
        if (jmf)
            jupiterLookPop();
    }

} // namespace gucci::ui::pages
