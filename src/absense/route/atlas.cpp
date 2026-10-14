#include "absense/route/atlas.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "absense/gpu/classify.hpp"
#include "absense/world/world.hpp"

using namespace geode::prelude;

namespace absense::route {

namespace {

    // The floor's top: a cube standing on it is at y 105 (half its 30-unit
    // hitbox higher).
    constexpr float kGroundTop = 90.0f;
    // Half the player's hitbox at normal size; mini is 0.6 of it. Every mode
    // uses this here, the wave included: whether its hitbox is smaller for
    // hazards is not settled, so the map may draw wave gaps a little narrow.
    constexpr float kPlayerHalf = 15.0f;
    // A gap whose room for the centre is under this many player halves is
    // tight: physically, under about two player heights.
    constexpr float kTightHalves = 2.0f;
    // At least one gate this many columns apart (600 units, about two seconds
    // at normal speed), so no stretch of the route is left unplanned.
    constexpr int kGateSpacing = 20;

    enum Kind : uint32_t {
        kSolid = 0, kHazard = 1, kJumpOrb = 2, kGravityOrb = 3, kJumpPad = 4, kGravityPad = 5,
        kGravityPortalDown = 6, kGravityPortalUp = 7, kSlope = 8, kSpeedPortal = 9, kModePortal = 10,
        kSizePortal = 11, kGravityToggle = 12, kDropOrb = 13, kSpiderOrb = 14, kSpiderPad = 15,
        kDashOrb = 16, kGravityDashOrb = 17,
    };

    bool isOrb(uint32_t k) {
        return k == kJumpOrb || k == kGravityOrb || k == kDropOrb || k == kSpiderOrb || k == kDashOrb ||
               k == kGravityDashOrb;
    }
    bool isPad(uint32_t k) { return k == kJumpPad || k == kGravityPad || k == kSpiderPad; }
    bool isPortal(uint32_t k) {
        return k == kGravityPortalDown || k == kGravityPortalUp || k == kSpeedPortal || k == kModePortal ||
               k == kSizePortal || k == kGravityToggle;
    }
    bool blocks(uint32_t k) { return k == kSolid || k == kHazard || k == kSlope; }

    // The room a flying mode gives, floor to ceiling: ten blocks for the
    // ship, UFO, wave and swing, eight for the ball and spider. Cube and
    // robot have no ceiling (0 here).
    float corridorHeight(uint8_t mode) {
        switch (mode) {
            case 1: case 3: case 4: case 7: return 300.0f;
            case 2: case 6: return 240.0f;
            default: return 0.0f;
        }
    }

    // Where a flying mode's room starts when a portal at `y` sets it: centred
    // on the portal, on the block grid, never below the floor. An estimate of
    // what the game's updateDualGround works out; the overlay shows when it
    // is off.
    float corridorBottom(uint8_t mode, float y) {
        const float h = corridorHeight(mode);
        if (h <= 0.0f) return kGroundTop;
        return std::max(kGroundTop, std::round((y - h * 0.5f) / 30.0f) * 30.0f);
    }

    struct Box {
        gpu::Object o;
        bool moving = false;
        float cx() const { return (o.minX + o.maxX) * 0.5f; }
        float cy() const { return (o.minY + o.maxY) * 0.5f; }
    };

    struct Change {
        float x;
        int mode;      // -1: not a mode change
        int mini;      // -1: not a size change
        float y;       // the portal's centre (a flying mode's room is set from it)
    };

    std::unique_ptr<Atlas> g_atlas;
    GJBaseGameLayer* g_atlasFor = nullptr;
    int g_atlasObjects = -1;
    bool g_overlay = false;

    void subtract(float lo, float hi, std::vector<std::pair<float, float>>& blocked, std::vector<Opening>& out) {
        out.clear();
        if (hi <= lo) return;
        std::sort(blocked.begin(), blocked.end());
        float at = lo;
        for (auto const& [b0, b1] : blocked) {
            if (b1 <= at) continue;
            if (b0 >= hi) break;
            if (b0 > at && b0 - at > 1.0f) out.push_back({at, std::min(b0, hi)});
            at = std::max(at, b1);
            if (at >= hi) break;
        }
        if (hi - at > 1.0f) out.push_back({at, hi});
    }

    std::unique_ptr<Atlas> build(GJBaseGameLayer* pl) {
        auto const started = std::chrono::steady_clock::now();
        auto a = std::make_unique<Atlas>();
        if (pl->m_level) a->level = std::string(pl->m_level->m_levelName);

        // 1. Every object, once: what it is to the pathfinder, and whether
        // something moves or toggles it.
        std::vector<Box> boxes;
        std::vector<float> unknownX;
        const int n = pl->m_objects ? pl->m_objects->count() : 0;
        boxes.reserve((size_t)n);
        float levelEnd = pl->m_levelLength;
        float levelTop = kGroundTop + 600.0f;
        for (int i = 0; i < n; i++) {
            auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!obj) continue;
            if (gpu::objectChangesEverything(obj)) {
                unknownX.push_back(obj->getPositionX());
                a->unknownObjects++;
                continue;
            }
            Box b;
            if (!gpu::classifyObject(obj, b.o)) continue;
            b.moving = world::certaintyOf(pl, obj) != world::Certainty::Static;
            if (b.moving) a->movingObjects++;
            levelEnd = std::max(levelEnd, b.o.maxX);
            if (blocks(b.o.kind)) levelTop = std::max(levelTop, b.o.maxY + 150.0f);
            boxes.push_back(b);
        }
        a->objects = (int)boxes.size();
        a->length = levelEnd;
        std::sort(unknownX.begin(), unknownX.end());
        a->unknownX = unknownX;

        // 2. The mode and size in force along the level, from the portals.
        std::vector<Change> changes;
        for (auto const& b : boxes) {
            if (b.o.kind == kModePortal) changes.push_back({b.cx(), (int)std::lround(b.o.power), -1, b.cy()});
            if (b.o.kind == kSizePortal) changes.push_back({b.cx(), -1, b.o.power < 0.8f ? 1 : 0, b.cy()});
        }
        std::sort(changes.begin(), changes.end(), [](Change const& l, Change const& r) { return l.x < r.x; });
        uint8_t mode = 0;
        bool mini = false;
        if (auto* s = pl->m_levelSettings) {
            mode = (uint8_t)std::clamp(s->m_startMode, 0, 7);
            mini = s->m_startMini;
        }
        float bottom = kGroundTop;

        // 3. Columns: which objects can touch a player whose centre is in
        // each one, then the gaps that are left for the centre.
        const int columns = std::max(1, (int)std::ceil(levelEnd / kColumnWidth) + 1);
        a->columns.resize((size_t)columns);
        std::vector<std::vector<int>> bucket((size_t)columns);
        const float reach = kPlayerHalf;  // the widest the player gets sideways
        for (int i = 0; i < (int)boxes.size(); i++) {
            auto const& o = boxes[(size_t)i].o;
            const int c0 = std::max(0, (int)std::floor((o.minX - reach) / kColumnWidth));
            const int c1 = std::min(columns - 1, (int)std::floor((o.maxX + reach) / kColumnWidth));
            for (int c = c0; c <= c1; c++) bucket[(size_t)c].push_back(i);
        }
        for (float x : unknownX) {
            const int c = std::clamp((int)std::floor(x / kColumnWidth), 0, columns - 1);
            a->columns[(size_t)c].unknown = true;
        }

        size_t next = 0;
        std::vector<std::pair<float, float>> blocked;
        for (int c = 0; c < columns; c++) {
            auto& col = a->columns[(size_t)c];
            col.x0 = (float)c * kColumnWidth;
            const float centre = col.x0 + kColumnWidth * 0.5f;
            while (next < changes.size() && changes[next].x <= centre) {
                auto const& ch = changes[next++];
                if (ch.mode >= 0) {
                    mode = (uint8_t)std::clamp(ch.mode, 0, 7);
                    bottom = corridorBottom(mode, ch.y);
                }
                if (ch.mini >= 0) mini = ch.mini == 1;
            }
            col.mode = mode;
            col.mini = mini;
            const float half = kPlayerHalf * (mini ? 0.6f : 1.0f);
            const float h = corridorHeight(mode);
            col.bandLo = (h > 0.0f ? bottom : kGroundTop) + half;
            col.bandHi = h > 0.0f ? bottom + h - half : levelTop;

            // Solids and hazards both stop the centre a half hitbox away up
            // and down: the player rides a block's top or bottom rather than
            // sinking into it (the inner hitbox only matters side-on, which
            // a gap's height does not show). Moving ones are left out: where
            // they are depends on when, which is the simulation's job.
            blocked.clear();
            for (int i : bucket[(size_t)c]) {
                auto const& b = boxes[(size_t)i];
                if (!blocks(b.o.kind)) continue;
                if (b.moving) {
                    col.dynamic = true;
                    continue;
                }
                if (b.o.radius > 0.0f) {
                    const float cy = b.cy();
                    blocked.push_back({cy - b.o.radius - half, cy + b.o.radius + half});
                } else {
                    blocked.push_back({b.o.minY - half, b.o.maxY + half});
                }
            }
            subtract(col.bandLo, col.bandHi, blocked, col.openings);
        }

        // 4. Gates: orbs, pads and portals; forks where a gap splits; tight
        // gaps; and one at least every kGateSpacing columns.
        std::vector<Gate> gates;
        auto optionsAt = [&](int c) { return a->columns[(size_t)c].openings; };
        for (auto const& b : boxes) {
            GateKind kind;
            if (isOrb(b.o.kind)) kind = GateKind::Orb;
            else if (isPad(b.o.kind)) kind = GateKind::Pad;
            else if (isPortal(b.o.kind)) kind = GateKind::Portal;
            else continue;
            const int c = std::clamp((int)std::floor(b.cx() / kColumnWidth), 0, columns - 1);
            gates.push_back({kind, c, b.cx(), b.cy(), optionsAt(c)});
        }
        int lastGate = -1000;
        for (int c = 1; c < columns; c++) {
            auto const& col = a->columns[(size_t)c];
            auto const& prev = a->columns[(size_t)c - 1];
            if (col.openings.empty()) continue;
            const float half = kPlayerHalf * (col.mini ? 0.6f : 1.0f);
            bool tight = false;
            for (auto const& o : col.openings)
                if (o.hi - o.lo < kTightHalves * half) tight = true;
            if (!prev.openings.empty() && col.openings.size() > prev.openings.size()) {
                gates.push_back({GateKind::Fork, c, col.x0, 0.0f, col.openings});
                lastGate = c;
            } else if (tight && c - lastGate > 3) {
                gates.push_back({GateKind::Tight, c, col.x0, 0.0f, col.openings});
                lastGate = c;
            }
        }
        std::sort(gates.begin(), gates.end(), [](Gate const& l, Gate const& r) {
            return l.column != r.column ? l.column < r.column : (int)l.kind < (int)r.kind;
        });
        lastGate = 0;
        std::vector<Gate> spaced;
        size_t gi = 0;
        for (int c = 0; c < columns; c++) {
            bool here = false;
            while (gi < gates.size() && gates[gi].column == c) {
                spaced.push_back(gates[gi++]);
                here = true;
            }
            if (here) {
                lastGate = c;
            } else if (c - lastGate >= kGateSpacing && !a->columns[(size_t)c].openings.empty()) {
                spaced.push_back({GateKind::Spacing, c, a->columns[(size_t)c].x0, 0.0f, optionsAt(c)});
                lastGate = c;
            }
        }
        a->gates = std::move(spaced);

        a->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        log::info("[AbsPF] level map: {}", summary(*a));
        return a;
    }

    ccColor4F gateColor(GateKind k) {
        switch (k) {
            case GateKind::Fork: return ccc4f(1.0f, 1.0f, 1.0f, 0.85f);
            case GateKind::Orb: return ccc4f(0.3f, 0.85f, 1.0f, 0.9f);
            case GateKind::Pad: return ccc4f(1.0f, 0.4f, 0.9f, 0.9f);
            case GateKind::Portal: return ccc4f(0.7f, 0.5f, 1.0f, 0.9f);
            case GateKind::Tight: return ccc4f(1.0f, 0.6f, 0.1f, 0.9f);
            default: return ccc4f(0.6f, 0.6f, 0.6f, 0.35f);
        }
    }

    CCDrawNode* draw(Atlas const& a) {
        auto* node = CCDrawNode::create();
        node->setID("route-atlas"_spr);
        const ccColor4F none = ccc4f(0.0f, 0.0f, 0.0f, 0.0f);
        const ccColor4F open = ccc4f(0.25f, 0.95f, 0.45f, 0.16f);
        const ccColor4F moving = ccc4f(1.0f, 0.85f, 0.2f, 0.18f);
        // Gaps: one rectangle per run of columns whose gaps are the same.
        size_t c = 0;
        while (c < a.columns.size()) {
            auto const& col = a.columns[c];
            size_t end = c + 1;
            auto same = [](Column const& l, Column const& r) {
                if (l.dynamic != r.dynamic || l.openings.size() != r.openings.size()) return false;
                for (size_t i = 0; i < l.openings.size(); i++)
                    if (std::fabs(l.openings[i].lo - r.openings[i].lo) > 0.5f ||
                        std::fabs(l.openings[i].hi - r.openings[i].hi) > 0.5f)
                        return false;
                return true;
            };
            while (end < a.columns.size() && same(col, a.columns[end])) end++;
            const float x0 = col.x0;
            const float x1 = (float)end * kColumnWidth;
            for (auto const& o : col.openings) {
                CCPoint v[4] = {ccp(x0, o.lo), ccp(x1, o.lo), ccp(x1, o.hi), ccp(x0, o.hi)};
                node->drawPolygon(v, 4, col.dynamic ? moving : open, 0.0f, none);
            }
            c = end;
        }
        // Gates: a line across the bands a route can be in; orbs, pads and
        // portals get a dot where they are.
        for (auto const& g : a.gates) {
            if (g.options.empty()) continue;
            const float lo = g.options.front().lo;
            const float hi = g.options.back().hi;
            const auto colour = gateColor(g.kind);
            node->drawSegment(ccp(g.x, lo), ccp(g.x, hi), g.kind == GateKind::Spacing ? 0.6f : 1.0f, colour);
            if (g.kind == GateKind::Orb || g.kind == GateKind::Pad || g.kind == GateKind::Portal)
                node->drawDot(ccp(g.x, g.y), 4.0f, colour);
        }
        // What the map cannot follow past: a red line, floor to sky.
        for (float x : a.unknownX)
            node->drawSegment(ccp(x, kGroundTop), ccp(x, kGroundTop + 900.0f), 1.5f, ccc4f(1.0f, 0.2f, 0.2f, 0.85f));
        return node;
    }

}  // namespace

const char* gateKindName(GateKind k) {
    switch (k) {
        case GateKind::Fork: return "fork";
        case GateKind::Orb: return "orb";
        case GateKind::Pad: return "pad";
        case GateKind::Portal: return "portal";
        case GateKind::Tight: return "tight";
        default: return "spacing";
    }
}

const Atlas* atlasFor(GJBaseGameLayer* pl) {
    if (!pl || !pl->m_objects || pl->m_loadingProgress < 1.0f) return nullptr;
    const int count = pl->m_objects->count();
    if (!g_atlas || g_atlasFor != pl || g_atlasObjects != count) {
        g_atlas = build(pl);
        g_atlasFor = pl;
        g_atlasObjects = count;
    }
    return g_atlas.get();
}

std::string summary(Atlas const& a) {
    int counts[6] = {};
    for (auto const& g : a.gates) counts[(int)g.kind]++;
    int moving = 0;
    for (auto const& c : a.columns) moving += c.dynamic ? 1 : 0;
    return fmt::format("{}: {} columns, {} objects ({} moving, {} it can't follow), {} gates (forks {}, orbs {}, pads {}, "
                       "portals {}, tight {}, spacing {}), {} columns with moving parts, built in {:.0f} ms",
                       a.level.empty() ? std::string("level") : a.level, a.columns.size(), a.objects, a.movingObjects,
                       a.unknownObjects, a.gates.size(), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
                       moving, a.buildMs);
}

bool overlayEnabled() {
    return g_overlay;
}

void setOverlayEnabled(bool on) {
    g_overlay = on;
}

void tickOverlay(PlayLayer* pl) {
    if (!pl || !pl->m_objectLayer) return;
    auto* existing = pl->m_objectLayer->getChildByID("route-atlas"_spr);
    if (!g_overlay) {
        if (existing) existing->removeFromParent();
        return;
    }
    if (existing) return;
    if (auto const* a = atlasFor(pl)) pl->m_objectLayer->addChild(draw(*a), 1500);
}

}  // namespace absense::route
