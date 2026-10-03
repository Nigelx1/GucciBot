// The Frame Editor page: the loaded macro's inputs on a timeline. Every click
// is a bar from its press to its release, in a lane per button of each player
// (player 2 has lanes of its own). Drag a bar to move it, drag either end to
// stretch or shorten it, double-click an empty spot to add one; the inspector
// takes exact frame numbers. Undo and redo, with Ctrl+Z / Ctrl+Y while the
// page has focus. The overview strip on top is the whole macro, with the part
// on screen boxed; click or drag in it to jump.
//
// The rules (where a click may go, and the check that no edit breaks the
// macro) are src/tools/edit_core, ported from Absense's macro editor (by
// Absent). This file is the timeline, written fresh, and the inspector, after
// Absense's (pages/editor_inspector.cpp): exact fields for the selected
// click, a move for a selection, an add form when nothing is selected, and
// the reason whenever an edit is stopped short or refused.
//
// Edits change the macro in memory (GucciReplaySystem::m_actionAtom); the
// Macro page's Save writes it. Nothing is edited while a macro records or
// plays in a level, or while Calculate or the Pathfinder runs: each of those
// walks the list with its own index, and an edit under it would shift what
// that index points at.

#include "ui/kit.hpp"
#include "ui/look.hpp"

#include "absense/glue.hpp"
#include "analysis/pathfinder.hpp"
#include "core/GucciBot.hpp"
#include "tools/edit_core.hpp"
#include "tools/macro_check.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gucci::ui::pages {

    namespace {

        namespace ed = gucci::edit;
        using ed::Actions;

        // Layout, in pixels.
        constexpr float kLabelW = 72.f;    // lane names, left of the track
        constexpr float kOverviewH = 30.f;
        constexpr float kRulerH = 18.f;
        constexpr float kLaneH = 26.f;
        constexpr float kEdgeGrab = 5.f;   // how close to a bar's end counts as grabbing that end
        constexpr float kMinEdgeBar = 14.f;  // narrower bars are moved whole; zoom in for their ends
        // Zoom limits: 40 pixels a frame at the closest.
        constexpr double kMinFramesPerPx = 1.0 / 40.0;

        enum class Drag { None, Move, PressEdge, ReleaseEdge, Box, Overview };

        struct Message {
            std::string text;
            Tone tone = Tone::Muted;
        };

        struct State {
            // The macro as the page last saw it. A different fingerprint that
            // the page did not make is a load, a recording or the pathfinder:
            // undo starts over and the selection goes.
            bool seenAny = false;
            uint64_t seen = 0;
            std::string seenName;
            ed::Model model;
            macrocheck::Report report;
            std::vector<uint32_t> problemKeys;  // sorted keys of items Check Macro flags
            ed::History history;

            std::vector<uint32_t> sel;  // sorted item keys

            // The view: the frame at the track's left edge and the zoom.
            // fpp 0 fits the whole macro on the next draw.
            double start = 0.0;
            double fpp = 0.0;
            bool allLanes = false;
            float trackW = 600.f;  // the track's width last drawn, for the zoom buttons

            Drag drag = Drag::None;
            uint32_t dragKey = ed::kNone;
            double dragFrom = 0.0;      // frame under the mouse when the drag began
            int64_t dragOrigin = 0;     // an edge drag: the edge's frame then
            int64_t dragValue = 0;      // a move: the delta; an edge: the frame
            int64_t dragWanted = 0;     // what the mouse asked for, before the range
            ed::Range range;
            ImVec2 boxFrom{};
            std::vector<uint32_t> boxBase;  // selection a Ctrl box adds to

            // The inspector's fields, filled from the macro when the
            // selection or the macro changes.
            uint32_t fieldsKey = ed::kNone;
            uint64_t fieldsFor = 0;
            int fieldPress = 0, fieldRelease = 0;
            int moveBy = 10;

            int addLane = 0, addAt = 1, addHold = 1;

            // The multi-selection's move range, worked out again only when
            // the macro or the selection changes (it walks every lane).
            uint64_t rangeFor = 0, rangeSel = 0;
            ed::Range selRange;

            Message msg;
        };

        State& st() {
            static State s;
            return s;
        }

        Actions& macro() {
            return GucciEngine::get()->replay.m_actionAtom.m_actions;
        }

        // Why the macro can't be edited right now, or nullptr.
        const char* lockReason() {
            auto* gb = GucciEngine::get();
            if (gb->isRecording())
                return "A macro is recording. Switch Mode to Off on the Macro page to edit it.";
            if (gb->fwAnalyzing)
                return "Calculate is running. Edit once it has finished.";
            if (absense::isRunning() || absense::startPending() || Pathfinder::get()->active)
                return "The Pathfinder is running. Edit once it has stopped.";
            if (gb->isPlaying() && GJBaseGameLayer::get())
                return "The macro is playing in this level. Leave the level, or switch Mode to Off on the Macro "
                       "page, to edit it.";
            return nullptr;
        }

        // ------------------------------------------------------------ the model

        void rebuild(State& s, Actions const& a) {
            s.model = ed::build(a);
            s.report = macrocheck::check(a);
            s.problemKeys.clear();
            if (!s.report.findings.empty()) {
                std::vector<uint32_t> itemOfAction(a.size(), ed::kNone);
                for (auto const& it : s.model.items) {
                    if (it.press != ed::kNone)
                        itemOfAction[it.press] = it.key();
                    if (it.release != ed::kNone)
                        itemOfAction[it.release] = it.key();
                }
                for (auto const& f : s.report.findings)
                    if (f.index < itemOfAction.size() && itemOfAction[f.index] != ed::kNone)
                        s.problemKeys.push_back(itemOfAction[f.index]);
                std::sort(s.problemKeys.begin(), s.problemKeys.end());
                s.problemKeys.erase(std::unique(s.problemKeys.begin(), s.problemKeys.end()), s.problemKeys.end());
            }
            // Keys that no longer name an item are dropped.
            std::erase_if(s.sel, [&](uint32_t k) { return ed::itemOfKey(s.model, k) == ed::kNone; });
        }

        void sync(State& s) {
            auto* gb = GucciEngine::get();
            Actions const& a = macro();
            uint64_t const fp = ed::fingerprint(a);
            if (s.seenAny && fp == s.seen)
                return;
            if (s.seenAny) {
                if (s.history.canUndo() || s.history.canRedo())
                    s.msg = {"The macro changed outside the editor, so undo starts over.", Tone::Muted};
                s.history.clear();
                s.sel.clear();
                s.drag = Drag::None;
            }
            // A different macro (or the first one) is fitted to the view; a
            // recording that grows the same one keeps where the user is.
            if (!s.seenAny || s.seenName != gb->replayName || s.model.items.empty())
                s.fpp = 0.0;
            s.seenAny = true;
            s.seen = fp;
            s.seenName = gb->replayName;
            rebuild(s, a);
        }

        // After every change the page makes: the playback cursor goes back to
        // the start, which is what leaving a level leaves it at, and the next
        // level start or reset (GucciReplaySystem::onReset) puts it on the
        // right action. Editing only happens with nothing playing.
        void afterChange(State& s) {
            auto* gb = GucciEngine::get();
            gb->replay.m_inputIndex = 0;
            gb->replay.buildClickIntervals(gb->updater.m_tps);
            s.seen = ed::fingerprint(macro());
            rebuild(s, macro());
            s.fieldsKey = ed::kNone;  // refill the inspector
        }

        bool canEdit(State& s) {
            if (const char* why = lockReason()) {
                s.msg = {why, Tone::Warn};
                return false;
            }
            return true;
        }

        void commit(State& s, ed::Draft d) {
            if (!d.ok) {
                s.msg = {d.error, Tone::Warn};
                return;
            }
            Actions& a = macro();
            s.history.record(a, d.result, d.label, s.sel, d.selection);
            a = std::move(d.result);
            s.sel = std::move(d.selection);
            s.msg = {d.label + ".", Tone::Good};
            afterChange(s);
        }

        void undo(State& s) {
            if (!canEdit(s))
                return;
            if (!s.history.canUndo()) {
                s.msg = {"Nothing to undo.", Tone::Muted};
                return;
            }
            std::vector<uint32_t> sel;
            std::string label;
            if (!s.history.undo(macro(), sel, label)) {
                s.msg = {"The macro changed outside the editor, so there is nothing left to undo.", Tone::Warn};
                return;
            }
            s.sel = std::move(sel);
            s.msg = {"Undid: " + label + ".", Tone::Muted};
            afterChange(s);
        }

        void redo(State& s) {
            if (!canEdit(s))
                return;
            if (!s.history.canRedo()) {
                s.msg = {"Nothing to redo.", Tone::Muted};
                return;
            }
            std::vector<uint32_t> sel;
            std::string label;
            if (!s.history.redo(macro(), sel, label)) {
                s.msg = {"The macro changed outside the editor, so there is nothing left to redo.", Tone::Warn};
                return;
            }
            s.sel = std::move(sel);
            s.msg = {"Redid: " + label + ".", Tone::Muted};
            afterChange(s);
        }

        void deleteSelection(State& s) {
            if (s.sel.empty() || !canEdit(s))
                return;
            commit(s, ed::remove(macro(), s.model, s.sel));
        }

        // A move of the selection by `delta`, stopped at the edge of what is
        // allowed; the message says where and why.
        void moveSelection(State& s, int64_t delta) {
            if (s.sel.empty() || delta == 0 || !canEdit(s))
                return;
            Actions const& a = macro();
            ed::Range const r = ed::moveRange(a, s.model, s.sel);
            int64_t const d = r.clamp(delta);
            if (d == 0) {
                s.msg = {std::string("It can't move ") + (delta < 0 ? "earlier: " : "later: ") +
                             (delta < 0 ? r.loWhy : r.hiWhy) + ".",
                         Tone::Warn};
                return;
            }
            std::string const why = d != delta ? (delta < 0 ? r.loWhy : r.hiWhy) : std::string();
            commit(s, ed::move(a, s.model, s.sel, d));
            if (!why.empty() && s.msg.tone == Tone::Good)
                s.msg.text += " Stopped there: " + why + ".";
        }

        bool selected(State const& s, uint32_t key) {
            return std::binary_search(s.sel.begin(), s.sel.end(), key);
        }

        // ------------------------------------------------------------ geometry

        struct View {
            float x0 = 0.f;  // the track's left edge on screen
            float w = 1.f;   // the track's width
            double start = 0.0, fpp = 1.0;
            float x(double frame) const { return x0 + static_cast<float>((frame - start) / fpp); }
            double frame(float sx) const { return start + static_cast<double>(sx - x0) * fpp; }
            double end() const { return start + static_cast<double>(w) * fpp; }
        };

        // The frames the view can show: the macro plus room after it.
        double extent(State const& s) {
            return std::max(600.0, static_cast<double>(s.model.lastFrame) + 120.0);
        }

        void clampView(State& s, float trackW) {
            double const total = extent(s);
            if (s.fpp <= 0.0) {
                s.fpp = total / trackW;
                s.start = 0.0;
            }
            double const maxFpp = std::max(1.0, total / trackW * 1.25);
            s.fpp = std::clamp(s.fpp, kMinFramesPerPx, maxFpp);
            double const shown = static_cast<double>(trackW) * s.fpp;
            s.start = std::clamp(s.start, 0.0, std::max(0.0, total - shown * 0.5));
        }

        void zoomAround(State& s, View const& v, float sx, double factor) {
            double const f = v.frame(sx);
            s.fpp *= factor;
            clampView(s, v.w);
            s.start = f - static_cast<double>(sx - v.x0) * s.fpp;
            clampView(s, v.w);
        }

        // Where a bar of item `it` reaches: a click to its release, a lone
        // press to the frame before the next death or restart (or the end of
        // the macro), a lone release is a point.
        double barEnd(State const& s, Actions const& a, ed::Item const& it) {
            if (it.kind == ed::Kind::Press) {
                uint32_t const e = ed::holdEnd(a, s.model, it);
                return e == ed::kLastFrame ? std::max<double>(s.model.lastFrame, ed::firstFrame(a, it)) + 30.0 : e;
            }
            return ed::lastFrame(a, it);
        }

        // Bars narrower than a pixel or two are merged into runs, so a long
        // macro zoomed out draws a few hundred rectangles, not one per click.
        struct Runs {
            ImDrawList* dl = nullptr;
            float y0 = 0.f, y1 = 0.f;
            ImU32 col = 0;
            float x0 = 0.f, x1 = 0.f;
            bool open = false;
            void add(float a, float b, ImU32 c) {
                if (open && c == col && a <= x1 + 1.f) {
                    x1 = std::max(x1, b);
                    return;
                }
                flush();
                x0 = a;
                x1 = b;
                col = c;
                open = true;
            }
            void flush() {
                if (!open)
                    return;
                float const w = x1 - x0;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 1.f), y1), col, w > 6.f ? 3.f : 0.f);
                open = false;
            }
        };

        // The lanes on screen: every lane with something in it, P1 Jump
        // always, all six with "All lanes" on (to add to an empty one).
        std::vector<int> shownLanes(State const& s) {
            std::vector<int> out;
            for (int l = 0; l < ed::kLanes; ++l)
                if (s.allLanes || l == 0 || !s.model.lanes[static_cast<size_t>(l)].empty())
                    out.push_back(l);
            return out;
        }

        // The index range of lane items that can reach [f0, f1].
        std::pair<size_t, size_t> visibleItems(State const& s, Actions const& a, int lane, double f0, double f1) {
            auto const& L = s.model.lanes[static_cast<size_t>(lane)];
            if (!s.model.sorted)
                return {0, L.size()};
            auto first = [&](uint32_t i) { return static_cast<double>(ed::firstFrame(a, s.model.items[i])); };
            size_t lo = static_cast<size_t>(
                std::lower_bound(L.begin(), L.end(), f0, [&](uint32_t i, double v) { return first(i) < v; }) -
                L.begin());
            if (lo > 0)
                --lo;  // the one before can reach into view (only one can, by the lane rule)
            size_t hi = static_cast<size_t>(
                std::upper_bound(L.begin(), L.end(), f1, [&](double v, uint32_t i) { return v < first(i); }) -
                L.begin());
            return {lo, std::max(lo, hi)};
        }

        struct Hit {
            uint32_t key = ed::kNone;
            Drag zone = Drag::Move;
        };

        Hit hitTest(State const& s, Actions const& a, View const& v, int lane, float sx) {
            Hit best;
            float bestDist = 1e9f;
            double const slack = 6.0 * v.fpp;
            auto [lo, hi] = visibleItems(s, a, lane, v.frame(sx) - slack, v.frame(sx) + slack);
            auto const& L = s.model.lanes[static_cast<size_t>(lane)];
            for (size_t j = lo; j < hi && j < L.size(); ++j) {
                ed::Item const& it = s.model.items[L[j]];
                float x0 = v.x(ed::firstFrame(a, it));
                float x1 = v.x(barEnd(s, a, it));
                if (x1 - x0 < 3.f)
                    x1 = x0 + 3.f;
                if (sx < x0 - 4.f || sx > x1 + 4.f)
                    continue;
                float const d = sx < x0 ? x0 - sx : sx > x1 ? sx - x1 : 0.f;
                if (d > bestDist)
                    continue;
                bestDist = d;
                best.key = it.key();
                best.zone = Drag::Move;
                if (it.kind == ed::Kind::Click && x1 - x0 >= kMinEdgeBar) {
                    if (std::abs(sx - x0) <= kEdgeGrab)
                        best.zone = Drag::PressEdge;
                    else if (std::abs(sx - x1) <= kEdgeGrab)
                        best.zone = Drag::ReleaseEdge;
                }
            }
            return best;
        }

        std::string describeItem(State const& s, Actions const& a, ed::Item const& it) {
            std::string const who = ed::laneName(it.lane);
            uint32_t const f = ed::firstFrame(a, it);
            std::string out;
            switch (it.kind) {
                case ed::Kind::Click: {
                    uint32_t const r = ed::lastFrame(a, it);
                    out = fmt::format("{} click\nPress {}, release {} (held {} frame{})", who, f, r, r - f,
                                      r - f == 1 ? "" : "s");
                    break;
                }
                case ed::Kind::Press: {
                    uint32_t const e = ed::holdEnd(a, s.model, it);
                    out = e == ed::kLastFrame
                              ? fmt::format("{} press at {}\nNever released: held to the end", who, f)
                              : fmt::format("{} press at {}\nHeld until {}, where a death or restart lets go", who, f,
                                            e);
                    break;
                }
                case ed::Kind::Release:
                    out = fmt::format("{} release at {}\nNo press before it", who, f);
                    break;
            }
            if (std::binary_search(s.problemKeys.begin(), s.problemKeys.end(), it.key()))
                for (auto const& fd : s.report.findings)
                    if (fd.index == it.press || fd.index == it.release)
                        out += fmt::format("\nProblem: {}", macrocheck::name(fd.problem));
            return out;
        }

        // Every item of the shown lanes that overlaps the box.
        std::vector<uint32_t> itemsInBox(State const& s, Actions const& a, View const& v, std::vector<int> const& lanes,
                                         float lanesTop, ImVec2 p0, ImVec2 p1) {
            std::vector<uint32_t> out;
            float const ya = std::min(p0.y, p1.y), yb = std::max(p0.y, p1.y);
            double const fa = v.frame(std::min(p0.x, p1.x)), fb = v.frame(std::max(p0.x, p1.x));
            for (size_t r = 0; r < lanes.size(); ++r) {
                float const top = lanesTop + static_cast<float>(r) * kLaneH;
                if (yb < top || ya > top + kLaneH)
                    continue;
                auto [lo, hi] = visibleItems(s, a, lanes[r], fa, fb);
                auto const& L = s.model.lanes[static_cast<size_t>(lanes[r])];
                for (size_t j = lo; j < hi && j < L.size(); ++j) {
                    ed::Item const& it = s.model.items[L[j]];
                    if (barEnd(s, a, it) >= fa && ed::firstFrame(a, it) <= fb)
                        out.push_back(it.key());
                }
            }
            return out;
        }

        // A tick spacing of 1, 2 or 5 times a power of ten, at least `minPx` apart.
        double rulerStep(double fpp, float minPx) {
            double const want = fpp * minPx;
            double step = 1.0;
            while (true) {
                for (double m : {1.0, 2.0, 5.0})
                    if (step * m >= want)
                        return step * m;
                step *= 10.0;
            }
        }

        // ------------------------------------------------------------ the timeline

        void overview(State& s, Actions const& a, View const& v, std::vector<int> const& lanes) {
            auto const& pal = look().pal;
            auto* dl = ImGui::GetWindowDrawList();
            ImVec2 const o = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##overview", ImVec2(kLabelW + v.w, kOverviewH));
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            bool const hovered = ImGui::IsItemHovered();

            double const total = extent(s);
            View ov;
            ov.x0 = v.x0;
            ov.w = v.w;
            ov.start = 0.0;
            ov.fpp = total / v.w;

            {
                UseFont f(Font::Small);
                dl->AddText(ImVec2(o.x, o.y + (kOverviewH - ImGui::GetFontSize()) * 0.5f), u32(pal.muted), "Overview");
            }
            ImVec2 const t0(v.x0, o.y), t1(v.x0 + v.w, o.y + kOverviewH);
            dl->AddRectFilled(t0, t1, u32(pal.raised, 0.6f), pal.radius);
            float const rowH = (kOverviewH - 6.f) / static_cast<float>(std::max<size_t>(1, lanes.size()));
            for (size_t r = 0; r < lanes.size(); ++r) {
                Runs runs;
                runs.dl = dl;
                runs.y0 = o.y + 3.f + static_cast<float>(r) * rowH;
                runs.y1 = runs.y0 + std::max(1.f, rowH - 1.f);
                ImU32 const col = u32(lanes[r] >= 3 ? mix(pal.accent, pal.ink, 0.35f) : pal.accent, 0.85f);
                for (uint32_t i : s.model.lanes[static_cast<size_t>(lanes[r])]) {
                    ed::Item const& it = s.model.items[i];
                    runs.add(ov.x(ed::firstFrame(a, it)), std::max(ov.x(barEnd(s, a, it)), ov.x(ed::firstFrame(a, it)) + 1.f),
                             col);
                }
                runs.flush();
            }
            for (uint32_t r : s.model.resetFrames)
                dl->AddLine(ImVec2(ov.x(r), t0.y + 1.f), ImVec2(ov.x(r), t1.y - 1.f), u32(pal.bad, 0.7f));
            // The part of the macro the lanes below show.
            float const vx0 = std::clamp(ov.x(v.start), t0.x, t1.x);
            float const vx1 = std::max(vx0 + 2.f, std::min(ov.x(v.end()), t1.x));
            dl->AddRectFilled(ImVec2(vx0, t0.y), ImVec2(vx1, t1.y), u32(pal.ink, 0.08f), 2.f);
            dl->AddRect(ImVec2(vx0, t0.y), ImVec2(vx1, t1.y), u32(pal.accent), 2.f, 0, 1.5f);

            ImGuiIO const& io = ImGui::GetIO();
            if (ImGui::IsItemActivated())
                s.drag = Drag::Overview;
            if (s.drag == Drag::Overview && ImGui::IsItemActive()) {
                double const centre = ov.frame(io.MousePos.x);
                s.start = centre - static_cast<double>(v.w) * s.fpp * 0.5;
                clampView(s, v.w);
            }
            if (s.drag == Drag::Overview && !ImGui::IsItemActive())
                s.drag = Drag::None;
            if (hovered && io.MouseWheel != 0.f)
                zoomAround(s, v, v.x0 + v.w * 0.5f, std::pow(0.8, static_cast<double>(io.MouseWheel)));
        }

        void lanesView(State& s, Actions const& a, View const& v, std::vector<int> const& lanes, bool editable) {
            auto const& pal = look().pal;
            ImGuiIO const& io = ImGui::GetIO();
            auto* dl = ImGui::GetWindowDrawList();
            ImVec2 const c = ImGui::GetCursorScreenPos();
            float const height = kRulerH + kLaneH * static_cast<float>(lanes.size());
            ImGui::InvisibleButton("##lanes", ImVec2(kLabelW + v.w, height),
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            bool const hovered = ImGui::IsItemHovered();
            float const lanesTop = c.y + kRulerH;
            float const right = v.x0 + v.w;
            // An edit this frame rebuilds the model; `hit` then names an old key.
            uint64_t const seenAtStart = s.seen;
            double const f0 = v.start, f1 = v.end();

            auto laneAt = [&](float y) -> int {
                if (y < lanesTop)
                    return -1;
                int const r = static_cast<int>((y - lanesTop) / kLaneH);
                return r >= 0 && r < static_cast<int>(lanes.size()) ? lanes[static_cast<size_t>(r)] : -1;
            };

            // ---- input

            if (hovered) {
                if (io.MouseWheel != 0.f) {
                    if (io.KeyShift) {
                        s.start -= static_cast<double>(io.MouseWheel) * v.w * 0.15 * s.fpp;
                        clampView(s, v.w);
                    } else {
                        zoomAround(s, v, std::clamp(io.MousePos.x, v.x0, right),
                                   std::pow(0.8, static_cast<double>(io.MouseWheel)));
                    }
                }
                if (io.MouseWheelH != 0.f) {
                    s.start -= static_cast<double>(io.MouseWheelH) * v.w * 0.15 * s.fpp;
                    clampView(s, v.w);
                }
            }

            int const laneHere = laneAt(io.MousePos.y);
            bool const inTrack = io.MousePos.x >= v.x0 && io.MousePos.x <= right;
            Hit const hit = (hovered && laneHere >= 0 && inTrack) ? hitTest(s, a, v, laneHere, io.MousePos.x) : Hit{};

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && inTrack && laneHere >= 0) {
                if (hit.key != ed::kNone) {
                    bool const isSel = selected(s, hit.key);
                    if (io.KeyCtrl) {
                        if (isSel)
                            std::erase(s.sel, hit.key);
                        else
                            s.sel.insert(std::lower_bound(s.sel.begin(), s.sel.end(), hit.key), hit.key);
                    } else if (editable) {
                        s.dragFrom = v.frame(io.MousePos.x);
                        s.dragKey = hit.key;
                        if (hit.zone == Drag::Move) {
                            if (!isSel)
                                s.sel = {hit.key};
                            s.drag = Drag::Move;
                            s.range = ed::moveRange(a, s.model, s.sel);
                            s.dragValue = s.dragWanted = 0;
                        } else {
                            s.sel = {hit.key};
                            s.drag = hit.zone;
                            ed::Edge const e = hit.zone == Drag::PressEdge ? ed::Edge::Press : ed::Edge::Release;
                            s.range = ed::edgeRange(a, s.model, hit.key, e);
                            ed::Item const& it = s.model.items[ed::itemOfKey(s.model, hit.key)];
                            s.dragOrigin = a[e == ed::Edge::Press ? it.press : it.release].m_frame;
                            s.dragValue = s.dragWanted = s.dragOrigin;
                        }
                    } else if (!isSel) {
                        s.sel = {hit.key};  // locked: it can still be selected and looked at
                    }
                } else {
                    s.boxBase = io.KeyCtrl ? s.sel : std::vector<uint32_t>{};
                    if (!io.KeyCtrl)
                        s.sel.clear();
                    s.drag = Drag::Box;
                    s.boxFrom = io.MousePos;
                }
            }

            // Double-click an empty spot: a click there, with the add form's hold.
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && inTrack && laneHere >= 0 &&
                hit.key == ed::kNone) {
                s.drag = Drag::None;
                if (canEdit(s)) {
                    int64_t const at = static_cast<int64_t>(std::floor(v.frame(io.MousePos.x)));
                    s.addLane = laneHere;
                    commit(s, ed::addClick(a, s.model, laneHere, static_cast<uint32_t>(std::max<int64_t>(at, 0)),
                                           static_cast<uint32_t>(std::max(0, s.addHold))));
                }
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hit.key != ed::kNone) {
                if (!selected(s, hit.key))
                    s.sel = {hit.key};
                ImGui::OpenPopup("##barmenu");
            }

            // Dragging. Past the track's ends the view scrolls along.
            if (s.drag == Drag::Move || s.drag == Drag::PressEdge || s.drag == Drag::ReleaseEdge ||
                s.drag == Drag::Box) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float over = 0.f;
                    if (io.MousePos.x < v.x0)
                        over = io.MousePos.x - v.x0;
                    else if (io.MousePos.x > right)
                        over = io.MousePos.x - right;
                    if (over != 0.f && s.drag != Drag::Box) {
                        s.start += static_cast<double>(over) * s.fpp * std::min(1.0, io.DeltaTime * 8.0);
                        clampView(s, v.w);
                    }
                    int64_t const moved = std::llround(v.frame(io.MousePos.x) - s.dragFrom);
                    if (s.drag == Drag::Move) {
                        s.dragWanted = moved;
                        s.dragValue = s.range.clamp(moved);
                    } else if (s.drag != Drag::Box) {
                        s.dragWanted = s.dragOrigin + moved;
                        s.dragValue = s.range.clamp(s.dragWanted);
                    }
                } else {
                    // Released: the drag becomes one edit (one undo step).
                    Drag const done = s.drag;
                    s.drag = Drag::None;
                    if (done == Drag::Box) {
                        std::vector<uint32_t> got =
                            itemsInBox(s, a, v, lanes, lanesTop, s.boxFrom, io.MousePos);
                        got.insert(got.end(), s.boxBase.begin(), s.boxBase.end());
                        std::sort(got.begin(), got.end());
                        got.erase(std::unique(got.begin(), got.end()), got.end());
                        s.sel = std::move(got);
                    } else if (done == Drag::Move && s.dragValue != 0 && canEdit(s)) {
                        commit(s, ed::move(a, s.model, s.sel, s.dragValue));
                        if (s.dragValue != s.dragWanted && s.msg.tone == Tone::Good)
                            s.msg.text += " Stopped there: " +
                                          (s.dragWanted < s.dragValue ? s.range.loWhy : s.range.hiWhy) + ".";
                    } else if ((done == Drag::PressEdge || done == Drag::ReleaseEdge) && s.dragValue != s.dragOrigin &&
                               canEdit(s)) {
                        uint32_t const idx = ed::itemOfKey(s.model, s.dragKey);
                        if (idx != ed::kNone) {
                            ed::Item const& it = s.model.items[idx];
                            uint32_t p = a[it.press].m_frame, r = a[it.release].m_frame;
                            (done == Drag::PressEdge ? p : r) = static_cast<uint32_t>(s.dragValue);
                            commit(s, ed::setEdges(a, s.model, s.dragKey, p, r));
                            if (s.dragValue != s.dragWanted && s.msg.tone == Tone::Good)
                                s.msg.text += " Stopped there: " +
                                              (s.dragWanted < s.dragValue ? s.range.loWhy : s.range.hiWhy) + ".";
                        }
                    }
                }
            }

            // ---- drawing (the model may have just been rebuilt by an edit)

            dl->PushClipRect(c, ImVec2(right, c.y + height), true);
            for (size_t r = 0; r < lanes.size(); ++r) {
                float const top = lanesTop + static_cast<float>(r) * kLaneH;
                dl->AddRectFilled(ImVec2(v.x0, top), ImVec2(right, top + kLaneH),
                                  u32(r % 2 ? pal.raised : pal.backdrop, r % 2 ? 0.35f : 0.25f));
                ImVec4 const labelCol = lanes[r] >= 3 ? mix(pal.ink, pal.accent, 0.45f) : pal.ink;
                dl->AddText(ImVec2(c.x + 2.f, top + (kLaneH - ImGui::GetFontSize()) * 0.5f), u32(labelCol),
                            ed::laneName(lanes[r]));
            }
            dl->AddLine(ImVec2(c.x, lanesTop), ImVec2(right, lanesTop), u32(pal.line));

            // Ruler: frame numbers, at least 70 px apart.
            dl->PushClipRect(ImVec2(v.x0, c.y), ImVec2(right, c.y + height), true);
            {
                UseFont f(Font::Small);
                double const step = rulerStep(s.fpp, 70.f);
                for (double fr = std::ceil(f0 / step) * step; fr <= f1; fr += step) {
                    float const x = v.x(fr);
                    dl->AddLine(ImVec2(x, c.y + kRulerH - 5.f), ImVec2(x, c.y + height), u32(pal.line, 0.45f));
                    dl->AddText(ImVec2(x + 3.f, c.y + 1.f), u32(pal.muted),
                                fmt::format("{}", static_cast<int64_t>(fr)).c_str());
                }
            }

            // Deaths, restarts and TPS changes, top to bottom.
            if (!s.model.events.empty()) {
                auto const& E = s.model.events;
                size_t i = 0;
                if (s.model.sorted)
                    i = static_cast<size_t>(std::lower_bound(E.begin(), E.end(), f0,
                                                             [&](uint32_t k, double fv) { return a[k].m_frame < fv; }) -
                                            E.begin());
                for (; i < E.size(); ++i) {
                    gb::Action const& x = a[E[i]];
                    if (x.m_frame > f1) {
                        if (s.model.sorted)
                            break;
                        continue;
                    }
                    if (x.m_frame < f0)
                        continue;
                    bool const reset = macrocheck::isReset(x);
                    float const ex = v.x(x.m_frame);
                    dl->AddLine(ImVec2(ex, c.y), ImVec2(ex, c.y + height),
                                u32(reset ? pal.bad : pal.accent, reset ? 0.85f : 0.5f), reset ? 2.f : 1.f);
                }
            }

            // The bars.
            ImU32 const colClick = u32(mix(pal.accent, pal.surface, 0.35f), 0.95f);
            ImU32 const colClickP2 = u32(mix(mix(pal.accent, pal.ink, 0.35f), pal.surface, 0.35f), 0.95f);
            ImU32 const colLonePress = u32(pal.warn, 0.75f);
            ImU32 const colLoneRelease = u32(pal.muted, 0.9f);
            ImU32 const colProblem = u32(pal.bad, 0.95f);
            ImU32 const colSel = u32(pal.accent);
            bool const ghosting = (s.drag == Drag::Move && s.dragValue != 0) ||
                                  ((s.drag == Drag::PressEdge || s.drag == Drag::ReleaseEdge) &&
                                   s.dragValue != s.dragOrigin);
            for (size_t r = 0; r < lanes.size(); ++r) {
                int const lane = lanes[r];
                float const top = lanesTop + static_cast<float>(r) * kLaneH;
                Runs runs;
                runs.dl = dl;
                runs.y0 = top + 5.f;
                runs.y1 = top + kLaneH - 5.f;
                auto [lo, hi] = visibleItems(s, a, lane, f0, f1);
                auto const& L = s.model.lanes[static_cast<size_t>(lane)];
                for (size_t j = lo; j < hi && j < L.size(); ++j) {
                    ed::Item const& it = s.model.items[L[j]];
                    uint32_t const key = it.key();
                    bool const isSel = selected(s, key);
                    bool const problem = std::binary_search(s.problemKeys.begin(), s.problemKeys.end(), key);
                    float x0 = v.x(ed::firstFrame(a, it));
                    float x1 = v.x(barEnd(s, a, it));
                    ImU32 col = it.kind == ed::Kind::Click ? (lane >= 3 ? colClickP2 : colClick)
                                : it.kind == ed::Kind::Press ? colLonePress
                                                             : colLoneRelease;
                    if (problem)
                        col = colProblem;
                    if (isSel)
                        col = colSel;
                    if (isSel && ghosting)
                        col = u32(pal.accent, 0.3f);  // where it was; the ghost is where it goes
                    if (it.kind == ed::Kind::Release) {
                        runs.add(x0 - 1.f, x0 + 1.f, col);
                        continue;
                    }
                    runs.add(x0, std::max(x1, x0 + 3.f), col);
                    if (isSel && !ghosting && x1 - x0 > 4.f) {
                        runs.flush();
                        dl->AddRect(ImVec2(x0, runs.y0), ImVec2(x1, runs.y1), u32(pal.ink, 0.9f), 3.f, 0, 1.5f);
                    }
                }
                runs.flush();
            }

            // Where the drag will put things.
            if (ghosting) {
                for (size_t r = 0; r < lanes.size(); ++r) {
                    int const lane = lanes[r];
                    float const top = lanesTop + static_cast<float>(r) * kLaneH;
                    for (uint32_t key : s.sel) {
                        uint32_t const idx = ed::itemOfKey(s.model, key);
                        if (idx == ed::kNone || s.model.items[idx].lane != lane)
                            continue;
                        ed::Item const& it = s.model.items[idx];
                        double g0 = ed::firstFrame(a, it), g1 = barEnd(s, a, it);
                        if (s.drag == Drag::Move) {
                            g0 += static_cast<double>(s.dragValue);
                            g1 += static_cast<double>(s.dragValue);
                        } else if (key == s.dragKey) {
                            (s.drag == Drag::PressEdge ? g0 : g1) = static_cast<double>(s.dragValue);
                        } else {
                            continue;
                        }
                        if (g1 < f0 || g0 > f1)
                            continue;
                        float const x0 = v.x(g0), x1 = std::max(v.x(g1), v.x(g0) + 3.f);
                        dl->AddRectFilled(ImVec2(x0, top + 5.f), ImVec2(x1, top + kLaneH - 5.f), u32(pal.accent, 0.8f),
                                          3.f);
                        dl->AddRect(ImVec2(x0, top + 5.f), ImVec2(x1, top + kLaneH - 5.f), u32(pal.ink), 3.f, 0, 1.5f);
                    }
                }
            }

            // The frame under the mouse.
            if (hovered && inTrack && s.drag == Drag::None) {
                double const fr = std::floor(v.frame(io.MousePos.x));
                float const x = v.x(fr);
                dl->AddLine(ImVec2(x, c.y), ImVec2(x, c.y + height), u32(pal.ink, 0.25f));
            }

            if (s.drag == Drag::Box) {
                ImVec2 const p0 = s.boxFrom, p1 = io.MousePos;
                ImVec2 const mn(std::min(p0.x, p1.x), std::min(p0.y, p1.y));
                ImVec2 const mx(std::max(p0.x, p1.x), std::max(p0.y, p1.y));
                dl->AddRectFilled(mn, mx, u32(pal.accent, 0.12f));
                dl->AddRect(mn, mx, u32(pal.accent, 0.8f));
            }
            dl->PopClipRect();
            dl->PopClipRect();

            // ---- tooltips and the bar menu

            if (s.drag == Drag::Move && ghosting) {
                std::string t = fmt::format("Move {}{}", s.dragValue > 0 ? "+" : "", s.dragValue);
                if (s.dragWanted != s.dragValue)
                    t += "\nStopped: " + (s.dragWanted < s.dragValue ? s.range.loWhy : s.range.hiWhy);
                ImGui::SetTooltip("%s", t.c_str());
            } else if ((s.drag == Drag::PressEdge || s.drag == Drag::ReleaseEdge) && s.dragKey != ed::kNone) {
                std::string t = fmt::format("{} {}", s.drag == Drag::PressEdge ? "Press" : "Release", s.dragValue);
                if (s.dragWanted != s.dragValue)
                    t += "\nStopped: " + (s.dragWanted < s.dragValue ? s.range.loWhy : s.range.hiWhy);
                ImGui::SetTooltip("%s", t.c_str());
            } else if (hovered && s.drag == Drag::None && s.seen == seenAtStart) {
                if (hit.key != ed::kNone) {
                    if (hit.zone != Drag::Move)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    uint32_t const idx = ed::itemOfKey(s.model, hit.key);
                    if (idx != ed::kNone)
                        ImGui::SetTooltip("%s", describeItem(s, a, s.model.items[idx]).c_str());
                } else if (inTrack && laneHere >= 0) {
                    ImGui::SetTooltip("Frame %lld", static_cast<long long>(std::floor(v.frame(io.MousePos.x))));
                }
            }

            if (ImGui::BeginPopup("##barmenu")) {
                std::string const label = s.sel.size() == 1 ? "Delete" : fmt::format("Delete {}", s.sel.size());
                if (ImGui::MenuItem(label.c_str(), "Del", false, editable))
                    deleteSelection(s);
                if (ImGui::MenuItem("Select this lane")) {
                    uint32_t const idx = s.sel.empty() ? ed::kNone : ed::itemOfKey(s.model, s.sel.front());
                    if (idx != ed::kNone) {
                        std::vector<uint32_t> keys;
                        for (uint32_t i : s.model.lanes[static_cast<size_t>(s.model.items[idx].lane)])
                            keys.push_back(s.model.items[i].key());
                        s.sel = std::move(keys);
                    }
                }
                ImGui::EndPopup();
            }
        }

        // ------------------------------------------------------------ cards

        void headerCard(State& s, const char* lock) {
            auto* gb = GucciEngine::get();
            kit::BeginCard("Loaded macro", "Every click of it as a bar. Edits stay in memory until Save on the Macro page.");
            if (lock)
                kit::Note(lock, Tone::Warn);

            auto const& m = s.model;
            if (macro().empty()) {
                kit::Note("No macro is loaded. Record one or load one on the Macro page, or start one here by "
                          "double-clicking a lane.",
                          Tone::Muted);
            } else {
                kit::Chip(fmt::format("{} click{}", m.clicks, m.clicks == 1 ? "" : "s").c_str(), Tone::Accent);
                if (!m.lanes[3].empty() || !m.lanes[4].empty() || !m.lanes[5].empty()) {
                    ImGui::SameLine();
                    kit::Chip("Player 2", Tone::Muted);
                }
                if (!m.resetFrames.empty()) {
                    ImGui::SameLine();
                    kit::Chip(fmt::format("{} death{} / restart{}", m.resetFrames.size(),
                                          m.resetFrames.size() == 1 ? "" : "s", m.resetFrames.size() == 1 ? "" : "s")
                                  .c_str(),
                              Tone::Muted);
                }
                if (!s.report.findings.empty()) {
                    ImGui::SameLine();
                    kit::Chip(fmt::format("{} problem{}", s.report.findings.size(),
                                          s.report.findings.size() == 1 ? "" : "s")
                                  .c_str(),
                              Tone::Bad);
                }
                if (!gb->replayName.empty()) {
                    ImGui::SameLine();
                    kit::Chip(gb->replayName.c_str(), Tone::Plain);
                }
            }
            kit::Gap(0.3f);

            ImGui::BeginDisabled(!s.history.canUndo() || lock);
            if (kit::Button("Undo"))
                undo(s);
            ImGui::EndDisabled();
            if (s.history.canUndo() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Undo: %s  (Ctrl+Z)", s.history.undoLabel().c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!s.history.canRedo() || lock);
            if (kit::Button("Redo"))
                redo(s);
            ImGui::EndDisabled();
            if (s.history.canRedo() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Redo: %s  (Ctrl+Y)", s.history.redoLabel().c_str());
            ImGui::SameLine(0.f, 18.f);
            bool const zoomIn = kit::Button(" + ");
            ImGui::SameLine();
            bool const zoomOut = kit::Button(" - ");
            ImGui::SameLine();
            if (kit::Button("Fit"))
                s.fpp = 0.0;
            if ((zoomIn || zoomOut) && s.fpp > 0.0) {
                // Around the middle of what is on screen.
                double const centre = s.start + static_cast<double>(s.trackW) * s.fpp * 0.5;
                s.fpp *= zoomIn ? 0.5 : 2.0;
                clampView(s, s.trackW);
                s.start = centre - static_cast<double>(s.trackW) * s.fpp * 0.5;
                clampView(s, s.trackW);
            }
            ImGui::SameLine(0.f, 18.f);
            kit::Switch("##alllanes", &s.allLanes);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("All lanes");

            if (!s.msg.text.empty())
                kit::Note(s.msg.text.c_str(), s.msg.tone);
            kit::EndCard();
        }

        void timelineCard(State& s, bool editable) {
            Actions const& a = macro();
            kit::BeginCard("Timeline", nullptr);
            float const width = kit::Avail();
            float const trackW = std::max(40.f, width - kLabelW);
            s.trackW = trackW;
            clampView(s, trackW);
            View v;
            v.x0 = ImGui::GetCursorScreenPos().x + kLabelW;
            v.w = trackW;
            v.start = s.start;
            v.fpp = s.fpp;
            std::vector<int> const lanes = shownLanes(s);

            overview(s, a, v, lanes);
            v.start = s.start;
            v.fpp = s.fpp;
            kit::Gap(0.25f);
            lanesView(s, macro(), v, lanes, editable);
            kit::Hint("Drag a bar to move it, or its ends to stretch or shorten it (zoom in for short ones). "
                      "Ctrl+click adds to the selection; drag on empty space to box-select; double-click an empty "
                      "spot to add a click. Wheel zooms, Shift+wheel scrolls. Frame numbers are the macro's own: a "
                      "press made while the frame counter shows 99 is stored at 100.");
            kit::EndCard();
        }

        // Refill the inspector's fields when the selection or the macro changed.
        void refreshFields(State& s, Actions const& a) {
            uint32_t const key = s.sel.size() == 1 ? s.sel.front() : ed::kNone;
            if (key == s.fieldsKey && s.fieldsFor == s.seen)
                return;
            s.fieldsKey = key;
            s.fieldsFor = s.seen;
            uint32_t const idx = key == ed::kNone ? ed::kNone : ed::itemOfKey(s.model, key);
            if (idx == ed::kNone)
                return;
            ed::Item const& it = s.model.items[idx];
            s.fieldPress = static_cast<int>(ed::firstFrame(a, it));
            s.fieldRelease = static_cast<int>(ed::lastFrame(a, it));
        }

        void nudgeRow(State& s) {
            kit::RowBegin("Nudge", "Earlier or later, in frames");
            for (int d : {-10, -1, 1, 10}) {
                if (d != -10)
                    ImGui::SameLine();
                if (kit::Button(fmt::format("{}{}##n{}", d > 0 ? "+" : "", d, d).c_str()))
                    moveSelection(s, d);
            }
            kit::RowEnd();
        }

        void inspectorCard(State& s, bool editable) {
            Actions const& a = macro();
            refreshFields(s, a);
            kit::BeginCard("Inspector", nullptr);
            ImGui::BeginDisabled(!editable);

            uint32_t const idx = s.sel.size() == 1 ? ed::itemOfKey(s.model, s.sel.front()) : ed::kNone;
            if (idx != ed::kNone) {
                ed::Item const it = s.model.items[idx];
                uint32_t const key = it.key();
                kit::Section(describeItem(s, a, it).c_str());
                if (it.kind == ed::Kind::Click) {
                    uint32_t const p = a[it.press].m_frame, r = a[it.release].m_frame;
                    ed::Range const pr = ed::edgeRange(a, s.model, key, ed::Edge::Press);
                    ed::Range const rr = ed::edgeRange(a, s.model, key, ed::Edge::Release);
                    kit::RowBegin("Press", "Frame the button goes down");
                    kit::InputInt("press", &s.fieldPress, 0);
                    bool const pressDone = ImGui::IsItemDeactivatedAfterEdit();
                    kit::RowEnd();
                    kit::RowBegin("Release", "Frame it comes back up");
                    kit::InputInt("release", &s.fieldRelease, 0);
                    bool const releaseDone = ImGui::IsItemDeactivatedAfterEdit();
                    kit::RowEnd();
                    if ((pressDone || releaseDone) && canEdit(s)) {
                        uint32_t const np = pressDone ? static_cast<uint32_t>(std::max(0, s.fieldPress)) : p;
                        uint32_t const nr = releaseDone ? static_cast<uint32_t>(std::max(0, s.fieldRelease)) : r;
                        if (np != p || nr != r)
                            commit(s, ed::setEdges(a, s.model, key, np, nr));
                        s.fieldsKey = ed::kNone;  // show what the macro holds now
                    }
                    kit::Hint(fmt::format("The press can go from {} ({}); the release up to {} ({}).", pr.lo, pr.loWhy,
                                          rr.hi, rr.hiWhy)
                                  .c_str());
                } else {
                    kit::RowBegin("Frame", nullptr);
                    kit::InputInt("frame", &s.fieldPress, 0);
                    bool const done = ImGui::IsItemDeactivatedAfterEdit();
                    kit::RowEnd();
                    if (done && canEdit(s)) {
                        int64_t const delta = static_cast<int64_t>(s.fieldPress) - ed::firstFrame(a, it);
                        if (delta != 0)
                            commit(s, ed::move(a, s.model, s.sel, delta));
                        s.fieldsKey = ed::kNone;
                    }
                }
                nudgeRow(s);
                kit::Gap(0.3f);
                if (kit::Button("Delete", Tone::Bad))
                    deleteSelection(s);
            } else if (s.sel.size() > 1) {
                uint64_t selHash = 1469598103934665603ull;
                for (uint32_t k : s.sel)
                    selHash = (selHash ^ k) * 1099511628211ull;
                if (s.rangeFor != s.seen || s.rangeSel != selHash) {
                    s.selRange = ed::moveRange(a, s.model, s.sel);
                    s.rangeFor = s.seen;
                    s.rangeSel = selHash;
                }
                ed::Range const& r = s.selRange;
                kit::Section(fmt::format("{} selected", s.sel.size()).c_str());
                kit::Hint(fmt::format("Together they can move {} frame{} earlier ({}) and {} later ({}).", -r.lo,
                                      r.lo == -1 ? "" : "s", r.loWhy, r.hi, r.hiWhy)
                              .c_str());
                nudgeRow(s);
                kit::RowBegin("Move by", "Frames; negative moves earlier");
                ImGui::SetNextItemWidth(120.f);
                ImGui::PushID("moveby");
                ImGui::InputInt("##v", &s.moveBy, 0, 0);
                ImGui::PopID();
                ImGui::SameLine();
                if (kit::Button("Move"))
                    moveSelection(s, s.moveBy);
                kit::RowEnd();
                kit::Gap(0.3f);
                if (kit::Button(fmt::format("Delete {}", s.sel.size()).c_str(), Tone::Bad))
                    deleteSelection(s);
                ImGui::SameLine();
                if (kit::Button("Clear selection"))
                    s.sel.clear();
            } else {
                kit::Section("Add a click");
                static const char* const kLaneNames[ed::kLanes] = {"P1 Jump", "P1 Left", "P1 Right",
                                                                   "P2 Jump", "P2 Left", "P2 Right"};
                kit::DropdownRow("Lane", nullptr, &s.addLane, kLaneNames, ed::kLanes);
                kit::RowBegin("Press at", "Frame");
                kit::InputInt("at", &s.addAt, 0);
                kit::RowEnd();
                kit::RowBegin("Hold", "Frames until the release; double-clicking a lane uses this too");
                kit::InputInt("hold", &s.addHold, 0);
                kit::RowEnd();
                s.addAt = std::max(0, s.addAt);
                s.addHold = std::max(0, s.addHold);
                if (kit::Button("Add click", Tone::Accent) && canEdit(s)) {
                    commit(s, ed::addClick(a, s.model, s.addLane, static_cast<uint32_t>(s.addAt),
                                           static_cast<uint32_t>(s.addHold)));
                    if (s.msg.tone == Tone::Good)
                        s.addAt += s.addHold + 1;  // ready for the next one
                }
                kit::Hint("Select a bar to edit it. Ctrl+A selects everything.");
            }
            ImGui::EndDisabled();
            kit::EndCard();
        }

        // Keys while the page has focus and no field is being typed in.
        void shortcuts(State& s) {
            ImGuiIO const& io = ImGui::GetIO();
            if (io.WantTextInput)
                return;
            bool const ctrl = io.KeyCtrl;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (io.KeyShift)
                    redo(s);
                else
                    undo(s);
            } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                redo(s);
            } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                s.sel.clear();
                for (auto const& it : s.model.items)
                    s.sel.push_back(it.key());
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                deleteSelection(s);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                s.sel.clear();
            } else if (!s.sel.empty() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                moveSelection(s, io.KeyShift ? -10 : -1);
            } else if (!s.sel.empty() && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                moveSelection(s, io.KeyShift ? 10 : 1);
            }
        }

    } // namespace

    void editor() {
        State& s = st();
        sync(s);
        const char* lock = lockReason();
        // Taken before anything is drawn: the page (or a card in it) has focus.
        bool const focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        ImGui::PushID("frame-editor");
        headerCard(s, lock);
        timelineCard(s, lock == nullptr);
        inspectorCard(s, lock == nullptr);
        if (focused && s.drag == Drag::None)
            shortcuts(s);
        ImGui::PopID();
    }

} // namespace gucci::ui::pages
