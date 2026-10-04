// Editor Tools page: Replace All and Macro Buffing (the trail buffer). Both
// still worked after -bz, but nothing in the new menu reached them.
//
// Macro Buffing's controls follow Silicate's own trail buffer card
// (src/ui/manager.cpp, peony, GPL-3.0): its labels, ranges and the note for an
// empty recording. Its settings live in SLSettings::trailBuffer, whose handles
// don't save anything in GucciBot (analysis/ac/shim.hpp), so this page saves
// them, under the keys Silicate's handles use.

#include "ui/kit.hpp"
#include "ui/pages.hpp"

#include "analysis/ac/shim.hpp"
#include "tools/replace_all.hpp"
#include "trailbuf/trailbuf.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>

#include <algorithm>
#include <string>
#include <type_traits>
#include <variant>

using namespace geode::prelude;

namespace gucci::ui::pages {

    namespace {

        using TrailSettings = std::remove_reference_t<decltype(SLSettings::get()->trailBuffer)>;
        using Field = std::variant<bool TrailSettings::*, int TrailSettings::*, float TrailSettings::*>;

        struct SavedField {
            const char* key;
            Field field;
        };

        // Every trail buffer setting the generator reads.
        const SavedField kTrailFields[] = {
            {"trailbuf.enabled", &TrailSettings::enabled},
            {"trailbuf.object_id", &TrailSettings::objectId},
            {"trailbuf.gap", &TrailSettings::gap},
            {"trailbuf.inner_hitbox", &TrailSettings::useInnerHitbox},
            {"trailbuf.fill_radius", &TrailSettings::fillRadius},
            {"trailbuf.column_width", &TrailSettings::columnWidth},
            {"trailbuf.min_block_size", &TrailSettings::minBlockSize},
            {"trailbuf.max_scale", &TrailSettings::maxScale},
            {"trailbuf.sweep", &TrailSettings::sweepBetweenTicks},
            {"trailbuf.max_objects", &TrailSettings::maxObjects},
            {"trailbuf.skip_solids", &TrailSettings::skipSolids},
            {"trailbuf.frame_interval", &TrailSettings::frameInterval},
            {"trailbuf.gate_width", &TrailSettings::gateWidth},
            {"trailbuf.merge_tolerance", &TrailSettings::mergeTolerance},
            {"trailbuf.separate_players", &TrailSettings::separatePlayers},
            {"trailbuf.object_id_p2", &TrailSettings::objectIdP2},
            {"trailbuf.start_trim", &TrailSettings::startTrim},
            {"trailbuf.end_trim", &TrailSettings::endTrim},
            {"trailbuf.spike_player2", &TrailSettings::spikePlayer2},
            {"trailbuf.spike_object_id", &TrailSettings::spikeObjectId},
            {"trailbuf.spike_gap", &TrailSettings::spikeGap},
            {"trailbuf.spike_below", &TrailSettings::spikeBelow},
            {"trailbuf.spike_above", &TrailSettings::spikeAbove},
            {"trailbuf.spike_left", &TrailSettings::spikeLeft},
            {"trailbuf.spike_right", &TrailSettings::spikeRight},
            {"trailbuf.spike_every_nth", &TrailSettings::spikeEveryNth},
            {"trailbuf.spike_around_clicks", &TrailSettings::spikeAroundClicks},
            {"trailbuf.spike_click_radius", &TrailSettings::spikeClickRadius},
            {"trailbuf.spike_releases", &TrailSettings::spikeReleases},
        };

        void saveTrailSettings() {
            auto& s = SLSettings::get()->trailBuffer;
            auto* mod = Mod::get();
            for (auto const& f : kTrailFields)
                std::visit([&](auto member) { mod->setSavedValue(f.key, s.*member); }, f.field);
        }

        bool intRow(const char* label, int* v, int least, int most) {
            kit::RowBegin(label, nullptr);
            bool const changed = kit::InputInt(label, v, 1);
            kit::RowEnd();
            if (changed)
                *v = std::clamp(*v, least, most);
            return changed;
        }

        bool floatRow(const char* label, float* v, float least, float most, const char* fmt) {
            kit::RowBegin(label, nullptr);
            bool const changed = kit::InputFloat(label, v, 0.f, fmt);
            kit::RowEnd();
            if (changed)
                *v = std::clamp(*v, least, most);
            return changed;
        }

        std::string g_replaceMessage;
        bool g_replaceOk = true;
        std::string g_trailMessage;
        bool g_trailOk = true;

        void replaceAllCard(LevelEditorLayer* editor) {
            kit::BeginCard("Replace All", "Every object of one ID becomes another, keeping everything else about it");
            static int s_from = 1;
            static int s_to = 1;
            intRow("From object ID", &s_from, 1, 100000);
            intRow("To object ID", &s_to, 1, 100000);
            ImGui::BeginDisabled(!editor || s_from == s_to);
            if (kit::Button("Replace all", Tone::Accent)) {
                auto const r = editortools::replaceAll(s_from, s_to);
                g_replaceOk = r.ok;
                g_replaceMessage = !r.message.empty() ? r.message : fmt::format("Replaced {} objects.", r.replaced);
            }
            ImGui::EndDisabled();
            if (!editor)
                kit::Hint("Open the level editor to use it.");
            else
                kit::Hint("Undo takes two presses: the first removes the new objects, the second brings the old ones back.");
            if (!g_replaceMessage.empty())
                kit::Note(g_replaceMessage.c_str(), g_replaceOk ? Tone::Muted : Tone::Bad);
            kit::EndCard();
        }

        void macroBuffingCard(LevelEditorLayer* editor) {
            auto& tb = ::Bot::get()->trailBuffer();
            auto& s = SLSettings::get()->trailBuffer;
            bool changed = false;

            kit::BeginCard("Macro Buffing", "Lays the path you played into the level as objects");
            changed |= kit::SwitchRow("Record the path", "While you play the level for real", &s.enabled);
            if (tb.tickCount() == 0)
                kit::Note("Nothing recorded. Play the level for real, since editor playtests run on the frame rate.",
                          Tone::Muted);
            else
                kit::Hint(fmt::format("{} ticks ({} from spider snaps) | frames {} to {} | {}", tb.tickCount(),
                                      tb.dashSamples(), tb.firstFrame(), tb.lastFrame(),
                                      tb.sourceLevel().empty() ? "unknown level" : tb.sourceLevel())
                              .c_str());

            ImGui::BeginDisabled(!editor || tb.tickCount() == 0);
            if (kit::Button("Generate", Tone::Accent)) {
                auto const r = tb.generate(editor);
                g_trailMessage = r.message;
                g_trailOk = r.ok && r.verified;
            }
            ImGui::SameLine();
            if (kit::Button("Place spikes")) {
                auto const r = tb.placeSpikes(editor);
                g_trailMessage = r.message;
                g_trailOk = r.ok;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (kit::Button("Clear recording")) {
                tb.clear();
                g_trailMessage = "Recording cleared.";
                g_trailOk = true;
            }
            if (!editor)
                kit::Hint("Open the editor to generate.");
            if (!g_trailMessage.empty())
                kit::Note(g_trailMessage.c_str(), g_trailOk ? Tone::Muted : Tone::Bad);

            kit::Section("Walls");
            kit::Hint("Distances are in units. One block is 30.");
            changed |= intRow("Object ID", &s.objectId, 1, 100000);
            changed |= kit::SwitchRow("Separate object for player 2", nullptr, &s.separatePlayers);
            if (s.separatePlayers)
                changed |= intRow("Player 2 object ID", &s.objectIdP2, 1, 100000);
            changed |= floatRow("Clearance", &s.gap, 0.f, 30.f, "%.6f");
            changed |= kit::SwitchRow("Wall against inner hitbox", nullptr, &s.useInnerHitbox);
            changed |= floatRow("Fill radius", &s.fillRadius, 1.f, 900.f, "%.2f");
            changed |= floatRow("Column width", &s.columnWidth, 0.0001f, 30.f, "%.4f");
            changed |= floatRow("Minimum block size", &s.minBlockSize, 0.000001f, 30.f, "%.6f");
            changed |= floatRow("Maximum object scale", &s.maxScale, 1.f, 200.f, "%.0f");
            changed |= floatRow("Merge tolerance", &s.mergeTolerance, 0.f, 30.f, "%.4f");
            changed |= kit::SwitchRow("Keep the between-tick path clear", nullptr, &s.sweepBetweenTicks);
            changed |= kit::SwitchRow("Skip existing solids", nullptr, &s.skipSolids);
            changed |= intRow("Object limit", &s.maxObjects, 1, 800000);
            changed |= intRow("Wall every N frames", &s.frameInterval, 1, 240);
            if (s.frameInterval > 1)
                changed |= floatRow("Gate width", &s.gateWidth, 1.f, 900.f, "%.0f");
            changed |= floatRow("Open at start", &s.startTrim, 0.f, 900.f, "%.0f");
            changed |= floatRow("Open at end", &s.endTrim, 0.f, 900.f, "%.0f");

            kit::Section("Spikes");
            changed |= intRow("Spike object ID", &s.spikeObjectId, 1, 100000);
            changed |= floatRow("Spike gap", &s.spikeGap, 0.f, 30.f, "%.3f");
            changed |= kit::SwitchRow("Below the path", nullptr, &s.spikeBelow);
            changed |= kit::SwitchRow("Above the path", nullptr, &s.spikeAbove);
            changed |= kit::SwitchRow("Left of the path", nullptr, &s.spikeLeft);
            changed |= kit::SwitchRow("Right of the path", nullptr, &s.spikeRight);
            changed |= intRow("Every Nth", &s.spikeEveryNth, 1, 1000);
            changed |= kit::SwitchRow("Only around clicks", nullptr, &s.spikeAroundClicks);
            if (s.spikeAroundClicks) {
                changed |= intRow("Click radius", &s.spikeClickRadius, 0, 240);
                changed |= kit::SwitchRow("Releases count as clicks", nullptr, &s.spikeReleases);
            }
            changed |= kit::SwitchRow("Player 2's path too", nullptr, &s.spikePlayer2);
            kit::EndCard();

            if (changed)
                saveTrailSettings();
        }

    } // namespace

    void editorTools() {
        auto* editor = LevelEditorLayer::get();
        replaceAllCard(editor);
        macroBuffingCard(editor);
    }

} // namespace gucci::ui::pages

$on_mod(Loaded) {
    auto& s = SLSettings::get()->trailBuffer;
    auto* mod = Mod::get();
    for (auto const& f : gucci::ui::pages::kTrailFields)
        std::visit(
            [&](auto member) {
                using T = std::remove_reference_t<decltype(s.*member)>;
                s.*member = mod->getSavedValue<T>(f.key, s.*member);
            },
            f.field);
}
