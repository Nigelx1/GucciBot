// The queues' cards (tools/queue.hpp; Juice's idea, 2026-11-17): "Queue" on
// the Pathfinder page (level IDs in, macros out) and on the Calculate page
// (saved macros in, frame windows out). Each card: what goes in, Start/Stop,
// what it's doing now, and a results table that a restart of the game keeps.

#include "core/platform.hpp"
#include "ui/pages.hpp"
#include "ui/kit.hpp"
#include "ui/look.hpp"
#include "ui/ui.hpp"
#include "core/GucciBot.hpp"
#include "hacks/legit.hpp"
#include "tools/queue.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace gucci::ui::pages {

    namespace {

        using queue::Kind;
        using queue::Status;

        std::string g_pfNote;       // what the last Add or Start said
        bool g_pfNoteOk = false;
        std::string g_fwNote;
        bool g_fwNoteOk = false;
        char g_ids[4096] = "";
        std::set<std::string> g_ticked;  // macros ticked in the frame-window card
        std::string g_filter;
        bool g_listLoaded = false;
        bool g_confirmPf = false;  // Start pressed once with an unsaved macro in memory
        bool g_confirmFw = false;

        const char* resultText(queue::Item const& it, Kind k, bool now, std::string& buf) {
            switch (it.status) {
                case Status::Pending: buf = "Waiting"; break;
                case Status::Running: buf = now ? "Working on it" : "Waiting"; break;
                case Status::Beaten: buf = k == Kind::Pathfinder ? "Beaten" : "Counted"; break;
                case Status::NotBeaten: buf = fmt::format("Best {:.1f}%", it.bestPercent); break;
                case Status::Failed: buf = "Failed"; break;
                case Status::Skipped: buf = "Skipped"; break;
            }
            return buf.c_str();
        }

        Tone toneOf(Status s) {
            switch (s) {
                case Status::Beaten: return Tone::Good;
                case Status::NotBeaten: return Tone::Warn;
                case Status::Failed:
                case Status::Skipped: return Tone::Bad;
                default: return Tone::Muted;
            }
        }

        void colored(const char* text, Tone t) {
            ImVec4 c = look().pal.ink;
            switch (t) {
                case Tone::Good: c = look().pal.good; break;
                case Tone::Warn: c = look().pal.warn; break;
                case Tone::Bad: c = look().pal.bad; break;
                case Tone::Muted: c = look().pal.muted; break;
                case Tone::Accent: c = look().pal.accent; break;
                default: break;
            }
            ImGui::TextColored(c, "%s", text);
        }

        std::string windowsText(queue::Item const& it) {
            if (it.windows < 0)
                return "-";
            if (it.lstar >= 0)
                return fmt::format("{} (L* {:.3f})", it.windows, it.lstar);
            return std::to_string(it.windows);
        }

        // The results, one row an item. The note (why it failed, what
        // happened) shows when the row is hovered, and under the table for the
        // one running.
        void resultsTable(Kind k) {
            auto const& st = queue::state(k);
            if (st.items.empty()) {
                kit::Hint(k == Kind::Pathfinder ? "Nothing in the queue yet." : "No macros in the queue yet.");
                return;
            }
            int const now = queue::currentIndex(k);
            ImGuiTableFlags const flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            float const rowH = ImGui::GetFrameHeightWithSpacing();
            float const height = std::min(300.f, rowH * ((float)st.items.size() + 1.5f));
            int removeAt = -1;
            if (ImGui::BeginTable(k == Kind::Pathfinder ? "##pfqueue" : "##fwqueue", 6, flags, ImVec2(0.f, height))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(k == Kind::Pathfinder ? "Level" : "Macro", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Result");
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn(k == Kind::Pathfinder ? "Macro" : "Level", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Windows");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (int i = 0; i < (int)st.items.size(); ++i) {
                    auto const& it = st.items[(size_t)i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    std::string first;
                    if (k == Kind::Pathfinder)
                        first = it.levelName.empty() ? std::to_string(it.levelId)
                                                     : fmt::format("{} ({})", it.levelName, it.levelId);
                    else
                        first = it.macro;
                    ImGui::TextUnformatted(first.c_str());
                    if (!it.note.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", it.note.c_str());
                    ImGui::TableNextColumn();
                    std::string buf;
                    colored(resultText(it, k, i == now, buf), i == now ? Tone::Accent : toneOf(it.status));
                    if (!it.note.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", it.note.c_str());
                    ImGui::TableNextColumn();
                    double const secs = i == now ? queue::currentSeconds(k) : it.seconds;
                    ImGui::TextUnformatted(secs > 0 ? queue::formatSeconds(secs).c_str() : "-");
                    ImGui::TableNextColumn();
                    std::string const second = k == Kind::Pathfinder ? (it.macro.empty() ? "-" : it.macro)
                                                                     : (it.levelName.empty() ? "-" : it.levelName);
                    ImGui::TextUnformatted(second.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(windowsText(it).c_str());
                    ImGui::TableNextColumn();
                    if (i != now && it.status != Status::Running) {
                        if (ImGui::SmallButton("x"))
                            removeAt = i;
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Take it out of the queue");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (removeAt >= 0)
                queue::remove(k, removeAt);
            // Notes of the failed ones, readable without hovering.
            for (auto const& it : st.items)
                if ((it.status == Status::Failed || it.status == Status::Skipped) && !it.note.empty())
                    kit::Hint(fmt::format("{}: {}",
                                          k == Kind::Pathfinder
                                              ? (it.levelName.empty() ? std::to_string(it.levelId) : it.levelName)
                                              : it.macro,
                                          it.note)
                                  .c_str());
            bool const run = queue::running(k);
            kit::RowBegin("", nullptr);
            ImGui::BeginDisabled(run);
            if (kit::Button("Clear finished", Tone::Muted))
                queue::clearFinished(k);
            ImGui::SameLine();
            if (kit::Button("Clear all", Tone::Muted))
                queue::clear(k);
            ImGui::EndDisabled();
            kit::RowEnd();
        }

        // Start/Stop, the continue question and what it's doing now.
        void controls(Kind k, std::string& note, bool& noteOk) {
            bool const run = queue::running(k);
            bool& confirm = k == Kind::Pathfinder ? g_confirmPf : g_confirmFw;
            if (run || !queue::unsavedMacroInMemory())
                confirm = false;
            if (queue::continueOffered(k)) {
                kit::Note(fmt::format("The game closed while this queue ran. Continue the queue ({} left)?",
                                      queue::state(k).left())
                              .c_str(),
                          Tone::Accent);
                kit::RowBegin("", nullptr);
                {
                    kit::LegitGate gate;
                    if (kit::Button("Continue", Tone::Accent))
                        queue::answerContinue(k, true);
                }
                ImGui::SameLine();
                if (kit::Button("Not now", Tone::Muted))
                    queue::answerContinue(k, false);
                kit::RowEnd();
            }
            kit::RowBegin("", nullptr);
            if (!run) {
                kit::LegitGate gate;  // a queue runs cheats only
                if (kit::Button(confirm ? "Start anyway" : "Start", Tone::Accent, -1.f)) {
                    if (!confirm && queue::unsavedMacroInMemory()) {
                        // The queue loads and records macros of its own: the
                        // one in memory would be gone.
                        confirm = true;
                        note = "The macro in memory isn't saved, and the queue will replace it. Save it on the "
                               "Macro page first, or press Start anyway.";
                        noteOk = false;
                    } else {
                        confirm = false;
                        note = queue::start(k);
                        noteOk = note.empty();
                    }
                }
            } else if (kit::Button("Stop", Tone::Bad, -1.f)) {
                queue::stop(k);
                note.clear();
            }
            kit::RowEnd();
            if (run) {
                auto const& st = queue::state(k);
                int const total = (int)st.items.size();
                int const done = total - st.left();
                kit::Progress(total > 0 ? (float)done / (float)total : 0.f,
                              fmt::format("{} of {} done", done, total).c_str());
                auto const line = queue::activity(k);
                if (!line.empty())
                    kit::Hint(line.c_str());
                kit::Hint("Safe mode is on until the queue ends.");
            }
            if (!run && queue::holdingUntilLeft())
                kit::Hint("Safe mode stays on until you leave the level the queue had open.");
            if (!note.empty())
                kit::Note(note.c_str(), noteOk ? Tone::Muted : Tone::Warn);
        }

    }  // namespace

    void pathfinderQueueCard() {
        kit::BeginCard("Queue",
                       "Paste level IDs and leave your PC on: the Pathfinder plays each level in turn from the start "
                       "and saves what it finds. It's a search that tries inputs in a copy of the game's physics, "
                       "not a trained AI.");
        if (!queue::pathfinderAvailable()) {
            kit::Note("The Pathfinder queue isn't available here yet: for now it runs only on Windows with "
                      "GD 2.2081. The frame-window queue (Calculate page) works here.",
                      Tone::Warn);
            kit::EndCard();
            return;
        }
        auto const& st = queue::state(Kind::Pathfinder);
        bool const run = queue::running(Kind::Pathfinder);

        kit::RowBegin("Level IDs", "Online IDs, or 1 to 22 for RobTop's levels. Split them with spaces, commas or "
                                   "lines");
        ImGui::InputTextMultiline("##queue_ids", g_ids, sizeof g_ids, ImVec2(kit::Avail(), ImGui::GetTextLineHeight() * 3.5f));
        kit::RowEnd();
        kit::RowBegin("", nullptr);
        if (kit::Button("Add to the queue", Tone::Plain, -1.f)) {
            std::vector<std::string> bad;
            int const added = queue::addLevelIds(g_ids, &bad);
            std::string msg = added == 1 ? "Added 1 level." : fmt::format("Added {} levels.", added);
            if (!bad.empty()) {
                std::string list;
                for (size_t i = 0; i < bad.size() && i < 5; ++i)
                    list += (i ? ", " : "") + bad[i];
                msg += fmt::format(" Not level IDs, left out: {}{}", list, bad.size() > 5 ? "..." : "");
            } else if (added == 0) {
                msg = "Nothing new to add: type level IDs first.";
            }
            g_pfNote = msg;
            g_pfNoteOk = bad.empty() && added > 0;
            if (added > 0)
                g_ids[0] = '\0';
        }
        kit::RowEnd();

        ImGui::BeginDisabled(run);
        int limit = st.limitMinutes;
        kit::RowBegin("Time per level", "Minutes the Pathfinder gets on each level before it moves on (30 by default)");
        if (kit::InputInt("queue_limit", &limit, 5))
            queue::setLimitMinutes(limit);
        kit::RowEnd();
        bool count = st.countWindows;
        if (kit::SwitchRow("Count frame windows when beaten",
                           "On by default: Calculate counts each win's frame windows and saves them with its macro",
                           &count))
            queue::setCountWindows(count);
        ImGui::EndDisabled();

        controls(Kind::Pathfinder, g_pfNote, g_pfNoteOk);
        kit::Hint("A win is saved as \"<level> (<ID>)\", otherwise the furthest it got as \"<level> (<ID>) furthest\". "
                  "Levels are downloaded the way the game does when you open them.");
        resultsTable(Kind::Pathfinder);
        kit::EndCard();
    }

    void frameWindowQueueCard() {
        kit::BeginCard("Queue",
                       "Tick saved macros and leave your PC on: each one's level is opened and Calculate counts its "
                       "frame windows, saved with the macro as usual.");
        auto* gb = GucciEngine::get();
        bool const run = queue::running(Kind::FrameWindows);
        if (!g_listLoaded) {
            gb->reloadMacroList();
            g_listLoaded = true;
        }
        kit::RowBegin("Saved macros", "Tick the ones to count");
        kit::InputText("queue_filter", &g_filter, "Search", 0, kit::Avail() - 90.f);
        ImGui::SameLine();
        if (kit::Button("Refresh", Tone::Muted, -1.f))
            gb->reloadMacroList();
        kit::RowEnd();
        std::vector<std::string> names = gb->storedMacros;
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        auto const lower = [](std::string s) {
            for (auto& c : s)
                c = (char)std::tolower((unsigned char)c);
            return s;
        };
        std::string const f = lower(g_filter);
        float const rowH = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##fwqueue_pick", ImVec2(0.f, std::min(220.f, rowH * ((float)names.size() + 0.5f))), true)) {
            int shown = 0;
            for (auto const& n : names) {
                if (!f.empty() && lower(n).find(f) == std::string::npos)
                    continue;
                shown++;
                bool on = g_ticked.count(n) > 0;
                ImGui::PushID(n.c_str());
                if (ImGui::Checkbox(n.c_str(), &on)) {
                    if (on)
                        g_ticked.insert(n);
                    else
                        g_ticked.erase(n);
                }
                ImGui::PopID();
            }
            if (shown == 0)
                ImGui::TextDisabled("%s", names.empty() ? "No saved macros." : "None match.");
        }
        ImGui::EndChild();
        kit::RowBegin("", nullptr);
        ImGui::BeginDisabled(g_ticked.empty());
        if (kit::Button(g_ticked.empty() ? "Add the ticked macros"
                                         : fmt::format("Add the {} ticked", g_ticked.size()).c_str(),
                        Tone::Plain, -1.f)) {
            int const added = queue::addMacros(std::vector<std::string>(g_ticked.begin(), g_ticked.end()));
            g_fwNote = added == 1 ? "Added 1 macro." : fmt::format("Added {} macros.", added);
            g_fwNoteOk = added > 0;
            if (added == 0)
                g_fwNote = "They're in the queue already.";
            g_ticked.clear();
        }
        ImGui::EndDisabled();
        kit::RowEnd();
        (void)run;

        controls(Kind::FrameWindows, g_fwNote, g_fwNoteOk);
        kit::Hint("Each macro's level is found by the \"(<ID>)\" in its name, or by the level name it was "
                  "recorded on. A level you've never downloaded is skipped.");
        resultsTable(Kind::FrameWindows);
        kit::EndCard();
    }

}  // namespace gucci::ui::pages
