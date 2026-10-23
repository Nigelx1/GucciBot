#include "absense/route/planner.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <utility>

using namespace geode::prelude;

namespace absense::route {

namespace {

    constexpr float kHalf = 15.0f;  // half the player's hitbox at normal size (atlas.cpp)
    // The price of a column the map thinks the mode cannot make up: high
    // enough that any way it can make is taken first, never infinite.
    constexpr double kOffMap = 1000.0;

    // How far the player's centre can rise and fall over one column (30
    // units) in each mode: an estimate from how the modes fly at normal speed,
    // until the simulation measures it (step R3). The spider's and ball's
    // gravity flips make big vertical moves cheap.
    void envelope(uint8_t mode, bool mini, float& rise, float& fall) {
        // On the generous side: a way the map wrongly allows costs one failed
        // leg search (then the gap is priced up, see noteLeg), while one it
        // wrongly rules out is never tried. The first run (Zafari 2, -di) had
        // 86 of its 128 gates out of reach on tighter numbers.
        switch (mode) {
            case 1: rise = 60.0f; fall = 60.0f; break;                        // ship
            case 2: rise = 90.0f; fall = 90.0f; break;                        // ball
            case 3: rise = 60.0f; fall = 60.0f; break;                        // ufo
            case 4: rise = mini ? 60.0f : 30.0f; fall = rise; break;          // wave: 45 degrees, steeper mini
            case 6: rise = 1.0e6f; fall = 1.0e6f; break;                      // spider
            case 7: rise = 60.0f; fall = 60.0f; break;                        // swing
            default: rise = 45.0f; fall = 90.0f; break;                       // cube, robot
        }
    }

    struct Interval {
        float lo, hi;
    };

    void merge(std::vector<Interval>& v) {
        std::sort(v.begin(), v.end(), [](Interval const& l, Interval const& r) { return l.lo < r.lo; });
        std::vector<Interval> out;
        for (auto const& i : v) {
            if (!out.empty() && i.lo <= out.back().hi + 0.5f) {
                out.back().hi = std::max(out.back().hi, i.hi);
            } else {
                out.push_back(i);
            }
        }
        v.swap(out);
    }

    // The price of getting from `from` (in column c0) to the band `to` (in
    // column c1 > c0) through the map's gaps, column by column, with gravity
    // upside down or not on the way.
    double stepCost(Atlas const& a, int c0, Interval from, int c1, Opening to, bool upsideDown) {
        std::vector<Interval> reach{from};
        std::vector<Interval> next;
        double cost = 0.0;
        for (int c = c0 + 1; c <= c1; c++) {
            auto const& col = a.columns[(size_t)c];
            float rise, fall;
            envelope(col.mode, col.mini, rise, fall);
            // Upside down, a cube or robot jumps down and falls up: the two
            // swap. (The other modes climb and fall alike here.)
            if (upsideDown) std::swap(rise, fall);
            const float half = kHalf * (col.mini ? 0.6f : 1.0f);
            next.clear();
            double tight = 1.0;
            for (auto const& o : col.openings) {
                bool reached = false;
                for (auto const& r : reach) {
                    const float lo = std::max(o.lo, r.lo - fall);
                    const float hi = std::min(o.hi, r.hi + rise);
                    if (hi >= lo) {
                        next.push_back({lo, hi});
                        reached = true;
                    }
                }
                // How tight the best gap reached here is: the player's height
                // over the gap's.
                if (reached) tight = std::min(tight, (double)(2.0f * half / (o.hi - o.lo + 2.0f * half)));
            }
            if (col.openings.empty()) {
                // A column the map sees no gap in at all: carry on through it
                // at the off-map price (physics may know better).
                cost += kOffMap * 2.0;
                continue;
            }
            if (next.empty()) {
                // Every gap here is out of the mode's reach from where the
                // route can be: jump to the nearest at the off-map price.
                const Opening* best = nullptr;
                float bestGap = std::numeric_limits<float>::max();
                for (auto const& o : col.openings)
                    for (auto const& r : reach) {
                        const float gap = r.hi < o.lo ? o.lo - r.hi : (r.lo > o.hi ? r.lo - o.hi : 0.0f);
                        if (gap < bestGap) {
                            bestGap = gap;
                            best = &o;
                        }
                    }
                cost += kOffMap * (1.0 + bestGap / 30.0);
                next.push_back({best->lo, best->hi});
                tight = 1.0;
            }
            cost += 1.0 + tight + (col.dynamic ? 0.5 : 0.0) + (col.unknown ? 2.0 : 0.0);
            merge(next);
            reach.swap(next);
        }
        float gap = std::numeric_limits<float>::max();
        for (auto const& r : reach) {
            if (r.hi >= to.lo && r.lo <= to.hi) return cost;
            gap = std::min(gap, r.hi < to.lo ? to.lo - r.hi : r.lo - to.hi);
        }
        return cost + kOffMap * (1.0 + gap / 30.0);
    }

    // Gaps the physics map has failed to reach, by (column, bottom of the gap):
    // after kFailsToPrice failures one costs kFailedGap more, so the next plan
    // goes another way where there is one.
    constexpr int kFailsToPrice = 2;
    constexpr double kFailedGap = 5000.0;
    std::map<std::pair<int, int>, int> g_fails;
    uint64_t g_failsAtlas = 0;
    // Up by one whenever what failed changes (a gap priced up, a new map): the
    // gate graph is priced again with it.
    uint64_t g_failsVersion = 1;
    bool g_replan = false;
    double failPrice(int column, float lo) {
        auto const it = g_fails.find({column, (int)std::lround(lo)});
        return it != g_fails.end() && it->second >= kFailsToPrice ? kFailedGap : 0.0;
    }
    void syncFails(Atlas const& a) {
        if (g_failsAtlas == a.id) return;
        g_fails.clear();  // a new map: what failed on the old one says nothing
        g_failsAtlas = a.id;
        g_failsVersion++;
    }

    // One option of a column's gates, as the planner sees it.
    struct Choice {
        int gate = -1;     // index into Atlas::gates
        int index = -1;    // into that gate's choices
        float x = 0.0f;    // the gate's x
        Option opt;
        Opening gap;       // the opening its band lies in (Waypoint::band)
    };

    // A column with gates: one step of a route, the options of every gate in
    // it together (gates sharing a column used to lose all but the first).
    struct Layer {
        int column = 0;
        std::vector<Choice> choices;  // named options first, then FREE
        // choices[0, eligible) are what a route may take here: the named
        // options where the gates offer any, else the FREE ones. Beside named
        // options FREE is the fallback, tried last, not a route choice: its
        // band is all of theirs together, so it would undercut every one of
        // them while saying nothing about what to do.
        size_t eligible = 0;
    };

    // Ties: use, touch and fire, then skip and avoid, then FREE.
    int kindRank(OptionKind k) {
        switch (k) {
            case OptionKind::Use: case OptionKind::Touch: case OptionKind::Fire: return 0;
            case OptionKind::Skip: case OptionKind::Avoid: return 1;
            default: return 2;
        }
    }

    // Costs compared to a thousandth, so two ways summed in a different order
    // tie as they should and the tie rules decide.
    int64_t costKey(double c) {
        if (!std::isfinite(c)) return std::numeric_limits<int64_t>::max();
        return (int64_t)std::llround(c * 1000.0);
    }

    void buildLayers(Atlas const& a, std::vector<Layer>& layers) {
        layers.clear();
        for (size_t gi = 0; gi < a.gates.size(); gi++) {
            auto const& g = a.gates[gi];
            if (g.choices.empty()) continue;
            if (layers.empty() || layers.back().column != g.column) {
                layers.emplace_back();
                layers.back().column = g.column;
            }
            auto& layer = layers.back();
            for (size_t k = 0; k < g.choices.size(); k++) {
                auto const& o = g.choices[k];
                if (o.kind == OptionKind::Free) {
                    // Every gate in a column has its gaps: one FREE per gap.
                    bool again = false;
                    for (auto const& c : layer.choices)
                        if (c.opt.kind == OptionKind::Free && std::fabs(c.opt.band.lo - o.band.lo) < 0.5f &&
                            std::fabs(c.opt.band.hi - o.band.hi) < 0.5f) {
                            again = true;
                            break;
                        }
                    if (again) continue;
                }
                Choice c;
                c.gate = (int)gi;
                c.index = (int)k;
                c.x = g.x;
                c.opt = o;
                c.gap = o.opening >= 0 && (size_t)o.opening < g.options.size() ? g.options[(size_t)o.opening] : o.band;
                layer.choices.push_back(c);
            }
        }
        for (auto& layer : layers) {
            std::sort(layer.choices.begin(), layer.choices.end(), [](Choice const& l, Choice const& r) {
                const int kl = kindRank(l.opt.kind);
                const int kr = kindRank(r.opt.kind);
                if (kl != kr) return kl < kr;
                if (l.opt.band.lo != r.opt.band.lo) return l.opt.band.lo < r.opt.band.lo;
                if (l.gate != r.gate) return l.gate < r.gate;
                return l.index < r.index;
            });
            size_t named = 0;
            for (auto const& c : layer.choices) named += c.opt.kind != OptionKind::Free ? 1 : 0;
            layer.eligible = named > 0 ? named : layer.choices.size();
        }
    }

    // The gates as the planner walks them, priced once per map (and again
    // when a gap is priced up): every way from an option of one column to an
    // option of the next, both ways up, and from each option the cheapest way
    // on to the level's end (the cost to go). Plans from anywhere and the
    // next gate's ranking all read it.
    struct Graph {
        uint64_t atlas = 0;
        uint64_t fails = 0;
        bool built = false;
        std::vector<Layer> layers;
        // asym[c]: how many of the columns before c are cube or robot, the
        // modes whose climb and fall swap upside down.
        std::vector<int> asym;
        // edge[i]: layer i to i + 1, at ((o * n) + b) * 2 + gravity on the
        // way (n: layer i + 1's choices, b one it may take).
        std::vector<std::vector<double>> edge;
        // ctg[i]: at b * 2 + gravity after b, the cheapest way from choice b
        // of layer i on to the level's end.
        std::vector<std::vector<double>> ctg;
        int edges = 0;
        double ms = 0.0;
    };
    Graph g_graph;

    Graph const& graphFor(Atlas const& a) {
        syncFails(a);
        if (g_graph.built && g_graph.atlas == a.id && g_graph.fails == g_failsVersion) return g_graph;
        auto const started = std::chrono::steady_clock::now();
        Graph gr;
        gr.atlas = a.id;
        gr.fails = g_failsVersion;
        gr.built = true;
        buildLayers(a, gr.layers);
        gr.asym.assign(a.columns.size() + 1, 0);
        for (size_t c = 0; c < a.columns.size(); c++)
            gr.asym[c + 1] = gr.asym[c] + ((a.columns[c].mode == 0 || a.columns[c].mode == 5) ? 1 : 0);
        const size_t L = gr.layers.size();
        gr.edge.resize(L > 0 ? L - 1 : 0);
        for (size_t i = 0; i + 1 < L; i++) {
            auto const& from = gr.layers[i];
            auto const& to = gr.layers[i + 1];
            const size_t n = to.choices.size();
            auto& e = gr.edge[i];
            e.assign(from.choices.size() * n * 2, 0.0);
            // Which way up only changes the price where a cube or robot is on the way.
            const bool sides = gr.asym[(size_t)to.column + 1] - gr.asym[(size_t)from.column + 1] > 0;
            for (size_t o = 0; o < from.choices.size(); o++) {
                const Interval start{from.choices[o].opt.band.lo, from.choices[o].opt.band.hi};
                for (size_t b = 0; b < to.eligible; b++) {
                    auto const& ch = to.choices[b];
                    const double price = failPrice(to.column, ch.gap.lo);
                    const double up = stepCost(a, from.column, start, to.column, ch.opt.band, false) + price;
                    const double down =
                        sides ? stepCost(a, from.column, start, to.column, ch.opt.band, true) + price : up;
                    e[(o * n + b) * 2] = up;
                    e[(o * n + b) * 2 + 1] = down;
                    gr.edges += sides ? 2 : 1;
                }
            }
        }
        // The cost to go, from the level's end back.
        gr.ctg.resize(L);
        if (L > 0) gr.ctg[L - 1].assign(gr.layers[L - 1].choices.size() * 2, 0.0);
        for (size_t k = L > 0 ? L - 1 : 0; k-- > 0;) {
            auto const& here = gr.layers[k];
            auto const& next = gr.layers[k + 1];
            const size_t n = next.choices.size();
            auto const& e = gr.edge[k];
            auto const& later = gr.ctg[k + 1];
            auto& out = gr.ctg[k];
            out.assign(here.choices.size() * 2, 0.0);
            for (size_t o = 0; o < here.choices.size(); o++)
                for (int g = 0; g < 2; g++) {
                    double best = std::numeric_limits<double>::infinity();
                    for (size_t b = 0; b < next.eligible; b++) {
                        const int g2 = gravityAfter(next.choices[b].opt, g);
                        best = std::min(best, e[(o * n + b) * 2 + (size_t)g] + later[b * 2 + (size_t)g2]);
                    }
                    out[o * 2 + (size_t)g] = best;
                }
        }
        gr.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        g_graph = std::move(gr);
        return g_graph;
    }

    // The first layer past column c (layers.size() when there is none).
    size_t firstAfter(Graph const& gr, int c) {
        size_t f = 0;
        while (f < gr.layers.size() && gr.layers[f].column <= c) f++;
        return f;
    }

    std::string rankingText(Atlas const& a, std::vector<Ranked> const& next, size_t most) {
        std::string out;
        for (size_t i = 0; i < next.size() && i < most; i++) {
            auto const& r = next[i];
            if (r.gate < 0 || (size_t)r.gate >= a.gates.size()) continue;
            out += fmt::format("{}{} (cost {:.1f})", out.empty() ? "" : "; ", describe(a.gates[(size_t)r.gate], r.option),
                               r.cost);
        }
        if (next.size() > most) out += fmt::format("; {} more", next.size() - most);
        return out;
    }

    std::unique_ptr<Plan> g_plan;
    uint64_t g_planAtlas = 0;  // the id of the map it was planned on
    bool g_routeShown = false;

    // Where the line is drawn at a waypoint: the middle of the band of the
    // option it takes, but near the floor of an open-sky gap (cube and
    // robot), not up in the sky.
    float drawY(Atlas const& a, Waypoint const& w) {
        auto const& col = a.columns[(size_t)std::clamp(w.column, 0, (int)a.columns.size() - 1)];
        auto const& band = w.option.band;
        const float h = band.hi - band.lo;
        if (col.mode == 0 || col.mode == 5) return band.lo + std::min(h * 0.5f, 45.0f);
        return band.lo + h * 0.5f;
    }

    bool sameStep(Waypoint const& l, Waypoint const& r) {
        return l.gate == r.gate && l.choice == r.choice;
    }

    CCDrawNode* draw(Atlas const& a, Plan const& p) {
        auto* node = CCDrawNode::create();
        node->setID("route-plan"_spr);
        if (p.routes.empty()) return node;
        auto const& best = p.routes.front();
        // The others first, thin and dim, only where they leave the best.
        for (size_t k = 1; k < p.routes.size(); k++) {
            auto const& r = p.routes[k];
            for (size_t i = 0; i + 1 < r.points.size() && i + 1 < best.points.size(); i++) {
                auto const& u = r.points[i];
                auto const& v = r.points[i + 1];
                if (sameStep(u, best.points[i]) && sameStep(v, best.points[i + 1])) continue;
                node->drawSegment(ccp(u.x, drawY(a, u)), ccp(v.x, drawY(a, v)), 1.0f, ccc4f(1.0f, 1.0f, 1.0f, 0.35f));
            }
        }
        // The best route on top: gold, and red where the map thinks the mode
        // cannot make it (physics will have the last word there).
        for (size_t i = 0; i + 1 < best.points.size(); i++) {
            auto const& u = best.points[i];
            auto const& v = best.points[i + 1];
            const bool doubtful = v.step >= kOffMap;
            node->drawSegment(ccp(u.x, drawY(a, u)), ccp(v.x, drawY(a, v)), 2.0f,
                              doubtful ? ccc4f(1.0f, 0.25f, 0.25f, 0.95f) : ccc4f(1.0f, 0.8f, 0.2f, 0.95f));
        }
        // Step 4: the options at the gates on the way. Each gate's own options
        // get their letter (U use, S skip, T touch, A avoid, F fire), dim, at
        // their band; the one the route takes is gold and larger, just above
        // where the route crosses, with a dot there.
        for (size_t i = 1; i < best.points.size(); i++) {
            auto const& w = best.points[i];
            auto it = std::lower_bound(a.gates.begin(), a.gates.end(), w.column,
                                       [](Gate const& g, int c) { return g.column < c; });
            for (; it != a.gates.end() && it->column == w.column; ++it) {
                const int gi = (int)(it - a.gates.begin());
                for (size_t k = 0; k < it->choices.size(); k++) {
                    auto const& o = it->choices[k];
                    if (o.kind == OptionKind::Free || (gi == w.gate && (int)k == w.choice)) continue;
                    drawMark(node, o.kind, it->x + 4.0f, markY(o.band), 8.0f, 0.55f);
                }
            }
            if (w.option.kind == OptionKind::Free) continue;
            const float y = drawY(a, w);
            node->drawDot(ccp(w.x, y), 2.5f, ccc4f(1.0f, 0.8f, 0.2f, 0.95f));
            drawMark(node, w.option.kind, w.x + 3.0f, y + 11.0f, 11.0f, 0.95f, true);
        }
        return node;
    }

}  // namespace

Plan plan(Atlas const& a, float fromX, float fromY, int fromGravity) {
    auto const started = std::chrono::steady_clock::now();
    Plan p;
    p.fromX = fromX;
    p.fromY = fromY;
    p.fromGravity = fromGravity ? 1 : 0;
    if (a.columns.empty()) return p;
    Graph const& gr = graphFor(a);

    // The layers: where the player is, then each column with gates ahead.
    const int startColumn = std::clamp((int)std::floor(fromX / kColumnWidth), 0, (int)a.columns.size() - 1);
    const size_t L = gr.layers.size();
    const size_t f = firstAfter(gr, startColumn);
    p.layers = (int)(L - f);
    Waypoint start;
    start.column = startColumn;
    start.x = fromX;
    start.band = Opening{fromY, fromY};
    start.option.band = start.band;
    start.gravity = p.fromGravity;
    if (f >= L) {
        // No gate ahead: the route is where it is.
        Route only;
        only.points.push_back(start);
        p.routes.push_back(std::move(only));
        p.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return p;
    }

    // Cheapest-first, kRoutesKept per option and gravity: the layers only
    // ever go forward, so each gate's best ways come from the one before it.
    struct Label {
        double cost;
        int prev;         // the choice at the layer before (-1: the start)
        int prevGravity;  // ... and the gravity after it
        int prevRank;
        double step;      // what arriving here cost
    };
    // labels[i - f][b * 2 + g]: the ways to choice b of layer i that leave
    // gravity g after it.
    std::vector<std::vector<std::vector<Label>>> labels(L - f);
    {
        auto const& first = gr.layers[f];
        labels[0].resize(first.choices.size() * 2);
        const Interval at{fromY, fromY};
        for (size_t b = 0; b < first.eligible; b++) {
            auto const& ch = first.choices[b];
            const double step = stepCost(a, startColumn, at, first.column, ch.opt.band, p.fromGravity == 1) +
                                failPrice(first.column, ch.gap.lo);
            p.edges++;
            const int g2 = gravityAfter(ch.opt, p.fromGravity);
            labels[0][b * 2 + (size_t)g2].push_back(Label{step, -1, p.fromGravity, -1, step});
        }
    }
    struct Candidate {
        double cost;
        int prev;
        int prevGravity;
        int prevRank;
        double step;
    };
    std::array<std::vector<Candidate>, 2> candidates;
    for (size_t i = f + 1; i < L; i++) {
        auto const& from = gr.layers[i - 1];
        auto const& here = gr.layers[i];
        const size_t n = here.choices.size();
        auto const& before = labels[i - 1 - f];
        auto& now = labels[i - f];
        now.resize(n * 2);
        auto const& e = gr.edge[i - 1];
        for (size_t b = 0; b < here.eligible; b++) {
            candidates[0].clear();
            candidates[1].clear();
            for (size_t o = 0; o < from.eligible; o++)
                for (int g = 0; g < 2; g++) {
                    auto const& ways = before[o * 2 + (size_t)g];
                    if (ways.empty()) continue;
                    const double step = e[(o * n + b) * 2 + (size_t)g];
                    p.edges++;
                    const int g2 = gravityAfter(here.choices[b].opt, g);
                    for (size_t r = 0; r < ways.size(); r++)
                        candidates[(size_t)g2].push_back(Candidate{ways[r].cost + step, (int)o, g, (int)r, step});
                }
            for (size_t g2 = 0; g2 < 2; g2++) {
                auto& cs = candidates[g2];
                std::sort(cs.begin(), cs.end(), [](Candidate const& l, Candidate const& r) {
                    const int64_t kl = costKey(l.cost);
                    const int64_t kr = costKey(r.cost);
                    if (kl != kr) return kl < kr;
                    if (l.prev != r.prev) return l.prev < r.prev;
                    if (l.prevGravity != r.prevGravity) return l.prevGravity < r.prevGravity;
                    return l.prevRank < r.prevRank;
                });
                for (size_t k = 0; k < cs.size() && k < (size_t)kRoutesKept; k++)
                    now[b * 2 + g2].push_back(Label{cs[k].cost, cs[k].prev, cs[k].prevGravity, cs[k].prevRank, cs[k].step});
            }
        }
    }

    // The cheapest ways to the last gate, traced back.
    struct End {
        double cost;
        int choice;
        int gravity;
        int rank;
    };
    std::vector<End> ends;
    auto const& last = labels[L - 1 - f];
    for (size_t b = 0; b < gr.layers[L - 1].eligible; b++)
        for (int g = 0; g < 2; g++) {
            auto const& ways = last[b * 2 + (size_t)g];
            for (size_t r = 0; r < ways.size(); r++) ends.push_back(End{ways[r].cost, (int)b, g, (int)r});
        }
    std::sort(ends.begin(), ends.end(), [](End const& l, End const& r) {
        const int64_t kl = costKey(l.cost);
        const int64_t kr = costKey(r.cost);
        if (kl != kr) return kl < kr;
        if (l.choice != r.choice) return l.choice < r.choice;
        if (l.gravity != r.gravity) return l.gravity < r.gravity;
        return l.rank < r.rank;
    });
    for (size_t k = 0; k < ends.size() && k < (size_t)kRoutesKept; k++) {
        Route route;
        route.cost = ends[k].cost;
        route.points.resize(1 + (L - f));
        route.points[0] = start;
        int b = ends[k].choice;
        int g = ends[k].gravity;
        int r = ends[k].rank;
        for (size_t i = L; i-- > f;) {
            auto const& lab = labels[i - f][(size_t)b * 2 + (size_t)g][(size_t)r];
            auto const& layer = gr.layers[i];
            auto const& ch = layer.choices[(size_t)b];
            Waypoint w;
            w.column = layer.column;
            w.x = ch.x;
            w.band = ch.gap;
            w.step = lab.step;
            w.gate = ch.gate;
            w.choice = ch.index;
            w.option = ch.opt;
            w.gravity = g;
            route.points[1 + (i - f)] = w;
            b = lab.prev;
            g = lab.prevGravity;
            r = lab.prevRank;
            if (b < 0) break;
        }
        p.routes.push_back(std::move(route));
    }
    p.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return p;
}

std::vector<Ranked> nextChoices(Atlas const& a, float x, float y, int gravity) {
    std::vector<Ranked> out;
    if (a.columns.empty()) return out;
    Graph const& gr = graphFor(a);
    gravity = gravity ? 1 : 0;
    const int column = std::clamp((int)std::floor(x / kColumnWidth), 0, (int)a.columns.size() - 1);
    const size_t f = firstAfter(gr, column);
    if (f >= gr.layers.size()) return out;
    auto const& layer = gr.layers[f];
    auto const& later = gr.ctg[f];
    const Interval at{y, y};
    for (size_t b = 0; b < layer.choices.size(); b++) {
        auto const& ch = layer.choices[b];
        Ranked r;
        r.gate = ch.gate;
        r.choice = ch.index;
        r.x = ch.x;
        r.option = ch.opt;
        r.gravity = gravityAfter(ch.opt, gravity);
        r.step = stepCost(a, column, at, layer.column, ch.opt.band, gravity == 1) + failPrice(layer.column, ch.gap.lo);
        r.cost = r.step + later[b * 2 + (size_t)r.gravity];
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](Ranked const& l, Ranked const& r) {
        const bool lf = l.option.kind == OptionKind::Free;
        const bool rf = r.option.kind == OptionKind::Free;
        if (lf != rf) return rf;  // FREE last
        const int64_t kl = costKey(l.cost);
        const int64_t kr = costKey(r.cost);
        if (kl != kr) return kl < kr;
        const int ql = kindRank(l.option.kind);
        const int qr = kindRank(r.option.kind);
        if (ql != qr) return ql < qr;
        if (l.option.band.lo != r.option.band.lo) return l.option.band.lo < r.option.band.lo;
        if (l.gate != r.gate) return l.gate < r.gate;
        return l.choice < r.choice;
    });
    return out;
}

std::vector<Ranked> nextChoices(GJBaseGameLayer* pl, float x, float y, int gravity) {
    auto const* a = atlasFor(pl);
    if (!a) return {};
    if (gravity < 0) gravity = pl->m_player1 && pl->m_player1->m_isUpsideDown ? 1 : 0;
    return nextChoices(*a, x, y, gravity);
}

const Plan* shownPlan(GJBaseGameLayer* pl) {
    auto const* a = atlasFor(pl);
    if (!a || !pl->m_player1) return nullptr;
    if (!g_plan || g_planAtlas != a->id) {
        auto* p1 = pl->m_player1;
        const float half = kHalf * (p1->m_vehicleSize < 0.8f ? 0.6f : 1.0f);
        const float x = p1->getPositionX();
        const float y = std::max(p1->getPositionY(), 90.0f + half);
        const int gravity = p1->m_isUpsideDown ? 1 : 0;
        g_plan = std::make_unique<Plan>(plan(*a, x, y, gravity));
        g_planAtlas = a->id;
        log::info("[AbsPF] route plan: {}", summary(*g_plan));
        // What it would do at the next gate from here, best first (step 4):
        // the ranking a decision there would start from.
        auto const next = nextChoices(*a, x, y, gravity);
        if (!next.empty()) log::info("[AbsPF] route: the next gate's options from here, best first: {}", rankingText(*a, next, 6));
    }
    return g_plan.get();
}

std::string summary(Plan const& p) {
    if (p.routes.empty()) return fmt::format("no route found from x {:.0f} ({} gates)", p.fromX, p.layers);
    auto const& best = p.routes.front();
    int doubtful = 0;
    int taken[6] = {};
    for (auto const& w : best.points) {
        doubtful += w.step >= kOffMap ? 1 : 0;
        if (w.gate >= 0) taken[std::clamp((int)w.option.kind, 0, 5)]++;
    }
    std::string options;
    for (OptionKind k : {OptionKind::Use, OptionKind::Skip, OptionKind::Touch, OptionKind::Avoid, OptionKind::Fire}) {
        if (taken[(int)k] == 0) continue;
        options += fmt::format("{}{} {}", options.empty() ? "" : ", ", optionKindName(k), taken[(int)k]);
    }
    return fmt::format(
        "best route from x {:.0f}{} through {} gates, cost {:.0f}{}{}; {} kept, {} ways priced in {:.1f} ms", p.fromX,
        p.fromGravity ? " (upside down)" : "", p.layers, best.cost,
        doubtful ? fmt::format(", {} stretches the map doubts (red)", doubtful) : std::string(),
        options.empty() ? std::string()
                        : fmt::format("; at orbs, pads and portals it would {} (letters on the route: U use, S skip, "
                                      "T touch, A avoid, F fire)",
                                      options),
        p.routes.size(), p.edges, p.ms);
}

namespace {
    std::unique_ptr<Plan> g_guide;
    uint64_t g_guideAtlas = 0;  // the id of the map it was planned on
    // Follow the planned route: read from the mod's saved values the first
    // time it is asked for (-1: not yet), then kept here - every decision asks.
    constexpr const char* kGuidanceKey = "pf_route_mode";
    int g_guidance = -1;
    int guidanceValue() {
        if (g_guidance < 0) g_guidance = std::clamp(Mod::get()->getSavedValue<int>(kGuidanceKey, 0), 0, 2);
        return g_guidance;
    }
}

const Plan* guidePlan(GJBaseGameLayer* pl) {
    auto const* a = atlasFor(pl);
    if (!a) return nullptr;
    syncFails(*a);
    if (!g_guide || g_guideAtlas != a->id || g_replan) {
        g_replan = false;
        // The level starts on the floor: y 105, or 99 when it starts mini;
        // upside down if the level's settings say so.
        const bool mini = pl->m_levelSettings && pl->m_levelSettings->m_startMini;
        const bool flipped = pl->m_levelSettings && pl->m_levelSettings->m_isFlipped;
        g_guide = std::make_unique<Plan>(plan(*a, 0.0f, 90.0f + kHalf * (mini ? 0.6f : 1.0f), flipped ? 1 : 0));
        g_guideAtlas = a->id;
        log::info("[AbsPF] route to follow: {}", summary(*g_guide));
    }
    return g_guide.get();
}

Guidance guidance() {
    return (Guidance)guidanceValue();
}

void setGuidance(Guidance g) {
    g_guidance = std::clamp((int)g, 0, 2);
    Mod::get()->setSavedValue<int>(kGuidanceKey, g_guidance);
}

bool guidanceEnabled() {
    return guidanceValue() != (int)Guidance::Off;
}

bool drivesEnabled() {
    return guidanceValue() == (int)Guidance::Drives;
}

void noteLeg(float gateX, float lo, bool found) {
    if (found) return;
    const int column = (int)std::floor(gateX / kColumnWidth);
    int& n = g_fails[{column, (int)std::lround(lo)}];
    if (++n == kFailsToPrice) {
        g_replan = true;
        g_failsVersion++;  // the gate graph is priced again with it
        log::info("[AbsPF] route: the gap at x {:.0f} (from y {:.0f}) failed {} times; planning again", gateX, lo, n);
    }
}

bool routeShown() {
    return g_routeShown;
}

void setRouteShown(bool on) {
    g_routeShown = on;
    if (on) g_plan.reset();  // switched on again: planned afresh from where the player is
}

void showLeg(PlayLayer* pl, std::vector<std::pair<float, float>> const& dots,
             std::vector<std::pair<float, float>> const& path) {
    if (!pl || !pl->m_objectLayer) return;
    if (auto* old = pl->m_objectLayer->getChildByID("route-leg"_spr)) old->removeFromParent();
    auto* node = CCDrawNode::create();
    node->setID("route-leg"_spr);
    for (auto const& [x, y] : dots) node->drawDot(ccp(x, y), 1.5f, ccc4f(0.4f, 0.9f, 1.0f, 0.45f));
    for (size_t i = 0; i + 1 < path.size(); i++)
        node->drawSegment(ccp(path[i].first, path[i].second), ccp(path[i + 1].first, path[i + 1].second), 1.5f,
                          ccc4f(0.3f, 1.0f, 0.4f, 0.95f));
    pl->m_objectLayer->addChild(node, 1502);
}

void tickRoute(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;
    auto* existing = pl->m_objectLayer->getChildByID("route-plan"_spr);
    if (!g_routeShown) {
        if (existing) existing->removeFromParent();
        return;
    }
    // Drawn again when the map is built again (and the route planned again on it).
    auto const* a = atlasFor(pl);
    if (existing && a && existing->getTag() == (int)a->id) return;
    if (existing) existing->removeFromParent();
    auto const* p = shownPlan(pl);
    if (!a || !p) return;
    auto* node = draw(*a, *p);
    node->setTag((int)a->id);
    pl->m_objectLayer->addChild(node, 1501);
}

}  // namespace absense::route
