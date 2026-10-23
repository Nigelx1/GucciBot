#include "absense/route/atlas.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>
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
    // Step 4 (docs/ROUTE_ENGINE_PLAN.md): a gap whose room for the centre
    // narrows to under this many halves - three player heights - gets a gate
    // where it is narrowest (a waist). ToE2's big ship dies in a slope
    // "diamond" whose waists are 79 units: over the tight rule's two halves,
    // so the route never named a height to go through them at.
    constexpr float kWaistHalves = 6.0f;
    // At least one gate this many columns apart (600 units, about two seconds
    // at normal speed), so no stretch of the route is left unplanned.
    constexpr int kGateSpacing = 20;
    // A piece of a gap thinner than this is no band to aim for (subtract
    // drops gaps that thin too).
    constexpr float kMinBand = 1.0f;

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
        int uid = 0;     // the object's m_uniqueID
        int index = -1;  // ... and where it is in the layer's object list
        float cx() const { return (o.minX + o.maxX) * 0.5f; }
        float cy() const { return (o.minY + o.maxY) * 0.5f; }
    };

    // A toggle orb (1594 and 3643, the game's custom rings: a press while
    // touching switches a group on or off, or spawns it) or a touch trigger
    // (1595: once the player is past it, a press or release switches its
    // group). Neither is something classify() reads.
    struct Toggle {
        int uid = 0;
        int index = -1;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;  // a touch trigger: its position, both corners
        int target = 0;    // the group it switches
        bool orb = false;  // a toggle orb, else a touch trigger
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
    // The objects the map's gates are at, by uid: where each is in the layer's
    // object list (objectOf), for the map in g_atlas.
    std::unordered_map<int, int> g_uidIndex;

    // The ghost run (3b), out a slice a frame (tickGhost): what it has found
    // so far, for the level and the read of it it is out in. Kept apart from
    // the map, which is built again with it once the ghost is back.
    struct Ghost {
        GJBaseGameLayer* pl = nullptr;
        unsigned generation = ~0u;
        std::vector<std::pair<float, float>> speeds;  // the map's speed portals
        std::vector<uint8_t> timed;
        std::vector<std::vector<std::pair<float, float>>> boxes;   // (bottom, top)
        std::vector<std::vector<std::pair<float, float>>> rounds;  // (centre y, radius)
        bool running = false;  // out now (Trajectory::stepGhost)
        bool retry = false;    // could not go out (the simulation was busy, say): tried again each second
        bool fresh = false;    // back, and not in the map yet
        float reachedX = 0.0f;
        double ms = 0.0;
        std::string note;
        std::chrono::steady_clock::time_point lastTry;
    };
    Ghost g_ghost;
    // The ghost's share of a frame, and what it may take in all: LIMBO's
    // first 45% took 8.7 s in one go, which froze the game for all of it.
    constexpr double kGhostShare = 0.3;
    constexpr double kGhostSliceMinMs = 1.0;
    constexpr double kGhostSliceMaxMs = 6.0;
    constexpr double kGhostTotalMs = 120000.0;

    // What the object is, in the words a player uses (classify's power tells
    // the orbs and pads of one kind apart: 1.0 yellow, 0.72 pink, 1.38 red
    // jump orbs; 0.8 blue, 1.0 green gravity orbs; 1.0 yellow, 0.65 pink, 1.25
    // red pads).
    const char* objectWhat(gpu::Object const& o) {
        switch (o.kind) {
            case kJumpOrb: return o.power < 0.86f ? "pink orb" : (o.power < 1.19f ? "yellow orb" : "red orb");
            case kGravityOrb: return o.power < 0.9f ? "blue orb" : "green orb";
            case kDropOrb: return "black orb";
            case kSpiderOrb: return "spider orb";
            case kDashOrb: return "green dash orb";
            case kGravityDashOrb: return "pink dash orb";
            case kJumpPad: return o.power < 0.82f ? "pink pad" : (o.power < 1.12f ? "yellow pad" : "red pad");
            case kGravityPad: return "blue pad";
            case kSpiderPad: return "spider pad";
            case kGravityPortalDown: return "normal gravity portal";
            case kGravityPortalUp: return "upside-down gravity portal";
            case kGravityToggle: return "gravity toggle portal";
            case kSpeedPortal: return "speed portal";
            case kModePortal: {
                static const char* const kModes[] = {"cube portal",  "ship portal",  "ball portal",   "UFO portal",
                                                     "wave portal",  "robot portal", "spider portal", "swing portal"};
                return kModes[std::clamp((int)std::lround(o.power), 0, 7)];
            }
            case kSizePortal: return o.power < 0.8f ? "mini portal" : "normal size portal";
            default: return "";
        }
    }

    // Step 4: the choices at an object a route goes through or around. In:
    // the column's openings inside the object's contact region [lo, hi] (its
    // box grown by the player's half), each piece one `through` option that
    // must fire it. Out: the openings minus that region, each piece one
    // `around` option that must not; where nothing is left outside and
    // `aroundInside` is set (an orb, which only a press fires), the whole
    // openings, passed without a press. Nothing is offered when the region
    // misses every opening (the object is out of reach), or for a pad or
    // portal with no way around it (TOUCH alone: every way through touches it).
    void contactChoices(Gate& g, float lo, float hi, OptionKind through, OptionKind around, bool aroundInside, int uid,
                        int8_t gravity, bool flips) {
        std::vector<Option> in;
        std::vector<Option> out;
        for (size_t k = 0; k < g.options.size(); k++) {
            auto const& o = g.options[k];
            Option piece;
            piece.objectUid = uid;
            piece.opening = (int)k;
            const float a = std::max(o.lo, lo);
            const float b = std::min(o.hi, hi);
            if (b - a > kMinBand) {
                piece.kind = through;
                piece.band = {a, b};
                piece.mustFire = true;
                piece.gravityAfter = gravity;
                piece.flipsGravity = flips;
                in.push_back(piece);
            }
            piece.kind = around;
            piece.mustFire = false;
            piece.mustNotFire = true;
            piece.gravityAfter = -1;
            piece.flipsGravity = false;
            if (lo - o.lo > kMinBand) {
                piece.band = {o.lo, std::min(o.hi, lo)};
                out.push_back(piece);
            }
            if (o.hi - hi > kMinBand) {
                piece.band = {std::max(o.lo, hi), o.hi};
                out.push_back(piece);
            }
        }
        if (in.empty()) return;
        if (out.empty() && aroundInside) {
            for (size_t k = 0; k < g.options.size(); k++) {
                Option piece;
                piece.kind = around;
                piece.band = g.options[k];
                piece.objectUid = uid;
                piece.mustNotFire = true;
                piece.opening = (int)k;
                out.push_back(piece);
            }
        }
        g.choices.insert(g.choices.end(), in.begin(), in.end());
        g.choices.insert(g.choices.end(), out.begin(), out.end());
    }

    // Every gate's last options: anywhere in one of its gaps, nothing asked.
    void freeChoices(Gate& g) {
        for (size_t k = 0; k < g.options.size(); k++) {
            Option o;
            o.band = g.options[k];
            o.opening = (int)k;
            g.choices.push_back(o);
        }
    }

    // A gate at column c, with that column's gaps as its options.
    Gate makeGate(Atlas const& a, GateKind kind, int c, float x, float y, int uid, const char* what) {
        Gate g;
        g.kind = kind;
        g.column = c;
        g.x = x;
        g.y = y;
        g.options = a.columns[(size_t)c].openings;
        g.key = GateKey{c, kind, uid};
        g.what = what;
        return g;
    }

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

    std::unique_ptr<Atlas> build(GJBaseGameLayer* pl, unsigned generation) {
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
        std::vector<Toggle> toggles;
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
            auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(o);
            if (!e || e->m_targetGroupID <= 0) continue;
            dynamicGroups.insert(e->m_targetGroupID);
            // Step 4: a touch trigger the player crosses (one another trigger
            // spawns has no place in the level, and one that is off now is
            // left to the ghost) is a FIRE / SKIP gate where it stands, if
            // its group holds something the player can touch (checked below).
            if (o->m_objectID == 1595 && !e->m_isSpawnTriggered && !o->m_isGroupDisabled && !o->m_isDisabled) {
                Toggle t;
                t.uid = o->m_uniqueID;
                t.index = i;
                t.minX = t.maxX = o->getPositionX();
                t.minY = t.maxY = o->getPositionY();
                t.target = e->m_targetGroupID;
                toggles.push_back(t);
            }
        }
        // The groups that hold something the player can touch (a block, a
        // hazard, an orb, a pad or a portal, on now or switched on later):
        // switching one of those can change a route, switching scenery cannot.
        // Only gathered when there is a touch trigger to ask about (so far the
        // list holds nothing else).
        std::unordered_set<int> playerGroups;
        const bool wantGroups = !toggles.empty();
        const auto noteGroups = [&](GameObject* o) {
            if (!wantGroups || o->m_groupCount <= 0 || !o->m_groups) return;
            for (int g = 0; g < o->m_groupCount && g < 10; g++) playerGroups.insert((int)(*o->m_groups)[(size_t)g]);
        };
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
                // Step 4: a toggle orb is still an orb to press or not. What
                // it switches stays the map's blind spot (its column is
                // unknown), but the choice is named.
                if (obj->m_objectType == GameObjectType::CustomRing) {
                    const cocos2d::CCRect r = obj->getObjectRect();
                    Toggle t;
                    t.uid = obj->m_uniqueID;
                    t.index = i;
                    t.minX = r.getMinX();
                    t.minY = r.getMinY();
                    t.maxX = r.getMaxX();
                    t.maxY = r.getMaxY();
                    t.orb = true;
                    toggles.push_back(t);
                }
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
                noteGroups(obj);
                continue;
            }
            Box b;
            if (!gpu::classifyObject(obj, b.o)) continue;
            noteGroups(obj);
            b.uid = obj->m_uniqueID;
            b.index = i;
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
        // with the level's own triggers (Trajectory::beginGhost), and each
        // column keeps what the triggers had in it while the ghost went
        // through - the level as the player meets it, moving blocks and blocks
        // that appear included. LIMBO is built out of those; left out, its map
        // was "any height" from one end to the other. The ghost runs a slice a
        // frame (tickGhost) and the map is built again once it is back; until
        // then a column it has not timed is what the objects alone say. Raw
        // extents here, the player's half added per column below.
        for (auto const& b : boxes)
            if (b.o.kind == kSpeedPortal) a->speedPortals.push_back({b.cx(), b.o.power});
        std::sort(a->speedPortals.begin(), a->speedPortals.end());
        const bool ghostHere =
            g_ghost.pl == pl && g_ghost.generation == generation && g_ghost.timed.size() == (size_t)columns;
        std::vector<uint8_t> noGhost;
        if (!ghostHere) noGhost.assign((size_t)columns, 0);
        const std::vector<uint8_t>& timed = ghostHere ? g_ghost.timed : noGhost;
        // (Only read where `timed` is set.)
        auto const& timedBoxes = g_ghost.boxes;
        auto const& timedRounds = g_ghost.rounds;
        for (uint8_t t : timed) a->timedColumns += t;
        if (ghostHere) {
            a->timedTo = g_ghost.reachedX;
            a->ghostMs = g_ghost.ms;
            a->ghostNote = g_ghost.note;
            a->ghostOut = g_ghost.running;
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

        // 4. Gates: orbs, pads and portals; toggle orbs and touch triggers;
        // forks where a gap splits; tight gaps and waists; and one at least
        // every kGateSpacing columns. Each with what a route can do there
        // (step 4): the object's own options, then FREE.
        std::vector<Gate> gates;
        g_uidIndex.clear();
        for (auto const& b : boxes) {
            GateKind kind;
            if (isOrb(b.o.kind)) kind = GateKind::Orb;
            else if (isPad(b.o.kind)) kind = GateKind::Pad;
            else if (isPortal(b.o.kind)) kind = GateKind::Portal;
            else continue;
            const int c = std::clamp((int)std::floor(b.cx() / kColumnWidth), 0, columns - 1);
            Gate g = makeGate(*a, kind, c, b.cx(), b.cy(), b.uid, objectWhat(b.o));
            const float half = kPlayerHalf * (a->columns[(size_t)c].mini ? 0.6f : 1.0f);
            const float r = b.o.radius;
            const float lo = r > 0.0f ? b.cy() - r - half : b.o.minY - half;
            const float hi = r > 0.0f ? b.cy() + r + half : b.o.maxY + half;
            const bool facingDown = (b.o.flags & gpu::kFlagFacingDown) != 0;
            // The gravity it leaves: gravity pads and portals set it, spider
            // orbs and pads put the player on the far side the way they face
            // (phys::bumpPlayer, ringJump: turned the way it faces, then the
            // spider's own jump flips it), blue, green and pink dash orbs and
            // the toggle portal flip it.
            int8_t gravity = -1;
            bool flips = false;
            switch (b.o.kind) {
                case kGravityOrb: case kGravityDashOrb: case kGravityToggle: flips = true; break;
                case kGravityPad: case kSpiderPad: case kSpiderOrb: gravity = facingDown ? 0 : 1; break;
                case kGravityPortalDown: gravity = 0; break;
                case kGravityPortalUp: gravity = 1; break;
                default: break;
            }
            if (kind == GateKind::Orb) {
                contactChoices(g, lo, hi, OptionKind::Use, OptionKind::Skip, true, b.uid, gravity, flips);
            } else if (b.o.kind != kSpeedPortal) {
                // A speed portal stays a gate with nothing to choose: the
                // ghost's timing of everything after it assumes it is taken.
                contactChoices(g, lo, hi, OptionKind::Touch, OptionKind::Avoid, false, b.uid, gravity, flips);
            }
            freeChoices(g);
            g_uidIndex[b.uid] = b.index;
            gates.push_back(std::move(g));
        }
        for (auto const& t : toggles) {
            if (!t.orb && playerGroups.count(t.target) == 0) continue;
            const float cx = (t.minX + t.maxX) * 0.5f;
            const float cy = (t.minY + t.maxY) * 0.5f;
            const int c = std::clamp((int)std::floor(cx / kColumnWidth), 0, columns - 1);
            Gate g = makeGate(*a, GateKind::Toggle, c, cx, cy, t.uid, t.orb ? "toggle orb" : "touch trigger");
            if (t.orb) {
                const float half = kPlayerHalf * (a->columns[(size_t)c].mini ? 0.6f : 1.0f);
                contactChoices(g, t.minY - half, t.maxY + half, OptionKind::Fire, OptionKind::Skip, true, t.uid, -1,
                               false);
            } else {
                // A touch trigger fires on the input, not on contact: from
                // its x on, a press (or release) switches its group. So no
                // band of its own: FIRE is a press after it while it holds,
                // SKIP none.
                for (size_t k = 0; k < g.options.size(); k++) {
                    Option fire;
                    fire.kind = OptionKind::Fire;
                    fire.band = g.options[k];
                    fire.objectUid = t.uid;
                    fire.mustFire = true;
                    fire.opening = (int)k;
                    g.choices.push_back(fire);
                }
                for (size_t k = 0; k < g.options.size(); k++) {
                    Option skip;
                    skip.kind = OptionKind::Skip;
                    skip.band = g.options[k];
                    skip.objectUid = t.uid;
                    skip.mustNotFire = true;
                    skip.opening = (int)k;
                    g.choices.push_back(skip);
                }
            }
            freeChoices(g);
            g_uidIndex[t.uid] = t.index;
            gates.push_back(std::move(g));
        }
        // The smallest height of the gaps in column k that overlap `o` (as
        // good as unlimited when none does).
        const auto heightNear = [&](int k, Opening const& o) {
            float best = std::numeric_limits<float>::max();
            if (k < 0 || k >= columns) return best;
            for (auto const& p : a->columns[(size_t)k].openings)
                if (p.lo < o.hi && p.hi > o.lo) best = std::min(best, p.hi - p.lo);
            return best;
        };
        int lastGate = -1000;
        for (int c = 1; c < columns; c++) {
            auto const& col = a->columns[(size_t)c];
            auto const& prev = a->columns[(size_t)c - 1];
            if (col.openings.empty()) continue;
            const float half = kPlayerHalf * (col.mini ? 0.6f : 1.0f);
            bool tight = false;
            bool waist = false;
            for (auto const& o : col.openings) {
                const float h = o.hi - o.lo;
                if (h < kTightHalves * half) tight = true;
                // A waist: the gap gets narrower here than in the column
                // before and no narrower in the next, so the gate sits at the
                // narrowest point of the squeeze, not on every step into it.
                if (h < kWaistHalves * half && h < heightNear(c - 1, o) - 1.0f && h <= heightNear(c + 1, o) + 0.5f)
                    waist = true;
            }
            if (!prev.openings.empty() && col.openings.size() > prev.openings.size()) {
                Gate g = makeGate(*a, GateKind::Fork, c, col.x0, 0.0f, 0, "");
                freeChoices(g);
                gates.push_back(std::move(g));
                lastGate = c;
            } else if (tight && c - lastGate > 3) {
                Gate g = makeGate(*a, GateKind::Tight, c, col.x0, 0.0f, 0, "");
                freeChoices(g);
                gates.push_back(std::move(g));
                lastGate = c;
            } else if (waist && c - lastGate > 3) {
                Gate g = makeGate(*a, GateKind::Waist, c, col.x0, 0.0f, 0, "");
                freeChoices(g);
                gates.push_back(std::move(g));
                lastGate = c;
            }
        }
        // Column, then kind, then where, then the object: the same order on
        // every build, whatever order the level lists its objects in.
        std::sort(gates.begin(), gates.end(), [](Gate const& l, Gate const& r) {
            if (l.column != r.column) return l.column < r.column;
            if (l.kind != r.kind) return (int)l.kind < (int)r.kind;
            if (l.x != r.x) return l.x < r.x;
            if (l.y != r.y) return l.y < r.y;
            return l.key.objectUid < r.key.objectUid;
        });
        lastGate = 0;
        std::vector<Gate> spaced;
        size_t gi = 0;
        for (int c = 0; c < columns; c++) {
            bool here = false;
            while (gi < gates.size() && gates[gi].column == c) {
                spaced.push_back(std::move(gates[gi++]));
                here = true;
            }
            if (here) {
                lastGate = c;
            } else if (c - lastGate >= kGateSpacing && !a->columns[(size_t)c].openings.empty()) {
                Gate g = makeGate(*a, GateKind::Spacing, c, a->columns[(size_t)c].x0, 0.0f, 0, "");
                freeChoices(g);
                spaced.push_back(std::move(g));
                lastGate = c;
            }
        }
        a->gates = std::move(spaced);

        a->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        log::info("[AbsPF] level map: {}", summary(*a));
        return a;
    }

    // Sends the ghost out from where the player is, or notes why it cannot go
    // yet (tickGhost tries again each second).
    void sendGhost(GJBaseGameLayer* pl) {
        g_ghost.lastTry = std::chrono::steady_clock::now();
        g_ghost.retry = false;
        auto* traj = Bot::get()->trajectory().unsafeInner();
        if (!traj) {
            g_ghost.note = "no simulation yet";
            g_ghost.retry = true;
            return;
        }
        // A column is read while the ghost is within a few units of its
        // middle, not the whole time it is in it: a block that moves swept a
        // tall stretch of the column over those 30 units (half a second at
        // the slowest speed), and the map called all of it blocked - ground
        // Nigel's LIMBO macro stands on at x 1036.
        constexpr float kMiddle = 6.0f;
        const int columns = (int)g_ghost.timed.size();
        Trajectory::GhostReport refused;
        const bool out = traj->beginGhost(
            pl, g_ghost.speeds, 75.0f, 400000, kGhostTotalMs,
            [](float x) { return std::fabs(x - (std::floor(x / kColumnWidth) + 0.5f) * kColumnWidth) <= kMiddle; },
            [columns](float x, std::span<GameObject* const> nearby) {
                const int gc = std::clamp((int)std::floor(x / kColumnWidth), 0, columns - 1);
                g_ghost.timed[(size_t)gc] = 1;
                // What can touch a player whose centre is in the ghost's column.
                const float x0 = (float)gc * kColumnWidth - kPlayerHalf;
                const float x1 = (float)(gc + 1) * kColumnWidth + kPlayerHalf;
                for (GameObject* o : nearby) {
                    gpu::Object ob;
                    if (!gpu::classifyObject(o, ob) || !blocks(ob.kind)) continue;  // off just now, or nothing that stops a player
                    if (ob.maxX < x0 || ob.minX > x1) continue;
                    if (ob.radius > 0.0f) g_ghost.rounds[(size_t)gc].push_back({(ob.minY + ob.maxY) * 0.5f, ob.radius});
                    else g_ghost.boxes[(size_t)gc].push_back({ob.minY, ob.maxY});
                }
            },
            refused);
        if (out) {
            g_ghost.running = true;
            g_ghost.note.clear();
            return;
        }
        g_ghost.note = refused.why ? refused.why : "it could not start";
        // A level without the World stays without it; anything else passes.
        g_ghost.retry = g_ghost.note != "the World is off for this level";
    }

    ccColor4F gateColor(GateKind k) {
        switch (k) {
            case GateKind::Fork: return ccc4f(1.0f, 1.0f, 1.0f, 0.85f);
            case GateKind::Orb: return ccc4f(0.3f, 0.85f, 1.0f, 0.9f);
            case GateKind::Pad: return ccc4f(1.0f, 0.4f, 0.9f, 0.9f);
            case GateKind::Portal: return ccc4f(0.7f, 0.5f, 1.0f, 0.9f);
            case GateKind::Tight: return ccc4f(1.0f, 0.6f, 0.1f, 0.9f);
            case GateKind::Waist: return ccc4f(1.0f, 0.75f, 0.35f, 0.8f);
            case GateKind::Toggle: return ccc4f(1.0f, 0.95f, 0.3f, 0.9f);
            default: return ccc4f(0.6f, 0.6f, 0.6f, 0.35f);
        }
    }

    // An option's own colour: what it asks for in greens and yellow, what it
    // keeps away from in reds.
    ccColor4F markColor(OptionKind k, float alpha) {
        switch (k) {
            case OptionKind::Use: return ccc4f(0.35f, 1.0f, 0.45f, alpha);
            case OptionKind::Touch: return ccc4f(0.4f, 1.0f, 0.85f, alpha);
            case OptionKind::Fire: return ccc4f(1.0f, 0.95f, 0.3f, alpha);
            case OptionKind::Skip: return ccc4f(1.0f, 0.4f, 0.4f, alpha);
            case OptionKind::Avoid: return ccc4f(1.0f, 0.6f, 0.3f, alpha);
            default: return ccc4f(1.0f, 1.0f, 1.0f, alpha);
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
        // Gates: a line across the bands a route can be in; orbs, pads,
        // portals and toggles get a dot where they are.
        for (auto const& g : a.gates) {
            if (g.options.empty()) continue;
            const float lo = g.options.front().lo;
            const float hi = g.options.back().hi;
            const auto colour = gateColor(g.kind);
            node->drawSegment(ccp(g.x, lo), ccp(g.x, hi), g.kind == GateKind::Spacing ? 0.6f : 1.0f, colour);
            if (g.kind == GateKind::Orb || g.kind == GateKind::Pad || g.kind == GateKind::Portal ||
                g.kind == GateKind::Toggle)
                node->drawDot(ccp(g.x, g.y), 4.0f, colour);
            // Step 4: the gate's own options, each a thin bar over its band
            // just right of the line and its letter (FREE, everywhere, has
            // neither).
            for (auto const& o : g.choices) {
                if (o.kind == OptionKind::Free) continue;
                const float top = std::min(o.band.hi, o.band.lo + 600.0f);  // open sky: not up to the level's top
                node->drawSegment(ccp(g.x + 3.0f, o.band.lo), ccp(g.x + 3.0f, top), 0.8f, markColor(o.kind, 0.7f));
                drawMark(node, o.kind, g.x + 6.0f, markY(o.band), 9.0f, 0.85f);
            }
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
        case GateKind::Waist: return "waist";
        case GateKind::Toggle: return "toggle";
        default: return "spacing";
    }
}

const char* optionKindName(OptionKind k) {
    switch (k) {
        case OptionKind::Use: return "use";
        case OptionKind::Skip: return "skip";
        case OptionKind::Touch: return "touch";
        case OptionKind::Avoid: return "avoid";
        case OptionKind::Fire: return "fire";
        default: return "free";
    }
}

int gravityAfter(Option const& o, int before) {
    if (o.gravityAfter >= 0) return o.gravityAfter;
    return o.flipsGravity ? 1 - before : before;
}

std::string describe(Gate const& g, Option const& o) {
    std::string out = fmt::format("{} {:.0f}-{:.0f} at the {}, x {:.0f}", optionKindName(o.kind), o.band.lo, o.band.hi,
                                  *g.what ? g.what : gateKindName(g.kind), g.x);
    if (o.flipsGravity) out += ", flips gravity";
    else if (o.gravityAfter == 0) out += ", gravity normal after";
    else if (o.gravityAfter == 1) out += ", gravity upside down after";
    return out;
}

GameObject* objectOf(GJBaseGameLayer* pl, int uid) {
    if (!pl || !pl->m_objects || uid == 0) return nullptr;
    const int n = (int)pl->m_objects->count();
    if (pl == g_atlasFor) {
        auto const it = g_uidIndex.find(uid);
        if (it != g_uidIndex.end() && it->second >= 0 && it->second < n) {
            auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex((unsigned)it->second));
            if (o && o->m_uniqueID == uid) return o;
        }
    }
    for (int i = 0; i < n; i++) {
        auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex((unsigned)i));
        if (o && o->m_uniqueID == uid) return o;
    }
    return nullptr;
}

float markY(Opening const& band) {
    // The middle of the band, but never more than 45 above its bottom: an
    // open-sky gap reaches the top of the level.
    return band.lo + std::min((band.hi - band.lo) * 0.5f, 45.0f);
}

void drawMark(CCDrawNode* node, OptionKind kind, float x, float y, float size, float alpha, bool taken) {
    // Letters on a 4 x 6 grid, bottom-left at (0, 0).
    struct Stroke {
        float x0, y0, x1, y1;
    };
    static const Stroke kU[] = {{0, 6, 0, 0}, {0, 0, 4, 0}, {4, 0, 4, 6}};
    static const Stroke kS[] = {{4, 6, 0, 6}, {0, 6, 0, 3}, {0, 3, 4, 3}, {4, 3, 4, 0}, {4, 0, 0, 0}};
    static const Stroke kT[] = {{0, 6, 4, 6}, {2, 6, 2, 0}};
    static const Stroke kA[] = {{0, 0, 2, 6}, {2, 6, 4, 0}, {1, 3, 3, 3}};
    static const Stroke kF[] = {{0, 0, 0, 6}, {0, 6, 4, 6}, {0, 3, 3, 3}};
    const Stroke* strokes = nullptr;
    size_t count = 0;
    switch (kind) {
        case OptionKind::Use: strokes = kU; count = sizeof(kU) / sizeof(kU[0]); break;
        case OptionKind::Skip: strokes = kS; count = sizeof(kS) / sizeof(kS[0]); break;
        case OptionKind::Touch: strokes = kT; count = sizeof(kT) / sizeof(kT[0]); break;
        case OptionKind::Avoid: strokes = kA; count = sizeof(kA) / sizeof(kA[0]); break;
        case OptionKind::Fire: strokes = kF; count = sizeof(kF) / sizeof(kF[0]); break;
        default: return;
    }
    if (!node || size <= 0.0f) return;
    const float s = size / 6.0f;
    const float x0 = x;
    const float y0 = y - size * 0.5f;
    const ccColor4F colour = taken ? ccc4f(1.0f, 0.8f, 0.2f, alpha) : markColor(kind, alpha);
    const float width = std::max(0.45f, size * (taken ? 0.08f : 0.06f));
    for (size_t i = 0; i < count; i++) {
        auto const& st = strokes[i];
        node->drawSegment(ccp(x0 + st.x0 * s, y0 + st.y0 * s), ccp(x0 + st.x1 * s, y0 + st.y1 * s), width, colour);
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
    if (!g_atlas || g_atlasFor != pl || g_atlasObjects != count || g_atlasGeneration != generation || g_ghost.fresh) {
        g_ghost.fresh = false;
        g_atlas = build(pl, generation);
        g_atlas->id = ++g_nextAtlasId;
        g_atlasFor = pl;
        g_atlasObjects = count;
        g_atlasGeneration = generation;
        // A level (or a read of it) the ghost has not been out in: it goes
        // now, from where the player is.
        if (g_ghost.pl != pl || g_ghost.generation != generation || g_ghost.timed.size() != g_atlas->columns.size()) {
            if (auto* traj = Bot::get()->trajectory().unsafeInner()) traj->dropGhost();
            const size_t columns = g_atlas->columns.size();
            g_ghost = Ghost{};
            g_ghost.pl = pl;
            g_ghost.generation = generation;
            g_ghost.speeds = g_atlas->speedPortals;
            g_ghost.timed.assign(columns, 0);
            g_ghost.boxes.assign(columns, {});
            g_ghost.rounds.assign(columns, {});
            sendGhost(pl);
        }
    }
    return g_atlas.get();
}

void tickGhost(PlayLayer* pl) {
    if (!pl || g_ghost.pl != pl) return;
    auto* traj = Bot::get()->trajectory().unsafeInner();
    if (!traj) return;
    if (g_ghost.generation != moverCacheGeneration()) {
        // The level was read again: what the ghost holds is of the old read.
        // atlasFor sends a new one when the map is next asked for.
        if (g_ghost.running) traj->dropGhost();
        g_ghost.running = false;
        g_ghost.retry = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_ghost.running) {
        if (g_ghost.retry && now - g_ghost.lastTry > std::chrono::seconds(1)) sendGhost(pl);
        return;
    }
    if (!traj->ghosting()) {
        // Dropped from under it (the simulation was made again): out again
        // from where the player is, with what it found so far kept.
        g_ghost.running = false;
        g_ghost.retry = true;
        return;
    }
    // One slice a frame drawn: Calculate, the pathfinder and its judge step
    // the game many times inside one frame, each with its own postUpdate.
    static unsigned lastFrame = ~0u;
    const unsigned frame = CCDirector::sharedDirector()->getTotalFrames();
    if (frame == lastFrame) return;
    lastFrame = frame;
    const double frameMs =
        std::clamp((double)CCDirector::sharedDirector()->getAnimationInterval(), 1.0 / 1000.0, 1.0 / 30.0) * 1000.0;
    const double sliceMs = std::clamp(frameMs * kGhostShare, kGhostSliceMinMs, kGhostSliceMaxMs);
    if (!traj->stepGhost(now + std::chrono::microseconds((int64_t)(sliceMs * 1000.0)))) return;
    const auto report = traj->finishGhost();
    g_ghost.running = false;
    g_ghost.reachedX = report.reachedX;
    g_ghost.ms = report.ms;
    g_ghost.note = report.why ? report.why : "";
    g_ghost.fresh = true;  // the map takes it in when it is next asked for
    log::info("[AbsPF] ghost run back: to x {:.0f}, {} ticks in {} slices, {:.0f} ms ({:.0f} of it picking up){}{}",
              report.reachedX, report.ticks, report.slices, report.ms, report.resumeMs, report.why ? ": " : "",
              report.why ? report.why : "");
}

std::string summary(Atlas const& a) {
    int counts[8] = {};
    int choosing = 0;  // gates with options of their own (use/skip, touch/avoid, fire/skip)
    for (auto const& g : a.gates) {
        counts[std::clamp((int)g.kind, 0, 7)]++;
        for (auto const& o : g.choices)
            if (o.kind != OptionKind::Free) {
                choosing++;
                break;
            }
    }
    int moving = 0;
    for (auto const& c : a.columns) moving += c.dynamic ? 1 : 0;
    const std::string objects = fmt::format(
        "{}: {} columns, {} objects ({} moving, {} it can't follow), {} gates (forks {}, orbs {}, pads {}, portals {}, "
        "toggles {}, tight {}, waists {}, spacing {}; {} with options of their own), {} columns with moving parts, "
        "built in {:.0f} ms",
        a.level.empty() ? std::string("level") : a.level, a.columns.size(), a.objects, a.movingObjects,
        a.unknownObjects, a.gates.size(), counts[(int)GateKind::Fork], counts[(int)GateKind::Orb],
        counts[(int)GateKind::Pad], counts[(int)GateKind::Portal], counts[(int)GateKind::Toggle],
        counts[(int)GateKind::Tight], counts[(int)GateKind::Waist], counts[(int)GateKind::Spacing], choosing, moving,
        a.buildMs);
    // The map being shown, with the ghost still out for it: how far it is.
    const bool current = g_atlas && a.id == g_atlas->id && g_ghost.pl == g_atlasFor;
    if (current && g_ghost.running) {
        auto* traj = Bot::get()->trajectory().unsafeInner();
        const auto so = traj ? traj->ghostSoFar() : Trajectory::GhostReport{};
        return objects + fmt::format("; the ghost run is out: at x {:.0f} of {:.0f}, {:.0f} ms so far", so.reachedX,
                                     a.length, so.ms);
    }
    if (current && g_ghost.retry) return objects + fmt::format("; the ghost run is waiting: {}", g_ghost.note);
    return objects + fmt::format("; {} columns timed by the ghost run (to x {:.0f}, {:.0f} ms{}{})", a.timedColumns,
                                 a.timedTo, a.ghostMs, a.ghostNote.empty() ? "" : ": ", a.ghostNote);
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
    // Drawn again when the map is built again (the ghost run back, say).
    auto const* a = atlasFor(pl);
    if (existing && a && existing->getTag() == (int)a->id) return;
    if (existing) existing->removeFromParent();
    if (!a) return;
    auto* node = draw(*a);
    node->setTag((int)a->id);
    pl->m_objectLayer->addChild(node, 1500);
}

}  // namespace absense::route
