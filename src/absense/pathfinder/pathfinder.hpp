#pragma once

// Absense - pathfinder.
//
// Plays a level by search, on the real game. Every physics tick it commits is
// a real tick of the real level (recorded into the replay like a real
// input), so the result is exact; what to press is decided by trying ideas
// on a copy of the player with the mod's own physics (the trajectory
// simulation) and keeping the one that survives the longest. When nothing
// survives it goes back - a little at first, then further and further - to
// a state it saved earlier and tries something else there, remembering what
// already failed at that tick so it never repeats it.
//
// Runs one slice of work per visual frame (see tick()), inside a time
// budget, so the game keeps drawing and the interface stays responsive.

#include <Geode/Geode.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absense/compat/bot.hpp"
#include "absense/gpu/protocol.hpp"
#include "human.hpp"
#include "human.hpp"
#include "memory.hpp"
#include "absense/compat/settings.hpp"
#include "absense/compat/settings.hpp"
#include "absense/trajectory/trajectory.hpp"

class AbsensePathfinder {
   public:
    enum class Phase { Idle, Deciding, Committing, Backtracking, Probing, Restoring, Done };

    struct Stats {
        uint64_t decisions = 0;
        uint64_t simulations = 0;
        uint64_t simulatedTicks = 0;
        uint64_t committedTicks = 0;
        uint64_t deadEnds = 0;
        uint64_t backtracks = 0;
        uint64_t probes = 0;        // ticks tried on the way back after a dead end
        uint64_t divergences = 0;   // times the real path left the simulated one
        uint64_t escalations = 0;   // times the search had to look further back than the limit
        uint64_t fragile = 0;       // decisions where no idea survived being a tick late
        uint64_t branches = 0;      // branch points the searching idea tried
        uint64_t dualDeaths = 0;    // searches whose straight path died as player 2 (dual mode)
        uint64_t clicksDropped = 0; // inputs taken out of plans as unneeded
        uint64_t phantoms = 0;      // deaths the simulation did not see, kept as spots to avoid
        uint64_t unrepeatable = 0;  // ideas whose own script did not repeat them (the simulation did not put something back)
        uint64_t worldDrift = 0;    // times the World was caught disagreeing with the real game or with itself
        uint64_t gpuBatches = 0;    // batches scored on the graphics card
        uint64_t gpuScripts = 0;    // ideas it scored
        uint64_t remembered = 0;    // remembered solutions tried as ideas
        uint64_t rememberedWins = 0; // ... that were the one played
        uint64_t humanTried = 0;    // the human macro's inputs tried as ideas
        uint64_t humanWins = 0;     // ... that were the one played
        uint64_t aheadDecisions = 0; // decisions made about where a plan ends while it was still playing
        uint64_t freezes = 0;       // times the game had to wait for a decision (realtime)
        uint64_t startTick = 0;
        uint64_t currentTick = 0;
        uint64_t bestTick = 0;
        float bestX = 0.0f;
        float progress = 0.0f;  // 0..1 of the level length (current)
        float bestProgress = 0.0f;
        double seconds = 0.0;   // wall clock spent
        double sliceMs = 0.0;   // the last slice of work, and what it was allowed
        double budgetMs = 0.0;
        double gameMs = 0.0;    // what the frame cost besides the slice
        double unitMs = 0.0;    // what one indivisible piece of the search costs
        double ticksPerSecond = 0.0;
        // Where the time goes (seconds): stepping the real game, simulating,
        // going back to earlier states.
        double secondsReal = 0.0;
        double secondsSim = 0.0;
        double secondsRestore = 0.0;
        std::string lastDecision;  // what the last search picked
        std::string message;       // why it stopped (or a live note)
    };

    AbsensePathfinder();

    // Bindable switch: on starts, off stops.
    SLValuePtr<bool> m_running = SLValue<bool>::create("pathfinder.running", &m_runningFlag);

    bool isRunning() const { return m_phase != Phase::Idle && m_phase != Phase::Done; }
    Phase phase() const { return m_phase; }
    const Stats& stats() const { return m_stats; }

    // The drawScene hook measures what the frame cost outside this slice (the
    // game and the drawing, and the restore that runs after it) and hands it
    // back here; the next slice is sized from it. See AbsensePathfinder::tick().
    void noteFrameCost(double gameMs, double restoreMs);

private:
    // May another unit of work start? A unit is indivisible - a whole run of
    // one idea over the look, a whole try of the trim, a whole robustness
    // check - so the answer is yes while there is time left, and once more
    // per slice after the time has gone, because a slice that did nothing
    // would never get anywhere. See the definition.
    bool mayWork();
    bool m_forcedThisSlice = false;
    std::chrono::steady_clock::time_point m_unitStart{};
    bool m_unitTimed = false;
    double m_unitMs = 0.0;  // what one unit costs, as they are measured running

public:

    // Starts from the current tick of the current level. Returns false with
    // a message in stats().message when it cannot.
    bool start();
    void stop(const std::string& reason, bool leavingLevel = false);

    // One slice of work; call once per visual frame while running.
    void tick();

    // Realtime: the game runs on its own clock while the plan plays. The
    // input hook asks for the tick's planned input; the tick hook does the
    // bookkeeping after it.
    bool live() const { return m_live; }
    void liveInput();
    void afterLiveTick();

    // Hooks
    // The real player would have died on this tick: the tick is a dead end.
    // Returns true when the death must be suppressed (the pathfinder is
    // driving and will go back on its own).
    bool onWouldDie(PlayerObject* player, GameObject* object);
    void onLevelComplete();
    // A reset the pathfinder did not ask for (the user restarted, died
    // outside of our control): stop.
    void onForeignReset();
    void onQuit();

    // Helpers the gameplay hooks use to stay out of the way while it runs.
    bool drivesGame() const { return isRunning(); }
    // Time the updater spent putting an earlier state back (statistics).
    void noteRestoreTime(double seconds) { m_stats.secondsRestore += seconds; }
    // Lets go of all but the newest `keep` kept states (the memory guard);
    // returns how many went.
    size_t dropOldAnchors(size_t keep);

   private:
    struct Candidate {
        std::vector<TickInput> inputs;
        int survived = 0;
        bool complete = false;
        bool evaluated = false;  // outcome already known
        int claimed = -1;        // what the steering / search itself reported (checked against its script)
        int steer = 0;           // a steered idea with this look-ahead; its script is made when evaluated
        // GucciBot: a steered idea that holds a height once the copy flies (the
        // height it starts flying at plus this); NaN for the plain steers.
        float aimOffset = std::numeric_limits<float>::quiet_NaN();
        int events = 0;      // presses / releases in the script
        int eventsWindow = 0; // ... of which inside the part that will be played
        bool killerStatic = false;  // it dies at an object that cannot move (the simulation is right about it)
        int killerId = 0;
        int killerUid = 0;  // the object instance it dies at (the key confirmed killers are kept by)
        float killerX = 0.0f, killerY = 0.0f;
        int firstEvent = -1; // tick of the first event (-1: none)
        bool screen = false; // a first cheap look at one of the card's ideas
        // How long the graphics card says this script lasts over the whole
        // horizon (-1: it was never scored). A key for ranking ideas that tie
        // on the cheap look, never a verdict: every one of them is still run
        // through the game's own physics before it can be chosen.
        int card = -1;
        int memoryIndex = -1; // a remembered solution (index into the level's memory)
        bool carryOn = false; // the verified rest of the last plan (wins ties among full survivors)
        int reach = 0;       // where the run ended, in blocks of 30 units (set in evaluate, ranked in better)
        std::string name;
    };

    // A decision in progress (decide() works in slices).
    struct Decision {
        bool active = false;
        int H = 0;
        int K = 0;
        int margin = 0;
        int need = 0;               // the bar an idea has to clear to be viable (settled at set-up, see decide)
        int level = 0;              // the widening of the spot this decision is about (see widenLevel / escalate)
        int window = 0;             // ticks of the plan that will be played (the rest is decided again)
        uint64_t tick = 0;
        bool held = false;
        std::vector<Candidate> candidates;
        std::vector<size_t> tried;  // indexes of the judged ideas
        size_t next = 0;            // next idea to judge
        int best = -1;              // index of the best judged idea
        int fullSurvivors = 0;
        enum Stage { Steers, Gpu, GpuFull, Search, Rest, Verdict, Trim };
        Stage stage = Steers;
        bool hard = false;          // needed more than the cheap ideas, or is repairing: worth remembering
        std::string situation;      // for the priors (which kinds of idea win here)
        size_t steersEnd = 0;       // ideas before this are carrying on and the steered ones
        size_t gpuEnd = 0;          // the ideas the graphics card picked end here
        int screenHorizon = 0;      // the short horizon the first look uses (0: the whole one)
        size_t gpuResume = 0;       // where the ordinary ideas were up to when the card's turn came
        size_t restScreenStart = 0; // the ordinary ideas' first cheap look starts here
        int keep = -1;              // the "keep" idea, judged whole first in the Rest stage (see judge)
        // The samples of the run just made, and the winning run's own: the
        // commit takes its trace from these instead of running the chosen
        // script a third time. Good only while the real game has not moved on
        // - a run places the moving objects for the tick it is made at, and a
        // decision made ahead is made while the plan plays, so an older trace
        // would hand the drift check object positions that are out of date.
        std::vector<TraceSample> scratchTrace;
        std::vector<TraceSample> bestTrace;
        int bestTraceFor = -1;                // the candidate the samples belong to
        uint64_t bestTraceTick = UINT64_MAX;  // the real tick they were made at
        // The card's search, kept between frames: one round of it per slice.
        struct GpuIdea {
            absense::gpu::Script script;
            uint32_t survived = 0;
            float x = 0.0f;
        };
        std::vector<GpuIdea> gpuBeam;  // what the next round extends
        std::vector<GpuIdea> gpuBest;  // the best of the last round
        int gpuRound = 0;
        uint32_t gpuPeak = 0;       // the longest any of the card's rounds has lasted so far
        double gpuMs = 0.0;         // what the card's rounds of this decision cost so far
        double gpuFirstMs = 0.0;    // ... and what the first one cost
        bool gpuGained = false;     // the last round got further than any before it
        size_t restEnd = 0;         // the single-change and random ideas end here (the search's comes after)
        bool searching = false;     // the branching search is running
        // Where the search ran from and its key in the run memo (see memoStart).
        const Trajectory::SimStart* searchMemoFrom = nullptr;
        std::pair<uint64_t, uint64_t> searchKey{0, 0};
        // The verdict, kept while the chosen idea has its needless clicks
        // taken out (in slices, see trimStep).
        Candidate chosen;
        std::vector<TickInput> chosenRaw;  // the chosen idea before the trim: what the next probe would generate again
        bool viable = false;        // the chosen idea gets past what this decision had to see
        int forcedCommit = -1;      // play only this many ticks (the simulation sees a death after them)
        int trimTries = 0;
        int trimMax = 0;
        size_t trimAt = 0;          // the switch being looked at
        int trimVariant = 0;        // which change to it is tried next
        int trimmed = 0;            // switches taken out so far
        int trimFrom = 0;           // how far the chosen idea lasted before the trim
        bool trimComplete = false;  // ... and whether it reached the end
        // Two-player mode (separate controls): which player this decision
        // is for, and the other player's script over the horizon, held
        // fixed while this one is decided (player 1 first, with player 2
        // carrying on; then player 2, with player 1's choice).
        bool pair = false;
        bool player2 = false;
        std::vector<TickInput> other;
        bool otherHeld = false;
        // The slack check of the verdict (the best ideas run again with
        // every input a tick late), a run per slice.
        std::vector<size_t> slackOrder;
        size_t slackAt = 0;
        int slackChecked = 0;
        int slackNeeded = 0;
        int slackChosen = -1;
        bool slackDone = false;
    };

    struct Anchor {
        SavedCheckpoint state;
        uint64_t tick = 0;
    };

    // Search
    void decide();
    void buildCandidates(std::vector<Candidate>& out, bool held, int horizon);
    void buildSteeredCandidates(std::vector<Candidate>& out, bool held, int horizon);
    // Scores a big pool of ideas on the graphics card and adds the best few
    // as ordinary candidates (the simulation still judges them). How many
    // were added, or 0 when the card was not used.
    // One round per call: -1 while there are more rounds to run, otherwise
    // how many ideas were added.
    int buildGpuCandidates(Decision& d);
    // Scores the ideas named by `which` on the graphics card, each as the
    // per-tick script it is, and keeps the answer in Candidate::card.
    void scoreOnCard(Decision& d, const std::vector<size_t>& which);
    // The player as the decision sees it (live, or the kept start being probed).
    Trajectory::StartState decisionState(const Decision& d);
    void addSearchCandidate(std::vector<Candidate>& out, bool held, int horizon, const RunResult& r,
                            std::vector<TickInput> script);
    static void countEvents(Candidate& c, bool held, int window);
    // Whether the idea still gets as far when every input lands one tick
    // late: what a plan that hugs walls or times frame-perfect jumps fails.
    bool robust(const Candidate& c, int horizon, bool held, int need);
    // A run of the simulation for the player the decision is about, with
    // the other player following its fixed script in two-player mode.
    RunResult runFor(const Decision& d, std::span<const TickInput> inputs, int ticks,
                     std::vector<TraceSample>* trace = nullptr, bool eitherDeath = false);
    // Dual part of a two-player level: each player has its own controls.
    bool separateNow() const;
    // Takes the switches the chosen idea does not need out of it, a few
    // simulated tries per slice (a lone tap, a press-release pair, a change
    // of state made later or not at all - whatever leaves the copy just as
    // far along): the same path with fewer clicks. Only the part of the
    // plan that will be played is looked at. True once it is done.
    bool trimStep(Decision& d);
    void addCandidate(std::vector<Candidate>& out, std::vector<TickInput> inputs, std::string name, bool held);
    bool evaluate(Candidate& c, int horizon, bool held, std::vector<TraceSample>* trace = nullptr);
    // Runs made from a kept start, remembered by start and script: a run is a
    // pure function of the two, and the passes and retries of a repair walk the
    // same stops from the same kept starts again. Forgotten whenever a kept
    // start is dropped (every real tick, every restore), so nothing in it was
    // made under a different real state.
    struct RunMemo {
        uint64_t check = 0;  // a second, independent hash of the key
        int survived = 0;
        bool complete = false;
        bool died = false;
        bool dualDeath = false;
        float x = 0.0f;
        bool goingLeft = false;
        Trajectory::Killer killer;
        std::vector<TickInput> script;  // a steered idea's or the search's script
        int claimed = -1;
    };
    std::unordered_map<uint64_t, RunMemo> m_runMemo;
    // The memo is only cleared when the real game moves, and a walk on a spot
    // nothing gets past never moves it: past this many entries it starts over
    // (it is only a cache, so clearing it never changes an answer).
    static constexpr size_t kMaxRunMemo = size_t(1) << 16;
    const Trajectory::SimStart* memoStart() const;
    void forgetRunMemo() { m_runMemo.clear(); }
    bool isTabu(uint64_t tick, const std::vector<TickInput>& inputs, bool player2 = false) const;
    static uint64_t hashPrefix(const std::vector<TickInput>& inputs, int length);

    // Real game
    bool commitTick();
    bool applyInput(const TickInput& in, bool player2 = false);
    void stepGame();
    bool playerHeld(bool player2 = false) const;
    void keepAnchor(uint64_t at = UINT64_MAX);
    void dropAnchorsAfter(uint64_t tick);
    void releaseAnchors();
    void beginBacktrack();
    void finishRestore();
    void noteProgress();
    void rememberBest();
    void saveResult();
    void saveFurthest();

    bool levelReady() const;
    uint64_t currentTick() const;
    float levelLength() const;
    float playerX() const;

    void log(const char* fmt, ...);

    Phase m_phase = Phase::Idle;
    bool m_runningFlag = false;
    Stats m_stats;
    std::mt19937 m_rng;

    // Plan being committed (inputs for the ticks after the decision) and
    // what was committed at every tick since the start.
    std::vector<TickInput> m_plan;
    std::vector<TickInput> m_plan2;    // player 2's, in a two-player level's dual part
    size_t m_planIndex = 0;
    Decision m_decision;
    // Player 1's choice, kept while player 2 is decided (two-player mode).
    bool m_pairSecondPass = false;
    std::vector<TickInput> m_pairChosen1;
    bool m_pairHeld1 = false;
    int m_pairSurvived1 = 0;
    bool m_pairComplete1 = false;
    int m_pairForced1 = -1;
    uint64_t m_pairKnownDeath1 = UINT64_MAX;
    std::string m_pairName1;
    std::chrono::steady_clock::time_point m_deadline{};  // end of this frame's budget
    std::vector<TickInput> m_history;  // index: tick - startTick
    std::vector<TickInput> m_history2; // player 2's (two-player mode)
    std::vector<uint8_t> m_historyPair;  // 1: the tick was played with separate controls
    uint64_t m_decisionTick = 0;
    // What the simulation expected the committed plan to do, tick by tick;
    // the real game is checked against it while committing.
    std::vector<TraceSample> m_planTrace;
    uint64_t m_lastReplanTick = 0;

    // Dead ends: tick -> (length, hash) of input prefixes that led nowhere.
    struct Tabu {
        int length = 0;
        uint64_t hash = 0;   // player 1's prefix
        uint64_t hash2 = 0;  // player 2's (two-player mode)
        bool pair = false;   // the dead end was the pair's: the two prefixes together are forbidden
    };
    std::unordered_map<uint64_t, std::vector<Tabu>> m_tabu;
    std::unordered_map<uint64_t, uint32_t> m_visits;
    // What was actually committed at a tick (the idea before the trim, and how
    // much of it was played): a stop walked back to forbids that idea, not the
    // whole of what the ticks after it happened to play.
    std::unordered_map<uint64_t, Tabu> m_played;
    int m_prefixLength = 8;

    // Going back: after a dead end the search steps back one tick at a
    // time (then in growing steps) and asks the simulation at every stop
    // whether there is a way past the spot it died at; it resumes from the
    // first stop that has one. Dying again at the same spot starts the next
    // repair one step further back.
    struct Repair {
        bool active = false;
        uint64_t deathTick = 0;        // where the death is (real, or where the simulation sees it)
        uint64_t from = 0;             // the tick the dead end was declared at (the stops count back from here)
        uint64_t floor = 0;            // never resume before this tick (the limit)
        int probe = 0;                 // how many stops were tried so far
        int passes = 0;                // times every stop has been walked with fresh ideas
        int retries = 0;               // times the last stop was tried again with another idea
        uint64_t retryTarget = UINT64_MAX;  // set: go back to this stop instead of the next one
        std::vector<TickInput> failed; // the inputs that led to the dead end (from startTick)
        std::vector<TickInput> failed2;      // player 2's
        std::vector<uint8_t> failedPair;     // 1: that tick was played with separate controls
    };
    Repair m_repair;
    // Where the last repair resumed from, so a death at the same spot can try
    // something else from that stop before walking further back (the
    // simulation approved what was played, and the game disagreed: what is
    // played from there is the thing to change, not how far back it is).
    struct LastResume {
        bool valid = false;
        uint64_t tick = 0;
        uint64_t deathTick = 0;
        int probe = 0;
        int retries = 0;
    };
    LastResume m_lastResume;
    // Every spot the search died at, and how often: a dead end at a spot
    // that already failed starts its repair further back. Kept per spot so
    // a detour that dies somewhere else does not wipe what was learned.
    struct Trap {
        uint64_t tick = 0;   // where it keeps dying
        int fails = 0;       // how many dead ends around there
        float x = 0.0f;      // where that is in the level (for the memory)
        int anyway = 0;      // times the floor's best idea was played although it does not get past
        int level = 0;       // times every stop of this spot failed: each one widens what is tried (see escalate)
    };
    std::vector<Trap> m_traps;
    Trap* trapAt(uint64_t tick, uint64_t slack);
    // The widening of the spot a decision at `at` is about: the one being
    // repaired, or the one resumed past while the path is still before it.
    int widenLevel(uint64_t at = 0);
    // What happens instead of giving up when every stop down to the floor
    // failed: the spot's level goes up (a wider search at every stop), the
    // floor - or the start - goes a bounded step further back when anything is
    // kept there, and the stops are walked again. Never stops the search.
    void escalate(const char* why);
    // Every batch of ideas the card scores uses new random ones, so walking
    // the same stops again is not the same walk.
    uint32_t m_gpuSeed = 0;
    bool m_gpuWarned = false;  // the card's trouble is logged once a run
    // Times the start itself was moved back into the frames stored before
    // it, because the way past a spot lay before where the search began.
    int m_startMoves = 0;
    bool moveStartBack();

    // Realtime (see tick()). The game plays the plan on its own clock, and
    // the next decision is made meanwhile about where the plan ends, from a
    // start the simulation carried there. The game only stops when the plan
    // runs out before that decision is done.
    bool m_live = false;
    bool m_livePending = false;
    uint64_t m_liveTick = 0;
    TickInput m_liveIn{};
    TickInput m_liveIn2{};
    struct Ahead {
        bool active = false;
        uint64_t tick = 0;     // where the plan ends: the decision is about here
        std::shared_ptr<Trajectory::SimStart> start;
        bool held = false, flying = false, pair = false, held2 = false, flying2 = false;
    };
    Ahead m_ahead;
    bool m_aheadFailed = false;  // the decision about this plan's end found nothing: the plan plays out, the real state decides
    void goLive();
    void pauseLive();
    void beginAhead();
    void cancelAhead();
    bool afterTick(uint64_t tick, const TickInput& in, const TickInput& in2);
    uint64_t m_liveNextTick = 0;  // the tick the next live input is for
    // Where the game was meant to stop (realtime stops land a few ticks
    // late: the frame's remaining ticks still run); it goes back there.
    uint64_t m_pauseAt = UINT64_MAX;
    // Back to `target` (a stored frame, then the history), for when the
    // game ran on past where a decision is about. False without a frame.
    bool rewindTo(uint64_t target);
    // The verified rest of the last chosen script beyond what was played:
    // the next decision's search starts from it instead of from nothing.
    std::vector<TickInput> m_planTail;
    uint64_t m_planTailTick = UINT64_MAX;
    // Objects the simulation saw a death at and the game then confirmed:
    // from then on the simulation's word about them is final, and the
    // search never plays into them again to "see what the game says".
    // What world::drifts() read when this run started: m_stats.worldDrift is
    // the count since, not the process's.
    uint64_t m_worldDriftBase = 0;
    std::unordered_set<int> m_confirmedKillers;
    int m_planKiller = 0;  // the object the plan being played dies at (0: none)

    // What a decision would have the memory learn - kept until the real
    // game has got as far as the simulation promised. A plan that dies
    // ten ticks later is not a way past anything, and going back past it
    // drops it unlearned.
    struct Lesson {
        uint64_t decidedAt = 0;
        uint64_t confirmAt = 0;  // the real tick that confirms it
        Trajectory::StartState state;
        std::vector<TickInput> inputs;  // empty: nothing to remember, only the priors
        int lasted = 0;
        int memoryIndex = -1;  // a remembered idea that worked again
        std::string situation;
        std::string name;
    };
    std::vector<Lesson> m_lessons;
    void confirmLessons(uint64_t now);
    void dropLessonsAfter(uint64_t tick);
    // Stops with a reason the player is told about (a dialog), for the
    // stops that are not the player's own doing.
    void giveUp(const std::string& reason);
    uint64_t m_backtrackTarget = 0;
    uint64_t m_restepTo = 0;       // after a restore to a stored frame, step the history up to here
    uint64_t backDistance(int probe) const;
    void beginRepair();
    void resumeFrom(uint64_t tick);  // bookkeeping when a repair found a way on

    // The players' state at every tick played (the last few seconds). After
    // a dead end the stops on the way back are looked at from these, in the
    // simulation, and the game only goes back for real - a full reset of
    // the level, the slow part - to the stop that has a way past.
    struct Start {
        uint64_t tick = 0;
        std::shared_ptr<Trajectory::SimStart> sim;
    };
    std::deque<Start> m_starts;
    void keepStart(uint64_t at = UINT64_MAX);
    void dropStartsAfter(uint64_t tick);
    const Start* startAt(uint64_t tick) const;
    // A decision being made at an earlier tick, from a kept start.
    struct Probe {
        bool active = false;
        uint64_t tick = 0;
        bool held = false;
        bool flying = false;
        bool pair = false;     // two-player mode, both players kept
        bool held2 = false;
        bool flying2 = false;
        // What it found: a plan to play once the game is back at `tick`.
        bool found = false;
        std::vector<TickInput> plan;
        std::vector<TickInput> plan2;
        std::vector<TraceSample> trace;
        std::string decision;
        uint64_t knownDeath = UINT64_MAX;  // where the simulation sees the plan die (a try played to let the game speak)
        // The lesson of the decision made here: filed once the game is back
        // and its plan is about to play (the restore drops everything past
        // the tick it lands on, this one included).
        bool hasLesson = false;
        Lesson lesson;
    };
    Probe m_probe;
    void probe();
    void endProbe();
    uint64_t m_goTo = UINT64_MAX;  // the next backtrack goes exactly here (a probed stop)
    bool m_restorePending = false;
    // A restore that landed on a kept state re-steps the real ticks from there
    // to the stop. That is hundreds to thousands of whole game ticks in one
    // lump; it is cut at the slice's deadline and carried on next frame, and
    // this says the preamble of finishRestore has already run.
    bool m_restepping = false;
    bool m_wouldDie = false;
    uint64_t m_wouldDieTick = 0;
    cocos2d::CCRect m_wouldDieRect;   // the player's inner hitbox when the game killed it
    bool m_wouldDieP2 = false;        // it was player 2 the game killed
    // How well the World follows the object the game killed the player with
    // (world::certaintyOf, asked outside a run): only a death at one it cannot
    // follow leaves a phantom spot behind. Uncertain when nothing named it -
    // a plain block or a fall out of the level kills with no object at all,
    // and that is exactly the death the simulation is most likely to miss.
    world::Certainty m_wouldDieCertainty = world::Certainty::Uncertain;
    // Where the obstacle of the dead end being repaired is: the tick the
    // real game died at, or the tick the simulation sees the death at when
    // no idea gets past (a dead end declared without dying).
    uint64_t m_deadEndAt = 0;
    bool m_deadEndReal = false;
    bool m_deadEndUnseen = false;  // the game killed the player where the simulation saw nothing
    float m_deadEndX = 0.0f;       // where the obstacle is in the level (the memory's spots are keyed by it)
    // Where the simulation expects the plan being played to die (it is
    // being played to see whether the game agrees), or none.
    uint64_t m_planKnownDeath = UINT64_MAX;
    bool m_expectReset = false;
    bool m_completed = false;

    std::deque<Anchor> m_anchors;  // oldest first; the start state is kept apart
    Anchor m_startAnchor;
    bool m_hasStartAnchor = false;

    // The furthest replay seen (actions), kept so stopping early still
    // leaves the best macro found.
    std::vector<slc::Action> m_bestActions;

    // Settings restored on stop
    bool m_savedPaused = false;
    bool m_savedBackstep = false;
    bool m_savedLockDelta = false;
    uint32_t m_savedStoredFrames = 0;
    uint32_t m_savedStoreEvery = 1;
    int m_savedMode = 0;

    std::chrono::steady_clock::time_point m_startedAt;
    // Frame pacing: a slice takes what is left of a frame at the target
    // frame rate. When the last frame came late the game's own share is
    // taken from it (the time between slices minus the slice) and the next
    // slice is cut to fit; when it came on time the slice grows a little.
    std::chrono::steady_clock::time_point m_lastSliceStart{};
    double m_lastSliceMs = 0.0;
    double m_budgetMs = 0.0;
    // What the frame costs besides the slice, measured by the CCDirector hook
    // and fed back with noteFrameCost(); smoothed so one spike does not swing
    // the next slice.
    double m_gameMs = 0.0;
    double m_gameMsAvg = 0.0;
    std::chrono::steady_clock::time_point m_lastRateAt;
    uint64_t m_lastRateTicks = 0;
};
