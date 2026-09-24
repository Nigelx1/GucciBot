#include "analysis/pathfinder.hpp"

#include "analysis/trajectory.hpp"

#include "gui/gui.hpp" // currentThemeExtension, for auto-saving the solved macro

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <algorithm>
#include <filesystem>

using namespace geode::prelude;

namespace gucci {

    // Tap first, then progressively longer holds. Cube only ever needs the
    // tap; Ship/Wave/Swing need real hold lengths. Frame-major ordering
    // (all holds at the frame nearest the death before moving one frame
    // earlier) because "just before the hazard" is the common case.
    static const int kHoldSet[] = {1, 3, 6, 10, 16, 24};

    // How far back a decision point may reach when the frames in between had
    // no agency -- a long fall can put the real decision hundreds of frames
    // before the death. Bounded because the ring of restore points has to
    // cover whatever this allows.
    static constexpr int kMaxAgencyLookback = 960;   // 4s at 240 TPS

    // Below this many agency frames between a candidate's release and the
    // death it reached, the decision point it would open is cramped: pinned
    // against the input just committed, with too little room for anything
    // meaningfully different. The fall that prompted this showed "6 frames
    // with agency, reaching back 9" -- the real fix was the input before.
    static constexpr int kCrampedPoints = 8;

    Pathfinder* Pathfinder::get() {
        static Pathfinder inst;
        return &inst;
    }

    Pathfinder::Pathfinder() {
        auto* mod = Mod::get();
        windowFrames = (int)mod->getSavedValue<int64_t>("pf_window", 30);
        checkpointInterval = (int)mod->getSavedValue<int64_t>("pf_ckpt_interval", 8);
        maxRuns = (int)mod->getSavedValue<int64_t>("pf_max_runs", 20000);
        hideSearch = mod->getSavedValue<bool>("pf_hide_search", true);
        minProgressFrames = (int)mod->getSavedValue<int64_t>("pf_min_progress", 8);
        windowFrames = std::clamp(windowFrames, 5, 240);
        checkpointInterval = std::clamp(checkpointInterval, 1, 60);
        maxRuns = std::clamp(maxRuns, 100, 1000000);
        minProgressFrames = std::clamp(minProgressFrames, 1, 60);
    }

    void Pathfinder::saveSettings() const {
        auto* mod = Mod::get();
        mod->setSavedValue("pf_window", (int64_t)windowFrames);
        mod->setSavedValue("pf_ckpt_interval", (int64_t)checkpointInterval);
        mod->setSavedValue("pf_max_runs", (int64_t)maxRuns);
        mod->setSavedValue("pf_hide_search", hideSearch);
        mod->setSavedValue("pf_min_progress", (int64_t)minProgressFrames);
    }

    void Pathfinder::releaseStoredFrame(StoredFrame& sf) {
        if (sf.state.m_checkpoint) {
            sf.state.m_checkpoint->release();
            sf.state.m_checkpoint = nullptr;
        }
    }

    void Pathfinder::releaseRing() {
        for (auto& sf : ring)
            releaseStoredFrame(sf);
        ring.clear();
    }

    void Pathfinder::begin() {
        // Fresh search: nothing proven dead yet.
        deadEnds.clear();
        skippedDeadEnds = 0;
        this->loadSolutionMemory();
        hazardCount.clear();

        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (active || !pl)
            return;

        savedAtom = gb->replay.m_actionAtom;
        committed.clear();
        for (auto& n : stack)
            releaseStoredFrame(n.ckpt);
        stack.clear();
        releaseRing();
        runs = 0;
        depth = 0;
        probeRuns = 0;
        probeFails = 0;
        agencyFramesSeen = 0;
        maxGapSeen = 0.0f;
        lastHoldSurvived = -1;
        lastReleaseSurvived = -1;
        lastPointCount = 0;
        lastLookback = 0;
        lastUsedAgency = false;
        deferredCramped = 0;
        deferredReplayed = 0;
        replayingDeferred = false;
        bestFrame = 0;
        bestX = 0.f;
        bestPct = 0.f;
        hasResult = false;
        lastResultSuccess = false;
        resultInputCount = 0;
        haveCandidate = false;
        died = false;
        completed = false;
        confirming = false;

        // Same pause-dismiss Calculate does before a headless run -- the
        // GUI is usually open (and GD paused) when Start gets pressed.
        if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
            for (auto* child : CCArrayExt<CCNode*>(scene->getChildren())) {
                if (auto* pause = typeinfo_cast<PauseLayer*>(child)) {
                    pause->onResume(nullptr);
                    break;
                }
            }
        }
        pl->m_isPaused = false;
        pl->m_isPracticeMode = false;

        // Create the fork's preview players up front. probeAgency would do it
        // on demand, but that would land mid-physics-step on the first frame
        // of the search; doing it here keeps allocation out of that path.
        TrajectoryPredictionService::get().attach(pl);
        agencyMap.clear();

        gb->fwAnalyzing = true;
        gb->fwState = GucciEngine::FwState::Idle;
        gb->fwProbeDied = false;
        gb->muteAnalysisMusic();

        active = true;
        stage = "exploring";
        log::info("[Pathfinder] started -- window {} frames back, ckpt every {} frames, max {} runs",
                  windowFrames,
                  checkpointInterval,
                  maxRuns);
        startRun(nullptr);
    }

    void Pathfinder::cancel() {
        if (!active)
            return;
        log::info("[Pathfinder] cancelled after {} runs, best {:.1f}%", runs, bestPct);
        finish(false);
        stage = "cancelled";
    }

    void Pathfinder::serviceAgencyProbe() {
        if (!active || confirming)
            return;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1)
            return;
        auto* gb = GucciEngine::get();
        uint32_t frame = gb->updater.getFrame();

        probeRuns++;
        AgencyResult agency;
        if (!TrajectoryPredictionService::get().probeAgency(
                pl, pl->m_player1, agency, kAgencyProbeFrames)) {
            probeFails++;
            return;
        }

        if (agencyMap.size() <= (size_t)frame)
            agencyMap.resize((size_t)frame + 512, 0);
        agencyMap[(size_t)frame] = agency.matters ? 1 : 0;

        if (agency.matters)
            agencyFramesSeen++;
        if (agency.divergence > maxGapSeen)
            maxGapSeen = agency.divergence;
        lastHoldSurvived = agency.holdSurvived;
        lastReleaseSurvived = agency.releaseSurvived;
    }

    void Pathfinder::noteDeath(uint32_t frame, float x) {
        if (!active || died)
            return;
        died = true;
        deathFrame = frame;
        deathX = x;
        deathStateKey = this->captureDeathStateKey();
    }

    // What the player was, where, and how fast, at the moment it went wrong --
    // quantised, so two attempts that die at the same hazard hash the same
    // even though neither the approach nor the exact pixel matches.
    //
    // Position is bucketed at 4 units. A GD block is 30, so this is well
    // inside "the same spot" while still separating two hazards a block apart.
    uint64_t Pathfinder::captureDeathStateKey() const {
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1)
            return 0;
        auto* p = pl->m_player1;

        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; i++) {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 1099511628211ull;
            }
        };

        constexpr float kPosBucket = 4.f;
        mix((uint64_t)(int64_t)std::floor(p->getPositionX() / kPosBucket));
        mix((uint64_t)(int64_t)std::floor(p->getPositionY() / kPosBucket));

        // Form. Cube is the absence of all of these, which hashes fine as 0.
        uint64_t form = 0;
        if (p->m_isShip) form |= 1u << 0;
        if (p->m_isBird) form |= 1u << 1;
        if (p->m_isBall) form |= 1u << 2;
        if (p->m_isDart) form |= 1u << 3;
        if (p->m_isRobot) form |= 1u << 4;
        if (p->m_isSpider) form |= 1u << 5;
        if (p->m_isSwing) form |= 1u << 6;
        if (p->m_isUpsideDown) form |= 1u << 7;
        if (p->m_vehicleSize < 1.f) form |= 1u << 8;
        if (p->m_isSideways) form |= 1u << 9;
        mix(form);

        // Speed is one of a handful of fixed values; rounding keeps float
        // noise from splitting a bucket.
        mix((uint64_t)(int64_t)std::lround(p->m_playerSpeed * 100.0));
        return h;
    }

    void Pathfinder::noteLevelComplete() {
        if (!active)
            return;
        completed = true;
    }

    void Pathfinder::applyAtom() {
        auto* gb = GucciEngine::get();
        auto& acts = gb->replay.m_actionAtom.m_actions;
        acts = committed;
        if (haveCandidate) {
            gb::Action press;
            press.m_frame = cur.pressFrame;
            press.m_type = gb::ActionType::Jump;
            press.m_holding = true;
            press.m_player2 = false;
            gb::Action release = press;
            release.m_frame = cur.pressFrame + (uint32_t)std::max(1, cur.holdFrames);
            release.m_holding = false;
            acts.push_back(press);
            acts.push_back(release);
        }
        std::stable_sort(acts.begin(), acts.end(), [](const gb::Action& a, const gb::Action& b) {
            return a.m_frame < b.m_frame;
        });
        gb->replay.m_inputIndex = 0;
    }

    void Pathfinder::startRun(const Node* node) {
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        // Any rolling checkpoints belong to the run we're abandoning.
        releaseRing();
        applyAtom();
        died = false;
        completed = false;
        runFrames = 0;
        agencyMap.clear();
        gb->fwProbeDied = false;

        auto& pf = gb->practiceFix;
        pf.m_savedCheckpoints.clear();
        pf.m_brokenObjects.clear();
        pf.m_storedFrames.clear();

        if (!node || node->fullResetInstead) {
            // Mirrors analyzeFrameWindows()' cold start.
            pf.m_loadCheckpoint = false;
            pf.m_isBackstep = false;
            gb->updater.m_fullReset = true;
            pl->resetLevel();
            gb->updater.m_fullReset = false;
            gb->updater.resetFrame();
            gb->replay.m_inputIndex = 0;
            gb->mode = GucciEngine::Mode::Playing;
            return;
        }

        // Mirrors beginProbeRun()'s arbitrary-checkpoint restore idiom --
        // the target pushed twice on purpose (restorePreviousFrame pops one
        // then applies the one behind it).
        pf.m_storedFrames.push_back(node->ckpt);
        pf.m_storedFrames.push_back(node->ckpt);
        gb->mode = GucciEngine::Mode::Playing;
        if (pf.canRestoreState()) {
            pf.m_loadCheckpoint = true;
            pf.m_isBackstep = true;
            pl->resetLevel();
            pf.m_loadCheckpoint = false;
            pf.m_isBackstep = false;
        }
    }

    void Pathfinder::startConfirmRun() {
        auto* pl = PlayLayer::get();
        if (!pl) {
            // Can't run a confirmation without a level to run it in -- ship
            // what the search found rather than get stuck with no path
            // forward at all.
            finish(true);
            return;
        }

        confirming = true;
        bestX = 0.f;
        bestFrame = 0;
        bestPct = 0.f;
        stage = "confirming the solution with a clean run";

        // Delegate to the exact same cold-start path the search's own first
        // probe already uses (startRun(nullptr)), on purpose -- NOT a hand-
        // rolled duplicate. A real test proved why that matters: this
        // function used to set gb->replay.m_actionAtom itself AFTER calling
        // pl->resetLevel(), whereas applyAtom() (which startRun() always
        // calls first) sets it BEFORE resetLevel() -- and the confirmation
        // run registered exactly zero clicks as a result (Nigel: "it seemed
        // to not even attempt a click"), because whatever resetLevel()'s
        // hook does with the current action-atom state at the moment it
        // runs, doing it against an empty atom breaks input consumption for
        // the rest of that run even after the atom is filled in afterward.
        // `committed` already holds the full solution and haveCandidate is
        // false by this point, so applyAtom() inside startRun() loads
        // exactly what we want.
        startRun(nullptr);
    }

    void Pathfinder::takeRingCheckpoint(uint32_t frame) {
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp)
            return;
        cp->retain();
        gb->fwCkptCreatedThisFrame = true;
        auto& pf = gb->practiceFix;
        pf.saveState(cp, frame);
        StoredFrame sf;
        sf.frame = frame;
        if (!pf.m_storedFrames.empty()) {
            sf.state = pf.m_storedFrames.back().state;
            pf.m_storedFrames.pop_back();
        } else {
            sf.state.m_checkpoint = cp;
        }
        if (!pf.m_savedCheckpoints.empty())
            pf.m_savedCheckpoints.pop_back();
        ring.push_back(sf);

        // Only need to reach back windowFrames from wherever the next death
        // lands -- m_maxBackstepFrames-style unbounded growth is exactly
        // what to avoid here.
        // Deep enough to cover the furthest back a decision point can now
        // reach: with no agency for a long stretch, the restore point has to
        // be older than the whole stretch or the search can't branch there.
        size_t cap = (size_t)std::clamp(kMaxAgencyLookback / std::max(1, checkpointInterval) + 4,
                                        8,
                                        128);
        while (ring.size() > cap) {
            releaseStoredFrame(ring.front());
            ring.erase(ring.begin());
        }
    }

    void Pathfinder::tick() {
        if (!active)
            return;
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        if (completed) {
            if (confirming) {
                log::info("[Pathfinder] confirmation run PASSED -- the committed macro reaches LEVEL "
                          "COMPLETE on its own from a genuine frame-0 replay, not just as a chain of "
                          "checkpoint restores. Shipping it.");
                finish(true);
                return;
            }
            log::info("[Pathfinder] LEVEL COMPLETE after {} runs, {} committed inputs -- verifying with "
                      "a clean frame-0 replay before handing it off (a chain of checkpoint-restored "
                      "segments isn't proof it holds up end to end)",
                      runs,
                      committed.size() + (haveCandidate ? 2 : 0));
            if (haveCandidate) {
                gb::Action press;
                press.m_frame = cur.pressFrame;
                press.m_type = gb::ActionType::Jump;
                press.m_holding = true;
                gb::Action release = press;
                release.m_frame = cur.pressFrame + (uint32_t)std::max(1, cur.holdFrames);
                release.m_holding = false;
                committed.push_back(press);
                committed.push_back(release);
                haveCandidate = false;
            }
            replayingDeferred = false;
            startConfirmRun();
            return;
        }

        runFrames++;
        uint32_t f = gb->updater.getFrame();
        float x = pl->m_player1 ? pl->m_player1->m_position.x : 0.f;

        if (x > bestX) {
            bestX = x;
            bestFrame = f;
            // pl->m_levelLength directly, NOT the cached gb->m_levelLength --
            // confirmed via a real test run that the cache reads 0 even mid-
            // level (GB7PlayLayer::init() captures it too early, before GD
            // finishes computing it). destroyPlayer's own [CAP-DIE] log line
            // already reads the live field this same way; follow that.
            bestPct = pl->m_levelLength > 0.f ? std::min(100.f, x / pl->m_levelLength * 100.f)
                                              : 0.f;
        }

        if (!died && runFrames > (uint32_t)maxRunFrames) {
            // Stuck (platformer standstill, softlock, etc.) -- treat as a
            // death at this frame so the search keeps moving.
            died = true;
            deathFrame = f;
            deathX = x;
        }

        if (died) {
            if (confirming) {
                // This should now be rare. It was written when the restore
                // chain accumulated a one-frame X drift, which made the
                // search commit candidates a continuous replay wouldn't
                // survive; that drift was fixed on 2026-09-13 (capture moved
                // to the settled point) and confirmed in-game. Kept anyway,
                // because the reasoning holds for a failure from any cause:
                // this run IS a genuine continuous replay, so its death is
                // ground truth and the checkpoint chain is what got it
                // wrong. Salvage rather than discard -- keep the committed
                // prefix that survives past it, drop the rest, and reopen
                // from here on the ring checkpoints this run just took.
                //
                // Truncate by whole (press, release) pairs, never action by
                // action. committed is only ever built in pairs, and dropping
                // a release while keeping its press leaves a dangling press
                // that pins lastCommitted against the next death with no room
                // for a candidate -- an unrecoverable dead end that reads
                // exactly like the search legitimately running out of
                // options. The first version of this fix did that; a real
                // test caught it.
                size_t before = committed.size();
                size_t keepPairs = 0;
                for (size_t i = 0; i + 1 < committed.size(); i += 2) {
                    if (committed[i].m_frame < deathFrame && committed[i + 1].m_frame < deathFrame)
                        keepPairs = i + 2;
                    else
                        break;
                }
                committed.resize(keepPairs);
                log::warn("[Pathfinder] confirmation FAILED -- a genuine frame-0 replay of the "
                          "committed macro died @f={} x={:.1f} instead of reaching the end. Dropped "
                          "{} of {} committed inputs that didn't survive a clean run and reopening "
                          "the search from this real death instead of shipping something unproven.",
                          deathFrame,
                          deathX,
                          before - committed.size(),
                          before);
                confirming = false;
                for (auto& n : stack)
                    releaseStoredFrame(n.ckpt);
                stack.clear();
                depth = 0;
                stage = fmt::format("confirmation didn't hold up @f={} -- resuming search", deathFrame);
                buildNodeFromDeath(deathFrame);
                startNextCandidateOrBacktrack();
                return;
            }
            handleDeath();
            return;
        }

        // Ring checkpoints are NOT taken here any more -- see
        // serviceSettledCapture(), called from the settled point in
        // frameUpdateMidhook. tick() runs after incrementFrame() but before
        // this frame's physics has actually run, so anything captured here
        // holds the previous frame's position under this frame's label.
    }

    // Runs at the "settled point": the top of frameUpdateMidhook, BEFORE
    // incrementFrame(). At that instant getFrame() reads the frame whose
    // physics has just finished, and the live player state is that frame's
    // completed state -- so capturing here and labelling with getFrame() is
    // correct by construction.
    //
    // Capturing from tick() instead (what this used to do) pairs a position
    // with a label one frame ahead of it. Restore then trusts the label
    // (hook_playlayer.cpp sets m_frameOnLastAttempt from it), so every
    // restore silently dropped exactly one frame of X advancement, and the
    // error compounded across decision points. That is the ~1.047-unit X gap
    // build 2026-09-06-w measured with Y/velocity/rotation matching exactly
    // (X is the only value that moves every single frame regardless of what
    // the player is doing, so it was the only field that showed it).
    //
    // This is the same bug, and the same fix, as the deferred capture in
    // storeCheckpoint (hook_playlayer.cpp) -- that one is even commented
    // "compounding per-checkpoint position drift". That fix was only ever
    // applied to that one call site. Silicate avoids the whole class
    // structurally by reading the frame inside its own createCheckpoint
    // rather than letting callers pass a label; this is the same idea.
    void Pathfinder::serviceSettledCapture() {
        if (!active)
            return;
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();
        if (!pl || pl->m_playerDied)
            return;
        if (completed || died)
            return;

        uint32_t f = gb->updater.getFrame();
        // Unconditional during confirm runs too: a confirmation run needs its
        // own rolling checkpoints, so that if it fails, the search reopening
        // has a genuinely trustworthy restore point close to the real failure
        // instead of only a cold full reset.
        if (checkpointInterval > 0 && f > 0 && (f % (uint32_t)checkpointInterval) == 0)
            takeRingCheckpoint(f);
    }

    int Pathfinder::agencyPointsBetween(int64_t floor, uint32_t d) const {
        floor = std::max<int64_t>(floor, std::max<int64_t>((int64_t)d - kMaxAgencyLookback, 1));
        int points = 0;
        for (int64_t f = (int64_t)d - 1; f >= floor && points < windowFrames; --f)
            if ((size_t)f < agencyMap.size() && agencyMap[(size_t)f])
                points++;
        return points;
    }

    void Pathfinder::handleDeath() {
        died = false;
        uint32_t d = deathFrame;

        if (stack.empty()) {
            log::info("[Pathfinder] first death @f={} x={:.1f} -- opening decision point", d, deathX);
            buildNodeFromDeath(d);
            startNextCandidateOrBacktrack();
            return;
        }

        Node& top = stack.back();
        // Real test run (2026-09-06) exposed a trap here: accepting ANY
        // single extra frame as "progress" lets the search commit a
        // candidate that only delays death by the hold-length difference --
        // same hazard, hit one frame later, x unchanged -- which then opens
        // a decision point whose window is immediately empty (lastCommitted
        // sits right up against the new death frame), forcing an instant
        // backtrack. The whole run thrashed on exactly that instead of
        // searching for a real escape. Requiring a real minimum gain forces
        // rejected-as-failure instead of falsely-accepted-as-progress.
        bool progressed = d >= top.deathFrame + (uint32_t)std::max(1, minProgressFrames);
        bool wasDeferredReplay = replayingDeferred;
        replayingDeferred = false;

        // Commits `cur` and opens the decision point its death creates. Logs
        // before buildNodeFromDeath, which can reallocate the stack `top`
        // refers into.
        auto commitCurrent = [&](const char* how) {
            gb::Action press;
            press.m_frame = cur.pressFrame;
            press.m_type = gb::ActionType::Jump;
            press.m_holding = true;
            gb::Action release = press;
            release.m_frame = cur.pressFrame + (uint32_t)std::max(1, cur.holdFrames);
            release.m_holding = false;
            // Worked. Remember it against the situation this node was opened
            // to repair -- top.deathFrame and top.stateKey, not the death that
            // just happened, which is the NEXT problem further along.
            this->rememberWin(top.stateKey, top.deathFrame, cur.pressFrame, cur.holdFrames);

            committed.push_back(press);
            committed.push_back(release);
            haveCandidate = false;
            log::info("[Pathfinder] press@{} hold {} got from f={} to f={} (x={:.1f}) -- {}, "
                      "{} inputs so far",
                      cur.pressFrame,
                      cur.holdFrames,
                      top.deathFrame,
                      d,
                      deathX,
                      how,
                      committed.size());
            buildNodeFromDeath(d);
            startNextCandidateOrBacktrack();
        };

        if (!progressed) {
            // Proven dead: this exact candidate, from this exact committed
            // prefix, does not get past the death. Recorded so the search
            // cannot spend another run rediscovering it after a backtrack
            // brings it back here -- which the floor-reopen in step 4 makes
            // considerably more likely, since it deliberately revisits.
            if (deadEnds.size() >= kMaxDeadEnds)
                deadEnds.clear();  // a cache; dropping it costs time, never correctness
            deadEnds.insert(this->deadEndKey(cur.pressFrame, cur.holdFrames));

            // No progress -- next candidate at this decision point (or backtrack).
            startNextCandidateOrBacktrack();
            return;
        }

        if (wasDeferredReplay) {
            // Already judged: it made progress, and nothing with more room did.
            commitCurrent("committed after nothing roomier worked");
            return;
        }

        // Progress, but how much room does it leave? If the next decision
        // point would be pinned right behind this input, the likelier fix is
        // a different version of this input -- and those are exactly this
        // node's remaining candidates. Try them first; keep this one in
        // reserve. Only judged when this run actually measured up to the death.
        uint32_t release = cur.pressFrame + (uint32_t)std::max(1, cur.holdFrames);
        bool measured = agencyMap.size() >= (size_t)d;
        int room = measured ? agencyPointsBetween((int64_t)release + 1, d) : windowFrames;
        if (room < kCrampedPoints) {
            if (!top.hasDeferred || d > top.deferredDeath) {
                top.hasDeferred = true;
                top.deferred = cur;
                top.deferredDeath = d;
            }
            deferredCramped++;
            haveCandidate = false;
            log::info("[Pathfinder] press@{} hold {} got from f={} to f={} but leaves only {} frames "
                      "with agency behind it -- held back while this decision point's other "
                      "candidates get a turn",
                      cur.pressFrame,
                      cur.holdFrames,
                      top.deathFrame,
                      d,
                      room);
            startNextCandidateOrBacktrack();
            return;
        }

        // Frame-perfect solutions are worse solutions. Absense tests this by
        // re-running the candidate a tick late and seeing whether it still
        // survives, which costs a whole extra simulation per candidate --
        // runs are exactly what this search is short of.
        //
        // The dead-end cache already holds the answer whenever the neighbour
        // has been tried: if press+1 from this same prefix is known dead, this
        // press only works on its own exact frame. So the question is answered
        // for nothing, and only when it can be answered honestly -- an untried
        // neighbour says nothing and is not held against the candidate.
        //
        // Held back rather than rejected, through the same slot the cramped
        // check uses: if nothing more tolerant turns up, it gets replayed and
        // committed, so no solution is ever lost to this preference.
        if (preferRobust && !top.hasDeferred &&
            deadEnds.count(this->deadEndKey(cur.pressFrame + 1, cur.holdFrames))) {
            top.hasDeferred = true;
            top.deferred = cur;
            top.deferredDeath = d;
            deferredFragile++;
            haveCandidate = false;
            log::info(
                "[Pathfinder] press@{} hold {} works but press@{} is already known dead -- this is "
                "frame-perfect, held back while this decision point's other candidates get a turn",
                cur.pressFrame,
                cur.holdFrames,
                cur.pressFrame + 1);
            startNextCandidateOrBacktrack();
            return;
        }

        commitCurrent("committed");
    }

    // FNV-1a over the committed inputs. Only what actually changes the run --
    // frame, whether it is a press or a release, and which player -- so two
    // prefixes that produce the same playback hash the same.
    uint64_t Pathfinder::committedHash() const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; i++) {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 1099511628211ull;
            }
        };
        for (auto const& a : committed) {
            mix(a.m_frame);
            mix((a.m_holding ? 1ull : 0ull) | (a.m_player2 ? 2ull : 0ull));
        }
        mix(committed.size());
        return h;
    }

    uint64_t Pathfinder::deadEndKey(uint32_t pressFrame, int holdFrames) const {
        uint64_t h = this->committedHash();
        h ^= (uint64_t)pressFrame * 1099511628211ull;
        h *= 1099511628211ull;
        h ^= (uint64_t)(uint32_t)holdFrames * 14695981039346656ull;
        return h;
    }

    // One saved blob per level, under the mod's own save data. Levels are
    // keyed by GD's level ID, so a search on a different level never reads
    // another's answers.
    void Pathfinder::loadSolutionMemory() {
        solutionMemory.clear();
        memoryHits = 0;
        memoryDirty = false;
        memoryLevelID = 0;

        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_level)
            return;
        memoryLevelID = pl->m_level->m_levelID.value();
        if (memoryLevelID == 0)
            return;  // local/unsaved level: nothing stable to key on

        // v2: keys are situation hashes and values are offsets back from the
        // death. Neither is readable as the old (frame, prefix-hash) data, so
        // it gets its own slot rather than a migration -- the old entries were
        // only ever a search-order hint and are cheap to lose.
        auto const raw = Mod::get()->getSavedValue<std::string>(
            fmt::format("pf_memory_v2_{}", memoryLevelID), "");
        if (raw.empty())
            return;

        auto parsed = matjson::parse(raw);
        if (!parsed.isOk())
            return;
        auto obj = parsed.unwrap();
        if (!obj.isObject())
            return;
        for (auto const& [key, value] : obj) {
            if (!value.isObject())
                continue;
            RememberedWin w;
            w.pressOffset = (uint32_t)value["o"].asInt().unwrapOr(0);
            w.holdFrames = (int)value["h"].asInt().unwrapOr(1);
            if (w.pressOffset == 0)
                continue;
            solutionMemory[std::strtoull(key.c_str(), nullptr, 10)] = w;
        }
        if (!solutionMemory.empty())
            log::info("[Pathfinder] loaded {} remembered solution(s) for level {}",
                      solutionMemory.size(),
                      memoryLevelID);
    }

    void Pathfinder::saveSolutionMemory() {
        if (!memoryDirty || memoryLevelID == 0)
            return;
        auto obj = matjson::Value::object();
        for (auto const& [key, w] : solutionMemory) {
            auto entry = matjson::Value::object();
            entry["o"] = (int)w.pressOffset;
            entry["h"] = w.holdFrames;
            obj[std::to_string(key)] = entry;
        }
        Mod::get()->setSavedValue(fmt::format("pf_memory_v2_{}", memoryLevelID), obj.dump());
        memoryDirty = false;
        log::info("[Pathfinder] saved {} solution(s) for level {}",
                  solutionMemory.size(),
                  memoryLevelID);
    }

    void Pathfinder::rememberWin(uint64_t stateKey, uint32_t deathFrame, uint32_t pressFrame,
                                 int holdFrames) {
        if (memoryLevelID == 0 || stateKey == 0)
            return;
        if (pressFrame >= deathFrame)
            return;  // nothing to describe as "before the death"
        uint32_t const offset = deathFrame - pressFrame;
        if (offset > (uint32_t)kMaxAgencyLookback)
            return;  // further back than a search would ever look again

        auto const it = solutionMemory.find(stateKey);
        if (it != solutionMemory.end() && it->second.pressOffset == offset &&
            it->second.holdFrames == holdFrames)
            return;  // already known, nothing to write
        solutionMemory[stateKey] = RememberedWin{offset, holdFrames};
        memoryDirty = true;
    }

    void Pathfinder::buildNodeFromDeath(uint32_t d) {
        Node n;
        n.deathFrame = d;
        n.stateKey = deathStateKey;
        n.committedBefore = committed.size();

        // Candidates must start strictly after the last committed input so
        // committed holds and new presses never overlap (strictly sequential,
        // non-overlapping inputs, player 1 only).
        uint32_t lastCommitted = committed.empty() ? 0 : committed.back().m_frame;
        int64_t floor = std::max<int64_t>((int64_t)lastCommitted + 1, 1);
        floor = std::max<int64_t>(floor, (int64_t)d - kMaxAgencyLookback);

        // Walk back from the death collecting only the frames the player had
        // a say on. Frames without agency produce a bit-identical run whatever
        // is pressed, so they can never be the answer -- and they are the bulk
        // of what a fixed window contains whenever a death is delayed. Falling
        // off a ledge is the clear case: the whole descent has no agency, and
        // the frame that decided it sits before all of it.
        // A spot that has bitten repeatedly gets a wider look straight away.
        // Creeping outward one exhausted decision point at a time is how the
        // search spends its runs proving the same small window empty over and
        // over -- and with the floor-reopen in step 4 it can arrive here
        // several times.
        int const bites = ++hazardCount[d / kHazardBucket];
        int effectiveWindow = windowFrames;
        if (bites > 1) {
            effectiveWindow = std::min(windowFrames * std::min(bites, 4), kMaxAgencyLookback);
            log::info(
                "[Pathfinder] decision point @f={} has bitten {} times -- widening the look from "
                "{} to {} agency frames",
                d,
                bites,
                windowFrames,
                effectiveWindow);
        }

        std::vector<uint32_t> points;
        for (int64_t f = (int64_t)d - 1;
             f >= floor && (int)points.size() < effectiveWindow;
             --f) {
            if ((size_t)f < agencyMap.size() && agencyMap[(size_t)f])
                points.push_back((uint32_t)f);
        }

        // Was the walk stopped by the previous committed input, with agency
        // still available underneath it? If so the frame that decided this
        // death is very likely one this node is not allowed to touch --
        // startNextCandidateOrBacktrack acts on it.
        if ((int64_t)lastCommitted + 1 > (int64_t)d - kMaxAgencyLookback) {
            int64_t const lookFloor = std::max<int64_t>(1, (int64_t)d - kMaxAgencyLookback);
            for (int64_t f = floor - 1; f >= lookFloor; --f) {
                if ((size_t)f < agencyMap.size() && agencyMap[(size_t)f]) {
                    n.floorClipped = true;
                    break;
                }
            }
        }

        bool usedAgency = !points.empty();
        if (!usedAgency) {
            // No measurement to go on -- either the probe never ran here or
            // the lookback genuinely held no agency at all. Fall back to the
            // old fixed window rather than dead-end on a missing reading.
            for (int64_t f = (int64_t)d - 1;
                 f >= std::max<int64_t>(floor, (int64_t)d - effectiveWindow);
                 --f)
                points.push_back((uint32_t)f);
        }

        // If this exact decision point was solved before, try that answer
        // first. It is still run and still judged by the real game -- this
        // only changes the order, so a stale memory costs one run and is then
        // treated like any other failure.
        if (!points.empty() && n.stateKey != 0) {
            auto const it = solutionMemory.find(n.stateKey);
            if (it != solutionMemory.end() && (uint64_t)it->second.pressOffset < (uint64_t)d) {
                uint32_t const pf = d - it->second.pressOffset;
                // The offset came from a run whose committed prefix may have
                // been different, so the frame it lands on has to be checked
                // against THIS node's floor. Out of range means the memory is
                // simply not applicable here, not that anything is wrong.
                if ((int64_t)pf >= floor) {
                    n.rememberedFirst = true;
                    n.remembered = Candidate{pf, it->second.holdFrames};
                    log::info("[Pathfinder] seen this spot before -- trying press@{} hold {} "
                              "first (remembered as {} frames before the death)",
                              pf,
                              it->second.holdFrames,
                              it->second.pressOffset);
                }
            }
        }

        int64_t start = points.empty() ? (int64_t)d : (int64_t)points.back();
        lastPointCount = (int)points.size();
        lastLookback = (int)((int64_t)d - start);
        lastUsedAgency = usedAgency;

        // Restore point: the latest rolling checkpoint strictly before the
        // earliest candidate, else a cold full reset.
        int best = -1;
        for (size_t i = 0; i < ring.size(); ++i) {
            if ((int64_t)ring[i].frame < start && (best < 0 || ring[i].frame > ring[best].frame))
                best = (int)i;
        }
        if (best >= 0) {
            n.ckpt = ring[best];
            ring.erase(ring.begin() + best);
            start = std::max<int64_t>(start, (int64_t)n.ckpt.frame + 1);
        } else {
            n.fullResetInstead = true;
        }
        releaseRing();

        // A human already solved this level, probably. Ported from Absense's
        // pathfinder/human: if a macro was loaded when the search started, the
        // presses it makes around this death go in as candidates FIRST, with a
        // frame either side, because a route a person actually played is a far
        // better guess than the nearest agency frame.
        //
        // It is only an ordering hint. The game's own physics judges these
        // exactly like any other candidate, so where the human's route still
        // holds the decision costs one run, and where it does not -- different
        // tick rate, different approach, or the search has already diverged
        // from their path -- they simply fail and the normal search continues.
        size_t humanAdded = 0;
        if (!savedAtom.m_actions.empty() && !points.empty()) {
            uint32_t const lo = (uint32_t)std::max<int64_t>(start, 1);
            auto const& acts = savedAtom.m_actions;
            for (size_t i = 0; i < acts.size(); i++) {
                auto const& a = acts[i];
                if (a.m_player2 || !a.m_holding)
                    continue;  // player 1 presses only, same as everything else here
                if (a.m_frame < lo || a.m_frame >= d)
                    continue;

                // Pair it with its release to get the real hold length.
                int hold = 0;
                for (size_t j = i + 1; j < acts.size(); j++) {
                    if (acts[j].m_player2 || acts[j].m_holding)
                        continue;
                    hold = (int)((int64_t)acts[j].m_frame - (int64_t)a.m_frame);
                    break;
                }
                if (hold <= 0)
                    continue;

                for (int off : {0, -1, 1}) {
                    int64_t const f = (int64_t)a.m_frame + off;
                    if (f < (int64_t)lo || f >= (int64_t)d)
                        continue;
                    n.cands.push_back({(uint32_t)f, hold});
                    humanAdded++;
                }
            }
            if (humanAdded > 0)
                log::info(
                    "[Pathfinder] decision point @f={} : {} candidate(s) taken from the loaded "
                    "macro's own presses, tried before the generated ones",
                    d,
                    humanAdded);
        }

        // points is already ordered nearest-the-death first, which is the
        // common case and the order v1 searched in.
        for (uint32_t f : points)
            if ((int64_t)f >= start)
                for (int hold : kHoldSet)
                    n.cands.push_back({f, hold});

        log::info("[Pathfinder] decision point @f={} : {} candidates at {} {} over [{}..{}], "
                  "reaching back {} frames, restore {}",
                  d,
                  n.cands.size(),
                  n.cands.size() / (sizeof(kHoldSet) / sizeof(kHoldSet[0])),
                  usedAgency ? "frames with agency" : "frames (no agency data -- fixed window)",
                  start,
                  (int64_t)d - 1,
                  (int64_t)d - start,
                  n.fullResetInstead ? std::string("full reset")
                                     : fmt::format("ckpt@f={}", n.ckpt.frame));
        stack.push_back(std::move(n));
        depth = stack.size();
    }

    void Pathfinder::startNextCandidateOrBacktrack() {
        while (true) {
            if (stack.empty()) {
                log::info(
                    "[Pathfinder] exhausted every branch after {} runs -- giving up, best {:.1f}% "
                    "({} candidate(s) skipped as already-proven dead ends)",
                    runs,
                    bestPct,
                    skippedDeadEnds);
                finish(false);
                return;
            }
            Node& top = stack.back();
            // Step 4 (2026-09-22): this node's reach-back was cut short by the
            // previous committed input, and there is agency below that floor.
            // The frame that actually decided this death is therefore one this
            // node cannot reach, and every candidate in the clipped window is a
            // whole run spent proving that. Give the window up and reopen the
            // input that clipped it -- backtracking does exactly that, because
            // the parent owns the last committed input and will try its next
            // alternative for it.
            //
            // This is the reach-back FLOOR problem: falls crawl because the
            // real mistake is the jump that led off the ledge, which sits
            // before the last commit, and the only way there used to be
            // exhausting every node in between.
            //
            // Once per node. If the search comes back through here the guess
            // was wrong, so the second visit searches the window properly and
            // a bad guess costs one extra backtrack rather than a loop.
            if (top.floorClipped && !top.reopenSpent && top.next == 0 &&
                stack.size() > 1) {
                top.reopenSpent = true;
                log::info(
                    "[Pathfinder] decision point @f={} is floor-clipped by the last committed "
                    "input and has agency below it -- reopening that input instead of spending "
                    "{} runs on a window that cannot contain the answer",
                    top.deathFrame,
                    top.cands.size());
                releaseStoredFrame(top.ckpt);
                stack.pop_back();
                depth = stack.size();
                if (!stack.empty())
                    committed.resize(std::min(committed.size(), stack.back().committedBefore));
                haveCandidate = false;
                continue;
            }
            // Remembered answer goes first, once, before the generated list.
            if (top.rememberedFirst && !top.rememberedTried) {
                top.rememberedTried = true;
                if (!deadEnds.count(
                        this->deadEndKey(top.remembered.pressFrame, top.remembered.holdFrames))) {
                    cur = top.remembered;
                    haveCandidate = true;
                    runs++;
                    memoryHits++;
                    log::info(
                        "[Pathfinder] decision point @f={} solved here before -- trying the "
                        "remembered press@{} hold {} first",
                        top.deathFrame,
                        cur.pressFrame,
                        cur.holdFrames);
                    stage = fmt::format(
                        "f={} remembered press@{} hold {}", top.deathFrame, cur.pressFrame,
                        cur.holdFrames);
                    startRun(&top);
                    return;
                }
            }

            if (top.next < top.cands.size()) {
                // Skip anything already proven dead from this same committed
                // prefix. Bit-identical inputs against bit-identical state
                // cannot produce a different death.
                {
                    bool skipped = false;
                    while (top.next < top.cands.size() &&
                           deadEnds.count(this->deadEndKey(top.cands[top.next].pressFrame,
                                                           top.cands[top.next].holdFrames))) {
                        top.next++;
                        skippedDeadEnds++;
                        skipped = true;
                    }
                    if (skipped && top.next >= top.cands.size())
                        continue;  // everything here is known dead -- fall through to backtrack
                }
                cur = top.cands[top.next++];
                haveCandidate = true;
                runs++;
                if (runs > maxRuns) {
                    log::info("[Pathfinder] hit max runs ({}) -- giving up, best {:.1f}%",
                              maxRuns,
                              bestPct);
                    finish(false);
                    return;
                }
                stage = fmt::format("f={} press@{} hold {}", top.deathFrame, cur.pressFrame, cur.holdFrames);
                startRun(&top);
                return;
            }
            // Out of candidates, but one made progress and was held back for
            // leaving too little room. Nothing roomier worked, so use it:
            // replay it to get its death, ring and agency map back, then
            // commit it from handleDeath.
            if (top.hasDeferred && !top.deferredUsed) {
                top.deferredUsed = true;
                cur = top.deferred;
                haveCandidate = true;
                replayingDeferred = true;
                deferredReplayed++;
                runs++;
                if (runs > maxRuns) {
                    log::info("[Pathfinder] hit max runs ({}) -- giving up, best {:.1f}%",
                              maxRuns,
                              bestPct);
                    finish(false);
                    return;
                }
                log::info("[Pathfinder] decision point @f={} found nothing roomier -- replaying the "
                          "held-back press@{} hold {} (reached f={})",
                          top.deathFrame,
                          cur.pressFrame,
                          cur.holdFrames,
                          top.deferredDeath);
                stage = fmt::format("f={} replaying press@{} hold {}",
                                    top.deathFrame,
                                    cur.pressFrame,
                                    cur.holdFrames);
                startRun(&top);
                return;
            }
            // Exhausted -- backtrack into the parent and drop its candidate.
            log::info("[Pathfinder] dead end @f={} ({} candidates tried) -- backtracking",
                      top.deathFrame,
                      top.cands.size());
            releaseStoredFrame(top.ckpt);
            stack.pop_back();
            depth = stack.size();
            if (!stack.empty())
                committed.resize(std::min(committed.size(), stack.back().committedBefore));
            haveCandidate = false;
        }
    }

    void Pathfinder::finish(bool success) {
        this->saveSolutionMemory();
        if (memoryHits > 0)
            log::info("[Pathfinder] {} decision point(s) answered from memory", memoryHits);
        if (skippedDeadEnds > 0)
            log::info("[Pathfinder] skipped {} run(s) that were already proven dead", skippedDeadEnds);
        if (deferredFragile > 0)
            log::info("[Pathfinder] held back {} frame-perfect candidate(s) in favour of tolerant ones",
                      deferredFragile);
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();

        active = false;
        confirming = false;
        releaseRing();
        for (auto& n : stack)
            releaseStoredFrame(n.ckpt);
        stack.clear();
        depth = 0;
        haveCandidate = false;

        auto& pf = gb->practiceFix;
        pf.m_storedFrames.clear();
        pf.m_loadCheckpoint = false;
        pf.m_isBackstep = false;
        if (pl) {
            gb->updater.m_fullReset = true;
            pl->resetLevel();
            gb->updater.m_fullReset = false;
            gb->updater.resetFrame();
        }
        gb->setMode(GucciEngine::Mode::Idle);

        if (success) {
            // processQueuedButtons (hook_gjbasegamelayer.cpp) consumes an
            // action at real frame R when R == m_frame under fwAnalyzing
            // (what every search run here executes under), but only at
            // R == m_frame - 1 for normal playback -- i.e. a normally-
            // played action fires one frame EARLIER than its label. Every
            // committed frame number here was validated against the
            // fwAnalyzing convention, so it has to shift +1 to land on the
            // same real frame once fwAnalyzing goes false for actual
            // playback -- confirmed as the cause of a real test run that
            // "calculated correctly" but failed on playback.
            for (auto& a : committed)
                a.m_frame += 1;
            std::stable_sort(committed.begin(), committed.end(), [](const gb::Action& a, const gb::Action& b) {
                return a.m_frame < b.m_frame;
            });
            gb->replay.m_actionAtom.m_actions = committed;
            resultInputCount = committed.size();

            // Save it to disk here rather than leaving the user to press
            // Playback and then Save by hand -- Nigel (2026-09-13): a solved
            // search that looks like nothing happened is a bad hand-off.
            // Only auto-names when the user hasn't already named something;
            // if they have, that name is theirs and we don't touch it.
            if (gb->replayName.empty()) {
                auto dir = Mod::get()->getSaveDir() / "replays";
                std::string ext = currentThemeExtension(MenuInterface::get());
                // Named after the level rather than "pathfinder" (Nigel's ask,
                // 2026-09-13). Level names are free text and routinely contain
                // characters that aren't legal in a Windows filename, so strip
                // those instead of letting the save quietly fail. Trailing
                // dots/spaces are stripped too -- Windows won't store them.
                std::string base;
                if (pl && pl->m_level) {
                    static constexpr std::string_view kIllegal = "\\/:*?\"<>|";
                    for (char c : std::string(pl->m_level->m_levelName)) {
                        if ((unsigned char)c < 0x20 ||
                            kIllegal.find(c) != std::string_view::npos)
                            continue;
                        base += c;
                    }
                    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
                        base.pop_back();
                    size_t firstReal = base.find_first_not_of(' ');
                    base = (firstReal == std::string::npos) ? std::string() : base.substr(firstReal);
                }
                if (base.empty())
                    base = "pathfinder";
                std::string name = base;
                std::error_code ec;
                for (int n = 2; std::filesystem::exists(dir / (name + ext), ec); ++n)
                    name = fmt::format("{} {}", base, n);
                gb->replayName = name;
            }

            auto savePath =
                Mod::get()->getSaveDir() / "replays" / (gb->replayName + currentThemeExtension(MenuInterface::get()));
            if (gb->replayBackupsEnabled)
                gb->replay.backupExisting(savePath);
            gb->replay.save(savePath);
            savedAs = gb->replayName;
            // No explicit macro-list refresh needed: the list checks the
            // replays directory's own mtime (refreshReplayListIfNeeded), and
            // writing this file changes it.
            log::info("[Pathfinder] auto-saved solution as \"{}\"", gb->replayName);
            Notification::create(fmt::format("Macro saved as \"{}\"", gb->replayName),
                                 NotificationIcon::Success)
                ->show();
            stage = "done";
        } else {
            gb->replay.m_actionAtom = savedAtom;
            stage = "failed";
        }
        gb->replay.m_inputIndex = 0;
        gb->unmuteAnalysisMusic();
        gb->fwAnalyzing = false;
        gb->fwProbeDied = false;
        hasResult = true;
        lastResultSuccess = success;
    }

} // namespace gucci

namespace gucci {

namespace {

    // Pathfinder v2 step 1 -- the agency map, drawn live.
    //
    // One dot per frame at the player's position: lit when pressing would
    // have changed something from there, dim when it would not. Walk off a
    // ledge in Cube and the trail goes dim the instant you leave the ground
    // and stays dim the whole way down -- that stretch is exactly what v1
    // wastes its entire search budget inside.
    //
    // Nothing here feeds the search yet. This step exists to check the idea
    // holds up in the real game before anything is built on it.
    struct AgencyOverlay {
        static AgencyOverlay* get() {
            static AgencyOverlay inst;
            return &inst;
        }

        struct Sample {
            cocos2d::CCPoint pos;
            bool matters;
        };

        cocos2d::CCDrawNode* m_trail = nullptr;
        cocos2d::CCLabelBMFont* m_readout = nullptr;
        PlayLayer* m_attachedTo = nullptr;
        std::vector<Sample> m_samples;
        uint32_t m_lastFrame = UINT32_MAX;
        bool m_needsRedraw = false;

        // Same lesson as the 1.7 overlay crash: drop cached child pointers
        // without touching them once the PlayLayer they belonged to is gone.
        void forgetStaleNodes() {
            m_trail = nullptr;
            m_readout = nullptr;
            m_attachedTo = nullptr;
            m_samples.clear();
            m_lastFrame = UINT32_MAX;
        }

        void detach() {
            if (m_trail)
                m_trail->removeFromParent();
            if (m_readout)
                m_readout->removeFromParent();
            forgetStaleNodes();
        }

        void attach(PlayLayer* pl) {
            if (m_trail || !pl || !pl->m_objectLayer || !pl->m_uiLayer)
                return;
            m_attachedTo = pl;

            m_trail = cocos2d::CCDrawNode::create();
            m_trail->setZOrder(9000);
            pl->m_objectLayer->addChild(m_trail);

            m_readout = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
            m_readout->setScale(0.32f);
            m_readout->setAnchorPoint({0.f, 0.5f});
            m_readout->setZOrder(9000);
            auto win = cocos2d::CCDirector::sharedDirector()->getWinSize();
            m_readout->setPosition({14.f, win.height - 26.f});
            pl->m_uiLayer->addChild(m_readout);
        }

        void render(PlayLayer* pl) {
            auto* gb = GucciEngine::get();
            if (!gb->pfAgencyDebug) {
                if (m_trail)
                    detach();
                return;
            }
            if (!pl || !pl->m_player1)
                return;
            if (pl != m_attachedTo)
                forgetStaleNodes();
            if (!m_trail) {
                attach(pl);
                if (!m_trail)
                    return;
            }

            uint32_t frame = gb->updater.getFrame();
            if (frame < m_lastFrame) {   // reset or respawn -- start a new map
                m_samples.clear();
                m_needsRedraw = true;
            }
            if (gb->pfAgencyValid && frame != m_lastFrame) {
                if (m_samples.size() >= 900)
                    m_samples.erase(m_samples.begin());
                m_samples.push_back({pl->m_player1->getPosition(), gb->pfAgencyMatters});
                m_needsRedraw = true;
            }
            m_lastFrame = frame;

            if (m_needsRedraw) {
                m_needsRedraw = false;
                m_trail->clear();
                const cocos2d::ccColor4F live{0.83f, 0.69f, 0.22f, 0.95f};
                const cocos2d::ccColor4F dead{0.35f, 0.35f, 0.38f, 0.55f};
                for (auto const& sample : m_samples)
                    m_trail->drawDot(sample.pos, sample.matters ? 2.6f : 1.6f,
                                     sample.matters ? live : dead);
            }

            if (m_readout) {
                if (!gb->pfAgencyValid) {
                    m_readout->setString("");
                } else if (gb->pfAgencyMatters) {
                    m_readout->setString(
                        fmt::format("AGENCY  gap {:.2f}", gb->pfAgencyDivergence).c_str());
                    m_readout->setColor({212, 175, 55});
                } else {
                    m_readout->setString("NO AGENCY");
                    m_readout->setColor({120, 120, 128});
                }
            }
        }
    };

} // namespace

namespace gbpf {
    void renderAgencyDebug(PlayLayer* pl) {
        AgencyOverlay::get()->render(pl);
    }

    void detachAgencyDebug() {
        AgencyOverlay::get()->detach();
    }
}

} // namespace gucci
