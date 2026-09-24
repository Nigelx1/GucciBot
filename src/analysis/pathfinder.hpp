#pragma once

#include "core/GucciBot.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gucci {

    // Pathfinder -- autonomously searches for a click sequence that beats
    // the current level, with no pre-recorded macro to start from.
    //
    // v1 design ("death-driven search"): run the real game forward with no
    // input until GD's own collision system kills the player at frame D.
    // That death IS the decision-point detector -- something before D had
    // to be different. Search backward from D for a press (+ hold length)
    // that gets further, committing it and continuing; when a decision
    // point runs out of candidates, backtrack to the previous one via a
    // real checkpoint restore and try its next candidate (DFS). Every
    // candidate that failed at a decision point is never re-tried there,
    // which is exactly the failure mode yBot's pathfinder is known for.
    //
    // No physics is reimplemented and no level geometry is parsed: input
    // goes through the ordinary macro-playback path (replay.m_actionAtom),
    // survival is whatever GD's real destroyPlayer says, and gamemode
    // interpretation of a button press is the hooked engine's problem, not
    // ours. That's the structural difference from a from-scratch simulator.
    //
    // Driven one physics tick at a time from frameUpdateMidhook, like
    // Calculate's fwTick -- there is no blocking "simulate N frames" call
    // in this codebase. Reuses GucciEngine::fwAnalyzing as the "headless
    // sim in progress" flag on purpose (12+ hook sites already gate on it
    // for exactly the behavior we want: input lookup offset, path-capture
    // suppression, native death/levelComplete suppression, quiet logging),
    // with fwState left at Idle so fwTick() itself is a no-op.
    class Pathfinder {
    public:
        static Pathfinder* get();

        // --- settings (persisted) ---
        int windowFrames = 30;        // how far back from a death to try clicks
        int checkpointInterval = 8;   // frames between rolling restore points
        int maxRuns = 20000;          // total candidate runs before giving up
        int maxRunFrames = 240 * 180; // stuck-guard: one run never exceeds this
        // How many frames further a candidate must survive past the death
        // that opened its decision point before it counts as real progress
        // (not just "the same hazard, hit slightly later"). See the comment
        // at its use in Pathfinder::handleDeath -- a real test run showed
        // accepting any single-frame gain traps the search in place forever.
        int minProgressFrames = 8;
        // Nigel's ask (2026-09-06): the full-screen "Calculating..." cover
        // is a toggle, not forced -- default on ("surprises are cool"), off
        // falls back to a small corner status HUD so the level is actually
        // visible while it searches.
        bool hideSearch = true;

        // --- live status (read by the GUI) ---
        bool active = false;
        std::string stage;
        int runs = 0;
        size_t depth = 0;
        uint32_t bestFrame = 0;
        float bestX = 0.f;
        float bestPct = 0.f;
        bool hasResult = false;
        bool lastResultSuccess = false;
        size_t resultInputCount = 0;
        // Name the solution was auto-saved under, for the result UI to show.
        std::string savedAs;

        // --- agency diagnostics, read by the GUI ---
        // Without these the only evidence the new backtracking is doing
        // anything lives in a log Geode doesn't persist, which makes "it
        // looks the same" impossible to tell from "it is the same".
        // Cumulative across the whole search, not per run -- a per-run count
        // resets thousands of times a second and reads as 0 whatever happens.
        int probeRuns = 0;          // probes attempted this search
        int probeFails = 0;         // probes that couldn't run at all
        int agencyFramesSeen = 0;   // probes that came back "this mattered"
        // Enough to tell the two failure shapes apart: a fork that dies on
        // frame 0 (holdSurvived stays 0) versus a fork that runs fine but
        // whose press changes nothing (survives fully, gap stays 0).
        float maxGapSeen = 0.0f;
        int lastHoldSurvived = -1;
        int lastReleaseSurvived = -1;
        int lastPointCount = 0;     // decision points found at the last death
        int lastLookback = 0;       // frames between that death and the earliest
        bool lastUsedAgency = false;
        // Candidates that made progress but left almost no room for the next
        // decision point, and how many of those later had to be used anyway.
        int deferredCramped = 0;
        int deferredReplayed = 0;

        void begin();
        void cancel();
        void tick();
        // Ring checkpoints MUST be taken from here, not from tick(). Called
        // from the settled point in frameUpdateMidhook -- see the comment on
        // the definition (pathfinder.cpp). Capturing from tick() records a
        // position one frame staler than the label it gets filed under, which
        // is the whole checkpoint X-drift bug.
        void serviceSettledCapture();
        // Runs the hold-vs-release fork for the frame that just settled and
        // records whether it mattered. Search-only -- normal play never calls
        // this, so a fault here can only ever reach an active search.
        void serviceAgencyProbe();
        void noteDeath(uint32_t frame, float x);
        void noteLevelComplete();
        void saveSettings() const;

    private:
        Pathfinder();

        struct Candidate {
            uint32_t pressFrame;
            int holdFrames;
        };
        struct Node {
            StoredFrame ckpt;
            bool fullResetInstead = false;
            uint32_t deathFrame = 0;
            std::vector<Candidate> cands;
            size_t next = 0;
            // committed.size() at creation -- backtracking INTO this node
            // truncates committed back to this, which drops exactly this
            // node's own (later-appended) successful candidate.
            size_t committedBefore = 0;
            // A candidate that got further but would have left the next
            // decision point cramped against it. Held back while this node's
            // other candidates -- the alternatives for that same input -- get
            // their turn, and used only if none of them works out.
            bool hasDeferred = false;
            bool deferredUsed = false;
            Candidate deferred{0, 1};
            uint32_t deferredDeath = 0;

            // A solution remembered from a previous search of this level,
            // tried ahead of the generated candidates.
            bool rememberedFirst = false;
            Candidate remembered{0, 1};
            bool rememberedTried = false;

            // Step 4 (2026-09-22): this node's reach-back was cut short by the
            // previous committed input rather than by running out of agency,
            // AND there are agency frames below that floor. So the frame that
            // actually decided this death is very likely one this node is not
            // allowed to touch, and every candidate in the clipped window is a
            // run spent to prove it. Set once at build time.
            bool floorClipped = false;
            // Guards the reopen: a node that already gave up its window once
            // searches it properly the next time, so a bad guess costs one
            // extra backtrack rather than looping.
            bool reopenSpent = false;
        };

        // Which frames of the CURRENT run the player actually had a say on,
        // indexed by absolute frame. Filled by the agency probe as the run
        // plays; cleared per run, since a different branch is a different
        // trajectory and its measurements don't carry over.
        std::vector<uint8_t> agencyMap;

        std::vector<Node> stack;
        std::vector<gb::Action> committed;

        // Dead ends already proven, so the search never pays for one twice.
        // Idea taken from Absense's tabu list; the soundness argument is ours.
        //
        // A key is (hash of the committed prefix, press frame, hold length).
        // If all three match a run that already failed, the replay is
        // bit-identical to that run -- same level state, same inputs, same
        // physics -- so it cannot end differently. Skipping it is not a
        // heuristic, it is arithmetic.
        //
        // That argument only holds because of the 2026-09-22 determinism work
        // (object variance, Random triggers, teleport and shake RNG all seeded
        // from the macro). Before that, two runs of the same inputs genuinely
        // could diverge, and this cache would have been unsound.
        std::unordered_set<uint64_t> deadEnds;
        int skippedDeadEnds = 0;  // reported at finish, so the saving is visible
        // Prefer a press that still works a frame late over one that only
        // works on exactly its frame. Absense runs an extra simulation to ask
        // that question; here it is answered for free from deadEnds, which
        // already knows the neighbour's fate whenever it has been tried.
        bool preferRobust = true;
        int deferredFragile = 0;

        // What got past a hard spot on this level last time, so a second
        // search tries the known answer first instead of re-deriving it.
        // Idea from Absense's pathfinder/memory, including the part that makes
        // it safe: a remembered candidate is still RUN and judged by real
        // physics like any other. Memory only changes the ORDER things are
        // tried in -- it can never let a wrong answer through.
        //
        // Keyed by (death frame, committed-prefix hash), which is exact rather
        // than fuzzy: with the run deterministic, a repeat search reaches the
        // same decision point with the same prefix, so an exact key hits. If
        // anything upstream differs the key simply misses and the search
        // proceeds normally.
        struct RememberedWin {
            uint32_t pressFrame = 0;
            int holdFrames = 1;
        };
        std::unordered_map<uint64_t, RememberedWin> solutionMemory;
        int memoryHits = 0;
        bool memoryDirty = false;
        int memoryLevelID = 0;

        uint64_t memoryKey(uint32_t deathFrame) const;
        void loadSolutionMemory();
        void saveSolutionMemory();
        void rememberWin(uint32_t deathFrame, uint32_t pressFrame, int holdFrames);
        static constexpr size_t kMaxDeadEnds = size_t(1) << 16;

        uint64_t committedHash() const;
        uint64_t deadEndKey(uint32_t pressFrame, int holdFrames) const;
        gb::ActionAtom savedAtom;
        std::vector<StoredFrame> ring;

        bool died = false;
        uint32_t deathFrame = 0;
        float deathX = 0.f;
        bool completed = false;
        uint32_t runFrames = 0;
        bool haveCandidate = false;
        Candidate cur{0, 1};

        // DFS "success" is a chain of checkpoint-restore-and-continue
        // segments, never one continuous frame-0 run -- so before trusting
        // it, replay the whole committed list from a genuine cold reset
        // (still under fwAnalyzing, same frame convention it was built
        // under) and require it to independently reach LEVEL COMPLETE too.
        // See the 2026-09-06 build -t log analysis this fixes: the search
        // reported "press@149 got from f=161 to f=248", a huge margin, yet
        // real playback of the exact same frame died at f=161 regardless --
        // proof the chained-checkpoint result wasn't reproducible end to
        // end, which a frame-numbering fix alone could never explain.
        bool confirming = false;
        // The run in flight is a deferred candidate being replayed so its
        // death (and this run's ring and agency map) can seed its node.
        bool replayingDeferred = false;

        void handleDeath();
        void buildNodeFromDeath(uint32_t d);
        int agencyPointsBetween(int64_t floor, uint32_t d) const;
        void startNextCandidateOrBacktrack();
        void startRun(const Node* node);
        void startConfirmRun();
        void applyAtom();
        void takeRingCheckpoint(uint32_t frame);
        void releaseRing();
        static void releaseStoredFrame(StoredFrame& sf);
        void finish(bool success);
    };

} // namespace gucci
