// Show Hitboxes, ported from Silicate: its hitbox drawing
// (silicate/src/assist/hitboxes.cpp) and the hooks that drive it
// (GJBaseGameLayer::processCommands for the trail; PlayLayer init,
// setupHasCompleted, showCompleteText and onQuit for the overlay's life).
// Silicate is by peony, GPL-3.0, which GucciBot is under too.
//
// What changed on the way across:
// - The switches are GucciBot's (GucciEngine), the look is HitboxOverlay's
//   Style: one fill amount for every part instead of one each, and the trail
//   is capped by Trail Length (ticks) instead of 676767 units.
// - Show On Death (hitboxOnDeath): drawn once the player has died, even with
//   Show Hitboxes off. Not in Silicate.
// - The live player boxes are always drawn above the trail (Silicate's
//   playerAbove / rotatedAbove defaults; the two options are not ported).
// - Player 2 is only drawn, sampled and checked against orbs in dual mode.
// - The progress bar, the % and the level-complete text are only ever moved
//   up to clear the overlay, never down.
// - PlayLayer only. Silicate draws in the editor as well (calling
//   updateStartValues() on every visible object each frame); that part is
//   not ported.
// - Absense's look-ahead copies (the extra PlayerObjects it adds to
//   m_objectLayer, and the layer's m_player1/2 while its CopyPlayersScope
//   holds) are never drawn, sampled or touched: nothing here runs while one
//   of them stands in for a real player.

#include "hacks/hitboxes.hpp"

#include "core/GucciBot.hpp"
#include "analysis/ac/shim.hpp"
#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "absense/judge.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>

using namespace geode::prelude;

namespace gucci {
    namespace {

        // Saved keys. The first three are engine fields engine_core's loader
        // does not read; the rest are the look.
        constexpr const char* kKeyOnDeath = "hack_hitbox_death";
        constexpr const char* kKeyTrail = "hack_hitbox_trail";
        constexpr const char* kKeyTrailLength = "hack_hitbox_trail_len";
        constexpr const char* kKeyLineWidth = "hitbox_line_width";
        constexpr const char* kKeyFill = "hitbox_fill";
        constexpr const char* kPartKeys[HitboxOverlay::PartCount] = {
            "player", "player_rotated", "player_inner", "player_circle", "player_dead",
            "solid",  "passable",       "hazard",       "interactable",  "interactable_active",
        };

        // Modifiers Silicate shows whether or not they are touch triggered
        // (the speed portals among them).
        constexpr std::array<int, 7> kAlwaysShownModifiers = {200, 201, 202, 203, 1334, 1816, 3643};

        // Trail boxes closer together on screen than 1 / this many pixels are
        // drawn once (Silicate's optimizationFactor default).
        constexpr float kTrailPixelFactor = 2.f;

        struct HbTrailUnit {
            CCRect rect;
            CCRect inner;
            std::array<CCPoint, 4> rotated{};
            bool hasRotated = false;
        };

        struct HbPen {
            bool on = false;
            ccColor4F line{};
            ccColor4F fill{};
        };

        struct HbPens {
            HbPen player, rotated, inner, circle, dead;
            HbPen solid, passable, hazard, interactable, active;
        };

        // One drawn frame's facts, worked out once rather than per object.
        struct HbFrame {
            GJBaseGameLayer* pl = nullptr;
            TrajectoryManager* traj = nullptr;
            PlayerObject* p1 = nullptr;  // the real player 1
            PlayerObject* p2 = nullptr;  // the real player 2, in dual mode only
            CCRect p1Rect;
            CCRect p2Rect;
            OBB2D* p1Box = nullptr;
            OBB2D* p2Box = nullptr;
            bool ringTouched = false;  // either player is touching an orb
            float width = 0.5f;
            float zoom = 1.f;
            HbPens pens;
        };

        // The node the overlay hangs from. It is added to the PlayLayer itself
        // (above the UI layer, as in Silicate) and carries the object layer's
        // transform, so everything inside it is drawn in level coordinates.
        class HbOverlayNode : public CCNode {
        public:
            GJBaseGameLayer* m_layer = nullptr;  // the level it was made for

            static HbOverlayNode* create(GJBaseGameLayer* layer) {
                auto* node = new HbOverlayNode();
                if (node->init()) {
                    node->m_layer = layer;
                    node->autorelease();
                    return node;
                }
                delete node;
                return nullptr;
            }

            void visit() override;
        };

        struct HbState {
            HbOverlayNode* container = nullptr;  // retained while we hold it
            // Children of the container, bottom to top.
            CCDrawNode* trailRotated = nullptr;
            CCDrawNode* trailCircle = nullptr;
            CCDrawNode* trailSolid = nullptr;
            CCDrawNode* trailInner = nullptr;
            CCDrawNode* main = nullptr;  // objects, then the live players
            bool drawn = false;          // something is on the nodes
            std::deque<HbTrailUnit> trailP1;
            std::deque<HbTrailUnit> trailP2;
        };

        HbState& hbState() {
            static HbState s;
            return s;
        }

        void HbOverlayNode::visit() {
            if (m_layer && getParent() == m_layer && hbState().container == this)
                HitboxOverlay::get()->paint(m_layer);
            CCNode::visit();
        }

        size_t trailCap() {
            int const len = GucciEngine::get()->hitboxTrailLength;
            return static_cast<size_t>(
                std::clamp(len, HitboxOverlay::kMinTrailTicks, HitboxOverlay::kMaxTrailTicks));
        }

        void trimTrail(std::deque<HbTrailUnit>& trail, size_t cap) {
            while (trail.size() > cap)
                trail.pop_front();
        }

        uint32_t packColour(float const* rgba) {
            uint32_t out = 0;
            for (int i = 0; i < 4; ++i) {
                auto const byte = static_cast<uint32_t>(std::lround(std::clamp(rgba[i], 0.f, 1.f) * 255.f));
                out = (out << 8) | byte;
            }
            return out;
        }

        void unpackColour(uint32_t packed, float* rgba) {
            for (int i = 3; i >= 0; --i) {
                rgba[i] = static_cast<float>(packed & 0xFFu) / 255.f;
                packed >>= 8;
            }
        }

        HbPen penFor(HitboxOverlay::Style const& st, HitboxOverlay::Part part) {
            auto const& p = st.parts[part];
            float const alpha = std::clamp(p.rgba[3], 0.f, 1.f);
            float const fill = alpha * std::clamp(st.fill, 0.f, 1.f);
            HbPen pen;
            pen.on = p.on;
            pen.line = ccColor4F{p.rgba[0], p.rgba[1], p.rgba[2], alpha};
            pen.fill = ccColor4F{p.rgba[0], p.rgba[1], p.rgba[2], fill};
            return pen;
        }

        HbPens pensFor(HitboxOverlay::Style const& st) {
            using P = HitboxOverlay::Part;
            HbPens pens;
            pens.player = penFor(st, P::Player);
            pens.rotated = penFor(st, P::PlayerRotated);
            pens.inner = penFor(st, P::PlayerInner);
            pens.circle = penFor(st, P::PlayerCircle);
            pens.dead = penFor(st, P::PlayerDead);
            pens.solid = penFor(st, P::Solid);
            pens.passable = penFor(st, P::Passable);
            pens.hazard = penFor(st, P::Hazard);
            pens.interactable = penFor(st, P::Interactable);
            pens.active = penFor(st, P::InteractableActive);
            return pens;
        }

        unsigned circleSegments(float radius, float zoom) {
            float const n = std::max(radius, 8.f) * 2.f * std::sqrt(zoom);
            return static_cast<unsigned>(std::clamp(n, 8.f, 512.f));
        }

        // Gives the container the object layer's transform, whatever sits
        // between the two.
        void syncToObjectLayer(CCNode* node, CCNode* objectLayer) {
            if (!node || !objectLayer || !node->getParent())
                return;
            node->setAdditionalTransform(CCAffineTransformConcat(objectLayer->nodeToWorldTransform(),
                                                                 node->getParent()->worldToNodeTransform()));
        }

        // The screen, in object layer coordinates.
        CCRect objectSpaceViewport(CCNode* objectLayer) {
            auto const size = CCDirector::get()->getWinSize();
            auto const toObject = objectLayer->worldToNodeTransform();
            CCPoint const corners[4] = {
                CCPointApplyAffineTransform(CCPoint(0.f, 0.f), toObject),
                CCPointApplyAffineTransform(CCPoint(size.width, 0.f), toObject),
                CCPointApplyAffineTransform(CCPoint(0.f, size.height), toObject),
                CCPointApplyAffineTransform(CCPoint(size.width, size.height), toObject),
            };
            float minX = corners[0].x, maxX = corners[0].x;
            float minY = corners[0].y, maxY = corners[0].y;
            for (auto const& c : corners) {
                minX = std::min(minX, c.x);
                maxX = std::max(maxX, c.x);
                minY = std::min(minY, c.y);
                maxY = std::max(maxY, c.y);
            }
            return CCRect(minX, minY, maxX - minX, maxY - minY);
        }

        // The node under `layer` that holds `node` (or is it).
        CCNode* childOfLayerHolding(CCNode* node, CCNode* layer) {
            for (auto* n = node; n; n = n->getParent()) {
                if (n->getParent() == layer)
                    return n;
            }
            return nullptr;
        }

        // Every object in the sections GD has marked visible.
        template <class Fn>
        void forEachVisibleObject(GJBaseGameLayer* pl, Fn&& fn) {
            auto& sections = pl->m_sections;
            auto& sizes = pl->m_sectionSizes;
            for (int i = std::max(pl->m_leftSectionIndex, 0);
                 i <= pl->m_rightSectionIndex && static_cast<size_t>(i) < sections.size(); ++i) {
                auto* column = sections[i];
                auto* columnSizes = static_cast<size_t>(i) < sizes.size() ? sizes[i] : nullptr;
                if (!column || !columnSizes)
                    continue;
                for (int j = std::max(pl->m_bottomSectionIndex, 0);
                     j <= pl->m_topSectionIndex && static_cast<size_t>(j) < column->size(); ++j) {
                    auto* section = column->at(j);
                    if (!section || static_cast<size_t>(j) >= columnSizes->size())
                        continue;
                    int const count = columnSizes->at(j);
                    for (int k = 0; k < count && static_cast<size_t>(k) < section->size(); ++k) {
                        if (auto* object = section->at(k))
                            fn(object);
                    }
                }
            }
        }

        bool touchingRing(HbFrame const& f, GameObject* object) {
            if (!geode::cast::typeinfo_cast<RingObject*>(object))
                return false;
            if (f.p1->m_touchingRings && f.p1->m_touchingRings->containsObject(object))
                return true;
            return f.p2 && f.p2->m_touchingRings && f.p2->m_touchingRings->containsObject(object);
        }

        void paintObject(CCDrawNode* node, HbFrame const& f, GameObject* object) {
            auto* pl = f.pl;
            if (object->m_isGroupDisabled || !object->m_isActivated ||
                object->m_objectType == GameObjectType::Decoration)
                return;
            // GD does not keep players in the sections; this is a guard all
            // the same, so neither a real player nor one of Absense's copies
            // is ever drawn (or touched) as an object.
            if (object == pl->m_player1 || object == pl->m_player2 ||
                f.traj->isFakePlayer(static_cast<PlayerObject*>(object)))
                return;

            auto const& pens = f.pens;
            float const width = f.width;
            // Leave the object's rect cache as GD had it (GD's own debug draw
            // does the same).
            bool const rectDirty = object->m_isObjectRectDirty;
            bool const offsetDone = object->m_boxOffsetCalculated;

            switch (object->m_objectType) {
            case GameObjectType::Solid: {
                HbPen const& pen = object->m_isPassable ? pens.passable : pens.solid;
                if (pen.on)
                    node->drawRect(object->getObjectRect(), pen.fill, width, pen.line, BorderAlignment::Inside);
                break;
            }

            case GameObjectType::Slope: {
                HbPen const& pen = object->m_isPassable ? pens.passable : pens.solid;
                if (!pen.on)
                    break;
                CCRect const rect = object->getObjectRect();
                CCPoint const topRight(rect.getMaxX(), rect.getMaxY());
                std::array<CCPoint, 3> verts = {
                    rect.origin,
                    CCPoint(rect.getMinX(), rect.getMaxY()),
                    CCPoint(rect.getMaxX(), rect.getMinY()),
                };
                switch (object->m_slopeDirection) {
                case 0:
                case 7:
                    verts[1] = topRight;
                    break;
                case 1:
                case 5:
                    verts[0] = topRight;
                    break;
                case 3:
                case 6:
                    verts[2] = topRight;
                    break;
                default:
                    break;
                }
                node->drawPolygon(verts.data(), 3, pen.fill, width, pen.line, BorderAlignment::Inside);
                break;
            }

            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard: {
                if (object == pl->m_anticheatSpike || !pens.hazard.on)
                    break;
                float const radius = object->m_objectRadius * std::max(object->m_scaleX, object->m_scaleY);
                if (radius > 0.f) {
                    node->drawCircle(object->getPosition(), radius, pens.hazard.fill, width, pens.hazard.line,
                                     circleSegments(radius, f.zoom));
                } else if (auto* box = object->m_orientedBox) {
                    node->drawPolygon(box->m_corners.data(), 4, pens.hazard.fill, width, pens.hazard.line,
                                      BorderAlignment::Inside);
                } else {
                    node->drawRect(object->getObjectRect(), pens.hazard.fill, width, pens.hazard.line,
                                   BorderAlignment::Inside);
                }
                break;
            }

            default: {
                // Orbs, pads and portals already used are gone.
                if (object->hasBeenActivatedByPlayer(f.p1) && (!f.p2 || object->hasBeenActivatedByPlayer(f.p2)))
                    break;
                if (object == pl->m_player1CollisionBlock || object == pl->m_player2CollisionBlock)
                    break;
                if (object->m_objectType == GameObjectType::Modifier &&
                    std::find(kAlwaysShownModifiers.begin(), kAlwaysShownModifiers.end(), object->m_objectID) ==
                        kAlwaysShownModifiers.end() &&
                    !static_cast<EffectGameObject*>(object)->m_isTouchTriggered)
                    break;
                if (!pens.interactable.on)
                    break;

                HbPen const* pen = &pens.interactable;
                if (pens.active.on && f.ringTouched && touchingRing(f, object))
                    pen = &pens.active;

                if (auto* box = object->m_orientedBox) {
                    if (pens.active.on && ((f.p1Box && box->overlaps(f.p1Box)) || (f.p2Box && box->overlaps(f.p2Box))))
                        pen = &pens.active;
                    node->drawPolygon(box->m_corners.data(), 4, pen->fill, width, pen->line, BorderAlignment::Inside);
                } else {
                    CCRect const rect = object->getObjectRect();
                    if (pens.active.on && (rect.intersectsRect(f.p1Rect) || (f.p2 && rect.intersectsRect(f.p2Rect))))
                        pen = &pens.active;
                    node->drawRect(rect, pen->fill, width, pen->line, BorderAlignment::Inside);
                }
                break;
            }
            }

            object->m_isObjectRectDirty = rectDirty;
            object->m_boxOffsetCalculated = offsetDone;
        }

        void paintPlayer(CCDrawNode* node, HbFrame const& f, PlayerObject* player, CCRect const& rect, OBB2D* box) {
            auto const& pens = f.pens;
            if (pens.rotated.on && box)
                node->drawPolygon(box->m_corners.data(), 4, pens.rotated.fill, f.width, pens.rotated.line,
                                  BorderAlignment::Inside);
            float const radius = rect.size.width / 2.f;
            if (pens.circle.on && radius > 0.f)
                node->drawCircle(player->getPosition(), radius, ccColor4F{0.f, 0.f, 0.f, 0.f}, f.width, pens.circle.line,
                                 circleSegments(radius, f.zoom));
            if (pens.player.on)
                node->drawRect(rect, pens.player.fill, f.width, pens.player.line, BorderAlignment::Inside);
            if (pens.inner.on)
                node->drawRect(player->getObjectRect(0.3f, 0.3f), pens.inner.fill, f.width, pens.inner.line,
                               BorderAlignment::Inside);
            if (pens.dead.on && player->m_isDead)
                node->drawRect(rect, pens.dead.fill, f.width, pens.dead.line, BorderAlignment::Inside);
        }

        void paintTrail(HbState& s, HbFrame const& f) {
            auto const& pens = f.pens;
            bool const rotated = pens.rotated.on;
            bool const solid = pens.player.on;
            bool const circle = pens.circle.on;
            bool const inner = pens.inner.on;
            if (!rotated && !solid && !circle && !inner)
                return;

            auto* objectLayer = f.pl->m_objectLayer;
            CCRect const view = objectSpaceViewport(objectLayer);
            float const minX = view.getMinX(), maxX = view.getMaxX();
            float const minY = view.getMinY(), maxY = view.getMaxY();
            auto const toWorld = objectLayer->nodeToWorldTransform();

            // Points to pixels; the layer's scale is there for zoom mods
            // (Silicate: compatibility with Zoooom).
            auto* director = CCDirector::get();
            auto const points = director->getWinSize();
            auto const pixels = director->getWinSizeInPixels();
            float const ratio = std::max(points.width > 0.f ? pixels.width / points.width : 1.f,
                                         points.height > 0.f ? pixels.height / points.height : 1.f) *
                                f.pl->getScale();
            float const grid = kTrailPixelFactor * ratio;

            auto drawTrail = [&](std::deque<HbTrailUnit>& trail) {
                bool hasLast = false;
                int lastX = 0, lastY = 0;
                for (auto& unit : trail) {
                    auto const& r = unit.rect;
                    if (r.getMaxX() < minX || r.getMinX() > maxX || r.getMinY() > maxY || r.getMaxY() < minY)
                        continue;
                    CCPoint const centre(r.getMidX(), r.getMidY());
                    CCPoint const onScreen = CCPointApplyAffineTransform(centre, toWorld);
                    int const x = static_cast<int>(std::floor(onScreen.x * grid));
                    int const y = static_cast<int>(std::floor(onScreen.y * grid));
                    if (hasLast && x == lastX && y == lastY)
                        continue;
                    hasLast = true;
                    lastX = x;
                    lastY = y;

                    if (rotated && unit.hasRotated)
                        s.trailRotated->drawPolygon(unit.rotated.data(), 4, pens.rotated.fill, f.width,
                                                    pens.rotated.line, BorderAlignment::Inside);
                    if (solid)
                        s.trailSolid->drawRect(r, pens.player.fill, f.width, pens.player.line, BorderAlignment::Inside);
                    if (circle) {
                        float const radius = r.size.width / 2.f;
                        if (radius > 0.f)
                            s.trailCircle->drawCircle(centre, radius, pens.circle.fill, f.width, pens.circle.line,
                                                      circleSegments(radius, f.zoom));
                    }
                    if (inner)
                        s.trailInner->drawRect(unit.inner, pens.inner.fill, f.width, pens.inner.line,
                                               BorderAlignment::Inside);
                }
            };
            drawTrail(s.trailP1);
            drawTrail(s.trailP2);
        }

        void appendTrail(std::deque<HbTrailUnit>& trail, PlayerObject* player, size_t cap) {
            HbTrailUnit unit;
            unit.rect = player->getObjectRect();
            unit.inner = player->getObjectRect(0.3f, 0.3f);
            if (auto* box = player->getOrientedBox()) {
                unit.rotated = box->m_corners;
                unit.hasRotated = true;
            }
            trail.push_back(unit);
            trimTrail(trail, cap);
        }

        void clearNodes(HbState& s) {
            for (auto* node : {s.trailRotated, s.trailCircle, s.trailSolid, s.trailInner, s.main}) {
                if (node)
                    node->clear();
            }
        }

        // The draw nodes, not the container, are hidden while nothing is
        // shown: the container's visit() is what paints, so it has to keep
        // being visited.
        void showNodes(HbState& s, bool visible) {
            for (auto* node : {s.trailRotated, s.trailCircle, s.trailSolid, s.trailInner, s.main}) {
                if (node && node->isVisible() != visible)
                    node->setVisible(visible);
            }
        }

    } // namespace

    // ---------------------------------------------------------------- style

    HitboxOverlay* HitboxOverlay::get() {
        static HitboxOverlay inst;
        return &inst;
    }

    HitboxOverlay::HitboxOverlay() : style(defaults()) {}

    HitboxOverlay::Style HitboxOverlay::defaults() {
        Style st;
        auto set = [&st](Part part, float r, float g, float b) {
            st.parts[part].on = true;
            st.parts[part].rgba[0] = r;
            st.parts[part].rgba[1] = g;
            st.parts[part].rgba[2] = b;
            st.parts[part].rgba[3] = 1.f;
        };
        set(Player, 1.f, 0.f, 0.f);
        set(PlayerRotated, 0.5f, 0.f, 0.f);
        set(PlayerInner, 0.f, 0.f, 1.f);
        set(PlayerCircle, 1.f, 0.f, 0.f);
        set(PlayerDead, 1.f, 0.4f, 0.f);
        set(Solid, 0.f, 0.f, 1.f);
        set(Passable, 0.f, 1.f, 1.f);
        set(Hazard, 1.f, 0.f, 0.f);
        set(Interactable, 1.f, 1.f, 0.f);
        set(InteractableActive, 0.2f, 1.f, 0.f);
        st.lineWidth = 0.5f;
        st.fill = 0.f;
        return st;
    }

    const char* HitboxOverlay::partLabel(Part part) {
        switch (part) {
        case Player: return "Player";
        case PlayerRotated: return "Player, rotated";
        case PlayerInner: return "Player, inner";
        case PlayerCircle: return "Player, circle";
        case PlayerDead: return "Player, dead";
        case Solid: return "Blocks";
        case Passable: return "Passable blocks";
        case Hazard: return "Hazards";
        case Interactable: return "Orbs, pads and portals";
        case InteractableActive: return "Orbs being touched";
        default: return "?";
        }
    }

    const char* HitboxOverlay::partHint(Part part) {
        switch (part) {
        case PlayerRotated: return "The box turned with the icon";
        case PlayerInner: return "The small box blocks kill with";
        case PlayerDead: return "Over the box once the player dies";
        case Interactable: return "And touch triggers";
        case InteractableActive: return "Or pads and portals the player overlaps";
        default: return nullptr;
        }
    }

    void HitboxOverlay::loadSettings() {
        auto* mod = Mod::get();
        auto* gb = GucciEngine::get();
        gb->hitboxOnDeath = mod->getSavedValue<bool>(kKeyOnDeath, gb->hitboxOnDeath);
        gb->hitboxTrail = mod->getSavedValue<bool>(kKeyTrail, gb->hitboxTrail);
        auto const len = mod->getSavedValue<int64_t>(kKeyTrailLength, gb->hitboxTrailLength);
        gb->hitboxTrailLength = static_cast<int>(
            std::clamp<int64_t>(len, kMinTrailTicks, kMaxTrailTicks));

        Style const def = defaults();
        style.lineWidth = static_cast<float>(
            std::clamp(mod->getSavedValue<double>(kKeyLineWidth, def.lineWidth), 0.05, 10.0));
        style.fill = static_cast<float>(std::clamp(mod->getSavedValue<double>(kKeyFill, def.fill), 0.0, 1.0));
        for (int i = 0; i < PartCount; ++i) {
            std::string const stem = std::string("hitbox_") + kPartKeys[i];
            auto& part = style.parts[i];
            part.on = mod->getSavedValue<bool>(stem + "_on", def.parts[i].on);
            auto const packed = mod->getSavedValue<int64_t>(stem + "_color", packColour(def.parts[i].rgba));
            unpackColour(static_cast<uint32_t>(packed), part.rgba);
        }
    }

    void HitboxOverlay::saveStyle() const {
        auto* mod = Mod::get();
        mod->setSavedValue<double>(kKeyLineWidth, style.lineWidth);
        mod->setSavedValue<double>(kKeyFill, style.fill);
        for (int i = 0; i < PartCount; ++i) {
            std::string const stem = std::string("hitbox_") + kPartKeys[i];
            mod->setSavedValue<bool>(stem + "_on", style.parts[i].on);
            mod->setSavedValue<int64_t>(stem + "_color", static_cast<int64_t>(packColour(style.parts[i].rgba)));
        }
    }

    // ---------------------------------------------------------------- life

    bool HitboxOverlay::attachedTo(CCNode* layer) const {
        auto const& s = hbState();
        return layer && s.container && s.container->getParent() == layer;
    }

    void HitboxOverlay::init(PlayLayer* pl) {
        if (!pl || !pl->m_objectLayer)
            return;
        if (attachedTo(pl)) {
            raiseGameUI(pl);
            return;
        }
        destroy();

        auto* container = HbOverlayNode::create(pl);
        if (!container)
            return;
        auto addDrawNode = [container](const char* id, int z) -> CCDrawNode* {
            auto* node = CCDrawNode::create();
            if (!node)
                return nullptr;
            node->m_bUseArea = false;  // RobTop's culling; the trail spans the level
            node->setID(id);
            node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            container->addChild(node, z);
            return node;
        };
        auto* trailRotated = addDrawNode("hitbox-trail-rotated"_spr, 0);
        auto* trailCircle = addDrawNode("hitbox-trail-circle"_spr, 1);
        auto* trailSolid = addDrawNode("hitbox-trail-solid"_spr, 2);
        auto* trailInner = addDrawNode("hitbox-trail-inner"_spr, 3);
        auto* main = addDrawNode("hitbox-node"_spr, 4);
        if (!trailRotated || !trailCircle || !trailSolid || !trailInner || !main)
            return;  // the container is autoreleased, nothing else holds it

        container->setID("hitbox-layer"_spr);
        container->retain();
        auto& s = hbState();
        s.container = container;
        s.trailRotated = trailRotated;
        s.trailCircle = trailCircle;
        s.trailSolid = trailSolid;
        s.trailInner = trailInner;
        s.main = main;
        s.drawn = false;

        int const z = (pl->m_uiLayer ? pl->m_uiLayer->getZOrder() : 101) + 1;
        pl->addChild(container, z);
        syncToObjectLayer(container, pl->m_objectLayer);
        showNodes(s, false);  // until there is something to draw
        raiseGameUI(pl);
    }

    void HitboxOverlay::draw(PlayLayer* pl) {
        if (!pl || attachedTo(pl) || !GucciEngine::get()->enabled)
            return;
        init(pl);
    }

    void HitboxOverlay::destroy() {
        auto& s = hbState();
        if (s.container) {
            s.container->removeFromParent();
            s.container->release();
        }
        s.container = nullptr;
        s.trailRotated = nullptr;
        s.trailCircle = nullptr;
        s.trailSolid = nullptr;
        s.trailInner = nullptr;
        s.main = nullptr;
        s.drawn = false;
        clearTrail();
    }

    void HitboxOverlay::clearTrail() {
        auto& s = hbState();
        s.trailP1.clear();
        s.trailP2.clear();
    }

    void HitboxOverlay::raiseGameUI(PlayLayer* pl) {
        if (!pl || !attachedTo(pl) || !pl->m_progressBar)
            return;
        int const z = hbState().container->getZOrder();
        auto* bar = childOfLayerHolding(pl->m_progressBar, pl);
        auto* label = childOfLayerHolding(pl->m_percentageLabel, pl);
        // Inside the UI layer they are already under the overlay together
        // with everything else there; leave them be (as Silicate does).
        if (bar == pl->m_uiLayer || label == pl->m_uiLayer)
            return;
        if (bar && bar->getZOrder() <= z)
            bar->setZOrder(z + 1);
        if (label && label != bar && label->getZOrder() <= z + 1)
            label->setZOrder(z + 2);
    }

    void HitboxOverlay::raiseAbove(CCNode* node) {
        auto const& s = hbState();
        if (!s.container || !node || node == s.container)
            return;
        int const z = s.container->getZOrder();
        if (node->getZOrder() <= z)
            node->setZOrder(z + 1 + std::max(0, node->getZOrder()));
    }

    // ---------------------------------------------------------------- tick

    void HitboxOverlay::sampleTick(GJBaseGameLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled || !gb->hitboxTrail || !(gb->showHitboxes || gb->hitboxOnDeath))
            return;
        if (!pl || pl != PlayLayer::get() || gb->updater.m_onlyRefresh)
            return;
        if (pl->m_resumeTimer > 0 || pl->m_playerDied || pl->m_levelEndAnimationStarted)
            return;
        // A script the judge plays for real is put back afterwards; it is not
        // where the player went.
        if (absense::judge::active())
            return;

        auto& traj = ::Bot::get()->trajectory();
        auto* p1 = pl->m_player1;
        if (!p1 || traj.isFakePlayer(p1))
            return;
        auto& s = hbState();
        size_t const cap = trailCap();
        appendTrail(s.trailP1, p1, cap);
        if (pl->m_gameState.m_isDualMode) {
            auto* p2 = pl->m_player2;
            if (p2 && !traj.isFakePlayer(p2))
                appendTrail(s.trailP2, p2, cap);
        }
    }

    // ---------------------------------------------------------------- frame

    void HitboxOverlay::paint(GJBaseGameLayer* pl) {
        auto& s = hbState();
        if (!pl || !s.container || !s.main)
            return;
        if (s.drawn) {
            clearNodes(s);
            s.drawn = false;
        }

        auto* gb = GucciEngine::get();
        bool show = gb->enabled && (gb->showHitboxes || gb->hitboxOnDeath) && pl->m_objectLayer;
        PlayerObject* p1 = nullptr;
        PlayerObject* p2 = nullptr;
        TrajectoryManager* traj = nullptr;
        if (show) {
            traj = &::Bot::get()->trajectory();
            p1 = pl->m_player1;
            p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
            if (!p1 || traj->isFakePlayer(p1) || (p2 && traj->isFakePlayer(p2)))
                show = false;
            else if (!gb->showHitboxes)
                show = pl->m_playerDied || p1->m_isDead || (p2 && p2->m_isDead);
        }
        showNodes(s, show);
        if (!show)
            return;
        s.drawn = true;

        syncToObjectLayer(s.container, pl->m_objectLayer);

        HbFrame f;
        f.pl = pl;
        f.traj = traj;
        f.p1 = p1;
        f.p2 = p2;
        f.zoom = pl->m_gameState.m_cameraZoom > 0.f ? pl->m_gameState.m_cameraZoom : 1.f;
        f.width = style.lineWidth / f.zoom;
        f.pens = pensFor(style);
        f.p1Rect = p1->getObjectRect();
        f.p1Box = p1->getOrientedBox();
        if (p2) {
            f.p2Rect = p2->getObjectRect();
            f.p2Box = p2->getOrientedBox();
        }
        f.ringTouched = (p1->m_touchingRings && p1->m_touchingRings->count() > 0) ||
                        (p2 && p2->m_touchingRings && p2->m_touchingRings->count() > 0);

        if (gb->hitboxTrail) {
            size_t const cap = trailCap();
            trimTrail(s.trailP1, cap);
            trimTrail(s.trailP2, cap);
            paintTrail(s, f);
        }

        forEachVisibleObject(pl, [&](GameObject* object) { paintObject(s.main, f, object); });

        paintPlayer(s.main, f, p1, f.p1Rect, f.p1Box);
        if (p2)
            paintPlayer(s.main, f, p2, f.p2Rect, f.p2Box);
    }

} // namespace gucci

// ---------------------------------------------------------------- hooks

// The trail: one sample per physics tick, after GD's commands for it
// (Silicate samples at the same point). A pass with no time in it is not a
// tick (processCommands only moves the command index when dt > 0).
class $modify(GBHitboxTickHook, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (dt > 0.f)
            gucci::HitboxOverlay::get()->sampleTick(this);
    }
};

class $modify(GBHitboxPlayLayerHook, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (gucci::GucciEngine::get()->enabled)
            gucci::HitboxOverlay::get()->init(this);
        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        gucci::HitboxOverlay::get()->raiseGameUI(this);
    }

    // The level-complete text is added to the PlayLayer under the overlay;
    // lift what this call adds above it.
    void showCompleteText() {
        auto* overlay = gucci::HitboxOverlay::get();
        auto* children = this->getChildren();
        if (!overlay->attachedTo(this) || !children)
            return PlayLayer::showCompleteText();

        std::unordered_set<CCNode*> before;
        for (unsigned i = 0; i < children->count(); ++i)
            before.insert(static_cast<CCNode*>(children->objectAtIndex(i)));

        PlayLayer::showCompleteText();

        children = this->getChildren();
        if (!children)
            return;
        for (unsigned i = 0; i < children->count(); ++i) {
            auto* child = static_cast<CCNode*>(children->objectAtIndex(i));
            if (child && !before.contains(child))
                overlay->raiseAbove(child);
        }
    }

    void onQuit() {
        gucci::HitboxOverlay::get()->destroy();
        PlayLayer::onQuit();
    }
};

$on_mod(Loaded) {
    gucci::HitboxOverlay::get()->loadSettings();
}
