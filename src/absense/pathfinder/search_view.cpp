// GucciBot (2026-11-15, the vsearch lane): the Pathfinder's "Show the search".
// See search_view.hpp. Main thread only: the search's slices, the drawScene
// hook and the menu all run there.

#include "absense/pathfinder/search_view.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/Mod.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "absense/compat/bot.hpp"
#include "absense/glue.hpp"
#include "absense/pathfinder/pathfinder.hpp"
#include "absense/trajectory/trajectory.hpp"

using namespace geode::prelude;

namespace {
    constexpr int kPaths = 32;       // the newest paths of the decision in progress
    constexpr int kBestPoints = 96;  // the best's path, sampled from its own samples
    constexpr int kNodeZ = 1600;     // over the level map's drawing (1500)
    constexpr const char* kSaved = "pf_show_search";

    struct Path {
        float x[PathTap::kPoints + 1] = {};
        float y[PathTap::kPoints + 1] = {};
        int count = 0;
        int candidate = -1;
        int survived = 0;
        bool died = false;
        bool complete = false;
    };

    struct State {
        bool loaded = false;
        bool shown = true;
        uint64_t key = 0;     // the decision the ring belongs to
        PathTap tap;          // the run in progress
        std::array<Path, kPaths> ring;
        int next = 0;         // where the next path goes
        int filled = 0;       // paths in the ring
        uint64_t runs = 0;    // copies run for this decision (the line's "paths tried")
        bool haveOrigin = false;
        float originX = 0.0f, originY = 0.0f;
        uint64_t version = 0;  // moves on with every path kept
        // What the node shows now, so it is redrawn only when that changes.
        uint64_t drawnVersion = ~0ull;
        uint64_t drawnKey = 0;
        int drawnBest = -2;
        int drawnBestSize = -1;
        bool drawn = false;   // the node has something on it
        float accent[3] = {0.35f, 0.85f, 1.0f};
        // Cost, for Assistant Access (microseconds).
        double lastUs = 0.0, maxUs = 0.0, sumUs = 0.0;
        uint64_t draws = 0;
        uint64_t over1ms = 0;   // redraws that took over a millisecond (wall clock, so a busy PC counts too)
        int lastSegments = 0;
        std::string line;     // the status line, empty while nothing is shown
    };
    State& st() {
        static State s;
        return s;
    }

    void load() {
        auto& s = st();
        if (s.loaded) return;
        s.loaded = true;
        s.shown = Mod::get()->getSavedValue<bool>(kSaved, true);
    }

    void resetFor(uint64_t key) {
        auto& s = st();
        s.key = key;
        s.next = 0;
        s.filled = 0;
        s.runs = 0;
        s.haveOrigin = false;
        s.version++;
    }

    CCDrawNode* nodeIn(PlayLayer* pl, bool make) {
        if (!pl || !pl->m_objectLayer) return nullptr;
        auto* n = typeinfo_cast<CCDrawNode*>(pl->m_objectLayer->getChildByID("search-view"_spr));
        if (n || !make) return n;
        n = CCDrawNode::create();
        if (!n) return nullptr;
        n->m_bUseArea = false;  // RobTop's culling: the paths reach past the screen's area
        n->setID("search-view"_spr);
        n->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        pl->m_objectLayer->addChild(n, kNodeZ);
        return n;
    }

    void clearNode(PlayLayer* pl) {
        auto& s = st();
        if (!s.drawn) return;
        if (auto* n = nodeIn(pl, false)) n->clear();
        s.drawn = false;
        s.drawnVersion = ~0ull;
        s.drawnBest = -2;
        s.line.clear();
    }

    // The line in plain words: where it decides, how many ideas, the best.
    std::string makeLine(const AbsensePathfinder::ViewInfo& v, PlayLayer* pl) {
        auto& s = st();
        const float len = pl ? pl->m_levelLength : 0.0f;
        const float x = s.haveOrigin ? s.originX : (pl && pl->m_player1 ? pl->m_player1->getPositionX() : 0.0f);
        const float pct = len > 0.0f ? std::clamp(x / len, 0.0f, 1.0f) * 100.0f : 0.0f;
        const double tps = std::max(1.0, Bot::get()->updater().getTps());
        std::string out = v.repairing ? fmt::format("Looking for a way past a death, at {:.1f}%: ", pct)
                                      : fmt::format("Deciding at {:.1f}%: ", pct);
        // Paths, not "ideas": the log's "N of M ideas tried" counts judged
        // ideas only, and each idea can take more than one run (a first
        // look, then the whole look ahead).
        out += fmt::format("{} path{} tried", s.runs, s.runs == 1 ? "" : "s");
        if (v.best < 0) return out + ", none judged yet";
        if (v.bestComplete) return out + ", the best reaches the end";
        const double sec = (double)std::max(0, v.bestSurvived) / tps;
        if (v.bestSurvived >= v.horizon) return out + fmt::format(", the best lasts {:.1f} s, as far as it looks", sec);
        return out + fmt::format(", the best lasts {:.1f} s", sec);
    }

    void drawPath(CCDrawNode* n, const float* xs, const float* ys, int count, float width, const ccColor4F& c,
                  int& segments) {
        for (int i = 1; i < count; i++) {
            if (xs[i] == xs[i - 1] && ys[i] == ys[i - 1]) continue;
            n->drawSegment(ccp(xs[i - 1], ys[i - 1]), ccp(xs[i], ys[i]), width, c);
            segments++;
        }
    }
}  // namespace

namespace absense::view {
    PathTap* tapFor(uint64_t decisionKey) {
        load();
        auto& s = st();
        if (!s.shown) return nullptr;
        if (decisionKey != s.key) resetFor(decisionKey);
        s.tap.reset();
        return &s.tap;
    }

    void keep(int candidate, int survived, bool complete) {
        auto& s = st();
        const PathTap& t = s.tap;
        if (t.count <= 0) return;
        if (!s.haveOrigin) {
            s.haveOrigin = true;
            s.originX = t.x[0];
            s.originY = t.y[0];
        }
        Path& p = s.ring[(size_t)s.next];
        p.count = 0;
        for (int i = 0; i < t.count; i++) {
            p.x[p.count] = t.x[i];
            p.y[p.count] = t.y[i];
            p.count++;
        }
        if (p.x[p.count - 1] != t.endX || p.y[p.count - 1] != t.endY) {  // the exact end, past the last kept point
            p.x[p.count] = t.endX;
            p.y[p.count] = t.endY;
            p.count++;
        }
        p.candidate = candidate;
        p.survived = survived;
        p.died = t.died;
        p.complete = complete;
        s.next = (s.next + 1) % kPaths;
        s.filled = std::min(s.filled + 1, kPaths);
        s.runs++;
        s.version++;
    }
}  // namespace absense::view

bool absense::searchShown() {
    load();
    return st().shown;
}

void absense::setSearchShown(bool on) {
    load();
    st().shown = on;
    Mod::get()->setSavedValue<bool>(kSaved, on);
}

void absense::setSearchAccent(float r, float g, float b) {
    auto& s = st();
    if (s.accent[0] == r && s.accent[1] == g && s.accent[2] == b) return;
    s.accent[0] = r;
    s.accent[1] = g;
    s.accent[2] = b;
    s.drawnVersion = ~0ull;  // drawn again in the new colour
}

std::string absense::searchLine() {
    return st().line;
}

void absense::searchViewFrame() {
    load();
    auto& s = st();
    auto* pl = PlayLayer::get();
    if (!pl) {
        s.drawn = false;  // the node went with its level
        s.line.clear();
        return;
    }
    if (!s.shown) {
        clearNode(pl);
        return;
    }
    auto& pf = Bot::get()->pathfinder();
    if (!pf.isRunning()) {
        clearNode(pl);
        return;
    }
    const auto v = pf.viewInfo();
    if (!v.waiting) {
        clearNode(pl);
        return;
    }
    if (v.key != s.key) resetFor(v.key);  // a decision with no run yet: nothing of the last one
    s.line = makeLine(v, pl);
    const int bestSize = v.bestTrace ? (int)v.bestTrace->size() : -1;
    if (s.drawn && s.drawnVersion == s.version && s.drawnKey == s.key && s.drawnBest == v.best &&
        s.drawnBestSize == bestSize)
        return;  // nothing new since the last drawing

    const auto t0 = std::chrono::steady_clock::now();
    auto* n = nodeIn(pl, true);
    if (!n) return;
    n->clear();
    int segments = 0;
    // The ideas, oldest faintest. Where one dies, a small red dot.
    const ccColor4F dotDead = ccc4f(1.0f, 0.3f, 0.3f, 0.45f);
    int bestRing = -1;
    for (int k = 0; k < s.filled; k++) {
        const int idx = (s.next - s.filled + k + kPaths) % kPaths;
        const Path& p = s.ring[(size_t)idx];
        if (v.best >= 0 && p.candidate == v.best && p.survived == v.bestSurvived) bestRing = idx;
        const float age = s.filled > 1 ? (float)k / (float)(s.filled - 1) : 1.0f;  // 0 oldest .. 1 newest
        const ccColor4F faint = ccc4f(0.88f, 0.93f, 1.0f, 0.10f + 0.22f * age);
        drawPath(n, p.x, p.y, p.count, 0.75f, faint, segments);
        if (p.died && p.count > 0) n->drawDot(ccp(p.x[p.count - 1], p.y[p.count - 1]), 1.8f, dotDead);
    }
    // The best so far, brighter: its own samples when the decision has them,
    // else its path in the ring.
    const ccColor4F bright = ccc4f(s.accent[0], s.accent[1], s.accent[2], 0.95f);
    float bx[kBestPoints + 1], by[kBestPoints + 1];
    int bn = 0;
    if (v.bestTrace && !v.bestTrace->empty() && s.haveOrigin) {
        const auto& tr = *v.bestTrace;
        bx[bn] = s.originX;
        by[bn] = s.originY;
        bn++;
        const size_t total = tr.size();
        const size_t step = std::max<size_t>(1, (total + kBestPoints - 2) / (kBestPoints - 1));
        for (size_t i = step - 1; i < total && bn < kBestPoints; i += step) {
            bx[bn] = tr[i].x;
            by[bn] = tr[i].y;
            bn++;
        }
        if (bx[bn - 1] != tr.back().x || by[bn - 1] != tr.back().y) {
            bx[bn] = tr.back().x;
            by[bn] = tr.back().y;
            bn++;
        }
    } else if (bestRing >= 0) {
        const Path& p = s.ring[(size_t)bestRing];
        for (int i = 0; i < p.count; i++) {
            bx[bn] = p.x[i];
            by[bn] = p.y[i];
            bn++;
        }
    }
    if (bn > 1) {
        drawPath(n, bx, by, bn, 1.6f, bright, segments);
        n->drawDot(ccp(bx[bn - 1], by[bn - 1]), 3.0f, bright);
    }
    // Where every idea starts.
    if (s.haveOrigin) n->drawDot(ccp(s.originX, s.originY), 2.5f, ccc4f(1.0f, 1.0f, 1.0f, 0.85f));
    s.drawn = true;
    s.drawnVersion = s.version;
    s.drawnKey = s.key;
    s.drawnBest = v.best;
    s.drawnBestSize = bestSize;
    s.lastSegments = segments;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    s.lastUs = us;
    s.maxUs = std::max(s.maxUs, us);
    s.sumUs += us;
    s.draws++;
    if (us > 1000.0) s.over1ms++;
}

matjson::Value absense::searchViewJson() {
    load();
    auto& s = st();
    auto out = matjson::Value::object();
    out["shown"] = s.shown;
    out["drawn"] = s.drawn;
    out["line"] = s.line;
    out["paths"] = (int64_t)s.filled;
    out["paths_tried"] = (int64_t)s.runs;
    out["segments_last"] = (int64_t)s.lastSegments;
    out["draws"] = (int64_t)s.draws;
    out["draw_us_last"] = s.lastUs;
    out["draw_us_max"] = s.maxUs;
    out["draw_us_avg"] = s.draws ? s.sumUs / (double)s.draws : 0.0;
    out["draws_over_1ms"] = (int64_t)s.over1ms;
    // What the ring holds, for tests: where the paths go (the box around
    // every point, how many different end points) and the newest few.
    if (s.filled > 0) {
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        int ends = 0, died = 0, points = 0;
        float seenX[kPaths], seenY[kPaths];
        auto newest = matjson::Value::array();
        for (int k = 0; k < s.filled; k++) {
            const int idx = (s.next - 1 - k + kPaths) % kPaths;  // newest first
            const Path& p = s.ring[(size_t)idx];
            points += p.count;
            if (p.died) died++;
            for (int i = 0; i < p.count; i++) {
                x0 = std::min(x0, p.x[i]);
                x1 = std::max(x1, p.x[i]);
                y0 = std::min(y0, p.y[i]);
                y1 = std::max(y1, p.y[i]);
            }
            if (p.count > 0) {
                const float ex = p.x[p.count - 1], ey = p.y[p.count - 1];
                bool same = false;
                for (int j = 0; j < ends && !same; j++)
                    same = std::fabs(seenX[j] - ex) < 0.5f && std::fabs(seenY[j] - ey) < 0.5f;
                if (!same) {
                    seenX[ends] = ex;
                    seenY[ends] = ey;
                    ends++;
                }
            }
            if (k < 4 && p.count > 0) {
                auto one = matjson::Value::object();
                one["points"] = (int64_t)p.count;
                one["candidate"] = (int64_t)p.candidate;
                one["survived"] = (int64_t)p.survived;
                one["died"] = p.died;
                one["start"] = fmt::format("{:.1f},{:.1f}", p.x[0], p.y[0]);
                one["end"] = fmt::format("{:.1f},{:.1f}", p.x[p.count - 1], p.y[p.count - 1]);
                newest.push(one);
            }
        }
        auto ring = matjson::Value::object();
        ring["origin"] = fmt::format("{:.1f},{:.1f}", s.originX, s.originY);
        ring["box"] = fmt::format("x {:.1f}..{:.1f}, y {:.1f}..{:.1f}", x0, x1, y0, y1);
        ring["points"] = (int64_t)points;
        ring["different_ends"] = (int64_t)ends;
        ring["died"] = (int64_t)died;
        ring["newest"] = newest;
        out["ring"] = ring;
    }
    return out;
}

bool absense::searchViewReady(int minPaths) {
    auto& s = st();
    return s.shown && s.drawn && s.filled >= minPaths;
}

void absense::resetSearchViewCost() {
    auto& s = st();
    s.lastUs = s.maxUs = s.sumUs = 0.0;
    s.draws = 0;
    s.over1ms = 0;
}
