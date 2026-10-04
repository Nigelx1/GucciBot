// The Macro Tools page: Diff, Trim, Merge, and editing what a saved macro
// carries (its metadata and its mid-macro TPS changes). Everything works on
// saved GBR6 files by name, through GucciEngine's wrappers and
// tools/macro_ops; the loaded macro is never read or changed here. Trim and
// Merge write a new macro next to their source and reload the list. Written
// fresh for 2.0.

#include "ui/kit.hpp"
#include "ui/look.hpp"

#include "core/GucciBot.hpp"
#include "tools/edit_core.hpp"
#include "tools/macro_ops.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace gucci::ui::pages {

    void macroTools();

    namespace {

        namespace fs = std::filesystem;
        namespace mo = gucci::macroops;

        struct Message {
            std::string text;
            Tone tone = Tone::Muted;
        };

        void message(Message const& m) {
            if (!m.text.empty())
                kit::Note(m.text.c_str(), m.tone);
        }

        // A macro picker that remembers the name, not the position, so a
        // reloaded list (Trim and Merge add to it) keeps the choice.
        bool pickMacro(const char* id, std::string& name) {
            auto const& names = GucciEngine::get()->storedMacros;
            auto it = std::find(names.begin(), names.end(), name);
            int index = it == names.end() ? -1 : static_cast<int>(it - names.begin());
            if (!kit::Dropdown(id, &index, names) || index < 0 || index >= static_cast<int>(names.size()))
                return false;
            name = names[static_cast<size_t>(index)];
            return true;
        }

        void macroRow(const char* label, const char* id, std::string& name) {
            kit::RowBegin(label, nullptr);
            pickMacro(id, name);
            kit::RowEnd();
        }

        std::string fmtTps(double tps) {
            return fmt::format("{:g}", tps);
        }

        std::string actionName(mo::DiffItem const& d) {
            if (d.lane >= 0)
                return fmt::format("{} {}", edit::laneName(d.lane), d.press ? "press" : "release");
            switch (d.type) {
                case gb::ActionType::Death: return "Death";
                case gb::ActionType::Restart: return "Restart";
                case gb::ActionType::RestartFull: return "Full restart";
                case gb::ActionType::TPS: return "TPS change";
                default: return "Action";
            }
        }

        std::string describe(mo::DiffItem const& d) {
            std::string const what = actionName(d);
            switch (d.kind) {
                case mo::DiffKind::Moved: {
                    long long const by = static_cast<long long>(d.frameB) - static_cast<long long>(d.frameA);
                    return fmt::format("{}: A on {}, B on {} ({:+d})", what, d.frameA, d.frameB, by);
                }
                case mo::DiffKind::OnlyA:
                    return d.type == gb::ActionType::TPS
                               ? fmt::format("{} to {} only in A", what, fmtTps(d.valueA))
                               : fmt::format("{} only in A", what);
                case mo::DiffKind::OnlyB:
                    return d.type == gb::ActionType::TPS
                               ? fmt::format("{} to {} only in B", what, fmtTps(d.valueB))
                               : fmt::format("{} only in B", what);
                case mo::DiffKind::SubtickDiffers:
                    return fmt::format("{}: A at {:.4f} of the tick, B at {:.4f}", what, d.valueA, d.valueB);
                case mo::DiffKind::TpsDiffers:
                    return fmt::format("{}: A to {}, B to {}", what, fmtTps(d.valueA), fmtTps(d.valueB));
            }
            return what;
        }

        Tone toneOf(mo::DiffKind k) {
            switch (k) {
                case mo::DiffKind::Moved: return Tone::Warn;
                case mo::DiffKind::OnlyA:
                case mo::DiffKind::OnlyB: return Tone::Bad;
                default: return Tone::Muted;
            }
        }

        // ------------------------------------------------------------ Diff

        struct DiffState {
            std::string a, b;
            bool ran = false;
            std::string ranA, ranB;
            GucciEngine::MacroDiffResult result;
        };
        DiffState g_diff;
        int g_moveWindow = -1;  // loaded on first draw

        void diffCard() {
            auto* gb = GucciEngine::get();
            auto& s = g_diff;
            kit::BeginCard("Diff", "Compare two macros frame by frame.");
            macroRow("Macro A", "diffA", s.a);
            macroRow("Macro B", "diffB", s.b);
            if (kit::SliderRow("Moved within",
                               "An input this many frames or fewer from the same input in the other macro counts as "
                               "moved, not as one taken out and another put in. 0 = only the same frame matches",
                               &g_moveWindow, 0, 60, "%d frames"))
                Mod::get()->setSavedValue<int>("macro_tools_move_window", g_moveWindow);

            bool const ready = !s.a.empty() && !s.b.empty();
            ImGui::BeginDisabled(!ready);
            if (kit::Button("Compare", Tone::Accent)) {
                s.result = gb->diffMacros(s.a, s.b, g_moveWindow);
                s.ran = true;
                s.ranA = s.a;
                s.ranB = s.b;
            }
            ImGui::EndDisabled();
            if (!ready)
                kit::Hint("Pick two macros.");

            if (s.ran) {
                auto const& r = s.result;
                if (!r.ok) {
                    message({r.error, Tone::Bad});
                } else if (r.diff.identical()) {
                    kit::Note(fmt::format("'{}' and '{}' are the same: {} inputs, all on the same frames.", s.ranA,
                                          s.ranB, r.diff.matched)
                                  .c_str(),
                              Tone::Good);
                } else {
                    auto const& d = r.diff;
                    kit::Label(fmt::format("A = '{}', B = '{}'", s.ranA, s.ranB).c_str(), Tone::Muted);
                    kit::Chip(fmt::format("{} the same", d.matched).c_str(), Tone::Good);
                    ImGui::SameLine();
                    kit::Chip(fmt::format("{} moved", d.moved).c_str(), d.moved ? Tone::Warn : Tone::Muted);
                    ImGui::SameLine();
                    kit::Chip(fmt::format("{} only in A", d.onlyA).c_str(), d.onlyA ? Tone::Bad : Tone::Muted);
                    ImGui::SameLine();
                    kit::Chip(fmt::format("{} only in B", d.onlyB).c_str(), d.onlyB ? Tone::Bad : Tone::Muted);
                    if (d.subtick || d.tps) {
                        kit::Chip(fmt::format("{} sub-tick", d.subtick).c_str(), Tone::Muted);
                        ImGui::SameLine();
                        kit::Chip(fmt::format("{} TPS", d.tps).c_str(), Tone::Muted);
                    }

                    std::string summary;
                    if (!d.items.empty()) {
                        auto const& first = d.items.front();
                        uint32_t const at = first.kind == mo::DiffKind::Moved ? std::min(first.frameA, first.frameB)
                                                                              : first.frame();
                        summary += fmt::format("They part ways on frame {}. ", at);
                    }
                    if (d.startTpsDiffers)
                        summary += "They start at different TPS. ";
                    if (d.levelDiffers)
                        summary += "They name different levels. ";
                    if (d.seedDiffers)
                        summary += "They started with different random seeds, so random triggers can play "
                                   "differently. ";
                    if (!summary.empty())
                        kit::Paragraph(summary.c_str());

                    if (!d.items.empty()) {
                        float const rowH = ImGui::GetTextLineHeightWithSpacing();
                        float const h = std::min(260.f, rowH * static_cast<float>(d.items.size()) + 12.f);
                        ImGui::BeginChild("##diffList", ImVec2(0.f, h), ImGuiChildFlags_FrameStyle);
                        ImGuiListClipper clip;
                        clip.Begin(static_cast<int>(d.items.size()));
                        while (clip.Step()) {
                            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                                auto const& it = d.items[static_cast<size_t>(i)];
                                uint32_t const at =
                                    it.kind == mo::DiffKind::Moved ? std::min(it.frameA, it.frameB) : it.frame();
                                ImGui::TextColored(look().tone(Tone::Muted), "%u", at);
                                ImGui::SameLine(ImGui::GetFontSize() * 5.f);
                                ImGui::TextColored(look().tone(toneOf(it.kind)), "%s", describe(it).c_str());
                            }
                        }
                        ImGui::EndChild();
                    }
                }
            }
            kit::EndCard();
        }

        // ------------------------------------------------------------ Trim

        struct TrimState {
            std::string name;
            int start = 0;
            int end = 0;
            Message msg;
        };
        TrimState g_trim;
        bool g_rebase = false;  // loaded on first draw

        void trimCard() {
            auto* gb = GucciEngine::get();
            auto& s = g_trim;
            kit::BeginCard("Trim", "Keep one stretch of a macro, as a new macro.");
            macroRow("Macro", "trimMacro", s.name);
            kit::RowBegin("From frame", nullptr);
            if (kit::InputInt("start", &s.start, 10))
                s.start = std::max(0, s.start);
            kit::RowEnd();
            kit::RowBegin("To frame", "Included");
            if (kit::InputInt("end", &s.end, 10))
                s.end = std::max(0, s.end);
            kit::RowEnd();
            if (kit::SwitchRow("Move it to the start",
                               "The first frame of the stretch becomes frame 1, so the new macro plays it from the "
                               "level's start. Off keeps every frame number",
                               &g_rebase))
                Mod::get()->setSavedValue<bool>("macro_tools_trim_rebase", g_rebase);

            bool const ready = !s.name.empty() && s.end >= s.start;
            ImGui::BeginDisabled(!ready);
            if (kit::Button("Trim", Tone::Accent)) {
                auto r = gb->trimMacro(s.name, s.start, s.end, g_rebase);
                if (r.outcome.ok)
                    s.msg = {fmt::format("Saved as '{}': {} inputs. {}", r.savedAs,
                                         mo::stats(r.outcome.macro).inputs, r.outcome.message),
                             Tone::Good};
                else
                    s.msg = {r.outcome.message, Tone::Bad};
            }
            ImGui::EndDisabled();
            if (s.name.empty())
                kit::Hint("Pick a macro.");
            else if (s.end < s.start)
                kit::Hint("The end is before the start.");
            kit::Hint("A button held into the stretch is pressed on its first frame, and one held past its end is "
                      "let go of on the frame after, so every press keeps its release.");
            message(s.msg);
            kit::EndCard();
        }

        // ------------------------------------------------------------ Merge

        struct MergeState {
            std::string a, b;
            Message msg;
        };
        MergeState g_merge;
        int g_gap = -1;  // loaded on first draw

        void mergeCard() {
            auto* gb = GucciEngine::get();
            auto& s = g_merge;
            kit::BeginCard("Merge", "Play one macro after another, as a new macro.");
            macroRow("First (A)", "mergeA", s.a);
            macroRow("Then (B)", "mergeB", s.b);
            kit::RowBegin("Gap", "Frames between A's last action and B's frame 0");
            if (kit::InputInt("gap", &g_gap, 1)) {
                g_gap = std::max(0, g_gap);
                Mod::get()->setSavedValue<int>("macro_tools_merge_gap", g_gap);
            }
            kit::RowEnd();

            bool const ready = !s.a.empty() && !s.b.empty();
            ImGui::BeginDisabled(!ready);
            if (kit::Button("Merge", Tone::Accent)) {
                auto r = gb->mergeMacros(s.a, s.b, g_gap);
                if (r.outcome.ok)
                    s.msg = {fmt::format("Saved as '{}': {} inputs. {}", r.savedAs,
                                         mo::stats(r.outcome.macro).inputs, r.outcome.message),
                             Tone::Good};
                else
                    s.msg = {r.outcome.message, Tone::Bad};
            }
            ImGui::EndDisabled();
            if (!ready)
                kit::Hint("Pick both macros.");
            kit::Hint("B's frames are counted on from where A ends. The new macro keeps A's level and random seed.");
            message(s.msg);
            kit::EndCard();
        }

        // ------------------------------------------------------------ Edit (metadata + TPS changes)

        struct EditState {
            std::string pick;      // what the picker shows
            std::string name;      // what is open
            fs::path path;
            std::optional<mo::Macro> macro;
            mo::Stats stats;
            bool dirty = false;
            Message msg;
            int addFrame = 1;
            double addTps = 240.0;
            Message tpsMsg;
        };
        EditState g_edit;

        void openForEdit(std::string const& name) {
            auto& s = g_edit;
            s = EditState{};
            s.pick = name;
            s.name = name;
            s.path = GucciEngine::get()->findMacroFile(name);
            std::string err;
            s.macro = s.path.empty() ? std::nullopt : mo::load(s.path, &err);
            if (!s.macro) {
                s.msg = {s.path.empty() ? "That macro isn't there any more." : err, Tone::Bad};
                return;
            }
            s.stats = mo::stats(*s.macro);
            s.addTps = s.macro->header.tps > 0.f ? s.macro->header.tps : 240.0;
        }

        std::string savedWhen(int64_t ts) {
            if (ts <= 0)
                return "unknown";
            std::time_t const t = static_cast<std::time_t>(ts);
            std::tm tm{};
#ifdef _WIN32
            if (localtime_s(&tm, &t) != 0)
                return "unknown";
#else
            if (!localtime_r(&t, &tm))
                return "unknown";
#endif
            char buf[64];
            if (!std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm))
                return "unknown";
            return buf;
        }

        // Play time up to `last`: each stretch between TPS changes at its own
        // rate.
        double lengthSeconds(mo::Macro const& m, uint32_t last) {
            double rate = m.header.tps > 0.f ? m.header.tps : 240.0;
            double secs = 0.0;
            uint32_t from = 0;
            for (auto const& c : mo::tpsChanges(m)) {
                if (c.frame >= last)
                    break;
                secs += static_cast<double>(c.frame - from) / rate;
                from = c.frame;
                rate = c.tps;
            }
            return secs + static_cast<double>(last - from) / rate;
        }

        void metadataSection(EditState& s) {
            auto& h = s.macro->header;
            kit::Section("Metadata");

            kit::RowBegin("Name in the file",
                          "What Calculate and Assistant Access call it. The list shows the file's name, which this "
                          "doesn't change");
            s.dirty |= kit::InputText("hdrName", &h.name);
            kit::RowEnd();

            kit::RowBegin("Level", "The Trainer plays its stats, ghosts and music only in a level of this name");
            s.dirty |= kit::InputText("hdrLevel", &h.levelName, "no level");
            kit::RowEnd();

            kit::RowBegin("Starting TPS",
                          "The rate it starts at, and what loading it sets the game to. Frames stay where they are, "
                          "so a different rate changes how long each one lasts");
            double tps = h.tps;
            if (kit::InputDouble("hdrTps", &tps, 0.0, "%g") && std::isfinite(tps) && tps > 0.0) {
                h.tps = static_cast<float>(tps);
                s.dirty = true;
            }
            kit::RowEnd();

            kit::RowBegin("Random seed", "What the level's random triggers started from. Not editable: another "
                                         "seed makes them play differently than when it was recorded");
            kit::Label(fmt::format("{}", h.rngSeed).c_str());
            kit::RowEnd();

            kit::RowBegin("Saved", nullptr);
            kit::Label(savedWhen(h.timestamp).c_str());
            kit::RowEnd();

            auto const& st = s.stats;
            kit::Chip(fmt::format("{} clicks", st.clicks).c_str(), Tone::Accent);
            ImGui::SameLine();
            kit::Chip(fmt::format("{} inputs", st.inputs).c_str(), Tone::Muted);
            ImGui::SameLine();
            kit::Chip(fmt::format("last frame {} (~{:.1f} s)", st.lastFrame, lengthSeconds(*s.macro, st.lastFrame))
                          .c_str(),
                      Tone::Muted);
            if (st.resets || st.subticks || st.player2) {
                if (st.resets) {
                    kit::Chip(fmt::format("{} deaths/restarts", st.resets).c_str(), Tone::Muted);
                    ImGui::SameLine();
                }
                if (st.subticks) {
                    kit::Chip(fmt::format("{} sub-tick inputs", st.subticks).c_str(), Tone::Muted);
                    ImGui::SameLine();
                }
                if (st.player2)
                    kit::Chip("2 players", Tone::Muted);
                else
                    ImGui::NewLine();
            }
            if (st.problems)
                kit::Note(fmt::format("Check Macro finds {} problem(s) in it.", st.problems).c_str(), Tone::Warn);
        }

        void tpsSection(EditState& s) {
            auto& m = *s.macro;
            kit::Section("TPS changes");
            kit::Hint("From a change's frame on, the macro plays at its rate. Inputs keep their frame numbers. A "
                      "GucciBot from before 2.0 plays the whole macro at the starting TPS.");

            auto const changes = mo::tpsChanges(m);
            if (changes.empty())
                kit::Label("None: it plays at the starting TPS throughout.", Tone::Muted);
            std::optional<uint32_t> remove;
            for (auto const& c : changes) {
                ImGui::PushID(static_cast<int>(c.frame));
                kit::RowBegin(fmt::format("Frame {}", c.frame).c_str(), nullptr);
                if (kit::Button("Remove", Tone::Bad))
                    remove = c.frame;
                ImGui::SameLine();
                kit::Label(fmt::format("{} TPS", fmtTps(c.tps)).c_str());
                kit::RowEnd();
                ImGui::PopID();
            }
            if (remove && mo::removeTpsChange(m, *remove)) {
                s.dirty = true;
                s.tpsMsg = {};
            }

            kit::RowBegin("Add at frame", "On a frame that already has one, this changes its rate");
            if (kit::InputInt("addFrame", &s.addFrame, 1))
                s.addFrame = std::max(1, s.addFrame);
            kit::RowEnd();
            kit::RowBegin("New TPS", nullptr);
            kit::InputDouble("addTps", &s.addTps, 0.0, "%g");
            kit::RowEnd();
            if (kit::Button("Add TPS change")) {
                std::string err;
                if (mo::setTpsChange(m, static_cast<uint32_t>(std::max(0, s.addFrame)), s.addTps, &err)) {
                    s.dirty = true;
                    s.tpsMsg = {};
                    if (static_cast<uint32_t>(s.addFrame) > s.stats.lastFrame)
                        s.tpsMsg = {"That's after the macro's last action, so it changes nothing while it plays.",
                                    Tone::Warn};
                } else {
                    s.tpsMsg = {err, Tone::Bad};
                }
            }
            message(s.tpsMsg);
        }

        void editCard() {
            auto* gb = GucciEngine::get();
            auto& s = g_edit;
            kit::BeginCard("Edit a macro", "What a saved macro carries besides its inputs.");

            kit::RowBegin("Macro", s.dirty ? "Save or Revert first" : nullptr);
            ImGui::BeginDisabled(s.dirty);
            if (kit::Button("Open") && !s.pick.empty())
                openForEdit(s.pick);
            ImGui::SameLine();
            pickMacro("editMacro", s.pick);
            ImGui::EndDisabled();
            kit::RowEnd();

            if (!s.macro) {
                if (s.msg.text.empty())
                    kit::Hint("Pick a macro and press Open.");
                message(s.msg);
                kit::EndCard();
                return;
            }

            kit::Label(fmt::format("Editing '{}'", s.name).c_str(), Tone::Accent);
            metadataSection(s);
            tpsSection(s);

            kit::Divider();
            ImGui::BeginDisabled(!s.dirty);
            if (kit::Button("Save", Tone::Accent)) {
                std::string err;
                if (mo::save(*s.macro, s.path, &err)) {
                    s.dirty = false;
                    s.stats = mo::stats(*s.macro);
                    s.msg = {fmt::format("Saved '{}'.", s.name), Tone::Good};
                    log::info("[GucciBot] Macro Tools: saved metadata/TPS changes of '{}'", s.name);
                } else {
                    s.msg = {err, Tone::Bad};
                }
            }
            ImGui::SameLine();
            if (kit::Button("Revert"))
                openForEdit(s.name);
            ImGui::EndDisabled();
            message(s.msg);

            // The loaded macro is a copy in memory: it doesn't see the file
            // change, and saving it from the Macro page would write over it.
            if (s.name == gb->replayName && !gb->replay.m_actionAtom.empty()) {
                kit::Note("This is the macro you have loaded. It plays as it was loaded until you load it again, "
                          "and saving it from the Macro page would put back what it was.",
                          Tone::Warn);
                ImGui::BeginDisabled(gb->isRecording() || s.dirty);
                if (kit::Button("Load it again")) {
                    gb->replay.load(s.path);
                    gb->replayName = s.name;
                }
                ImGui::EndDisabled();
            }
            kit::EndCard();
        }

        void loadChoices() {
            auto* mod = Mod::get();
            if (g_moveWindow < 0)
                g_moveWindow = std::clamp(static_cast<int>(mod->getSavedValue<int64_t>("macro_tools_move_window", 5)), 0, 60);
            if (g_gap < 0)
                g_gap = std::max(0, static_cast<int>(mod->getSavedValue<int64_t>("macro_tools_merge_gap", 0)));
            static bool rebaseLoaded = false;
            if (!rebaseLoaded) {
                g_rebase = mod->getSavedValue<bool>("macro_tools_trim_rebase", false);
                rebaseLoaded = true;
            }
        }

    } // namespace

    void macroTools() {
        loadChoices();
        auto* gb = GucciEngine::get();

        kit::BeginCard("Macro Tools",
                       "Work on your saved macros. Nothing here changes the macro you have loaded; Trim and Merge "
                       "save a new macro next to the one they start from.");
        kit::RowBegin("Saved macros", gb->storedMacros.empty() ? "None yet" : nullptr);
        if (kit::Button("Refresh list"))
            gb->reloadMacroList();
        ImGui::SameLine();
        kit::Label(fmt::format("{}", gb->storedMacros.size()).c_str(), Tone::Muted);
        kit::RowEnd();
        kit::EndCard();

        diffCard();
        trimCard();
        mergeCard();
        editCard();
    }

} // namespace gucci::ui::pages
