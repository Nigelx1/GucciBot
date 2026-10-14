#include "absense/route/planner.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <tuple>

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
        switch (mode) {
            case 1: rise = 30.0f; fall = 30.0f; break;                        // ship
            case 2: rise = 60.0f; fall = 60.0f; break;                        // ball
            case 3: rise = 30.0f; fall = 45.0f; break;                        // ufo
            case 4: rise = mini ? 60.0f : 30.0f; fall = rise; break;          // wave: 45 degrees, steeper mini
            case 6: rise = 1.0e6f; fall = 1.0e6f; break;                      // spider
            case 7: rise = 45.0f; fall = 45.0f; break;                        // swing
            default: rise = 30.0f; fall = 60.0f; break;                       // cube, robot
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
    // column c1 > c0) through the map's gaps, column by column.
    double stepCost(Atlas const& a, int c0, Interval from, int c1, Opening to) {
        std::vector<Interval> reach{from};
        std::vector<Interval> next;
        double cost = 0.0;
        for (int c = c0 + 1; c <= c1; c++) {
            auto const& col = a.columns[(size_t)c];
            float rise, fall;
            envelope(col.mode, col.mini, rise, fall);
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

    struct Layer {
        int column = 0;
        float x = 0.0f;
        std::vector<Opening> options;
    };

    struct Label {
        double cost;
        int prevOption;
        int prevRank;
        double step;  // what arriving here cost
    };

    std::unique_ptr<Plan> g_plan;
    const Atlas* g_planAtlas = nullptr;
    bool g_routeShown = false;

    // Where the line is drawn in a band: its middle, but near the floor of an
    // open-sky gap (cube and robot), not up in the sky.
    float drawY(Atlas const& a, Waypoint const& w) {
        auto const& col = a.columns[(size_t)std::clamp(w.column, 0, (int)a.columns.size() - 1)];
        const float h = w.band.hi - w.band.lo;
        if (col.mode == 0 || col.mode == 5) return w.band.lo + std::min(h * 0.5f, 45.0f);
        return w.band.lo + h * 0.5f;
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
                const bool same = std::fabs(u.band.lo - best.points[i].band.lo) < 0.5f &&
                                  std::fabs(v.band.lo - best.points[i + 1].band.lo) < 0.5f;
                if (same) continue;
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
        return node;
    }

}  // namespace

Plan plan(Atlas const& a, float fromX, float fromY) {
    auto const started = std::chrono::steady_clock::now();
    Plan p;
    p.fromX = fromX;
    p.fromY = fromY;
    if (a.columns.empty()) return p;

    // The layers: where the player is, then each column with a gate ahead
    // (several gates in one column share its gaps).
    const int startColumn = std::clamp((int)std::floor(fromX / kColumnWidth), 0, (int)a.columns.size() - 1);
    std::vector<Layer> layers;
    layers.push_back({startColumn, fromX, {Opening{fromY, fromY}}});
    for (auto const& g : a.gates) {
        if (g.column <= layers.back().column || g.options.empty()) continue;
        layers.push_back({g.column, g.x, g.options});
    }
    p.layers = (int)layers.size() - 1;

    // Cheapest-first, kRoutesKept per gap: the layers only ever go forward, so
    // each gate's best ways come from the one before it.
    std::vector<std::vector<std::vector<Label>>> labels(layers.size());
    labels[0] = {{Label{0.0, -1, -1, 0.0}}};
    for (size_t i = 1; i < layers.size(); i++) {
        auto const& from = layers[i - 1];
        auto const& here = layers[i];
        labels[i].resize(here.options.size());
        for (size_t b = 0; b < here.options.size(); b++) {
            std::vector<std::tuple<double, int, int, double>> candidates;
            for (size_t o = 0; o < from.options.size(); o++) {
                if (labels[i - 1][o].empty()) continue;
                const double step = stepCost(a, from.column, Interval{from.options[o].lo, from.options[o].hi},
                                             here.column, here.options[b]);
                p.edges++;
                for (size_t r = 0; r < labels[i - 1][o].size(); r++)
                    candidates.emplace_back(labels[i - 1][o][r].cost + step, (int)o, (int)r, step);
            }
            std::sort(candidates.begin(), candidates.end());
            for (size_t k = 0; k < candidates.size() && k < (size_t)kRoutesKept; k++) {
                auto const& [cost, o, r, step] = candidates[k];
                labels[i][b].push_back(Label{cost, o, r, step});
            }
        }
    }

    // The cheapest ways to the last gate, traced back.
    const size_t last = layers.size() - 1;
    std::vector<std::tuple<double, int, int>> ends;
    for (size_t b = 0; b < labels[last].size(); b++)
        for (size_t r = 0; r < labels[last][b].size(); r++) ends.emplace_back(labels[last][b][r].cost, (int)b, (int)r);
    std::sort(ends.begin(), ends.end());
    for (size_t k = 0; k < ends.size() && k < (size_t)kRoutesKept; k++) {
        auto [cost, b, r] = ends[k];
        Route route;
        route.cost = cost;
        route.points.resize(layers.size());
        for (size_t i = last + 1; i-- > 0;) {
            auto const& lab = labels[i][(size_t)b][(size_t)r];
            route.points[i] = Waypoint{layers[i].column, layers[i].x, layers[i].options[(size_t)b], lab.step};
            b = lab.prevOption;
            r = lab.prevRank;
            if (b < 0) break;
        }
        p.routes.push_back(std::move(route));
    }
    p.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return p;
}

const Plan* shownPlan(GJBaseGameLayer* pl) {
    auto const* a = atlasFor(pl);
    if (!a || !pl->m_player1) return nullptr;
    if (!g_plan || g_planAtlas != a) {
        const float half = kHalf * (pl->m_player1->m_vehicleSize < 0.8f ? 0.6f : 1.0f);
        const float y = std::max(pl->m_player1->getPositionY(), 90.0f + half);
        g_plan = std::make_unique<Plan>(plan(*a, pl->m_player1->getPositionX(), y));
        g_planAtlas = a;
        log::info("[AbsPF] route plan: {}", summary(*g_plan));
    }
    return g_plan.get();
}

std::string summary(Plan const& p) {
    if (p.routes.empty()) return fmt::format("no route found from x {:.0f} ({} gates)", p.fromX, p.layers);
    auto const& best = p.routes.front();
    int doubtful = 0;
    for (auto const& w : best.points) doubtful += w.step >= kOffMap ? 1 : 0;
    return fmt::format("best route from x {:.0f} through {} gates, cost {:.0f}{}; {} kept, {} ways priced in {:.1f} ms",
                       p.fromX, p.layers, best.cost,
                       doubtful ? fmt::format(", {} stretches the map doubts (red)", doubtful) : std::string(),
                       p.routes.size(), p.edges, p.ms);
}

bool routeShown() {
    return g_routeShown;
}

void setRouteShown(bool on) {
    g_routeShown = on;
    if (on) g_plan.reset();  // switched on again: planned afresh from where the player is
}

void tickRoute(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;
    auto* existing = pl->m_objectLayer->getChildByID("route-plan"_spr);
    if (!g_routeShown) {
        if (existing) existing->removeFromParent();
        return;
    }
    if (existing) return;
    auto const* a = atlasFor(pl);
    if (auto const* p = shownPlan(pl); a && p) pl->m_objectLayer->addChild(draw(*a, *p), 1501);
}

}  // namespace absense::route
