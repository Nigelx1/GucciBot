// Click Indicators (click_indicators.hpp says what and why).

#include "trainers/click_indicators.hpp"

#include "core/GucciBot.hpp"
#include "trainers/trainer_core.hpp"

#include <Geode/Geode.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace gucci::clickind {

    namespace {
        Settings g_s;
        bool g_loaded = false;

        double steadyNow() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        enum Result : uint8_t { None = 0, Perfect = 1, Ok = 2, Miss = 3 };

        struct Note {
            double press = 0.0;  // macro seconds from the level's start
            double release = 0.0;
            bool p2 = false;
            uint8_t result = None;
        };

        // One macro's presses, and how its frames map onto seconds: a macro can
        // change its tick rate part way (Macro Tools adds such changes).
        struct Source {
            struct Seg {
                double frame, sec, tps;
            };
            std::string key;
            std::string label;
            std::vector<Note> notes;  // in press order
            std::vector<Seg> segs;    // the first one at frame 0
            std::vector<MacroPathSample> const* path = nullptr;
            bool dual = false;     // a two-player macro: player 2 has its own presses
            bool trainer = false;  // timed by the trainers' clock

            double secOf(double frame) const {
                Seg s = segs.empty() ? Seg{0.0, 0.0, 240.0} : segs.front();
                for (auto const& g : segs) {
                    if (g.frame > frame)
                        break;
                    s = g;
                }
                return s.sec + (frame - s.frame) / s.tps;
            }
            double frameOf(double sec) const {
                Seg s = segs.empty() ? Seg{0.0, 0.0, 240.0} : segs.front();
                for (auto const& g : segs) {
                    if (g.sec > sec)
                        break;
                    s = g;
                }
                return s.frame + (sec - s.sec) * s.tps;
            }
        };

        Source g_src;
        bool g_active = false;  // something is shown this frame
        double g_now = 0.0;     // macro seconds, this frame
        double g_lastNow = -1.0;
        double g_attemptStart = 0.0;
        PlayLayer* g_level = nullptr;
        Tally g_tally;

        Result g_verdict = None;
        double g_verdictErrMs = 0.0;
        double g_verdictAt = -10.0;
        double g_flashAt[2] = {-10.0, -10.0};
        bool g_flashHit[2] = {false, false};

        bool buildFromTrainer(Source& src, trainers::Kind k, const char* label) {
            auto st = trainers::state(k);
            auto const& m = st.macro;
            if (!m.loaded || m.clickIntervalsSec.empty())
                return false;
            std::string const key = fmt::format("trainer:{}:{}:{}", (int)k, m.clickIntervalsSec.size(),
                                                m.clickIntervalsSec.front().first);
            if (key == src.key)
                return true;
            src = {};
            src.key = key;
            src.label = label;
            src.trainer = true;
            src.segs.push_back({0.0, 0.0, m.clickBarTps > 0.0 ? m.clickBarTps : 240.0});
            src.path = &m.pathSamples;
            for (auto const& [a, b] : m.clickIntervalsSec)
                src.notes.push_back({a, std::max(a, b), false});
            std::stable_sort(src.notes.begin(), src.notes.end(),
                             [](Note const& x, Note const& y) { return x.press < y.press; });
            return true;
        }

        bool buildFromLoaded(Source& src, PlayLayer* pl) {
            auto* gb = GucciEngine::get();
            auto const& acts = gb->replay.m_actionAtom.m_actions;
            if (acts.empty())
                return false;
            // A macro that says which level it was recorded on shows only there.
            if (!gb->loadedMacroLevelName.empty() && pl->m_level &&
                lower(gb->loadedMacroLevelName) != lower(std::string(pl->m_level->m_levelName)))
                return false;
            std::string const key =
                fmt::format("loaded:{}:{}:{}", gb->replayName, acts.size(), acts.back().m_frame);
            if (key == src.key)
                return !src.notes.empty();
            src = {};
            src.key = key;
            src.label = "the macro loaded on the Macro page";
            src.segs.push_back({0.0, 0.0, gb->replay.m_initialTPS > 0.0 ? gb->replay.m_initialTPS : 240.0});
            src.path = &gb->replay.m_pathSamples;
            double open[2] = {-1.0, -1.0};
            for (auto const& a : acts) {
                if (a.m_type == gb::ActionType::TPS && a.m_tps > 0.0) {
                    double const f = (double)a.m_frame;
                    src.segs.push_back({f, src.secOf(f), a.m_tps});
                    continue;
                }
                if (a.m_type != gb::ActionType::Jump)
                    continue;
                int const p = a.m_player2 ? 1 : 0;
                if (a.m_player2)
                    src.dual = true;
                double const f = (double)a.m_frame + a.m_subtick;
                if (a.m_holding) {
                    if (open[p] < 0.0)
                        open[p] = f;
                } else if (open[p] >= 0.0) {
                    src.notes.push_back({src.secOf(open[p]), src.secOf(f), p == 1});
                    open[p] = -1.0;
                }
            }
            for (int p = 0; p < 2; p++)
                if (open[p] >= 0.0) {
                    double const s = src.secOf(open[p]);
                    src.notes.push_back({s, s, p == 1});
                }
            std::stable_sort(src.notes.begin(), src.notes.end(),
                             [](Note const& x, Note const& y) { return x.press < y.press; });
            return !src.notes.empty();
        }

        // Macro seconds right now: the trainers' clock in a trainer's level,
        // the macro's own frame-to-seconds map anywhere else.
        double currentNow() {
            if (g_src.trainer)
                return trainers::levelTimeSec();
            return g_src.secOf((double)GucciEngine::get()->updater.getFrame() + 1.0);
        }

        void recount() {
            g_tally = {};
            for (auto const& n : g_src.notes) {
                if (n.result == Perfect)
                    g_tally.perfect++;
                else if (n.result == Ok)
                    g_tally.ok++;
                else if (n.result == Miss)
                    g_tally.miss++;
            }
        }

        void flash(bool p2, bool hit) {
            int const i = p2 && g_src.dual && g_s.p2Lane ? 1 : 0;
            g_flashAt[i] = steadyNow();
            g_flashHit[i] = hit;
        }

        std::array<float, 3> rgbFrom(int64_t v) {
            return {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f};
        }
        int64_t rgbTo(std::array<float, 3> const& c) {
            auto ch = [](float f) { return (int64_t)std::lround(std::clamp(f, 0.f, 1.f) * 255.f); };
            return (ch(c[0]) << 16) | (ch(c[1]) << 8) | ch(c[2]);
        }
    } // namespace

    Settings& settings() {
        load();
        return g_s;
    }

    void load() {
        if (g_loaded)
            return;
        g_loaded = true;
        auto* m = Mod::get();
        auto b = [m](const char* k, bool& v) { v = m->getSavedValue<bool>(k, v); };
        auto i = [m](const char* k, int& v, int lo, int hi) {
            v = std::clamp((int)m->getSavedValue<int64_t>(k, v), lo, hi);
        };
        auto f = [m](const char* k, float& v, float lo, float hi) {
            v = std::clamp((float)m->getSavedValue<double>(k, v), lo, hi);
        };
        auto c = [m](const char* k, std::array<float, 3>& v) { v = rgbFrom(m->getSavedValue<int64_t>(k, rgbTo(v))); };
        // The trainers' in-level bars came first; their two settings carry over.
        g_s.enabled = m->getSavedValue<bool>("ci_enabled", m->getSavedValue<bool>("trainer_bars_in_level", true));
        g_s.lookAhead = (float)m->getSavedValue<double>("trainer_bars_lookahead", g_s.lookAhead);
        f("ci_look_ahead", g_s.lookAhead, 0.5f, 10.f);
        i("ci_offset_ms", g_s.offsetMs, -2000, 2000);
        i("ci_max_notes", g_s.maxNotes, 1, 60);
        c("ci_color", g_s.color);
        i("ci_transparency", g_s.transparency, 0, 100);
        c("ci_p2_color", g_s.p2Color);
        i("ci_p2_transparency", g_s.p2Transparency, 0, 100);
        f("ci_line_width", g_s.lineWidth, 1.f, 20.f);
        i("ci_tap_ms", g_s.tapMs, 5, 300);
        b("ci_edge_marker", g_s.edgeMarker);
        b("ci_hold_tracks", g_s.holdTracks);
        b("ci_press_badge", g_s.pressBadge);
        f("ci_badge_size", g_s.badgeSize, 2.f, 20.f);
        b("ci_player_square", g_s.playerSquare);
        b("ci_player_line", g_s.playerLine);
        c("ci_player_color", g_s.playerColor);
        i("ci_player_transparency", g_s.playerTransparency, 0, 100);
        b("ci_lane", g_s.lane);
        i("ci_lane_side", g_s.laneSide, 0, 2);
        f("ci_mid_lane_hit", g_s.midLaneHit, 0.08f, 0.5f);
        f("ci_lane_window", g_s.laneWindow, 0.3f, 6.f);
        f("ci_lane_scale", g_s.laneScale, 0.4f, 2.5f);
        f("ci_lane_depth", g_s.laneDepth, 0.2f, 4.f);
        i("ci_note_shape", g_s.noteShape, 0, 1);
        b("ci_lane_flip", g_s.laneFlip);
        i("ci_lane_transparency", g_s.laneTransparency, 0, 100);
        b("ci_p2_lane", g_s.p2Lane);
        b("ci_accuracy", g_s.accuracy);
        i("ci_perfect_ms", g_s.perfectMs, 1, 100);
        i("ci_ok_ms", g_s.okMs, 2, 400);
        g_s.okMs = std::max(g_s.okMs, g_s.perfectMs);
    }

    void save() {
        auto* m = Mod::get();
        m->setSavedValue("ci_enabled", g_s.enabled);
        m->setSavedValue<double>("ci_look_ahead", g_s.lookAhead);
        m->setSavedValue<int64_t>("ci_offset_ms", g_s.offsetMs);
        m->setSavedValue<int64_t>("ci_max_notes", g_s.maxNotes);
        m->setSavedValue<int64_t>("ci_color", rgbTo(g_s.color));
        m->setSavedValue<int64_t>("ci_transparency", g_s.transparency);
        m->setSavedValue<int64_t>("ci_p2_color", rgbTo(g_s.p2Color));
        m->setSavedValue<int64_t>("ci_p2_transparency", g_s.p2Transparency);
        m->setSavedValue<double>("ci_line_width", g_s.lineWidth);
        m->setSavedValue<int64_t>("ci_tap_ms", g_s.tapMs);
        m->setSavedValue("ci_edge_marker", g_s.edgeMarker);
        m->setSavedValue("ci_hold_tracks", g_s.holdTracks);
        m->setSavedValue("ci_press_badge", g_s.pressBadge);
        m->setSavedValue<double>("ci_badge_size", g_s.badgeSize);
        m->setSavedValue("ci_player_square", g_s.playerSquare);
        m->setSavedValue("ci_player_line", g_s.playerLine);
        m->setSavedValue<int64_t>("ci_player_color", rgbTo(g_s.playerColor));
        m->setSavedValue<int64_t>("ci_player_transparency", g_s.playerTransparency);
        m->setSavedValue("ci_lane", g_s.lane);
        m->setSavedValue<int64_t>("ci_lane_side", g_s.laneSide);
        m->setSavedValue<double>("ci_mid_lane_hit", g_s.midLaneHit);
        m->setSavedValue<double>("ci_lane_window", g_s.laneWindow);
        m->setSavedValue<double>("ci_lane_scale", g_s.laneScale);
        m->setSavedValue<double>("ci_lane_depth", g_s.laneDepth);
        m->setSavedValue<int64_t>("ci_note_shape", g_s.noteShape);
        m->setSavedValue("ci_lane_flip", g_s.laneFlip);
        m->setSavedValue<int64_t>("ci_lane_transparency", g_s.laneTransparency);
        m->setSavedValue("ci_p2_lane", g_s.p2Lane);
        m->setSavedValue("ci_accuracy", g_s.accuracy);
        m->setSavedValue<int64_t>("ci_perfect_ms", g_s.perfectMs);
        m->setSavedValue<int64_t>("ci_ok_ms", g_s.okMs);
    }

    void frame() {
        load();
        auto* pl = PlayLayer::get();
        auto* gb = GucciEngine::get();
        g_active = false;
        auto* layer = pl ? pl->m_objectLayer : nullptr;
        auto* node = layer ? static_cast<CCDrawNode*>(layer->getChildByID("click-indicators"_spr)) : nullptr;
        auto const hide = [&] {
            if (node)
                node->removeFromParent();
        };
        // Only while you play the level yourself: not under a macro playing or
        // recording, Calculate, or a pathfinder (those record as they play).
        if (!layer || !gb->enabled || !g_s.enabled || !gb->isIdle() || gb->analyzerOwnsRun()) {
            if (!pl)
                g_level = nullptr;
            hide();
            return;
        }

        bool have = false;
        if (trainers::followsLevel(trainers::Kind::Jupiter))
            have = buildFromTrainer(g_src, trainers::Kind::Jupiter, "JMF's macro");
        else if (trainers::followsLevel(trainers::Kind::Any))
            have = buildFromTrainer(g_src, trainers::Kind::Any, "the Trainer's macro");
        if (!have)
            have = buildFromLoaded(g_src, pl);
        if (!have) {
            hide();
            return;
        }
        g_active = true;

        // A new level or a new macro starts the tally over.
        double const now = currentNow();
        static std::string s_lastKey;
        if (pl != g_level || g_lastNow < 0.0 || g_src.key != s_lastKey) {
            g_level = pl;
            s_lastKey = g_src.key;
            for (auto& n : g_src.notes)
                n.result = None;
            g_attemptStart = now;
            recount();
        }
        double const off = g_s.offsetMs / 1000.0;
        // The clock went back: a restart, or a checkpoint. What hadn't happened
        // by then counts again.
        if (now < g_lastNow - 0.02) {
            for (auto& n : g_src.notes)
                if (n.press + off >= now - 1e-6)
                    n.result = None;
            g_attemptStart = now;
            g_verdict = None;
            recount();
        }
        g_lastNow = now;
        g_now = now;

        // Presses that went by unanswered are misses.
        if (g_s.accuracy) {
            double const okS = g_s.okMs / 1000.0;
            for (auto& n : g_src.notes) {
                double const t = n.press + off;
                if (t > now)
                    break;
                if (n.result == None && t >= g_attemptStart - 1e-6 && t + okS < now) {
                    n.result = Miss;
                    g_tally.miss++;
                    g_verdict = Miss;
                    g_verdictAt = steadyNow();
                    flash(n.p2, false);
                }
            }
        }

        if (!node) {
            node = CCDrawNode::create();
            node->setID("click-indicators"_spr);
            layer->addChild(node, 1400);
        }
        node->clear();

        float const zoom = std::max(0.05f, layer->getScale());
        float const badge = g_s.badgeSize / zoom;  // the same size on screen at any zoom
        float const thin = std::max(0.5f, 1.f / zoom);
        auto* p1 = pl->m_player1;
        auto* p2 = pl->m_player2;
        auto const playerAt = [&](bool second) {
            auto* p = second && p2 ? p2 : p1;
            return p ? p->getPosition() : CCPoint{0.f, 0.f};
        };
        auto const fillOf = [&](bool second) {
            auto const& c = second ? g_s.p2Color : g_s.color;
            int const t = second ? g_s.p2Transparency : g_s.transparency;
            return ccColor4F{c[0], c[1], c[2], 1.f - t / 100.f};
        };
        auto const solidOf = [&](bool second) {
            auto const& c = second ? g_s.p2Color : g_s.color;
            return ccColor4F{c[0], c[1], c[2], 0.95f};
        };
        // Where the player was at a time: the macro's own path when it has
        // one, the level's time-to-position otherwise.
        auto const xAt = [&](double sec, bool second) {
            long const fi = std::lround(g_src.frameOf(sec));
            if (g_src.path && fi >= 0 && (size_t)fi < g_src.path->size()) {
                auto const& smp = (*g_src.path)[(size_t)fi];
                bool const useP2 = second && smp.hasP2;
                float const x = useP2 ? smp.p2x : smp.p1x;
                float const y = useP2 ? smp.p2y : smp.p1y;
                if (x != 0.f || y != 0.f)
                    return x;
            }
            return pl->posForTime((float)sec).x;
        };
        auto const square = [&](CCPoint at, float side, float border, ccColor4F colour) {
            float const h = side * 0.5f;
            CCPoint sq[4] = {{at.x - h, at.y - h}, {at.x + h, at.y - h}, {at.x + h, at.y + h}, {at.x - h, at.y + h}};
            node->drawPolygon(sq, 4, {0.f, 0.f, 0.f, 0.f}, border, colour);
        };

        constexpr float kBottom = -2000.f, kTop = 6000.f;  // the floor to above any camera
        double const tapS = g_s.tapMs / 1000.0;
        int drawn = 0;
        for (auto const& n : g_src.notes) {
            double const t0 = n.press + off, t1 = n.release + off;
            if (t0 > now + g_s.lookAhead)
                break;
            bool const tap = n.release - n.press < tapS;
            bool const pressed = t0 <= now;
            if ((tap && pressed) || t1 < now)
                continue;
            if (drawn++ >= g_s.maxNotes)
                break;
            float x0 = xAt(t0, n.p2);
            if (tap) {
                node->drawSegment({x0, kBottom}, {x0, kTop}, std::max(0.5f, g_s.lineWidth * 0.5f), fillOf(n.p2));
            } else {
                float const x1 = xAt(t1, n.p2);
                if (pressed && g_s.holdTracks)
                    x0 = playerAt(n.p2).x;
                if (x1 <= x0)
                    continue;
                CCPoint span[4] = {{x0, kBottom}, {x1, kBottom}, {x1, kTop}, {x0, kTop}};
                node->drawPolygon(span, 4, fillOf(n.p2), 0.f, fillOf(n.p2));
            }
            if (pressed)
                continue;
            if (g_s.edgeMarker)
                node->drawSegment({x0, kBottom}, {x0, kTop}, thin, solidOf(n.p2));
            if (g_s.pressBadge)
                square({x0, playerAt(n.p2).y}, badge, thin, solidOf(n.p2));
        }

        // Your player: the line through you, and the square a badge sits in at
        // the right moment.
        auto const marks = [&](bool second) {
            CCPoint const at = playerAt(second);
            if (g_s.playerLine) {
                auto const& c = g_s.playerColor;
                node->drawSegment({at.x, kBottom}, {at.x, kTop}, thin,
                                  {c[0], c[1], c[2], 1.f - g_s.playerTransparency / 100.f});
            }
            if (g_s.playerSquare)
                square(at, badge, thin * 1.5f, solidOf(second));
        };
        marks(false);
        if (g_src.dual && p2 && pl->m_gameState.m_isDualMode)
            marks(true);
    }

    void drawOverlay() {
        if (!g_active || !PlayLayer::get() || (!g_s.lane && !g_s.accuracy))
            return;
        ImGuiIO const& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        double const t = steadyNow();
        double const off = g_s.offsetMs / 1000.0;
        float const sc = std::clamp(g_s.laneScale, 0.4f, 2.5f);
        bool const split = g_s.p2Lane && g_src.dual;
        float const colW = 64.f * sc;
        float const laneW = colW * (split ? 2.f : 1.f);
        bool const middle = g_s.laneSide == 2;
        auto const rgba = [](std::array<float, 3> const& c, float a) {
            return ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], std::clamp(a, 0.f, 1.f)));
        };

        // The verdict sits beside the lane, or at the top when there's none.
        ImVec2 verdictAt(io.DisplaySize.x * 0.5f, 40.f);
        bool verdictLeftOf = false;

        if (g_s.lane) {
            float const k = g_s.noteShape == 0 ? std::max(0.f, g_s.laneDepth) : 0.f;
            // Along the lane, 0 at the hit line and 1 at the far end, bunched
            // toward the far end by the perspective; its width tapers the same way.
            auto const along = [k](float v) { return k > 0.f ? (1.f + k) * v / (1.f + k * v) : v; };
            auto const taper = [k](float v) { return k > 0.f ? 1.f / (1.f + k * v) : 1.f; };
            float length = 0.f;
            ImVec2 origin, dirAlong, dirAcross;
            if (!middle) {
                length = std::min(io.DisplaySize.y * 0.62f * sc, io.DisplaySize.y - 40.f);
                float const margin = 18.f;
                float const cx = g_s.laneSide == 0 ? margin + laneW * 0.5f : io.DisplaySize.x - margin - laneW * 0.5f;
                float const hitY = g_s.laneFlip ? (io.DisplaySize.y - length) * 0.5f : (io.DisplaySize.y + length) * 0.5f;
                origin = {cx, hitY};
                dirAlong = {0.f, g_s.laneFlip ? 1.f : -1.f};
                dirAcross = {1.f, 0.f};
                verdictLeftOf = g_s.laneSide == 1;
                verdictAt = {verdictLeftOf ? cx - laneW * 0.5f - 12.f : cx + laneW * 0.5f + 12.f, hitY - 30.f};
            } else {
                float const hitFrac = std::clamp(g_s.midLaneHit, 0.08f, 0.5f);
                float const hitX = g_s.laneFlip ? io.DisplaySize.x * (1.f - hitFrac) : io.DisplaySize.x * hitFrac;
                length = g_s.laneFlip ? hitX - 20.f : io.DisplaySize.x - 20.f - hitX;
                origin = {hitX, 14.f + laneW * 0.5f};
                dirAlong = {g_s.laneFlip ? -1.f : 1.f, 0.f};
                dirAcross = {0.f, 1.f};
                verdictAt = {hitX - 30.f, 14.f + laneW + 10.f};
            }
            auto const P = [&](float u, float v) {
                float const vv = std::clamp(v, 0.f, 1.f);
                float const a = along(vv) * length;
                float const w = laneW * taper(vv);
                return ImVec2(origin.x + dirAlong.x * a + dirAcross.x * u * w,
                              origin.y + dirAlong.y * a + dirAcross.y * u * w);
            };
            float const laneAlpha = 1.f - g_s.laneTransparency / 100.f;
            dl->AddQuadFilled(P(-0.5f, 0.f), P(0.5f, 0.f), P(0.5f, 1.f), P(-0.5f, 1.f),
                              ImGui::GetColorU32(ImVec4(0.04f, 0.04f, 0.06f, 0.85f * laneAlpha)));
            for (float v : {0.25f, 0.5f, 0.75f})
                dl->AddLine(P(-0.5f, v), P(0.5f, v), ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.08f * laneAlpha)));
            if (split)
                dl->AddLine(P(0.f, 0.f), P(0.f, 1.f), ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.15f * laneAlpha)));

            // The hit line, a flash of green on a hit and red on a miss.
            for (int c = 0; c < (split ? 2 : 1); c++) {
                float const uLo = split ? (c == 0 ? -0.5f : 0.f) : -0.5f;
                float const uHi = split ? (c == 0 ? 0.f : 0.5f) : 0.5f;
                float const since = (float)(t - g_flashAt[c]);
                ImVec4 colour(1.f, 1.f, 1.f, 0.8f);
                if (since < 0.3f) {
                    float const a = 1.f - since / 0.3f;
                    ImVec4 const hit = g_flashHit[c] ? ImVec4(0.3f, 1.f, 0.45f, 1.f) : ImVec4(1.f, 0.3f, 0.3f, 1.f);
                    colour = ImVec4(colour.x + (hit.x - colour.x) * a, colour.y + (hit.y - colour.y) * a,
                                    colour.z + (hit.z - colour.z) * a, 1.f);
                }
                dl->AddLine(P(uLo, 0.f), P(uHi, 0.f), ImGui::GetColorU32(colour), 3.f * sc);
            }

            double const win = std::max(0.3f, g_s.laneWindow);
            double const tapS = g_s.tapMs / 1000.0;
            int drawn = 0;
            for (auto const& n : g_src.notes) {
                double const t0 = n.press + off, t1 = n.release + off;
                if (t0 > g_now + win)
                    break;
                bool const tap = n.release - n.press < tapS;
                bool const answered = n.result == Perfect || n.result == Ok;
                if (t1 < g_now || n.result == Miss || (tap && answered))
                    continue;
                if (drawn++ >= g_s.maxNotes)
                    break;
                float const v0 = std::max(0.f, (float)((t0 - g_now) / win));
                float const v1 = std::min(1.f, (float)((t1 - g_now) / win));
                if (v0 > 1.f)
                    continue;
                bool const second = split && n.p2;
                float const uLo = split ? (second ? 0.f : -0.5f) : -0.5f;
                float const uHi = split ? (second ? 0.5f : 0.f) : 0.5f;
                float const pad = 0.04f;
                auto const& c = n.p2 ? g_s.p2Color : g_s.color;
                ImU32 const solid = rgba(c, 0.95f * std::max(0.35f, laneAlpha));
                if (g_s.noteShape == 0) {
                    float const thick = 0.03f;
                    float const a = v0, b = tap ? std::min(1.f, v0 + thick) : std::max(v0 + thick, v1);
                    if (!tap)
                        dl->AddQuadFilled(P(uLo + pad, a), P(uHi - pad, a), P(uHi - pad, b), P(uLo + pad, b),
                                          rgba(c, 0.45f * std::max(0.35f, laneAlpha)));
                    float const e = std::min(1.f, a + thick);
                    dl->AddQuadFilled(P(uLo + pad, a), P(uHi - pad, a), P(uHi - pad, e), P(uLo + pad, e), solid);
                } else {
                    float const r = colW * 0.3f;
                    ImVec2 const head = P((uLo + uHi) * 0.5f, v0);
                    if (!tap) {
                        ImVec2 const tail = P((uLo + uHi) * 0.5f, v1);
                        ImU32 const grey = ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.62f, 0.7f * std::max(0.35f, laneAlpha)));
                        dl->AddLine(head, tail, grey, r * 2.f);
                        dl->AddCircleFilled(tail, r, grey);
                    }
                    dl->AddCircleFilled(head, r, solid);
                }
            }
        }

        if (!g_s.accuracy)
            return;
        // The last verdict, fading, and the tally under it.
        ImFont* font = ImGui::GetFont();
        float const bigSize = ImGui::GetFontSize() * 1.25f;
        float const smallSize = ImGui::GetFontSize() * 0.85f;
        float const age = (float)(t - g_verdictAt);
        if (g_verdict != None && age < 0.8f) {
            float const a = 1.f - age / 0.8f;
            std::string text;
            ImVec4 colour;
            if (g_verdict == Perfect) {
                text = "PERFECT";
                colour = ImVec4(0.35f, 1.f, 0.45f, a);
            } else if (g_verdict == Ok) {
                // Yellow near perfect, orange near a miss.
                float const span = std::max(1.f, (float)(g_s.okMs - g_s.perfectMs));
                float const f = std::clamp(((float)std::abs(g_verdictErrMs) - g_s.perfectMs) / span, 0.f, 1.f);
                text = fmt::format("OK {:+.0f} ms", g_verdictErrMs);
                colour = ImVec4(1.f, 0.92f - 0.37f * f, 0.25f - 0.1f * f, a);
            } else {
                text = "MISS";
                colour = ImVec4(1.f, 0.3f, 0.3f, a);
            }
            ImVec2 pos = verdictAt;
            if (verdictLeftOf)
                pos.x -= font->CalcTextSizeA(bigSize, FLT_MAX, 0.f, text.c_str()).x;
            dl->AddText(font, bigSize, pos, ImGui::GetColorU32(colour), text.c_str());
        }
        std::string const line =
            fmt::format("Perfect {}  OK {}  Miss {}", g_tally.perfect, g_tally.ok, g_tally.miss);
        ImVec2 pos(verdictAt.x, verdictAt.y + bigSize + 4.f);
        if (verdictLeftOf)
            pos.x -= font->CalcTextSizeA(smallSize, FLT_MAX, 0.f, line.c_str()).x;
        dl->AddText(font, smallSize, pos, ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.75f)), line.c_str());
    }

    void onRealClick(bool player2, bool down) {
        if (!down || !g_active || !g_s.accuracy)
            return;
        double const off = g_s.offsetMs / 1000.0;
        double const okS = g_s.okMs / 1000.0;
        double const now = currentNow();
        Note* best = nullptr;
        double bestErr = 0.0;
        for (auto& n : g_src.notes) {
            double const t = n.press + off;
            if (t > now + okS)
                break;
            if (n.result != None || (g_src.dual && n.p2 != player2))
                continue;
            double const err = now - t;
            if (std::abs(err) <= okS && (!best || std::abs(err) < std::abs(bestErr))) {
                best = &n;
                bestErr = err;
            }
        }
        // A click with no press near it isn't scored, as in osu!mania.
        if (!best)
            return;
        double const errMs = bestErr * 1000.0;
        best->result = std::abs(errMs) <= g_s.perfectMs ? Perfect : Ok;
        (best->result == Perfect ? g_tally.perfect : g_tally.ok)++;
        g_verdict = (Result)best->result;
        g_verdictErrMs = errMs;
        g_verdictAt = steadyNow();
        flash(best->p2, true);
    }

    Tally tally() {
        return g_tally;
    }

    void resetTally() {
        for (auto& n : g_src.notes)
            n.result = None;
        g_attemptStart = g_now;
        g_verdict = None;
        recount();
    }

    const char* showing() {
        return g_active ? g_src.label.c_str() : "";
    }

} // namespace gucci::clickind
