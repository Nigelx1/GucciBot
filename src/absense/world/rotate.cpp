// Rotate triggers in the World (world/rotate.hpp): the transform chain and its
// checks. The fire is in world/fire.cpp, the step in world/step.cpp, what a
// rotate does to one object in world/objects.cpp, the writes into the level in
// world/materialize.cpp - all behind WorldState::rotate.
//
// FACT TABLE - GeometryDash.exe 2.2081 (Windows), read with capstone
// (GucciBot 2026-11-15, rotate lane; checked against ROTATE_PLAN.md section 3,
// and where the two disagree this is the binary's).
//
// (R1) EffectGameObject::triggerObject 0x4a66be, object 1346: angle =
//      (float)m_times360 (+0x61c) * 360.0f + m_rotationDegrees (+0x618), in
//      float. m_isDynamicMode (+0x610) with a duration (+0x5bc) that is not
//      0 and m_useMoveTarget (+0x5f4) or the direction mode (+0x605) is a
//      dynamic rotate (triggerDynamicRotateCommand 0x21f2a0, a list of its
//      own); either of those two flags without it works the angle out from
//      objects (0x21efb0, the aim and follow modes). Neither flag is the
//      default mode - a dynamic flag alone is still the default. Every mode
//      but the dynamic one then calls createRotateCommand.
// (R2) GJEffectManager::createRotateCommand 0x25c9b0(angle, duration, target,
//      centre, easing, rate, lock, p1, p2, uid, control): a new command at the
//      end of the command vector (the constructor's reset takes the game's
//      uid counter), then +0x2c centre, +0x158 uid, +0x18 (double)duration,
//      +0x15c control, +0x0c easing, +0x28 target, +0xd0 = 1, +0xb8 lock,
//      +0xbc = 0, +0x10 (double)rate, +0xb0 = (double)angle; an angle of 0
//      sets +0x70 and +0x72 (finished, finishRelated), any other +0x198 =
//      (double)angle and +0x190 = 3. p1 and p2 are not read, and unlike a
//      move there is no check of the target.
// (R3) prepareMoveActions 0x25f3f0 clears three registries at its top: EM
//      +0x678 (target -> commands), +0x660 (a list) and +0x7f0 ((target,
//      centre) -> commands). Case 1 (0x25f7b7), in command order with the
//      moves: when EM +0x818 is set, +0x678[target] is emptied; +0x1a8 = 0;
//      pushed onto +0x678[target]; registerRotationCommand 0x25f170 (with
//      +0x818, +0x7f0[(target, centre)] emptied first when it exists; then
//      pushed there and onto +0x660). Without m_finishRelated: step(dt);
//      +0x110 = +0xa0; +0xa0 = +0xa0 + +0x98; +0x118 = +0xa8; and, not
//      intermediate, +0xa8 = +0xa0. With it: +0x110 = +0xa0, +0x118 = +0xa8,
//      +0xa8 = +0xa0, no step. Then the tail every command takes (step.cpp
//      (j)), its thirds cleared. EM +0x818 = (LevelSettings +0x1c8 == 0),
//      set at level setup (0x23b2db); the World reads it from the live EM.
// (R4) processMoveActionsStep 0x22b5d0 runs processRotationActions 0x22b7f0
//      after the transform and dynamic actions and before processMoveActions.
//      processRotationActions parks the object layer (+0xfe8): its scaleX,
//      scaleY and position are saved, set to 1 and (0,0), and put back at the
//      end - its rotation is not touched. Every command in the +0x678 lists
//      gets +0x204 = the command index (+0x3e0); then, for each command of
//      +0x660 in order, one whose +0x204 is not the index, or whose +0x1a8 is
//      set, is passed.
// (R5) The centre: m_parentGroupsDict (+0xf60)[centre], else getGroup(centre)
//      when it holds exactly one object, else none (a centre <= 0 is none).
//      The objects: when m_staticGroups[target] (+0xf30) is not empty, every
//      entry of m_groups[target] (+0xf18) - not the static array, as the plan
//      had it - with delta (float)(+0xa0 - +0x110); otherwise
//      m_optimizedGroups[target] (+0xf48) with (float)(+0xa8 - +0x118) (arrays
//      that are null are made there: an allocation a run never copies). A
//      delta of 0 (not a NaN) without m_finishRelated passes the command (its
//      +0x1a8 stays clear). Otherwise +0x1a8 = 1, the object rotation is 0
//      with lock object rotation (+0xb8) and the delta without, and
//      claimRotationAction 0x22d9a0 adds, for every command of +0x7f0[(target,
//      centre)] whose +0x1a8 is clear - whatever its stamp - its own delta to
//      the rotation and, unless it locks, to the object rotation (d + sum, in
//      float), sets its +0x1a8, and erases the map entry.
// (R6) With a centre: c = ((float)(posX - (double)+0x2a0), (float)(posY -
//      (double)+0x2a4)) of the centre object; claimMoveAction 0x22dbe0 takes
//      the target group's move node (+0x6d0) when it has one with +0xd0 and
//      +0xd1 clear: +0xd0 = 1 (processMoveActions passes it: neither its
//      static nor its optimized groups move) and returns ((float)+0x38,
//      (float)+0x40), zeroing +0x160/+0x168 of its commands (already zero on
//      a last step); on the optimized path ((float)+0x90, (float)+0x98) and
//      nothing zeroed. The claim is made before the array is looked at.
//      m_areaTransformNode (+0x8c8): position c, scale 1, skew 0, rotation the
//      delta; m_areaScaleNode (+0x8d8) scale 1; m_areaSkewNode (+0x8d0) skew
//      0; T = +0x8c8's nodeToWorldTransform (vtable +0x320, checked against
//      libcocos2d's CCNode vtable). Per object (m_movedCount += count first):
//      +0x513 = finishRelated; when +0x512 is clear and the marker is not the
//      index, m_lastPosition = (float)pos, marker = index, +0x368 = 0x101;
//      +0x350 = +0x352 = 1; p = ((float)((posX - (double)+0x2a0) - (double)
//      c.x), the same in y); t = __CCPointApplyAffineTransform(p, T); posX =
//      (((double)(claim.x + t.x) - posX) + (double)+0x2a0) + posX, the same in
//      y; when the object rotation r is not 0 (a NaN is) and +0x36c
//      (m_canRotateFree): +0x2b0 = r + +0x2b0, +0x2a8 = r + +0x2a8,
//      addRotation(r), and for a type other than 7 with +0x2e8 clear: +0x2e8 =
//      1, updateOrientedBox (vtable +0x578), getObjectRect (+0x490). Then the
//      layer's updateObjectSection.
// (R7) Without a centre: nothing when r is 0 (not a NaN); otherwise per object
//      with +0x36c: +0x2b0, +0x2a8 and addRotation as above, and for a type
//      other than 7: +0x368 = 0x101 and, with +0x2e8 clear, +0x2e8 = 1 and the
//      two calls. No position, no last position, no +0x513, no section.
// (R8) addRotation 0x197cd0: rotationX == rotationY -> setRotation(getRotation()
//      + r); otherwise setRotationX(rotationX + r), setRotationY(rotationY + r).
//      GameObject::setRotation 0x197e00: nothing when both are already the
//      value; else +0x368 = 0x101, +0x350 = 1, CCSprite::setRotation, the same
//      on the followers (+0x258, when +0x268), the glow sprite (+0x2f0), the
//      colour sprite (+0x380, unless +0x36a) and the particle (+0x308, unless
//      +0x430), and +0x390 = (value is 90, -90, 270 or -270). setRotationX
//      0x197f50 the same on x, +0x390 from the new x; setRotationY 0x1980a0
//      on y, without +0x390.
// (R9) The oriented box: vtable +0x578 is updateOrientedBox 0x1a1570 (makes
//      the OBB2D at +0x2e0 when there is none, else recomputes it while +0x369
//      is set); its angle is vtable +0x580, m_startRotationX (+0x3d4) +
//      m_rotationXOffset (+0x2a8). +0x490 is getObjectRect through 0x197850.

#include "absense/world/rotate.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <typeinfo>
#include <vector>

#include "absense/compat/devlog.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

namespace world {

bool g_rotate = false;
bool g_captureRotates = false;

namespace rotate {

namespace {

using cocos2d::CCAffineTransform;
using cocos2d::CCNode;
using cocos2d::CCPoint;

// The live chain from m_areaTransformNode up to the root, and private twins
// of every node of it, each the next one's child (setParent only: no node of
// the game holds them, and the live nodes are never handed one).
struct Chain {
    GJBaseGameLayer* pl = nullptr;
    std::vector<CCNode*> live;
    std::vector<CCNode*> mine;
};
Chain g_chain;
CCNode* g_lone = nullptr;
bool g_loneOn = false;

bool g_selfTested = false;
const char* g_selfTestFailure = nullptr;
bool g_levelChecked = false;
bool g_levelOk = false;
GJBaseGameLayer* g_levelFor = nullptr;
uint64_t g_restoreMisses = 0;
uint64_t g_ops = 0;
uint64_t g_writes = 0;
uint64_t g_restoreChecks = 0;
uint64_t g_runChecks = 0;
uint64_t g_runFails = 0;
// What the twins hold, hashed when they were last set up (chainKey).
uint64_t g_chainKey = 0;
// GucciBot (2026-11-16, rotate lane review, ROTATE_PLAN.md risk 1): the
// parents above m_areaTransformNode (the layer and its scene), hashed at the
// level's first run start. The private chain is frozen for a run while the
// game rebuilds T from these every tick, so if they ever differ at a later
// run start, they can move during a run too: rotates go off for the level.
uint64_t g_parentsKey = 0;
GJBaseGameLayer* g_parentsFor = nullptr;
uint64_t g_parentsChanges = 0;

uint64_t parentsKey() {
    uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 1; i < g_chain.live.size(); i++) {
        CCNode* n = g_chain.live[i];
        const float f[] = {n->getPosition().x, n->getPosition().y, n->getAnchorPointInPoints().x,
                           n->getAnchorPointInPoints().y, n->getRotationX(), n->getRotationY(),
                           n->getScaleX(), n->getScaleY(), n->getSkewX(), n->getSkewY()};
        const auto* b = reinterpret_cast<const unsigned char*>(f);
        for (std::size_t k = 0; k < sizeof(f); k++) {
            h ^= b[k];
            h *= 0x100000001b3ull;
        }
        const unsigned char ignore = n->isIgnoreAnchorPointForPosition() ? 1 : 0;
        h ^= ignore;
        h *= 0x100000001b3ull;
    }
    return h;
}

CCNode* makeNode() {
    CCNode* n = CCNode::create();
    n->retain();
    return n;
}

void releaseChain() {
    for (CCNode* n : g_chain.mine) {
        n->setParent(nullptr);
        n->release();
    }
    g_chain = Chain{};
}

// The live chain walked again; the twins are made again only when it is not
// the one they were made for.
bool buildChain(GJBaseGameLayer* pl) {
    if (!pl || !pl->m_areaTransformNode) return false;
    std::vector<CCNode*> live;
    for (CCNode* n = pl->m_areaTransformNode; n; n = n->getParent()) {
        live.push_back(n);
        if (live.size() > 64) return false;
    }
    if (g_chain.pl == pl && g_chain.live == live && g_chain.mine.size() == live.size()) return true;
    releaseChain();
    g_chain.pl = pl;
    g_chain.live = live;
    for (std::size_t i = 0; i < live.size(); i++) g_chain.mine.push_back(makeNode());
    for (std::size_t i = 0; i + 1 < g_chain.mine.size(); i++) g_chain.mine[i]->setParent(g_chain.mine[i + 1]);
    return true;
}

// Everything CCNode::nodeToParentTransform reads, from the live node.
void copyNode(CCNode* to, CCNode* from) {
    to->setContentSize(from->getContentSize());
    to->setAnchorPoint(from->getAnchorPoint());
    to->ignoreAnchorPointForPosition(from->isIgnoreAnchorPointForPosition());
    to->setPosition(from->getPosition());
    to->setRotationX(from->getRotationX());
    to->setRotationY(from->getRotationY());
    to->setScaleX(from->getScaleX());
    to->setScaleY(from->getScaleY());
    to->setSkewX(from->getSkewX());
    to->setSkewY(from->getSkewY());
}

// The twins as the live chain stands while processRotationActions works T
// out: the object layer parked, the scale and skew nodes reset (R4, R6).
void refresh(GJBaseGameLayer* pl) {
    for (std::size_t i = 0; i < g_chain.live.size(); i++) {
        CCNode* from = g_chain.live[i];
        CCNode* to = g_chain.mine[i];
        copyNode(to, from);
        if (from == pl->m_areaScaleNode) {
            to->setScaleX(1.0f);
            to->setScaleY(1.0f);
        }
        if (from == pl->m_areaSkewNode) {
            to->setSkewX(0.0f);
            to->setSkewY(0.0f);
        }
        if (from == pl->m_objectLayer) {
            to->setScale(1.0f);
            to->setPosition(cocos2d::CCPointZero);
        }
    }
    // FNV-1a over every field a twin was given, bit for bit.
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 0x100000001b3ull;
        }
    };
    for (CCNode* n : g_chain.mine) {
        const float f[] = {n->getPosition().x, n->getPosition().y, n->getAnchorPointInPoints().x,
                           n->getAnchorPointInPoints().y, n->getRotationX(), n->getRotationY(),
                           n->getScaleX(), n->getScaleY(), n->getSkewX(), n->getSkewY()};
        mix(f, sizeof(f));
        const bool ignore = n->isIgnoreAnchorPointForPosition();
        mix(&ignore, 1);
    }
    g_chainKey = h | 1;  // never 0: 0 is the lone node's
}

CCAffineTransform onNode(CCNode* n, float angle, CCPoint c) {
    n->setPosition(c);
    n->setScaleX(1.0f);
    n->setScaleY(1.0f);
    n->setSkewX(0.0f);
    n->setSkewY(0.0f);
    n->setRotation(angle);
    return n->nodeToWorldTransform();
}

// T the way processRotationActions works it out (0x22b821-0x22bcb4), on the
// live nodes, with everything it changes put back: the object layer the way
// the game puts it back (its own getters and setters), and the three area
// nodes field for field.
CCAffineTransform liveTransform(GJBaseGameLayer* pl, float angle, CCPoint c) {
    CCNode* layer = pl->m_objectLayer;
    CCNode* n = pl->m_areaTransformNode;
    CCNode* scale = pl->m_areaScaleNode;
    CCNode* skew = pl->m_areaSkewNode;
    const float layerSx = layer->getScaleX();
    const float layerSy = layer->getScaleY();
    const CCPoint layerPos = layer->getPosition();
    layer->setScale(1.0f);
    layer->setPosition(cocos2d::CCPointZero);
    const CCPoint nPos = n->getPosition();
    const float nSx = n->getScaleX(), nSy = n->getScaleY();
    const float nKx = n->getSkewX(), nKy = n->getSkewY();
    const float nRx = n->getRotationX(), nRy = n->getRotationY();
    const float sSx = scale->getScaleX(), sSy = scale->getScaleY();
    const float kKx = skew->getSkewX(), kKy = skew->getSkewY();
    n->setPosition(c);
    n->setScaleX(1.0f);
    n->setScaleY(1.0f);
    scale->setScaleX(1.0f);
    scale->setScaleY(1.0f);
    n->setSkewX(0.0f);
    n->setSkewY(0.0f);
    skew->setSkewX(0.0f);
    skew->setSkewY(0.0f);
    n->setRotation(angle);
    const CCAffineTransform t = n->nodeToWorldTransform();
    n->setPosition(nPos);
    n->setScaleX(nSx);
    n->setScaleY(nSy);
    n->setSkewX(nKx);
    n->setSkewY(nKy);
    n->setRotationX(nRx);
    n->setRotationY(nRy);
    scale->setScaleX(sSx);
    scale->setScaleY(sSy);
    skew->setSkewX(kKx);
    skew->setSkewY(kKy);
    layer->setScaleX(layerSx);
    layer->setScaleY(layerSy);
    layer->setPosition(layerPos);
    return t;
}

bool sameT(const CCAffineTransform& a, const CCAffineTransform& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

bool haveNodes(GJBaseGameLayer* pl) {
    return pl && pl->m_areaTransformNode && pl->m_areaScaleNode && pl->m_areaSkewNode && pl->m_objectLayer;
}

void logChain() {
    for (std::size_t i = 0; i < g_chain.live.size(); i++) {
        CCNode* n = g_chain.live[i];
        const CCPoint p = n->getPosition();
        const CCPoint a = n->getAnchorPointInPoints();
        devlog::logf(devlog::Cat::System,
                     "world: rotate chain %zu: \"%s\" pos %.3f,%.3f scale %.4f,%.4f rot %.3f,%.3f skew %.3f,%.3f anchor "
                     "%.2f,%.2f%s",
                     i, n->getID().c_str(), p.x, p.y, n->getScaleX(), n->getScaleY(), n->getRotationX(),
                     n->getRotationY(), n->getSkewX(), n->getSkewY(), a.x, a.y,
                     n->isIgnoreAnchorPointForPosition() ? " (anchor ignored)" : "");
    }
}

}  // namespace

void checkLevel(GJBaseGameLayer* pl) {
    g_levelChecked = true;
    g_levelFor = pl;
    g_levelOk = false;
    if (!g_selfTested) {
        g_selfTested = true;
        g_selfTestFailure = rotateSelfTest();
        if (g_selfTestFailure) {
            geode::log::error("World: rotate self-test FAILED: {}; rotate is off for this launch", g_selfTestFailure);
        } else {
            geode::log::info("[AbsPF] World: rotate self-test passed");
        }
        devlog::logf(devlog::Cat::System, "world: rotate self-test %s%s", g_selfTestFailure ? "FAILED: " : "passed",
                     g_selfTestFailure ? g_selfTestFailure : "");
    }
    if (g_selfTestFailure) return;
    if (!haveNodes(pl) || !buildChain(pl)) {
        geode::log::warn("World: rotate is off for this level: the layer's transform nodes are missing");
        devlog::logf(devlog::Cat::System, "world: rotate T: the layer's transform nodes are missing; rotate off");
        return;
    }
    // The node's rotation is written and put back raw (world/materialize.cpp):
    // CCNode's m_fRotationX / m_fRotationY have to be where offsets.hpp says.
    {
        CCNode* n = pl->m_areaTransformNode;
        if (!(off::at<float>(n, off::kNodeRotX) == n->getRotationX() &&
              off::at<float>(n, off::kNodeRotY) == n->getRotationY())) {
            geode::log::warn("World: rotate is off for this level: CCNode's rotation is not at +0x38/+0x3c");
            devlog::logf(devlog::Cat::System, "world: rotate T: CCNode's rotation is not at +0x38/+0x3c; rotate off");
            return;
        }
    }
    refresh(pl);
    logChain();
    // Angles and centres of every kind a level gives: zeros of both signs,
    // the quarter turns, whole turns, the exam levels' own (Slaughterhouse's
    // 50, -51 and -77), tiny and huge ones, and odd ones from a fixed
    // sequence; centres near the origin, far out and below it.
    const float fixedAngles[] = {0.0f, -0.0f, 1.0f, -1.0f, 45.0f, 90.0f, -90.0f, 180.0f, 270.0f, -270.0f,
                                 360.0f, 720.3f, 1e-4f, -77.0f, 50.0f, -51.0f, 12.345f, 3599.5f, -0.5f, 0.1f};
    uint64_t s = 0x2545F4914F6CDD1Dull;
    auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 16);
    };
    constexpr int kChecks = 50;
    int same = 0;
    std::string firstMiss;
    for (int i = 0; i < kChecks; i++) {
        const float angle = i < (int)(sizeof(fixedAngles) / sizeof(fixedAngles[0]))
                                ? fixedAngles[i]
                                : ((float)(next() % 200001) - 100000.0f) / 137.0f;
        const CCPoint c = i % 5 == 0 ? CCPoint{0.0f, 0.0f}
                                     : CCPoint{((float)(next() % 400001) - 20000.0f) / 3.0f,
                                               ((float)(next() % 40001) - 10000.0f) / 7.0f};
        const CCAffineTransform a = liveTransform(pl, angle, c);
        const CCAffineTransform b = onNode(g_chain.mine[0], angle, c);
        if (sameT(a, b)) {
            same++;
        } else if (firstMiss.empty()) {
            firstMiss = fmt::format("angle {} centre {},{}: live {} {} {} {} {} {}, private {} {} {} {} {} {}", angle,
                                    c.x, c.y, a.a, a.b, a.c, a.d, a.tx, a.ty, b.a, b.b, b.c, b.d, b.tx, b.ty);
        }
    }
    g_levelOk = same == kChecks;
    geode::log::info("[AbsPF] World: rotate T: {}/{} bit-identical ({} nodes in the chain){}", same, kChecks,
                     g_chain.live.size(), g_levelOk ? "" : "; rotate is off for this level");
    devlog::logf(devlog::Cat::System, "world: rotate T: %d/%d bit-identical (%zu nodes in the chain)%s%s", same,
                 kChecks, g_chain.live.size(), g_levelOk ? "" : "; rotate off for this level, first miss: ",
                 firstMiss.c_str());
}

bool levelOk() { return g_levelChecked && g_levelOk; }

bool wantedForRun(GJBaseGameLayer* pl) {
    if (!g_rotate || World::disabled || !haveNodes(pl)) return false;
    if (!g_levelChecked || g_levelFor != pl) checkLevel(pl);
    if (!g_levelOk) return false;
    if (!buildChain(pl)) return false;
    {
        const uint64_t k = parentsKey();
        if (g_parentsFor != pl) {
            g_parentsFor = pl;
            g_parentsKey = k;
        } else if (k != g_parentsKey) {
            g_parentsChanges++;
            g_levelOk = false;
            geode::log::warn("World: the rotate chain's parents (layer, scene) changed between run starts; rotate "
                             "is off for this level");
            devlog::logf(devlog::Cat::System,
                         "world: rotate chain parents changed between run starts; rotate off for this level");
            logChain();
            return false;
        }
    }
    refresh(pl);
    // ROTATE_PLAN.md risk 1: the chain's parents are only partly reset by the
    // game, and one of them can change during the level. One angle compared
    // at every run start; the run goes without rotates when it differs.
    const float angle = 33.3f;
    const CCPoint c{517.25f, -213.5f};
    const CCAffineTransform a = liveTransform(pl, angle, c);
    const CCAffineTransform b = onNode(g_chain.mine[0], angle, c);
    g_runChecks++;
    if ((g_runChecks & (g_runChecks - 1)) == 0 || g_runChecks % 4096 == 0) {
        geode::log::info("[AbsPF] World: rotate so far: {} run starts ({} with another T), {} rotate ops, {} objects "
                         "written, {} restores checked, {} restore misses",
                         g_runChecks - 1, g_runFails, g_ops, g_writes, g_restoreChecks, g_restoreMisses);
    }
    if (!sameT(a, b)) {
        g_runFails++;
        if (g_runFails <= 8 || (g_runFails & (g_runFails - 1)) == 0) {
            geode::log::warn("World: rotate T differs at a run start ({} of {} runs); that run goes without rotates",
                             g_runFails, g_runChecks);
            devlog::logf(devlog::Cat::System,
                         "world: rotate T differs at a run start (%llu of %llu): live %g %g %g %g %g %g, private %g %g "
                         "%g %g %g %g; that run goes without rotates",
                         (unsigned long long)g_runFails, (unsigned long long)g_runChecks, a.a, a.b, a.c, a.d, a.tx,
                         a.ty, b.a, b.b, b.c, b.d, b.tx, b.ty);
            logChain();
        }
        return false;
    }
    return true;
}

CCAffineTransform transform(float angle, CCPoint c) {
    if (g_loneOn || g_chain.mine.empty()) {
        if (!g_lone) g_lone = makeNode();
        return onNode(g_lone, angle, c);
    }
    return onNode(g_chain.mine[0], angle, c);
}

void setLoneChain(bool on) { g_loneOn = on; }

uint64_t chainKey() { return (g_loneOn || g_chain.mine.empty()) ? 0 : g_chainKey; }

void forget() {
    releaseChain();
    g_chainKey = 0;
    g_levelChecked = false;
    g_levelOk = false;
    g_levelFor = nullptr;
    g_parentsFor = nullptr;
}

uint64_t restoreMisses() { return g_restoreMisses; }
void noteRestoreMiss() { g_restoreMisses++; }
void noteOp() { g_ops++; }
void noteWrite() { g_writes++; }
void noteRestoreChecked() { g_restoreChecks++; }

}  // namespace rotate
}  // namespace world
