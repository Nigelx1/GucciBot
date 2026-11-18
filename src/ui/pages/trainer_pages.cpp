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
        // The hold bars: idle see-through grey; held, solid gold with a
        // light gold edge (the PERFECT and pulse color, not a muddy mix).
        ImVec4 const kBarIdle{0.588f, 0.612f, 0.675f, 1.f};   // rgb(150,156,172)
        ImVec4 const kBarHeld{1.f, 0.702f, 0.f, 1.f};         // #FFB300
        ImVec4 const kBarHeldEdge{1.f, 0.878f, 0.510f, 1.f};  // #FFE082

        // The lane's height: 12% of the screen's at 1x (about 130 px at 1080p).
        constexpr float kLaneOfScreen = 0.12f;
        // The hit marker's center, this many lane heights from the left edge.
        constexpr float kHitAt = 1.0f;
        // The verdict's font, as a share of the lane's height, and Roboto
        // Bold's capital height per font pixel (1456 / (2146 + 555) units:
        // ImGui sizes a font by ascent + descent, not the em). Capitals come
        // to 0.74 x 0.539 = 40% of the lane's height.
        constexpr float kVerdictOfLane = 0.74f;
        constexpr float kCapHeight = 0.539f;
        // A slider's "RELEASE" line under the word: half the word's size.
        constexpr float kTagOfVerdict = 0.5f;
        // How much bigger the verdict starts as it pops in.
        constexpr float kPop = 1.08f;
        // Over the level the verdict sits left of the box, this far from it;
        // on the card, under the lane this far.
        constexpr float kVerdictSideGap = 12.f;
        constexpr float kVerdictGap = 4.f;
        // The box's fill over the level, and while the player is near it.
        constexpr int kFillLevel = 165;
        constexpr int kFillBottomOverPlay = 217;  // 85%
        constexpr int kFillNearPlayer = 90;
        constexpr int kFillCard = 235;
        // The whole lane's opacity while a player is inside its band.
        constexpr float kInBandAlpha = 0.3f;

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
        // dark outline outside its edge.
        void drawHead(ImDrawList* dl, ImVec2 c, LTier tier, NoteSizes const& z, bool p2, float fade,
                      float radius = 0.f) {
            if (fade <= 0.01f)
                return;
            float const ew = z.edge;
            float const r = radius > 0.f ? radius : outerRadius(tier, z);
            dl->AddCircleFilled(c, r + ew, darkEdge(fade));
            dl->AddCircleFilled(c, r, u32(tierColor(tier), fade));
            if (tier != LTier::Unknown && tier != LTier::One) {
                float const cr = std::min(z.center, r * 0.6f);
                dl->AddCircleFilled(c, cr + ew * 0.8f, darkEdge(fade));
                dl->AddCircleFilled(c, cr, u32(kNoteRed, fade));
            }
            if (p2)
                dl->AddCircle(c, r + ew * 1.9f, u32(kNoteP2, fade), 0, ew * 1.5f);
        }

        // A release (a slider's end): a ring with no fill in its window's
        // color at its tier's press size (1 frame 40% of the lane, 2 frames
        // 58%, 3+ 76%: bigger = more lenient, as for presses), its stroke 11%
        // of the lane inside that, a dark line either side and a white
        // upright line through the hole. A filled bullseye always means
        // press; an empty ring, let go.
        void drawRelease(ImDrawList* dl, ImVec2 c, LTier tier, NoteSizes const& z, float h, bool p2, float fade) {
            if (fade <= 0.01f)
                return;
            float const ew = z.edge;
            float const r = outerRadius(tier, z);
            float const t = std::max(4.f, h * 0.11f);
            float const mid = r - t * 0.5f;
            dl->AddCircle(c, mid, darkEdge(fade), 0, t + 2.f * ew);
            dl->AddCircle(c, mid, u32(tierColor(tier), fade), 0, t);
            float const lw = 3.f;
            float const lh = std::max(3.f, (r - t - ew) * 0.85f);
            dl->AddRectFilled(ImVec2(c.x - lw * 0.5f - 1.f, c.y - lh - 1.f), ImVec2(c.x + lw * 0.5f + 1.f, c.y + lh + 1.f),
                              darkEdge(fade), 2.f);
            dl->AddRectFilled(ImVec2(c.x - lw * 0.5f, c.y - lh), ImVec2(c.x + lw * 0.5f, c.y + lh),
                              IM_COL32(255, 255, 255, (int)(245 * fade)), 1.5f);
            if (p2)
                dl->AddCircle(c, r + ew * 1.9f, u32(kNoteP2, fade), 0, ew * 1.5f);
        }

        // A release right by a press (they'd touch or overlap) is drawn as a
        // ring around that press instead, centered on it: its stroke just
        // outside the press's dark outline, so the press stays whole.
        // ringStroke: the colored band's width; the press inside is drawn
        // after it (on top).
        float ringStroke(float h) {
            return std::max(4.f, std::round(h * 0.032f));
        }
        // The press's radius inside such a ring, so the ring's outside
        // stays within 80% of the lane's height (a 3-frame press shrinks a
        // little; smaller ones keep their size).
        float ringedPressRadius(float pressR, NoteSizes const& z, float h) {
            return std::min(pressR, h * 0.40f - ringStroke(h) - z.edge * 1.7f);
        }
        void drawReleaseAround(ImDrawList* dl, ImVec2 c, LTier tier, float pressR, NoteSizes const& z, float h,
                               float fade) {
            if (fade <= 0.01f)
                return;
            float const ew = z.edge;
            float const t = ringStroke(h);
            float const mid = pressR + ew + t * 0.5f;
            dl->AddCircle(c, mid, darkEdge(fade), 0, t + 1.4f * ew);
            dl->AddCircle(c, mid, u32(tierColor(tier), fade), 0, t);
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

        ImFont* wordFont(float px) {
            ImFont* f = look().bigFont(px);
            return f ? f : look().font(Font::Strong);
        }

        // The verdict's pill: its padding, and its height for a lane h tall
        // with or without the RELEASE line (the card keeps that much room
        // under its lane).
        float verdictPad(float h) {
            return std::max(6.f, std::round(h * 0.07f));
        }
        float verdictBlockHeight(float px, bool release) {
            float const cap = px * kCapHeight;
            return release ? cap * (1.f + 0.28f + kTagOfVerdict) : cap;
        }
        // The widest word the verdict often shows, as a share of its font
        // size ("EARLY 3f" in Roboto Bold is 3.2 font pixels per pixel).
        constexpr float kWideWord = 3.2f;
        float verdictReserve(float h) {
            return kPop * (kWideWord * h * kVerdictOfLane + 2.f * verdictPad(h));
        }

        // Where the verdict goes: over the level, left of the box, its right
        // edge `rightX`, centered on the lane's middle `midY` (in the lane's
        // own band, so it never reaches into the play field); on the card,
        // centered under the marker at `hitX`, its top at `topY`.
        struct VerdictSpot {
            bool side = true;
            float rightX = 0.f, midY = 0.f;  // side
            float hitX = 0.f, topY = 0.f;    // under
            float minX = 0.f, maxX = 0.f;    // keep the pill between these
            ImU32 pill = 0;                  // its fill at full alpha
        };

        // The verdict, big, on a pill: the word, and a slider's release says
        // so on a line under it at half size. It pops in and fades over 0.6 s.
        void drawVerdict(ImDrawList* dl, char const* text, bool release, ImVec4 col, float age, float h,
                         VerdictSpot const& at) {
            float const pop = age < 0.07f ? kPop - (kPop - 1.f) * (age / 0.07f) : 1.f;
            float const alpha = age < 0.35f ? 1.f : std::max(0.f, 1.f - (age - 0.35f) / 0.25f);
            float px = h * kVerdictOfLane;
            float const pad0 = verdictPad(h);
            // Fit the room (a long word like EARLY 12f shrinks a little).
            {
                ImFont* f = wordFont(px);
                float const tw = f->CalcTextSizeA(px, FLT_MAX, 0.f, text).x;
                float const room = (at.maxX - at.minX) / kPop - 2.f * pad0;  // popped, it still fits
                if (tw > room && tw > 0.f)
                    px *= std::max(0.5f, room / tw);
            }
            px = std::round(px * pop);
            ImFont* font = wordFont(px);
            float const asc = font->Ascent * (px / font->FontSize);  // the baseline, under the text's top
            float const cap = px * kCapHeight;
            ImVec2 const ts = font->CalcTextSizeA(px, FLT_MAX, 0.f, text);
            float const pad = pad0 * pop;
            float tagPx = 0.f, tagW = 0.f;
            ImFont* tagFont = nullptr;
            if (release) {
                tagPx = std::round(px * kTagOfVerdict);
                tagFont = wordFont(tagPx);
                tagW = tagFont->CalcTextSizeA(tagPx, FLT_MAX, 0.f, "RELEASE").x;
            }
            float const pillW = std::max(ts.x, tagW) + 2.f * pad;
            float const pillH = verdictBlockHeight(px, release) + 2.f * pad;
            float x0, y0;
            if (at.side) {
                x0 = at.rightX - pillW;
                y0 = at.midY - pillH * 0.5f;
            } else {
                x0 = at.hitX - pillW * 0.5f;
                y0 = at.topY;
            }
            x0 = std::clamp(x0, at.minX, std::max(at.minX, at.maxX - pillW));
            ImU32 const pill = (at.pill & ~IM_COL32_A_MASK) |
                               ((ImU32)(((at.pill >> IM_COL32_A_SHIFT) & 0xFF) * alpha) << IM_COL32_A_SHIFT);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + pillW, y0 + pillH), pill, std::min(pillH * 0.5f, 22.f));
            float const cx = x0 + pillW * 0.5f;
            float const base = y0 + pad + cap;  // where the word's capitals stand
            outlinedText(dl, font, px, ImVec2(std::round(cx - ts.x * 0.5f), std::round(base - asc)), col, alpha, text);
            if (tagFont) {
                float const tasc = tagFont->Ascent * (tagPx / tagFont->FontSize);
                float const tbase = base + cap * 0.28f + tagPx * kCapHeight;
                outlinedText(dl, tagFont, tagPx, ImVec2(std::round(cx - tagW * 0.5f), std::round(tbase - tasc)),
                             ImVec4(1.f, 1.f, 1.f, 0.92f), alpha, "RELEASE");
            }
        }

        // How fast a lane w x h scrolls: the lead fills it from the marker to
        // its right edge.
        double lanePxPerSec(float w, float h, double tps) {
            double const leadSec = std::max(4, ln::settings().leadTicks) / (tps > 0.0 ? tps : 240.0);
            return std::max(1.0, (double)(w - h * kHitAt - 4.f) / leadSec);
        }

        // The lane over the level: 12% of the screen's height at 1x, so it
        // looks the same at any size; its width a share of the screen's,
        // centered, nudged right if the verdict needs more room on its left.
        // Its top: under GD's progress bar (Top) or over the ground (Bottom).
        struct LaneGeom {
            float x = 0.f, w = 0.f, h = 0.f;
            float topY = 0.f, bottomY = 0.f;
        };
        LaneGeom levelGeom() {
            ImGuiIO const& io = ImGui::GetIO();
            auto const& set = ln::settings();
            LaneGeom g;
            g.h = std::round(std::max(40.f, io.DisplaySize.y * kLaneOfScreen * set.size));
            g.w = std::clamp(io.DisplaySize.x * set.levelWidth, std::min(g.h * 5.f, io.DisplaySize.x - 16.f),
                             std::max(260.f, io.DisplaySize.x - 16.f));
            float const reserve = 8.f + verdictReserve(g.h) + kVerdictSideGap;
            g.x = std::max((io.DisplaySize.x - g.w) * 0.5f, set.judgments ? reserve : 0.f);
            g.w = std::max(g.h * 4.f, std::min(g.w, io.DisplaySize.x - 16.f - g.x));
            g.x = std::round(g.x);
            g.topY = std::round(io.DisplaySize.y * 0.075f);
            g.bottomY = std::round(io.DisplaySize.y * 0.975f - g.h);
            return g;
        }
        // The level's scroll speed. The card scrolls at it too, so a note is
        // as many pixels ahead on the card as over the level.
        double levelPxPerSec(double tps) {
            auto const g = levelGeom();
            return lanePxPerSec(g.w, g.h, tps);
        }

        // The lane in the rectangle p0..p0+size, scrolling at pxPerSec, its
        // fill `fill` (alpha), the verdict where `spot` says (its x and y
        // filled in here from the box).
        // Returns the first vertex of the verdict (or the list's end if none
        // was drawn), so the overlay can fade the box and the verdict apart.
        int drawLaneBox(Kind k, ImDrawList* dl, ImVec2 p0, ImVec2 size, double pxPerSec, int fill, VerdictSpot spot) {
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
            // Nothing is drawn across the border: every note, ring and bar
            // vertex is kept inside the box's inner edge (3 px inside the
            // border), and they fade out together over the last 5% of the
            // lane, to nothing at that edge (one pass over their vertices,
            // below); notes fade at the left end too.
            float const inL = p0.x + border + 3.f, inR = p1.x - border - 3.f;
            float const inT = p0.y + border + 1.f, inB = p1.y - border - 1.f;
            float const fadeIn = std::max(10.f, w * 0.05f);
            float const fadeFrom = inR - fadeIn;
            auto leftFade = [&](float cx, float r) { return std::clamp(((cx - r) - inL) / (fadeIn * 0.5f), 0.f, 1.f); };
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

            // Each slider end close to a press (they'd touch, or come within
            // 6 px of each other) is drawn as a ring around that press,
            // centered on it; its bar runs to that press. Farther apart, the
            // two are separate shapes with a gap of 6 px or more. Reused
            // buffers: no allocation per frame once they've grown.
            static std::vector<int> s_ringOn;  // per note in view: the press its release rings, or -1
            static std::vector<int> s_ringBy;  // per note in view: the release ringing its press, or -1
            size_t const count = to - from;
            s_ringOn.assign(count, -1);
            s_ringBy.assign(count, -1);
            float const sep = 2.f * z.edge + 6.f;
            for (size_t i = from; i < to; ++i) {
                auto const& n = notes[i];
                if (n.tail != ln::Note::Tail::Slider || results[i].released)
                    continue;
                float const xb = x(n.releaseSec);
                float const rr = outerRadius(n.releaseTier, z);
                float best = FLT_MAX;
                int pick = -1;
                for (size_t j = from; j < to; ++j) {
                    if (results[j].pressed || s_ringBy[j - from] >= 0)
                        continue;
                    float const dx = std::fabs(x(notes[j].pressSec) - xb);
                    if (dx < rr + outerRadius(notes[j].pressTier, z) + sep && dx < best) {
                        best = dx;
                        pick = (int)j;
                    }
                }
                if (pick >= 0) {
                    s_ringOn[i - from] = pick;
                    s_ringBy[(size_t)pick - from] = (int)i;
                }
            }

            int const fadeVtx = dl->VtxBuffer.Size;  // bars and notes, kept inside and faded below

            // 1. The hold bars. Each is split where the right end's fade
            // starts, so the fade has a vertex to start from, and its edge is
            // drawn as lines and caps (no seam across it there).
            auto barSpan = [&](float a, float b, float half, ImU32 body, ImU32 edge, float edgeW, bool roundL,
                               bool roundR) {
                ImDrawFlags corners = (roundL ? ImDrawFlags_RoundCornersLeft : 0) |
                                      (roundR ? ImDrawFlags_RoundCornersRight : 0);
                if (corners == 0)
                    corners = ImDrawFlags_RoundCornersNone;
                float const rnd = std::min(half, std::max(0.f, (b - a) * (roundL && roundR ? 0.5f : 1.f)));
                dl->AddRectFilled(ImVec2(a, cy - half), ImVec2(b, cy + half), body, rnd, corners);
                float const la = roundL ? a + rnd : a, lb = roundR ? b - rnd : b;
                if (lb > la) {
                    dl->AddLine(ImVec2(la, cy - half), ImVec2(lb, cy - half), edge, edgeW);
                    dl->AddLine(ImVec2(la, cy + half), ImVec2(lb, cy + half), edge, edgeW);
                }
                if (roundL && rnd >= half - 0.5f) {
                    dl->PathArcTo(ImVec2(a + half, cy), half, 1.5707963f, 4.7123890f);
                    dl->PathStroke(edge, 0, edgeW);
                }
                if (roundR && rnd >= half - 0.5f) {
                    dl->PathArcTo(ImVec2(b - half, cy), half, -1.5707963f, 1.5707963f);
                    dl->PathStroke(edge, 0, edgeW);
                }
            };
            for (size_t i = from; i < to; ++i) {
                auto const& n = notes[i];
                if (n.tail == ln::Note::Tail::None || n.releaseSec < leftT)
                    continue;
                auto const& r = results[i];
                float xa = x(n.pressSec);
                int const ringOn = s_ringOn[i - from];
                float const xb = ringOn >= 0 ? x(notes[(size_t)ringOn].pressSec) : x(n.releaseSec);
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
                // a hold whose release doesn't matter thin and faint. Solid
                // gold while you hold it.
                float const half = std::round(h * (slider ? 0.22f : isFree ? 0.05f : 0.085f));
                float const fillA = (held ? 0.9f : slider ? 0.55f : isFree ? 0.25f : 0.45f) * fade;
                float const edgeA = (held ? 1.f : slider ? 0.6f : isFree ? 0.25f : 0.45f) * fade;
                ImU32 const body = u32(held ? kBarHeld : kBarIdle, fillA);
                ImU32 const edge = u32(held ? kBarHeldEdge : ImVec4(1.f, 1.f, 1.f, 1.f), edgeA);
                float const ew = slider || held ? 2.f : 1.5f;
                bool const rl = ca == xa, rr = cb == xb;
                if (cb <= fadeFrom || ca >= fadeFrom) {
                    barSpan(ca, cb, half, body, edge, ew, rl, rr);
                } else {
                    barSpan(ca, fadeFrom, half, body, edge, ew, rl, false);
                    barSpan(fadeFrom, cb, half, body, edge, ew, false, rr);
                }
            }

            // The pulse: a thick ring in the verdict's color, out from the
            // marker (1.0x) to 1.25x over 0.2 s, inside the border, and for
            // the first 0.1 s the marker's inside lit with that color at 25%.
            // Over the bars, under every release and press: the next note
            // coming in stays whole on top of it.
            float const age = (float)(realNow - v.flash.atReal);
            ImVec4 const vcol = wordColor(v.flash.text);
            if (age >= 0.f && age < 0.2f) {
                float const t = age / 0.2f;
                float const ease = 1.f - (1.f - t) * (1.f - t);
                float const th = std::max(6.f, std::round(h * 0.048f));
                float const maxR = h * 0.5f - border - th * 0.5f - 1.5f;  // its dark ring's outside, inside the border
                float const rr = std::min(z.marker * (1.f + 0.25f * ease), maxR);
                if (age < 0.1f)
                    dl->AddCircleFilled(hc, z.marker - mt * 0.5f, u32(vcol, 0.25f));
                dl->AddCircle(hc, rr, IM_COL32(0, 0, 0, (int)(170 * (1.f - t))), 0, th + 3.f);
                dl->AddCircle(hc, rr, u32(vcol, 1.f - t * t), 0, th);
            }

            // 2. Slider ends on their own, at their tier's size, over the
            // bars and under every press.
            for (size_t i = from; i < to; ++i) {
                auto const& n = notes[i];
                auto const& r = results[i];
                if (n.tail != ln::Note::Tail::Slider || r.released || s_ringOn[i - from] >= 0)
                    continue;
                float const xb = x(n.releaseSec);
                float const rr = outerRadius(n.releaseTier, z);
                if (xb + rr < inL || xb - rr > inR)
                    continue;
                float const fade = (r.missed ? 0.32f : 1.f) * leftFade(xb, rr);
                drawRelease(dl, ImVec2(xb, cy), n.releaseTier, z, h, n.player2, fade);
            }

            // 3. Presses, latest first so the next to hit is on top; a hit
            // one is gone, as in taiko. A press with a release ringing it
            // gets that ring first, then the press (whole, with its outline)
            // over it.
            for (size_t j = to; j-- > from;) {
                auto const& n = notes[j];
                auto const& r = results[j];
                if (r.pressed)
                    continue;
                float const xa = x(n.pressSec);
                float rr = outerRadius(n.pressTier, z);
                int const by = s_ringBy[j - from];
                float const reach = by >= 0 ? h * 0.40f : rr;
                if (xa + reach < inL || xa - reach > inR)
                    continue;
                float const fade = (r.missed ? 0.32f : 1.f) * leftFade(xa, reach);
                if (by >= 0) {
                    auto const& rn = notes[(size_t)by];
                    rr = ringedPressRadius(rr, z, h);
                    drawReleaseAround(dl, ImVec2(xa, cy), rn.releaseTier, rr, z, h,
                                      fade * (results[(size_t)by].missed ? 0.32f : 1.f));
                }
                drawHead(dl, ImVec2(xa, cy), n.pressTier, z, n.player2, fade, rr);
            }

            // Keep everything above inside the box, and fade it at the right
            // end (a note on a bar fades with it, not over it). The alpha is
            // from each vertex's own spot, then the spot is pulled in to the
            // inner edge: a big triangle that crossed the edge would
            // otherwise smear color past the border (round 3's leak).
            for (int vi = fadeVtx; vi < dl->VtxBuffer.Size; ++vi) {
                ImDrawVert& vx = dl->VtxBuffer.Data[vi];
                if (vx.pos.x > fadeFrom) {
                    float const f = std::clamp((inR - vx.pos.x) / fadeIn, 0.f, 1.f);
                    ImU32 const a = (ImU32)(((vx.col >> IM_COL32_A_SHIFT) & 0xFF) * f);
                    vx.col = (vx.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
                }
                vx.pos.x = std::clamp(vx.pos.x, inL, inR);
                vx.pos.y = std::clamp(vx.pos.y, inT, inB);
            }

            // The border: light, with a dark line outside it so it shows over
            // a white level too.
            dl->AddRect(ImVec2(p0.x - 1.f, p0.y - 1.f), ImVec2(p1.x + 1.f, p1.y + 1.f), IM_COL32(0, 0, 0, 190),
                        round + 1.f, 0, 2.f);
            dl->AddRect(ImVec2(p0.x + border * 0.5f, p0.y + border * 0.5f),
                        ImVec2(p1.x - border * 0.5f, p1.y - border * 0.5f), IM_COL32(255, 255, 255, 230), round, 0,
                        border);

            if (set.judgments && !v.flash.text.empty() && age >= 0.f && age < 0.6f) {
                if (spot.side) {
                    spot.rightX = p0.x - kVerdictSideGap;
                    spot.midY = cy;
                    spot.maxX = spot.rightX;
                } else {
                    spot.hitX = hitX;
                    spot.topY = p1.y + kVerdictGap;
                    spot.minX = p0.x;
                    spot.maxX = p1.x;
                }
                int const verdictVtx = dl->VtxBuffer.Size;
                drawVerdict(dl, v.flash.text.c_str(), v.flash.release, vcol, age, h, spot);
                return verdictVtx;
            }
            return dl->VtxBuffer.Size;
        }

        // The lane's height on the card: as over the level, but no more than
        // 16% of the card's width, so it keeps about the level's proportions
        // and a few notes' room shows between the marker and the next note.
        float cardLaneHeight(float w) {
            return std::round(std::clamp(levelGeom().h, 48.f, std::max(48.f, w * 0.16f)));
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
            // The verdict under the lane, on the theme's raised surface.
            VerdictSpot spot;
            spot.side = false;
            spot.pill = u32(look().pal.raised, 0.97f);
            drawLaneBox(k, ImGui::GetWindowDrawList(), p0, ImVec2(w, h), pxPerSec, kFillCard, spot);
            // Room under the lane for the verdict, a RELEASE line and its pop.
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
            float const px = h * kVerdictOfLane * kPop;
            ImGui::Dummy(ImVec2(w, kVerdictGap + verdictBlockHeight(px, true) + 2.f * verdictPad(h) * kPop + 4.f));
        }

        // What the colors and sizes mean, under the lane: at the card's body
        // size, in its text color, with swatches drawn as a lane 56 px tall
        // would (a 1-frame note 22 px across, a 3-frame one 43).
        void laneLegend(Kind k) {
            auto const& pal = look().pal;
            auto const v = ln::view(k);
            float const sh = 56.f;
            NoteSizes const z = noteSizes(sh);
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
                    drawRelease(dl, c, it.tier, z, sh, false, 1.f);
                else
                    drawHead(dl, c, it.tier, z, false, 1.f);
                dl->AddText(ImVec2(c.x + r + 7.f, c.y - ts.y * 0.5f), u32(pal.ink), it.words);
                p.x = c.x + r + 7.f + ts.x + 18.f;
            }
            ImGui::Dummy(ImVec2(std::max(1.f, right - startX), y + lineH - p.y));
            if (v.notes && !v.notes->empty() && v.counted == 0)
                kit::Hint("No windows are counted for this macro yet, so its notes are plain grey and a press is "
                          "judged by the OK range below. Count them on the Calculate page, then press Reload at the "
                          "top.");
        }

        // The players on screen, as boxes in ImGui's pixels (about each
        // one's size: a ship is wider), their gamemodes as bits, and GD's
        // floor and ceiling lines on screen (ImGui's y, down; -1 when that
        // line isn't shown).
        struct PlayersOnScreen {
            int n = 0;
            ImVec2 lo[2], hi[2];
            int modes = 0;
            float floorY = -1.f;  // the top of the ground
            float ceilY = -1.f;   // the bottom of the ceiling (ship, UFO, wave, ball, spider, swing parts)
            float floorObjY = -1.f;  // where the level's floor (y 90) is, from the object layer
        };
        // A ground layer's line (its edge toward the play field) in ImGui's
        // y, or -1 if that ground isn't showing.
        float groundLineY(GJGroundLayer* g, float winH, float sy) {
            if (!g || !g->isVisible() || !g->m_lineSprite)
                return -1.f;
            for (cocos2d::CCNode* n = g; n; n = n->getParent())
                if (!n->isVisible())
                    return -1.f;
            auto* line = g->m_lineSprite;
            cocos2d::CCPoint const w = line->getParent()->convertToWorldSpace(line->getPosition());
            return (winH - w.y) * sy;
        }
        PlayersOnScreen playersOnScreen() {
            PlayersOnScreen out;
            auto* pl = PlayLayer::get();
            auto const win = cocos2d::CCDirector::get()->getWinSize();
            ImVec2 const disp = ImGui::GetIO().DisplaySize;
            if (!pl || !pl->m_objectLayer || win.width <= 0.f || win.height <= 0.f)
                return out;
            float const sx = disp.x / win.width, sy = disp.y / win.height;
            out.floorY = groundLineY(pl->m_groundLayer, win.height, sy);
            out.ceilY = groundLineY(pl->m_groundLayer2, win.height, sy);
            out.floorObjY = (win.height - pl->m_objectLayer->convertToWorldSpace(cocos2d::CCPoint{0.f, 90.f}).y) * sy;
            int slot = 0;
            for (PlayerObject* p : {pl->m_player1, pl->m_player2}) {
                ++slot;
                if (!p || !p->isVisible())
                    continue;
                cocos2d::CCPoint const at = p->getPosition();
                cocos2d::CCPoint const a = pl->m_objectLayer->convertToWorldSpace(at);
                cocos2d::CCPoint const b = pl->m_objectLayer->convertToWorldSpace(at + cocos2d::CCPoint{30.f, 0.f});
                float const half = 20.f * std::max(0.3f, p->m_vehicleSize) * a.getDistance(b) / 30.f;
                float const cx = a.x * sx, cyp = (win.height - a.y) * sy;
                out.lo[out.n] = ImVec2(cx - half * sx, cyp - half * sy);
                out.hi[out.n] = ImVec2(cx + half * sx, cyp + half * sy);
                ++out.n;
                int const m = (p->m_isShip ? 1 : 0) | (p->m_isBird ? 2 : 0) | (p->m_isBall ? 4 : 0) |
                              (p->m_isDart ? 8 : 0) | (p->m_isRobot ? 16 : 0) | (p->m_isSpider ? 32 : 0) |
                              (p->m_isSwing ? 64 : 0);
                out.modes |= m << (slot == 1 ? 0 : 8);
            }
            return out;
        }

        // The box's fill over the level: see-through (85% when the lane sits
        // at the bottom over the play field, not over the ground, so the
        // level's spikes don't make it busy), and more see-through (eased)
        // while a player is within 40 px of it, so the player shows.
        int levelFill(Kind k, PlayersOnScreen const& ps, ImVec2 p0, ImVec2 p1, int base) {
            static float eased[2] = {1.f, 1.f};  // 1 = the full fill
            float& e = eased[k == Kind::Jupiter ? 0 : 1];
            bool nearPlayer = false;
            for (int i = 0; i < ps.n; ++i)
                if (ps.hi[i].x + 40.f > p0.x && ps.lo[i].x - 40.f < p1.x && ps.hi[i].y + 40.f > p0.y &&
                    ps.lo[i].y - 40.f < p1.y)
                    nearPlayer = true;
            float const dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f);
            e += std::clamp((nearPlayer ? 0.f : 1.f) - e, -dt * 8.f, dt * 8.f);
            return (int)std::round(kFillNearPlayer + (base - kFillNearPlayer) * e);
        }

        // Where the lane sits over the level, and how see-through it is.
        // Auto:
        //  - With a ceiling on screen (ship, UFO, wave, swing, and ball or
        //    spider parts with one), by the corridor between the ceiling and
        //    the floor, not by where the player is this moment: the band
        //    that covers clearly less of it (a quarter lane or more).
        //  - Otherwise (cube, robot, ball, spider on the ground): the top,
        //    unless the bottom band fits wholly under GD's ground line, and
        //    then only once the player has been in the top band for 0.25 s
        //    (one jump's apex isn't enough). A player in the top band with
        //    no room under the ground: the lane fades to 30% instead.
        //  - It moves only after it has wanted the other band for 0.25 s of
        //    the level's time, at most every 0.6 s, sliding over 0.3 s; a
        //    new level, restart or respawn decides again with no slide.
        // On any place, a player inside the lane's band fades the whole lane
        // to 30%.
        struct AutoPlace {
            PlayLayer* level = nullptr;
            double lastNow = -1.0;
            int band = 0;
            float slide = 0.f;  // 0 top .. 1 bottom, before easing
            double lastMove = -100.0;  // the level's time
            double wantSince = -1.0;   // the level's time the other band became the better one
            double inTopSince = -1.0;  // the level's time a player came into the top band
            float alpha = 1.f;
            float verdictAlpha = 1.f;
        };
        struct PlaceOut {
            float y = 0.f, alpha = 1.f, verdictAlpha = 1.f;
            int fill = kFillLevel;
        };
        PlaceOut placeLane(Kind k, LaneGeom const& g, PlayersOnScreen const& ps) {
            static AutoPlace s_auto[2];
            AutoPlace& st = s_auto[k == Kind::Jupiter ? 0 : 1];
            auto const& set = ln::settings();
            float const dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f);
            float const screenH = ImGui::GetIO().DisplaySize.y;
            double const now = ln::now(k);
            auto* pl = PlayLayer::get();
            // A new level, a restart or a respawn: decide again, no slide.
            bool const snap = pl != st.level || now < st.lastNow - 0.05;
            st.level = pl;
            st.lastNow = now;
            if (snap) {
                st.wantSince = st.inTopSince = -1.0;
                st.lastMove = -100.0;
            }
            auto inBandAt = [&](float y0, float right) {
                for (int i = 0; i < ps.n; ++i)
                    if (ps.hi[i].x > 0.f && ps.lo[i].x < right && ps.hi[i].y > y0 && ps.lo[i].y < y0 + g.h)
                        return true;
                return false;
            };
            // Flying (ship, UFO, wave, swing, either player), or a ceiling on
            // screen: judge by the corridor. Its ceiling can sit at or above
            // the screen's top edge (then the corridor starts at the top).
            int const flyBits = 1 | 2 | 8 | 64;
            bool const flying = (ps.modes & (flyBits | flyBits << 8)) != 0;
            bool const ceiling = flying || (ps.ceilY > 0.f && ps.ceilY < screenH);
            float const floorLine = ps.floorY >= 0.f ? ps.floorY : ps.floorObjY;
            bool const bottomFits = floorLine >= 0.f && g.bottomY >= floorLine;
            auto& out = ln::placement(k);
            int want = st.band;
            if (set.place != ln::Place::Auto) {
                want = set.place == ln::Place::Bottom ? 1 : 0;
            } else if (ps.n > 0) {
                if (ceiling) {
                    // The corridor the player flies in, on screen.
                    float const top = std::max(0.f, ps.ceilY);
                    float const bot = floorLine >= 0.f ? std::min(floorLine, screenH) : screenH;
                    auto cover = [&](float y0) {
                        return std::max(0.f, std::min(y0 + g.h, bot) - std::max(y0, top));
                    };
                    float const cTop = cover(g.topY), cBot = cover(g.bottomY);
                    // Only for a clear difference (a quarter of the lane):
                    // when the corridor fills the screen both bands cover
                    // it, and the lane stays put (fading while the player
                    // is in it) instead of moving on a few pixels.
                    float const clear = g.h * 0.25f;
                    if (cBot + clear < cTop)
                        want = 1;
                    else if (cTop + clear < cBot)
                        want = 0;
                    st.inTopSince = -1.0;
                } else {
                    if (inBandAt(g.topY, g.x + g.w)) {
                        if (st.inTopSince < 0.0)
                            st.inTopSince = now;
                    } else {
                        st.inTopSince = -1.0;
                    }
                    bool const topBusy = st.inTopSince >= 0.0 && now - st.inTopSince >= 0.25;
                    if (st.band == 1)
                        want = bottomFits ? 1 : 0;
                    else
                        want = bottomFits && topBusy ? 1 : 0;
                }
            }
            if (snap) {
                // At a start, without a reason to be at the bottom, the top.
                st.band = set.place == ln::Place::Auto && !ceiling ? 0 : want;
                st.wantSince = -1.0;
            } else if (want != st.band) {
                if (set.place != ln::Place::Auto) {
                    st.band = want;
                } else {
                    if (st.wantSince < 0.0)
                        st.wantSince = now;
                    if (now - st.wantSince >= 0.25 && now - st.lastMove >= 0.6) {
                        st.band = want;
                        st.lastMove = now;
                        st.wantSince = -1.0;
                        ++out.moves;
                    }
                }
            } else {
                st.wantSince = -1.0;
            }
            float const target = (float)st.band;
            if (snap)
                st.slide = target;
            else
                st.slide += std::clamp(target - st.slide, -dt / 0.3f, dt / 0.3f);
            float const e = st.slide * st.slide * (3.f - 2.f * st.slide);  // ease in and out
            float const y = std::round(g.topY + (g.bottomY - g.topY) * e);
            // Inside the band (the box and the verdict's room left of it)?
            // The box fades for a player over the box; the verdict, left of
            // it, only for a player over the verdict's own spot.
            auto inRect = [&](float y0, float left, float right) {
                for (int i = 0; i < ps.n; ++i)
                    if (ps.hi[i].x > left && ps.lo[i].x < right && ps.hi[i].y > y0 && ps.lo[i].y < y0 + g.h)
                        return true;
                return false;
            };
            float const wantA = inRect(y, g.x, g.x + g.w) ? kInBandAlpha : 1.f;
            float const wantV = inRect(y, 0.f, g.x) ? kInBandAlpha : 1.f;
            st.alpha = snap ? wantA : st.alpha + std::clamp(wantA - st.alpha, -dt * 5.f, dt * 5.f);
            st.verdictAlpha = snap ? wantV : st.verdictAlpha + std::clamp(wantV - st.verdictAlpha, -dt * 5.f, dt * 5.f);
            PlaceOut po;
            po.y = y;
            po.alpha = st.alpha;
            po.verdictAlpha = st.verdictAlpha;
            // At the bottom over the play field (not under the ground line):
            // a fuller fill.
            bool const overPlay = floorLine < 0.f || y < floorLine;
            po.fill = e > 0.5f && overPlay ? kFillBottomOverPlay : kFillLevel;
            out.band = st.band;
            out.slide = e;
            out.alpha = st.alpha;
            out.y = y;
            out.h = g.h;
            out.floorY = floorLine;
            out.ceilY = ps.ceilY;
            out.fill = po.fill;
            out.bottomFits = bottomFits;
            return po;
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
            auto const g = levelGeom();
            auto const ps = playersOnScreen();
            auto const place = placeLane(k, g, ps);
            float const alpha = place.alpha;
            float const y = place.y;
            ImVec2 const p0(g.x, y), p1(g.x + g.w, y + g.h);
            // The background list: over the game, under every window, and it
            // never takes a click from the game. The verdict goes left of the
            // box, in its band.
            auto* dl = ImGui::GetBackgroundDrawList();
            int const vtx0 = dl->VtxBuffer.Size;
            VerdictSpot spot;
            spot.side = true;
            spot.minX = 8.f;
            spot.pill = IM_COL32(0, 0, 0, 165);
            int const verdictVtx = drawLaneBox(k, dl, p0, ImVec2(g.w, g.h), lanePxPerSec(g.w, g.h, ln::view(k).tps),
                                               levelFill(k, ps, p0, p1, place.fill), spot);
            // A player over the box: the whole box (fill, notes, bars,
            // marker, border) at 30%; over the verdict's spot, the verdict.
            if (alpha < 0.999f || place.verdictAlpha < 0.999f) {
                for (int vi = vtx0; vi < dl->VtxBuffer.Size; ++vi) {
                    float const f = vi < verdictVtx ? alpha : place.verdictAlpha;
                    ImDrawVert& vx = dl->VtxBuffer.Data[vi];
                    ImU32 const a = (ImU32)(((vx.col >> IM_COL32_A_SHIFT) & 0xFF) * f);
                    vx.col = (vx.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
                }
            }
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
            changed |= kit::SwitchRow("Judgments", "PERFECT, EARLY 1f, LATE 2f or MISS as you hit: left of the lane in the level, under it here",
                                      &set.judgments);
            changed |= kit::SwitchRow("Show in the level", "The lane over the trainer's level while you play it, "
                                                            "with the menu closed",
                                      &set.inLevel);
            if (set.inLevel) {
                changed |= kit::SliderRow("Width in the level", nullptr, &set.levelWidth, 0.3f, 1.f, "%.2f");
                // Auto first: the band the player is farther from.
                int place = set.place == ln::Place::Auto ? 0 : set.place == ln::Place::Top ? 1 : 2;
                const char* const places[] = {"Auto", "Top", "Bottom"};
                if (kit::DropdownRow("Place in the level",
                                     "Auto: at the top or the bottom, whichever the player is farther from. It "
                                     "moves over at a gamemode portal or when the player comes near, and fades "
                                     "while the player is inside it.",
                                     &place, places, 3)) {
                    set.place = place == 0 ? ln::Place::Auto : place == 1 ? ln::Place::Top : ln::Place::Bottom;
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
