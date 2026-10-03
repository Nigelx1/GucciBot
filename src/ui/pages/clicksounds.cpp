// The Click sounds page, written from scratch on 2026-10-02/03 (the earlier
// one went out with the ToastyReplay-derived code) and built from the kit
// alone. The sounds themselves are audio/clicks.*.

#include "ui/pages.hpp"

#include "audio/clicks.hpp"
#include "ui/kit.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include <string>
#include <vector>

namespace gucci::ui::pages {

    namespace {

        std::string counted(int n, char const* what) {
            return fmt::format("{} {}{}", n, what, n == 1 ? "" : "s");
        }

        // A pack picker row. A chosen pack that has gone stays in the list,
        // marked, so the dropdown still says what is chosen.
        bool packRow(char const* label, char const* id, std::string* pack) {
            auto const& names = clicks::packNames();
            int index = -1;
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i] == *pack) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            bool changed = false;
            kit::RowBegin(label, nullptr);
            if (index >= 0) {
                changed = kit::Dropdown(id, &index, names);
            } else {
                std::vector<std::string> shown = names;
                shown.push_back(*pack + " (missing)");
                index = static_cast<int>(names.size());
                changed = kit::Dropdown(id, &index, shown) && index < static_cast<int>(names.size());
            }
            kit::RowEnd();
            if (changed)
                *pack = names[static_cast<size_t>(index)];
            return changed;
        }

        // What the chosen pack holds, under its picker.
        void packStatus(bool p2) {
            auto const st = clicks::status(p2);
            if (!st.audioReady) {
                kit::Note("The game's sound system is not up yet.", Tone::Warn);
                return;
            }
            if (st.missing) {
                std::string const text =
                    st.builtIn ? fmt::format("That pack is gone, so {} plays instead. Put it back in the "
                                             "clickpacks folder and press Refresh.",
                                             st.name)
                               : std::string("That pack is gone, and there is no built-in pack to stand in.");
                kit::Note(text.c_str(), Tone::Warn);
            }
            kit::Hint(fmt::format("{}{}, {}, {}, {}", st.builtIn ? "Built in  |  " : "",
                                  counted(st.clicks, "click"), counted(st.releases, "release"),
                                  counted(st.softClicks, "soft click"), counted(st.softReleases, "soft release"))
                          .c_str());
            if (st.clicks + st.softClicks == 0)
                kit::Note("This pack has no click sounds, so presses stay silent.", Tone::Warn);
            if (st.unreadable > 0)
                kit::Note(fmt::format("{} could not be opened; the Geode log says which.",
                                      counted(st.unreadable, "sound file"))
                              .c_str(),
                          Tone::Warn);
        }

    } // namespace

    void clickSounds() {
        auto& s = clicks::settings();
        bool dirty = false;

        kit::BeginCard("Click sounds", "A click when jump is pressed, a release when it is let go.");
        bool flipped = false;
        if (kit::FeatureBegin("On", nullptr, &s.enabled, nullptr, &flipped)) {
            if (packRow("Pack", "pack", &s.pack)) {
                dirty = true;
                clicks::playClick(false);  // hear the new pack straight away
            }
            packStatus(false);
            kit::RowBegin("", nullptr);
            if (kit::Button("Test", Tone::Accent))
                clicks::playClick(false);
            kit::RowEnd();

            float percent = s.volume * 100.f;
            if (kit::SliderRow("Volume", nullptr, &percent, 0.f, 200.f, "%.0f%%")) {
                s.volume = percent / 100.f;
                dirty = true;
            }
            dirty |= kit::SliderRow("Soft clicks",
                                    "A press this soon after a release, or a release this soon after its "
                                    "press, plays a soft sound. 0 never does",
                                    &s.softMs, 0, 300, "%d ms");
            dirty |= kit::SwitchRow("During playback", "Click for a playing macro's inputs too",
                                    &s.duringPlayback);
            dirty |= kit::SwitchRow("In renders", "Click while rendering, so the clicks are in the video",
                                    &s.inRenders);
            dirty |= kit::SwitchRow("Separate pack for player 2", "For two-player levels", &s.separateP2);
            if (s.separateP2) {
                if (packRow("Player 2 pack", "p2pack", &s.p2Pack)) {
                    dirty = true;
                    clicks::playClick(true);
                }
                packStatus(true);
                kit::RowBegin("", nullptr);
                if (kit::Button("Test player 2", Tone::Accent))
                    clicks::playClick(true);
                kit::RowEnd();
            }
            kit::FeatureEnd();
        }
        dirty |= flipped;
        kit::EndCard();

        kit::BeginCard("Your own packs", "Put a pack's folder in the clickpacks folder, then press Refresh.");
        kit::Paragraph("A pack is a folder holding up to four folders of sounds: clicks, releases, softClicks "
                       "and softReleases (WAV, MP3, OGG, FLAC or AIFF). Only clicks is needed, and its sounds "
                       "can also sit loose in the pack's own folder. A pack with player1 and player2 folders "
                       "gives each player their own sounds. Packs made for other bots usually work as they are.");
        kit::RowBegin("", nullptr);
        if (kit::Button("Open the clickpacks folder"))
            geode::utils::file::openFolder(clicks::packsDir());
        ImGui::SameLine();
        if (kit::Button("Refresh"))
            clicks::refresh();
        kit::RowEnd();
        kit::EndCard();

        if (dirty)
            clicks::saveSettings();
    }

} // namespace gucci::ui::pages
