// Hitborders: see hitborders.hpp. The shapes are tools/hitborder_geom's; this
// file reads the level into it once a drawn frame, draws what comes back,
// and keeps the player's own path and clicks.
//
// Drawing. Overlapping regions get one outline around them and one coat of
// fill, the look of NaN GD's videos: every region is drawn twice into a
// texture of its own with blending off, first grown by the line width in the
// outline color, then as it is in the fill color, blue first, then green,
// then red. Inside a color the fills paint over the inner outlines, so only
// the union's edge is left, and red ends on top. The texture then goes over
// the level once, with ordinary blending. The texture is the size of the GL
// viewport at the time and the matrices aren't touched, so the same pass
// works inside the video renderer's capture, nested in its framebuffer. With
// "One outline" off, or where the GPU won't make the texture, each region is
// drawn with its own outline, as Show Hitboxes draws.
//
// cocos' drawPolygon grows a polygon drawn with no border by half a unit
// (its anti-alias fringe; RobTop's CCDrawNode::drawPolygon keeps it, the 0.5
// at libcocos2d +0x13402). The merged pass insets its polygons by that half
// unit first, so the edges land where GD's rules put them.
//
// What's read, per object near the camera, is what GD's own collision pass
// reads (RESEARCH.md §7.4): getObjectRect() (its dirty flags put back after,
// as GD's debug draw does), the oriented box only of an object already
// flagged for it (getOrientedBox() would flag any other one, which changes
// gameplay), the circle at getRealPosition(), and GD's own filters
// (m_isDisabled and m_isGroupDisabled, not m_isActivated).

#include "hacks/hitborders.hpp"

#include "absense/compat/bot.hpp"
#include "absense/judge.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "core/GucciBot.hpp"
#include "tools/hitborder_geom.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace gucci {
    namespace {

        namespace hb = hitborder;

        constexpr const char* kPartKeys[HitborderOverlay::PartCount] = {"lethal", "touch", "special", "marks"};

        // Modifiers Show Hitboxes draws whether or not they are touch
        // triggered (the speed portals among them): the same list.
        constexpr std::array<int, 7> kAlwaysShownModifiers = {200, 201, 202, 203, 1334, 1816, 3643};

        // In the object layer: the icon is at z 59 (GJBaseGameLayer::
        // createPlayer adds both players there, 0x20b4f7), so the borders go
        // just under it; the path and the point above everything there (the
        // macro path is 1600 and 1601).
        constexpr int kBordersZ = 58;
        constexpr int kMarksZ = 1603;

        // A tick's step longer than this is a teleport or a respawn: the
        // path breaks there.
        constexpr float kPathBreak = 90.f;

        struct PathPoint {
            CCPoint p;
            uint32_t tick = 0;
            bool breakBefore = false;
        };

        struct ClickMark {
            CCPoint p;
            uint32_t tick = 0;
            bool pressed = false;
        };

        // A texture and its framebuffer, for the merged pass.
        struct Fbo {
            GLuint fbo = 0;
            GLuint tex = 0;
            int w = 0;
            int h = 0;
            unsigned lastFrame = 0;

            void release() {
                if (fbo)
                    glDeleteFramebuffers(1, &fbo);
                if (tex)
                    ccGLDeleteTexture(tex);
                *this = Fbo{};
            }

            bool make(int width, int height) {
                release();
                GLint oldFbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
                glGenTextures(1, &tex);
                ccGLBindTexture2D(tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
                bool const ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFbo));
                ccGLBindTexture2D(0);
                w = width;
                h = height;
                if (!ok)
                    release();
                return ok;
            }
        };

        // The nodes the overlay hangs in the object layer. Their visit() is
        // where everything is drawn.
        class BordersNode : public CCNode {
        public:
            GJBaseGameLayer* m_layer = nullptr;
            static BordersNode* create(GJBaseGameLayer* layer) {
                auto* node = new BordersNode();
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

        class MarksNode : public CCNode {
        public:
            GJBaseGameLayer* m_layer = nullptr;
            static MarksNode* create(GJBaseGameLayer* layer) {
                auto* node = new MarksNode();
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

        struct State {
            BordersNode* borders = nullptr;  // retained while held
            MarksNode* marks = nullptr;      // retained while held
            // Retained, never in the tree: the nodes' visit() draws them.
            CCDrawNode* merged = nullptr;     // blending off, for the texture
            CCDrawNode* plain = nullptr;      // each region with its own outline
            CCDrawNode* second = nullptr;     // player 2's outlines
            CCDrawNode* marksDraw = nullptr;  // the path, the clicks, the point
            unsigned builtFrame = ~0u;
            bool haveRegions = false;
            bool useMerged = false;
            bool fboFailed = false;  // the GPU wouldn't make the texture: per-region outlines from then on
            std::array<Fbo, 2> fbos{};  // the screen's size, and the renderer's while it captures
            std::deque<PathPoint> path[2];
            std::deque<ClickMark> clicks[2];
            uint32_t tick = 0;
        };

        State& st() {
            static State s;
            return s;
        }

        bool heldBy(GJBaseGameLayer* layer, CCNode* node) {
            return layer && node && node->getParent() == layer->m_objectLayer;
        }

        void BordersNode::visit() {
            auto& s = st();
            if (!m_bVisible || s.borders != this || !heldBy(m_layer, this))
                return;
            auto* overlay = HitborderOverlay::get();
            overlay->paint(m_layer);
            if (!s.haveRegions)
                return;
            kmGLPushMatrix();
            transform();
            bool drawn = false;
            if (s.useMerged)
                drawn = overlay->drawMerged();
            if (!drawn && s.plain)
                s.plain->draw();
            if (s.second)
                s.second->draw();
            kmGLPopMatrix();
        }

        void MarksNode::visit() {
            auto& s = st();
            if (!m_bVisible || s.marks != this || !heldBy(m_layer, this))
                return;
            HitborderOverlay::get()->paint(m_layer);
            if (!s.marksDraw)
                return;
            kmGLPushMatrix();
            transform();
            s.marksDraw->draw();
            kmGLPopMatrix();
        }

        // ---------------------------------------------------------------- colors and saving

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

        ccColor4F lineColour(HitborderOverlay::Settings const& st, int part) {
            auto const* c = st.rgba[part];
            return {c[0], c[1], c[2], std::clamp(c[3], 0.f, 1.f)};
        }

        ccColor4F fillColour(HitborderOverlay::Settings const& st, int part) {
            auto const* c = st.rgba[part];
            return {c[0], c[1], c[2], std::clamp(c[3], 0.f, 1.f) * std::clamp(st.fill, 0.f, 1.f)};
        }

        // Player 2's outlines: halfway to white.
        ccColor4F lighter(ccColor4F c) {
            return {(c.r + 1.f) / 2.f, (c.g + 1.f) / 2.f, (c.b + 1.f) / 2.f, c.a};
        }

        int partFor(hb::Kind kind) {
            switch (kind) {
            case hb::Kind::Lethal: return HitborderOverlay::Lethal;
            case hb::Kind::Touch: return HitborderOverlay::Touch;
            case hb::Kind::Special: return HitborderOverlay::Special;
            }
            return HitborderOverlay::Lethal;
        }

        // ---------------------------------------------------------------- the level

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

        // Every object in the sections GD has marked visible, and one more
        // section all round: a region grown by up to 15 plus a saw's radius
        // can reach the screen from just off it.
        template <class Fn>
        void forEachNearbyObject(GJBaseGameLayer* pl, Fn&& fn) {
            auto& sections = pl->m_sections;
            auto& sizes = pl->m_sectionSizes;
            int const left = std::max(pl->m_leftSectionIndex - 1, 0);
            int const right = pl->m_rightSectionIndex + 1;
            int const bottom = std::max(pl->m_bottomSectionIndex - 1, 0);
            int const top = pl->m_topSectionIndex + 1;
            for (int i = left; i <= right && static_cast<size_t>(i) < sections.size(); ++i) {
                auto* column = sections[i];
                auto* columnSizes = static_cast<size_t>(i) < sizes.size() ? sizes[i] : nullptr;
                if (!column || !columnSizes)
                    continue;
                for (int j = bottom; j <= top && static_cast<size_t>(j) < column->size(); ++j) {
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

        hb::Rect toRect(CCRect const& r) {
            return {r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()};
        }

        hb::Player readPlayer(PlayerObject* p) {
            hb::Player r;
            CCPoint const pos = p->getPosition();
            r.pos = {pos.x, pos.y};
            CCRect const outer = p->getObjectRect();
            CCRect const inner = p->getObjectRect(0.3f, 0.3f);
            r.hx = outer.size.width / 2.0;
            r.hy = outer.size.height / 2.0;
            r.ix = inner.size.width / 2.0;
            r.iy = inner.size.height / 2.0;
            r.rotation = p->getRotation();
            using M = hb::Mode;
            r.mode = p->m_isShip     ? M::Ship
                     : p->m_isBird   ? M::Ufo
                     : p->m_isBall   ? M::Ball
                     : p->m_isDart   ? M::Wave
                     : p->m_isRobot  ? M::Robot
                     : p->m_isSpider ? M::Spider
                     : p->m_isSwing  ? M::Swing
                                     : M::Cube;
            r.upsideDown = p->m_isUpsideDown;
            r.platformer = p->m_isPlatformer;
            r.sideways = p->m_isSideways;
            r.goingLeft = p->m_isGoingLeft;
            r.dartSlide = p->m_stateDartSlide > 0;
            r.hitHead = p->m_stateHitHead > 0;
            r.onSlope = p->m_isOnSlope;
            r.jumpBuffered = p->m_jumpBuffered;
            r.boosted = p->m_maybeIsBoosted;
            r.yVelocity = p->m_yVelocity;
            r.ridingSlopeId = p->m_collidingWithSlopeId;
            r.ridingSlopeTop = p->m_isCurrentSlopeTop;
            r.vehicleSize = p->m_vehicleSize;
            return r;
        }

        // Player 2's shapes from `from` on, dropped when they are player 1's
        // from `mine` on, region for region: the same input makes the same
        // numbers, so equal means equal.
        void dropSame(std::vector<hb::Shape> const& shapes, size_t mine, std::vector<hb::Shape>& shapes2,
                      size_t from) {
            if (shapes2.size() - from != shapes.size() - mine)
                return;
            for (size_t i = 0; i < shapes2.size() - from; ++i) {
                auto const& a = shapes[mine + i];
                auto const& b = shapes2[from + i];
                if (a.kind != b.kind || a.poly.size() != b.poly.size())
                    return;
                for (size_t k = 0; k < a.poly.size(); ++k) {
                    if (a.poly[k].x != b.poly[k].x || a.poly[k].y != b.poly[k].y)
                        return;
                }
            }
            shapes2.erase(shapes2.begin() + static_cast<std::ptrdiff_t>(from), shapes2.end());
        }

        // One object near the camera, as the collision pass sees it.
        struct ObjectRead {
            enum class Type { Hazard, Special, Block, Slope } type = Type::Hazard;
            hb::Collider collider;
            hb::Block block;
            hb::Slope slope;
        };

        void readCollider(GameObject* object, hb::Collider& c) {
            c.rect = toRect(object->getObjectRect());
            // Only an object GD already tests as rotated: getOrientedBox()
            // would flag any other one (0x1a153c) and change how it collides.
            c.oriented = object->m_shouldUseOuterOb;
            if (c.oriented) {
                if (auto* box = object->getOrientedBox()) {
                    for (int i = 0; i < 4; ++i)
                        c.box[i] = {box->m_corners[i].x, box->m_corners[i].y};
                } else {
                    c.oriented = false;
                }
            }
            // playerCircleCollision 0x211e5d-0x211e95: the radius as is at a
            // scale of exactly 1, else times the larger scale.
            float radius = object->m_objectRadius;
            if (radius > 0.f && !(object->m_scaleX == 1.f && object->m_scaleY == 1.f))
                radius *= std::max(object->m_scaleX, object->m_scaleY);
            c.radius = radius;
            if (radius > 0.f) {
                CCPoint const centre = object->getRealPosition();
                c.center = {centre.x, centre.y};
            }
        }

        bool readObject(GJBaseGameLayer* pl, TrajectoryManager& traj, PlayerObject* p1, PlayerObject* p2,
                        GameObject* object, ObjectRead& out) {
            // collisionCheckObjects 0x214a2e-0x214a94: what the pass skips.
            if (object->m_isGroupDisabled || object->m_isDisabled)
                return false;
            auto const type = object->m_objectType;
            if (type == GameObjectType::Decoration || type == GameObjectType::CollisionObject)
                return false;
            // Players aren't kept in the sections; a guard all the same, so
            // neither a real player nor one of Absense's copies is ever read
            // (or touched) as an object.
            if (object == pl->m_player1 || object == pl->m_player2 ||
                traj.isFakePlayer(static_cast<PlayerObject*>(object)) || object == pl->m_anticheatSpike)
                return false;

            // Leave the object's rect cache as GD had it (GD's debug draw and
            // Show Hitboxes do the same).
            bool const rectDirty = object->m_isObjectRectDirty;
            bool const offsetDone = object->m_boxOffsetCalculated;
            bool keep = true;
            switch (type) {
            case GameObjectType::Solid:
            case GameObjectType::Breakable:
                out.type = ObjectRead::Type::Block;
                out.block.rect = toRect(object->getObjectRect());
                out.block.passable = object->m_isPassable;
                out.block.breakable = type == GameObjectType::Breakable;
                break;
            case GameObjectType::Slope:
                out.type = ObjectRead::Type::Slope;
                out.slope.rect = toRect(object->getObjectRect());
                out.slope.direction = object->m_slopeDirection;
                out.slope.uphill = object->m_slopeUphill;
                out.slope.hazard = object->m_slopeIsHazard;
                out.slope.passable = object->m_isPassable;
                out.slope.uniqueId = object->m_uniqueID;
                break;
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard:
                out.type = ObjectRead::Type::Hazard;
                readCollider(object, out.collider);
                break;
            default:
                // Orbs, pads and portals every player has used are spent.
                if (object->hasBeenActivatedByPlayer(p1) && (!p2 || object->hasBeenActivatedByPlayer(p2))) {
                    keep = false;
                    break;
                }
                if (object == pl->m_player1CollisionBlock || object == pl->m_player2CollisionBlock) {
                    keep = false;
                    break;
                }
                // Triggers and enter effect triggers both go to
                // playerTouchedTrigger (0x215a55), which does nothing for one
                // that isn't touch triggered (+0x5d0, 0x217f93).
                if ((type == GameObjectType::Modifier || type == GameObjectType::EnterEffectObject) &&
                    std::find(kAlwaysShownModifiers.begin(), kAlwaysShownModifiers.end(), object->m_objectID) ==
                        kAlwaysShownModifiers.end() &&
                    !static_cast<EffectGameObject*>(object)->m_isTouchTriggered) {
                    keep = false;
                    break;
                }
                out.type = ObjectRead::Type::Special;
                readCollider(object, out.collider);
                break;
            }
            object->m_isObjectRectDirty = rectDirty;
            object->m_boxOffsetCalculated = offsetDone;
            return keep;
        }

        void regionsFor(ObjectRead const& o, hb::Player const& p, hb::Options const& opt,
                        HitborderOverlay::Settings const& set, std::vector<hb::Shape>& out) {
            switch (o.type) {
            case ObjectRead::Type::Hazard:
                if (set.hazards)
                    hb::colliderShapes(o.collider, p, opt, hb::Kind::Lethal, out);
                break;
            case ObjectRead::Type::Special:
                if (set.specials)
                    hb::colliderShapes(o.collider, p, opt, hb::Kind::Special, out);
                break;
            case ObjectRead::Type::Block:
                if (set.blocks)
                    hb::blockShapes(o.block, p, opt, out);
                break;
            case ObjectRead::Type::Slope:
                // A hazard slope goes with the hazards.
                if (o.slope.hazard ? set.hazards : set.slopes)
                    hb::slopeShapes(o.slope, p, opt, out);
                break;
            }
        }

        // The floor and the ceiling (checkCollisions 0x21395f-0x214162).
        void groundFor(GJBaseGameLayer* pl, PlayerObject* player, hb::Player const& p, CCRect const& view,
                       std::vector<hb::Shape>& out) {
            bool const corridor = !pl->m_gameState.m_isFreeMode &&
                                  (player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing ||
                                   player->m_isBall || player->m_isSpider || pl->m_gameState.m_isDualMode);
            float const g = player->m_unkAngle1 * player->m_vehicleSize * 0.5f;
            hb::Ground ground;
            ground.corridor = corridor;
            ground.floorY = corridor ? pl->getMinPortalY() + g : 90.f + g;
            ground.ceilingY = corridor ? pl->getMaxPortalY() - g : 0.f;
            ground.left = view.getMinX() - 60.f;
            ground.right = view.getMaxX() + 60.f;
            ground.reach = view.size.height + 120.f;
            hb::groundShapes(ground, p, out);
        }

        bool onScreen(std::vector<hb::Vec2> const& poly, hb::Rect const& cull) {
            double x0 = poly[0].x, x1 = poly[0].x, y0 = poly[0].y, y1 = poly[0].y;
            for (auto const& v : poly) {
                x0 = std::min(x0, v.x);
                x1 = std::max(x1, v.x);
                y0 = std::min(y0, v.y);
                y1 = std::max(y1, v.y);
            }
            return !(x1 < cull.x0 || x0 > cull.x1 || y1 < cull.y0 || y0 > cull.y1);
        }

        void drawPoly(CCDrawNode* node, std::vector<hb::Vec2> const& poly, ccColor4F fill, float width,
                      ccColor4F line, std::vector<CCPoint>& scratch) {
            if (poly.size() < 3)
                return;
            scratch.clear();
            for (auto const& v : poly)
                scratch.emplace_back(static_cast<float>(v.x), static_cast<float>(v.y));
            node->drawPolygon(scratch.data(), static_cast<unsigned>(scratch.size()), fill, width, line,
                              BorderAlignment::Outside);
        }

        void trimMarks(State& s, int ticks) {
            uint32_t const keep = static_cast<uint32_t>(std::max(ticks, 1));
            for (int i = 0; i < 2; ++i) {
                while (!s.path[i].empty() && s.tick - s.path[i].front().tick >= keep)
                    s.path[i].pop_front();
                while (!s.clicks[i].empty() && s.tick - s.clicks[i].front().tick >= keep)
                    s.clicks[i].pop_front();
            }
        }

        // ---------------------------------------------------------------- the audit's state

        struct AuditRun {
            HitborderOverlay::Audit audit;
            PlayerObject* inPass = nullptr;  // the real player whose collision pass is running
            hb::Player before;               // ... as the pass began
            bool killed = false;             // GD called destroyPlayer for it in the pass
            GameObject* killer = nullptr;
        };

        AuditRun& auditRun() {
            static AuditRun run;
            return run;
        }

        constexpr size_t kAuditNotes = 50;

        // The audit's and describe()'s regions: every kind, blocks Exact.
        HitborderOverlay::Settings everyKind(bool specials) {
            HitborderOverlay::Settings s = HitborderOverlay::defaults();
            s.hazards = s.blocks = s.slopes = s.exactBlocks = true;
            s.specials = specials;
            return s;
        }

        // An object's reach, generously: its rect, its circle.
        bool withinReach(ObjectRead const& read, hb::Vec2 c, double reach) {
            hb::Rect r;
            switch (read.type) {
            case ObjectRead::Type::Block: r = read.block.rect; break;
            case ObjectRead::Type::Slope: r = read.slope.rect; break;
            default:
                r = read.collider.rect;
                if (read.collider.radius > 0) {
                    r.x0 = std::min(r.x0, read.collider.center.x - read.collider.radius);
                    r.y0 = std::min(r.y0, read.collider.center.y - read.collider.radius);
                    r.x1 = std::max(r.x1, read.collider.center.x + read.collider.radius);
                    r.y1 = std::max(r.y1, read.collider.center.y + read.collider.radius);
                }
                break;
            }
            return c.x >= r.x0 - reach && c.x <= r.x1 + reach && c.y >= r.y0 - reach && c.y <= r.y1 + reach;
        }

        const char* modeWord(hb::Mode m) {
            switch (m) {
            case hb::Mode::Cube: return "cube";
            case hb::Mode::Ship: return "ship";
            case hb::Mode::Ball: return "ball";
            case hb::Mode::Ufo: return "ufo";
            case hb::Mode::Wave: return "wave";
            case hb::Mode::Robot: return "robot";
            case hb::Mode::Spider: return "spider";
            case hb::Mode::Swing: return "swing";
            }
            return "?";
        }

        std::string objectWords(GameObject* o) {
            if (!o)
                return "no object (the ground, the ceiling or the edge of the level)";
            return fmt::format("object {} (uid {}, type {})", o->m_objectID, o->m_uniqueID, static_cast<int>(o->m_objectType));
        }

        // Whether a tick or a button press of the real players is the
        // player's own: not a pass that only refreshes, not the judge's run,
        // not after a death or the level's end.
        bool liveTick(GJBaseGameLayer* pl) {
            auto* gb = GucciEngine::get();
            if (!pl || pl != PlayLayer::get() || gb->updater.m_onlyRefresh)
                return false;
            if (pl->m_resumeTimer > 0 || pl->m_playerDied || pl->m_levelEndAnimationStarted)
                return false;
            // A script the judge plays for real is put back afterwards; it is
            // not where the player went.
            return !absense::judge::active();
        }

    } // namespace

    // ---------------------------------------------------------------- settings

    HitborderOverlay* HitborderOverlay::get() {
        static HitborderOverlay inst;
        return &inst;
    }

    HitborderOverlay::HitborderOverlay() : settings(defaults()) {}

    HitborderOverlay::Settings HitborderOverlay::defaults() {
        Settings st;
        auto set = [&st](Part part, float r, float g, float b) {
            st.rgba[part][0] = r;
            st.rgba[part][1] = g;
            st.rgba[part][2] = b;
            st.rgba[part][3] = 1.f;
        };
        // Sampled from NaN GD's look (RESEARCH.md §1): red #FF0000, blue
        // #1414FF, green #00FF00, the path and dots yellow.
        set(Lethal, 1.f, 0.f, 0.f);
        set(Touch, 20.f / 255.f, 20.f / 255.f, 1.f);
        set(Special, 0.f, 1.f, 0.f);
        set(Marks, 1.f, 1.f, 0.f);
        return st;
    }

    const char* HitborderOverlay::partLabel(Part part) {
        switch (part) {
        case Lethal: return "Deadly";
        case Touch: return "Touching";
        case Special: return "Orbs, pads and portals";
        case Marks: return "Your dot, path and clicks";
        default: return "?";
        }
    }

    void HitborderOverlay::load() {
        auto* mod = Mod::get();
        Settings const def = defaults();
        auto flag = [&](const char* key, bool fallback) { return mod->getSavedValue<bool>(key, fallback); };
        settings.show = flag("hack_hitborders", def.show);
        settings.onDeath = flag("hack_hitborders_death", def.onDeath);
        settings.hazards = flag("hitborder_hazards", def.hazards);
        settings.blocks = flag("hitborder_blocks", def.blocks);
        settings.exactBlocks = flag("hitborder_blocks_exact", def.exactBlocks);
        settings.slopes = flag("hitborder_slopes", def.slopes);
        settings.specials = flag("hitborder_specials", def.specials);
        settings.ground = flag("hitborder_ground", def.ground);
        settings.useTilt = flag("hitborder_tilt", def.useTilt);
        settings.merged = flag("hitborder_merged", def.merged);
        settings.player2 = flag("hitborder_player2", def.player2);
        settings.point = flag("hitborder_point", def.point);
        settings.path = flag("hitborder_path", def.path);
        settings.clicks = flag("hitborder_clicks", def.clicks);
        settings.pathTicks = static_cast<int>(std::clamp<int64_t>(
            mod->getSavedValue<int64_t>("hitborder_path_len", def.pathTicks), kMinPathTicks, kMaxPathTicks));
        settings.lineWidth = static_cast<float>(
            std::clamp(mod->getSavedValue<double>("hitborder_line_width", def.lineWidth), 0.25, 6.0));
        settings.fill = static_cast<float>(std::clamp(mod->getSavedValue<double>("hitborder_fill", def.fill), 0.0, 1.0));
        for (int i = 0; i < PartCount; ++i) {
            auto const packed = mod->getSavedValue<int64_t>(std::string("hitborder_") + kPartKeys[i] + "_color",
                                                            packColour(def.rgba[i]));
            unpackColour(static_cast<uint32_t>(packed), settings.rgba[i]);
        }
    }

    void HitborderOverlay::save() const {
        auto* mod = Mod::get();
        mod->setSavedValue<bool>("hack_hitborders", settings.show);
        mod->setSavedValue<bool>("hack_hitborders_death", settings.onDeath);
        mod->setSavedValue<bool>("hitborder_hazards", settings.hazards);
        mod->setSavedValue<bool>("hitborder_blocks", settings.blocks);
        mod->setSavedValue<bool>("hitborder_blocks_exact", settings.exactBlocks);
        mod->setSavedValue<bool>("hitborder_slopes", settings.slopes);
        mod->setSavedValue<bool>("hitborder_specials", settings.specials);
        mod->setSavedValue<bool>("hitborder_ground", settings.ground);
        mod->setSavedValue<bool>("hitborder_tilt", settings.useTilt);
        mod->setSavedValue<bool>("hitborder_merged", settings.merged);
        mod->setSavedValue<bool>("hitborder_player2", settings.player2);
        mod->setSavedValue<bool>("hitborder_point", settings.point);
        mod->setSavedValue<bool>("hitborder_path", settings.path);
        mod->setSavedValue<bool>("hitborder_clicks", settings.clicks);
        mod->setSavedValue<int64_t>("hitborder_path_len", settings.pathTicks);
        mod->setSavedValue<double>("hitborder_line_width", settings.lineWidth);
        mod->setSavedValue<double>("hitborder_fill", settings.fill);
        for (int i = 0; i < PartCount; ++i)
            mod->setSavedValue<int64_t>(std::string("hitborder_") + kPartKeys[i] + "_color",
                                        static_cast<int64_t>(packColour(settings.rgba[i])));
    }

    void HitborderOverlay::toggleShown() {
        settings.show = !settings.show;
        Mod::get()->setSavedValue<bool>("hack_hitborders", settings.show);
    }

    // ---------------------------------------------------------------- life

    bool HitborderOverlay::attachedTo(GJBaseGameLayer* layer) const {
        auto const& s = st();
        return layer && s.borders && s.marks && s.borders->m_layer == layer && heldBy(layer, s.borders) &&
               heldBy(layer, s.marks);
    }

    void HitborderOverlay::init(PlayLayer* pl) {
        if (!pl || !pl->m_objectLayer || attachedTo(pl))
            return;
        destroy();

        auto* borders = BordersNode::create(pl);
        auto* marks = MarksNode::create(pl);
        if (!borders || !marks)
            return;  // both autoreleased; nothing else holds them
        auto drawNode = [](ccBlendFunc blend) -> CCDrawNode* {
            auto* node = CCDrawNode::create();
            if (!node)
                return nullptr;
            node->m_bUseArea = false;  // RobTop's culling; regions span the screen
            node->setBlendFunc(blend);
            node->retain();
            return node;
        };
        auto& s = st();
        s.merged = drawNode({GL_ONE, GL_ZERO});
        s.plain = drawNode({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s.second = drawNode({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s.marksDraw = drawNode({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        if (!s.merged || !s.plain || !s.second || !s.marksDraw) {
            destroy();
            return;
        }
        borders->setID("hitborders"_spr);
        marks->setID("hitborder-marks"_spr);
        borders->retain();
        marks->retain();
        s.borders = borders;
        s.marks = marks;
        s.builtFrame = ~0u;
        pl->m_objectLayer->addChild(borders, kBordersZ);
        pl->m_objectLayer->addChild(marks, kMarksZ);
    }

    void HitborderOverlay::draw(PlayLayer* pl) {
        if (!pl || attachedTo(pl) || !GucciEngine::get()->enabled)
            return;
        init(pl);
    }

    void HitborderOverlay::destroy() {
        auto& s = st();
        if (s.borders) {
            s.borders->removeFromParent();
            s.borders->release();
            s.borders = nullptr;
        }
        if (s.marks) {
            s.marks->removeFromParent();
            s.marks->release();
            s.marks = nullptr;
        }
        for (CCDrawNode** node : {&s.merged, &s.plain, &s.second, &s.marksDraw}) {
            if (*node) {
                (*node)->release();
                *node = nullptr;
            }
        }
        for (auto& f : s.fbos)
            f.release();
        s.fboFailed = false;
        s.haveRegions = false;
        s.builtFrame = ~0u;
        clearPath();
    }

    void HitborderOverlay::clearPath() {
        auto& s = st();
        for (int i = 0; i < 2; ++i) {
            s.path[i].clear();
            s.clicks[i].clear();
        }
    }

    // ---------------------------------------------------------------- tick

    void HitborderOverlay::sampleTick(GJBaseGameLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled || !(settings.show || settings.onDeath) || !(settings.path || settings.clicks))
            return;
        if (!liveTick(pl))
            return;
        auto& traj = ::Bot::get()->trajectory();
        auto& s = st();
        ++s.tick;
        auto add = [&](int i, PlayerObject* p) {
            if (!p || traj.isFakePlayer(p))
                return;
            auto& path = s.path[i];
            CCPoint const pos = p->getPosition();
            bool const jump = !path.empty() && ccpDistance(path.back().p, pos) > kPathBreak;
            path.push_back({pos, s.tick, jump});
        };
        add(0, pl->m_player1);
        if (pl->m_gameState.m_isDualMode)
            add(1, pl->m_player2);
        trimMarks(s, settings.pathTicks);
    }

    void HitborderOverlay::onButton(PlayerObject* player, bool pressed) {
        auto* gb = GucciEngine::get();
        if (!gb->enabled || !settings.clicks || !(settings.show || settings.onDeath) || !player)
            return;
        auto* pl = PlayLayer::get();
        if (!liveTick(pl))
            return;
        int const index = player == pl->m_player1 ? 0 : player == pl->m_player2 ? 1 : -1;
        if (index < 0 || ::Bot::get()->trajectory().isFakePlayer(player))
            return;
        auto& s = st();
        s.clicks[index].push_back({player->getPosition(), s.tick, pressed});
        trimMarks(s, settings.pathTicks);
    }

    // ---------------------------------------------------------------- frame

    bool HitborderOverlay::shownFor(GJBaseGameLayer* layer, PlayerObject* p1, PlayerObject* p2) const {
        if (settings.show)
            return true;
        return settings.onDeath && (layer->m_playerDied || p1->m_isDead || (p2 && p2->m_isDead));
    }

    void HitborderOverlay::paint(GJBaseGameLayer* pl) {
        auto& s = st();
        unsigned const frame = CCDirector::get()->getTotalFrames();
        if (s.builtFrame == frame)
            return;  // the renderer's capture visits the level a second time
        s.builtFrame = frame;
        s.haveRegions = false;
        if (!s.merged || !s.plain || !s.second || !s.marksDraw)
            return;
        s.merged->clear();
        s.plain->clear();
        s.second->clear();
        s.marksDraw->clear();

        auto* gb = GucciEngine::get();
        if (!pl || !gb->enabled || !pl->m_objectLayer)
            return;
        auto& traj = ::Bot::get()->trajectory();
        PlayerObject* p1 = pl->m_player1;
        PlayerObject* p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
        if (!p1 || traj.isFakePlayer(p1) || (p2 && traj.isFakePlayer(p2)))
            return;
        if (!shownFor(pl, p1, p2))
            return;

        // Screen points to level units: the object layer's scale on screen
        // (the camera's zoom, and any zoom mod's).
        auto const toWorld = pl->m_objectLayer->nodeToWorldTransform();
        float scale = std::sqrt(toWorld.a * toWorld.a + toWorld.b * toWorld.b);
        if (!(scale > 0.f))
            scale = 1.f;
        float const width = settings.lineWidth / scale;
        CCRect const view = objectSpaceViewport(pl->m_objectLayer);
        // Generous: the video renderer can capture a view of another size
        // than the window's, and a region only partly on screen still counts.
        float const pad = 2.f * width + 100.f;
        hb::Rect const cull{view.getMinX() - pad, view.getMinY() - pad, view.getMaxX() + pad, view.getMaxY() + pad};

        // ---- the regions
        hb::Player const first = readPlayer(p1);
        // Player 2's own borders, as outlines, wherever they differ from
        // player 1's. A mode or size of its own changes them all; in an
        // ordinary dual (player 2 upside down) only slopes, the ground and
        // turned objects differ, and the rest would draw player 1's regions
        // again, one outline each, over the merged ones.
        std::optional<hb::Player> second;
        if (p2 && settings.player2)
            second = readPlayer(p2);
        hb::Options opt;
        opt.fixRadius = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
        opt.useTilt = settings.useTilt;
        opt.core = settings.exactBlocks;

        std::vector<hb::Shape> shapes;
        std::vector<hb::Shape> shapes2;
        shapes.reserve(512);
        forEachNearbyObject(pl, [&](GameObject* object) {
            ObjectRead read;
            if (!readObject(pl, traj, p1, p2, object, read))
                return;
            size_t const mine = shapes.size();
            regionsFor(read, first, opt, settings, shapes);
            if (second) {
                size_t const from = shapes2.size();
                regionsFor(read, *second, opt, settings, shapes2);
                dropSame(shapes, mine, shapes2, from);
            }
        });
        if (settings.ground) {
            size_t const mine = shapes.size();
            groundFor(pl, p1, first, view, shapes);
            if (second) {
                size_t const from = shapes2.size();
                groundFor(pl, p2, *second, view, shapes2);
                dropSame(shapes, mine, shapes2, from);
            }
        }

        s.useMerged = settings.merged && !s.fboFailed;
        std::vector<CCPoint> scratch;
        ccColor4F const clear{0.f, 0.f, 0.f, 0.f};
        // Blue, then green, then red: red ends up on top.
        constexpr hb::Kind kOrder[] = {hb::Kind::Touch, hb::Kind::Special, hb::Kind::Lethal};
        for (hb::Kind const kind : kOrder) {
            int const part = partFor(kind);
            ccColor4F const line = lineColour(settings, part);
            ccColor4F const fill = fillColour(settings, part);
            if (s.useMerged) {
                // Grown by the line width (less the half unit cocos adds) in
                // the outline color, then the regions themselves (inset by
                // that half unit) in the fill color, blending off.
                for (auto const& shape : shapes) {
                    if (shape.kind != kind || !onScreen(shape.poly, cull))
                        continue;
                    auto const grown = hb::offsetConvex(shape.poly, width - 0.5);
                    drawPoly(s.merged, grown.empty() ? shape.poly : grown, line, 0.f, clear, scratch);
                }
                for (auto const& shape : shapes) {
                    if (shape.kind != kind || !onScreen(shape.poly, cull))
                        continue;
                    auto const inset = hb::offsetConvex(shape.poly, -0.5);
                    drawPoly(s.merged, inset.empty() ? shape.poly : inset, fill, 0.f, clear, scratch);
                }
            } else {
                for (auto const& shape : shapes) {
                    if (shape.kind == kind && onScreen(shape.poly, cull))
                        drawPoly(s.plain, shape.poly, fill, width, line, scratch);
                }
            }
            for (auto const& shape : shapes2) {
                if (shape.kind == kind && onScreen(shape.poly, cull))
                    drawPoly(s.second, shape.poly, clear, width * 0.75f, lighter(line), scratch);
            }
        }
        s.haveRegions = true;

        // ---- the player's marks
        ccColor4F const mark = lineColour(settings, Marks);
        if (settings.path) {
            float const radius = std::max(width * 0.5f, 0.35f / scale);
            float const minStep = 1.f / scale;  // closer than a point on screen: skipped
            for (int i = 0; i < 2; ++i) {
                auto const& path = s.path[i];
                bool have = false;
                CCPoint last;
                for (auto const& pt : path) {
                    if (pt.breakBefore)
                        have = false;
                    if (!have) {
                        last = pt.p;
                        have = true;
                        continue;
                    }
                    if (ccpDistance(last, pt.p) < minStep)
                        continue;
                    s.marksDraw->drawSegment(last, pt.p, radius, mark);
                    last = pt.p;
                }
            }
        }
        if (settings.clicks) {
            float const dot = 2.5f / scale;
            for (int i = 0; i < 2; ++i) {
                for (auto const& c : s.clicks[i]) {
                    if (c.pressed)
                        s.marksDraw->drawDot(c.p, dot, mark);
                    else
                        s.marksDraw->drawCircle(c.p, dot, clear, std::max(width * 0.6f, 0.75f / scale), mark, 16);
                }
            }
        }
        if (settings.point) {
            float const dot = 3.f / scale;
            s.marksDraw->drawDot(p1->getPosition(), dot, mark);
            if (p2)
                s.marksDraw->drawDot(p2->getPosition(), dot, mark);
        }
    }

    bool HitborderOverlay::drawMerged() {
        auto& s = st();
        if (!s.merged)
            return false;
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] <= 0 || vp[3] <= 0)
            return false;

        unsigned const frame = CCDirector::get()->getTotalFrames();
        Fbo* target = nullptr;
        for (auto& f : s.fbos) {
            if (f.fbo && f.w == vp[2] && f.h == vp[3])
                target = &f;
        }
        if (!target) {
            // The one used least lately makes way.
            target = &s.fbos[0];
            for (auto& f : s.fbos) {
                if (!f.fbo || f.lastFrame < target->lastFrame)
                    target = &f;
            }
            if (!target->make(vp[2], vp[3])) {
                s.fboFailed = true;
                log::warn("[Hitborders] no framebuffer at {}x{}: drawing each region's own outline instead", vp[2],
                          vp[3]);
                return false;
            }
        }
        target->lastFrame = frame;

        GLint oldFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
        GLboolean const scissor = glIsEnabled(GL_SCISSOR_TEST);
        GLfloat clearWas[4] = {0.f, 0.f, 0.f, 0.f};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearWas);

        // Into the texture, with the matrices as they are: the same pixels
        // the level's own drawing would land on.
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        glViewport(0, 0, vp[2], vp[3]);
        if (scissor)
            glDisable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        s.merged->draw();
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(oldFbo));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
        glClearColor(clearWas[0], clearWas[1], clearWas[2], clearWas[3]);

        // ... and the texture over the level, once, pixel for pixel.
        auto* program = CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTexture);
        if (!program)
            return true;
        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPushMatrix();
        kmGLLoadIdentity();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();
        kmGLLoadIdentity();
        program->use();
        program->setUniformsForBuiltins();
        ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ccGLBindTexture2D(target->tex);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);
        static GLfloat const kQuad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
        static GLfloat const kUv[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, kUv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPopMatrix();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPopMatrix();
        return true;
    }

    // ---------------------------------------------------------------- the audit

    HitborderOverlay::Audit const& HitborderOverlay::audit() const {
        return auditRun().audit;
    }

    void HitborderOverlay::setAudit(bool on) {
        auto& run = auditRun();
        if (on && !run.audit.on)
            run.audit = Audit{};
        run.audit.on = on;
        run.inPass = nullptr;
    }

    bool HitborderOverlay::auditBefore(GJBaseGameLayer* pl, PlayerObject* p) {
        auto& run = auditRun();
        if (!run.audit.on || !pl || !p || pl != PlayLayer::get() || p->m_isDead)
            return false;
        if (p != pl->m_player1 && p != pl->m_player2)
            return false;
        if (::Bot::get()->trajectory().isFakePlayer(p) || GucciEngine::get()->updater.m_onlyRefresh)
            return false;
        run.inPass = p;
        run.before = readPlayer(p);
        run.killed = false;
        run.killer = nullptr;
        return true;
    }

    void HitborderOverlay::auditDeath(GJBaseGameLayer* pl, PlayerObject* p, GameObject* by) {
        auto& run = auditRun();
        if (!run.audit.on || run.inPass != p || !pl || by == pl->m_anticheatSpike || run.killed)
            return;
        run.killed = true;
        run.killer = by;
    }

    void HitborderOverlay::auditAfter(GJBaseGameLayer* pl, PlayerObject* p) {
        auto& run = auditRun();
        if (run.inPass != p)
            return;
        run.inPass = nullptr;
        auto& a = run.audit;
        ++a.passes;

        auto& traj = ::Bot::get()->trajectory();
        PlayerObject* p1 = pl->m_player1;
        PlayerObject* p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
        // The state the pass began with, where the pass left the center: GD
        // tests blocks and slopes, snaps, then tests hazards from there; a
        // landing or a head snap moves the center out of a red core, a death
        // leaves it where it was.
        hb::Player probe = run.before;
        CCPoint const at = p->getPosition();
        probe.pos = {at.x, at.y};
        hb::Vec2 const c{at.x, at.y};
        hb::Options opt;
        opt.fixRadius = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
        opt.useTilt = true;
        opt.core = true;
        Settings const every = everyKind(false);

        GameObject* inside = nullptr;
        double depth = 0.0;
        double killerGap = -1.0;
        std::vector<hb::Shape> shapes;
        forEachNearbyObject(pl, [&](GameObject* object) {
            if (inside && (!run.killed || !run.killer || killerGap >= 0.0))
                return;
            ObjectRead read;
            if (!readObject(pl, traj, p1, p2, object, read) || !withinReach(read, c, 80.0))
                return;
            shapes.clear();
            regionsFor(read, probe, opt, every, shapes);
            for (auto const& s : shapes) {
                if (s.kind != hb::Kind::Lethal)
                    continue;
                bool const in = hb::contains(s.poly, c, 1e-6);
                if (in && !inside) {
                    inside = object;
                    depth = hb::distanceToEdge(s.poly, c);
                }
                if (object == run.killer) {
                    double const gap = in ? 0.0 : hb::distanceToEdge(s.poly, c);
                    killerGap = killerGap < 0.0 ? gap : std::min(killerGap, gap);
                }
            }
        });

        auto note = [&](std::string text) {
            a.notes.push_back(std::move(text));
            while (a.notes.size() > kAuditNotes)
                a.notes.pop_front();
        };
        std::string const who = fmt::format("frame {}, player {} ({}{}{})", GucciEngine::get()->updater.getFrame(),
                                            p == pl->m_player2 && p != pl->m_player1 ? 2 : 1, modeWord(probe.mode),
                                            probe.vehicleSize != 1.0 ? " mini" : "",
                                            probe.upsideDown ? " upside down" : "");
        if (run.killed) {
            ++a.deaths;
            if (inside) {
                ++a.agreed;
            } else {
                ++a.missed;
                note(fmt::format("{}: GD killed it at ({:.2f}, {:.2f}) by {}, but no red region holds that point{}", who,
                                 c.x, c.y, objectWords(run.killer),
                                 killerGap >= 0.0 ? fmt::format(" (that object's red edge is {:.2f} away)", killerGap)
                                                  : std::string()));
            }
        } else if (inside) {
            ++a.falseAlarms;
            note(fmt::format("{}: the center at ({:.2f}, {:.2f}) was {:.2f} inside the red of {}, and GD didn't kill it",
                             who, c.x, c.y, depth, objectWords(inside)));
        }
        run.killed = false;
        run.killer = nullptr;
    }

    matjson::Value HitborderOverlay::describe(float radius) {
        auto out = matjson::Value::object();
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 || !pl->m_objectLayer) {
            out["error"] = "not in a level";
            return out;
        }
        auto& traj = ::Bot::get()->trajectory();
        PlayerObject* p1 = pl->m_player1;
        PlayerObject* p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
        hb::Player const pp = readPlayer(p1);

        auto player = matjson::Value::object();
        player["x"] = pp.pos.x;
        player["y"] = pp.pos.y;
        player["outer_half"] = std::vector<double>{pp.hx, pp.hy};
        player["inner_half"] = std::vector<double>{pp.ix, pp.iy};
        player["rotation"] = pp.rotation;
        player["mode"] = std::string(modeWord(pp.mode));
        player["mini"] = pp.vehicleSize != 1.0;
        player["upside_down"] = pp.upsideDown;
        player["platformer"] = pp.platformer;
        player["sideways"] = pp.sideways;
        player["going_left"] = pp.goingLeft;
        player["d_block_slide"] = pp.dartSlide;
        player["h_block"] = pp.hitHead;
        player["on_slope"] = pp.onSlope;
        out["player"] = player;
        out["fix_radius_collision"] = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;

        hb::Options opt;
        opt.fixRadius = pl->m_levelSettings && pl->m_levelSettings->m_fixRadiusCollision;
        opt.useTilt = true;
        opt.core = true;
        Settings const every = everyKind(true);
        hb::Vec2 const c = pp.pos;
        auto regions = matjson::Value::array();
        size_t count = 0;
        std::vector<hb::Shape> shapes;
        forEachNearbyObject(pl, [&](GameObject* object) {
            ObjectRead read;
            if (!readObject(pl, traj, p1, p2, object, read) || !withinReach(read, c, radius))
                return;
            shapes.clear();
            regionsFor(read, pp, opt, every, shapes);
            for (auto const& s : shapes) {
                auto r = matjson::Value::object();
                r["kind"] = std::string(s.kind == hb::Kind::Lethal ? "deadly"
                                        : s.kind == hb::Kind::Touch ? "touch"
                                                                     : "special");
                r["object_id"] = object->m_objectID;
                r["uid"] = object->m_uniqueID;
                r["type"] = static_cast<int>(object->m_objectType);
                auto points = matjson::Value::array();
                for (auto const& v : s.poly)
                    points.push(std::vector<double>{std::round(v.x * 100.0) / 100.0, std::round(v.y * 100.0) / 100.0});
                r["points"] = points;
                r["holds_player"] = hb::contains(s.poly, c, 1e-6);
                regions.push(r);
                ++count;
            }
        });
        out["regions"] = regions;
        out["count"] = static_cast<int64_t>(count);
        auto const& a = auditRun().audit;
        auto audit = matjson::Value::object();
        audit["on"] = a.on;
        audit["passes"] = static_cast<int64_t>(a.passes);
        audit["deaths"] = static_cast<int64_t>(a.deaths);
        audit["agreed"] = static_cast<int64_t>(a.agreed);
        audit["missed"] = static_cast<int64_t>(a.missed);
        audit["false_alarms"] = static_cast<int64_t>(a.falseAlarms);
        out["audit"] = audit;
        return out;
    }

} // namespace gucci

// ---------------------------------------------------------------- hooks

// The path: one point per physics tick, after GD's commands for it (as Show
// Hitboxes' trail samples). A pass with no time in it is not a tick.
class $modify(GBHitborderTickHook, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (dt > 0.f)
            gucci::HitborderOverlay::get()->sampleTick(this);
    }
};

// The click marks: every jump press and release a real player takes, the
// player's own or the macro's.
class $modify(GBHitborderButtonHook, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool const handled = PlayerObject::pushButton(button);
        if (button == PlayerButton::Jump)
            gucci::HitborderOverlay::get()->onButton(this, true);
        return handled;
    }
    bool releaseButton(PlayerButton button) {
        bool const handled = PlayerObject::releaseButton(button);
        if (button == PlayerButton::Jump)
            gucci::HitborderOverlay::get()->onButton(this, false);
        return handled;
    }
};

// The accuracy audit (gucci_hitborders): one real collision pass at a time.
// A hazard hit with ignoreDamage set returns 1 instead of killing; it counts
// as a death all the same.
class $modify(GBHitborderAuditPassHook, GJBaseGameLayer) {
    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        auto* overlay = gucci::HitborderOverlay::get();
        bool const watched = overlay->auditBefore(this, player);
        int const result = GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
        if (watched) {
            if (ignoreDamage && result == 1)
                overlay->auditDeath(this, player, nullptr);
            overlay->auditAfter(this, player);
        }
        return result;
    }
};

class $modify(GBHitborderPlayLayerHook, PlayLayer) {
    // First in line, so the audit sees GD's decision even when noclip,
    // Calculate or a pathfinder swallows the death further down.
    static void onModify(auto& self) {
        (void)self.setHookPriorityPre("PlayLayer::destroyPlayer", Priority::First);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        gucci::HitborderOverlay::get()->auditDeath(this, player, object);
        PlayLayer::destroyPlayer(player, object);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (gucci::GucciEngine::get()->enabled)
            gucci::HitborderOverlay::get()->init(this);
        return true;
    }

    void onQuit() {
        gucci::HitborderOverlay::get()->destroy();
        PlayLayer::onQuit();
    }
};

$on_mod(Loaded) {
    gucci::HitborderOverlay::get()->load();
}
