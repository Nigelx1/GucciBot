#include "absense/route/atlas.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_set>

#include "absense/compat/bot.hpp"
#include "absense/gpu/classify.hpp"
#include "absense/trajectory/trajectory.hpp"

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
    // The set-up of the level the map was built for (moverCacheGeneration):
    // a new level, or the same one opened again after an edit, can come back at
    // the same layer address with the same object count.
    unsigned g_atlasGeneration = ~0u;
    uint64_t g_nextAtlasId = 0;
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
        // something moves or toggles it. Only triggers that move a group (the
        // kinds Trajectory::scanMovedGroups follows) or switch it on and off
        // count: a colour, pulse or fade trigger naming a group changes
        // nothing about where its objects can hit. (The World's certaintyOf
        // counts any trigger naming a group, and with the World off, any
        // group at all, which made most of a level "moving" on the map.)
        const int n = pl->m_objects ? pl->m_objects->count() : 0;
        std::unordered_set<int> dynamicGroups;
        for (int i = 0; i < n; i++) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
            if (!o) continue;
            switch (o->m_objectID) {
                case 901: case 1346: case 1347: case 1814: case 2067:  // move, rotate, follow, follow player Y, scale
                case 3032: case 3033:                                  // keyframes
                case 3016: case 3660: case 3661:                       // advanced follow
                case 3006: case 3007: case 3008: case 3009: case 3010: case 3011:  // area effects
                case 1049:                                             // toggle
                case 1594: case 1595:                                  // toggle orb, touch
                case 1611: case 1811: case 1815:                       // count, instant count, collision
                    break;
                default:
                    continue;
            }
            if (auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o); e && e->m_targetGroupID > 0)
                dynamicGroups.insert(e->m_targetGroupID);
        }
        const auto movesOrToggles = [&](GameObject* o) {
            const cocos2d::CCPoint start = o->m_startPosition;
            if (std::fabs(o->getPositionX() - start.x) > 0.01f || std::fabs(o->getPositionY() - start.y) > 0.01f)
                return true;  // already moved
            if (o->m_groupCount <= 0 || !o->m_groups) return false;
            for (int g = 0; g < o->m_groupCount && g < 10; g++)
                if (dynamicGroups.count((int)(*o->m_groups)[(size_t)g]) != 0) return true;
            return false;
        };

        std::vector<Box> boxes;
        std::vector<float> unknownX;
        std::vector<float> toggledX;
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
            if ((obj->m_isGroupDisabled || obj->m_isDisabled) && movesOrToggles(obj)) {
                // Off now, but a trigger turns it on (or moves it) later:
                // the map makes no claim about its columns. Only for what the
                // player could touch once it is on: LIMBO switches 161,117
                // objects on and off, nearly all of them decoration, and
                // counting those had 2,463 of its 2,519 columns doubted.
                gpu::Object probe;
                if (!gpu::classifyObject(obj, probe, true)) continue;
                toggledX.push_back(obj->getPositionX());
                a->movingObjects++;
                continue;
            }
            Box b;
            if (!gpu::classifyObject(obj, b.o)) continue;
            b.moving = movesOrToggles(obj);
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

        // 3b. What moves, and when: a ghost runs the level in the simulation
        // with the level's own triggers (Trajectory::ghostRun), and each column
        // keeps what the triggers had in it while the ghost went through - the
        // level as the player meets it, moving blocks and blocks that appear
        // included. LIMBO is built out of those; left out, its map was "any
        // height" from one end to the other. Raw extents here, the player's
        // half added per column below.
        std::vector<uint8_t> timed((size_t)columns, 0);
        std::vector<std::vector<std::pair<float, float>>> timedBoxes((size_t)columns);   // (bottom, top)
        std::vector<std::vector<std::pair<float, float>>> timedRounds((size_t)columns);  // (centre y, radius)
        if (auto* traj = Bot::get()->trajectory().unsafeInner()) {
            std::vector<std::pair<float, float>> speeds;
            for (auto const& b : boxes)
                if (b.o.kind == kSpeedPortal) speeds.push_back({b.cx(), b.o.power});
            std::sort(speeds.begin(), speeds.end());
            // A column is read while the ghost is within a few units of its
            // middle, not the whole time it is in it: a block that moves
            // swept a tall stretch of the column over those 30 units (half a
            // second at the slowest speed), and the map called all of it
            // blocked - ground Nigel's LIMBO macro stands on at x 1036.
            constexpr float kMiddle = 6.0f;
            const auto middleOf = [](float x) { return (std::floor(x / kColumnWidth) + 0.5f) * kColumnWidth; };
            const auto report = traj->ghostRun(
                pl, speeds, 75.0f, 400000, 8000.0, [&](float x) { return std::fabs(x - middleOf(x)) <= kMiddle; },
                [&](float x, std::span<GameObject* const> nearby) {
                    const int gc = std::clamp((int)std::floor(x / kColumnWidth), 0, columns - 1);
                    timed[(size_t)gc] = 1;
                    // What can touch a player whose centre is in the ghost's column.
                    const float x0 = (float)gc * kColumnWidth - kPlayerHalf;
                    const float x1 = (float)(gc + 1) * kColumnWidth + kPlayerHalf;
                    for (GameObject* o : nearby) {
                        gpu::Object ob;
                        if (!gpu::classifyObject(o, ob) || !blocks(ob.kind)) continue;  // off just now, or nothing that stops a player
                        if (ob.maxX < x0 || ob.minX > x1) continue;
                        if (ob.radius > 0.0f) timedRounds[(size_t)gc].push_back({(ob.minY + ob.maxY) * 0.5f, ob.radius});
                        else timedBoxes[(size_t)gc].push_back({ob.minY, ob.maxY});
                    }
                });
            for (uint8_t t : timed) a->timedColumns += t;
            a->timedTo = report.reachedX;
            a->ghostMs = report.ms;
            a->ghostNote = report.why ? report.why : "";
            a->ghostBusy = !report.ran && report.why && std::string(report.why) == "the simulation is busy";
        }
        for (float x : toggledX) {
            const int c = std::clamp((int)std::floor(x / kColumnWidth), 0, columns - 1);
            for (int k = std::max(0, c - 1); k <= std::min(columns - 1, c + 1); k++)
                if (!timed[(size_t)k]) a->columns[(size_t)k].dynamic = true;
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
                    // Where it stands while the player is here is the ghost's
                    // to say (3b); without it, the map makes no claim.
                    if (!timed[(size_t)c]) col.dynamic = true;
                    continue;
                }
                if (b.o.radius > 0.0f) {
                    const float cy = b.cy();
                    blocked.push_back({cy - b.o.radius - half, cy + b.o.radius + half});
                } else {
                    blocked.push_back({b.o.minY - half, b.o.maxY + half});
                }
            }
            if (timed[(size_t)c]) {
                col.timed = true;
                for (auto const& [y0, y1] : timedBoxes[(size_t)c]) blocked.push_back({y0 - half, y1 + half});
                for (auto const& [cy, r] : timedRounds[(size_t)c]) blocked.push_back({cy - r - half, cy + r + half});
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
    // The ghost run (3b) needs the level's simulation, which is otherwise made
    // when the pathfinder first starts. Made here before the generation is
    // read: making it starts a new one, which would build the map twice.
    if (!Bot::get()->trajectory().exists() && PlayLayer::get()) Bot::get()->trajectory().init();
    const int count = pl->m_objects->count();
    const unsigned generation = moverCacheGeneration();
    // A map whose ghost could not run (the simulation was busy with a search)
    // is built again once a second until it can.
    static auto lastBuild = std::chrono::steady_clock::now();
    const bool retryGhost = g_atlas && g_atlas->ghostBusy &&
                            std::chrono::steady_clock::now() - lastBuild > std::chrono::seconds(1);
    // A ghost that ran out of time short of the end (LIMBO took 8.8 s for the
    // first 45%): once the player is within 2000 units of where it stopped,
    // the map is built again, the ghost starting from the player.
    const bool extendGhost = g_atlas && g_atlas->ghostNote == "out of time" && pl->m_player1 &&
                             pl->m_player1->getPositionX() > g_atlas->timedTo - 2000.0f &&
                             std::chrono::steady_clock::now() - lastBuild > std::chrono::seconds(1);
    if (!g_atlas || g_atlasFor != pl || g_atlasObjects != count || g_atlasGeneration != generation || retryGhost ||
        extendGhost) {
        lastBuild = std::chrono::steady_clock::now();
        g_atlas = build(pl);
        g_atlas->id = ++g_nextAtlasId;
        g_atlasFor = pl;
        g_atlasObjects = count;
        g_atlasGeneration = generation;
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
                       moving, a.buildMs) +
           fmt::format("; {} columns timed by the ghost run (to x {:.0f}, {:.0f} ms{}{})", a.timedColumns, a.timedTo,
                       a.ghostMs, a.ghostNote.empty() ? "" : ": ", a.ghostNote);
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
