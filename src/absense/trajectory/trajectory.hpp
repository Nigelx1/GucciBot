#ifndef TRAJECTORY_TRAJECTORY_HPP
#define TRAJECTORY_TRAJECTORY_HPP

#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "absense/compat/settings.hpp"
#include "absense/compat/settings.hpp"
#include "absense/world/world.hpp"
#include "Geode/cocos/cocoa/CCGeometry.h"
#include "Geode/cocos/platform/CCPlatformMacros.h"

struct SavedPlayerCheckpoint;

// Throws away the cached scan of the moving objects (it holds raw objects of
// one level): called when a level is set up or torn down.
void moverCacheInvalidate();
// Which set-up of a level the per-level caches belong to: moverCacheInvalidate
// moves it on (the World's WorldDef is rebuilt when it does).
unsigned moverCacheGeneration();
// Puts every moving object of the run in progress where it belongs at the run's
// tick, near the copies or not: called before a copy teleports. A run whose
// objects the World puts in place catches all of them up through its
// materializer (world/materialize.hpp); any other through MovingObjects.
void moversCatchUp();

class TrajectoryDrawNode : public cocos2d::CCDrawNode {
   public:
    static TrajectoryDrawNode* create() {
        TrajectoryDrawNode* node = new TrajectoryDrawNode();

        if (node && node->init()) {
            node->autorelease();
            node->m_bUseArea = false;
        } else {
            CC_SAFE_DELETE(node);
        }

        return node;
    }
};

struct TrajectoryPlayerData {
    cocos2d::CCPoint position;
    cocos2d::CCRect hitbox;
    cocos2d::CCRect innerHitbox;
    float rotation;
    bool p1;
    bool holding;

    int score;
};

struct TrajectoryState {
    SLValuePtr<bool> m_enabled = SLValue<bool>::create(
        "trajectory.enabled", &SLSettings::get()->trajectory.enabled);
    SLValuePtr<double> m_width = SLValue<double>::create(
        "trajectory.width", &SLSettings::get()->trajectory.width);
    SLValuePtr<double> m_length = SLValue<double>::create(
        "trajectory.length", &SLSettings::get()->trajectory.length);
};

struct PredictionConfig {
    bool m_bypassConfig = false;
    int m_maxLength = 10'000'000;
    double m_overridenTPS = 0.0;
    bool m_silent = false;  // no drawing (the pathfinder's runs)
};

// One tick of input for Trajectory::run: `presses` pushes on this tick
// (each one preceded by a release when the button is already down) and
// whether the button is down at the end of the tick.
struct TickInput {
    uint8_t presses = 0;
    bool held = false;
};

struct RunResult {
    int survived = 0;        // ticks survived whole; the death, if any, is in tick survived + 1 (all of them when none)
    bool died = false;
    bool complete = false;   // reached the end of the level
    float x = 0.0f;          // final position
    float y = 0.0f;
    float minY = 0.0f;       // lowest / highest point along the way
    float maxY = 0.0f;
    bool goingLeft = false;  // the copy was travelling towards smaller x at the end
    int simulated = 0;       // ticks simulated to get this (a search runs many)
    int branches = 0;        // branch points a search tried
    bool dualDeath = false;  // the death was player 2's (dual mode)
    // With separate controls (two-player mode): whether the other copy,
    // following its own script, died, and after how many ticks.
    bool otherDied = false;
    int otherSurvived = 0;
};

// One tick of a run, for callers that want the whole path (the MCP tools).
struct TraceSample {
    int tick = 0;            // 1-based: state after this many ticks
    float x = 0.0f;
    float y = 0.0f;
    float yVel = 0.0f;
    float xVel = 0.0f;
    float rotation = 0.0f;
    bool onGround = false;
    bool held = false;       // jump button down after this tick's inputs
    bool dead = false;
    bool dual = false;       // the second player was simulated too
    float x2 = 0.0f;         // its position (dual mode)
    float y2 = 0.0f;
};

class Trajectory {
   public:
    using TrajectoryMode = SLSettings::TrajectorySettings::Mode;

    // Hold, Swift, Release, Double, Tap and DoubleHeld are exclusive click
    // kinds. Every kind's bit has to be in here: the settings are looked up by
    // `mode & CLICK_MASK`, and a bit left out would look up (and create) an
    // empty entry, so that kind's line would never be drawn.
    constexpr static int CLICK_MASK = 0b111 | SLSettings::TrajectorySettings::Mode::Double |
                                      SLSettings::TrajectorySettings::Mode::Tap |
                                      SLSettings::TrajectorySettings::Mode::DoubleHeld;
    constexpr static int DIRECTION_MASK = 0b11000;

   private:
    cocos2d::CCRenderTexture* m_renderTex;
    TrajectoryDrawNode* m_node;

   public:
    PlayerObject* m_fakePlayer1;
    PlayerObject* m_fakePlayer2;

   private:
    std::unordered_set<uintptr_t> m_activatedObjectsP1;
    std::unordered_set<uintptr_t> m_activatedObjectsP2;
    // The run's own copy of the replay system's teleport random state: a group
    // teleport in a run draws from it (phys::teleportPlayer), and snapshots and
    // kept starts carry it, so the same script lands at the same target.
    uint64_t m_teleportRand = 0;

    struct TrajectoryAction {
        int m_delay;
        std::function<void()> m_func;
        bool m_executed = false;
    };
    std::vector<TrajectoryAction> m_actions;

    uint64_t m_lastFrame = 0;
    bool m_calculated = false;

    struct Signature {
        uint32_t frame = 0;
        // x, y, rotation, yVelocity, platformerXVelocity, gravityMod, size
        float p1[7] = {};
        uint64_t p1flags = 0;
        float p2[7] = {};
        uint64_t p2flags = 0;
        float timeWarp = 0.0f;
        float cameraZoom = 0.0f;
        float width = 0.0f;
        float length = 0.0f;
        uint32_t boolPack = 0;
        size_t categoriesHash = 0;

        bool operator==(const Signature&) const = default;
    };
    Signature m_lastSignature;

    bool m_drawing;
    // True while a simulated tick of the copies runs (Sim::step): the hooks
    // keep the game's own code from reporting to the real level then.
    bool m_simulating = false;
    // The breakable blocks a copy broke in the run in progress. A branch puts
    // the list back with the rest of the run; the blocks are hidden only while
    // a simulated tick runs (disabled, disabled2: what the object had before),
    // because the real game ticks between the slices of a paused search.
    struct SimBroken {
        GameObject* object = nullptr;
        bool disabled = false;
        bool disabled2 = false;
    };
    std::vector<SimBroken> m_simBroken;
    bool m_deadP1;
    bool m_deadP2;

    // Part of the rebuild signature. The hook meant to set them
    // (hooks/PlayerObject.cpp) modifies a PlayerObject::handleButton that
    // 2.2081 does not have, so it never runs and they keep what they start with.
    bool m_p1Holding = false;
    bool m_p2Holding = false;

    float m_delta = 0.0;
    float m_lastRealDelta = 0.0f;
    float m_realDeltaSum = 0.0f;
    float m_realDeltaAvg = 0.0f;
    int m_realDeltaCount = 0;

    // A scripted run of the copies (one player, or both in dual mode):
    // set-up, one tick with an input, snapshots so a branch can be undone.
    struct SimSnapshot;
    struct Sim;
    struct SearchState;
   public:
    struct SimStart;  // see captureStart
   private:
    SearchState* m_search = nullptr;  // a search in progress (beginSearch)
    // Groups a trigger that moves things targets (move, rotate, follow, scale,
    // keyframe, advanced follow, area effects): the objects the moving-object
    // scan watches from one real tick to the next, for the runs that carry
    // them that way (MovingObjects). Scanned once per level. Whether a killer
    // can be somewhere else is world::certaintyOf's answer now, which asks the
    // level's own read rather than this list of target groups.
    std::unordered_set<int> m_movedGroups;
    GJBaseGameLayer* m_movedFor = nullptr;
    int m_movedCount = -1;
    void scanMovedGroups(GJBaseGameLayer* pl);
    void dropSearch();
    // The level-load check of design step 15: one fixed script run straight,
    // and the same script run with a snapshot every seventh tick and a branch
    // thrown away from each, must come out the same tick for tick. Runs once
    // per read of the level, the first tick everything it needs is there, and
    // turns the World off (never the level) when the two differ.
    void checkWorldBranches(GJBaseGameLayer* pl);
    unsigned m_branchCheckGen = ~0u;  // the level read the check has run for
    uint64_t m_branchCheckFrame = 0;  // the real tick the last look at it was made on

    // The kind of player a copy's node was last set up as. The checkpoint
    // only carries the flags (m_isDart...); the hitbox and the rest of the
    // node come from the game's own mode switches, so a copy set up as a
    // wave from flags alone still has a cube's hitbox.
    int m_fakeNodeMode[2] = {0, 0};

   public:
    enum FakeMode { kModeCube = 0, kModeShip, kModeBall, kModeBird, kModeDart, kModeRobot, kModeSpider, kModeSwing };
    static int modeOf(const PlayerObject* p);
    static int modeOf(const SavedPlayerCheckpoint& c);
    // Switches the copy's node to `mode` through the game's own toggles
    // (nothing when it is that already).
    void setFakeMode(PlayerObject* fake, int mode);
    // The simulated portal code switched the copy itself.
    void noteFakeMode(PlayerObject* fake, int mode);

    // A copy crossed a dual (toDual) or solo portal: the run in progress adds
    // or drops the copy of the other player the way GJBaseGameLayer::
    // toggleDualMode 0x2168d0 does, right here inside the collision pass.
    // Nothing happens outside a scripted run - the drawn prediction keeps the
    // copies it began with, as every run did before.
    void dualPortal(GJBaseGameLayer* pl, PlayerObject* copy, EffectGameObject* portal, bool toDual);

    // Rings (orbs) and contact: the game activates a ring at most once per
    // contact, however many presses come while the copy stays on it (the
    // same on an orb marked "multi activate"). Contact is tracked per
    // simulated tick: the collision pass notes it, a ring jump asks first.
    void noteRingContact(PlayerObject* p, GameObject* ring);
    bool ringUsedThisContact(PlayerObject* p, GameObject* ring);
    void noteRingUsed(PlayerObject* p, GameObject* ring);
    // The copy was just set up from a player that is on a ring right now:
    // a ring it has already activated during this contact counts as used
    // for the copy too (the game will not fire it again until the player
    // has left it), so a press on it in the simulation does nothing, the
    // way it does nothing in the game.
    void seedRingContacts(PlayerObject* copy);

    // What killed a copy last (diagnostics): the object's id, kind and
    // position, and its rects at the time, or id 0 when it was not an object.
    struct Killer {
        bool valid = false;
        int id = 0;
        int uid = 0;  // the object itself (m_uniqueID); id is only its kind, shared by every spike of that kind
        int type = 0;
        float x = 0.0f, y = 0.0f;
        float rot = 0.0f;
        bool outerOb = false;
        cocos2d::CCRect rect;         // the object's rect
        cocos2d::CCRect playerRect;   // the copy's rect
        cocos2d::CCRect playerInner;  // the copy's inner rect
        float playerRot = 0.0f;
        int tick = 0;
        // How well the run follows it (world::certaintyOf): nothing in the
        // level can move it, the World models everything that can, or it may
        // be somewhere else in the real game than the run had it.
        using Certainty = world::Certainty;
        Certainty certainty = Certainty::Uncertain;
        // Whether the object can be somewhere else in the real game. It is the
        // uncertain case of `certainty`; it used to be "it has moved from where
        // it started, or some trigger in the level names a group of it", which
        // called every object of a group a move trigger targets movable even
        // where the run runs that trigger itself.
        bool movable = false;
    };
    // A spot the real game killed the player at although the simulation saw
    // nothing there, and the real tick it happened on (see addPhantom).
    struct Phantom {
        cocos2d::CCRect rect;
        int realTick = 0;  // the updater frame the real game died on
    };

    const Killer& lastKiller() const { return m_killer; }
    void noteKiller(PlayerObject* p, GameObject* object);
    void clearKiller() { m_killer = Killer{}; }
    void clearRingContacts() {
        for (auto& m : m_ringTouch) m.clear();
        for (auto& m : m_ringUsed) m.clear();
    }

   private:
    std::unordered_map<uintptr_t, unsigned> m_ringTouch[2];  // ring -> last simulated tick in contact
    std::unordered_map<uintptr_t, unsigned> m_ringUsed[2];   // ring -> tick it was activated on
    Killer m_killer;
    std::shared_ptr<SimStart> m_start;  // runs begin from this instead of the real player (see useStart)
    std::vector<Phantom> m_phantoms;
    // The real tick the run's current tick stands at (the start's moveTick
    // plus the ticks run since), which is what a phantom's window is measured
    // against. -1 outside a run.
    int m_simRealTick = -1;
    // Half the window, in ticks: max(2, tps / 30), worked out once a real tick.
    int m_phantomWindow = 2;
    bool hitsPhantom(PlayerObject* p) const;
    // Activation flags cleared on objects ahead of the start while it is in
    // use (object, player 1 flag, player 2 flag), put back by useStart(nullptr).
    struct ClearedFlag {
        EnhancedGameObject* object;
        bool p1, p2;
    };
    std::vector<ClearedFlag> m_clearedFlags;
    GJBaseGameLayer* m_clearedFor = nullptr;  // the layer those objects belong to

    // ---- Drawing the prediction. Everything below belongs to the drawn lines
    // (update, simulate, runPrediction and the drawing half of iterate). A Sim
    // never reaches any of it: its runs are silent (PredictionConfig::m_silent),
    // and iterate only draws for a run that is not.

    // One draw call of a line, kept so update() can draw the same geometry
    // again in another click kind's colour (drawLines). A segment takes the
    // kind's colour on the copy of player 1 and the negated one on the copy of
    // player 2, as iterate has always drawn them; a hitbox takes the hitbox
    // settings' colours, whatever the kind.
    struct DrawOp {
        bool hitbox = false;
        bool firstCopy = true;        // drawn for m_fakePlayer1
        float width = 0.0f;           // a segment's radius, or a hitbox's line width
        cocos2d::CCPoint a, b;        // a segment's ends
        cocos2d::CCRect rect, inner;  // a hitbox's rects, as drawHitbox makes them
        float rotation = 0.0f;        // a hitbox's rotation
    };
    // Set while update() records a line: its draw calls are kept here instead
    // of being drawn.
    std::vector<DrawOp>* m_recordOps = nullptr;
    std::vector<DrawOp> m_lineOps;  // the recording's storage, kept from one rebuild to the next
    // Whether the line being recorded may merge its segments. That depends on
    // the colour its segments end up drawn in, which is the colour of the kind
    // drawn on top, not the recorded kind's own (see drawLines).
    bool m_recordMerge = false;
    unsigned m_recordedGroups = 0;  // recorded groups so far (how often drawLines checks a recording)

    // A run of consecutive per-tick segments of one copy, drawn as the one
    // segment from its start to its last point (runPrediction, for update()'s
    // rebuilds only). A tick moves the player a pixel or less at high TPS, so a
    // line was thousands of segments of 18 vertices each, all uploaded again
    // after every rebuild. A run keeps growing only while that one segment
    // stays within m_mergeEps of every point it stands for and the path keeps
    // moving away from where the run began; anything else ends it and starts
    // the next.
    struct SegmentRun {
        bool active = false;
        cocos2d::CCPoint a, p;  // the run's start and its last point
        float reach = 0.0f;     // |p - a|; a point closer to a than this ends the run
        // The directions a segment from a may take and still pass within eps of
        // every point so far: unit vectors, lo to hi counter-clockwise. Unset
        // until a point lies further than eps from a.
        bool coned = false;
        cocos2d::CCPoint lo, hi;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;  // the run's points' bounding box

        void start(const cocos2d::CCPoint& from, const cocos2d::CCPoint& to, float eps);
        bool extend(const cocos2d::CCPoint& to, float eps);
        void narrow(float dx, float dy, float d, float eps);
        bool meets(const cocos2d::CCPoint& from, const cocos2d::CCPoint& to, float pad) const;
    };
    SegmentRun m_runs[2];            // the copy of player 1's, player 2's
    bool m_mergeRuns = false;        // the runPrediction in progress merges its segments
    float m_mergeEps = 0.0f;         // a tenth of a screen pixel in level units, this rebuild's (0: no merging)
    float m_runWidth = 0.0f;         // the per-tick segments' radius
    float* m_runColors = nullptr;    // the colours of the line in progress

    // What a simulate() cost (devlog Cat::Trajectory only): the times are split
    // into the set-up (and taking it down again), the simulated ticks and the
    // drawing, which happens inside the ticks.
    struct LineProfile {
        bool on = false;
        int depth = 0;           // draw timers running (only the outermost one counts)
        int64_t drawNs = 0;      // drawing, or recording the draw calls
        int64_t loopNs = 0;      // runPrediction's tick loop, the drawing in it included
        int64_t loopDrawNs = 0;  // the part of drawNs spent inside that loop
        int ticks[2] = {0, 0};   // ticks simulated by the driven copy and by the other one
        int segments = 0;
        int hitboxes = 0;
    };
    LineProfile m_prof;
    // ... and what a whole rebuild of update() cost.
    struct RebuildProfile {
        int lines = 0;     // simulate() runs
        int replayed = 0;  // lines drawn from another line's recording
        int segments = 0;  // segments drawn, the replayed ones included
        int64_t setupNs = 0, simNs = 0, drawNs = 0, replayNs = 0, checkNs = 0;
    };
    RebuildProfile m_rebuildProf;

    // Whether simulate() would draw this line (its kind is on and, in a
    // platformer level, the player holds its direction).
    bool wants(GJBaseGameLayer* pl, bool p1, int mode);
    // Every line of one player, in the order update() has always drawn them.
    // `hold` and `release` receive those two kinds' results.
    void drawLines(GJBaseGameLayer* pl, bool p1, bool clickBothPlayers, TrajectoryPlayerData* hold,
                   TrajectoryPlayerData* release);
    void drawOps(const std::vector<DrawOp>& ops, float* colors, bool hitboxesOnly);
    void emitSegment(const cocos2d::CCPoint& a, const cocos2d::CCPoint& b, float width, bool firstCopy,
                     float* colors);
    void tickSegment(PlayerObject* copy, const cocos2d::CCPoint& from, const cocos2d::CCPoint& to, float* colors);
    void emitHitbox(PlayerObject* copy);
    void drawHitboxRects(cocos2d::CCRect& rect, cocos2d::CCRect& inner, float rotation, float width);
    void addToRun(int i, const cocos2d::CCPoint& from, const cocos2d::CCPoint& to);
    void flushRun(int i);
    void flushRuns();
    float mergeTolerance();

    Trajectory() = default;

   public:
    TrajectoryState* m_state;

    bool drawing() const { return m_drawing; }
    bool simulating() const { return m_simulating; }
    // A simulated copy broke this block (the destroyObject hook): it is hidden
    // for the rest of this tick and listed so the run's later ticks skip it
    // too, until a restore or the end of the run drops it.
    void simBreak(GameObject* o) {
        if (!o) return;
        for (const SimBroken& b : m_simBroken)
            if (b.object == o) return;
        m_simBroken.push_back({o, o->m_isDisabled, o->m_isDisabled2});
        o->m_isDisabled = true;
        o->m_isDisabled2 = true;
    }
    bool enabled() const { return m_state->m_enabled->inner(); }
    void setEnabled(bool enabled) {
        m_state->m_enabled->inner() = enabled;
        m_state->m_enabled->notifyChange();
    }
    void setDelta(float delta) {
        m_delta = delta;
        // Remember what the real game passes, so the simulation can be
        // compared with it (and use it) instead of a value computed from
        // the TPS.
        m_lastRealDelta = delta;
        m_realDeltaSum += delta;
        m_realDeltaCount++;
        if (m_realDeltaCount >= 256) {
            m_realDeltaAvg = m_realDeltaSum / (float)m_realDeltaCount;
            m_realDeltaSum = 0.0f;
            m_realDeltaCount = 0;
        }
    }
    float lastRealDelta() const { return m_lastRealDelta; }
    float realDeltaAverage() const { return m_realDeltaAvg; }

    PlayerObject* getOtherPlayer(PlayerObject* player) {
        if (player == m_fakePlayer1) return m_fakePlayer2;
        return m_fakePlayer1;
    }

    void scheduleAction(std::function<void()> action, int delay) {
        m_actions.push_back(TrajectoryAction{
            .m_delay = delay,
            .m_func = action,
        });
    }

    bool hasDied(PlayerObject* player) {
        if (player == m_fakePlayer1) {
            m_deadP1 = true;
            return true;
        }

        if (player == m_fakePlayer2) {
            m_deadP2 = true;
            return true;
        }

        return false;
    }

    bool isFakePlayer(PlayerObject* player) {
        return player == m_fakePlayer1 || player == m_fakePlayer2;
    }

    void handleButton(bool p1, bool holding) {
        if (p1) {
            m_p1Holding = holding;
        } else {
            m_p2Holding = holding;
        }
    }

    PlayerObject* createFakePlayer(std::string&& id) {
        GJBaseGameLayer* pl = GJBaseGameLayer::get();

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        player->retain();
        player->setPosition({0, 105});
        // player->setVisible(false);
        player->setID(id);
        pl->m_objectLayer->addChild(player);

        return player;
    }

    static Trajectory* create() {
        Trajectory* t = new Trajectory();

        t->m_node = TrajectoryDrawNode::create();
        t->m_node->retain();
        t->m_node->setID("trajectory-node"_spr);
        t->m_node->setBlendFunc(cocos2d::ccBlendFunc{770, 771});

        t->m_fakePlayer1 = t->createFakePlayer("trajectory-fake-player1"_spr);
        t->m_fakePlayer2 = t->createFakePlayer("trajectory-fake-player2"_spr);

        GJBaseGameLayer* pl = GJBaseGameLayer::get();
        pl->m_debugDrawNode->getParent()->addChild(
            t->m_node, pl->m_uiLayer->getZOrder() + 10000);

        // auto visibleSize = cocos2d::CCDirector::get()->getVisibleSize();

        // t->m_renderTex = cocos2d::CCRenderTexture::create(visibleSize.width,
        // visibleSize.height); t->m_renderTex->setPosition(visibleSize / 2);
        // t->m_renderTex->setAnchorPoint(cocos2d::CCPoint(0.5, 0.5));
        // t->m_renderTex->setVisible(true);
        // pl->m_debugDrawNode->getParent()->addChild(t->m_renderTex,
        // pl->m_debugDrawNode->getZOrder());

        return t;
    }

    int getPredictionLength();

    void invalidateCache() {
        m_lastFrame = UINT64_MAX;
        m_calculated = false;
    }
    Signature computeSignature(GJBaseGameLayer* pl);
    bool iterate(GJBaseGameLayer* pl, PlayerObject* player, int mode,
                 float* colors, bool& hasHeld, int& stepCount,
                 PredictionConfig config);

    // Runs `inputs` (one entry per tick; the last entry repeats for the
    // remaining `ticks`) on a copy of the player with the mod's physics and
    // reports how far it got. Draws nothing and restores the game state,
    // so it is safe from anywhere the death prevention runs (the physics
    // update and the frame loop). `buttonDown` is the real button state.
    // In a two-player level's dual part the other player has controls of
    // its own: `other` is its script for the run (empty: it keeps its
    // button as it is), `otherDown` its button now, and the run ends on the
    // driven copy's death only - unless `eitherDeath`.
    RunResult run(GJBaseGameLayer* pl, bool p1, std::span<const TickInput> inputs, int ticks, bool buttonDown,
                  std::vector<TraceSample>* trace = nullptr, std::span<const TickInput> other = {},
                  bool otherDown = false, bool eitherDeath = false);

    // Steers a copy of the player tick by tick instead of following a
    // script: it keeps the current input unless a short simulated
    // look-ahead (`lookahead` ticks) says that dies, then switches (hold or
    // release; the cube kinds also try a tap) - the way a wave zigzags
    // through a corridor or a ship rides under a ceiling. The inputs it
    // chose come back in `script`, one per tick run. Draws nothing and
    // restores the game state, like run().
    //
    // GucciBot: with `aimOffset` set (not NaN), once the copy flies it holds a
    // height instead - the height it started flying at plus the offset - the
    // way a person flies a ship through a gap. Until then it steers as above.
    RunResult steer(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int lookahead,
                    std::vector<TickInput>& script, std::span<const TickInput> other = {}, bool otherDown = false,
                    float aimOffset = std::numeric_limits<float>::quiet_NaN());

    // Searches for a script that survives `ticks`: runs the copy with no
    // change of input until it dies, then branches at the ticks before the
    // death (latest first: a tap or a hold for the cube kinds, a switch for
    // the flying kinds, a flap for the UFO) and carries on from there,
    // backing up to earlier branch points when a branch runs out - so a run
    // of hard timings comes out as one script. Stops after `budget`
    // simulated ticks and returns the longest path found. In dual mode both
    // players are simulated and either death counts.
    RunResult search(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget,
                     std::vector<TickInput>& script, std::span<const TickInput> other = {}, bool otherDown = false);

    // The same search in slices, so a caller can spread it over frames:
    // beginSearch() sets it up, each stepSearch() runs up to `maxTicks` more
    // simulated ticks and returns true once the search is over, and
    // finishSearch() hands back the result and the script (it can be called
    // early to stop). The game is left as it was between slices.
    // `widen` (a widened spot, see AbsensePathfinder::escalate): every tick is a
    // branch point for 8 more 240-TPS ticks before a death a level, and a
    // branch may start 128 more before it, up to three levels.
    bool beginSearch(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget,
                     std::span<const TickInput> other = {}, bool otherDown = false, std::span<const TickInput> base = {},
                     int widen = 0);
    // Given a deadline, the clock is looked at every maxTicks and the search
    // goes on in place; it is paused only once the time is up.
    bool stepSearch(int maxTicks, std::chrono::steady_clock::time_point until = {});
    RunResult finishSearch(std::vector<TickInput>& script);
    bool searching() const { return m_search != nullptr; }

    // Whether run() / steer() / search() simulate both players right now
    // (dual mode), and whether the two have controls of their own
    // (two-player mode), in which case the other copy follows its own script.
    bool simulatesBoth(GJBaseGameLayer* pl, bool p1) const;
    bool separateControls(GJBaseGameLayer* pl) const;

    // Spots the real game killed the player at although the simulation saw
    // nothing there (a block that moves, a trigger, anything it does not
    // model): a copy whose hitbox reaches one dies in the simulation too,
    // so the same path is not approved again. Rects in level units.
    //
    // A spot belongs to the real tick it happened on, and a copy only dies in
    // it while it is passing through that moment: whatever moved into the
    // player there was somewhere else a second earlier and will be somewhere
    // else a second later, and a spot that kills at every tick walls the
    // corridor off for the rest of the attempt. Only a death at an object the
    // World cannot follow (world::Certainty::Uncertain) leaves one at all -
    // the rest the search already learns from as confirmed killers.
    void addPhantom(const cocos2d::CCRect& rect, int realTick);
    void dropPhantomsBefore(float x);
    void clearPhantoms() { m_phantoms.clear(); }
    size_t phantomCount() const { return m_phantoms.size(); }
    const std::vector<Phantom>& phantoms() const { return m_phantoms; }

    // Somewhere else to start from: the players' state as it is now, kept
    // so a later run() / steer() / search() can begin from it instead of
    // from where the real player is by then (the pathfinder keeps one per
    // tick it played, and looks at a dead end from earlier ticks without
    // the game going back first). useStart() makes the runs begin from it
    // until useStart(nullptr); objects ahead of it that the real player
    // has activated since are treated as untouched meanwhile.
    struct SimStart;
    std::shared_ptr<SimStart> captureStart(GJBaseGameLayer* pl, bool p1);
    void useStart(std::shared_ptr<SimStart> start);
    // Drops the cached checkpoint of the real player: called whenever the
    // real game runs a tick or is put back.
    void realStateChanged();
    // The game reset every flag itself: what a start had cleared must not come back.
    void forgetClearedFlags() {
        m_clearedFlags.clear();
        m_clearedFor = nullptr;
    }
    // The state `inputs` later than `from`, as a start of its own - so a
    // decision can be made about where a plan ends while the plan is still
    // being played. Nothing when the script dies before its end.
    std::shared_ptr<SimStart> advanceStart(GJBaseGameLayer* pl, std::shared_ptr<SimStart> from,
                                           std::span<const TickInput> inputs, std::span<const TickInput> inputs2);
    bool usingStart() const { return m_start != nullptr; }
    // The button state and the kind of player of a kept start (and of the
    // other player, when it was kept too).
    static bool startHeld(const SimStart& s);
    static bool startFlying(const SimStart& s);
    static float startX(const SimStart& s);
    static int startCount(const SimStart& s);
    // Everything the graphics-card model needs about a kept start's player.
    struct StartState {
        float x = 0.0f, y = 0.0f, yVelocity = 0.0f;
        float gravity = 0.0f, gravityMod = 1.0f, yStart = 0.0f;
        float playerSpeed = 1.0f, speedMultiplier = 1.0f, vehicleSize = 1.0f;
        int mode = 0;
        bool upsideDown = false, onGround = false, held = false;
    };
    static StartState startState(const SimStart& s, bool second = false);
    // The same for the live player.
    static StartState liveState(GJBaseGameLayer* pl, bool p1);
    static bool startHeld2(const SimStart& s);
    static bool startFlying2(const SimStart& s);

    // Hitbox sizes of the real player and of its simulated copy after the
    // copy was set up the way run() does it (a diagnostic: they must agree).
    struct HitboxCheck {
        cocos2d::CCSize real, realInner, fake, fakeInner;
        float realScale = 1.0f, fakeScale = 1.0f;
        bool realDart = false, fakeDart = false;
        float realSize = 1.0f, fakeSize = 1.0f;
    };
    HitboxCheck checkHitboxes(GJBaseGameLayer* pl, bool p1);

    TrajectoryPlayerData runPrediction(GJBaseGameLayer* pl,
                                       PlayerObject* player,
                                       PlayerObject* other, int mode,
                                       float* colors, bool both,
                                       PredictionConfig config);
    TrajectoryPlayerData simulate(GJBaseGameLayer* pl, bool p1, int mode,
                                  bool clickBothPlayers,
                                  PredictionConfig config = PredictionConfig());
    void update(GJBaseGameLayer* pl);
    void handlePortal(PlayerObject* player, GameObject* object);
    void drawHitbox(PlayerObject* player);

    // ---- GucciBot additions (2026-10-03), for its fork service
    // (analysis/trajectory.cpp).

    // The moving objects of the DRAWN lines only (simulate / runPrediction):
    // off, every object stays where the real game has it for the whole line;
    // on, they move the way the rest of this file moves them, placed every
    // `every` ticks rather than every tick. A scripted run (run, steer,
    // search) always moves them every tick. The defaults are Absense's.
    struct DisplayMovers {
        bool on = true;
        int every = 1;
    };
    DisplayMovers m_displayMovers;

    // The sub-tick preview's two questions, ported from anticroom's Silicate
    // fork (src/trajectory/trajectory.cpp: cloneReal, splitStep, extrapolate,
    // extrapolateBranch; GPL-3) onto these copies. subtickPose: where the
    // player is `fraction` of the way through the next tick, with nothing
    // pressed. subtickBranch: from that point, the rest of the tick and then
    // `ticks` whole ticks with the button held (or let go), one point per
    // step in `path`, the first being the split point. Both put the game
    // back and draw nothing. False when they cannot run.
    bool subtickPose(GJBaseGameLayer* pl, bool p1, float fraction, TrajectoryPlayerData& out, bool& died);
    bool subtickBranch(GJBaseGameLayer* pl, bool p1, float fraction, bool hold, int ticks,
                       std::vector<cocos2d::CCPoint>& path);

   private:
    // A copy set up from the real player the way simulate() sets one up.
    PlayerObject* copyOfReal(GJBaseGameLayer* pl, bool p1);
    void splitStep(GJBaseGameLayer* pl, PlayerObject* copy, float delta);

   public:

    bool playerHasActivated(PlayerObject* player, EnhancedGameObject* object);
    bool realPlayerHasActivated(PlayerObject* player,
                                EnhancedGameObject* object);

    void rememberActivatedObject(EnhancedGameObject* object,
                                 PlayerObject* player) {
        if (player == m_fakePlayer1) {
            m_activatedObjectsP1.insert((uintptr_t)object);
        } else if (player == m_fakePlayer2) {
            m_activatedObjectsP2.insert((uintptr_t)object);
        }
    }

    // Whether this copy's run has fired the object, whatever its multi-activate
    // or platformer rule (playerHasActivated applies those).
    bool copyFired(PlayerObject* player, EnhancedGameObject* object) const {
        if (player == m_fakePlayer1) return m_activatedObjectsP1.contains((uintptr_t)object);
        if (player == m_fakePlayer2) return m_activatedObjectsP2.contains((uintptr_t)object);
        return false;
    }

    uint64_t& teleportRand() { return m_teleportRand; }

    void deactivateAllRemembered() {
        m_activatedObjectsP1.clear();
        m_activatedObjectsP2.clear();
    }

    ~Trajectory() {
        dropSearch();
        m_fakePlayer1->m_maybeLastGroundObject = nullptr;
        m_fakePlayer2->m_maybeLastGroundObject = nullptr;

        CC_SAFE_RELEASE(m_fakePlayer1);
        CC_SAFE_RELEASE(m_fakePlayer2);
        CC_SAFE_RELEASE(m_node);
    }
};

class TrajectoryManager {
   private:
    Trajectory* m_trajectory;

   public:
    TrajectoryState m_state;
    TrajectoryManager() { m_trajectory = nullptr; }

    bool exists() const { return m_trajectory != nullptr; }

    Trajectory* unsafeInner() { return m_trajectory; }

    bool drawing() const {
        if (m_trajectory) {
            return m_trajectory->drawing();
        }

        return false;
    }

    bool simulating() const { return m_trajectory && m_trajectory->simulating(); }
    void simBreak(GameObject* o) {
        if (m_trajectory) m_trajectory->simBreak(o);
    }

    bool enabled() const {
        if (m_trajectory) {
            return m_trajectory->enabled();
        }

        return false;
    }

    PlayerObject* getOtherPlayer(PlayerObject* player) {
        if (m_trajectory) {
            return m_trajectory->getOtherPlayer(player);
        }

        return 0;
    }

    void handleButton(bool p1, bool holding) {
        if (m_trajectory) {
            m_trajectory->handleButton(p1, holding);
        }
    }

    void dualPortal(GJBaseGameLayer* pl, PlayerObject* copy, EffectGameObject* portal, bool toDual) {
        if (m_trajectory) m_trajectory->dualPortal(pl, copy, portal, toDual);
    }

    RunResult steer(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int lookahead, std::vector<TickInput>& script,
                    std::span<const TickInput> other = {}, bool otherDown = false,
                    float aimOffset = std::numeric_limits<float>::quiet_NaN()) {
        if (m_trajectory)
            return m_trajectory->steer(pl, p1, ticks, buttonDown, lookahead, script, other, otherDown, aimOffset);
        script.clear();
        return {};
    }
    RunResult search(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget, std::vector<TickInput>& script,
                     std::span<const TickInput> other = {}, bool otherDown = false) {
        if (m_trajectory) return m_trajectory->search(pl, p1, ticks, buttonDown, budget, script, other, otherDown);
        script.clear();
        return {};
    }
    bool beginSearch(GJBaseGameLayer* pl, bool p1, int ticks, bool buttonDown, int budget,
                     std::span<const TickInput> other = {}, bool otherDown = false, std::span<const TickInput> base = {},
                     int widen = 0) {
        return m_trajectory && m_trajectory->beginSearch(pl, p1, ticks, buttonDown, budget, other, otherDown, base, widen);
    }
    bool stepSearch(int maxTicks, std::chrono::steady_clock::time_point until = {}) {
        return m_trajectory ? m_trajectory->stepSearch(maxTicks, until) : true;
    }
    RunResult finishSearch(std::vector<TickInput>& script) {
        if (m_trajectory) return m_trajectory->finishSearch(script);
        script.clear();
        return {};
    }
    bool searching() const { return m_trajectory && m_trajectory->searching(); }
    bool simulatesBoth(GJBaseGameLayer* pl, bool p1) const { return m_trajectory && m_trajectory->simulatesBoth(pl, p1); }
    bool separateControls(GJBaseGameLayer* pl) const { return m_trajectory && m_trajectory->separateControls(pl); }
    Trajectory::HitboxCheck checkHitboxes(GJBaseGameLayer* pl, bool p1) {
        if (m_trajectory) return m_trajectory->checkHitboxes(pl, p1);
        return {};
    }

    std::shared_ptr<Trajectory::SimStart> captureStart(GJBaseGameLayer* pl, bool p1) {
        return m_trajectory ? m_trajectory->captureStart(pl, p1) : nullptr;
    }
    std::shared_ptr<Trajectory::SimStart> advanceStart(GJBaseGameLayer* pl, std::shared_ptr<Trajectory::SimStart> from,
                                                       std::span<const TickInput> inputs, std::span<const TickInput> inputs2) {
        return m_trajectory ? m_trajectory->advanceStart(pl, std::move(from), inputs, inputs2) : nullptr;
    }
    void useStart(std::shared_ptr<Trajectory::SimStart> start) {
        if (m_trajectory) m_trajectory->useStart(std::move(start));
    }
    void forgetClearedFlags() {
        if (m_trajectory) m_trajectory->forgetClearedFlags();
    }
    void realStateChanged() {
        if (m_trajectory) m_trajectory->realStateChanged();
    }
    bool usingStart() const { return m_trajectory && m_trajectory->usingStart(); }
    void addPhantom(const cocos2d::CCRect& rect, int realTick) {
        if (m_trajectory) m_trajectory->addPhantom(rect, realTick);
    }
    void dropPhantomsBefore(float x) {
        if (m_trajectory) m_trajectory->dropPhantomsBefore(x);
    }
    void clearPhantoms() {
        if (m_trajectory) m_trajectory->clearPhantoms();
    }
    size_t phantomCount() const { return m_trajectory ? m_trajectory->phantomCount() : 0; }
    std::vector<Trajectory::Phantom> phantoms() const {
        return m_trajectory ? m_trajectory->phantoms() : std::vector<Trajectory::Phantom>{};
    }
    float lastRealDelta() const { return m_trajectory ? m_trajectory->lastRealDelta() : 0.0f; }
    float realDeltaAverage() const { return m_trajectory ? m_trajectory->realDeltaAverage() : 0.0f; }
    RunResult run(GJBaseGameLayer* pl, bool p1, std::span<const TickInput> inputs, int ticks, bool buttonDown,
                  std::vector<TraceSample>* trace = nullptr, std::span<const TickInput> other = {}, bool otherDown = false,
                  bool eitherDeath = false) {
        if (m_trajectory) return m_trajectory->run(pl, p1, inputs, ticks, buttonDown, trace, other, otherDown, eitherDeath);
        return {};
    }


    void update(GJBaseGameLayer* pl) {
        if (m_trajectory) m_trajectory->update(pl);
    }

    void toggle() {
        if (m_trajectory) {
            m_trajectory->setEnabled(!m_trajectory->enabled());
        }
    }

    void setEnabled(bool enabled) {
        if (m_trajectory) {
            m_trajectory->setEnabled(enabled);
        }
    }

    void init() {
        moverCacheInvalidate();
        m_trajectory = Trajectory::create();
        m_trajectory->m_state = &m_state;
        // The level is set up: the World checks its offsets against it (and
        // turns itself off, logging why, when they do not hold).
        world::World::init(GJBaseGameLayer::get());
    }

    void uninit() {
        moverCacheInvalidate();
        delete m_trajectory;
        m_trajectory = nullptr;
    }

    bool playerHasActivated(PlayerObject* player, EnhancedGameObject* object) {
        if (m_trajectory) {
            return m_trajectory->playerHasActivated(player, object);
        }

        return false;
    }
    bool copyFired(PlayerObject* player, EnhancedGameObject* object) const {
        return m_trajectory && m_trajectory->copyFired(player, object);
    }

    void noteRingContact(PlayerObject* p, GameObject* ring) {
        if (m_trajectory) m_trajectory->noteRingContact(p, ring);
    }
    void noteKiller(PlayerObject* p, GameObject* object) {
        if (m_trajectory) m_trajectory->noteKiller(p, object);
    }
    Trajectory::Killer lastKiller() const { return m_trajectory ? m_trajectory->lastKiller() : Trajectory::Killer{}; }
    void clearKiller() {
        if (m_trajectory) m_trajectory->clearKiller();
    }
    bool ringUsedThisContact(PlayerObject* p, GameObject* ring) {
        return m_trajectory && m_trajectory->ringUsedThisContact(p, ring);
    }
    void noteRingUsed(PlayerObject* p, GameObject* ring) {
        if (m_trajectory) m_trajectory->noteRingUsed(p, ring);
    }

    // The simulated portal code switched a copy to the kind a portal gives.
    void noteFakeModeFromPortal(PlayerObject* player, GameObjectType portal) {
        if (!m_trajectory) return;
        int mode = Trajectory::kModeCube;
        switch (portal) {
            case GameObjectType::ShipPortal: mode = Trajectory::kModeShip; break;
            case GameObjectType::BallPortal: mode = Trajectory::kModeBall; break;
            case GameObjectType::UfoPortal: mode = Trajectory::kModeBird; break;
            case GameObjectType::WavePortal: mode = Trajectory::kModeDart; break;
            case GameObjectType::RobotPortal: mode = Trajectory::kModeRobot; break;
            case GameObjectType::SpiderPortal: mode = Trajectory::kModeSpider; break;
            case GameObjectType::SwingPortal: mode = Trajectory::kModeSwing; break;
            default: break;
        }
        m_trajectory->noteFakeMode(player, mode);
    }

    bool realPlayerHasActivated(PlayerObject* player,
                                EnhancedGameObject* object) {
        if (m_trajectory) {
            return m_trajectory->realPlayerHasActivated(player, object);
        }

        return false;
    }

    bool hasDied(PlayerObject* player) {
        if (m_trajectory) {
            return m_trajectory->hasDied(player);
        }

        return false;
    }
    void setDelta(float delta) {
        if (m_trajectory) {
            m_trajectory->setDelta(delta);
        }
    }

    bool isFakePlayer(PlayerObject* player) {
        if (m_trajectory) {
            return m_trajectory->isFakePlayer(player);
        }

        return false;
    }

    void handlePortal(PlayerObject* player, GameObject* object) {
        if (m_trajectory) {
            m_trajectory->handlePortal(player, object);
        }
    }

    void rememberActivatedObject(EnhancedGameObject* object,
                                 PlayerObject* player) {
        if (m_trajectory) {
            m_trajectory->rememberActivatedObject(object, player);
        }
    }

    void scheduleAction(std::function<void()> action, int delay) {
        if (m_trajectory) {
            m_trajectory->scheduleAction(action, delay);
        }
    }
};

#endif
