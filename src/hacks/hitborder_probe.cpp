// gucci_hitborder_probe: see hitborder_probe.hpp.
//
// What GD's pass does to the copy is seen at the functions GD itself calls,
// each only for the copy and only while the probe runs:
//   - PlayLayer::destroyPlayer: killed, and by what (first in line, before
//     anything that would swallow the death). A block's inner box, a slope's
//     leg and a slope's line kill with no object (destroyPlayer(player,
//     nullptr), 0x3937f2 and 0x39021e), so a death without one is put down
//     to the object whose collision call is running;
//   - PlayerObject::collidedWithObject / collidedWithObjectInternal: a
//     block, or a slope's 1-unit leg, passed GD's touch test (the outer box);
//   - PlayerObject::collidedWithSlopeInternal: a slope's line snapped the
//     copy (moved it, or put it on the slope);
//   - Absense's collisionCheckObjects (phys::g_touchListener): an orb, pad,
//     portal or trigger passed the overlap test. The copy's pass can't run
//     GD's own collisionCheckObjects without firing real objects, so this is
//     Absense's port of it; the overlap test in it is GD's (CCRect,
//     playerCircleCollision, OBB2D).
// Each sign is kept only if the copy was still where it was put, as it was
// put, when it came.

#include "hacks/hitborder_probe.hpp"

#include "absense/compat/bot.hpp"
#include "absense/judge.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "hacks/hitborders.hpp"
#include "tools/hitborder_geom.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace gucci {
    namespace {

        namespace hb = hitborder;

        // ---------------------------------------------------------------- what the pass did

        enum class Ev : uint8_t { Dead, Touch, Special };

        struct Event {
            GameObject* object = nullptr;
            Ev ev = Ev::Dead;
            bool atStart = false;  // the copy was still where it was put, as it was put
        };

        // Closer than this, the copy hasn't moved (a snap moves it by more).
        constexpr float kStill = 1e-4f;

        // While the probe runs: the copy, where the pass under way put it, as
        // what, and what GD's pass did to it.
        struct Observer {
            bool on = false;
            PlayerObject* copy = nullptr;
            CCPoint at;
            int mode = 0;
            bool upsideDown = false;
            float size = 1.0f;
            std::vector<Event> events;
            // The object whose collision call (collidedWithObject(Internal),
            // collidedWithSlopeInternal) is running for the copy: a death
            // with no object in it is that object's.
            GameObject* current = nullptr;
            // Killed, with no object and outside any object's call: the
            // ground, the ceiling, the edge of the level.
            bool diedOfNothing = false;

            bool atStart(PlayerObject* p) const {
                CCPoint const pos = p->getPosition();
                return std::abs(pos.x - at.x) <= kStill && std::abs(pos.y - at.y) <= kStill &&
                       Trajectory::modeOf(p) == mode && p->m_isUpsideDown == upsideDown && p->m_vehicleSize == size;
            }

            void note(PlayerObject* p, GameObject* object, Ev ev) {
                if (on && p == copy && object)
                    events.push_back({object, ev, atStart(p)});
            }

            void noteDeath(PlayerObject* p, GameObject* object) {
                if (!on || p != copy)
                    return;
                if (GameObject* by = object ? object : current)
                    events.push_back({by, Ev::Dead, atStart(p)});
                else if (atStart(p))
                    diedOfNothing = true;
            }

            void clear() {
                events.clear();
                current = nullptr;
                diedOfNothing = false;
            }
        };

        Observer& observer() {
            static Observer o;
            return o;
        }

        // The object a collision call is about, for the copy, while it runs.
        struct InCall {
            Observer& o;
            GameObject* was = nullptr;
            bool mine = false;
            InCall(PlayerObject* p, GameObject* object) : o(observer()) {
                mine = o.on && p == o.copy;
                if (mine) {
                    was = o.current;
                    o.current = object;
                }
            }
            ~InCall() {
                if (mine)
                    o.current = was;
            }
            InCall(InCall const&) = delete;
            InCall& operator=(InCall const&) = delete;
        };

        // ---------------------------------------------------------------- kinds

        constexpr std::array<const char*, kHitborderProbeKinds> kKindNames = {
            "spike", "saw", "block", "slope", "hazard slope", "orb", "pad", "portal", "other"};

        // The speed portals: modifiers, always drawn (as Show Hitboxes draws them).
        constexpr std::array<int, 5> kSpeedPortals = {200, 201, 202, 203, 1334};

        HitborderProbeKind kindOf(HitborderRead const& read, GameObject* o) {
            switch (read.type) {
            case HitborderRead::Type::Hazard:
                return read.collider.radius > 0 ? HitborderProbeKind::Saw : HitborderProbeKind::Spike;
            case HitborderRead::Type::Block: return HitborderProbeKind::Block;
            case HitborderRead::Type::Slope:
                return read.slope.hazard ? HitborderProbeKind::HazardSlope : HitborderProbeKind::Slope;
            case HitborderRead::Type::Special: break;
            }
            switch (o->m_objectType) {
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::CustomRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::TeleportOrb: return HitborderProbeKind::Orb;
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::SpiderPad: return HitborderProbeKind::Pad;
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::GravityTogglePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::TeleportPortal:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal: return HitborderProbeKind::Portal;
            case GameObjectType::Modifier:
                if (std::find(kSpeedPortals.begin(), kSpeedPortals.end(), o->m_objectID) != kSpeedPortals.end())
                    return HitborderProbeKind::Portal;
                return HitborderProbeKind::Other;
            default: return HitborderProbeKind::Other;
            }
        }

        uint32_t bitOf(HitborderProbeKind k) {
            return 1u << static_cast<int>(k);
        }

        // ---------------------------------------------------------------- words

        const char* regionWord(hb::Region r) {
            switch (r) {
            case hb::Region::None: return "nothing";
            case hb::Region::Special: return "special";
            case hb::Region::Touch: return "touch";
            case hb::Region::Lethal: return "deadly";
            }
            return "?";
        }

        const char* modeWord(int mode) {
            switch (mode) {
            case Trajectory::kModeCube: return "cube";
            case Trajectory::kModeShip: return "ship";
            case Trajectory::kModeBall: return "ball";
            case Trajectory::kModeBird: return "ufo";
            case Trajectory::kModeDart: return "wave";
            case Trajectory::kModeRobot: return "robot";
            case Trajectory::kModeSpider: return "spider";
            case Trajectory::kModeSwing: return "swing";
            }
            return "?";
        }

        // Which way an edge faces: top, bottom, left, right, a slant, or a
        // circle's arc.
        std::string direction(hb::EdgeSample const& s) {
            if (s.arc)
                return "arc";
            if (std::abs(s.normal.y) >= 0.9999)
                return s.normal.y > 0 ? "top" : "bottom";
            if (std::abs(s.normal.x) >= 0.9999)
                return s.normal.x > 0 ? "right" : "left";
            return std::string("slant ") + (s.normal.y > 0 ? "up" : "down") + "-" + (s.normal.x > 0 ? "right" : "left");
        }

        // An edge as the report names it: what is drawn inside / outside it,
        // and which way it faces ("deadly/touch top").
        std::string edgeName(hb::EdgeSample const& s) {
            return std::string(regionWord(s.inside)) + "/" + regionWord(s.outside) + " " + direction(s);
        }

        double round3(double v) {
            return std::round(v * 1000.0) / 1000.0;
        }

        matjson::Value point(double x, double y) {
            matjson::Value v = std::vector<double>{round3(x), round3(y)};
            return v;
        }

        // ---------------------------------------------------------------- the level

        // Every object in the sections from x0 to x1, and two more on each
        // side (an object is filed under its position, and its rect can reach
        // further). addToSection files m_positionX * m_sectionXFactor (0.01).
        template <class Fn>
        void forEachObjectIn(GJBaseGameLayer* pl, double x0, double x1, Fn&& fn) {
            auto& sections = pl->m_sections;
            auto& sizes = pl->m_sectionSizes;
            int const columns = static_cast<int>(sections.size());
            if (columns <= 0)
                return;
            double const f = pl->m_sectionXFactor > 0.f ? pl->m_sectionXFactor : 0.01;
            double const lo = std::clamp(std::floor(x0 * f) - 2.0, 0.0, static_cast<double>(columns - 1));
            double const hi = std::clamp(std::floor(x1 * f) + 2.0, 0.0, static_cast<double>(columns - 1));
            for (int i = static_cast<int>(lo); i <= static_cast<int>(hi); ++i) {
                auto* column = sections[i];
                auto* columnSizes = static_cast<size_t>(i) < sizes.size() ? sizes[i] : nullptr;
                if (!column || !columnSizes)
                    continue;
                for (size_t j = 0; j < column->size() && j < columnSizes->size(); ++j) {
                    auto* section = column->at(j);
                    if (!section)
                        continue;
                    int const count = columnSizes->at(j);
                    for (int k = 0; k < count && static_cast<size_t>(k) < section->size(); ++k) {
                        if (auto* object = section->at(k))
                            fn(object);
                    }
                }
            }
        }

        // An object's reach: its rect, and its circle.
        hb::Rect reachOf(HitborderRead const& r) {
            switch (r.type) {
            case HitborderRead::Type::Block: return r.block.rect;
            case HitborderRead::Type::Slope: return r.slope.rect;
            default: break;
            }
            hb::Rect out = r.collider.rect;
            if (r.collider.radius > 0) {
                out.x0 = std::min(out.x0, r.collider.center.x - r.collider.radius);
                out.y0 = std::min(out.y0, r.collider.center.y - r.collider.radius);
                out.x1 = std::max(out.x1, r.collider.center.x + r.collider.radius);
                out.y1 = std::max(out.y1, r.collider.center.y + r.collider.radius);
            }
            return out;
        }

        // Kinds Absense's collisionCheckObjects passes over before its overlap
        // test, where the probe listens: a copy never touches them.
        bool untestable(GameObject* o) {
            switch (o->m_objectType) {
            case GameObjectType::EnterEffectObject:
            case GameObjectType::SecretCoin:
            case GameObjectType::UserCoin:
            case GameObjectType::Collectible: return true;
            default: return false;
            }
        }

        // ... and the ones the real player 1 has used, which it passes over
        // too (only the enhanced classes keep the flags it reads).
        bool spentForPlayer1(PlayerObject* copy, GameObject* o) {
            GameObjectClassType const cls = o->m_classType;
            if (cls != GameObjectClassType::Effect && cls != GameObjectClassType::Animated &&
                cls != GameObjectClassType::Enhanced)
                return false;
            return ::Bot::get()->trajectory().realPlayerHasActivated(copy, static_cast<EnhancedGameObject*>(o));
        }

        // ---------------------------------------------------------------- the count

        struct Tally {
            int64_t agree = 0;
            int64_t mismatch = 0;
            int64_t inconclusive = 0;
            int64_t quiet = 0;  // agreements in a quiet region: nothing killed, and GD shows no touch there

            matjson::Value json() const {
                auto v = matjson::Value::object();
                v["agree"] = agree;
                v["mismatch"] = mismatch;
                v["inconclusive"] = inconclusive;
                if (quiet > 0)
                    v["quiet"] = quiet;
                return v;
            }
        };

        struct KindTally {
            int64_t objects = 0;
            int64_t points = 0;
            int64_t internal = 0;  // edge points inside the object's own regions, left out
            Tally all;
            std::map<std::string, Tally> edges;
        };

        struct ObjectTally {
            HitborderProbeKind kind = HitborderProbeKind::Other;
            int id = 0;
            int uid = 0;
            double x = 0.0;
            double y = 0.0;
            Tally all;
        };

        enum class Verdict : uint8_t { Agree, Quiet, Mismatch, Inconclusive };

        void count(Tally& t, Verdict v) {
            switch (v) {
            case Verdict::Agree: ++t.agree; break;
            case Verdict::Quiet:
                ++t.agree;
                ++t.quiet;
                break;
            case Verdict::Mismatch: ++t.mismatch; break;
            case Verdict::Inconclusive: ++t.inconclusive; break;
            }
        }

    } // namespace

    const char* hitborderProbeKindName(HitborderProbeKind kind) {
        int const i = static_cast<int>(kind);
        return i >= 0 && i < kHitborderProbeKinds ? kKindNames[static_cast<size_t>(i)] : "?";
    }

    bool hitborderProbeKinds(std::string const& text, uint32_t& bits, std::string& bad) {
        bits = 0;
        std::string word;
        auto take = [&]() {
            if (word.empty())
                return true;
            using K = HitborderProbeKind;
            if (word == "all")
                bits = ~0u;
            else if (word == "spike" || word == "spikes")
                bits |= bitOf(K::Spike);
            else if (word == "saw" || word == "saws")
                bits |= bitOf(K::Saw);
            else if (word == "block" || word == "blocks")
                bits |= bitOf(K::Block);
            else if (word == "slope" || word == "slopes")
                bits |= bitOf(K::Slope) | bitOf(K::HazardSlope);
            else if (word == "hazard_slope" || word == "hazard_slopes")
                bits |= bitOf(K::HazardSlope);
            else if (word == "orb" || word == "orbs" || word == "ring" || word == "rings")
                bits |= bitOf(K::Orb);
            else if (word == "pad" || word == "pads")
                bits |= bitOf(K::Pad);
            else if (word == "portal" || word == "portals")
                bits |= bitOf(K::Portal);
            else if (word == "other" || word == "triggers")
                bits |= bitOf(K::Other);
            else {
                bad = word;
                return false;
            }
            word.clear();
            return true;
        };
        for (char c : text) {
            if (c == ',' || c == ' ' || c == ';') {
                if (!take())
                    return false;
            } else {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (!take())
            return false;
        if (bits == 0)
            bits = ~0u;  // nothing named: every kind
        return true;
    }

    matjson::Value hitborderProbe(HitborderProbeArgs const& args) {
        auto out = matjson::Value::object();
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 || !pl->m_objectLayer) {
            out["error"] = "not in a level";
            return out;
        }
        if (pl->m_loadingProgress < 1.0f) {
            out["error"] = "the level is still loading";
            return out;
        }
        if (absense::judge::active()) {
            out["error"] = "the judge is playing a script for real (gucci_sim_vs_real); try again when it is done";
            return out;
        }
        PlayerObject* real = pl->m_player1;
        auto& traj = ::Bot::get()->trajectory();
        // Absense's copies are made the first time something simulates in a
        // level (absense/compat/bot.cpp), as its diagnostics make them.
        if (!traj.exists())
            traj.init();

        Trajectory::ProbeSetup setup;
        setup.mode = args.mode >= 0 ? args.mode : Trajectory::modeOf(real);
        setup.mini = args.mini >= 0 ? args.mini != 0 : real->m_vehicleSize != 1.0f;
        setup.upsideDown = args.upsideDown >= 0 ? args.upsideDown != 0 : real->m_isUpsideDown;
        setup.rotation = args.rotation;
        setup.dt = args.dt;
        // An orb, pad, portal or trigger the copy touches is heard, and kept
        // from acting: whether it was touched is all the probe asks. A slope
        // acts: its own test (collidedWithSlopeInternal) decides.
        setup.touched = [](PlayerObject* copy, GameObject* object) {
            Observer& o = observer();
            if (!o.on || copy != o.copy || !object || object->m_objectType == GameObjectType::Slope)
                return false;
            o.events.push_back({object, Ev::Special, o.atStart(copy)});
            return true;
        };

        // Which objects.
        bool const range = args.xFrom < args.xTo;
        bool const yRange = range && args.yFrom < args.yTo;
        CCPoint const around = real->getPosition();
        double scanFrom = 0.0;
        double scanTo = 0.0;
        if (range) {
            scanFrom = args.xFrom;
            scanTo = args.xTo;
        } else if (args.uid != 0) {
            scanFrom = -1e7;
            scanTo = 1e7;
        } else {
            scanFrom = around.x - args.radius;
            scanTo = around.x + args.radius;
        }

        std::array<KindTally, kHitborderProbeKinds> kinds{};
        Tally totals;
        int64_t internalTotal = 0;
        int64_t objects = 0;
        int64_t passes = 0;
        int64_t untestableCount = 0;
        int64_t spentCount = 0;
        bool truncated = false;
        auto mismatches = matjson::Value::array();
        auto inconclusive = matjson::Value::array();
        auto skippedExamples = matjson::Value::array();
        std::vector<ObjectTally> worst;
        matjson::Value player = matjson::Value::object();
        float usedDt = 0.0f;
        auto const started = std::chrono::steady_clock::now();
        auto elapsedMs = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };

        const char* why = nullptr;
        bool const ran = traj.probeCollisions(
            pl, setup,
            [&](PlayerObject* copy, float dt, Trajectory::ProbePass const& passAt) {
                usedDt = dt;
                Observer& o = observer();
                o.copy = copy;
                o.mode = Trajectory::modeOf(copy);
                o.upsideDown = copy->m_isUpsideDown;
                o.size = copy->m_vehicleSize;
                o.clear();
                o.on = true;
                struct Off {
                    ~Off() {
                        Observer& ob = observer();
                        ob.on = false;
                        ob.copy = nullptr;
                        ob.clear();
                    }
                } off;

                // The copy, as the overlay would read it for its regions.
                hb::Player const hp = hitborderReadPlayer(copy);
                hb::Options opt;
                opt.fixRadius = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
                opt.useTilt = true;
                opt.core = true;
                player["mode"] = std::string(modeWord(o.mode));
                player["mini"] = o.size != 1.0f;
                player["upside_down"] = o.upsideDown;
                player["rotation"] = static_cast<double>(copy->getRotation());
                player["outer_half"] = std::vector<double>{round3(hp.hx), round3(hp.hy)};
                player["inner_half"] = std::vector<double>{round3(hp.ix), round3(hp.iy)};
                player["platformer"] = hp.platformer;
                player["going_left"] = hp.goingLeft;
                player["sideways"] = hp.sideways;

                std::unordered_set<GameObject*> seen;
                forEachObjectIn(pl, scanFrom, scanTo, [&](GameObject* object) {
                    if (truncated || !seen.insert(object).second)
                        return;
                    if (args.uid != 0 && object->m_uniqueID != args.uid)
                        return;
                    HitborderRead read;
                    if (!hitborderReadObject(pl, object, read))
                        return;
                    HitborderProbeKind const kind = kindOf(read, object);
                    if ((args.kinds & bitOf(kind)) == 0)
                        return;
                    hb::Rect const reach = reachOf(read);
                    if (range) {
                        if (reach.x1 < args.xFrom || reach.x0 > args.xTo)
                            return;
                        if (yRange && (reach.y1 < args.yFrom || reach.y0 > args.yTo))
                            return;
                    } else if (args.uid == 0) {
                        double const r = args.radius;
                        if (around.x < reach.x0 - r || around.x > reach.x1 + r || around.y < reach.y0 - r ||
                            around.y > reach.y1 + r)
                            return;
                    }
                    bool const noTest = untestable(object);
                    bool const spent = !noTest && spentForPlayer1(copy, object);
                    if (noTest || spent) {
                        ++(noTest ? untestableCount : spentCount);
                        if (skippedExamples.size() < 10) {
                            auto e = matjson::Value::object();
                            e["kind"] = std::string(hitborderProbeKindName(kind));
                            e["object_id"] = object->m_objectID;
                            e["uid"] = object->m_uniqueID;
                            e["why"] = std::string(noTest ? "a copy's pass never touches this kind of object"
                                                          : "player 1 has used it, so a copy of player 1 passes it by");
                            skippedExamples.push(e);
                        }
                        return;
                    }

                    std::vector<hb::Shape> shapes;
                    hitborderRegions(read, hp, opt, shapes);
                    if (shapes.empty())
                        return;
                    std::vector<hb::Circle> circles;
                    if (read.type == HitborderRead::Type::Hazard || read.type == HitborderRead::Type::Special)
                        circles = hb::colliderCircles(read.collider, hp, opt);
                    size_t internal = 0;
                    auto const samples = hb::edgeSamples(shapes, circles, args.eps, args.perEdge, &internal);
                    auto& kt = kinds[static_cast<size_t>(kind)];
                    ++kt.objects;
                    ++objects;
                    kt.internal += static_cast<int64_t>(internal);
                    internalTotal += static_cast<int64_t>(internal);
                    ObjectTally ot;
                    ot.kind = kind;
                    ot.id = object->m_objectID;
                    ot.uid = object->m_uniqueID;
                    ot.x = object->m_positionX;
                    ot.y = object->m_positionY;

                    for (auto const& s : samples) {
                        std::string const edge = edgeName(s);
                        for (int side = 0; side < 2; ++side) {
                            if (passes >= args.maxPoints || ((passes & 31) == 0 && elapsedMs() > args.maxMs)) {
                                truncated = true;
                                break;
                            }
                            hb::Vec2 const p = side == 0 ? s.in : s.out;
                            hb::Region const want = side == 0 ? s.inside : s.outside;
                            bool quiet = s.quietIn;
                            if (side == 1)
                                hb::regionAt(shapes, p, &quiet);

                            // One pass, the copy standing at p.
                            o.clear();
                            o.at = CCPoint(static_cast<float>(p.x), static_cast<float>(p.y));
                            passAt(o.at);
                            ++passes;
                            hb::Region got = hb::Region::None;
                            bool otherHazard = false;
                            for (auto const& e : o.events) {
                                if (!e.atStart)
                                    continue;
                                if (e.object == object) {
                                    hb::Region r = hb::Region::None;
                                    if (e.ev == Ev::Dead)
                                        r = hb::Region::Lethal;
                                    else if (e.ev == Ev::Touch && (read.type == HitborderRead::Type::Block ||
                                                                   read.type == HitborderRead::Type::Slope))
                                        r = hb::Region::Touch;
                                    else if (e.ev == Ev::Special && read.type == HitborderRead::Type::Special)
                                        r = hb::Region::Special;
                                    if (static_cast<int>(r) > static_cast<int>(got))
                                        got = r;
                                } else if (e.ev == Ev::Dead && (e.object->m_objectType == GameObjectType::Hazard ||
                                                                e.object->m_objectType == GameObjectType::AnimatedHazard)) {
                                    otherHazard = true;
                                }
                            }
                            CCPoint const end = copy->getPosition();
                            bool const displaced = !o.atStart(copy);

                            // A sign from the object, with the copy where it
                            // was put, is an answer; no sign is one only when
                            // nothing moved the copy first, the pass went on
                            // to the objects at all (a death with no object
                            // outside an object's call, the edge of the level,
                            // ends it before them: 0x214189), and, for a
                            // hazard, no other hazard killed it first (the
                            // first hazard to hit ends the pass, 0x214950;
                            // a block's or a slope's death doesn't).
                            Verdict v = Verdict::Agree;
                            const char* whyNot = nullptr;
                            if (got != hb::Region::None) {
                                v = got == want ? Verdict::Agree : Verdict::Mismatch;
                            } else if (displaced) {
                                v = Verdict::Inconclusive;
                                whyNot = "the pass moved the copy, or changed it, before this object gave any sign";
                            } else if (o.diedOfNothing) {
                                v = Verdict::Inconclusive;
                                whyNot = "the copy died of no object (the edge of the level) before any object was tested";
                            } else if (read.type == HitborderRead::Type::Hazard && otherHazard) {
                                v = Verdict::Inconclusive;
                                whyNot = "another hazard killed the copy first";
                            } else if (want == hb::Region::None) {
                                v = Verdict::Agree;
                            } else if (want == hb::Region::Touch && quiet) {
                                v = Verdict::Quiet;
                            } else {
                                v = Verdict::Mismatch;
                            }

                            count(totals, v);
                            count(kt.all, v);
                            count(kt.edges[edge], v);
                            count(ot.all, v);
                            ++kt.points;

                            if (v == Verdict::Mismatch && static_cast<int>(mismatches.size()) < args.maxList) {
                                auto m = matjson::Value::object();
                                m["kind"] = std::string(hitborderProbeKindName(kind));
                                m["object_id"] = object->m_objectID;
                                m["uid"] = object->m_uniqueID;
                                m["edge"] = edge;
                                m["side"] = std::string(side == 0 ? "inside" : "outside");
                                m["at"] = point(o.at.x, o.at.y);
                                m["on_edge"] = point(s.on.x, s.on.y);
                                m["drawn"] = std::string(regionWord(want));
                                m["gd"] = std::string(regionWord(got));
                                if (displaced)
                                    m["moved_to"] = point(end.x, end.y);
                                mismatches.push(m);
                            } else if (v == Verdict::Inconclusive && inconclusive.size() < 10) {
                                auto m = matjson::Value::object();
                                m["kind"] = std::string(hitborderProbeKindName(kind));
                                m["uid"] = object->m_uniqueID;
                                m["edge"] = edge;
                                m["side"] = std::string(side == 0 ? "inside" : "outside");
                                m["at"] = point(o.at.x, o.at.y);
                                m["moved_to"] = point(end.x, end.y);
                                m["why"] = std::string(whyNot ? whyNot : "");
                                inconclusive.push(m);
                            }
                        }
                        if (truncated)
                            break;
                    }
                    if (ot.all.mismatch > 0)
                        worst.push_back(ot);
                });
            },
            why);
        if (!ran) {
            out["error"] = std::string(why ? why : "the probe could not run");
            return out;
        }

        player["dt"] = static_cast<double>(usedDt);
        out["player"] = player;
        auto scope = matjson::Value::object();
        if (range) {
            scope["x_from"] = static_cast<double>(args.xFrom);
            scope["x_to"] = static_cast<double>(args.xTo);
            if (yRange) {
                scope["y_from"] = static_cast<double>(args.yFrom);
                scope["y_to"] = static_cast<double>(args.yTo);
            }
        } else if (args.uid == 0) {
            scope["around"] = point(around.x, around.y);
            scope["radius"] = static_cast<double>(args.radius);
        }
        if (args.uid != 0)
            scope["uid"] = args.uid;
        out["scope"] = scope;
        out["fix_radius_collision"] = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
        out["frame"] = static_cast<int64_t>(GucciEngine::get()->updater.getFrame());
        out["eps"] = args.eps;
        out["per_edge"] = args.perEdge;
        out["objects"] = objects;
        out["passes"] = passes;

        auto totalsJson = totals.json();
        totalsJson["internal_points"] = internalTotal;
        out["totals"] = totalsJson;

        auto byKind = matjson::Value::object();
        for (int k = 0; k < kHitborderProbeKinds; ++k) {
            auto const& kt = kinds[static_cast<size_t>(k)];
            if (kt.objects == 0)
                continue;
            auto v = kt.all.json();
            v["objects"] = kt.objects;
            v["points"] = kt.points;
            v["internal_points"] = kt.internal;
            auto edges = matjson::Value::object();
            for (auto const& [name, t] : kt.edges)
                edges[name] = t.json();
            v["edges"] = edges;
            byKind[kKindNames[static_cast<size_t>(k)]] = v;
        }
        out["by_kind"] = byKind;
        out["mismatches"] = mismatches;
        if (totals.mismatch > static_cast<int64_t>(mismatches.size()))
            out["mismatches_not_listed"] = totals.mismatch - static_cast<int64_t>(mismatches.size());

        std::sort(worst.begin(), worst.end(),
                  [](ObjectTally const& a, ObjectTally const& b) { return a.all.mismatch > b.all.mismatch; });
        auto worstJson = matjson::Value::array();
        for (size_t i = 0; i < worst.size() && i < 20; ++i) {
            auto v = worst[i].all.json();
            v["kind"] = std::string(hitborderProbeKindName(worst[i].kind));
            v["object_id"] = worst[i].id;
            v["uid"] = worst[i].uid;
            v["at"] = point(worst[i].x, worst[i].y);
            worstJson.push(v);
        }
        out["objects_with_mismatches"] = worstJson;
        out["inconclusive_examples"] = inconclusive;
        auto skipped = matjson::Value::object();
        skipped["untestable"] = untestableCount;
        skipped["used_by_player_1"] = spentCount;
        skipped["examples"] = skippedExamples;
        out["skipped"] = skipped;
        out["truncated"] = truncated;
        out["ms"] = std::round(elapsedMs());

        auto notes = matjson::Value::array();
        notes.push(std::string(
            "Every point is one GD collision pass of a hidden copy of the player, standing still there, from the same "
            "state each time (at rest, no grace timer, nothing held); the real game is put back afterwards."));
        notes.push(std::string(
            "drawn is what Hitborders shows at the point (deadly, touch, special or nothing); gd is what GD's pass did "
            "because of that object: killed the copy, touched it (a block's or a slope leg's touch test, a slope's "
            "line snapping it), or found it touching (orbs, pads, portals, triggers). GD names no object when a "
            "block, a slope's leg or a slope's line kills; that death is put down to the object whose collision call "
            "it came from."));
        notes.push(std::string(
            "Orbs, pads, portals and triggers: the copy's pass runs Absense's port of collisionCheckObjects (GD's own "
            "would fire real objects); its overlap test is GD's. They are kept from acting, so one never changes the "
            "copy for the rest of the pass."));
        notes.push(std::string(
            "quiet: a cube, robot or spider less than 2 units into a slope over its head; GD shows no sign of the "
            "touch there, so only 'nothing kills' is checked."));
        notes.push(std::string("Not probed: the ground and the ceiling, and player 2's own borders in dual."));
        out["notes"] = notes;
        return out;
    }

} // namespace gucci

// ---------------------------------------------------------------- hooks
//
// Each only notes what GD did, for the probe's copy while it runs, and hands
// the call on unchanged.

class $modify(GBHitborderProbeDeathHook, PlayLayer) {
    // First in line, so the probe sees GD's decision even where a later hook
    // (a killer the simulation distrusts) swallows it.
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        gucci::observer().noteDeath(player, object);
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(GBHitborderProbePlayerHook, PlayerObject) {
    // A block's touch test passed (a slope's 1-unit legs come here too, with
    // the slope as the object); a death inside is this object's.
    bool collidedWithObject(float dt, GameObject* object, cocos2d::CCRect rect, bool skipCheck) {
        gucci::observer().note(this, object, gucci::Ev::Touch);
        gucci::InCall call(this, object);
        return PlayerObject::collidedWithObject(dt, object, rect, skipCheck);
    }

    bool collidedWithObjectInternal(float dt, GameObject* object, cocos2d::CCRect rect, bool skipCheck) {
        gucci::observer().note(this, object, gucci::Ev::Touch);
        gucci::InCall call(this, object);
        return PlayerObject::collidedWithObjectInternal(dt, object, rect, skipCheck);
    }

    // Rotated gameplay's own test: not read for the regions, but a death in
    // it is still this object's, not the ground's.
    bool handleRotatedCollisionInternal(float dt, GameObject* object, cocos2d::CCRect rect, bool skipCheck,
                                        bool skipPre, bool slope) {
        gucci::InCall call(this, object);
        return PlayerObject::handleRotatedCollisionInternal(dt, object, rect, skipCheck, skipPre, slope);
    }

    // A slope's line: a hit snaps the copy onto it (or under it) and puts it
    // on the slope; a hit that kills is seen at destroyPlayer.
    void collidedWithSlopeInternal(float dt, GameObject* object, bool forced) {
        auto& o = gucci::observer();
        if (!o.on || this != o.copy || !object)
            return PlayerObject::collidedWithSlopeInternal(dt, object, forced);
        gucci::InCall call(this, object);
        bool const atStart = o.atStart(this);
        CCPoint const before = this->getPosition();
        bool const onBefore = m_isOnSlope;
        PlayerObject::collidedWithSlopeInternal(dt, object, forced);
        CCPoint const after = this->getPosition();
        bool const moved = std::abs(after.x - before.x) > gucci::kStill || std::abs(after.y - before.y) > gucci::kStill;
        if (atStart && (moved || (m_isOnSlope && !onBefore)))
            o.events.push_back({object, gucci::Ev::Touch, true});
    }
};
