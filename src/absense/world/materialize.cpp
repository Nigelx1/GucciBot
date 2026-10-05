// The materializer and SectionShadow (trigger design steps 8 and 9, see
// world/materialize.hpp). The game facts it is written against:
//
// (s1) checkCollisions 0x2137f0 takes the player's section from its node
//      position (vtable+0xc8), x <= 0 -> 0, x >= 1e7 -> factor * 1e7, else
//      (int)(x * factor) in float (0x2141ac-0x214257); it walks columns
//      max(sx - 1, 0) .. min(sx + 1, columns - 1) and, in each non-null
//      column, rows max(sy - 1, 0) .. min(sy + 1, rows - 1) (0x21425c-
//      0x214325). A null bucket is passed; otherwise the count is read from
//      +0x3658[x][y] and, when bit y of the words of +0x3688[x] is set, the
//      first count entries are sorted by m_uniqueID (0x2143a6, comparator
//      0x205210), each entry's +0x274 rewritten to its index and the bit
//      cleared (0x2143c0-0x2143fb), before collisionCheckObjects 0x214960.
// (s2) removeObjectFromSection 0x226ef8-0x226f94, for an object in the
//      collision buckets: count - 1; bucket[+0x274] = bucket[old count - 1];
//      that entry's +0x274 = the slot; bit +0x27c of +0x3688[+0x278] set. The
//      old last entry stays in the buffer past the count.
// (s3) addToSection 0x226abe-0x226d42: the three outer vectors grown to sx + 1
//      (0x249580 twice, the flags by hand), a null column given a 0x18-byte
//      bucket vector, a 0x18-byte count vector and a 0x20-byte vector<bool>
//      (operator new 0x4d0770); a column grown to sy + 1 rows; a null bucket
//      given a 0x18-byte vector; then bucket[count] = object when count <
//      size, push_back otherwise; bit sy set (bts 0x226d23); +0x274 = count;
//      count + 1.
// (s4) The caches a copy's collision pass fills in on an object:
//      getObjectRect 0x197850 (+0x358, clears +0x368), getUnmodifiedPosition
//      0x1978f0 (+0x340, clears +0x350 / +0x351), getOrientedBox 0x1a1510
//      (sets +0x2e8, makes the box at +0x2e0 with OBB2D::create and retains
//      it, or recomputes it while +0x369 is set, clearing it). A written
//      object gets all of them back; a box made during the run is released.
// (s5) GameObject::setPosition 0x197b60 positions the node and its child
//      sprites (+0x258 array, +0x308, +0x2f0, +0x380) and writes none of the
//      caches above; getRealPosition 0x197b20 is the double position as
//      floats. A written object's node is put back through setPosition, which
//      moves the child sprites back with it; the transform-dirty flags it
//      leaves set only make the renderer recompute the same transform.

#include "absense/world/materialize.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#endif
#include <cstdint>

#include "absense/compat/devlog.hpp"
#ifndef GEODE_IS_WINDOWS
#include <cstdlib>  // std::abort (GucciBot multiplatform, see gameNew)
#endif
#include "absense/trajectory/trajectory.hpp"  // moverCacheGeneration
#include "absense/world/offsets.hpp"
#include "absense/world/world.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

namespace world {

#ifdef GEODE_IS_WINDOWS // Windows layouts only (core/platform.hpp)
static_assert(sizeof(RawVec) == sizeof(gd::vector<int>));
// addToSection 0x226bb4 allocates 0x20 bytes for a column's flags.
static_assert(sizeof(RawBits) == 0x20 && sizeof(std::vector<bool>) == 0x20);
static_assert(offsetof(OBB2D, m_center) + sizeof(cocos2d::CCPoint) - offsetof(OBB2D, m_corners) == 0x78);
#endif

namespace {

constexpr std::size_t kBoxFirst = offsetof(OBB2D, m_corners);
constexpr std::size_t kBoxBytes = 0x78;
// addToSection files a position at 1e7 or more at factor * 1e7: 100000 with
// the level's 0.01. Nothing a run writes is filed past that.
constexpr int kMaxSection = 100001;

// ------------------------------------------------------------ the game's heap

#ifdef GEODE_IS_WINDOWS
void* gameNew(std::size_t bytes) {
    using Fn = void* (*)(std::size_t);
    return reinterpret_cast<Fn>(geode::base::get() + off::kGameNew)(bytes);
}

void gameDelete(void* p, std::size_t bytes) {
    if (!p) return;
    using Fn = void (*)(void*, std::size_t);
    reinterpret_cast<Fn>(geode::base::get() + off::kGameDelete)(p, bytes);
}
#else
// GucciBot multiplatform: the two image offsets above are the Windows build's.
// Off Windows the World never turns on (World::init), so no materializer is
// ever begun and nothing reaches these; if something ever did, stopping here
// with a log line beats calling into the middle of another platform's binary.
void* gameNew(std::size_t) {
    geode::log::error("World: the materializer ran off Windows, where the World is off; stopping");
    std::abort();
}

void gameDelete(void* p, std::size_t) {
    if (!p) return;
    geode::log::error("World: the materializer ran off Windows, where the World is off; stopping");
    std::abort();
}
#endif

char* allocZeroed(std::size_t bytes) {
    auto* p = static_cast<char*>(gameNew(bytes));
    std::memset(p, 0, bytes);
    return p;
}

// The vector's storage copied into a buffer of the materializer's with room
// for `extra` more bytes; the header points at the copy.
void copyStorage(RawVec& v, std::size_t extra, std::size_t& own) {
    const std::size_t used = (std::size_t)(v.last - v.first);
    const std::size_t cap = used + extra;
    char* buf = allocZeroed(cap);
    if (used) std::memcpy(buf, v.first, used);
    v.first = buf;
    v.last = buf + used;
    v.end = buf + cap;
    own = cap;
}

// A vector of the materializer's grown to `count` elements, the new ones zero.
// The buffer it leaves behind is not freed here but kept until the shadow is
// put back: the copies' collision pass holds the bucket it is walking over a
// call (collisionCheckObjects 0x214960), and an orb or portal inside that call
// can send the run's objects through here (catchUpCurrentRun, a teleport), so
// freeing it under the pass would leave it reading memory that is gone.
void growStorage(RawVec& v, std::size_t elemBytes, std::size_t count, std::size_t& own, Retired& retired) {
    const std::size_t need = count * elemBytes;
    const std::size_t used = (std::size_t)(v.last - v.first);
    if (used >= need) return;
    const std::size_t cap = (std::size_t)(v.end - v.first);
    if (own == 0 || cap < need) {
        const std::size_t newCap = std::max(need, cap * 2);
        char* buf = allocZeroed(newCap);
        if (used) std::memcpy(buf, v.first, used);
        if (own) retired.push_back({v.first, own});
        v.first = buf;
        v.end = buf + newCap;
        own = newCap;
    }
    v.last = v.first + need;
}

std::size_t countOf(const RawVec& v, std::size_t elemBytes) {
    return v.first ? (std::size_t)(v.last - v.first) / elemBytes : 0;
}

template <class T>
T& elem(const RawVec& v, std::size_t i) {
    return *reinterpret_cast<T*>(v.first + i * sizeof(T));
}

bool bitSet(const RawBits& b, std::size_t i) {
    if (i >= b.bits || (i >> 5) >= countOf(b.words, 4)) return false;
    return (elem<uint32_t>(b.words, i >> 5) >> (i & 31)) & 1u;
}

void setBit(RawBits& b, std::size_t i) { elem<uint32_t>(b.words, i >> 5) |= uint32_t{1} << (i & 31); }

// cvttsd2si / cvttss2si: INT_MIN for a NaN or out of range, where a C++
// cast is undefined.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
int truncDouble(double v) { return _mm_cvttsd_si32(_mm_set_sd(v)); }
int truncFloat(float v) { return _mm_cvtt_ss2si(_mm_set_ss(v)); }
#else
// Off x86: the same answer cvttsd2si gives, INT_MIN for NaN or out of range.
int truncDouble(double v) {
    return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : static_cast<int>(v);
}
// Off x86: the same answer cvttss2si gives, INT_MIN for NaN or out of range.
int truncFloat(float v) {
    return (v != v || v >= 2147483648.0f || v < -2147483648.0f) ? INT32_MIN : static_cast<int>(v);
}
#endif

bool sameBits(const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

// The fields a materializer writes, the same bit for bit.
bool sameWritable(const Pose& a, const Pose& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.lastX, b.lastX) && sameBits(a.lastY, b.lastY) &&
           a.marker == b.marker && a.counter == b.counter && a.disabled == b.disabled;
}

// ... and the object's own.
bool sameLive(const Pose& p, GameObject* o) {
    return sameBits(p.x, o->m_positionX) && sameBits(p.y, o->m_positionY) && sameBits(p.lastX, o->m_lastPosition.x) &&
           sameBits(p.lastY, o->m_lastPosition.y) && p.marker == off::moveMarker(o) &&
           p.counter == o->m_enabledGroupsCounter && p.disabled == o->m_isGroupDisabled;
}

// One step of a move: the object was moved on the step that stood at
// `commandIndex` (every move function stamps the step's command index into
// +0x4dc and takes the last position on its first move of the index,
// world/step.cpp (f)), by no more than MovingObjects takes for a move rather
// than a teleport.
bool moverStep(const Pose& p, uint32_t commandIndex, float& dx, float& dy) {
    if (p.skipLast || p.disabled || p.marker != commandIndex) return false;
    dx = (float)p.x - p.lastX;
    dy = (float)p.y - p.lastY;
    if (!(std::abs(dx) >= 0.0005f || std::abs(dy) >= 0.0005f)) return false;
    return std::abs(dx) <= 40.0f && std::abs(dy) <= 40.0f;
}

std::shared_ptr<const MaterialDef> g_materialDef;
std::shared_ptr<const BaseTable> g_lastTable;
// Real state changes so far (noteRealStateChanged). Only the game's thread
// reads or moves it.
uint64_t g_realEpoch = 1;

// The level the WorldDef was read from is still the one set up: the objects
// its slots name have not been let go of.
bool defCurrent(const WorldDef* def) { return def && def->gen == moverCacheGeneration(); }

}  // namespace

void noteRealStateChanged() {
    g_realEpoch++;
    g_lastTable.reset();
}

// ------------------------------------------------------------ MaterialDef

std::span<const uint16_t> MaterialDef::coGroupsOf(int group) const {
    const std::size_t g = WorldDef::clampGroup(group);
    if (g + 1 >= coOffsets.size()) return {};
    return std::span<const uint16_t>(coGroups.data() + coOffsets[g], coOffsets[g + 1] - coOffsets[g]);
}

void forgetMaterialDef() {
    g_materialDef.reset();
    g_lastTable.reset();
}

std::shared_ptr<const MaterialDef> materialDef(const std::shared_ptr<const WorldDef>& def) {
    if (!def) return nullptr;
    if (g_materialDef && g_materialDef->def == def) return g_materialDef;
    auto md = std::make_shared<MaterialDef>();
    md->def = def;
    const std::size_t n = def->slots.size();
    md->indexOf.assign(n, -1);
    for (std::size_t s = 0; s < n; s++) {
        GameObject* o = def->slots[s];
        const uint8_t f = def->flags[s];
        if (!o || !(f & kCollidable) || !(f & kReferenced) || def->groupsOf((int)s).empty()) continue;
        md->indexOf[s] = (int)md->slots.size();
        md->slots.push_back((int)s);
        // The bound WorldDef::groupBaseBox grows a member by.
        const float w = o->m_width * std::abs(o->m_scaleX);
        const float h = o->m_height * std::abs(o->m_scaleY);
        md->radius.push_back(std::max({0.5f * std::sqrt(w * w + h * h), o->m_objectRadius, 15.0f}));
    }
    md->coOffsets.assign((std::size_t)kGroupLimit + 1, 0);
    md->mult.assign((std::size_t)kGroupLimit, 1);
    md->hasExtended.assign((std::size_t)kGroupLimit, 0);
    std::vector<uint16_t> scratch;
    auto longestRun = [](std::span<const int> list) {
        int best = list.empty() ? 0 : 1;
        int run = 1;
        for (std::size_t i = 1; i < list.size(); i++) {
            run = list[i] == list[i - 1] ? run + 1 : 1;
            best = std::max(best, run);
        }
        return best;
    };
    for (int g = 0; g < kGroupLimit; g++) {
        md->coOffsets[(std::size_t)g] = (uint32_t)md->coGroups.size();
        scratch.clear();
        for (auto list : {def->members(g), def->staticMembers(g)}) {
            for (int s : list) {
                if (s < 0 || (std::size_t)s >= n || md->indexOf[(std::size_t)s] < 0) continue;
                if (def->flags[(std::size_t)s] & kExtended) md->hasExtended[(std::size_t)g] = 1;
                for (uint16_t h : def->groupsOf(s)) scratch.push_back(h);
            }
        }
        if (!scratch.empty()) {
            std::sort(scratch.begin(), scratch.end());
            scratch.erase(std::unique(scratch.begin(), scratch.end()), scratch.end());
            md->coGroups.insert(md->coGroups.end(), scratch.begin(), scratch.end());
        }
        // An object listed twice in an array is moved twice by one op
        // (moveObjects walks every entry), so its reach counts that often.
        const int m = std::max({longestRun(def->members(g)), longestRun(def->staticMembers(g)),
                                longestRun(def->optimizedMembers(g)), 1});
        md->mult[(std::size_t)g] = (uint8_t)std::min(m, 255);
    }
    md->coOffsets[(std::size_t)kGroupLimit] = (uint32_t)md->coGroups.size();
    g_materialDef = md;
    return md;
}

// ------------------------------------------------------------ BaseTable

const Pose* BaseTable::find(int slot) const {
    if (md && slot >= 0 && (std::size_t)slot < md->indexOf.size()) {
        const int i = md->indexOf[(std::size_t)slot];
        if (i >= 0) return &poses[(std::size_t)i];
    }
    if (!extraSlots.empty()) {
        auto it = std::lower_bound(extraSlots.begin(), extraSlots.end(), slot);
        if (it != extraSlots.end() && *it == slot) return &extraPoses[(std::size_t)(it - extraSlots.begin())];
    }
    return nullptr;
}

const Box& BaseTable::groupBox(int group) const {
    auto it = m_boxes.find(group);
    if (it != m_boxes.end()) return it->second;
    Box box;
    if (md && md->def) {
        for (auto list : {md->def->members(group), md->def->staticMembers(group)}) {
            for (int s : list) {
                const int i = (s >= 0 && (std::size_t)s < md->indexOf.size()) ? md->indexOf[(std::size_t)s] : -1;
                if (i < 0) continue;
                const Pose& p = poses[(std::size_t)i];
                const float r = md->radius[(std::size_t)i];
                box.add((float)p.x - r, (float)p.y - r, (float)p.x + r, (float)p.y + r);
            }
        }
    }
    return m_boxes.emplace(group, box).first->second;
}

const std::vector<int>& BaseTable::liveDiff(uint32_t liveProgress, uint32_t liveCommandIndex) const {
    if (m_diffValid && m_diffProgress == liveProgress && m_diffIndex == liveCommandIndex && m_diffEpoch == g_realEpoch) {
        return m_diff;
    }
    m_diff.clear();
    // The table's own tick with nothing real in between: the objects stand
    // where it read them (runs put back everything they write before the game
    // reads a field again). A level set up again since holds other objects,
    // and none of these slots may be read.
    const bool untouched = liveProgress == progress && liveCommandIndex == commandIndex && g_realEpoch == epoch;
    if (!untouched && md && defCurrent(md->def.get())) {
        const WorldDef& def = *md->def;
        for (std::size_t i = 0; i < md->slots.size(); i++) {
            GameObject* o = def.slots[(std::size_t)md->slots[i]];
            if (o && !sameLive(poses[i], o)) m_diff.push_back(md->slots[i]);
        }
        for (std::size_t i = 0; i < extraSlots.size(); i++) {
            GameObject* o = def.slots[(std::size_t)extraSlots[i]];
            if (o && !sameLive(extraPoses[i], o)) m_diff.push_back(extraSlots[i]);
        }
    }
    m_diffValid = true;
    m_diffProgress = liveProgress;
    m_diffIndex = liveCommandIndex;
    m_diffEpoch = g_realEpoch;
    return m_diff;
}

std::shared_ptr<const BaseTable> baseTable(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& def,
                                           uint32_t liveProgress, uint32_t liveCommandIndex) {
    if (!pl || !def) return nullptr;
    std::shared_ptr<const MaterialDef> md = materialDef(def);
    if (!md) return nullptr;
    if (g_lastTable && g_lastTable->md == md && g_lastTable->pl == pl && g_lastTable->progress == liveProgress &&
        g_lastTable->commandIndex == liveCommandIndex && g_lastTable->epoch == g_realEpoch) {
        return g_lastTable;
    }
    auto t = std::make_shared<BaseTable>();
    t->md = md;
    t->pl = pl;
    t->progress = liveProgress;
    t->commandIndex = liveCommandIndex;
    t->epoch = g_realEpoch;
    t->poses.resize(md->slots.size());
    for (std::size_t i = 0; i < md->slots.size(); i++) {
        t->poses[i] = ObjectCache::readLive(def->slots[(std::size_t)md->slots[i]]);
    }
    auto stepOf = [liveCommandIndex](const Pose& p, float& dx, float& dy) {
        return moverStep(p, liveCommandIndex, dx, dy);
    };
    for (std::size_t i = 0; i < md->slots.size(); i++) {
        float dx = 0.0f, dy = 0.0f;
        if (stepOf(t->poses[i], dx, dy)) t->movers.push_back(Carry{md->slots[i], dx, dy});
    }
    // An area effect or a dynamic move reaches objects in no group a trigger
    // names: every collidable object is looked at then, as MovingObjects does.
    const GJGameState& gs = pl->m_gameState;
    if (!gs.m_moveEffectInstances.empty() || !gs.m_dynamicMoveActions.empty()) {
        for (std::size_t s = 0; s < def->slots.size(); s++) {
            GameObject* o = def->slots[s];
            if (!o || md->indexOf[s] >= 0 || !(def->flags[s] & kCollidable)) continue;
            const Pose p = ObjectCache::readLive(o);
            float dx = 0.0f, dy = 0.0f;
            if (!stepOf(p, dx, dy)) continue;
            t->extraSlots.push_back((int)s);
            t->extraPoses.push_back(p);
            t->movers.push_back(Carry{(int)s, dx, dy});
        }
    }
    g_lastTable = t;
    return t;
}

std::shared_ptr<const BaseTable> rewoundTable(const BaseTable& now, uint32_t progress, uint32_t commandIndex,
                                              std::span<const std::pair<int, Pose>> undo) {
    if (!now.md) return nullptr;
    auto t = std::make_shared<BaseTable>();
    t->md = now.md;
    t->pl = now.pl;
    t->progress = progress;
    t->commandIndex = commandIndex;
    // Not any real epoch: a run from this table always looks at the level
    // again and writes back what the table holds, because the game has moved
    // on since the tick it describes.
    t->epoch = 0;
    t->poses = now.poses;
    t->extraSlots = now.extraSlots;
    t->extraPoses = now.extraPoses;
    const MaterialDef& md = *now.md;
    for (const auto& [slot, pose] : undo) {
        if (slot < 0 || (std::size_t)slot >= md.indexOf.size()) continue;
        const int i = md.indexOf[(std::size_t)slot];
        if (i >= 0) {
            t->poses[(std::size_t)i] = pose;
            continue;
        }
        // An object outside the table that something moved: it is carried only
        // when it is collidable, which is what put the extras in the table.
        auto it = std::lower_bound(t->extraSlots.begin(), t->extraSlots.end(), slot);
        if (it != t->extraSlots.end() && *it == slot) {
            t->extraPoses[(std::size_t)(it - t->extraSlots.begin())] = pose;
        }
    }
    // The movers of that tick, by the same rule baseTable uses, against the
    // command index the game stood at then.
    for (std::size_t i = 0; i < t->poses.size(); i++) {
        float dx = 0.0f, dy = 0.0f;
        if (moverStep(t->poses[i], commandIndex, dx, dy)) t->movers.push_back(Carry{md.slots[i], dx, dy});
    }
    for (std::size_t i = 0; i < t->extraSlots.size(); i++) {
        float dx = 0.0f, dy = 0.0f;
        if (moverStep(t->extraPoses[i], commandIndex, dx, dy)) t->movers.push_back(Carry{t->extraSlots[i], dx, dy});
    }
    return t;
}

std::shared_ptr<const std::vector<Carry>> carriedMovers(const BaseTable& table, const WorldState& ws,
                                                        const WorldDef& def) {
    auto out = std::make_shared<std::vector<Carry>>();
    if (table.movers.empty()) return out;
    // The groups the state's move commands move, and on which axes the step
    // follows them: all of them run in the World (world/step.cpp), a camera
    // lock's axis with no input.
    std::vector<std::pair<uint16_t, uint8_t>> followed;
    for (const WCmd& c : *ws.cmds) {
        if (c.commandType != off::kCmdMove) continue;
        const uint8_t axes = (uint8_t)((c.lockCameraX ? 0 : 1) | (c.lockCameraY ? 0 : 2));
        followed.push_back({(uint16_t)WorldDef::clampGroup(c.targetGroup), axes});
    }
    for (const Carry& m : table.movers) {
        uint8_t axes = 0;
        for (uint16_t g : def.groupsOf(m.slot)) {
            for (const auto& [group, bits] : followed) {
                if (group == g) axes |= bits;
            }
        }
        Carry c = m;
        if (axes & 1) c.dx = 0.0f;
        if (axes & 2) c.dy = 0.0f;
        if (c.dx != 0.0f || c.dy != 0.0f) out->push_back(c);
    }
    return out;
}

// ------------------------------------------------------------ SectionShadow

SectionGrid SectionGrid::of(GJBaseGameLayer* pl) {
    SectionGrid g;
    g.buckets = &off::at<RawVec>(pl, off::kCollBuckets);
    g.counts = &off::at<RawVec>(pl, off::kCollCounts);
    g.dirty = &off::at<RawVec>(pl, off::kCollDirty);
    g.factorX = off::at<float>(pl, off::kSectionW);
    g.factorY = off::at<float>(pl, off::kSectionH);
    return g;
}

SectionShadow::~SectionShadow() {
    // The Materializer puts everything back before it lets go; a shadow left
    // here belongs to a layer that may be gone, and leaking its copies is
    // safer than writing into one.
}

void SectionShadow::attach(const SectionGrid& grid) {
    if (!empty()) restore();
    m_grid = grid;
}

bool SectionShadow::sectionOf(double x, double y, int& sx, int& sy) const {
    // addToSection 0x226519-0x2265a8 (fact (b) of world/step.cpp): comisd 0,
    // pos / jb, so a NaN is multiplied out like a positive position.
    auto one = [](double pos, float factor) {
        if (pos <= 0.0) return 0;
        const double v = pos >= 10000000.0 ? (double)(factor * 10000000.0f) : (double)factor * pos;
        return truncDouble(v);
    };
    sx = one(x, m_grid.factorX);
    sy = one(y, m_grid.factorY);
    return sx >= 0 && sy >= 0 && sx <= kMaxSection && sy <= kMaxSection;
}

void SectionShadow::copySection(float x, float y, int& sx, int& sy) const {
    // checkCollisions 0x2141ac-0x214257 (s1).
    auto one = [](float pos, float factor) {
        if (pos <= 0.0f) return 0;
        const float v = pos >= 10000000.0f ? factor * 10000000.0f : pos * factor;
        return std::clamp(truncFloat(v), 0, kMaxSection);
    };
    sx = one(x, m_grid.factorX);
    sy = one(y, m_grid.factorY);
}

RawVec& SectionShadow::outer(int k) const { return k == 0 ? *m_grid.buckets : (k == 1 ? *m_grid.counts : *m_grid.dirty); }

std::size_t SectionShadow::outerSize(int k) const { return countOf(outer(k), sizeof(void*)); }

bool SectionShadow::touchOuter() {
    if (m_outer.saved) return true;
    for (int k = 0; k < 3; k++) {
        RawVec& v = outer(k);
        m_outer.orig[k] = v;
        copyStorage(v, 64 * sizeof(void*), m_outer.ownBytes[k]);
    }
    m_outer.saved = true;
    return true;
}

bool SectionShadow::growOuter(int x) {
    for (int k = 0; k < 3; k++) {
        if (outerSize(k) > (std::size_t)x) continue;
        touchOuter();
        growStorage(outer(k), sizeof(void*), (std::size_t)x + 1, m_outer.ownBytes[k], m_retired);
    }
    return true;
}

SectionShadow::ColumnImage* SectionShadow::touchColumn(int x) {
    if (auto it = m_columnOf.find(x); it != m_columnOf.end()) return &m_columns[it->second];
    if (x < 0 || x > kMaxSection) return nullptr;
    growOuter(x);
    auto*& col = elem<RawVec*>(outer(0), (std::size_t)x);
    auto*& cnt = elem<RawVec*>(outer(1), (std::size_t)x);
    auto*& bits = elem<RawBits*>(outer(2), (std::size_t)x);
    ColumnImage c;
    c.x = x;
    if (!col && !cnt && !bits) {
        // A column nobody was ever filed in (s3): three headers of the
        // materializer's, in outer vectors that are its copies.
        touchOuter();
        auto*& col2 = elem<RawVec*>(outer(0), (std::size_t)x);
        auto*& cnt2 = elem<RawVec*>(outer(1), (std::size_t)x);
        auto*& bits2 = elem<RawBits*>(outer(2), (std::size_t)x);
        c.created = true;
        c.col = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
        c.cnt = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
        c.bits = reinterpret_cast<RawBits*>(allocZeroed(sizeof(RawBits)));
        col2 = c.col;
        cnt2 = c.cnt;
        bits2 = c.bits;
    } else if (col && cnt && bits) {
        c.col = col;
        c.cnt = cnt;
        c.bits = bits;
        c.origCol = *col;
        c.origCnt = *cnt;
        c.origBits = *bits;
        copyStorage(*c.col, 16 * sizeof(void*), c.ownCol);
        copyStorage(*c.cnt, 16 * sizeof(int), c.ownCnt);
        copyStorage(c.bits->words, 4 * sizeof(uint32_t), c.ownWords);
    } else {
        // The game makes the three together; a column that is not is left alone.
        return nullptr;
    }
    m_columnOf[x] = (uint32_t)m_columns.size();
    m_columns.push_back(c);
    return &m_columns.back();
}

bool SectionShadow::growColumn(ColumnImage& c, int y) {
    const std::size_t rows = (std::size_t)y + 1;
    growStorage(*c.col, sizeof(void*), rows, c.ownCol, m_retired);
    growStorage(*c.cnt, sizeof(int), rows, c.ownCnt, m_retired);
    if (c.bits->bits < rows) {
        const std::size_t was = c.bits->bits;
        growStorage(c.bits->words, sizeof(uint32_t), (rows + 31) / 32, c.ownWords, m_retired);
        // The rows past the old size start clear, whatever the last word held.
        for (std::size_t i = was; i < rows; i++) elem<uint32_t>(c.bits->words, i >> 5) &= ~(uint32_t{1} << (i & 31));
        c.bits->bits = rows;
    }
    return true;
}

SectionShadow::BucketImage* SectionShadow::touchBucket(int x, int y) {
    const uint64_t key = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
    if (auto it = m_bucketOf.find(key); it != m_bucketOf.end()) return &m_buckets[it->second];
    if (y < 0 || y > kMaxSection) return nullptr;
    ColumnImage* c = touchColumn(x);
    if (!c) return nullptr;
    growColumn(*c, y);
    auto*& header = elem<RawVec*>(*c->col, (std::size_t)y);
    BucketImage b;
    b.x = x;
    b.y = y;
    if (!header) {
        b.created = true;
        b.header = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
        header = b.header;
        b.origCount = 0;
    } else {
        b.header = header;
        b.orig = *header;
        b.origCount = elem<int>(*c->cnt, (std::size_t)y);
        copyStorage(*b.header, 16 * sizeof(void*), b.own);
    }
    m_bucketOf[key] = (uint32_t)m_buckets.size();
    m_buckets.push_back(b);
    return &m_buckets.back();
}

bool SectionShadow::move(GameObject* o, int sx, int sy) {
    if (!attached() || !o || sx < 0 || sy < 0 || sx > kMaxSection || sy > kMaxSection) return false;
    const int x = off::at<int>(o, off::kSecX);
    const int y = off::at<int>(o, off::kSecY);
    const int i = off::at<int>(o, off::kSlot);
    if (x == sx && y == sy) return true;
    // Where the object is filed now, read before anything is copied.
    if (x < 0 || y < 0 || i < 0) return false;
    for (int k = 0; k < 3; k++) {
        if (outerSize(k) <= (std::size_t)x) return false;
    }
    const RawVec* col = elem<RawVec*>(outer(0), (std::size_t)x);
    const RawVec* cnt = elem<RawVec*>(outer(1), (std::size_t)x);
    const RawBits* bits = elem<RawBits*>(outer(2), (std::size_t)x);
    if (!col || !cnt || !bits || countOf(*col, 8) <= (std::size_t)y || countOf(*cnt, 4) <= (std::size_t)y ||
        bits->bits <= (std::size_t)y) {
        return false;
    }
    const RawVec* bucket = elem<RawVec*>(*col, (std::size_t)y);
    if (!bucket) return false;
    const int n = elem<int>(*cnt, (std::size_t)y);
    if (i >= n || (std::size_t)n > countOf(*bucket, 8) || elem<GameObject*>(*bucket, (std::size_t)i) != o) return false;

    // Both buckets copied (the destination made when it is new) before either
    // changes; a destination the game's own rules could not add to is left
    // with nothing changed (the copies are put back with the rest).
    if (!touchBucket(sx, sy)) return false;
    if (!touchBucket(x, y)) return false;
    BucketImage& to = m_buckets[m_bucketOf[((uint64_t)(uint32_t)sx << 32) | (uint32_t)sy]];
    ColumnImage& toCol = m_columns[m_columnOf[sx]];
    const int toCount = elem<int>(*toCol.cnt, (std::size_t)sy);
    if (toCount < 0 || (std::size_t)toCount > countOf(*to.header, 8)) return false;

    // (s2): the last entry into the slot.
    {
        BucketImage& from = m_buckets[m_bucketOf[((uint64_t)(uint32_t)x << 32) | (uint32_t)y]];
        ColumnImage& fromCol = m_columns[m_columnOf[x]];
        int& count = elem<int>(*fromCol.cnt, (std::size_t)y);
        const int lastIndex = count - 1;
        GameObject*& entry = elem<GameObject*>(*from.header, (std::size_t)i);
        entry = elem<GameObject*>(*from.header, (std::size_t)lastIndex);
        // (The game writes the slot without looking; a bucket of the real level
        // holds no null within its count, and a copy of one holds what it held.)
        if (entry) off::at<int>(entry, off::kSlot) = i;
        count = lastIndex;
        setBit(*fromCol.bits, (std::size_t)y);
    }
    // (s3): into bucket[count], or pushed.
    {
        BucketImage& into = m_buckets[m_bucketOf[((uint64_t)(uint32_t)sx << 32) | (uint32_t)sy]];
        ColumnImage& intoCol = m_columns[m_columnOf[sx]];
        int& count = elem<int>(*intoCol.cnt, (std::size_t)sy);
        RawVec& h = *into.header;
        if ((std::size_t)count < countOf(h, 8)) {
            elem<GameObject*>(h, (std::size_t)count) = o;
        } else {
            if (h.last == h.end) {
                const std::size_t used = (std::size_t)(h.last - h.first);
                const std::size_t newCap = std::max<std::size_t>(used * 2, used + 16 * sizeof(void*));
                char* buf = allocZeroed(newCap);
                if (used) std::memcpy(buf, h.first, used);
                // Kept until restore, not freed here: the pass walking this
                // bucket may be the one that sent the object here (growStorage).
                if (into.own) m_retired.push_back({h.first, into.own});
                h.first = buf;
                h.last = buf + used;
                h.end = buf + newCap;
                into.own = newCap;
            }
            *reinterpret_cast<GameObject**>(h.last) = o;
            h.last += sizeof(void*);
        }
        setBit(*intoCol.bits, (std::size_t)sy);
        off::at<int>(o, off::kSlot) = count;
        count++;
    }
    off::at<int>(o, off::kSecX) = sx;
    off::at<int>(o, off::kSecY) = sy;
    return true;
}

void SectionShadow::shadowDirty(int x0, int x1, int y0, int y1) {
    if (!attached()) return;
    const std::size_t columns = std::min({outerSize(0), outerSize(1), outerSize(2)});
    if (columns == 0) return;
    const int xs = std::max(x0, 0);
    const int xe = std::min(x1, (int)std::min<std::size_t>(columns - 1, (std::size_t)kMaxSection));
    for (int x = xs; x <= xe; x++) {
        const RawVec* col = elem<RawVec*>(outer(0), (std::size_t)x);
        const RawVec* cnt = elem<RawVec*>(outer(1), (std::size_t)x);
        const RawBits* bits = elem<RawBits*>(outer(2), (std::size_t)x);
        if (!col || !cnt || !bits) continue;
        const std::size_t rows = countOf(*col, 8);
        if (rows == 0) continue;
        const int ye = std::min(y1, (int)std::min<std::size_t>(rows - 1, (std::size_t)kMaxSection));
        for (int y = std::max(y0, 0); y <= ye; y++) {
            if (!bitSet(*bits, (std::size_t)y) || !elem<RawVec*>(*col, (std::size_t)y)) continue;
            const uint64_t key = ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
            if (m_bucketOf.contains(key)) continue;
            touchBucket(x, y);
        }
    }
}

void SectionShadow::restore() {
    // Buckets, then columns, then the outer vectors: each level's copies hold
    // the pointers to the level below that is put back first.
    for (auto it = m_buckets.rbegin(); it != m_buckets.rend(); ++it) {
        BucketImage& b = *it;
        if (b.created) {
            if (b.own) gameDelete(b.header->first, b.own);
            gameDelete(b.header, sizeof(RawVec));
            continue;
        }
        if (b.own) gameDelete(b.header->first, b.own);
        *b.header = b.orig;
        // The game's entries at their own slots again: a move swapped some
        // (s2) and the copies' collision pass sorted others (s1), and every
        // entry the game counts sits at the slot it records.
        for (int i = 0; i < b.origCount && (std::size_t)i < countOf(b.orig, 8); i++) {
            if (GameObject* e = elem<GameObject*>(b.orig, (std::size_t)i)) off::at<int>(e, off::kSlot) = i;
        }
    }
    for (auto it = m_columns.rbegin(); it != m_columns.rend(); ++it) {
        ColumnImage& c = *it;
        if (c.ownCol) gameDelete(c.col->first, c.ownCol);
        if (c.ownCnt) gameDelete(c.cnt->first, c.ownCnt);
        if (c.ownWords) gameDelete(c.bits->words.first, c.ownWords);
        if (c.created) {
            gameDelete(c.col, sizeof(RawVec));
            gameDelete(c.cnt, sizeof(RawVec));
            gameDelete(c.bits, sizeof(RawBits));
            continue;
        }
        *c.col = c.origCol;
        *c.cnt = c.origCnt;
        *c.bits = c.origBits;
    }
    if (m_outer.saved) {
        for (int k = 0; k < 3; k++) {
            if (m_outer.ownBytes[k]) gameDelete(outer(k).first, m_outer.ownBytes[k]);
            outer(k) = m_outer.orig[k];
        }
    }
    // The storage a grow replaced, which nothing points at now.
    for (const auto& [buf, bytes] : m_retired) gameDelete(buf, bytes);
    m_retired.clear();
    m_outer = OuterImage{};
    m_columns.clear();
    m_buckets.clear();
    m_columnOf.clear();
    m_bucketOf.clear();
}

void SectionShadow::forget() {
    // Nothing is written and nothing is freed: the vectors the images point at
    // belong to a layer that may be gone, and the copies are the game's own
    // heap, which goes with it.
    m_retired.clear();
    m_outer = OuterImage{};
    m_columns.clear();
    m_buckets.clear();
    m_columnOf.clear();
    m_bucketOf.clear();
    m_grid = SectionGrid{};
}

// ------------------------------------------------------------ Materializer

Materializer::~Materializer() {
    // A run is always ended (Sim::end, the prediction); this only guards one
    // that was not. The level has to be the one the table was read from and
    // still the one playing: a search dropped with the Trajectory (~Trajectory
    // runs after moverCacheInvalidate) would otherwise write into objects and
    // vectors the level has taken with it.
    if (m_written.empty() && m_shadow.empty()) return;
    if (m_pl && GJBaseGameLayer::get() == m_pl && tableCurrent()) {
        restoreWritten();
        return;
    }
    m_written.clear();
    m_writtenOf.clear();
    m_shadow.forget();
}

void Materializer::begin(GJBaseGameLayer* pl, std::shared_ptr<const BaseTable> table,
                         std::shared_ptr<const std::vector<Carry>> carry, uint32_t liveProgress, uint32_t liveCommandIndex,
                         int baseTick) {
    restoreWritten();
    m_pl = pl;
    m_baseTick = baseTick;
    m_table = std::move(table);
    m_carry = std::move(carry);
    // Every run of one decision is made from the same kept start and so shares
    // one carry list; the index of it is a pure function of the list. An area
    // effect carries thousands of objects, so building the index again for
    // every run was thousands of insertions per run for the same answer.
    if (m_carryOfFor != m_carry) {
        m_carryOf.clear();
        if (m_carry) {
            for (std::size_t i = 0; i < m_carry->size(); i++) m_carryOf[(*m_carry)[i].slot] = (uint32_t)i;
        }
        m_carryOfFor = m_carry;
    }
    if (pl) {
        const SectionGrid grid = SectionGrid::of(pl);
        m_shadow.attach(grid);
        m_fx = grid.factorX > 0.0f ? (double)grid.factorX : 0.01;
        m_fy = grid.factorY > 0.0f ? (double)grid.factorY : 0.01;
    }
    pendDiff(liveProgress, liveCommandIndex);
}

void Materializer::pendDiff(uint32_t liveProgress, uint32_t liveCommandIndex) {
    m_pending.clear();
    if (m_table) m_pending = m_table->liveDiff(liveProgress, liveCommandIndex);
    m_pendingPass = true;
    m_stale[0] = m_stale[1] = true;
}

void Materializer::resume(uint32_t liveProgress, uint32_t liveCommandIndex) {
    if (!m_table) return;
    pendDiff(liveProgress, liveCommandIndex);
}

void Materializer::invalidate() {
    m_stale[0] = m_stale[1] = true;
    // ... and what each copy has seen of every group, which counts the ops of
    // the log the branch appended to. A restore takes those ops back, so a
    // group whose count comes round again to one this copy has already looked
    // at would be skipped with its members still where the abandoned branch
    // left them - the same script answered two ways.
    m_memo[0].clear();
    m_memo[1].clear();
}

void Materializer::restoreWritten() {
    for (auto it = m_written.rbegin(); it != m_written.rend(); ++it) restoreOne(*it);
    m_shadow.restore();
    m_written.clear();
    m_writtenOf.clear();
    m_stale[0] = m_stale[1] = true;
    m_haveWindow[0] = m_haveWindow[1] = false;
    m_memo[0].clear();
    m_memo[1].clear();
    // What the real game had moved away from the table is back where the game
    // has it: written again on the next materializeNear.
    m_pendingPass = true;
}

void Materializer::end() {
    restoreWritten();
    m_table.reset();
    m_baseTick = 0;
    m_carry.reset();
    // m_carryOf is kept, and so is the list it indexes (m_carryOfFor), so the
    // next run over the same list does not build it again. It is only ever
    // read through m_carry, which is empty now - see desired().
    m_pending.clear();
    m_pendingPass = false;
    m_pl = nullptr;
}

Pose Materializer::desired(Run& run, int slot) {
    Pose p = run.cache->pose(*run.log, slot);
    if (m_carry) if (auto it = m_carryOf.find(slot); it != m_carryOf.end()) {
        const Carry& c = (*m_carry)[it->second];
        // MovingObjects' k: the run's first tick carries the object one step,
        // and its last position is always a step behind. The count runs from
        // the tick the table stands at, not from zero: a run out of a keyframe
        // begins at the keyframe's offset (Materializer::begin).
        const double k = (double)((int64_t)run.ws->tick - (int64_t)m_baseTick + 1);
        if (c.dx != 0.0f) {
            p.x = (double)c.dx * k + p.x;
            p.lastX = (float)p.x - c.dx;
        }
        if (c.dy != 0.0f) {
            p.y = (double)c.dy * k + p.y;
            p.lastY = (float)p.y - c.dy;
        }
        p.marker = run.ws->commandIndex;
    }
    return p;
}

Materializer::Window Materializer::windowOf(PlayerObject* copy) const {
    const cocos2d::CCPoint pos = copy->getPosition();
    int sx = 0, sy = 0;
    m_shadow.copySection(pos.x, pos.y, sx, sy);
    return Window{sx - 2, sx + 2, sy - 2, sy + 2};
}

void catchUpCurrentRun() {
    Run* run = currentRun();
    if (run && run->objects && run->objects->active()) run->objects->catchUpAll(*run);
}

bool Materializer::boxMeets(const Box& box, double rx, double ry, const Window& w) const {
    if (box.empty()) return false;
    if (std::isnan(box.minX) || std::isnan(box.maxX) || std::isnan(box.minY) || std::isnan(box.maxY) || std::isnan(rx) ||
        std::isnan(ry)) {
        return true;
    }
    const double inf = std::numeric_limits<double>::infinity();
    // Section k holds the positions whose factor times the position truncates
    // to k; section 0 everything at or below zero as well. A unit of slack
    // either side for the float of the factor.
    const double xlo = w.x0 <= 0 ? -inf : (double)w.x0 / m_fx - 1.0;
    const double xhi = (double)(w.x1 + 1) / m_fx + 1.0;
    const double ylo = w.y0 <= 0 ? -inf : (double)w.y0 / m_fy - 1.0;
    const double yhi = (double)(w.y1 + 1) / m_fy + 1.0;
    return (double)box.maxX + rx >= xlo && (double)box.minX - rx <= xhi && (double)box.maxY + ry >= ylo &&
           (double)box.minY - ry <= yhi;
}

bool Materializer::pointMeets(double x, double y, double rx, double ry, const Window& w) const {
    if (std::isnan(x) || std::isnan(y) || std::isnan(rx) || std::isnan(ry)) return true;
    const double inf = std::numeric_limits<double>::infinity();
    // The same bounds as boxMeets, for one position in double.
    const double xlo = w.x0 <= 0 ? -inf : (double)w.x0 / m_fx - 1.0;
    const double xhi = (double)(w.x1 + 1) / m_fx + 1.0;
    const double ylo = w.y0 <= 0 ? -inf : (double)w.y0 / m_fy - 1.0;
    const double yhi = (double)(w.y1 + 1) / m_fy + 1.0;
    return x + rx >= xlo && x - rx <= xhi && y + ry >= ylo && y - ry <= yhi;
}

bool Materializer::carriedAway(const Run& run, const Carry& c, const Window& w) const {
    const WorldDef& def = *run.def;
    if (c.slot < 0 || (std::size_t)c.slot >= def.slots.size()) return false;
    // Written (by this copy, the other, a catch-up or the pending pass): where
    // it stands is not the table's any more.
    if (m_writtenOf.contains(c.slot)) return false;
    if (def.flags[(std::size_t)c.slot] & kExtended) return false;
    // An op of the run on one of its groups: refresh works it out.
    const auto& reach = run.ws->reach;
    for (uint16_t g : def.groupsOf(c.slot)) {
        auto it = std::lower_bound(reach.begin(), reach.end(), g,
                                   [](const std::pair<uint16_t, GroupReach>& e, uint16_t key) { return e.first < key; });
        if (it != reach.end() && it->first == g) return false;
    }
    const Pose* base = m_table->find(c.slot);
    if (!base) return false;
    // Unwritten, it stands where the table read it and is filed in that
    // pose's section (the game re-files an object on every move); it belongs
    // where desired() puts it, with the same arithmetic.
    int sx = 0, sy = 0;
    if (m_shadow.sectionOf(base->x, base->y, sx, sy) && inWindow(w, sx, sy)) return false;
    const double k = (double)((int64_t)run.ws->tick - (int64_t)m_baseTick + 1);
    const double x = c.dx != 0.0f ? (double)c.dx * k + base->x : base->x;
    const double y = c.dy != 0.0f ? (double)c.dy * k + base->y : base->y;
    if (m_shadow.sectionOf(x, y, sx, sy) && inWindow(w, sx, sy)) return false;
    return true;
}

void Materializer::refresh(Run& run, int slot, const Window* window) {
    const WorldDef& def = *run.def;
    if (slot < 0 || (std::size_t)slot >= def.slots.size()) return;
    GameObject* o = def.slots[(std::size_t)slot];
    const uint8_t flags = def.flags[(std::size_t)slot];
    if (!o || !(flags & kCollidable)) return;
    const Pose want = desired(run, slot);
    auto it = m_writtenOf.find(slot);
    Written* w = it != m_writtenOf.end() ? &m_written[it->second] : nullptr;
    if (w ? sameWritable(w->pose, want) : sameLive(want, o)) return;
    if (window && !(flags & kExtended)) {
        // Neither where it stands nor where it belongs is anywhere the copy's
        // collision pass can reach: it waits until the window comes near.
        int wx = 0, wy = 0;
        const bool wantIn = m_shadow.sectionOf(want.x, want.y, wx, wy) && inWindow(*window, wx, wy);
        const bool nowIn = inWindow(*window, off::at<int>(o, off::kSecX), off::at<int>(o, off::kSecY));
        if (!wantIn && !nowIn) return;
    }
    if (!w) {
        Written fresh;
        fresh.o = o;
        fresh.slot = slot;
        Orig& r = fresh.orig;
        r.x = o->m_positionX;
        r.y = o->m_positionY;
        r.node = o->getPosition();
        r.last = o->m_lastPosition;
        r.marker = off::moveMarker(o);
        r.counter = o->m_enabledGroupsCounter;
        r.disabled = o->m_isGroupDisabled;
        r.slot = off::at<int>(o, off::kSlot);
        r.secX = off::at<int>(o, off::kSecX);
        r.secY = off::at<int>(o, off::kSecY);
        std::memcpy(r.rects.data(), &off::at<uint8_t>(o, off::kRectCaches), r.rects.size());
        r.box = off::at<OBB2D*>(o, off::kOrientedBox);
        r.useOuterOb = off::at<bool>(o, off::kUseOuterOb);
        if (r.box) std::memcpy(r.boxBytes.data(), reinterpret_cast<const char*>(r.box) + kBoxFirst, kBoxBytes);
        fresh.pose = ObjectCache::readLive(o);
        m_writtenOf[slot] = (uint32_t)m_written.size();
        m_written.push_back(fresh);
        w = &m_written.back();
    }
    write(*w, want, flags);
}

void Materializer::write(Written& w, const Pose& p, uint8_t flags) {
    GameObject* o = w.o;
    // The buckets first, from the slots the object has now; an object with
    // extended collision is in the list +0x35e0 whatever its section, and one
    // never filed (+0x278 below zero) in no bucket at all.
    if (!(flags & kExtended) && off::at<int>(o, off::kSecX) >= 0) {
        int sx = 0, sy = 0;
        if (m_shadow.sectionOf(p.x, p.y, sx, sy)) m_shadow.move(o, sx, sy);
    }
    o->m_positionX = p.x;
    o->m_positionY = p.y;
    o->setPosition(cocos2d::CCPoint{(float)p.x, (float)p.y});
    o->m_lastPosition = cocos2d::CCPoint{p.lastX, p.lastY};
    off::moveMarker(o) = p.marker;
    // moveObjects' words (world/step.cpp (f)): the position and both rects
    // dirty, so the copies' collision pass works them out at the new place.
    off::positionDirtyWord(o) = 0x101;
    off::rectDirtyWord(o) = 0x101;
    o->m_enabledGroupsCounter = p.counter;
    o->m_isGroupDisabled = p.disabled;
    w.pose = p;
}

void Materializer::restoreOne(Written& w) {
    GameObject* o = w.o;
    const Orig& r = w.orig;
    o->setPosition(r.node);
    o->m_positionX = r.x;
    o->m_positionY = r.y;
    o->m_lastPosition = r.last;
    off::moveMarker(o) = r.marker;
    o->m_enabledGroupsCounter = r.counter;
    o->m_isGroupDisabled = r.disabled;
    std::memcpy(&off::at<uint8_t>(o, off::kRectCaches), r.rects.data(), r.rects.size());
    OBB2D*& box = off::at<OBB2D*>(o, off::kOrientedBox);
    if (box == r.box) {
        if (box) std::memcpy(reinterpret_cast<char*>(box) + kBoxFirst, r.boxBytes.data(), kBoxBytes);
    } else if (!r.box && box) {
        // Made by the copies' collision pass while the object stood where the
        // run had it (s4): the retain getOrientedBox took is given back.
        box->release();
        box = nullptr;
    }
    off::at<bool>(o, off::kUseOuterOb) = r.useOuterOb;
    off::at<int>(o, off::kSlot) = r.slot;
    off::at<int>(o, off::kSecX) = r.secX;
    off::at<int>(o, off::kSecY) = r.secY;
}

// The members of a group a run's ops reach: m_groups, and anything the static
// array holds that m_groups does not (the game adds every static member to
// m_groups first, so normally nothing).
template <class Fn>
void forMembers(const WorldDef& def, int group, Fn&& fn) {
    const auto members = def.members(group);
    for (int slot : members) fn(slot);
    for (int slot : def.staticMembers(group)) {
        if (!std::binary_search(members.begin(), members.end(), slot)) fn(slot);
    }
}

bool Materializer::tableCurrent() const { return m_table && m_table->md && defCurrent(m_table->md->def.get()); }

bool Materializer::sameLevel(const Run& run) const {
    return tableCurrent() && m_table->md->def.get() == run.def && run.def && run.cache && run.log && run.ws;
}

void Materializer::materializeNear(Run& run, PlayerObject* copy) {
    if (!copy || !sameLevel(run)) return;
    if (m_pendingPass) {
        m_pendingPass = false;
        for (int slot : m_pending) refresh(run, slot, nullptr);
    }
    const int ci = (copy == run.player2 && run.player2) ? 1 : 0;
    const Window pass = windowOf(copy);
    // The dirty buckets the copy's collision pass will sort (s1), copied first.
    // Only checkCollisions sorts, and only its 3x3: a spider's taller window
    // below is read unsorted (staticObjectsInRect 0x211260 never looks at the
    // flags), so it copies nothing more.
    m_shadow.shadowDirty(pass.x0, pass.x1, pass.y0, pass.y1);
    Window w = pass;
    if (copy->m_isSpider) {
        // Every row of the columns (see world/materialize.hpp): the spider's
        // jump reads the buckets from the copy up or down to the bounds.
        w.y0 = 0;
        w.y1 = kMaxSection;
    }
    const bool look = m_stale[ci] || !m_haveWindow[ci] || !(m_window[ci] == w);
    if (look) {
        // What this run (or a branch it abandoned) wrote, near enough to be
        // met: back where the log has it now.
        for (std::size_t i = 0; i < m_written.size(); i++) refresh(run, m_written[i].slot, &w);
    }
    const MaterialDef& md = *m_table->md;
    const auto& reach = run.ws->reach;
    for (const auto& entry : reach) {
        const int g = entry.first;
        const auto co = md.coGroupsOf(g);
        if (co.empty()) continue;  // no member a copy can collide with
        // No op on the group itself since this copy last looked, and the
        // window is the same: none of its members has moved through it. A
        // member another group moves is looked at under that group - it has
        // ops, so it is in the reach, and its box is grown by what every group
        // of its members did - so the group's own count is all this needs,
        // and the sums over the co-groups are only worked out for a group it
        // moved. (They were worked out first for every group on every tick: a
        // group most of the level shares made that hundreds of searches for
        // each of hundreds of groups, per copy per tick of every run.)
        uint64_t& memo = m_memo[ci][g];
        const uint64_t seen = (uint64_t)entry.second.ops + 1;
        if (!look && memo == seen) continue;
        memo = seen;
        double rx = 0.0, ry = 0.0;
        for (uint16_t h : co) {
            auto it = std::lower_bound(reach.begin(), reach.end(), h,
                                       [](const std::pair<uint16_t, GroupReach>& e, uint16_t key) { return e.first < key; });
            if (it == reach.end() || it->first != h) continue;
            const double m = (double)md.mult[h];
            rx += it->second.x * m;
            ry += it->second.y * m;
        }
        if (!md.hasExtended[(std::size_t)g] && !boxMeets(m_table->groupBox(g), rx + 1.0, ry + 1.0, w)) continue;
        forMembers(*run.def, g, [&](int slot) {
            const int i = md.indexOf[(std::size_t)slot];
            if (i < 0) return;
            // A member whose table pose, grown by what every group of the
            // group's members has done, is nowhere near the window is neither
            // in it nor bound for it, and refresh would leave it: skipped
            // without reading the object (a group spread over the level meets
            // every window, and reading each of its members on every tick it
            // moves was most of the step). Where it stands is bound the same
            // way: unwritten, it is at the table pose; written and in the
            // window, it was put where the log had it at the last look or
            // since, and the sums only grow until a restore, which looks again.
            if (!(run.def->flags[(std::size_t)slot] & kExtended)) {
                const Pose& base = m_table->poses[(std::size_t)i];
                if (!pointMeets(base.x, base.y, rx + 1.0, ry + 1.0, w)) return;
            }
            refresh(run, slot, &w);
        });
    }
    if (m_carry) {
        // The same for what is carried: one far from the window, which nothing
        // of the run has written or moved, is left without reading it (an area
        // effect can carry thousands).
        for (const Carry& c : *m_carry) {
            if (!carriedAway(run, c, w)) refresh(run, c.slot, &w);
        }
    }
    m_stale[ci] = false;
    m_window[ci] = w;
    m_haveWindow[ci] = true;
}

void Materializer::catchUpAll(Run& run) {
    if (!sameLevel(run)) return;
    if (m_pendingPass) {
        m_pendingPass = false;
        for (int slot : m_pending) refresh(run, slot, nullptr);
    }
    for (std::size_t i = 0; i < m_written.size(); i++) refresh(run, m_written[i].slot, nullptr);
    const MaterialDef& md = *m_table->md;
    for (const auto& entry : run.ws->reach) {
        if (md.coGroupsOf(entry.first).empty()) continue;
        forMembers(*run.def, entry.first, [&](int slot) {
            if (md.indexOf[(std::size_t)slot] >= 0) refresh(run, slot, nullptr);
        });
    }
    if (m_carry) {
        for (const Carry& c : *m_carry) refresh(run, c.slot, nullptr);
    }
}

// ------------------------------------------------------------ the self-test

namespace {

// A grid of the test's own, laid out as the game's, with fake objects that
// hold nothing but the fields SectionShadow reads and writes.
struct TestGrid {
    RawVec buckets, counts, dirty;
    std::vector<std::unique_ptr<uint8_t[]>> objects;

    GameObject* object(std::size_t i) const { return reinterpret_cast<GameObject*>(objects[i].get()); }
    SectionGrid grid() {
        SectionGrid g;
        g.buckets = &buckets;
        g.counts = &counts;
        g.dirty = &dirty;
        return g;
    }
};

struct TestRng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 16);
    }
};

// Every header, buffer and slot of the grid, flattened: the image restore
// must give back.
std::vector<uint64_t> imageOf(const TestGrid& t) {
    std::vector<uint64_t> out;
    auto vec = [&](const RawVec& v, std::size_t elemBytes) {
        out.push_back((uint64_t)(uintptr_t)v.first);
        out.push_back((uint64_t)(uintptr_t)v.last);
        out.push_back((uint64_t)(uintptr_t)v.end);
        const std::size_t n = v.first ? (std::size_t)(v.last - v.first) / elemBytes : 0;
        for (std::size_t i = 0; i < n; i++) {
            uint64_t value = 0;
            std::memcpy(&value, v.first + i * elemBytes, elemBytes);
            out.push_back(value);
        }
    };
    vec(t.buckets, 8);
    vec(t.counts, 8);
    vec(t.dirty, 8);
    for (std::size_t x = 0; x < countOf(t.buckets, 8); x++) {
        if (auto* col = elem<RawVec*>(t.buckets, x)) {
            vec(*col, 8);
            for (std::size_t y = 0; y < countOf(*col, 8); y++) {
                if (auto* b = elem<RawVec*>(*col, y)) vec(*b, 8);
            }
        }
        if (x < countOf(t.counts, 8)) {
            if (auto* c = elem<RawVec*>(t.counts, x)) vec(*c, 4);
        }
        if (x < countOf(t.dirty, 8)) {
            if (auto* d = elem<RawBits*>(t.dirty, x)) {
                vec(d->words, 4);
                out.push_back(d->bits);
            }
        }
    }
    for (std::size_t i = 0; i < t.objects.size(); i++) {
        GameObject* o = t.object(i);
        out.push_back((uint32_t)off::at<int>(o, off::kSlot));
        out.push_back((uint32_t)off::at<int>(o, off::kSecX));
        out.push_back((uint32_t)off::at<int>(o, off::kSecY));
    }
    return out;
}

// checkCollisions' sort of a dirty bucket (s1), on whatever storage the grid
// has now.
void testSort(TestGrid& t, int x0, int x1, int y0, int y1) {
    for (int x = std::max(x0, 0); x <= x1 && (std::size_t)x < countOf(t.buckets, 8); x++) {
        auto* col = elem<RawVec*>(t.buckets, (std::size_t)x);
        auto* cnt = elem<RawVec*>(t.counts, (std::size_t)x);
        auto* bits = elem<RawBits*>(t.dirty, (std::size_t)x);
        if (!col || !cnt || !bits) continue;
        for (int y = std::max(y0, 0); y <= y1 && (std::size_t)y < countOf(*col, 8); y++) {
            auto* b = elem<RawVec*>(*col, (std::size_t)y);
            if (!b || !bitSet(*bits, (std::size_t)y)) continue;
            const int n = elem<int>(*cnt, (std::size_t)y);
            auto* first = reinterpret_cast<GameObject**>(b->first);
            std::sort(first, first + n, [](GameObject* a, GameObject* c) {
                return off::at<int>(a, off::kUid) < off::at<int>(c, off::kUid);
            });
            for (int i = 0; i < n; i++) off::at<int>(first[i], off::kSlot) = i;
            elem<uint32_t>(bits->words, (std::size_t)y >> 5) &= ~(uint32_t{1} << (y & 31));
        }
    }
}

void freeTestGrid(TestGrid& t) {
    for (std::size_t x = 0; x < countOf(t.buckets, 8); x++) {
        if (auto* col = elem<RawVec*>(t.buckets, x)) {
            for (std::size_t y = 0; y < countOf(*col, 8); y++) {
                if (auto* b = elem<RawVec*>(*col, y)) {
                    gameDelete(b->first, (std::size_t)(b->end - b->first));
                    gameDelete(b, sizeof(RawVec));
                }
            }
            gameDelete(col->first, (std::size_t)(col->end - col->first));
            gameDelete(col, sizeof(RawVec));
        }
        if (auto* c = elem<RawVec*>(t.counts, x)) {
            gameDelete(c->first, (std::size_t)(c->end - c->first));
            gameDelete(c, sizeof(RawVec));
        }
        if (auto* d = elem<RawBits*>(t.dirty, x)) {
            gameDelete(d->words.first, (std::size_t)(d->words.end - d->words.first));
            gameDelete(d, sizeof(RawBits));
        }
    }
    for (RawVec* v : {&t.buckets, &t.counts, &t.dirty}) gameDelete(v->first, (std::size_t)(v->end - v->first));
}

}  // namespace

const char* checkSectionShadow() {
    constexpr std::size_t kObjects = 48;
    constexpr int kColumns = 5;  // column 2 is null, the grid is 5 x 4 with a null bucket
    constexpr int kRows = 4;
    TestRng rng{0x5EC7105ull};
    TestGrid t;
    auto newVec = [](std::size_t elemBytes, std::size_t count, std::size_t cap) {
        RawVec v;
        v.first = allocZeroed(std::max<std::size_t>(cap, 1) * elemBytes);
        v.last = v.first + count * elemBytes;
        v.end = v.first + std::max<std::size_t>(cap, 1) * elemBytes;
        return v;
    };
    t.buckets = newVec(8, kColumns, kColumns);
    t.counts = newVec(8, kColumns, kColumns + 2);
    t.dirty = newVec(8, kColumns, kColumns);
    for (int x = 0; x < kColumns; x++) {
        if (x == 2) continue;
        auto* col = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
        auto* cnt = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
        auto* bits = reinterpret_cast<RawBits*>(allocZeroed(sizeof(RawBits)));
        *col = newVec(8, kRows, kRows);
        *cnt = newVec(4, kRows, kRows + 1);
        bits->words = newVec(4, 1, 1);
        bits->bits = kRows;
        for (int y = 0; y < kRows; y++) {
            if (x == 1 && y == 3) continue;
            auto* b = reinterpret_cast<RawVec*>(allocZeroed(sizeof(RawVec)));
            *b = newVec(8, 0, 4);
            elem<RawVec*>(*col, (std::size_t)y) = b;
        }
        elem<RawVec*>(t.buckets, (std::size_t)x) = col;
        elem<RawVec*>(t.counts, (std::size_t)x) = cnt;
        elem<RawBits*>(t.dirty, (std::size_t)x) = bits;
    }
    // The objects, filed as addToSection files them; some buckets keep a stale
    // entry past their count, as removeObjectFromSection leaves one.
    for (std::size_t i = 0; i < kObjects; i++) {
        t.objects.push_back(std::make_unique<uint8_t[]>(0x600));
        std::memset(t.objects.back().get(), 0, 0x600);
        GameObject* o = t.object(i);
        off::at<int>(o, off::kUid) = (int)((i * 7919u) % 1000u) + 1;
        int x, y;
        RawVec* b = nullptr;
        do {
            x = (int)(rng.next() % kColumns);
            y = (int)(rng.next() % kRows);
            auto* col = elem<RawVec*>(t.buckets, (std::size_t)x);
            b = col ? elem<RawVec*>(*col, (std::size_t)y) : nullptr;
        } while (!b);
        int& count = elem<int>(*elem<RawVec*>(t.counts, (std::size_t)x), (std::size_t)y);
        if ((std::size_t)count < countOf(*b, 8)) {
            elem<GameObject*>(*b, (std::size_t)count) = o;
        } else {
            if (b->last == b->end) {
                const std::size_t used = (std::size_t)(b->last - b->first);
                char* buf = allocZeroed(used * 2 + 8);
                std::memcpy(buf, b->first, used);
                gameDelete(b->first, (std::size_t)(b->end - b->first));
                b->first = buf;
                b->last = buf + used;
                b->end = buf + used * 2 + 8;
            }
            *reinterpret_cast<GameObject**>(b->last) = o;
            b->last += 8;
        }
        off::at<int>(o, off::kSlot) = count;
        off::at<int>(o, off::kSecX) = x;
        off::at<int>(o, off::kSecY) = y;
        count++;
        if (rng.next() % 3 == 0) {
            auto* bits = elem<RawBits*>(t.dirty, (std::size_t)x);
            setBit(*bits, (std::size_t)y);
        }
    }
    const std::vector<uint64_t> before = imageOf(t);
    // The slots each object records: the materializer puts these back from
    // what it saved before its first write, ahead of the shadow's restore.
    std::vector<std::array<int, 3>> slots(kObjects);
    for (std::size_t i = 0; i < kObjects; i++) {
        GameObject* o = t.object(i);
        slots[i] = {off::at<int>(o, off::kSlot), off::at<int>(o, off::kSecX), off::at<int>(o, off::kSecY)};
    }
    const char* failure = nullptr;
    {
        SectionShadow shadow;
        shadow.attach(t.grid());
        for (int step = 0; step < 400 && !failure; step++) {
            GameObject* o = t.object(rng.next() % kObjects);
            // Into sections past the grid, into the null column, into the null
            // bucket, and back.
            const int sx = (int)(rng.next() % 9);
            const int sy = (int)(rng.next() % 8);
            if (!shadow.move(o, sx, sy)) {
                failure = "SectionShadow check: a move the game's rules allow was refused";
                break;
            }
            if (off::at<int>(o, off::kSecX) != sx || off::at<int>(o, off::kSecY) != sy) {
                failure = "SectionShadow check: a moved object is not filed where it was sent";
                break;
            }
            if (step % 9 == 0) {
                const int cx = (int)(rng.next() % 9);
                const int cy = (int)(rng.next() % 8);
                shadow.shadowDirty(cx - 2, cx + 2, cy - 2, cy + 2);
                testSort(t, cx - 1, cx + 1, cy - 1, cy + 1);
            }
            // Every object sits at the slot it records, within its bucket's count.
            for (std::size_t i = 0; i < kObjects && !failure; i++) {
                GameObject* e = t.object(i);
                const int x = off::at<int>(e, off::kSecX), y = off::at<int>(e, off::kSecY), s = off::at<int>(e, off::kSlot);
                auto* col = (std::size_t)x < countOf(t.buckets, 8) ? elem<RawVec*>(t.buckets, (std::size_t)x) : nullptr;
                auto* cnt = (std::size_t)x < countOf(t.counts, 8) ? elem<RawVec*>(t.counts, (std::size_t)x) : nullptr;
                auto* b = (col && (std::size_t)y < countOf(*col, 8)) ? elem<RawVec*>(*col, (std::size_t)y) : nullptr;
                if (!b || !cnt || s < 0 || s >= elem<int>(*cnt, (std::size_t)y) ||
                    elem<GameObject*>(*b, (std::size_t)s) != e) {
                    failure = "SectionShadow check: an object is not at the slot it records";
                }
            }
        }
        for (std::size_t i = 0; i < kObjects; i++) {
            GameObject* o = t.object(i);
            off::at<int>(o, off::kSlot) = slots[i][0];
            off::at<int>(o, off::kSecX) = slots[i][1];
            off::at<int>(o, off::kSecY) = slots[i][2];
        }
        shadow.restore();
    }
    if (!failure && imageOf(t) != before) failure = "SectionShadow check: the restore left the buckets different";
    freeTestGrid(t);
    return failure;
}

}  // namespace world

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
