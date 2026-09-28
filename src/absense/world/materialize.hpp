#pragma once

// The materializer (trigger design steps 8 and 9): puts the objects a run's
// log moves or toggles where the log has them, near the copies, in the real
// level's objects and collision buckets - and puts every byte back when the
// run ends or a paused search's slice is suspended.
//
// A run's ops (world/state.hpp) say where objects stand; the copies collide
// through the game's own checkCollisions 0x2137f0, which reads the objects'
// fields and the collision buckets (+0x35b0, world/step.cpp (b), (c)). So an
// object is written (position, last position, move marker, the dirty words,
// the toggle counter and flag) and, when its section changes, moved between
// buckets the way removeObjectFromSection 0x226d60 and addToSection 0x226500
// do - through SectionShadow, which never writes a buffer of the game's: every
// bucket, column and outer vector it changes is copied first, and restore puts
// the game's own buffers back. updateObjectSection is never called (it files
// the object in m_objectsToDeactivate and reorders +0x3598 for good).
//
// The window: checkCollisions walks the 3x3 sections around the section of the
// copy's position (0x2141ac-0x21429d, float position times the 0.01 factor,
// truncated). The design sized the window as the copy's rect grown by a
// section and 60 units; what the collision pass meets is decided by sections,
// not by rects, so the window here is sections: two either side of the copy's
// section before its update - the pass's range after an update that moves the
// copy less than a section. An object is written when its section now or its
// section in the log is in the window, or when it has extended collision (the
// list +0x35e0 is walked whole). A spider's window is every row of those
// columns: its jump (spiderTestJumpInternal 0x3943f0, from updateJump
// 0x38bbb0 inside its update) looks for the floor or ceiling with
// staticObjectsInRect 0x211260 over a rect that runs up or down to the mode's
// bounds, and that walks the buckets of every row the rect covers. A spider
// orb or pad, which any kind can take, catches every object up instead
// (catchUpCurrentRun), as a teleport does.
//
// Determinism: where an object belongs is a pure function of the run's base
// table (read at one tick, BaseTable), its log and its carries. What stands in
// the level between slices, or after a restore, is only ever compared with
// that, never used as a base.

#include <Geode/Geode.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "absense/world/def.hpp"
#include "absense/world/state.hpp"

namespace world {

struct Run;

// ------------------------------------------------------------ the level

// What the materializer derives from a WorldDef once: the slots it may write
// and how their groups hang together.
struct MaterialDef {
    std::shared_ptr<const WorldDef> def;
    // Collidable slots in a group some effect object names: the only objects a
    // run's ops can move that a copy can collide with. Ascending.
    std::vector<int> slots;
    std::vector<int> indexOf;   // a def slot's index into `slots`, -1 when it is not there
    std::vector<float> radius;  // per index: half the diagonal, the object radius or 15, whichever is largest
    // Per group: the distinct groups of its members in `slots` (itself
    // included) - the groups whose ops can move one of its members.
    std::vector<uint32_t> coOffsets;
    std::vector<uint16_t> coGroups;
    std::vector<uint8_t> mult;         // per group: the most times one slot is listed in one of its arrays
    std::vector<uint8_t> hasExtended;  // per group: a member in `slots` has extended collision

    std::span<const uint16_t> coGroupsOf(int group) const;
};
std::shared_ptr<const MaterialDef> materialDef(const std::shared_ptr<const WorldDef>& def);
// A level is set up or torn down (forgetLevel): the derived data goes with it.
void forgetMaterialDef();

// An object that moved on the step before a table was read, by something the
// World does not follow: carried on at that step's speed, as MovingObjects
// carries it (the fallback path), for as long as the run lasts.
struct Carry {
    int slot = -1;
    float dx = 0.0f;
    float dy = 0.0f;
};

// Where a run's objects stood at one tick of the real game.
struct BaseTable {
    std::shared_ptr<const MaterialDef> md;
    GJBaseGameLayer* pl = nullptr;
    // The layer's m_currentProgress and m_commandIndex when it was read, and
    // the count of real state changes then (noteRealStateChanged): the real
    // game has not moved an object while all three are the same. The two
    // counters alone come round again when the game is put back to a tick it
    // has played (a restore, a restart that dies on its first tick).
    uint32_t progress = 0;
    uint32_t commandIndex = 0;
    uint64_t epoch = 0;
    std::vector<Pose> poses;  // per MaterialDef index
    // Collidable objects outside MaterialDef::slots that an area effect or a
    // dynamic move had moved on the step before (their poses parallel).
    std::vector<int> extraSlots;
    std::vector<Pose> extraPoses;
    // Every object that moved on the step before (its marker is the index of
    // that step, see world/step.cpp (f)) by between 0.0005 and 40 units - the
    // bounds MovingObjects takes a step to be one tick of a move by.
    std::vector<Carry> movers;

    const Pose* find(int slot) const;
    // Where the group's members in the table stood, grown by their radius.
    const Box& groupBox(int group) const;
    // The slots whose position, last position, marker or toggle state differs
    // in the level now from the table (read when the real game has stepped).
    const std::vector<int>& liveDiff(uint32_t liveProgress, uint32_t liveCommandIndex) const;

   private:
    mutable std::unordered_map<int, Box> m_boxes;
    mutable bool m_diffValid = false;
    mutable uint32_t m_diffProgress = 0;
    mutable uint32_t m_diffIndex = 0;
    mutable uint64_t m_diffEpoch = 0;
    mutable std::vector<int> m_diff;
};

// The table of the tick the real game stands at (`liveProgress` and
// `liveCommandIndex` are the layer's own, read before a run changes them),
// shared by every run begun at that tick.
std::shared_ptr<const BaseTable> baseTable(GJBaseGameLayer* pl, const std::shared_ptr<const WorldDef>& def,
                                           uint32_t liveProgress, uint32_t liveCommandIndex);

// The table of a real tick the game has left: the table of now with the poses
// `undo` puts back (the ledger's, newest change first), the counters that tick
// stood at, and the movers worked out again from those poses. Its epoch is not
// any real one, so a run from it always compares itself with the level and
// writes what it holds back over what the game has done since.
std::shared_ptr<const BaseTable> rewoundTable(const BaseTable& now, uint32_t progress, uint32_t commandIndex,
                                              std::span<const std::pair<int, Pose>> undo);

// The real game has ticked or been put back (Trajectory::realStateChanged):
// the shared table of the last tick is let go, and a table read before this
// compares itself with the objects again even at the same progress and index.
void noteRealStateChanged();

// A copy is about to read the level far from its window (a spider orb or pad
// looking up or down the screen): every object the run of the tick in
// progress moves goes where it belongs first. Nothing for a run whose objects
// move the old way (MovingObjects), which stays as it was.
void catchUpCurrentRun();

// The movers of `table` a run starting from `ws` carries: a mover the state's
// running move commands follow is left to the ops, per axis (a move locked to
// the camera is followed on its other axis only).
std::shared_ptr<const std::vector<Carry>> carriedMovers(const BaseTable& table, const WorldState& ws,
                                                        const WorldDef& def);

// ------------------------------------------------------------ starts

// The World's part of a kept or carried start (design step 9).
struct WorldStart {
    // The state a run from the start begins from.
    std::shared_ptr<const WorldState> keyframe;
    int keyframeTick = 0;  // the real tick (updater frame) the keyframe stands at
    // Ticks from the keyframe to the start: the ledger keeps one keyframe for
    // a run of real ticks whose shape does not change (world/ledger.hpp), and
    // the start is that keyframe stepped this many ticks on.
    int offset = 0;
    // The lock inputs and step deltas of those ticks, index i for the tick
    // keyframeTick + 1 + i. Shared with the ledger and with every other start
    // of the same keyframe; only the first `offset` entries are ever read, and
    // those never change once written.
    std::shared_ptr<const std::vector<LockInput>> inputs;
    // The level the keyframe was taken from: what the step of those ticks
    // reads the groups and triggers from, and the read of it those slots
    // belong to. A start kept before the level was read again (the editor
    // between playtests) is not stepped: the step can ask where an object
    // stands, and the objects of that read may be gone.
    std::shared_ptr<const WorldDef> def;
    unsigned gen = 0;
    // The contacts the real players were in at the start's own tick. The
    // keyframe is of an earlier tick, and a step with no copies forgets the
    // contacts it was imported with (endTick ages out everything no copy
    // touched), so they are taken from the game when the start is captured
    // and put back on the stepped state.
    std::vector<std::pair<int, uint32_t>> contact[2];
    bool haveContact = false;
    // What the triggers of the plan that carried the start here did to the
    // objects since `base` was read (a start carried ahead), or null.
    std::shared_ptr<const OpLog> log;
    // Where the objects stood when the log starts, and what the run carries.
    std::shared_ptr<const BaseTable> base;
    std::shared_ptr<const std::vector<Carry>> carry;
    // The tick of the start's own state the table stands at (Materializer::
    // begin): a start stepped from a keyframe begins at the keyframe's offset
    // and its table is of the start's tick, so its first run carries a mover
    // one step and not offset + 1 of them.
    int baseTick = 0;
    // The keyframe leaves out the running actions (a kept start captured on
    // every real tick, WorldState::partial) or no table was read: its runs move
    // objects by the old path, as before the materializer.
    bool inexact = true;
    // A run has begun from the start. Only its first run may give it the full
    // World of its tick (Sim::begin): a start that changed what it holds after
    // runs had been made from it - the game put back to its tick by a restore -
    // would answer the same script two ways, and the pathfinder keeps the
    // answers of a start's runs.
    bool taken = false;

    // The state at the start: the keyframe stepped `offset` ticks with the
    // recorded inputs (world/ledger.cpp). Worked out once and kept, so every
    // run from the start begins from the very same bits however many runs
    // have been made from it and whatever the real game has done meanwhile.
    const WorldState& materialize() const;

   private:
    mutable std::shared_ptr<const WorldState> m_stepped;
};

// ------------------------------------------------------------ buckets

// An MSVC std::vector as the game lays it out: first, last, end of storage.
struct RawVec {
    char* first = nullptr;
    char* last = nullptr;
    char* end = nullptr;
};
// std::vector<bool>: its word vector and its size in bits.
struct RawBits {
    RawVec words;
    std::size_t bits = 0;
};
// Storage of the materializer's that a grow has replaced. It is freed when the
// shadow is put back, not when it is replaced: the copies' collision pass keeps
// the bucket it is walking over a whole call, and an orb or portal inside that
// call can send the run's objects through the shadow (world/materialize.cpp).
using Retired = std::vector<std::pair<char*, std::size_t>>;

// The collision buckets as checkCollisions reads them: m_nonEffectObjects
// (+0x35b0, vector of columns, a column a vector of buckets, a bucket a
// vector<GameObject*>), m_nonEffectObjectsSizes (+0x3658, a vector<int> per
// column) and m_nonEffectObjectsFlags (+0x3688, a vector<bool> per column).
struct SectionGrid {
    RawVec* buckets = nullptr;
    RawVec* counts = nullptr;
    RawVec* dirty = nullptr;
    float factorX = 0.01f;
    float factorY = 0.01f;

    static SectionGrid of(GJBaseGameLayer* pl);
};

// Moves objects between the collision buckets without writing any buffer of
// the game's: a bucket, a column's three vectors and the outer vectors are
// each copied into storage of its own (the game's operator new 0x4d0770) the
// first time anything in them changes, and restore frees the copies and puts
// the game's headers back. What is written in place - the vector headers and
// the moved objects' slots (+0x274 of every entry the game's buffers hold) - is
// saved and put back.
class SectionShadow {
   public:
    ~SectionShadow();
    void attach(const SectionGrid& grid);
    bool attached() const { return m_grid.buckets != nullptr; }

    // The section addToSection 0x226500 files a double position under
    // (world/step.cpp (b)); false for a position the game would file at a
    // negative or absurd section (a NaN), which is never moved.
    bool sectionOf(double x, double y, int& sx, int& sy) const;
    // The section checkCollisions takes a copy's float position to be in.
    void copySection(float x, float y, int& sx, int& sy) const;

    // removeObjectFromSection's +0x35b0 part (the last entry into the slot,
    // count - 1, the dirty bit), then addToSection's (into bucket[count] or
    // pushed, count + 1, the dirty bit, +0x274 / +0x278 / +0x27c). False,
    // with nothing changed, when the object is not where its slots say.
    bool move(GameObject* o, int sx, int sy);
    // The buckets in the sections given whose dirty bit is set: the copies'
    // collision pass sorts them by uid and rewrites their slots, which must
    // happen in a copy too.
    void shadowDirty(int x0, int x1, int y0, int y1);
    // Everything back as the game had it; the copies freed.
    void restore();
    // The images dropped without writing anything back: for a level that has
    // been torn down or set up again, whose vectors and objects may be gone.
    void forget();
    bool empty() const { return m_buckets.empty() && m_columns.empty() && !m_outer.saved; }
    std::size_t bucketCount() const { return m_buckets.size(); }

   private:
    struct OuterImage {
        bool saved = false;
        RawVec orig[3];
        std::size_t ownBytes[3] = {0, 0, 0};  // capacity of the copy (0: still the game's)
    };
    struct ColumnImage {
        int x = 0;
        bool created = false;  // null before: the headers are the materializer's
        RawVec* col = nullptr;
        RawVec* cnt = nullptr;
        RawBits* bits = nullptr;
        RawVec origCol, origCnt;
        RawBits origBits;
        std::size_t ownCol = 0, ownCnt = 0, ownWords = 0;
    };
    struct BucketImage {
        int x = 0;
        int y = 0;
        bool created = false;  // null before: the header is the materializer's
        RawVec* header = nullptr;
        RawVec orig;
        int origCount = 0;
        std::size_t own = 0;
    };

    RawVec& outer(int k) const;
    std::size_t outerSize(int k) const;
    bool touchOuter();
    bool growOuter(int x);
    ColumnImage* touchColumn(int x);
    bool growColumn(ColumnImage& c, int y);
    BucketImage* touchBucket(int x, int y);
    static bool readable(const RawVec& v, std::size_t elem);

    SectionGrid m_grid;
    OuterImage m_outer;
    std::vector<ColumnImage> m_columns;
    std::vector<BucketImage> m_buckets;
    std::unordered_map<int, uint32_t> m_columnOf;
    std::unordered_map<uint64_t, uint32_t> m_bucketOf;
    Retired m_retired;
};

// ------------------------------------------------------------ the materializer

class Materializer {
   public:
    ~Materializer();
    // A new run over `table` (the base of every object it writes) carrying
    // `carry`. `liveProgress` / `liveCommandIndex` are the layer's own now:
    // when the table is of another tick, the objects the real game has moved
    // since are put where the table has them on the first materialize.
    // `baseTick` is the run's own tick number the table's poses stand at: a
    // carried mover is one step further on for every tick past it, and a run
    // does not always begin at tick 0 - a start stepped from a keyframe begins
    // at the keyframe's offset, and a start carried ahead by a plan begins
    // where the plan left off over the plan's own base.
    void begin(GJBaseGameLayer* pl, std::shared_ptr<const BaseTable> table, std::shared_ptr<const std::vector<Carry>> carry,
               uint32_t liveProgress, uint32_t liveCommandIndex, int baseTick);
    bool active() const { return m_table != nullptr; }
    const std::shared_ptr<const BaseTable>& table() const { return m_table; }
    const std::shared_ptr<const std::vector<Carry>>& carry() const { return m_carry; }
    int baseTick() const { return m_baseTick; }

    // Before `copy`'s tick, every object in its window stands
    // where the run's log has it.
    void materializeNear(Run& run, PlayerObject* copy);
    // Every object the run moves, wherever it is (a copy is about to teleport).
    void catchUpAll(Run& run);
    // A snapshot was put back: the written objects and every group are looked
    // at again on the next materializeNear.
    void invalidate();
    // A paused search resumes after real ticks: what the real game moved since
    // the table is put back on the next materializeNear.
    void resume(uint32_t liveProgress, uint32_t liveCommandIndex);
    // World::restoreWritten: every object field and bucket written goes back
    // as it was, in reverse; nothing is registered afterwards.
    void restoreWritten();
    // The run is over: restoreWritten, and the table let go.
    void end();

    std::size_t writtenCount() const { return m_written.size(); }

   private:
    struct Orig {
        double x = 0.0, y = 0.0;
        cocos2d::CCPoint node;
        cocos2d::CCPoint last;
        uint32_t marker = 0;
        int32_t counter = 0;
        bool disabled = false;
        int32_t slot = 0, secX = 0, secY = 0;
        std::array<uint8_t, 0x2a> rects{};
        OBB2D* box = nullptr;
        bool useOuterOb = false;
        std::array<uint8_t, 0x78> boxBytes{};
    };
    struct Written {
        GameObject* o = nullptr;
        int slot = -1;
        Orig orig;
        Pose pose;
    };
    struct Window {
        int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
        bool operator==(const Window&) const = default;
    };

    // The run is over the level the table was read from (a start kept before
    // the level was read again holds a table of other slots), and that level
    // is still set up: after moverCacheInvalidate the table's objects may be
    // gone (the editor deletes them between playtests).
    bool sameLevel(const Run& run) const;
    bool tableCurrent() const;
    Pose desired(Run& run, int slot);
    void refresh(Run& run, int slot, const Window* window);
    void write(Written& w, const Pose& p, uint8_t flags);
    void restoreOne(Written& w);
    // The sections the copy's collision pass can reach, from where it stands
    // before its update.
    Window windowOf(PlayerObject* copy) const;
    static bool inWindow(const Window& w, int sx, int sy) { return sx >= w.x0 && sx <= w.x1 && sy >= w.y0 && sy <= w.y1; }
    bool boxMeets(const Box& box, double rx, double ry, const Window& w) const;
    bool pointMeets(double x, double y, double rx, double ry, const Window& w) const;
    // A carried object refresh would leave where it is, told from the table
    // alone (see materializeNear).
    bool carriedAway(const Run& run, const Carry& c, const Window& w) const;
    void pendDiff(uint32_t liveProgress, uint32_t liveCommandIndex);

    GJBaseGameLayer* m_pl = nullptr;
    // The layer's section factors, read once: the window tests below run for
    // every member of every group a run has moved, on every tick of every copy.
    double m_fx = 0.01;
    double m_fy = 0.01;
    std::shared_ptr<const BaseTable> m_table;
    // The run tick the table's poses stand at (see begin).
    int m_baseTick = 0;
    std::shared_ptr<const std::vector<Carry>> m_carry;
    std::unordered_map<int, uint32_t> m_carryOf;
    // Which carry list m_carryOf is the index of. Held, not just pointed at:
    // a list that had been let go of could be freed and a different one built
    // at the same address, and the index would silently be of the wrong list.
    std::shared_ptr<const std::vector<Carry>> m_carryOfFor;
    SectionShadow m_shadow;
    std::vector<Written> m_written;
    std::unordered_map<int, uint32_t> m_writtenOf;
    // The slots the real game moved away from the table, written back on the
    // next materialize wherever they are.
    std::vector<int> m_pending;
    bool m_pendingPass = false;
    bool m_stale[2] = {true, true};
    bool m_haveWindow[2] = {false, false};
    Window m_window[2];
    std::unordered_map<int, uint64_t> m_memo[2];
};

// The self-test of SectionShadow on a grid of its own (world/selftest.cpp):
// null when random moves, dirty-bucket sorts and a restore leave every header,
// buffer and slot as they were, and every step matches the game's bucket rules.
const char* checkSectionShadow();

}  // namespace world
