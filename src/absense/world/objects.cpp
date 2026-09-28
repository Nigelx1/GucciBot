// OpLog and ObjectCache (trigger design step 4): where the objects of a run
// stand, from the ops its triggers emitted, with the game's own arithmetic.

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "absense/compat/bot.hpp"  // SL_DEV_MODE
#include "absense/compat/devlog.hpp"
#include "absense/world/def.hpp"
#include "absense/world/materialize.hpp"
#include "absense/world/offsets.hpp"
#include "absense/world/state.hpp"

namespace world {

// ------------------------------------------------------------ chunks and the log

std::span<const std::pair<int, uint32_t>> Chunk::opsOfGroup(int group) const {
    auto lo = std::lower_bound(byGroup.begin(), byGroup.end(), group,
                               [](const std::pair<int, uint32_t>& e, int g) { return e.first < g; });
    auto hi = std::upper_bound(lo, byGroup.end(), group,
                               [](int g, const std::pair<int, uint32_t>& e) { return g < e.first; });
    return std::span<const std::pair<int, uint32_t>>(byGroup.data() + (lo - byGroup.begin()), (std::size_t)(hi - lo));
}

uint64_t OpLog::nextKey() {
    // Runs are only ever stepped on the game's thread.
    static uint64_t counter = 0;
    return ++counter;
}

OpLog::OpLog() { m_open.id = nextKey(); }

OpLog::OpLog(const OpLog& other) : m_sealed(other.m_sealed), m_open(other.m_open) { m_open.id = nextKey(); }

OpLog& OpLog::operator=(const OpLog& other) {
    if (this != &other) {
        m_sealed = other.m_sealed;
        m_open.firstTick = other.m_open.firstTick;
        m_open.lastTick = other.m_open.lastTick;
        m_open.ops = other.m_open.ops;  // reuses the capacity it already has
        m_open.fromKey = 0;
        m_open.byGroup.clear();
    }
    // Even assigned from itself: whatever this log appends from now on is not
    // what it appended before under the old key.
    m_open.id = nextKey();
    return *this;
}

OpLog::OpLog(OpLog&& other) noexcept : m_sealed(std::move(other.m_sealed)), m_open(std::move(other.m_open)) {
    other.m_sealed.clear();
    other.m_open.ops.clear();
    other.m_open.id = nextKey();
}

OpLog& OpLog::operator=(OpLog&& other) noexcept {
    if (this != &other) {
        m_sealed = std::move(other.m_sealed);
        m_open = std::move(other.m_open);
        other.m_sealed.clear();
        other.m_open.ops.clear();
        other.m_open.id = nextKey();
    }
    return *this;
}

void OpLog::reset(uint32_t tick) {
    m_sealed.clear();
    m_open.ops.clear();
    m_open.byGroup.clear();
    m_open.fromKey = 0;
    m_open.firstTick = tick - tick % kSealTicks;
    m_open.lastTick = m_open.firstTick;
    m_open.id = nextKey();
}

void OpLog::seal() {
    auto c = std::make_shared<Chunk>();
    c->id = nextKey();
    c->fromKey = m_open.id;
    c->firstTick = m_open.firstTick;
    c->lastTick = m_open.lastTick;
    c->ops = std::move(m_open.ops);
    c->byGroup.reserve(c->ops.size());
    for (uint32_t i = 0; i < (uint32_t)c->ops.size(); i++) c->byGroup.push_back({c->ops[i].group, i});
    std::sort(c->byGroup.begin(), c->byGroup.end());
    m_sealed.push_back(std::move(c));
    m_open.ops.clear();
    m_open.byGroup.clear();
    m_open.id = nextKey();
}

void OpLog::beginTick(uint32_t tick) {
    const uint32_t window = tick - tick % kSealTicks;
    if (window == m_open.firstTick) return;
    if (!m_open.ops.empty()) seal();
    // An empty open chunk just moves on: nothing points into it.
    m_open.firstTick = window;
    m_open.lastTick = window;
}

void OpLog::append(const Op& op) {
    beginTick(std::max(op.tick, m_open.firstTick));
    m_open.ops.push_back(op);
    // The game clamps every group id it is handed to 0..9999 before it looks
    // the group up, so an op is kept under the group it really touches.
    m_open.ops.back().group = (int)WorldDef::clampGroup(op.group);
    m_open.lastTick = std::max(m_open.lastTick, op.tick);
}

std::size_t OpLog::size() const {
    std::size_t n = m_open.ops.size();
    for (const auto& c : m_sealed) n += c->ops.size();
    return n;
}

bool OpLog::sameOps(const OpLog& other) const {
    if (size() != other.size()) return false;
    auto walk = [](const OpLog& log, auto&& fn) {
        for (const auto& c : log.m_sealed) {
            for (const Op& op : c->ops) fn(op);
        }
        for (const Op& op : log.m_open.ops) fn(op);
    };
    std::vector<const Op*> mine;
    mine.reserve(size());
    walk(*this, [&](const Op& op) { mine.push_back(&op); });
    std::size_t i = 0;
    bool same = true;
    walk(other, [&](const Op& op) {
        if (same && !bitEqual(*mine[i], op)) same = false;
        i++;
    });
    return same;
}

bool bitEqual(const Op& a, const Op& b) {
    auto bits = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) == 0; };
    return a.tick == b.tick && a.commandIndex == b.commandIndex && a.kind == b.kind && a.sign == b.sign &&
           a.group == b.group && bits(a.dx, b.dx) && bits(a.dy, b.dy) && bits(a.angle, b.angle) && bits(a.cx, b.cx) &&
           bits(a.cy, b.cy) && bits(a.sx, b.sx) && bits(a.sy, b.sy);
}

// ------------------------------------------------------------ bit-exact equality

namespace {
// A float or double the same bit for bit; anything else by its own ==.
template <class T>
bool sameBits(const T& a, const T& b) {
    if (std::is_floating_point_v<T>) return std::memcmp(&a, &b, sizeof(T)) == 0;
    return a == b;
}
}  // namespace

bool WCmd::operator==(const WCmd& o) const {
#define WORLD_SAME(field) sameBits(field, o.field) &&
    return WORLD_SAME(uid) WORLD_SAME(moveOffsetX) WORLD_SAME(moveOffsetY) WORLD_SAME(easingType)
        WORLD_SAME(easingRate) WORLD_SAME(duration) WORLD_SAME(deltaTime) WORLD_SAME(targetGroup)
        WORLD_SAME(centerGroup) WORLD_SAME(currentX) WORLD_SAME(currentY) WORLD_SAME(deltaX) WORLD_SAME(deltaY)
        WORLD_SAME(oldDeltaX) WORLD_SAME(oldDeltaY) WORLD_SAME(lockedCurrentX) WORLD_SAME(lockedCurrentY)
        WORLD_SAME(finished) WORLD_SAME(disabled) WORLD_SAME(finishRelated) WORLD_SAME(lockPlayerX)
        WORLD_SAME(lockPlayerY) WORLD_SAME(lockCameraX) WORLD_SAME(lockCameraY) WORLD_SAME(lockedInX)
        WORLD_SAME(lockedInY) WORLD_SAME(modX) WORLD_SAME(modY) WORLD_SAME(rotateValue) WORLD_SAME(rotateDelta)
        WORLD_SAME(interpOne1) WORLD_SAME(interpOne2) WORLD_SAME(rotationOffset) WORLD_SAME(lockObjectRotation)
        WORLD_SAME(targetPlayer) WORLD_SAME(followXMod) WORLD_SAME(followYMod) WORLD_SAME(commandType)
        WORLD_SAME(interp1) WORLD_SAME(interp2) WORLD_SAME(keyframeRelated) WORLD_SAME(targetScaleX)
        WORLD_SAME(targetScaleY) WORLD_SAME(property450) WORLD_SAME(property451) WORLD_SAME(interpZero1)
        WORLD_SAME(interpZero2) WORLD_SAME(onlyMove) WORLD_SAME(transformFlag) WORLD_SAME(relativeRotation)
        WORLD_SAME(interpRelated1) WORLD_SAME(interpRelated2) WORLD_SAME(followYSpeed) WORLD_SAME(followYDelay)
        WORLD_SAME(followYOffset) WORLD_SAME(followYMaxSpeed) WORLD_SAME(triggerUid) WORLD_SAME(controlId)
        WORLD_SAME(deltaX3) WORLD_SAME(deltaY3) WORLD_SAME(oldDeltaX3) WORLD_SAME(oldDeltaY3)
        WORLD_SAME(delta3Related) WORLD_SAME(unusedDouble) WORLD_SAME(actionType1) WORLD_SAME(actionType2)
        WORLD_SAME(actionValue1) WORLD_SAME(actionValue2) WORLD_SAME(interpRelatedFalse) WORLD_SAME(deltaTimeFloat)
        WORLD_SAME(alreadyUpdated) WORLD_SAME(doUpdate) WORLD_SAME(keyframeCount) WORLD_SAME(splineX)
        WORLD_SAME(splineY) WORLD_SAME(objectSlot) WORLD_SAME(objectUid) WORLD_SAME(objectRotation)
        WORLD_SAME(remap) WORLD_SAME(interpRelatedTrue) WORLD_SAME(unk204) true;
}

bool GroupReach::operator==(const GroupReach& o) const {
    return WORLD_SAME(x) WORLD_SAME(y) WORLD_SAME(ops) true;
}

bool WSpawn::operator==(const WSpawn& o) const {
    return WORLD_SAME(finished) WORLD_SAME(disabled) WORLD_SAME(duration) WORLD_SAME(delta) WORLD_SAME(targetGroup)
        WORLD_SAME(triggerUid) WORLD_SAME(controlId) WORLD_SAME(ordered) WORLD_SAME(objectSlot) WORLD_SAME(objectUid)
        WORLD_SAME(remap) true;
}

bool WTimer::operator==(const WTimer& o) const {
    return WORLD_SAME(key) WORLD_SAME(itemId) WORLD_SAME(time) WORLD_SAME(active) WORLD_SAME(timeMod)
        WORLD_SAME(ignoreTimeWarp) WORLD_SAME(targetTime) WORLD_SAME(stopTimeEnabled) WORLD_SAME(targetGroup)
        WORLD_SAME(triggerUid) WORLD_SAME(controlId) WORLD_SAME(remap) WORLD_SAME(disabled) true;
}

bool WTimerListener::operator==(const WTimerListener& o) const {
    return WORLD_SAME(item) WORLD_SAME(disabled) WORLD_SAME(time) WORLD_SAME(targetTime) WORLD_SAME(targetGroup)
        WORLD_SAME(triggerUid) WORLD_SAME(controlId) WORLD_SAME(itemId) WORLD_SAME(multiActivate) WORLD_SAME(remap)
        true;
}

bool WSequence::operator==(const WSequence& o) const {
    return WORLD_SAME(slot) WORLD_SAME(key) WORLD_SAME(time) WORLD_SAME(index) true;
#undef WORLD_SAME
}

// ------------------------------------------------------------ poses

bool bitEqual(const Pose& a, const Pose& b) {
    auto same = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) == 0; };
    return same(a.x, b.x) && same(a.y, b.y) && same(a.lastX, b.lastX) && same(a.lastY, b.lastY) &&
           a.marker == b.marker && same(a.rot, b.rot) && same(a.sx, b.sx) && same(a.sy, b.sy) &&
           a.counter == b.counter && a.disabled == b.disabled && a.noMoveX == b.noMoveX && a.skipLast == b.skipLast &&
           a.uncertain == b.uncertain;
}

Pose ObjectCache::readLive(GameObject* o) {
    Pose p;
    if (!o) return p;
    p.x = o->m_positionX;
    p.y = o->m_positionY;
    p.lastX = o->m_lastPosition.x;
    p.lastY = o->m_lastPosition.y;
    p.marker = off::moveMarker(o);
    p.rot = o->getRotationX();
    p.sx = o->getScaleX();
    p.sy = o->getScaleY();
    p.counter = o->m_enabledGroupsCounter;
    p.disabled = o->m_isGroupDisabled;
    p.noMoveX = o->m_tempOffsetXRelated;
    p.skipLast = off::skipLastPosition(o);
    return p;
}

void ObjectCache::applyOnce(Pose& p, const Op& op) {
    switch (op.kind) {
        case OpKind::Translate:
        case OpKind::TranslateOptimized: {
            // moveObjects 0x22dd50, for one entry of the array (dx and dy come
            // in as doubles made from the node's float sums):
            // 0x22ddf5: the first move of a command index takes m_lastPosition
            // from the double position, unless +0x512 is set.
            if (!p.skipLast && p.marker != op.commandIndex) {
                p.lastX = (float)p.x;
                p.lastY = (float)p.y;
                p.marker = op.commandIndex;
            }
            const double dx = (double)op.dx;
            const double dy = (double)op.dy;
            // Then the double position moves, x only without +0x2c8.
            if (dx != 0.0 && !p.noMoveX) p.x += dx;
            if (dy != 0.0) p.y += dy;
            break;
        }
        case OpKind::Toggle:
            // toggleGroup 0x223c6c / 0x223ca9: the counter moves by one and the
            // object is disabled while it is below zero (the sign bit, 0x223c7c).
            p.counter += op.sign >= 0 ? 1 : -1;
            p.disabled = p.counter < 0;
            break;
        case OpKind::Place: {
            // triggerMoveCommand's silent move, 0x21ee2c-0x21eea0: no marker and
            // no dx != 0 test, the last position is the new one.
            const double dx = (double)op.dx;
            const double dy = (double)op.dy;
            if (!p.noMoveX) p.x = dx + p.x;
            p.y = dy + p.y;
            p.lastX = (float)p.x;
            p.lastY = (float)p.y;
            break;
        }
        case OpKind::RotateAbout:
        case OpKind::Scale:
            // Not ported, and not portable as arithmetic: rotateObjects
            // 0x22c1a0 does not work the rotated position out with sin and
            // cos. It parks the object layer's own scale, rotation and
            // position (m_objectLayer +0xfe8), sets m_areaTransformNode
            // (+0x8c8) to the centre with the angle on it, hangs
            // m_areaTransformNode2 (+0x8e8) under it, puts each object's
            // offset on the child and reads the translation out of the
            // child's nodeToWorldTransform (vtable+0x308, 0x22c543-0x22c5ad) -
            // live cocos nodes, which a run must not write, and a transform
            // chain that depends on what those nodes hold. processTransformActions
            // 0x22c680 is built the same way. The design listed RotateAbout
            // and Scale as ops with "the game arithmetic"; there is none to
            // copy, so until a run can reproduce that node chain exactly an
            // object either touches leaves the run's answer uncertain, which
            // is what it was before the World.
            p.uncertain = true;
            break;
    }
}

void ObjectCache::reset(std::shared_ptr<const WorldDef> def) {
    m_def = std::move(def);
    m_table.reset();
    m_bases.clear();
    m_entries.clear();
    m_boundaries.clear();
}

void ObjectCache::setBaseTable(std::shared_ptr<const BaseTable> table) {
    m_table = std::move(table);
    // Whatever was worked out from the live objects is stale.
    m_entries.clear();
    m_boundaries.clear();
}

void ObjectCache::setBase(int slot, const Pose& base) {
    m_bases[slot] = base;
    // Everything worked out from the old base is stale.
    m_entries.erase(slot);
    std::erase_if(m_boundaries, [slot](const auto& e) { return e.first.slot == slot; });
}

const Pose& ObjectCache::base(int slot) {
    auto it = m_bases.find(slot);
    if (it != m_bases.end()) return it->second;
    if (m_table) {
        if (const Pose* p = m_table->find(slot)) return *p;
    }
    GameObject* o = (m_def && slot >= 0 && (std::size_t)slot < m_def->slots.size()) ? m_def->slots[(std::size_t)slot] : nullptr;
    return m_bases.emplace(slot, readLive(o)).first->second;
}

int ObjectCache::multiplicity(OpKind kind, int group, int slot) const {
    if (!m_def) return 0;
    switch (kind) {
        case OpKind::Translate: return WorldDef::countIn(m_def->staticMembers(group), slot);
        case OpKind::TranslateOptimized: return WorldDef::countIn(m_def->optimizedMembers(group), slot);
        case OpKind::Toggle:
        case OpKind::Place: return WorldDef::countIn(m_def->members(group), slot);
        case OpKind::RotateAbout:
        case OpKind::Scale:
            // Whichever array the unported step uses, a member of the group is touched.
            return WorldDef::countIn(m_def->members(group), slot) + WorldDef::countIn(m_def->staticMembers(group), slot) +
                           WorldDef::countIn(m_def->optimizedMembers(group), slot) > 0
                       ? 1
                       : 0;
    }
    return 0;
}

bool ObjectCache::chunkTouches(const Chunk& c, int slot) const {
    for (uint16_t g : m_def->groupsOf(slot)) {
        if (!c.opsOfGroup(g).empty()) return true;
    }
    return false;
}

bool ObjectCache::resolve(Entry& e, const OpLog& log) const {
    const auto& sealed = log.sealed();
    if (e.chunk < sealed.size()) {
        const Chunk& c = *sealed[e.chunk];
        if (c.id == e.key) return e.op <= c.ops.size();
        // The open chunk the entry was made in has since been sealed, in this
        // very log: the ops it has already taken are the chunk's first ones.
        if (c.fromKey == e.key && e.op <= c.ops.size()) {
            e.key = c.id;
            return true;
        }
        return false;
    }
    return e.chunk == sealed.size() && e.key == log.openKey() && e.op <= log.open().ops.size();
}

void ObjectCache::replay(Pose& p, const OpLog& log, int slot, std::size_t chunk, uint32_t op, uint32_t uptoTick,
                         bool record) {
    const auto& sealed = log.sealed();
    const auto groups = m_def->groupsOf(slot);
    if (groups.empty()) return;  // in no group: no op ever touches it
    // The cache's own scratch list: a pose is asked for per object per tick,
    // and a vector made and freed on every call was an allocation each time.
    // (A replay never runs inside another one.)
    std::vector<uint32_t>& picks = m_picks;
    for (std::size_t c = chunk; c <= sealed.size(); c++) {
        const bool open = c == sealed.size();
        const Chunk& ch = open ? log.open() : *sealed[c];
        const uint32_t from = c == chunk ? op : 0;
        picks.clear();
        if (open) {
            for (uint32_t i = from; i < (uint32_t)ch.ops.size(); i++) {
                if (std::binary_search(groups.begin(), groups.end(), (uint16_t)ch.ops[i].group)) picks.push_back(i);
            }
        } else {
            for (uint16_t g : groups) {
                for (const auto& [group, index] : ch.opsOfGroup(g)) {
                    if (index >= from) picks.push_back(index);
                }
            }
            // In log order: an object in two groups takes their ops as the
            // game ran them.
            std::sort(picks.begin(), picks.end());
        }
        bool touched = false;
        for (uint32_t i : picks) {
            const Op& o = ch.ops[i];
            if (o.tick > uptoTick) return;  // ticks never go down along a log
            const int times = multiplicity(o.kind, o.group, slot);
            // An object listed twice in the array is moved twice, as moveObjects
            // walks every entry.
            for (int t = 0; t < times; t++) applyOnce(p, o);
            touched = touched || times > 0;
        }
        if (!open && ch.lastTick > uptoTick) return;
        if (record && !open && touched) {
            if (m_boundaries.size() >= (std::size_t{1} << 15)) m_boundaries.clear();  // only a cache (29 MB at 1<<18)
            m_boundaries[BoundaryKey{slot, ch.id}] = p;
        }
    }
}

bool ObjectCache::nearestBoundary(const OpLog& log, int slot, uint32_t uptoTick, Pose& p, std::size_t& chunk) {
    const auto& sealed = log.sealed();
    for (std::size_t k = sealed.size(); k-- > 0;) {
        const Chunk& c = *sealed[k];
        if (c.lastTick > uptoTick) continue;
        if (!chunkTouches(c, slot)) continue;
        auto it = m_boundaries.find(BoundaryKey{slot, c.id});
        if (it != m_boundaries.end()) {
            p = it->second;
            chunk = k + 1;
            return true;
        }
    }
    return false;
}

const Pose& ObjectCache::pose(const OpLog& log, int slot) {
    static const Pose kNone{};
    if (!m_def || slot < 0 || (std::size_t)slot >= m_def->slots.size()) return kNone;
    const uint32_t all = std::numeric_limits<uint32_t>::max();
    auto it = m_entries.find(slot);
    if (it != m_entries.end() && resolve(it->second, log)) {
        Entry& e = it->second;
        replay(e.pose, log, slot, e.chunk, e.op, all, true);
    } else {
        Entry e;
        std::size_t chunk = 0;
        if (!nearestBoundary(log, slot, all, e.pose, chunk)) e.pose = base(slot);
        replay(e.pose, log, slot, chunk, 0, all, true);
        it = m_entries.insert_or_assign(slot, e).first;
    }
    Entry& e = it->second;
    e.chunk = (uint32_t)log.sealed().size();
    e.key = log.openKey();
    e.op = (uint32_t)log.open().ops.size();
#ifdef SL_DEV_MODE
    // Developer builds: now and then the same pose the second way, from the
    // base alone. The two must be the same bit for bit, whatever branches the
    // cache has seen.
    static uint32_t calls = 0;
    if ((++calls & 63) == 0 && !bitEqual(replayFromBase(log, slot), e.pose)) {
        devlog::logf(devlog::Cat::System, "world: ObjectCache slot %d comes out two ways (x %.9f / y %.9f cached)", slot,
                     e.pose.x, e.pose.y);
    }
#endif
    return e.pose;
}

Pose ObjectCache::poseAt(const OpLog& log, int slot, uint32_t tick) {
    if (!m_def || slot < 0 || (std::size_t)slot >= m_def->slots.size()) return Pose{};
    Pose p;
    std::size_t chunk = 0;
    if (!nearestBoundary(log, slot, tick, p, chunk)) p = base(slot);
    replay(p, log, slot, chunk, 0, tick, false);
    return p;
}

Pose ObjectCache::replayFromBase(const OpLog& log, int slot, uint32_t uptoTick) {
    if (!m_def || slot < 0 || (std::size_t)slot >= m_def->slots.size()) return Pose{};
    Pose p = base(slot);
    replay(p, log, slot, 0, 0, uptoTick, false);
    return p;
}

}  // namespace world
