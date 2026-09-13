#pragma once

#include "core/GucciBot.hpp"

#include <string>
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

        void begin();
        void cancel();
        void tick();
        // Ring checkpoints MUST be taken from here, not from tick(). Called
        // from the settled point in frameUpdateMidhook -- see the comment on
        // the definition (pathfinder.cpp). Capturing from tick() records a
        // position one frame staler than the label it gets filed under, which
        // is the whole checkpoint X-drift bug.
        void serviceSettledCapture();
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
        };

        std::vector<Node> stack;
        std::vector<gb::Action> committed;
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

        void handleDeath();
        void buildNodeFromDeath(uint32_t d);
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
