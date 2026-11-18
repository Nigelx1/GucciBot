// The two practice trainer pages: Nigel's Jupiter My Favourite Trainer (one
// level, its macro built in) and the Trainer (any saved macro). They are the
// same page pointed at different state: a header, the Click Trainer (the bar,
// its clock controls, the deviation readout and Click Indicators scoring),
// Ghosts, Segments, Stats and Music. Everything that runs while the page is
// closed - the clock, input, scoring, the segment loop - is the engine's, in
// trainers/trainer_core.*; this file draws it and hands it the user's edits.
// Written fresh for 2.0.

#include "ui/kit.hpp"
#include "core/geode_compat.hpp" // gucci::stdPath
#include "ui/look.hpp"
#include "ui/search.hpp"
#include "ui/ui.hpp"

#include "core/GucciBot.hpp"
#include "trainers/trainer_core.hpp"
#include "trainers/lane.hpp"

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
    void videoModeCard();         // pages/videomode_card.cpp
    void trainerVideoModeCard();  // pages/videomode_card.cpp
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

        // "1 click", "7 clicks": the macro's chip on both trainer cards.
        std::string clicksChip(size_t n) {
            return fmt::format("{} {}", n, n == 1 ? "click" : "clicks");
        }

        // The Trainer page's macro picker and messages. g_pickFile is the file
        // the dropdown shows, so the entry follows that file when the list
        // changes under it (a Refresh, a Convert, a save) instead of keeping
        // its old place in the list.
        int g_pick = -1;
        std::pair<std::string, std::string> g_pickFile;
        bool g_relist = false;  // the list lacks the file that loaded: read it again
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
                got.path = gucci::stdPath(std::move(result).unwrap());
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

        // Branded here, where it shows (GBot while disguised): two messages
        // name GucciBot, one from trainers/trainer_core.
        void message(Message const& m) {
            if (!m.text.empty())
                kit::Note(branded(m.text).c_str(), m.tone);
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
        // answered turns the palette's hit color: green, or gold or white
        // where green is too close to the accent, look.cpp setHit), the
        // player's own through the bottom (presses full height, releases
        // half), segments shade the background, and the centre line stays put
        // while everything scrolls past it.
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
            // A look whose faded accent goes grey (the JMF look) draws the
            // bar's yellow solid: solid blocks and a glowing centre line. The
            // track stays see-through, over Video Mode's video too.
            bool const solid = pal.solidTints;
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
                                  solid ? u32(pal.line, sel ? 0.9f : 0.45f) : u32(pal.accent, sel ? 0.16f : 0.08f));
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
                dl->AddRectFilled(ImVec2(xa, laneTop), ImVec2(xb, laneMid - 4.f),
                                  solid ? u32(pal.accent) : u32(pal.accent, 0.45f), 3.f);
                // Its start: the hit colour once answered; until then the
                // accent, or white where the block is the accent already.
                ImVec4 const start = solid ? ImVec4(1.f, 1.f, 1.f, 1.f) : pal.accent;
                ImVec4 const edge = haveAnswers && answered[i] ? pal.hit : start;
                dl->AddLine(ImVec2(xa, laneTop - 2.f), ImVec2(xa, laneMid - 2.f), u32(edge), 2.5f);
            }

            // The player's own: white, or the ink colour on a light theme,
            // where white wouldn't show.
            ImU32 const mine = pal.light ? u32(pal.ink, 0.92f) : IM_COL32(255, 255, 255, 235);
            for (double t : s.myPresses)
                if (t >= left && t <= right)
                    dl->AddLine(ImVec2(x(t), laneMid), ImVec2(x(t), laneBot), mine, 2.f);
            for (double t : s.myReleases)
                if (t >= left && t <= right)
                    dl->AddLine(ImVec2(x(t), (laneMid + laneBot) * 0.5f), ImVec2(x(t), laneBot), mine, 1.f);

            // The fixed centre line.
            float const cx = p0.x + w * 0.5f;
            if (solid)
                dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), u32(pal.ink, 0.22f), 7.f);
            dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), u32(pal.ink), 2.f);
            dl->PopClipRect();
            dl->AddRect(p0, p1, u32(pal.line), pal.radius);

            // Lane names, outside the clip so the scroll never covers them.
            UseFont f(Font::Small);
            dl->AddText(ImVec2(p0.x + 6.f, laneTop + 2.f), u32(pal.muted, 0.8f), "macro");
            dl->AddText(ImVec2(p0.x + 6.f, laneMid + 2.f), u32(pal.muted, 0.8f), "you");
        }

        // ------------------------------------------------------------ the lane
        //
        // Juice's design (2026-11-17, "basically like osu!taiko"): the macro's
        // clicks scroll right to left into a fixed hit marker, an empty white
        // circle near the left end of a bold box. A note's look is its window
        // as Calculate counted it, bigger = more lenient: a small solid red
        // note for 1 frame, a red center in an orange ring for 2, in a big
        // yellow one for 3, in a green one past that (or when any timing
        // lived); a note with no counted window is plain light grey. Every
        // circle has a dark outline, so it reads on the lane. A hold whose
        // release is timed is a slider: a thick rounded bar from the press to
        // the release, a window mark at each end (the release end's center a
        // ring: let go there). Each press is judged as it lands: a big
        // PERFECT / EARLY 1f / LATE 2f / MISS pops up on the marker and fades,
        // and the marker pulses a ring in the verdict's color.
        //
        // Sizes are fractions of the lane's height, and the lane's height is
        // a fraction of the screen's (12% at size 1x), so it looks the same at
        // 720p, 1080p and 1440p. trainers/lane.* has the rules; this draws
        // them.

        namespace ln = gucci::trainers::lane;
        using LTier = gucci::lanerules::Tier;
        using LVerdict = gucci::lanerules::Verdict;

        // The notes, as Juice drew them.
        ImVec4 const kNoteRed{0.929f, 0.165f, 0.188f, 1.f};    // #ED2A30
        ImVec4 const kNoteOrange{1.f, 0.522f, 0.106f, 1.f};    // #FF851B
        ImVec4 const kNoteYellow{1.f, 0.894f, 0.106f, 1.f};    // #FFE41B
        ImVec4 const kNoteWide{0.341f, 0.831f, 0.341f, 1.f};   // #57D457
        ImVec4 const kNotePlain{0.835f, 0.851f, 0.882f, 1.f};  // #D5D9E1
        ImVec4 const kNoteP2{1.f, 0.388f, 0.788f, 1.f};        // #FF63C9, player 2's edge
        // The verdicts.
        ImVec4 const kWordPerfect{1.f, 0.792f, 0.157f, 1.f};  // gold #FFCA28
        ImVec4 const kWordEarly{0.424f, 0.753f, 1.f, 1.f};    // light blue #6CC0FF
        ImVec4 const kWordLate{1.f, 0.380f, 0.208f, 1.f};     // orange-red #FF6135
        ImVec4 const kWordMiss{0.651f, 0.667f, 0.702f, 1.f};  // grey #A6AAB3
        // The hold bars: see-through, so the notes stay the boldest shapes.
        ImVec4 const kBarIdle{0.588f, 0.612f, 0.675f, 1.f};   // rgb(150,156,172)

        // The lane's height: 12% of the screen's at 1x (about 130 px at 1080p).
        constexpr float kLaneOfScreen = 0.12f;
        // The hit marker's center, this many lane heights from the left edge.
        constexpr float kHitAt = 1.0f;
        // The verdict's font, as a share of the lane's height (its capitals
        // come to about 37%), and Roboto's capital height per font pixel.
        constexpr float kVerdictOfLane = 0.52f;
        constexpr float kCapHeight = 0.711f;
        // The verdict's pill sits this far outside the box.
        constexpr float kVerdictGap = 4.f;
        // The box's fill over the level, and while the player is near it.
        constexpr int kFillLevel = 165;
        constexpr int kFillNearPlayer = 90;
        constexpr int kFillCard = 235;

        // The verdict's color, by its word: PERFECT gold, EARLY blue, LATE
        // orange-red, MISS grey. A press near no note (no word) is grey.
        ImVec4 wordColor(std::string const& text) {
            if (text.empty())
                return kWordMiss;
            switch (text[0]) {
                case 'P': return kWordPerfect;
                case 'E': return kWordEarly;
                case 'L': return kWordLate;
                default: return kWordMiss;
            }
        }

        struct NoteSizes {
            float red;     // a 1-frame note's radius (40% of the lane's height across)
            float center;  // the red center inside a ring
            float two;     // 58% across
            float three;   // 76% across (and 4+)
            float plain;   // a note with no counted window
            float marker;  // the hit marker (70% across)
            float edge;    // the dark outline's width
        };
        NoteSizes noteSizes(float h) {
            return {h * 0.20f, h * 0.17f, h * 0.29f, h * 0.38f, h * 0.24f, h * 0.35f, std::max(2.f, h * 0.024f)};
        }

        float outerRadius(LTier tier, NoteSizes const& z) {
            switch (tier) {
                case LTier::Unknown: return z.plain;
                case LTier::One: return z.red;
                case LTier::Two: return z.two;
                case LTier::Three: return z.three;
                case LTier::Wide: return z.three;
            }
            return z.red;
        }

        ImVec4 tierColor(LTier tier) {
            switch (tier) {
                case LTier::Unknown: return kNotePlain;
                case LTier::One: return kNoteRed;
                case LTier::Two: return kNoteOrange;
                case LTier::Three: return kNoteYellow;
                case LTier::Wide: return kNoteWide;
            }
            return kNoteRed;
        }

        ImU32 darkEdge(float fade) {
            return IM_COL32(6, 7, 11, (int)(235 * fade));
        }

        // A press: a filled circle (red center in its window's ring), the
        // dark outline outside its edge. `gap`: more dark around it, for a
        // press drawn over a release end.
        void drawHead(ImDrawList* dl, ImVec2 c, LTier tier, NoteSizes const& z, bool p2, float fade, float gap) {
            if (fade <= 0.01f)
                return;
            float const ew = z.edge;
            float const r = outerRadius(tier, z);
            dl->AddCircleFilled(c, r + ew + gap, darkEdge(fade));
            if (tier == LTier::Unknown || tier == LTier::One) {
                dl->AddCircleFilled(c, r, u32(tierColor(tier), fade));
            } else {
                dl->AddCircleFilled(c, r, u32(tierColor(tier), fade));
                dl->AddCircleFilled(c, z.center + ew * 0.8f, darkEdge(fade));
                dl->AddCircleFilled(c, z.center, u32(kNoteRed, fade));
            }
            if (p2)
                dl->AddCircle(c, r + ew + gap + ew * 0.9f, u32(kNoteP2, fade), 0, ew * 1.5f);
        }

        // A release (a slider's end): a hollow ring in its window's color,
        // dark inside, a white upright line through it. A filled bullseye
        // always means press; a hollow ring, let go.
        void drawRelease(ImDrawList* dl, ImVec2 c, LTier tier, NoteSizes const& z, bool p2, float fade, float scale) {
            if (fade <= 0.01f)
                return;
            float const ew = z.edge;
            float const r = outerRadius(tier, z) * scale;
            float const t = std::max(3.f, r * 0.36f);  // 18% of its diameter
            dl->AddCircleFilled(c, r + ew, darkEdge(fade));
            dl->AddCircleFilled(c, r, u32(tierColor(tier), fade));
            dl->AddCircleFilled(c, r - t, IM_COL32(12, 13, 20, (int)(255 * fade)));
            float const lw = std::max(3.f, ew * 1.1f);
            float const lh = (r - t) * 0.78f;
            dl->AddRectFilled(ImVec2(c.x - lw * 0.5f, c.y - lh), ImVec2(c.x + lw * 0.5f, c.y + lh),
                              IM_COL32(255, 255, 255, (int)(240 * fade)), lw * 0.5f);
            if (p2)
                dl->AddCircle(c, r + ew * 1.9f, u32(kNoteP2, fade), 0, ew * 1.5f);
        }

        // A word drawn big with a dark outline, its top-left at `at`.
        void outlinedText(ImDrawList* dl, ImFont* font, float px, ImVec2 at, ImVec4 col, float alpha,
                          char const* text) {
            float const o = std::max(1.5f, px * 0.045f);
            ImU32 const dark = IM_COL32(0, 0, 0, (int)(235 * alpha));
            static constexpr float kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                                  {0.71f, 0.71f}, {-0.71f, 0.71f}, {0.71f, -0.71f}, {-0.71f, -0.71f}};
            for (auto const& d : kDirs)
                dl->AddText(font, px, ImVec2(at.x + d[0] * o, at.y + d[1] * o), dark, text);
            dl->AddText(font, px, at, u32(col, alpha), text);
        }

        // The verdict's pill: its padding and height for a lane h tall (the
        // card keeps that much room under its lane).
        float verdictPad(float h) {
            return std::max(6.f, std::round(h * 0.07f));
        }
        float verdictPillHeight(float h) {
            return h * kVerdictOfLane * kCapHeight + 2.f * verdictPad(h);
        }

        // The verdict, big, on a dark pill centered under the marker (over it
        // when `below` is false): off the track, so it never covers the next
        // note or sits on a bar. It pops in and fades over 0.6 s.
        void drawVerdict(ImDrawList* dl, char const* text, bool release, ImVec4 col, float age, float hitX, ImVec2 p0,
                         ImVec2 p1, float h, bool below) {
            float const pop = age < 0.07f ? 1.2f - 0.2f * (age / 0.07f) : 1.f;
            float const alpha = age < 0.35f ? 1.f : std::max(0.f, 1.f - (age - 0.35f) / 0.25f);
            float const px = std::round(h * kVerdictOfLane * pop);
            ImFont* font = look().bigFont(px);
            if (!font)
                font = look().font(Font::Strong);
            float const asc = font->Ascent * (px / font->FontSize);  // the baseline, under the text's top
            float const cap = px * kCapHeight;
            ImVec2 const ts = font->CalcTextSizeA(px, FLT_MAX, 0.f, text);
            float const pad = verdictPad(h) * pop;
            // A slider's release says so: "RELEASE" after the word, smaller.
            ImFont* tagFont = nullptr;
            float tagPx = 0.f, tagW = 0.f;
            if (release) {
                tagPx = std::round(h * 0.17f * pop);
                tagFont = look().bigFont(tagPx);
                if (!tagFont)
                    tagFont = look().font(Font::Strong);
                tagW = tagFont->CalcTextSizeA(tagPx, FLT_MAX, 0.f, "RELEASE").x + pad;
            }
            float const pillW = ts.x + tagW + 2.f * pad;
            float const pillH = cap + 2.f * pad;
            float const x0 = std::clamp(hitX - pillW * 0.5f, p0.x, std::max(p0.x, p1.x - pillW));
            float const y0 = below ? p1.y + kVerdictGap : p0.y - kVerdictGap - pillH;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + pillW, y0 + pillH), IM_COL32(0, 0, 0, (int)(165 * alpha)),
                              pillH * 0.5f);
            float const base = y0 + pad + cap;  // where the capitals stand
            outlinedText(dl, font, px, ImVec2(std::round(x0 + pad), std::round(base - asc)), col, alpha, text);
            if (tagFont) {
                float const tasc = tagFont->Ascent * (tagPx / tagFont->FontSize);
                float const tcap = tagPx * kCapHeight;
                outlinedText(dl, tagFont, tagPx,
                             ImVec2(std::round(x0 + pad + ts.x + pad), std::round(base - cap * 0.5f + tcap * 0.5f - tasc)),
                             ImVec4(1.f, 1.f, 1.f, 1.f), alpha, "RELEASE");
            }
        }

        // How fast a lane w x h scrolls: the lead fills it from the marker to
        // its right edge.
        double lanePxPerSec(float w, float h, double tps) {
            double const leadSec = std::max(4, ln::settings().leadTicks) / (tps > 0.0 ? tps : 240.0);
            return std::max(1.0, (double)(w - h * kHitAt - 4.f) / leadSec);
        }

        // The lane over the level: 12% of the screen's height at 1x, so it
        // looks the same at any size; Top sits under GD's progress bar, Bottom
        // over the ground.
        struct LaneRect {
            ImVec2 p0;
            ImVec2 size;
        };
        LaneRect levelRect() {
            ImGuiIO const& io = ImGui::GetIO();
            auto const& set = ln::settings();
            float const h = std::round(std::max(40.f, io.DisplaySize.y * kLaneOfScreen * set.size));
            float const w = std::clamp(io.DisplaySize.x * set.levelWidth, std::min(h * 5.f, io.DisplaySize.x - 16.f),
                                       std::max(260.f, io.DisplaySize.x - 16.f));
            float const y = set.place == ln::Place::Top ? io.DisplaySize.y * 0.075f : io.DisplaySize.y * 0.975f - h;
            return {ImVec2(std::round((io.DisplaySize.x - w) * 0.5f), std::round(y)), ImVec2(w, h)};
        }
        // The level's scroll speed. The card scrolls at it too, so a note is
        // as many pixels ahead on the card as over the level.
        double levelPxPerSec(double tps) {
            auto const r = levelRect();
            return lanePxPerSec(r.size.x, r.size.y, tps);
        }

        // The lane in the rectangle p0..p0+size, scrolling at pxPerSec, its
        // fill `fill` (alpha). The verdict goes under it when `verdictBelow`,
        // else over it.
        void drawLaneBox(Kind k, ImDrawList* dl, ImVec2 p0, ImVec2 size, double pxPerSec, int fill, bool verdictBelow) {
            auto const& set = ln::settings();
            auto const v = ln::view(k);
            // The clock as of now, not the start of the frame: in the level,
            // the tick the game is on as this is drawn.
            double const now = ln::now(k);
            ln::advance(k, now);
            double const realNow = ln::steadyNow();
            auto const& notes = *v.notes;
            auto const& results = *v.results;

            float const w = size.x, h = size.y;
            ImVec2 const p1(p0.x + w, p0.y + h);
            NoteSizes const z = noteSizes(h);
            float const cy = p0.y + h * 0.5f;
            float const hitX = p0.x + h * kHitAt;
            ImVec2 const hc(hitX, cy);
            float const border = std::max(2.f, h * 0.02f);
            float const round = std::min(h * 0.14f, 16.f);
            // Nothing is drawn across the border: a note fades in over the
            // last 6% of the lane before it would touch it, and out at the
            // left end; bars stop at it.
            float const inL = p0.x + border, inR = p1.x - border;
            float const fadeIn = std::max(8.f, w * 0.06f);
            auto edgeFade = [&](float cx, float r) {
                float const right = (inR - (cx + r)) / fadeIn;
                float const left = ((cx - r) - inL) / (fadeIn * 0.5f);
                return std::clamp(std::min(right, left), 0.f, 1.f);
            };
            auto x = [&](double t) { return hitX + (float)((t - now) * pxPerSec); };

            // The box: dark and see-through, the same on every theme, a
            // darker band down its middle for the notes' track.
            dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 14, fill), round);
            dl->AddRectFilled(ImVec2(inL, cy - h * 0.40f), ImVec2(inR, cy + h * 0.40f),
                              IM_COL32(0, 0, 0, fill * 70 / kFillLevel));

            // The hit marker: an empty white ring with dark either side, under
            // the notes (as in taiko, a note sits in it).
            float const mt = std::max(3.f, h * 0.025f);
            dl->AddCircle(hc, z.marker, IM_COL32(0, 0, 0, 170), 0, mt + 3.f);
            dl->AddCircle(hc, z.marker, IM_COL32(255, 255, 255, 245), 0, mt);

            // The notes in view: from the earliest whose hold can still reach
            // the left edge to just past the right one.
            double const leftT = now - (hitX - p0.x + z.three) / pxPerSec;
            double const rightT = now + (p1.x - hitX + z.three) / pxPerSec;
            auto const first = std::lower_bound(notes.begin(), notes.end(), leftT - v.maxHoldSec,
                                                [](ln::Note const& n, double t) { return n.pressSec < t; });
            size_t const from = (size_t)(first - notes.begin());
            size_t to = from;
            while (to < notes.size() && notes[to].pressSec <= rightT)
                ++to;

            // 1. The hold bars.
            for (size_t i = from; i < to; ++i) {
                auto const& n = notes[i];
                if (n.tail == ln::Note::Tail::None || n.releaseSec < leftT)
                    continue;
                auto const& r = results[i];
                float xa = x(n.pressSec);
                float const xb = x(n.releaseSec);
                if (r.pressed)
                    xa = std::max(xa, hitX);  // the part still to hold
                if (r.released || xb <= xa)
                    continue;
                float const ca = std::max(xa, inL), cb = std::min(xb, inR);
                if (cb <= ca)
                    continue;
                bool const held = v.holding == (int)i;
                bool const slider = n.tail == ln::Note::Tail::Slider;
                bool const isFree = n.tail == ln::Note::Tail::FreeHold;
                float const fade = r.missed ? 0.4f : 1.f;
                // A slider (its release timed) is thick; a plain hold thinner;
                // a hold whose release doesn't matter thin and faint. Gold
                // while you hold it.
                float const half = h * (slider ? 0.22f : isFree ? 0.05f : 0.085f);
                float const fillA = (held ? 0.45f : slider ? 0.55f : isFree ? 0.25f : 0.45f) * fade;
                float const edgeA = (held ? 1.f : slider ? 0.6f : isFree ? 0.25f : 0.45f) * fade;
                ImVec4 const body = held ? kWordPerfect : kBarIdle;
                ImVec4 const edge = held ? kWordPerfect : ImVec4(1.f, 1.f, 1.f, 1.f);
                // Round where the bar really ends, square where the border cuts it.
                ImDrawFlags corners = 0;
                if (ca == xa)
                    corners |= ImDrawFlags_RoundCornersLeft;
                if (cb == xb)
                    corners |= ImDrawFlags_RoundCornersRight;
                if (corners == 0)
                    corners = ImDrawFlags_RoundCornersNone;
                dl->AddRectFilled(ImVec2(ca, cy - half), ImVec2(cb, cy + half), u32(body, fillA), half, corners);
                dl->AddRect(ImVec2(ca, cy - half), ImVec2(cb, cy + half), u32(edge, edgeA), half, corners,
                            slider || held ? 2.f : 1.5f);
            }

            // 2. Slider ends (releases), under every press. A press too close
            // to one shrinks it to 60%, so the press's red center shows, and
            // gets a dark gap around it.
            size_t gapHeads[16];
            int gaps = 0;
            for (size_t i = from; i < to; ++i) {
                auto const& n = notes[i];
                auto const& r = results[i];
                if (n.tail != ln::Note::Tail::Slider || r.released)
                    continue;
                float const xb = x(n.releaseSec);
                float const rr = outerRadius(n.releaseTier, z);
                if (xb + rr < inL || xb - rr > inR)
                    continue;
                float scale = 1.f;
                for (size_t j = i; j < notes.size(); ++j) {
                    float const xj = x(notes[j].pressSec);
                    if (xj > xb + rr + z.three)
                        break;
                    if (results[j].pressed || xj < xb - rr - z.three)
                        continue;
                    if (std::fabs(xj - xb) < rr + outerRadius(notes[j].pressTier, z)) {
                        scale = 0.6f;
                        if (gaps < 16)
                            gapHeads[gaps++] = j;
                    }
                }
                float const fade = (r.missed ? 0.32f : 1.f) * edgeFade(xb, rr * scale);
                drawRelease(dl, ImVec2(xb, cy), n.releaseTier, z, n.player2, fade, scale);
            }

            // 3. Presses, latest first so the next to hit is on top; a hit
            // one is gone, as in taiko.
            for (size_t j = to; j-- > from;) {
                auto const& n = notes[j];
                auto const& r = results[j];
                if (r.pressed)
                    continue;
                float const xa = x(n.pressSec);
                float const rr = outerRadius(n.pressTier, z);
                if (xa + rr < inL || xa - rr > inR)
                    continue;
                bool gap = false;
                for (int g = 0; g < gaps; ++g)
                    gap |= gapHeads[g] == j;
                float const fade = (r.missed ? 0.32f : 1.f) * edgeFade(xa, rr);
                drawHead(dl, ImVec2(xa, cy), n.pressTier, z, n.player2, fade, gap ? 2.f : 0.f);
            }

            // The border: light, with a dark line outside it so it shows over
            // a white level too.
            dl->AddRect(ImVec2(p0.x - 1.f, p0.y - 1.f), ImVec2(p1.x + 1.f, p1.y + 1.f), IM_COL32(0, 0, 0, 170),
                        round + 1.f, 0, 1.5f);
            dl->AddRect(ImVec2(p0.x + border * 0.5f, p0.y + border * 0.5f),
                        ImVec2(p1.x - border * 0.5f, p1.y - border * 0.5f), IM_COL32(255, 255, 255, 230), round, 0,
                        border);

            // The pulse: a thick ring in the verdict's color, out from the
            // marker to 1.6x over 0.2 s. The marker itself stays empty.
            float const age = (float)(realNow - v.flash.atReal);
            ImVec4 const vcol = wordColor(v.flash.text);
            if (age >= 0.f && age < 0.2f) {
                float const t = age / 0.2f;
                float const ease = 1.f - (1.f - t) * (1.f - t);
                float const rr = z.marker * (1.f + 0.6f * ease);
                float const th = std::max(4.f, h * 0.05f);
                dl->AddCircle(hc, rr, IM_COL32(0, 0, 0, (int)(170 * (1.f - t))), 0, th + 3.f);
                dl->AddCircle(hc, rr, u32(vcol, 1.f - t * t), 0, th);
            }

            if (set.judgments && !v.flash.text.empty() && age >= 0.f && age < 0.6f)
                drawVerdict(dl, v.flash.text.c_str(), v.flash.release, vcol, age, hitX, p0, p1, h, verdictBelow);
        }

        // The lane's height on the card: as over the level, but no more than
        // 16% of the card's width, so it keeps about the level's proportions
        // and a few notes' room shows between the marker and the next note.
        float cardLaneHeight(float w) {
            return std::round(std::clamp(levelRect().size.y, 48.f, std::max(48.f, w * 0.16f)));
        }

        // The lane on the Click Trainer card: drag it to skim, as the bar.
        void drawLane(Kind k) {
            auto s = tr::state(k);
            auto& ps = page(k);
            bool const following = tr::followsLevel(k);
            float const w = kit::Avail();
            float const h = cardLaneHeight(w);
            double const pxPerSec = levelPxPerSec(ln::view(k).tps);
            ImVec2 const p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##lane", ImVec2(w, h));
            bool const hovered = ImGui::IsItemHovered();
            bool const active = ImGui::IsItemActive();
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
                ImGui::SetTooltip("The lane follows the level while it's open.");
            else if (hovered && !active)
                ImGui::SetTooltip("Drag to skim.");
            drawLaneBox(k, ImGui::GetWindowDrawList(), p0, ImVec2(w, h), pxPerSec, kFillCard, true);
            // Room under the lane for the verdict (and its pop).
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
            ImGui::Dummy(ImVec2(w, kVerdictGap + verdictPillHeight(h) * 1.2f + 4.f));
        }

        // What the colors and sizes mean, under the lane: at the card's body
        // size, in its text color, with swatches drawn as a lane 56 px tall
        // would (a 1-frame note 22 px across, a 3-frame one 43).
        void laneLegend(Kind k) {
            auto const& pal = look().pal;
            auto const v = ln::view(k);
            NoteSizes const z = noteSizes(56.f);
            auto* dl = ImGui::GetWindowDrawList();
            struct Item {
                LTier tier;
                bool release;
                const char* words;
            };
            Item const items[] = {{LTier::One, false, "1 frame"},
                                  {LTier::Two, false, "2 frames"},
                                  {LTier::Three, false, "3 frames"},
                                  {LTier::Wide, false, "4+ or any"},
                                  {LTier::Unknown, false, "not counted"},
                                  {LTier::Two, true, "let go (a slider's end)"}};
            ImVec2 p = ImGui::GetCursorScreenPos();
            float const startX = p.x;
            float const right = p.x + kit::Avail();
            float const lineH = std::max(z.three * 2.f + z.edge * 2.f + 4.f, ImGui::GetTextLineHeight());
            float y = p.y;
            for (auto const& it : items) {
                float const r = outerRadius(it.tier, z) + z.edge;
                ImVec2 const ts = ImGui::CalcTextSize(it.words);
                float const itemW = r * 2.f + 8.f + ts.x;
                if (p.x > startX && p.x + itemW > right) {  // wrap on a narrow card
                    p.x = startX;
                    y += lineH + 4.f;
                }
                ImVec2 const c(p.x + r + 2.f, y + lineH * 0.5f);
                if (it.release)
                    drawRelease(dl, c, it.tier, z, false, 1.f, 1.f);
                else
                    drawHead(dl, c, it.tier, z, false, 1.f, 0.f);
                dl->AddText(ImVec2(c.x + r + 7.f, c.y - ts.y * 0.5f), u32(pal.ink), it.words);
                p.x = c.x + r + 7.f + ts.x + 18.f;
            }
            ImGui::Dummy(ImVec2(std::max(1.f, right - startX), y + lineH - p.y));
            if (v.notes && !v.notes->empty() && v.counted == 0)
                kit::Hint("No windows are counted for this macro yet, so its notes are plain grey and a press is "
                          "judged by the OK range below. Count them on the Calculate page, then press Reload at the "
                          "top.");
        }

        // The box's fill over the level: see-through, and more so (eased)
        // while a player is within 40 px of it (ship, UFO and wave parts reach
        // up there), so the player and the spikes by it still show.
        int levelFill(Kind k, ImVec2 p0, ImVec2 p1) {
            static float eased[2] = {1.f, 1.f};  // 1 = the full fill
            float& e = eased[k == Kind::Jupiter ? 0 : 1];
            bool nearPlayer = false;
            auto* pl = PlayLayer::get();
            auto const win = cocos2d::CCDirector::get()->getWinSize();
            ImVec2 const disp = ImGui::GetIO().DisplaySize;
            if (pl && pl->m_objectLayer && win.width > 0.f && win.height > 0.f) {
                float const sx = disp.x / win.width, sy = disp.y / win.height;
                for (PlayerObject* p : {pl->m_player1, pl->m_player2}) {
                    if (!p || !p->isVisible())
                        continue;
                    cocos2d::CCPoint const at = p->getPosition();
                    cocos2d::CCPoint const a = pl->m_objectLayer->convertToWorldSpace(at);
                    cocos2d::CCPoint const b =
                        pl->m_objectLayer->convertToWorldSpace(at + cocos2d::CCPoint{30.f, 0.f});
                    // About the player's half size on screen (a ship is wider).
                    float const half = 20.f * std::max(0.3f, p->m_vehicleSize) * a.getDistance(b) / 30.f;
                    float const cx = a.x * sx, cyp = (win.height - a.y) * sy;
                    float const hx = half * sx + 40.f, hy = half * sy + 40.f;
                    if (cx + hx > p0.x && cx - hx < p1.x && cyp + hy > p0.y && cyp - hy < p1.y)
                        nearPlayer = true;
                }
            }
            float const dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f);
            e += std::clamp((nearPlayer ? 0.f : 1.f) - e, -dt * 8.f, dt * 8.f);
            return (int)std::round(kFillNearPlayer + (kFillLevel - kFillNearPlayer) * e);
        }

        // The lane over the trainer's level while you play it.
        void laneOverlay(Kind k) {
            auto s = tr::state(k);
            auto const& set = ln::settings();
            if (set.style != ln::Style::Lane || !set.inLevel || !s.barEnabled || s.macro.clickIntervalsSec.empty() ||
                !tr::followsLevel(k) || tr::busy())
                return;
            // On the trainer's own page, open, the card shows it.
            if (s.pageVisible)
                return;
            auto const r = levelRect();
            ImVec2 const p1(r.p0.x + r.size.x, r.p0.y + r.size.y);
            // The background list: over the game, under every window, and it
            // never takes a click from the game. At the top, the verdict goes
            // under the lane; at the bottom, over it.
            drawLaneBox(k, ImGui::GetBackgroundDrawList(), r.p0, r.size, lanePxPerSec(r.size.x, r.size.y, ln::view(k).tps),
                        levelFill(k, r.p0, p1), set.place == ln::Place::Top);
            ln::markShownInLevel(k);
        }
        // ------------------------------------------------------------ cards

        // The lane's own rows: how it scrolls, how big it is, and whether it
        // shows over the level.
        void laneSettings() {
            auto& set = ln::settings();
            bool changed = false;
            changed |= kit::SliderRow("Lead", "How many ticks ahead of the hit marker the lane shows. Fewer is "
                                              "faster scrolling.",
                                      &set.leadTicks, 24, 960, "%d ticks");
            changed |= kit::SliderRow("Lane size", "1x is 12% of the screen's height", &set.size, 0.6f, 2.f, "%.2fx");
            changed |= kit::SwitchRow("Judgments", "PERFECT, EARLY 1f, LATE 2f or MISS under the marker as you hit",
                                      &set.judgments);
            changed |= kit::SwitchRow("Show in the level", "The lane over the trainer's level while you play it, "
                                                            "with the menu closed",
                                      &set.inLevel);
            if (set.inLevel) {
                changed |= kit::SliderRow("Width in the level", nullptr, &set.levelWidth, 0.3f, 1.f, "%.2f");
                int place = (int)set.place;
                const char* const places[] = {"Top", "Bottom"};
                if (kit::DropdownRow("Place in the level", nullptr, &place, places, 2)) {
                    set.place = (ln::Place)place;
                    changed = true;
                }
            }
            if (changed)
                ln::saveSettings();
        }

        void clickTrainerCard(Kind k) {
            auto* gb = GucciEngine::get();
            auto s = tr::state(k);
            bool const following = tr::followsLevel(k);

            // Your presses are white, or the ink color on a light theme; the
            // line at the start of a macro press you answer takes the
            // palette's hit color (drawBar). Only that line: the press's block
            // stays in the accent.
            std::string const caption = fmt::format(
#if defined(GEODE_IS_MOBILE)
                // No keyboard here (iOS doesn't even get the key hook): the
                // card's Tap button stands in for space.
                "The macro's clicks scroll past a fixed center line at real speed. Hold the Tap button under the "
                "bar along with it, or play the level: every press and release you make shows up as a {} line "
                "under the macro's, and each of its presses you answer gets a {} line at its start.",
#else
                "The macro's clicks scroll past a fixed center line at real speed. Tap space, up or W along with "
                "it, or play the level: every press and release you make shows up as a {} line under the macro's, "
                "and each of its presses you answer gets a {} line at its start.",
#endif
                look().pal.light ? "dark" : "white", look().pal.hitName);
            bool const laneCaption = ln::settings().style == ln::Style::Lane;
            kit::BeginCard("Click Trainer",
                           laneCaption ?
#if defined(GEODE_IS_MOBILE)
                                       "The macro's clicks scroll into the hit marker as notes, each sized by how "
                                       "tight Calculate counted its window. Hold the Tap button under it along with "
                                       "them, or play the level: each press is judged as it lands."
#else
                                       "The macro's clicks scroll into the hit marker as notes, each sized by how "
                                       "tight Calculate counted its window. Tap space, up or W along with them, or "
                                       "play the level: each press is judged as it lands."
#endif
                                       : caption.c_str());
            {
                // A cheat only while it follows the trainer's own level.
                kit::LegitGate gate(tr::followsLevel(k));
                if (kit::SwitchRow("Click bar", "Off stops its clock and stops listening for your keys",
                                   &s.barEnabled))
                    tr::saveSettings(k);
            }
            if (!s.barEnabled) {
                kit::EndCard();
                return;
            }
            if (s.macro.clickIntervalsSec.empty()) {
                kit::Note("No clicks to show: the macro isn't loaded.", Tone::Warn);
                kit::EndCard();
                return;
            }

            bool const laneStyle = ln::settings().style == ln::Style::Lane;
            if (laneStyle) {
                drawLane(k);
                kit::Gap(0.2f);
                laneLegend(k);
            } else {
                drawBar(k);
            }
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
#if defined(GEODE_IS_MOBILE)
                // Away from the level only the keyboard reached the bar
                // (keybinds.cpp -> onKeyInput), and a touch on the bar skims:
                // pressed and let go, this is space.
                ImGui::Button("Tap##tapAlong", ImVec2(kit::Avail(), ImGui::GetFrameHeight() * 2.5f));
                if (ImGui::IsItemActivated())
                    tr::onKeyInput(true);
                if (ImGui::IsItemDeactivated())
                    tr::onKeyInput(false);
#endif
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
            {
                int style = (int)ln::settings().style;
                const char* const styles[] = {"Lane", "Classic"};
                if (kit::DropdownRow("Style", "Lane: notes scroll into a hit marker, sized by their window. "
                                              "Classic: the old bar, the macro's clicks over yours.",
                                     &style, styles, 2)) {
                    ln::settings().style = (ln::Style)style;
                    ln::saveSettings();
                }
            }
            if (laneStyle)
                laneSettings();
            else if (kit::SliderRow("Window", "How much of the macro the bar shows at once", &s.barWindowSec, 0.5f,
                                    10.f, "%.1f s"))
                tr::saveSettings(k);
            // Click Between Frames sits out while the bot records or plays
            // (GucciEngine::syncInputMods), so a CBF player trains without it
            // then: the trainer lane's measurer found that's the likeliest
            // "my clicks feel late".
            if (!gb->inputModsPaused.empty())
                kit::Note(fmt::format("{} is paused while the bot is on Record or Play, so your presses land on "
                                      "tick edges. Set the bot to Idle to train with it.",
                                      gb->inputModsPaused)
                              .c_str(),
                          Tone::Warn);

            kit::Section("Your timing");
            if (laneStyle) {
                auto const v = ln::view(k);
                kit::Chip(fmt::format("On the frame {}", v.tally.perfect).c_str(), Tone::Good);
                ImGui::SameLine();
                kit::Chip(fmt::format("In the window {}", v.tally.inside).c_str(), Tone::Warn);
                ImGui::SameLine();
                kit::Chip(fmt::format("Outside {}", v.tally.outside).c_str(), Tone::Bad);
                ImGui::SameLine();
                kit::Chip(fmt::format("Missed {}", v.tally.missed).c_str(), Tone::Muted);
                kit::Hint("A press is judged against the note it lands nearest, in ticks off the macro's frame. In "
                          "the window means the count says it still lives there. A slider's release is judged too. "
                          "Notes with no counted window use the OK range below. The tally starts over with each "
                          "attempt.");
                bool okChanged =
                    kit::SliderRow("OK within", "For notes with no counted window", &gb->clickIndicatorOkMs, 1.f,
                                   200.f, "%.0f ms");
                if (okChanged) {
                    gb->clickIndicatorOkMs = std::max(gb->clickIndicatorOkMs, gb->clickIndicatorPerfectMs);
                    tr::saveScoreWindows();
                }
                kit::EndCard();
                return;
            }
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
            {
                kit::LegitGate gate;
                if (kit::SwitchRow("Macro ghost", "Where the macro's player is on your frame", &s.ghost))
                    tr::saveSettings(k);
                if (kit::SwitchRow("Best-attempt ghost", "Your furthest run this session, replayed alongside you",
                                   &s.bestGhost))
                    tr::saveSettings(k);
            }
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
                kit::Chip(clicksChip(s.macro.clickIntervalsSec.size()).c_str(), Tone::Accent);
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

            // One entry per file, tagged with its theme as the Saved macros
            // list is (Compact Mode's picker does the same): a name saved
            // under two themes is two entries, each loading its own file.
            if (std::exchange(g_relist, false))
                gb->reloadMacroList();
            auto const& files = gb->storedMacroFiles;
            auto const label = [](std::string const& name, std::string const& ext) {
                std::string const tag = ui::savedMacroTag(ext);
                return tag.empty() ? name : fmt::format("{} ({})", name, tag);
            };
            std::vector<std::string> labels;
            labels.reserve(files.size());
            for (auto const& [name, ext] : files)
                labels.push_back(label(name, ext));
            auto const indexOf = [&](std::pair<std::string, std::string> const& f) {
                auto it = std::find(files.begin(), files.end(), f);
                return it == files.end() ? -1 : (int)(it - files.begin());
            };
            // A load can end on another file than the one picked: with the
            // picked file gone (converted to another theme), loadTrainerMacro
            // falls back to the name alone. The list still holds the gone
            // file then, so it is read again and the dropdown moves to the
            // file that loaded.
            // (Read again at the top of the next frame: this one's labels are
            // already built from the list.)
            auto const afterLoad = [&] {
                g_pickFile = {gb->trainerMacroName, gb->trainerMacroExt};
                g_pick = indexOf(g_pickFile);
                g_relist = g_pick < 0;
            };
            if (g_pick < 0 || g_pick >= (int)files.size() || files[(size_t)g_pick] != g_pickFile) {
                int const kept = g_pickFile.first.empty() ? -1 : indexOf(g_pickFile);
                g_pick = kept >= 0 ? kept : indexOf({gb->trainerMacroName, gb->trainerMacroExt});
                g_pickFile = g_pick >= 0 ? files[(size_t)g_pick] : std::pair<std::string, std::string>{};
            }
            kit::RowBegin("Macro", files.empty() ? "No saved macros yet" : nullptr);
            if (kit::Button("Load", Tone::Accent) && g_pick >= 0 && g_pick < (int)files.size()) {
                auto const [stem, ext] = files[(size_t)g_pick];
                bool const ok = tr::selectMacro(stem, ext);
                g_macroMsg = ok ? Message{}
                                : Message{fmt::format("'{}' didn't load: it has no click pairs, or it isn't a "
                                                      "format the Trainer reads (GucciBot macros are).",
                                                      label(stem, ext)),
                                          Tone::Bad};
                if (ok)
                    afterLoad();
            }
            ImGui::SameLine();
            if (kit::Button("Refresh"))
                gb->reloadMacroList();
            ImGui::SameLine();
            if (kit::Dropdown("macro", &g_pick, labels) && g_pick >= 0 && g_pick < (int)files.size())
                g_pickFile = files[(size_t)g_pick];
            kit::RowEnd();

            if (s.macro.loaded) {
                kit::RowBegin("Loaded", nullptr);
                kit::Label(label(gb->trainerMacroName, gb->trainerMacroExt).c_str());
                ImGui::SameLine();
                if (kit::Button("Reload")) {
                    if (!tr::selectMacro(gb->trainerMacroName, gb->trainerMacroExt))
                        g_macroMsg = {"It didn't load again; the file may have moved.", Tone::Bad};
                    else
                        afterLoad();
                }
                kit::RowEnd();
                kit::Chip(clicksChip(s.macro.clickIntervalsSec.size()).c_str(), Tone::Accent);
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
            else
                trainerVideoModeCard();
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
        // jmfing"): deep indigo cards, the level's yellow and lavender text,
        // over the star chart at night (pages/jupiter_backdrop.cpp). B's
        // colours (2026-11-02): the yellow is never faded, since a see-through
        // yellow over this indigo turns grey (they're complementary); the
        // quiet parts are blues instead, and the selected page is lit.
        ImVec4 const kJmfYellow{0.988f, 0.961f, 0.314f, 1.f};  // #FCF550
        ImVec4 const kJmfSky{0.027f, 0.016f, 0.247f, 1.f};     // #07043F
        ImVec4 const kJmfCard{0.051f, 0.027f, 0.412f, 1.f};    // #0D0769
        ImVec4 const kJmfRaised{0.102f, 0.055f, 0.557f, 1.f};  // #1A0E8E, fields and buttons
        ImVec4 const kJmfHover{0.133f, 0.078f, 0.620f, 1.f};   // #22149E
        ImVec4 const kJmfPress{0.165f, 0.102f, 0.682f, 1.f};   // #2A1AAE
        ImVec4 const kJmfLine{0.184f, 0.149f, 0.600f, 1.f};    // #2F2699, borders and dividers
        ImVec4 const kJmfLift{0.298f, 0.255f, 0.722f, 1.f};    // #4C41B8, the scroll grab, a selected row
        ImVec4 const kJmfTint{0.102f, 0.055f, 0.525f, 1.f};    // #1A0E86, chips, notes, a disabled button
        ImVec4 const kJmfLavender{0.714f, 0.690f, 0.910f, 1.f}; // #B6B0E8

        Palette g_jmfSaved;
        int g_jmfPushed = 0;

        // The JMF look: the palette the kit reads (look().pal), and through
        // `set(slot, colour)` the style colours ImGui's own widgets and the
        // window read, the window's sky at `opacity`. jupiterLookPush pushes
        // them for the JMF page; jupiterLookApply writes them over refresh()'s
        // for JupiterBot. The yellow parts are `accent`: the JMF page's own
        // yellow, or JupiterBot's as refresh() worked it out (the same yellow,
        // turning with Cycle the accent color).
        template <class Set>
        void jmfLook(Palette& p, float opacity, ImVec4 const& accent, Set&& set) {
            auto alpha = [](ImVec4 v, float a) { return ImVec4(v.x, v.y, v.z, a); };
            setLight(p, false);  // JMF's indigo is dark under a light theme too
            p.accent = accent;
            p.onAccent =
                luminance(accent) > 0.55f ? ImVec4(0.06f, 0.03f, 0.25f, 1.f) : ImVec4(0.97f, 0.97f, 0.97f, 1.f);
            p.surface = kJmfCard;
            p.raised = kJmfRaised;
            p.ink = ImVec4(0.957f, 0.949f, 1.f, 1.f);
            p.muted = kJmfLavender;
            p.line = kJmfLine;
            p.solidTints = true;  // the kit's chips, notes and disabled buttons, the lit page, the bar's blocks
            p.tint = kJmfTint;
            p.cardEdge.on = false;   // a scene's own touches (SolarBot's card edges and page frame) stay off it
            p.pageFrame.on = false;
            setHit(p);  // against the JMF accent and cards, not the theme's
            set(ImGuiCol_Text, p.ink);
            set(ImGuiCol_TextDisabled, p.muted);
            set(ImGuiCol_Border, p.line);
            set(ImGuiCol_FrameBg, kJmfRaised);
            set(ImGuiCol_FrameBgHovered, kJmfHover);
            set(ImGuiCol_FrameBgActive, kJmfPress);
            set(ImGuiCol_Button, kJmfRaised);
            set(ImGuiCol_ButtonHovered, kJmfHover);
            set(ImGuiCol_ButtonActive, kJmfPress);
            // A list's selected row (a dropdown's), hovered and held: blues,
            // under the list's own light text.
            set(ImGuiCol_Header, kJmfLine);
            set(ImGuiCol_HeaderHovered, kJmfPress);
            set(ImGuiCol_HeaderActive, kJmfLift);
            set(ImGuiCol_CheckMark, accent);
            set(ImGuiCol_SliderGrab, accent);
            set(ImGuiCol_SliderGrabActive, accent);
            // Solid, so it reads over the art.
            set(ImGuiCol_ScrollbarGrab, kJmfLift);
            set(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.353f, 0.310f, 0.784f, 1.f));  // #5A4FC8
            set(ImGuiCol_ScrollbarGrabActive, accent);
            set(ImGuiCol_PlotHistogram, accent);
            set(ImGuiCol_WindowBg, alpha(kJmfSky, opacity));
            set(ImGuiCol_PopupBg, kJmfCard);
            set(ImGuiCol_Tab, kJmfRaised);
            set(ImGuiCol_TabHovered, kJmfPress);
            set(ImGuiCol_TabActive, kJmfLift);
            set(ImGuiCol_Separator, kJmfLine);
            set(ImGuiCol_SeparatorHovered, kJmfLift);
            set(ImGuiCol_SeparatorActive, accent);
            set(ImGuiCol_ResizeGrip, kJmfLine);
            set(ImGuiCol_ResizeGripHovered, kJmfLift);
            set(ImGuiCol_ResizeGripActive, accent);
            set(ImGuiCol_TextSelectedBg, kJmfLift);
            set(ImGuiCol_TableBorderStrong, kJmfLine);
            set(ImGuiCol_TableBorderLight, kJmfLine);
            p.backdrop = alpha(kJmfSky, opacity);
        }
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
        jmfLook(look().pal, g_jmfSaved.opacity, kJmfYellow, [](ImGuiCol c, ImVec4 v) {
            ImGui::PushStyleColor(c, v);
            ++g_jmfPushed;
        });
    }

    // JupiterBot: the same look on every window, written straight over the
    // style rather than pushed, after refresh() has worked the theme's out.
    // refresh() rewrites every one of these slots each frame, so switching
    // away leaves nothing behind, and the JMF page's push on top of it is the
    // same colours again. Its yellow is refresh()'s accent, so Cycle the
    // accent color turns it (on the JMF page itself it stays the level's).
    void jupiterLookApply() {
        ImVec4 const accent = look().pal.accent;  // copied: jmfLook writes pal.accent
        jmfLook(look().pal, look().pal.opacity, accent,
                [](ImGuiCol c, ImVec4 v) { ImGui::GetStyle().Colors[c] = v; });
    }

    void jupiterLookPop() {
        if (g_jmfPushed == 0)
            return;
        ImGui::PopStyleColor(g_jmfPushed);
        g_jmfPushed = 0;
        look().pal = g_jmfSaved;
    }

    void jupiterTrainer() {
        tr::ensureLoaded();
        // The search's hidden index pass draws this page too: that isn't the
        // page being open (its bar's clock and music run while it is).
        if (!search::indexing())
            tr::markPageDrawn(Kind::Jupiter);
        jupiterHeader();
        body(Kind::Jupiter);
    }

    namespace {
        // Video Mode: in place of the menu, the trainer's click bar over the
        // bottom of the video (Nigel: "i still see the whole menu in the middle
        // instead of an overlay of the bar thing"), with its controls and the way
        // out. The page counts as open, so the bar's clock and its music carry on.
        void videoBar(Kind k) {
            tr::ensureLoaded();
            tr::markPageDrawn(k);
            auto* gb = GucciEngine::get();
            auto s = tr::state(k);
            ImGuiIO const& io = ImGui::GetIO();
            float const w = std::min(io.DisplaySize.x - 40.f, 960.f);
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 20.f), ImGuiCond_Always,
                                    ImVec2(0.5f, 1.f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, FLT_MAX));
            ImGui::SetNextWindowBgAlpha(overLevelAlpha(0.6f));
            if (ImGui::Begin(k == Kind::Jupiter ? "##jmfvideobar" : "##trainervideobar", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing)) {
                if (s.macro.clickIntervalsSec.empty())
                    kit::Note(k == Kind::Jupiter ? "No clicks to show: the JMF macro isn't loaded."
                                                 : "No clicks to show: load a macro on the Trainer page.",
                              Tone::Warn);
                else
                    drawBar(k);
                // While its level is open the bar follows the level (tick()
                // holds it paused at the level's time): Pause did nothing and
                // Restart only wiped your clicks, as on the Click Trainer card,
                // which hides them then.
                if (tr::followsLevel(k)) {
                    kit::Label("Following the level", Tone::Muted);
                } else {
                    if (kit::Button(s.paused ? "Resume" : "Pause", Tone::Accent))
                        tr::setPaused(k, !s.paused);
                    ImGui::SameLine();
                    if (kit::Button("Restart"))
                        tr::restart(k);
                }
                ImGui::SameLine();
                if (kit::Button("Exit Video Mode", Tone::Muted)) {
                    gb->jupiterVideoModeEnabled = false;
                    gb->trainerVideoModeEnabled = false;
                    gb->jupiterVideoAlignToolActive = false;
                }
            }
            ImGui::End();
        }
    } // namespace

    void jupiterVideoOverlay() {
        jupiterLookPush();
        videoBar(Kind::Jupiter);
        jupiterLookPop();
    }

    void trainerVideoOverlay() {
        videoBar(Kind::Any);
    }

    // The trainer lane over its level, menu open or not (ui/shell.cpp).
    void trainerLaneOverlay() {
        laneOverlay(Kind::Jupiter);
        laneOverlay(Kind::Any);
    }

    void anyTrainer() {
        tr::ensureLoaded();
        // The search's hidden index pass draws this page too: that isn't the
        // page being open (its bar's clock and music run while it is).
        if (!search::indexing())
            tr::markPageDrawn(Kind::Any);
        anyHeader();
        body(Kind::Any);
    }

} // namespace gucci::ui::pages
