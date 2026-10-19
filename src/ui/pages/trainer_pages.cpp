// The two practice trainer pages: Nigel's Jupiter My Favourite Trainer (one
// level, its macro built in) and the Trainer (any saved macro). They are the
// same page pointed at different state: a header, the Click Trainer (the bar,
// its clock controls, the deviation readout and Click Indicators scoring),
// Ghosts, Segments, Stats and Music. Everything that runs while the page is
// closed - the clock, input, scoring, the segment loop - is the engine's, in
// trainers/trainer_core.*; this file draws it and hands it the user's edits.
// Written fresh for 2.0.

#include "ui/kit.hpp"
#include "ui/look.hpp"

#include "core/GucciBot.hpp"
#include "trainers/trainer_core.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gucci::ui::pages {

    void jupiterTrainer();
    void anyTrainer();
    void videoModeCard();  // pages/videomode_card.cpp
    void indicators();     // pages/indicators.cpp (drawn at the foot of both trainer pages)

    namespace {

        namespace fs = std::filesystem;
        namespace gfile = geode::utils::file;
        namespace tr = gucci::trainers;
        using tr::Kind;

        constexpr float kBarH = 92.f;
        constexpr float kHeatH = 46.f;
        constexpr int kHeatBins = 50;

        struct Message {
            std::string text;
            Tone tone = Tone::Muted;
        };

        // What each page keeps between frames. None of it is a setting.
        struct PageState {
            int selected = -1;  // the segment open in the editor
            std::vector<tr::Segment> suggestions;
            bool suggested = false;
            std::string code;
            Message segMsg;
            // Dragging the bar: where the drag began, and whether the clock
            // was running then (it runs on again when the drag ends).
            bool dragging = false;
            bool resumeAfterDrag = false;
        };
        std::array<PageState, 2> g_page;

        PageState& page(Kind k) {
            return g_page[k == Kind::Jupiter ? 0 : 1];
        }

        // The Trainer page's macro picker and messages.
        int g_pick = -1;
        Message g_macroMsg;
        Message g_trackMsg;

        // ------------------------------------------------------------ the track picker
        //
        // The same rules as the themes page's (after issue #1): file::pick is
        // awaited only from an arc coroutine, a pending pick is never cancelled
        // or doubled, and the result reaches the page by value through a main
        // thread call.

        struct Picked {
            bool ready = false;
            std::optional<fs::path> path;  // nullopt: the dialog was cancelled
            std::string error;
        };
        Picked s_picked;
        bool s_pickPending = false;

        arc::Future<int> pickTask() {
            gfile::FilePickOptions options;
            options.filters.push_back({"MP3 audio", {"*.mp3"}});
            gfile::PickResult result = co_await gfile::pick(gfile::PickMode::OpenFile, std::move(options));
            Picked got;
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

        void pickTrack() {
            if (s_pickPending)
                return;
            s_pickPending = true;
            // The handle is a temporary: dropping it detaches the task.
            (void)geode::async::runtime().spawn(pickTask());
        }

        void takePicked() {
            if (!s_picked.ready)
                return;
            Picked got = std::move(s_picked);
            s_picked = Picked{};
            if (!got.error.empty()) {
                g_trackMsg = {"The file picker failed: " + got.error, Tone::Bad};
                return;
            }
            if (!got.path)
                return;
            std::string err;
            if (tr::importTrack(*got.path, err))
                g_trackMsg = {"Imported. It plays with the bar and in the macro's level.", Tone::Good};
            else
                g_trackMsg = {err, Tone::Bad};
        }

        // ------------------------------------------------------------ helpers

        std::string timeText(double sec) {
            if (sec < 0.0)
                sec = 0.0;
            int const m = (int)(sec / 60.0);
            double const s = sec - m * 60.0;
            return m > 0 ? fmt::format("{}:{:05.2f}", m, s) : fmt::format("{:.2f}s", s);
        }

        void message(Message const& m) {
            if (!m.text.empty())
                kit::Note(m.text.c_str(), m.tone);
        }

        int growString(ImGuiInputTextCallbackData* data) {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                auto* s = static_cast<std::string*>(data->UserData);
                s->resize(static_cast<size_t>(data->BufTextLen));
                data->Buf = s->data();
            }
            return 0;
        }

        bool multiline(const char* id, std::string* s, float height) {
            return ImGui::InputTextMultiline(id, s->data(), s->capacity() + 1, ImVec2(kit::Avail(), height),
                                             ImGuiInputTextFlags_CallbackResize, growString, s);
        }

        // ------------------------------------------------------------ the bar

        // A strip of macro time centred on the clock. The macro's clicks run
        // through the top lane (press to release; a press the player has
        // answered turns green), the player's own through the bottom (presses
        // full height, releases half), segments shade the background, and the
        // centre line stays put while everything scrolls past it.
        void drawBar(Kind k) {
            auto s = tr::state(k);
            auto& ps = page(k);
            auto const& pal = look().pal;
            bool const following = tr::followsLevel(k);

            float const w = kit::Avail();
            ImVec2 const p0 = ImGui::GetCursorScreenPos();
            ImVec2 const p1(p0.x + w, p0.y + kBarH);
            ImGui::InvisibleButton("##bar", ImVec2(w, kBarH));
            bool const hovered = ImGui::IsItemHovered();
            bool const active = ImGui::IsItemActive();

            double const span = std::max(0.5, (double)s.barWindowSec);
            double const pxPerSec = w / span;
            double const left = s.posSec - span * 0.5;
            double const right = s.posSec + span * 0.5;
            auto x = [&](double t) { return p0.x + (float)((t - left) * pxPerSec); };

            // Skimming: the clock follows the mouse while the bar is held.
            if (active && !following) {
                if (!ps.dragging) {
                    ps.dragging = true;
                    ps.resumeAfterDrag = !s.paused;
                    tr::setPaused(k, true);
                }
                float const dx = ImGui::GetIO().MouseDelta.x;
                if (dx != 0.f)
                    tr::seek(k, s.posSec - dx / pxPerSec);
            } else if (ps.dragging) {
                ps.dragging = false;
                if (ps.resumeAfterDrag)
                    tr::setPaused(k, false);
            }
            if (hovered && following)
                ImGui::SetTooltip("The bar follows the level while it's open.");
            else if (hovered && !active)
                ImGui::SetTooltip("Drag to skim.");

            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, u32(pal.raised, 0.55f), pal.radius);
            dl->PushClipRect(p0, p1, true);

            float const laneTop = p0.y + 18.f;
            float const laneMid = p0.y + 50.f;
            float const laneBot = p1.y - 6.f;

            // Segments, and the looped stretch outlined.
            auto const& segs = tr::segments(k);
            for (size_t i = 0; i < segs.size(); ++i) {
                auto const& seg = segs[i];
                if (seg.endSec < left || seg.startSec > right)
                    continue;
                bool const sel = (int)i == ps.selected;
                dl->AddRectFilled(ImVec2(x(seg.startSec), p0.y), ImVec2(x(seg.endSec), p1.y),
                                  u32(pal.accent, sel ? 0.16f : 0.08f));
                UseFont f(Font::Small);
                dl->AddText(ImVec2(std::max(x(seg.startSec), p0.x) + 4.f, p1.y - ImGui::GetFontSize() - 2.f),
                            u32(pal.muted), seg.name.c_str());
            }
            double lo = 0.0, hi = 0.0;
            if (s.loopEnabled && tr::loopRange(k, lo, hi)) {
                dl->AddLine(ImVec2(x(lo), p0.y), ImVec2(x(lo), p1.y), u32(pal.warn, 0.9f), 2.f);
                dl->AddLine(ImVec2(x(hi), p0.y), ImVec2(x(hi), p1.y), u32(pal.warn, 0.9f), 2.f);
            }

            // The ruler: a tick every half second (every second when zoomed out).
            {
                UseFont f(Font::Small);
                double const step = span > 4.0 ? 1.0 : 0.5;
                for (double t = std::ceil(std::max(0.0, left) / step) * step; t <= right; t += step) {
                    float const tx = x(t);
                    bool const whole = std::fabs(t - std::round(t)) < 1e-6;
                    dl->AddLine(ImVec2(tx, p0.y), ImVec2(tx, p0.y + (whole ? 8.f : 4.f)), u32(pal.line));
                    if (whole)
                        dl->AddText(ImVec2(tx + 3.f, p0.y + 1.f), u32(pal.muted), fmt::format("{}s", (int)t).c_str());
                }
            }

            // The macro's clicks.
            auto const& iv = s.macro.clickIntervalsSec;
            auto const& answered = s.score.answeredPress;
            bool const haveAnswers = answered.size() == iv.size();
            for (size_t i = 0; i < iv.size(); ++i) {
                double const a = iv[i].first, b = iv[i].second;
                if (b < left || a > right)
                    continue;
                float const xa = x(a), xb = std::max(x(b), x(a) + 2.f);
                dl->AddRectFilled(ImVec2(xa, laneTop), ImVec2(xb, laneMid - 4.f), u32(pal.accent, 0.45f), 3.f);
                ImVec4 const edge = haveAnswers && answered[i] ? pal.good : pal.accent;
                dl->AddLine(ImVec2(xa, laneTop - 2.f), ImVec2(xa, laneMid - 2.f), u32(edge), 2.5f);
            }

            // The player's own.
            ImU32 const white = IM_COL32(255, 255, 255, 235);
            for (double t : s.myPresses)
                if (t >= left && t <= right)
                    dl->AddLine(ImVec2(x(t), laneMid), ImVec2(x(t), laneBot), white, 2.f);
            for (double t : s.myReleases)
                if (t >= left && t <= right)
                    dl->AddLine(ImVec2(x(t), (laneMid + laneBot) * 0.5f), ImVec2(x(t), laneBot), white, 1.f);

            // The fixed centre line.
            float const cx = p0.x + w * 0.5f;
            dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), u32(pal.ink), 2.f);
            dl->PopClipRect();
            dl->AddRect(p0, p1, u32(pal.line), pal.radius);

            // Lane names, outside the clip so the scroll never covers them.
            UseFont f(Font::Small);
            dl->AddText(ImVec2(p0.x + 6.f, laneTop + 2.f), u32(pal.muted, 0.8f), "macro");
            dl->AddText(ImVec2(p0.x + 6.f, laneMid + 2.f), u32(pal.muted, 0.8f), "you");
        }

        // ------------------------------------------------------------ cards

        void clickTrainerCard(Kind k) {
            auto* gb = GucciEngine::get();
            auto s = tr::state(k);
            bool const following = tr::followsLevel(k);

            kit::BeginCard("Click Trainer",
                           "The macro's clicks scroll past a fixed centre line at real speed. Tap space, up or W "
                           "along with it, or play the level: every press and release you make shows up as a white "
                           "line under the macro's.");
            if (kit::SwitchRow("Click bar", "Off stops its clock and stops listening for your keys", &s.barEnabled))
                tr::saveSettings(k);
            if (!s.barEnabled) {
                kit::EndCard();
                return;
            }
            if (s.macro.clickIntervalsSec.empty()) {
                kit::Note("No clicks to show: the macro isn't loaded.", Tone::Warn);
                kit::EndCard();
                return;
            }

            drawBar(k);
            kit::Gap(0.3f);

            {
                UseFont f(Font::Small);
                kit::Label(fmt::format("{} / {}{}", timeText(s.posSec), timeText(tr::duration(k)),
                                       following ? "   following the level" : s.paused ? "   paused" : "")
                               .c_str(),
                           Tone::Muted);
            }

            if (following) {
                kit::Hint("While the level is open the bar is your run: it shows the frame you're on and resets "
                          "with you.");
            } else {
                if (kit::Button(s.paused ? "Resume" : "Pause", Tone::Accent))
                    tr::setPaused(k, !s.paused);
                ImGui::SameLine();
                if (kit::Button("Restart"))
                    tr::restart(k);
                ImGui::SameLine();
                if (kit::Button("Clear my clicks"))
                    tr::clearComparison(k);
            }

            if (kit::SwitchRow("Loop", "Start again at the end, with a fresh comparison each lap. With segment "
                                       "looping on, the bar loops the looped segments.",
                               &s.barLoop))
                tr::saveSettings(k);
            if (kit::SliderRow("Window", "How much of the macro the bar shows at once", &s.barWindowSec, 0.5f, 10.f,
                               "%.1f s"))
                tr::saveSettings(k);

            kit::Section("Your timing");
            auto const& sc = s.score;
            if (sc.hasLastReading) {
                int const d = sc.lastDeltaFrames;
                std::string const txt = d == 0 ? std::string("Last click: on the frame")
                                               : fmt::format("Last click: {} frame{} {}", std::abs(d),
                                                             std::abs(d) == 1 ? "" : "s", d < 0 ? "early" : "late");
                double const tps = following ? (gb->updater.m_tps > 0.0 ? gb->updater.m_tps : 240.0)
                                             : (s.macro.clickBarTps > 0.0 ? s.macro.clickBarTps : 240.0);
                double const ms = std::fabs(d) * 1000.0 / tps;
                Tone const tone = ms <= gb->clickIndicatorPerfectMs ? Tone::Good
                                  : ms <= gb->clickIndicatorOkMs    ? Tone::Warn
                                                                    : Tone::Bad;
                {
                    UseFont f(Font::Strong);
                    kit::Label(txt.c_str(), tone);
                }
            } else {
                kit::Label("Last click: none yet", Tone::Muted);
            }
            kit::Chip(fmt::format("Perfect {}", sc.perfect).c_str(), Tone::Good);
            ImGui::SameLine();
            kit::Chip(fmt::format("OK {}", sc.ok).c_str(), Tone::Warn);
            ImGui::SameLine();
            kit::Chip(fmt::format("Miss {}", sc.miss).c_str(), Tone::Bad);
            kit::Hint("Each press and release is matched to the nearest macro click nobody has answered yet. The "
                      "tally starts over with each attempt.");
            bool windows = false;
            windows |= kit::SliderRow("Perfect within", "Shared by both trainers", &gb->clickIndicatorPerfectMs, 1.f,
                                      100.f, "%.0f ms");
            windows |= kit::SliderRow("OK within", nullptr, &gb->clickIndicatorOkMs, 1.f, 200.f, "%.0f ms");
            windows |= kit::SwitchRow("Bars in the level",
                                      "The macro's next presses drawn in the level, from where it pressed to where "
                                      "it let go (Click Indicators, after Bogdaner09)",
                                      &gb->clickBarsInLevel);
            if (gb->clickBarsInLevel)
                windows |= kit::SliderRow("Show ahead", nullptr, &gb->clickBarsLookahead, 0.5f, 6.f, "%.1f s");
            if (windows) {
                gb->clickIndicatorOkMs = std::max(gb->clickIndicatorOkMs, gb->clickIndicatorPerfectMs);
                tr::saveScoreWindows();
            }
            kit::EndCard();
        }

        void ghostsCard(Kind k) {
            auto* gb = GucciEngine::get();
            auto s = tr::state(k);
            kit::BeginCard("Ghosts", "Drawn in the level while you play it.");
            if (kit::SwitchRow("Macro ghost", "Where the macro's player is on your frame", &s.ghost))
                tr::saveSettings(k);
            if (kit::SwitchRow("Best-attempt ghost", "Your furthest run this session, replayed alongside you",
                               &s.bestGhost))
                tr::saveSettings(k);
            if (kit::FeatureBegin("Scrub", "Freeze both ghosts at one point of the level to study it",
                                  &s.scrubActive)) {
                kit::SliderRow("Point", nullptr, &s.scrubPercent, 0.f, 100.f, "%.1f%%");
                kit::FeatureEnd();
            }

            size_t const pathLen = s.macro.pathSamples.size();
            if (pathLen > 0) {
                kit::Hint(fmt::format("The macro ghost has {} recorded positions.", pathLen).c_str());
            } else if (k == Kind::Jupiter) {
                kit::Note("The built-in JMF macro is inputs only, so its ghost needs a path from a run. Load a JMF "
                          "macro on the Macro page, play it with the bot to the end once, then use its path here.",
                          Tone::Muted);
            } else {
                kit::Note("This macro has no recorded path yet. Play it with the bot to the end once, then press "
                          "Reload at the top.",
                          Tone::Muted);
            }
            if (k == Kind::Jupiter) {
                auto const& loaded = gb->replay.m_pathSamples;
                kit::RowBegin("Path from the loaded macro",
                              loaded.empty() ? "The bot has no macro path loaded"
                                             : fmt::format("'{}', {} positions", gb->replayName, loaded.size()).c_str());
                ImGui::BeginDisabled(loaded.empty());
                if (kit::Button("Use it"))
                    tr::useLoadedPathForJupiter();
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(pathLen == 0);
                if (kit::Button("Clear"))
                    tr::clearJupiterPath();
                ImGui::EndDisabled();
                kit::RowEnd();
            }
            kit::EndCard();
        }

        void segmentEditor(Kind k, PageState& ps, std::vector<tr::Segment>& list) {
            auto s = tr::state(k);
            if (ps.selected < 0 || ps.selected >= (int)list.size())
                return;
            auto& seg = list[(size_t)ps.selected];
            bool changed = false;
            kit::Gap(0.3f);
            kit::RowBegin("Name", nullptr);
            changed |= kit::InputText("name", &seg.name, "a name for this part");
            kit::RowEnd();

            kit::RowBegin("Starts at", "Seconds into the macro");
            if (kit::Button("At the bar##start")) {
                seg.startSec = s.posSec;
                changed = true;
            }
            ImGui::SameLine();
            changed |= kit::InputDouble("start", &seg.startSec, 0.05, "%.2f s");
            kit::RowEnd();

            kit::RowBegin("Ends at", nullptr);
            if (kit::Button("At the bar##end")) {
                seg.endSec = s.posSec;
                changed = true;
            }
            ImGui::SameLine();
            changed |= kit::InputDouble("end", &seg.endSec, 0.05, "%.2f s");
            kit::RowEnd();

            kit::RowBegin("Note", nullptr);
            changed |= multiline("##segnote", &seg.note, ImGui::GetTextLineHeight() * 3.5f);
            kit::RowEnd();

            if (kit::Button("Jump the bar here"))
                tr::seek(k, seg.startSec);
            ImGui::SameLine();
            if (kit::Button("Delete", Tone::Bad)) {
                tr::removeSegment(k, ps.selected);
                ps.selected = -1;
                return;
            }

            if (changed) {
                seg.startSec = std::max(0.0, seg.startSec);
                if (seg.endSec <= seg.startSec) {
                    ps.segMsg = {"A segment has to end after it starts; it isn't saved until it does.", Tone::Warn};
                    return;
                }
                ps.segMsg = {};
                tr::saveSegments(k);
            }
        }

        void segmentsCard(Kind k) {
            auto s = tr::state(k);
            auto& ps = page(k);
            auto& list = tr::segments(k);
            bool const following = tr::followsLevel(k);

            kit::BeginCard("Segments", "Mark and name the hard parts, keep notes on them, and loop them.");

            if (list.empty())
                kit::Hint("No segments yet. Add one at the bar, or ask for suggestions below.");
            for (size_t i = 0; i < list.size(); ++i) {
                auto const& seg = list[i];
                ImGui::PushID((int)i);
                std::string const line =
                    fmt::format("{}   {} - {}", seg.name.empty() ? "(unnamed)" : seg.name, timeText(seg.startSec),
                                timeText(seg.endSec));
                if (ImGui::Selectable(line.c_str(), ps.selected == (int)i))
                    ps.selected = ps.selected == (int)i ? -1 : (int)i;
                if (!seg.note.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", seg.note.c_str());
                ImGui::PopID();
            }
            kit::Gap(0.3f);
            if (kit::Button("Add at the bar", Tone::Accent)) {
                double const at = s.posSec;
                list.push_back({fmt::format("Part {}", list.size() + 1), at, at + 2.0, ""});
                tr::sortSegments(k);
                for (size_t i = 0; i < list.size(); ++i)
                    if (list[i].startSec == at)
                        ps.selected = (int)i;
            }
            segmentEditor(k, ps, list);
            message(ps.segMsg);

            // ---- looping
            kit::Section("Loop");
            if (list.empty()) {
                kit::Hint("Add a segment to loop it.");
            } else {
                std::vector<std::string> names;
                for (size_t i = 0; i < list.size(); ++i)
                    names.push_back(fmt::format("{}. {}", i + 1, list[i].name.empty() ? "(unnamed)" : list[i].name));
                bool changed = false;
                if (kit::SwitchRow("Loop segments",
                                   "In practice mode, play into the first one: a checkpoint goes down at its start, "
                                   "and reaching the end of the last one sends you back there",
                                   &s.loopEnabled))
                    changed = true;
                if (s.loopStartIdx < 0 || s.loopStartIdx >= (int)list.size())
                    s.loopStartIdx = std::max(0, ps.selected);
                if (s.loopEndIdx < s.loopStartIdx || s.loopEndIdx >= (int)list.size())
                    s.loopEndIdx = s.loopStartIdx;
                kit::RowBegin("From", nullptr);
                changed |= kit::Dropdown("from", &s.loopStartIdx, names);
                kit::RowEnd();
                kit::RowBegin("To", nullptr);
                changed |= kit::Dropdown("to", &s.loopEndIdx, names);
                kit::RowEnd();
                if (changed && s.loopEndIdx < s.loopStartIdx)
                    s.loopEndIdx = s.loopStartIdx;

                if (s.loopEnabled) {
                    auto* pl = PlayLayer::get();
                    if (following && pl && !pl->m_isPracticeMode) {
                        kit::Note("Looping uses a practice checkpoint, so it needs practice mode.", Tone::Warn);
                        if (kit::Button("Turn on practice mode"))
                            geode::queueInMainThread([] {
                                if (auto* p = PlayLayer::get(); p && !p->m_isPracticeMode)
                                    p->togglePracticeMode(true);
                            });
                    } else if (following) {
                        kit::Chip(tr::loopArmed(k) ? "Checkpoint down" : "Waiting for you to reach the start",
                                  tr::loopArmed(k) ? Tone::Good : Tone::Muted);
                        ImGui::SameLine();
                        kit::Chip(fmt::format("Clean laps {}", tr::cleanLaps(k)).c_str(), Tone::Accent);
                    } else {
                        kit::Hint("It runs when you play the level.");
                    }
                }
            }

            // ---- suggestions
            kit::Section("Suggestions");
            if (kit::Button("Suggest from click density")) {
                ps.suggestions = tr::suggestSegments(k);
                ps.suggested = true;
            }
            if (ps.suggested && ps.suggestions.empty())
                kit::Hint("Nothing stands out: no stretch has noticeably more clicks than the rest, or your "
                          "segments already cover it.");
            for (size_t i = 0; i < ps.suggestions.size(); ++i) {
                auto const& sg = ps.suggestions[i];
                ImGui::PushID((int)i + 1000);
                kit::RowBegin(fmt::format("{} - {}", timeText(sg.startSec), timeText(sg.endSec)).c_str(),
                              sg.note.c_str());
                bool const add = kit::Button("Add");
                kit::RowEnd();
                ImGui::PopID();
                if (add) {
                    list.push_back(sg);
                    tr::sortSegments(k);
                    ps.selected = -1;
                    ps.suggestions.erase(ps.suggestions.begin() + (ptrdiff_t)i);
                    break;
                }
            }

            // ---- sharing
            kit::Section("Share");
            if (kit::Button("Copy code")) {
                ImGui::SetClipboardText(tr::exportCode(k).c_str());
                ps.segMsg = {list.empty() ? "Copied, but there are no segments in it."
                                          : fmt::format("Copied a code for {} segment{}.", list.size(),
                                                        list.size() == 1 ? "" : "s"),
                             Tone::Good};
            }
            ImGui::SameLine();
            if (kit::Button("Paste")) {
                if (const char* clip = ImGui::GetClipboardText())
                    ps.code = clip;
            }
            kit::InputText("code", &ps.code, "paste a GBSEG1: code here");
            if (kit::Button("Import (adds to the list)")) {
                std::string err;
                size_t const before = list.size();
                if (tr::importCode(k, ps.code, err)) {
                    size_t const added = tr::segments(k).size() - before;
                    ps.segMsg = {fmt::format("Added {} segment{}. {}", added, added == 1 ? "" : "s", err),
                                 err.empty() ? Tone::Good : Tone::Warn};
                    ps.code.clear();
                    ps.selected = -1;
                } else {
                    ps.segMsg = {err, Tone::Bad};
                }
            }

            // ---- notes
            kit::Section("Notes");
            if (multiline("##notes", &s.notes, ImGui::GetTextLineHeight() * 5.f))
                tr::saveNotes(k);
            kit::EndCard();
        }

        // Deaths across the level, in bins of 2%. Hover a bar for its count.
        void drawHeatmap(Kind k) {
            auto s = tr::state(k);
            auto const& pal = look().pal;
            std::array<int, kHeatBins> bins{};
            int most = 0;
            for (float pct : s.deathPcts) {
                int const b = std::clamp((int)(pct / 100.f * kHeatBins), 0, kHeatBins - 1);
                most = std::max(most, ++bins[(size_t)b]);
            }
            float const w = kit::Avail();
            ImVec2 const p0 = ImGui::GetCursorScreenPos();
            ImVec2 const p1(p0.x + w, p0.y + kHeatH);
            ImGui::InvisibleButton("##heat", ImVec2(w, kHeatH));
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1, u32(pal.raised, 0.55f), pal.radius);
            float const bw = w / kHeatBins;
            for (int b = 0; b < kHeatBins; ++b) {
                int const n = bins[(size_t)b];
                if (n == 0)
                    continue;
                float const t = (float)n / (float)std::max(1, most);
                float const top = p1.y - 2.f - (kHeatH - 6.f) * t;
                dl->AddRectFilled(ImVec2(p0.x + b * bw + 1.f, top), ImVec2(p0.x + (b + 1) * bw - 1.f, p1.y - 2.f),
                                  u32(mix(pal.warn, pal.bad, t), 0.9f), 2.f);
            }
            if (s.sessionBestPct > 0.f) {
                float const bx = p0.x + w * std::clamp(s.sessionBestPct / 100.f, 0.f, 1.f);
                dl->AddLine(ImVec2(bx, p0.y), ImVec2(bx, p1.y), u32(pal.good), 2.f);
            }
            dl->AddRect(p0, p1, u32(pal.line), pal.radius);
            if (ImGui::IsItemHovered()) {
                int const b = std::clamp((int)((ImGui::GetIO().MousePos.x - p0.x) / bw), 0, kHeatBins - 1);
                int const from = b * 100 / kHeatBins;
                ImGui::SetTooltip("%d-%d%%: %d death%s", from, from + 100 / kHeatBins, bins[(size_t)b],
                                  bins[(size_t)b] == 1 ? "" : "s");
            }
            // The scale under it; the green line is the session best.
            UseFont f(Font::Small);
            float const lineH = ImGui::GetTextLineHeight();
            ImVec2 const q(p0.x, ImGui::GetCursorScreenPos().y);
            dl->AddText(q, u32(pal.muted), "0%");
            float const mid = ImGui::CalcTextSize("50%").x;
            dl->AddText(ImVec2(q.x + (w - mid) * 0.5f, q.y), u32(pal.muted), "50%");
            float const full = ImGui::CalcTextSize("100%").x;
            dl->AddText(ImVec2(q.x + w - full, q.y), u32(pal.muted), "100%");
            ImGui::Dummy(ImVec2(w, lineH));
        }

        void statsCard(Kind k) {
            auto s = tr::state(k);
            kit::BeginCard("Stats", "This session, counted while you play the level yourself.");
            kit::Chip(fmt::format("Attempts {}", s.attempts).c_str(), Tone::Accent);
            ImGui::SameLine();
            kit::Chip(fmt::format("Best {:.1f}%", s.sessionBestPct).c_str(), Tone::Good);
            ImGui::SameLine();
            kit::Chip(fmt::format("Deaths {}", s.deathPcts.size()).c_str(), Tone::Bad);
            kit::Gap(0.3f);
            drawHeatmap(k);
            if (kit::Button("Reset stats"))
                tr::resetStats(k);
            kit::EndCard();
        }

        void musicCard(Kind k) {
            auto* gb = GucciEngine::get();
            auto s = tr::state(k);
            kit::BeginCard("Music",
                           k == Kind::Jupiter
                               ? "The level's own track, kept on your frame in the level and on the bar's clock "
                                 "here. GD's audio is muted while it plays so the two don't clash."
                               : "Your own track for this trainer, kept on your frame in the level and on the bar's "
                                 "clock here. GD's audio is muted while it plays.");
            if (kit::SwitchRow("Synced music", nullptr, &s.musicEnabled))
                tr::saveSettings(k);
            if (kit::SliderRow("Sync offset", "Positive puts the song ahead of the bar and the level",
                               &s.musicOffsetSec, -2.f, 2.f, "%.2f s"))
                tr::saveSettings(k);
            if (k == Kind::Any) {
                takePicked();
                kit::RowBegin("Track", gb->trainerMusicImported ? "Imported" : "None yet: import an .mp3");
                ImGui::BeginDisabled(s_pickPending);
                if (kit::Button(s_pickPending ? "Choosing..." : "Import"))
                    pickTrack();
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!gb->trainerMusicImported);
                if (kit::Button("Remove")) {
                    tr::removeTrack();
                    g_trackMsg = {};
                }
                ImGui::EndDisabled();
                kit::RowEnd();
                message(g_trackMsg);
            }
            kit::EndCard();
        }

        // ------------------------------------------------------------ headers

        void jupiterHeader() {
            auto s = tr::state(Kind::Jupiter);
            kit::BeginCard("Jupiter My Favourite",
                           "Nigel's trainer for one level, its macro built in. Open the level in GD to train on it; "
                           "away from it, the bar runs on its own.");
            if (!s.macro.loaded) {
                kit::Note("The built-in JMF macro didn't load. guccibot_jupitermacro.log in the mod's save folder "
                          "says what was found.",
                          Tone::Bad);
            } else {
                kit::Chip(fmt::format("{} clicks", s.macro.clickIntervalsSec.size()).c_str(), Tone::Accent);
                ImGui::SameLine();
                kit::Chip(fmt::format("{:.0f} TPS", s.macro.clickBarTps).c_str(), Tone::Muted);
                ImGui::SameLine();
                bool const in = tr::followsLevel(Kind::Jupiter);
                kit::Chip(in ? "In the level" : "Not in the level", in ? Tone::Good : Tone::Muted);
            }
            kit::EndCard();
        }

        void anyHeader() {
            auto* gb = GucciEngine::get();
            auto s = tr::state(Kind::Any);
            kit::BeginCard("Trainer", "Point it at any of your saved macros.");

            auto const& names = gb->storedMacros;
            if (g_pick < 0 || g_pick >= (int)names.size()) {
                auto it = std::find(names.begin(), names.end(), gb->trainerMacroName);
                g_pick = it == names.end() ? -1 : (int)(it - names.begin());
            }
            kit::RowBegin("Macro", names.empty() ? "No saved macros yet" : nullptr);
            if (kit::Button("Load", Tone::Accent) && g_pick >= 0 && g_pick < (int)names.size()) {
                std::string const stem = names[(size_t)g_pick];
                g_macroMsg = tr::selectMacro(stem)
                                 ? Message{}
                                 : Message{fmt::format("'{}' didn't load: it has no click pairs, or it isn't a "
                                                       "format the Trainer reads (GucciBot macros are).",
                                                       stem),
                                           Tone::Bad};
            }
            ImGui::SameLine();
            if (kit::Button("Refresh"))
                gb->reloadMacroList();
            ImGui::SameLine();
            kit::Dropdown("macro", &g_pick, names);
            kit::RowEnd();

            if (s.macro.loaded) {
                kit::RowBegin("Loaded", nullptr);
                kit::Label(gb->trainerMacroName.c_str());
                ImGui::SameLine();
                if (kit::Button("Reload")) {
                    if (!tr::selectMacro(gb->trainerMacroName))
                        g_macroMsg = {"It didn't load again; the file may have moved.", Tone::Bad};
                }
                kit::RowEnd();
                kit::Chip(fmt::format("{} clicks", s.macro.clickIntervalsSec.size()).c_str(), Tone::Accent);
                ImGui::SameLine();
                kit::Chip(fmt::format("{:.0f} TPS", s.macro.clickBarTps).c_str(), Tone::Muted);
                ImGui::SameLine();
                bool const in = tr::followsLevel(Kind::Any);
                kit::Chip(in ? "In the level" : "Not in the level", in ? Tone::Good : Tone::Muted);
                if (s.macro.levelName.empty())
                    kit::Note("This macro doesn't carry the name of the level it was recorded on, so stats, ghosts "
                              "and music run on whatever level you play.",
                              Tone::Warn);
                else
                    kit::Hint(fmt::format("Stats, ghosts and music run in '{}'.", s.macro.levelName).c_str());
            } else {
                kit::Hint("Nothing loaded. Pick a macro and press Load.");
            }
            message(g_macroMsg);
            kit::EndCard();
        }

        void body(Kind k) {
            clickTrainerCard(k);
            // Video Mode rides this trainer's click bar clock, so it sits right under it.
            if (k == Kind::Jupiter)
                videoModeCard();
            ghostsCard(k);
            segmentsCard(k);
            statsCard(k);
            musicCard(k);
            // The Survival Indicator, its click cue and the accuracy readout
            // (Nigel's layout, 2026-10-18: no Indicators tab of their own).
            indicators();
        }

    } // namespace

    namespace {
        // The JMF Trainer looks like Jupiter My Favourite (Nigel: "jmf tab isnt
        // jmfing"), as the site's trainer section does: deep indigo cards, the
        // level's yellow, lavender text, a sky of pentagrams and sparkles, and
        // the half-sun wheel at the bottom edge.
        ImVec4 const kJmfYellow{0.988f, 0.961f, 0.314f, 1.f};  // #FCF550
        ImVec4 const kJmfSky{0.027f, 0.016f, 0.247f, 1.f};     // #07043F
        ImVec4 const kJmfCard{0.051f, 0.027f, 0.408f, 1.f};    // #0D0768
        ImVec4 const kJmfRaised{0.082f, 0.043f, 0.510f, 1.f};  // #150B82
        ImVec4 const kJmfLavender{0.714f, 0.690f, 0.910f, 1.f}; // #B6B0E8

        ImU32 jmfYellow(float alpha) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(kJmfYellow.x, kJmfYellow.y, kJmfYellow.z, alpha));
        }

        // A true pentagram: five straight lines through every other point.
        void pentagram(ImDrawList* dl, ImVec2 c, float r, float turnDeg, ImU32 col, float width) {
            ImVec2 pts[5];
            for (int k = 0; k < 5; k++) {
                float const a = (turnDeg - 90.f + 72.f * (float)k) * 0.0174533f;
                pts[k] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a));
            }
            ImVec2 const star[5] = {pts[0], pts[2], pts[4], pts[1], pts[3]};
            dl->AddPolyline(star, 5, col, ImDrawFlags_Closed, width);
        }

        void sparkle(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
            float const w = r * 0.22f;
            dl->AddQuadFilled({c.x, c.y - r}, {c.x + w, c.y}, {c.x, c.y + r}, {c.x - w, c.y}, col);
            dl->AddQuadFilled({c.x - r, c.y}, {c.x, c.y - w}, {c.x + r, c.y}, {c.x, c.y + w}, col);
        }

        Palette g_jmfSaved;
        int g_jmfPushed = 0;
    }

    // The JMF colours over the whole menu while the JMF Trainer is the page
    // (Nigel: the colours should take the whole menu, not just the page): the
    // kit reads look().pal, ImGui's own widgets and the window the style
    // colours. The shell pushes before the window begins and pops after it
    // ends; Video Mode's bar uses them too.
    void jupiterLookPush() {
        if (g_jmfPushed > 0)
            return;
        g_jmfSaved = look().pal;
        {
            {
                Palette& p = look().pal;
                p.accent = kJmfYellow;
                p.onAccent = ImVec4(0.06f, 0.03f, 0.25f, 1.f);
                p.surface = kJmfCard;
                p.raised = kJmfRaised;
                p.ink = ImVec4(0.957f, 0.949f, 1.f, 1.f);
                p.muted = kJmfLavender;
                p.line = ImVec4(kJmfYellow.x, kJmfYellow.y, kJmfYellow.z, 0.22f);
                auto push = [&](ImGuiCol c, ImVec4 v) {
                    ImGui::PushStyleColor(c, v);
                    ++g_jmfPushed;
                };
                auto alpha = [](ImVec4 v, float a) { return ImVec4(v.x, v.y, v.z, a); };
                push(ImGuiCol_Text, p.ink);
                push(ImGuiCol_TextDisabled, p.muted);
                push(ImGuiCol_Border, p.line);
                push(ImGuiCol_FrameBg, kJmfRaised);
                push(ImGuiCol_FrameBgHovered, ImVec4(0.11f, 0.06f, 0.60f, 1.f));
                push(ImGuiCol_FrameBgActive, ImVec4(0.13f, 0.08f, 0.66f, 1.f));
                push(ImGuiCol_Button, kJmfRaised);
                push(ImGuiCol_ButtonHovered, ImVec4(0.11f, 0.06f, 0.60f, 1.f));
                push(ImGuiCol_ButtonActive, alpha(kJmfYellow, 0.55f));
                push(ImGuiCol_Header, alpha(kJmfYellow, 0.22f));
                push(ImGuiCol_HeaderHovered, alpha(kJmfYellow, 0.30f));
                push(ImGuiCol_HeaderActive, alpha(kJmfYellow, 0.40f));
                push(ImGuiCol_CheckMark, kJmfYellow);
                push(ImGuiCol_SliderGrab, kJmfYellow);
                push(ImGuiCol_SliderGrabActive, kJmfYellow);
                push(ImGuiCol_ScrollbarGrab, alpha(kJmfLavender, 0.35f));
                push(ImGuiCol_PlotHistogram, kJmfYellow);
                push(ImGuiCol_WindowBg, alpha(kJmfSky, g_jmfSaved.opacity));
                push(ImGuiCol_PopupBg, kJmfCard);
                push(ImGuiCol_Tab, kJmfRaised);
                push(ImGuiCol_TabHovered, alpha(kJmfYellow, 0.35f));
                push(ImGuiCol_TabActive, alpha(kJmfYellow, 0.45f));
                p.backdrop = alpha(kJmfSky, g_jmfSaved.opacity);
            }
        }
    }

    void jupiterLookPop() {
        if (g_jmfPushed == 0)
            return;
        ImGui::PopStyleColor(g_jmfPushed);
        g_jmfPushed = 0;
        look().pal = g_jmfSaved;
    }

    void jupiterBackdrop() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 const a = ImGui::GetWindowPos();
        ImVec2 const size = ImGui::GetWindowSize();
        ImVec2 const b(a.x + size.x, a.y + size.y);
        dl->PushClipRect(a, b, true);
        dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(kJmfSky), look().pal.radius);
        // Scattered over the whole page, as the level's skies scatter them
        // (positions and turns from the site's 400 x 500 sky).
        struct Mark {
            float x, y, scale, turn, alpha;
        };
        static constexpr Mark kPentagrams[] = {
            {70, 70, 1.5f, -8, 0.10f},  {330, 60, 1.9f, 14, 0.08f}, {250, 150, 1.1f, 30, 0.12f},
            {40, 220, 1.3f, -20, 0.09f}, {360, 250, 1.6f, 6, 0.10f}, {150, 320, 1.0f, -12, 0.11f},
            {300, 380, 1.4f, 22, 0.08f}, {60, 420, 0.9f, 10, 0.12f}, {200, 60, 0.8f, -16, 0.10f},
        };
        static constexpr Mark kSparkles[] = {
            {120, 110, 1.3f, 0, 0.35f}, {290, 220, 1.0f, 0, 0.30f}, {200, 300, 1.6f, 0, 0.25f}, {380, 400, 1.1f, 0, 0.30f},
        };
        auto at = [&](Mark const& m) { return ImVec2(a.x + m.x / 400.f * size.x, a.y + m.y / 500.f * size.y); };
        // With animations on, the sky turns slowly and the sparkles breathe.
        float const t = look().effects ? (float)ImGui::GetTime() : 0.f;
        for (auto const& m : kPentagrams)
            pentagram(dl, at(m), 16.f * m.scale, m.turn + t * 4.f, jmfYellow(m.alpha), 1.5f);
        for (size_t i = 0; i < std::size(kSparkles); i++) {
            auto const& m = kSparkles[i];
            float const breathe = 0.75f + 0.25f * std::sin(t * 1.7f + (float)i * 1.3f);
            sparkle(dl, at(m), 7.f * m.scale * breathe, jmfYellow(m.alpha));
        }
        // The half-sun wheel: its centre sits on the bottom edge, so only the
        // top half shows. Two rings, a dashed one between them, ticks round
        // the outside and a star in the middle.
        ImVec2 const c(a.x + size.x * 0.80f, b.y + 6.f);
        float const r = std::max(40.f, std::min(size.x, size.y) * 0.22f);
        dl->AddCircle(c, r, jmfYellow(0.20f), 72, 2.f);
        dl->AddCircle(c, r * 0.70f, jmfYellow(0.15f), 72, 1.5f);
        for (int k = 0; k < 48; k += 2) {
            float const a0 = (float)k * 6.2831853f / 48.f + t * 0.05f;
            dl->PathArcTo(c, r * 0.85f, a0, a0 + 6.2831853f / 48.f, 4);
            dl->PathStroke(jmfYellow(0.16f), 0, 1.5f);
        }
        for (int k = 0; k < 24; k++) {
            float const ang = (float)k * 6.2831853f / 24.f;
            ImVec2 const d(std::cos(ang), std::sin(ang));
            dl->AddLine({c.x + d.x * r * 1.03f, c.y + d.y * r * 1.03f}, {c.x + d.x * r * 1.12f, c.y + d.y * r * 1.12f},
                        jmfYellow(0.20f), 1.5f);
        }
        pentagram(dl, c, r * 0.32f, t * 6.f, jmfYellow(0.28f), 2.f);
        dl->PopClipRect();
    }

    void jupiterTrainer() {
        tr::ensureLoaded();
        tr::markPageDrawn(Kind::Jupiter);
        jupiterHeader();
        body(Kind::Jupiter);
    }

    // Video Mode: in place of the menu, the JMF click bar over the bottom of
    // the video (Nigel: "i still see the whole menu in the middle instead of an
    // overlay of the bar thing"), with its controls and the way out. The page
    // counts as open, so the bar's clock and its music carry on.
    void jupiterVideoOverlay() {
        tr::ensureLoaded();
        tr::markPageDrawn(Kind::Jupiter);
        auto* gb = GucciEngine::get();
        auto s = tr::state(Kind::Jupiter);
        ImGuiIO const& io = ImGui::GetIO();
        float const w = std::min(io.DisplaySize.x - 40.f, 960.f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 20.f), ImGuiCond_Always,
                                ImVec2(0.5f, 1.f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.6f);
        jupiterLookPush();
        if (ImGui::Begin("##jmfvideobar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            if (s.macro.clickIntervalsSec.empty())
                kit::Note("No clicks to show: the JMF macro isn't loaded.", Tone::Warn);
            else
                drawBar(Kind::Jupiter);
            if (kit::Button(s.paused ? "Resume" : "Pause", Tone::Accent))
                tr::setPaused(Kind::Jupiter, !s.paused);
            ImGui::SameLine();
            if (kit::Button("Restart"))
                tr::restart(Kind::Jupiter);
            ImGui::SameLine();
            if (kit::Button("Exit Video Mode", Tone::Muted)) {
                gb->jupiterVideoModeEnabled = false;
                gb->jupiterVideoAlignToolActive = false;
            }
        }
        ImGui::End();
        jupiterLookPop();
    }

    void anyTrainer() {
        tr::ensureLoaded();
        tr::markPageDrawn(Kind::Any);
        anyHeader();
        body(Kind::Any);
    }

} // namespace gucci::ui::pages
