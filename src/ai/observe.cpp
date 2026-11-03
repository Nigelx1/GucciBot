#include "ai/observe.hpp"

#include "absense/world/def.hpp"
#include "ai/sim.hpp"
#include "hacks/hitborders.hpp"
#include "tools/hitborder_geom.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

using namespace geode::prelude;
namespace hb = gucci::hitborder;

namespace gucci::ai::obs {

    // ------------------------------------------------------------------ tokens

    namespace {
        char const* const kTokenNames[kTokenCount] = {
            "empty", "yellow orb", "pink orb", "red orb", "blue orb", "green orb", "black orb", "green dash orb",
            "pink dash orb", "spider orb", "teleport orb", "yellow pad", "pink pad", "red pad", "blue pad",
            "spider pad", "gravity-down portal", "gravity-up portal", "gravity-flip portal", "cube portal",
            "ship portal", "ball portal", "ufo portal", "wave portal", "robot portal", "spider portal",
            "swing portal", "mini portal", "normal-size portal", "speed portal", "mirror portal", "dual portal",
        };

        char const* const kStateNames[kStateDim] = {
            "mode_cube", "mode_ship", "mode_ball", "mode_ufo", "mode_wave", "mode_robot", "mode_spider",
            "mode_swing", "gravity_flipped", "mini", "speed_0.5x", "speed_1x", "speed_2x", "speed_3x", "speed_4x",
            "yvel", "on_ground", "on_ceiling", "held_before", "log1p_ticks_since_press/5",
            "log1p_ticks_since_release/5", "floor_bound_blocks", "ceiling_bound_blocks", "60/tps", "is_dual",
            "p2_dy_blocks", "p2_mode/7", "p2_gravity_flipped", "p2_yvel", "p2_on_ground", "p2_mini", "zero",
            "orb_yellow", "orb_pink", "orb_red", "orb_blue", "orb_green", "orb_black", "orb_dash", "orb_spider",
        };
        char const* const kMetaNames[kMetaDim] = {"x", "y", "yvel", "mode", "gravity_flipped", "mini", "speed",
                                                  "dead"};

        // Speed portals: GameObjectType::Modifier with these ids.
        bool isSpeedPortal(int id) {
            return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
        }

        uint64_t fnv1a(std::string const& s) {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }

        std::string hex16(uint64_t h) {
            return fmt::format("{:016x}", h);
        }

        // Every choice that changes what a value means goes in here, so a
        // change to any of them changes the hash.
        std::string layoutText() {
            std::string s = "gbai-obs v3 2026-11-03 geo;";
            s += fmt::format("N={};state={};", kObsDim, kStateDim);
            for (auto const* n : kStateNames) s += std::string(n) + ",";
            s += fmt::format(";scan={}x4 cols from -1 block step 0.5, deadly-up,deadly-down,solid-up,solid-down, "
                             "1/8 block 0..{} none {} -> 12.5, hitborders upright player, gravity-relative;",
                             kColumns, kScanMax, kScanNone);
            s += fmt::format("tok={}x{} f16 dx f16 dy u8 type u8 0, dx[-1,20] |dy|<=10 by dx, spent skipped;", kTokens,
                             kTokenBytes);
            for (auto const* n : kTokenNames) s += std::string(n) + ",";
            s += ";meta=";
            for (auto const* n : kMetaNames) s += std::string(n) + ",";
            s += fmt::format(";geo={} at {}: rays from the center dx to first deadly then solid hitborder at heights "
                             "-2..+2 step 0.5 gravity-relative, floor_under, floor_end_or_start_dx, ceiling_over, "
                             "ceiling_end_or_start_dx; blocks 0..{} none {}; floor/ceiling within 6 blocks, chain "
                             "band 0.5 block;",
                             kGeoDim, kGeoOffset, kGeoReach, kGeoNone);
            return s;
        }
    }  // namespace

    char const* tokenName(int type) {
        return type >= 0 && type < kTokenCount ? kTokenNames[type] : "?";
    }

    uint8_t tokenOf(GameObject* o) {
        if (!o) return kNoToken;
        using T = GameObjectType;
        switch (o->m_objectType) {
        case T::YellowJumpRing: return kYellowOrb;
        case T::PinkJumpRing: return kPinkOrb;
        case T::RedJumpRing: return kRedOrb;
        case T::GravityRing: return kBlueOrb;
        case T::GreenRing: return kGreenOrb;
        case T::DropRing: return kBlackOrb;
        case T::DashRing: return kGreenDashOrb;
        case T::GravityDashRing: return kPinkDashOrb;
        case T::SpiderOrb: return kSpiderOrb;
        case T::TeleportOrb: return kTeleportOrb;
        case T::YellowJumpPad: return kYellowPad;
        case T::PinkJumpPad: return kPinkPad;
        case T::RedJumpPad: return kRedPad;
        case T::GravityPad: return kBluePad;
        case T::SpiderPad: return kSpiderPad;
        case T::NormalGravityPortal: return kGravityDownPortal;
        case T::InverseGravityPortal: return kGravityUpPortal;
        case T::GravityTogglePortal: return kGravityFlipPortal;
        case T::CubePortal: return kCubePortal;
        case T::ShipPortal: return kShipPortal;
        case T::BallPortal: return kBallPortal;
        case T::UfoPortal: return kUfoPortal;
        case T::WavePortal: return kWavePortal;
        case T::RobotPortal: return kRobotPortal;
        case T::SpiderPortal: return kSpiderPortal;
        case T::SwingPortal: return kSwingPortal;
        case T::MiniSizePortal: return kMiniPortal;
        case T::RegularSizePortal: return kNormalSizePortal;
        case T::InverseMirrorPortal:
        case T::NormalMirrorPortal: return kMirrorPortal;
        case T::DualPortal:
        case T::SoloPortal: return kDualPortal;
        case T::Modifier: return isSpeedPortal(o->m_objectID) ? kSpeedPortal : kNoToken;
        default: return kNoToken;
        }
    }

    std::string layoutHash() {
        static std::string const h = hex16(fnv1a(layoutText()));
        return h;
    }

    matjson::Value layoutJson() {
        auto out = matjson::Value::object();
        out["obs_dim"] = kObsDim;
        out["obs_layout_hash"] = layoutHash();
        auto state = matjson::Value::array();
        for (int i = 0; i < kStateDim; i++) state.push(kStateNames[i]);
        auto layout = matjson::Value::object();
        layout["state"] = state;
        layout["state_offset"] = 0;
        auto scan = matjson::Value::object();
        scan["offset"] = kStateDim;
        scan["columns"] = kColumns;
        scan["first_dx_blocks"] = -1.0;
        scan["column_blocks"] = 0.5;
        scan["per_column"] = std::vector<std::string>{"deadly_above", "deadly_below", "solid_above", "solid_below"};
        scan["unit"] = "1/8 block from the player's center (0-96); 255 = nothing within 12 blocks -> 12.5";
        layout["scan"] = scan;
        auto tok = matjson::Value::object();
        tok["offset"] = kStateDim + kScanDim;
        tok["slots"] = kTokens;
        tok["bytes"] = std::vector<std::string>{"f16 dx blocks", "f16 dy blocks (gravity-relative)", "u8 type",
                                                "u8 0"};
        tok["expanded"] = "dx, dy, one-hot(type, 32), index 0 unused; empty slot all zeros";
        auto types = matjson::Value::object();
        for (int i = 1; i < kTokenCount; i++) types[std::to_string(i)] = kTokenNames[i];
        tok["types"] = types;
        layout["tokens"] = tok;
        auto meta = matjson::Value::array();
        for (auto const* n : kMetaNames) meta.push(n);
        layout["meta"] = meta;
        auto geo = matjson::Value::object();
        geo["offset"] = kGeoOffset;
        geo["size"] = kGeoDim;
        geo["ray_heights_blocks"] = std::vector<double>{-2.0, -1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5, 2.0};
        geo["slots"] = std::vector<std::string>{"deadly_ray[9]", "solid_ray[9]", "floor_under", "floor_dx",
                                                "ceiling_over", "ceiling_dx"};
        geo["unit"] = fmt::format("blocks from the player's center, 0..{}; {} = none within reach", kGeoReach,
                                  kGeoNone);
        layout["geo"] = geo;
        out["obs_layout"] = layout;
        return out;
    }

    // ------------------------------------------------------------------ the observer

    namespace {
        constexpr float kBucket = 30.0f;
        constexpr float kMargin = 30.0f;        // a region reaches past its object's rect by the player's half box
        constexpr float kMoverReach = 900.0f;   // a mover is looked for this far from where the level put it
        constexpr float kReach = 360.0f;        // 12 blocks
        constexpr float kInf = std::numeric_limits<float>::infinity();
        constexpr float kRayReach = kGeoReach * kBlock;   // 20 blocks
        constexpr float kSurfReach = 180.0f;               // a floor/ceiling is looked for within 6 blocks
        constexpr float kSurfTouch = 2.0f;                 // a surface this far past the center still counts
        constexpr float kSurfBand = 15.0f;                 // a floor/ceiling continues within half a block

        // A convex polygon's x-range along the line y = yy (false: it misses it).
        bool lineRange(std::array<float, 2> const* v, int n, float dx, float dy, float yy, float& lo, float& hi) {
            lo = kInf;
            hi = -kInf;
            for (int i = 0; i < n; i++) {
                float const x0 = v[i][0] + dx, y0 = v[i][1] + dy;
                auto const& w = v[(i + 1) % n];
                float const x1 = w[0] + dx, y1 = w[1] + dy;
                if (y0 == yy) lo = std::min(lo, x0), hi = std::max(hi, x0);
                if ((y0 < yy && y1 > yy) || (y0 > yy && y1 < yy)) {
                    float const x = x0 + (x1 - x0) * (yy - y0) / (y1 - y0);
                    lo = std::min(lo, x), hi = std::max(hi, x);
                }
            }
            return lo <= hi;
        }

        struct Reg {
            float x0, x1, y0, y1;
            uint32_t v;   // first vertex (polygons)
            uint8_t nv;   // 0: the rect itself
            bool lethal;  // else solid
        };

        struct Config {
            int mode = 0;
            bool upsideDown = false;
            float hx = 0, hy = 0, ix = 0, iy = 0, size = 1;
            std::vector<uint32_t> first;  // per item: its first Reg, or kUnknown
            std::vector<uint16_t> count;
        };
        constexpr uint32_t kUnknown = std::numeric_limits<uint32_t>::max();

        int speedIndex(float s) {
            static float const kSpeeds[5] = {0.7f, 0.9f, 1.1f, 1.3f, 1.6f};
            int best = 1;
            float bd = 1e9f;
            for (int i = 0; i < 5; i++) {
                float const d = std::fabs(s - kSpeeds[i]);
                if (d < bd) bd = d, best = i;
            }
            return best;
        }

        int orbIndex(PlayerObject* p) {
            if (!p || !p->m_touchingRings || p->m_touchingRings->count() == 0) return -1;
            auto* o = static_cast<GameObject*>(p->m_touchingRings->objectAtIndex(0));
            if (!o) return -1;
            using T = GameObjectType;
            switch (o->m_objectType) {
            case T::YellowJumpRing: return 0;
            case T::PinkJumpRing: return 1;
            case T::RedJumpRing: return 2;
            case T::GravityRing: return 3;
            case T::GreenRing: return 4;
            case T::DropRing: return 5;
            case T::DashRing:
            case T::GravityDashRing: return 6;
            case T::SpiderOrb: return 7;
            default: return -1;
            }
        }

        uint8_t quant(float d) {
            if (!(d <= kReach)) return kScanNone;
            int const v = (int)std::floor(std::max(0.0f, d) / 3.75f);
            return (uint8_t)std::min(v, kScanMax);
        }

        // A convex polygon's y-range over the slab a <= x <= b (false: it misses it).
        bool slabRange(std::array<float, 2> const* v, int n, float dx, float dy, float a, float b, float& lo,
                       float& hi) {
            lo = kInf;
            hi = -kInf;
            for (int i = 0; i < n; i++) {
                float const x0 = v[i][0] + dx, y0 = v[i][1] + dy;
                auto const& w = v[(i + 1) % n];
                float const x1 = w[0] + dx, y1 = w[1] + dy;
                if (x0 >= a && x0 <= b) lo = std::min(lo, y0), hi = std::max(hi, y0);
                for (float const e : {a, b}) {
                    if ((x0 < e && x1 > e) || (x0 > e && x1 < e)) {
                        float const y = y0 + (y1 - y0) * (e - x0) / (x1 - x0);
                        lo = std::min(lo, y), hi = std::max(hi, y);
                    }
                }
            }
            return lo <= hi;
        }
    }  // namespace

    struct Observer::Impl {
        GJBaseGameLayer* pl = nullptr;
        LevelIndex index;
        std::vector<HitborderRead> reads;
        std::vector<std::vector<uint32_t>> buckets;       // static items by x (30 units)
        std::vector<std::vector<uint32_t>> moverBuckets;  // movers by where the level put them
        std::vector<uint32_t> seen;
        uint32_t stamp = 0;
        std::vector<Config> configs;
        std::vector<Reg> regs;
        std::vector<std::array<float, 2>> verts;
        std::vector<hb::Shape> shapes;  // scratch
        bool fixRadius = false;
        bool ready = false;
        Stats stats;
        // the button's history
        uint32_t tick = 0;
        bool prev = false;
        int64_t lastPress = -1;
        int64_t lastRelease = -1;
        // scratch for one tick
        float up[2][kColumns];
        float down[2][kColumns];
        struct Cand {
            float dx, dy;
            uint8_t type;
        };
        std::vector<Cand> cands;
        // geometry scratch (layout v3)
        float rayDx[2][kRays];
        float rayY[kRays];
        struct Sol {
            float x0, x1, lo, hi;  // x absolute; y relative to the player, gravity-relative (+ = up)
            float pLo, pHi;        // the y-range over x = px (pLo > pHi: it doesn't cover px)
            bool poly;
        };
        std::vector<Sol> sols;
        std::vector<std::pair<float, float>> chain;
        bool flipNow = false;

        int bucketOf(float x) const {
            return std::clamp((int)std::floor(x / kBucket), 0, (int)buckets.size() - 1);
        }

        void add(std::vector<std::vector<uint32_t>>& into, uint32_t idx, float x0, float x1) {
            int const b0 = bucketOf(x0), b1 = bucketOf(x1);
            for (int b = b0; b <= b1; b++) into[(size_t)b].push_back(idx);
        }

        bool build(GJBaseGameLayer* layer, std::string& why) {
            auto const t0 = std::chrono::steady_clock::now();
            pl = layer;
            if (!pl || !pl->m_objects) {
                why = "no level";
                return false;
            }
            auto def = world::WorldDef::get(pl);
            fixRadius = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
            index = LevelIndex{};
            index.levelLength = pl->m_levelLength;
            if (auto* lvl = static_cast<PlayLayer*>(pl)->m_level) index.levelId = lvl->m_levelID.value();
            reads.clear();
            float maxX = 0.0f;
            int const n = (int)pl->m_objects->count();
            for (int i = 0; i < n; i++) {
                auto* o = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
                if (!o) continue;
                int const slot = def ? def->slotOf(o) : -1;
                bool const mover = def && slot >= 0 && (def->flags[(size_t)slot] & world::kReferenced);
                // An object a toggle can turn on later is read as if on now
                // (its pose says whether it is on, tick by tick).
                bool const wasOff = o->m_isGroupDisabled;
                if (mover && wasOff) o->m_isGroupDisabled = false;
                HitborderRead r;
                bool const ok = hitborderReadObject(pl, o, r);
                if (mover && wasOff) o->m_isGroupDisabled = true;
                if (!ok) continue;
                Item it;
                it.object = o;
                it.slot = slot;
                it.mover = mover;
                it.bx = o->m_positionX;
                it.by = o->m_positionY;
                CCPoint const c = o->getRealPosition();
                it.cx = c.x;
                it.cy = c.y;
                hb::Rect rect;
                switch (r.type) {
                case HitborderRead::Type::Hazard: it.type = Item::Hazard; rect = r.collider.rect; break;
                case HitborderRead::Type::Special:
                    it.type = Item::Special;
                    rect = r.collider.rect;
                    it.token = tokenOf(o);
                    break;
                case HitborderRead::Type::Block: it.type = Item::Block; rect = r.block.rect; break;
                case HitborderRead::Type::Slope:
                    it.type = Item::Slope;
                    rect = r.slope.rect;
                    it.hazardSlope = r.slope.hazard;
                    break;
                }
                if (r.type == HitborderRead::Type::Hazard || r.type == HitborderRead::Type::Special) {
                    if (r.collider.radius > 0.0) {
                        double const rr = r.collider.radius;
                        rect.x0 = std::min(rect.x0, r.collider.center.x - rr);
                        rect.x1 = std::max(rect.x1, r.collider.center.x + rr);
                        rect.y0 = std::min(rect.y0, r.collider.center.y - rr);
                        rect.y1 = std::max(rect.y1, r.collider.center.y + rr);
                    }
                }
                // Specials that are no token (touch triggers, teleport
                // portals, D/H blocks) matter to neither the scan nor the tokens.
                if (it.type == Item::Special && it.token == kNoToken) continue;
                it.x0 = (float)rect.x0;
                it.y0 = (float)rect.y0;
                it.x1 = (float)rect.x1;
                it.y1 = (float)rect.y1;
                maxX = std::max(maxX, it.x1);
                index.items.push_back(it);
                reads.push_back(r);
                stats.items++;
                if (mover) stats.movers++;
                if (it.token) stats.tokens++;
            }
            size_t const nb = (size_t)std::max(1.0f, std::ceil((std::max(maxX, index.levelLength) + 2000.0f) / kBucket));
            buckets.assign(nb, {});
            moverBuckets.assign(nb, {});
            for (uint32_t i = 0; i < (uint32_t)index.items.size(); i++) {
                Item const& it = index.items[i];
                if (it.mover) add(moverBuckets, i, it.cx, it.cx);
                else add(buckets, i, it.x0 - kMargin, it.x1 + kMargin);
            }
            seen.assign(index.items.size(), 0);
            stamp = 0;
            configs.clear();
            regs.clear();
            verts.clear();
            stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ready = true;
            return true;
        }

        Config& configFor(hb::Player const& p) {
            int const mode = (int)p.mode;
            for (auto& c : configs) {
                if (c.mode == mode && c.upsideDown == p.upsideDown && c.hx == (float)p.hx && c.hy == (float)p.hy &&
                    c.ix == (float)p.ix && c.iy == (float)p.iy && c.size == (float)p.vehicleSize)
                    return c;
            }
            Config c;
            c.mode = mode;
            c.upsideDown = p.upsideDown;
            c.hx = (float)p.hx;
            c.hy = (float)p.hy;
            c.ix = (float)p.ix;
            c.iy = (float)p.iy;
            c.size = (float)p.vehicleSize;
            c.first.assign(index.items.size(), kUnknown);
            c.count.assign(index.items.size(), 0);
            configs.push_back(std::move(c));
            stats.configs = (int)configs.size();
            return configs.back();
        }

        // The canonical player a config's regions are made for: upright, at
        // rest, touching nothing special (the scan doesn't follow the icon's tilt).
        static hb::Player canonical(Config const& c) {
            hb::Player p;
            p.hx = c.hx;
            p.hy = c.hy;
            p.ix = c.ix;
            p.iy = c.iy;
            p.mode = (hb::Mode)c.mode;
            p.upsideDown = c.upsideDown;
            p.vehicleSize = c.size;
            return p;
        }

        void addShapes(std::vector<hb::Shape> const& from) {
            for (auto const& sh : from) {
                if (sh.kind == hb::Kind::Special || sh.poly.size() < 3) continue;
                Reg r{};
                r.lethal = sh.kind == hb::Kind::Lethal;
                double x0 = sh.poly[0].x, x1 = x0, y0 = sh.poly[0].y, y1 = y0;
                for (auto const& v : sh.poly) {
                    x0 = std::min(x0, v.x), x1 = std::max(x1, v.x);
                    y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                }
                r.x0 = (float)x0, r.x1 = (float)x1, r.y0 = (float)y0, r.y1 = (float)y1;
                bool rect = sh.poly.size() == 4;
                if (rect) {
                    for (auto const& v : sh.poly)
                        if ((v.x != x0 && v.x != x1) || (v.y != y0 && v.y != y1)) rect = false;
                }
                if (!rect) {
                    r.v = (uint32_t)verts.size();
                    r.nv = (uint8_t)std::min<size_t>(sh.poly.size(), 255);
                    for (size_t k = 0; k < r.nv; k++) verts.push_back({(float)sh.poly[k].x, (float)sh.poly[k].y});
                }
                regs.push_back(r);
            }
        }

        void regionsOf(Config& c, uint32_t idx) {
            if (c.first[idx] != kUnknown) return;
            shapes.clear();
            hb::Options opt;
            opt.fixRadius = fixRadius;
            opt.useTilt = false;
            opt.core = true;
            hitborderRegions(reads[idx], canonical(c), opt, shapes);
            c.first[idx] = (uint32_t)regs.size();
            addShapes(shapes);
            c.count[idx] = (uint16_t)std::min<size_t>(regs.size() - c.first[idx], 65535);
        }

        void apply(Reg const& r, float dx, float dy, float px, float py, float xa) {
            float const rx0 = r.x0 + dx, rx1 = r.x1 + dx;
            geometry(r, dx, dy, px, py, rx0, rx1);
            int c0 = (int)std::floor((rx0 - xa) / kColumnWidth);
            int c1 = (int)std::floor((rx1 - xa) / kColumnWidth);
            if (c1 < 0 || c0 >= kColumns) return;
            c0 = std::max(c0, 0);
            c1 = std::min(c1, kColumns - 1);
            int const k = r.lethal ? 0 : 1;
            for (int c = c0; c <= c1; c++) {
                float lo = r.y0 + dy, hi = r.y1 + dy;
                if (r.nv) {
                    float const a = std::max(xa + c * kColumnWidth, rx0);
                    float const b = std::min(xa + (c + 1) * kColumnWidth, rx1);
                    if (!slabRange(&verts[r.v], r.nv, dx, dy, a, b, lo, hi)) continue;
                }
                if (hi > py) up[k][c] = std::min(up[k][c], std::max(0.0f, lo - py));
                if (lo < py) down[k][c] = std::min(down[k][c], std::max(0.0f, py - hi));
            }
            (void)px;
        }

        // Layout v3: the rays and the floor/ceiling candidates from one region.
        void geometry(Reg const& r, float dx, float dy, float px, float py, float rx0, float rx1) {
            if (rx1 < px || rx0 > px + kRayReach) return;
            float const ry0 = r.y0 + dy, ry1 = r.y1 + dy;
            int const k = r.lethal ? 0 : 1;
            for (int h = 0; h < kRays; h++) {
                float const yy = rayY[h];
                if (yy < ry0 || yy > ry1) continue;
                float xl = rx0, xr = rx1;
                if (r.nv && !lineRange(&verts[r.v], r.nv, dx, dy, yy, xl, xr)) continue;
                if (xr < px) continue;
                rayDx[k][h] = std::min(rayDx[k][h], std::max(0.0f, xl - px));
            }
            if (r.lethal) return;
            float lo = ry0 - py, hi = ry1 - py;
            if (flipNow) lo = py - ry1, hi = py - ry0;
            if (hi < -kSurfReach || lo > kSurfReach) return;
            Sol so{rx0, rx1, lo, hi, 1.0f, 0.0f, r.nv != 0};
            if (rx0 <= px && rx1 >= px) {
                float a = ry0, b = ry1;
                if (!r.nv || slabRange(&verts[r.v], r.nv, dx, dy, px, px, a, b)) {
                    so.pLo = flipNow ? py - b : a - py;
                    so.pHi = flipNow ? py - a : b - py;
                }
            }
            sols.push_back(so);
        }

        // The floor (below) or the ceiling (!below) of the player: whether
        // there is one under/over it now (its surface within 6 blocks), and the
        // dx (blocks) to where it ends or, with none, to where the next one starts.
        void surface(bool below, float px, float& under, float& dxOut) {
            // Mirrored for the ceiling, so "below" is the only case.
            auto top = [&](Sol const& s) { return below ? s.hi : -s.lo; };
            auto bot = [&](Sol const& s) { return below ? s.lo : -s.hi; };
            float best = -kInf;
            for (auto const& s : sols) {
                if (s.pLo > s.pHi) continue;
                float const t = below ? s.pHi : -s.pLo;
                if (t <= kSurfTouch && t >= -kSurfReach) best = std::max(best, t);
            }
            float d = kInf;
            if (best > -kInf) {
                under = 1.0f;
                chain.clear();
                for (auto const& s : sols) {
                    if (s.x1 <= px) continue;
                    float const t = top(s), b = bot(s);
                    bool const inBand = s.poly ? (t >= best - kSurfBand && b <= best + kSurfBand)
                                             : (t >= best - kSurfBand && t <= best + kSurfBand);
                    if (inBand) chain.push_back({s.x0, s.x1});
                }
                std::sort(chain.begin(), chain.end());
                float cur = px;
                for (auto const& [a, b] : chain) {
                    if (a > cur + 0.5f) break;
                    cur = std::max(cur, b);
                }
                d = cur - px;
            } else {
                under = 0.0f;
                for (auto const& s : sols) {
                    float const t = top(s);
                    if (s.x0 > px && t <= kSurfTouch && t >= -kSurfReach) d = std::min(d, s.x0 - px);
                }
            }
            d /= kBlock;
            dxOut = d <= kGeoReach ? d : kGeoNone;
        }

        void visit(Config& c, uint32_t idx, float dx, float dy, float px, float py, float xa, float xb, bool flip) {
            Item const& it = index.items[idx];
            if (it.type == Item::Special) {
                float const tx = (it.cx + dx - px) / kBlock;
                float ty = (it.cy + dy - py) / kBlock;
                if (flip) ty = -ty;
                if (tx < -1.0f || tx > 20.0f || std::fabs(ty) > 10.0f) return;
                if (sim::copyActivated(it.object)) return;  // spent
                cands.push_back({tx, ty, it.token});
                return;
            }
            if (it.x1 + dx + kMargin < xa || it.x0 + dx - kMargin > xb) return;
            if (it.y1 + dy + kMargin < py - kReach || it.y0 + dy - kMargin > py + kReach) return;
            regionsOf(c, idx);
            uint32_t const f = c.first[idx], n = c.count[idx];
            for (uint32_t i = 0; i < n; i++) apply(regs[f + i], dx, dy, px, py, xa);
        }

        void observe(Record& out) {
            auto const t0 = std::chrono::steady_clock::now();
            out = Record{};
            sim::State st;
            sim::read(st);
            PlayerObject* copy = sim::copy(0);
            if (!copy || !ready) return;
            sim::Player const& p = st.p[0];
            bool const flip = p.upsideDown;
            float const px = p.x, py = p.y;
            hb::Player const hp = hitborderReadPlayer(copy);
            Config& cfg = configFor(hp);

            for (int k = 0; k < 2; k++)
                for (int c = 0; c < kColumns; c++) up[k][c] = down[k][c] = kInf;
            cands.clear();
            sols.clear();
            flipNow = flip;
            for (int h = 0; h < kRays; h++) {
                rayDx[0][h] = rayDx[1][h] = kInf;
                float const rel = (-2.0f + 0.5f * (float)h) * kBlock;
                rayY[h] = flip ? py - rel : py + rel;
            }
            float const xa = px - kBlock;                         // column 0's left edge
            // Column 39's right edge, or as far as the rays reach (layout v3).
            float const xb = std::max(xa + kColumns * kColumnWidth, px + kRayReach);
            if (++stamp == 0) {
                std::fill(seen.begin(), seen.end(), 0);
                stamp = 1;
            }
            // Static objects.
            for (int b = bucketOf(xa), e = bucketOf(xb); b <= e; b++) {
                for (uint32_t idx : buckets[(size_t)b]) {
                    if (seen[idx] == stamp) continue;
                    seen[idx] = stamp;
                    visit(cfg, idx, 0.0f, 0.0f, px, py, xa, xb, flip);
                }
            }
            // Objects the triggers move or toggle: where the session has them now.
            for (int b = bucketOf(xa - kMoverReach), e = bucketOf(xb + kMoverReach); b <= e; b++) {
                for (uint32_t idx : moverBuckets[(size_t)b]) {
                    Item const& it = index.items[idx];
                    double x = it.bx, y = it.by;
                    bool off = false;
                    if (sim::objectPose(it.slot, x, y, off)) {
                        stats.moverPoses++;
                        if (off) continue;
                    } else {
                        // No World objects in this session: the object's own fields.
                        if (it.object->m_isGroupDisabled) continue;
                        x = it.object->m_positionX;
                        y = it.object->m_positionY;
                    }
                    visit(cfg, idx, (float)(x - it.bx), (float)(y - it.by), px, py, xa, xb, flip);
                }
            }
            // The floor and, in a corridor, the ceiling.
            float floorY = 0.0f, ceilY = 0.0f;
            bool corridor = false;
            {
                corridor = !pl->m_gameState.m_isFreeMode &&
                           (copy->m_isShip || copy->m_isBird || copy->m_isDart || copy->m_isSwing || copy->m_isBall ||
                            copy->m_isSpider || pl->m_gameState.m_isDualMode);
                float const g = copy->m_unkAngle1 * copy->m_vehicleSize * 0.5f;
                floorY = corridor ? pl->getMinPortalY() + g : 90.0f + g;
                ceilY = corridor ? pl->getMaxPortalY() - g : 0.0f;
                hb::Ground ground;
                ground.corridor = corridor;
                ground.floorY = floorY;
                ground.ceilingY = ceilY;
                ground.left = xa - 60.0f;
                ground.right = xb + 60.0f;
                ground.reach = kReach + 120.0f;
                shapes.clear();
                hb::groundShapes(ground, canonical(cfg), shapes);
                size_t const r0 = regs.size(), v0 = verts.size();
                addShapes(shapes);
                for (size_t i = r0; i < regs.size(); i++) apply(regs[i], 0.0f, 0.0f, px, py, xa);
                regs.resize(r0);
                verts.resize(v0);
            }

            // The geometry block (layout v3).
            for (int h = 0; h < kRays; h++) {
                for (int k = 0; k < 2; k++) {
                    float const d = rayDx[k][h] / kBlock;
                    out.geo[k * kRays + h] = d <= kGeoReach ? d : kGeoNone;
                }
            }
            surface(true, px, out.geo[2 * kRays + 0], out.geo[2 * kRays + 1]);
            surface(false, px, out.geo[2 * kRays + 2], out.geo[2 * kRays + 3]);

            // The scan, gravity-relative.
            for (int c = 0; c < kColumns; c++) {
                float const dA = flip ? down[0][c] : up[0][c];
                float const dB = flip ? up[0][c] : down[0][c];
                float const sA = flip ? down[1][c] : up[1][c];
                float const sB = flip ? up[1][c] : down[1][c];
                out.scan[c * 4 + 0] = quant(dA);
                out.scan[c * 4 + 1] = quant(dB);
                out.scan[c * 4 + 2] = quant(sA);
                out.scan[c * 4 + 3] = quant(sB);
            }
            // Tokens: the 8 nearest by dx.
            std::stable_sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b) { return a.dx < b.dx; });
            for (int k = 0; k < kTokens && k < (int)cands.size(); k++) {
                uint8_t* b = out.tok + k * kTokenBytes;
                uint16_t const hx = toHalf(cands[(size_t)k].dx), hy = toHalf(cands[(size_t)k].dy);
                std::memcpy(b, &hx, 2);
                std::memcpy(b + 2, &hy, 2);
                b[4] = cands[(size_t)k].type;
                b[5] = 0;
            }

            // The state block.
            float* s = out.state;
            s[std::clamp(p.mode, 0, 7)] = 1.0f;
            s[8] = flip ? 1.0f : 0.0f;
            s[9] = p.mini ? 1.0f : 0.0f;
            int const sp = speedIndex(p.speed);
            s[10 + sp] = 1.0f;
            s[15] = flip ? -p.yVel : p.yVel;
            s[16] = p.onGround ? 1.0f : 0.0f;
            {
                // Against a surface over its head (gravity-relative): within a
                // unit of a solid region above, in the columns either side of it.
                float const above = std::min(flip ? down[1][1] : up[1][1], flip ? down[1][2] : up[1][2]);
                s[17] = above < 1.0f ? 1.0f : 0.0f;
            }
            s[18] = prev ? 1.0f : 0.0f;
            s[19] = std::log1p((float)((int64_t)tick - lastPress)) / 5.0f;
            s[20] = std::log1p((float)((int64_t)tick - lastRelease)) / 5.0f;
            {
                // The mode's floor and ceiling, gravity-relative, in blocks (15: none).
                float floorD = 15.0f, ceilD = 15.0f;
                float const toFloor = (py - floorY) / kBlock;
                float const toCeil = corridor ? (ceilY - py) / kBlock : 15.0f;
                if (!flip) {
                    floorD = toFloor;
                    ceilD = toCeil;
                } else {
                    floorD = corridor ? toCeil : 15.0f;
                    ceilD = toFloor;
                }
                s[21] = std::clamp(floorD, 0.0f, 15.0f);
                s[22] = std::clamp(ceilD, 0.0f, 15.0f);
            }
            s[23] = tpsRatio;
            if (st.count == 2) {
                sim::Player const& q = st.p[1];
                s[24] = 1.0f;
                float dy2 = (q.y - py) / kBlock;
                s[25] = flip ? -dy2 : dy2;
                s[26] = (float)std::clamp(q.mode, 0, 7) / 7.0f;
                s[27] = q.upsideDown ? 1.0f : 0.0f;
                s[28] = q.upsideDown ? -q.yVel : q.yVel;
                s[29] = q.onGround ? 1.0f : 0.0f;
                s[30] = q.mini ? 1.0f : 0.0f;
            }
            if (int const o = orbIndex(copy); o >= 0) s[32 + o] = 1.0f;

            float* m = out.meta;
            m[0] = px;
            m[1] = py;
            m[2] = p.yVel;
            m[3] = (float)std::clamp(p.mode, 0, 7);
            m[4] = flip ? 1.0f : 0.0f;
            m[5] = p.mini ? 1.0f : 0.0f;
            m[6] = (float)sp;
            m[7] = 0.0f;

            double const us =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            stats.observed++;
            stats.observeUs += us;
            stats.maxUs = std::max(stats.maxUs, us);
        }

        float tpsRatio = 0.25f;
    };

    Observer::Observer() : m(std::make_unique<Impl>()) {}
    Observer::~Observer() = default;

    bool Observer::begin(GJBaseGameLayer* pl, std::string& why) {
        *m = Impl{};
        return m->build(pl, why);
    }
    bool Observer::ready() const { return m->ready; }

    void Observer::setTps(double tps) { m->tpsRatio = (float)(60.0 / std::max(1.0, tps)); }

    void Observer::observe(Record& out) { m->observe(out); }

    void Observer::after(bool act) {
        if (act && !m->prev) m->lastPress = (int64_t)m->tick;
        if (!act && m->prev) m->lastRelease = (int64_t)m->tick;
        m->prev = act;
        m->tick++;
    }

    Observer::History Observer::history() const { return {m->tick, m->prev, m->lastPress, m->lastRelease}; }
    void Observer::setHistory(History const& h) {
        m->tick = h.tick;
        m->prev = h.prev;
        m->lastPress = h.lastPress;
        m->lastRelease = h.lastRelease;
    }

    Observer::Stats const& Observer::stats() const { return m->stats; }
    LevelIndex const* Observer::index() const { return m->ready ? &m->index : nullptr; }

} // namespace gucci::ai::obs
