#include "analysis/pathfinder.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

namespace gucci {

    // Tap first, then progressively longer holds. Cube only ever needs the
    // tap; Ship/Wave/Swing need real hold lengths. Frame-major ordering
    // (all holds at the frame nearest the death before moving one frame
    // earlier) because "just before the hazard" is the common case.
    static const int kHoldSet[] = {1, 3, 6, 10, 16, 24};

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
        bestFrame = 0;
        bestX = 0.f;
        bestPct = 0.f;
        hasResult = false;
        lastResultSuccess = false;
        resultInputCount = 0;
        haveCandidate = false;
        died = false;
        completed = false;

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

    void Pathfinder::noteDeath(uint32_t frame, float x) {
        if (!active || died)
            return;
        died = true;
        deathFrame = frame;
        deathX = x;
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
        size_t cap = (size_t)std::max(8, windowFrames / std::max(1, checkpointInterval) + 4);
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
            log::info("[Pathfinder] LEVEL COMPLETE after {} runs, {} committed inputs",
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
            finish(true);
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
            handleDeath();
            return;
        }

        if (checkpointInterval > 0 && f > 0 && (f % (uint32_t)checkpointInterval) == 0)
            takeRingCheckpoint(f);
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
        if (d >= top.deathFrame + (uint32_t)std::max(1, minProgressFrames)) {
            // Progress: this candidate got meaningfully further than the
            // death that created its decision point. Commit it, open a new one.
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
            log::info("[Pathfinder] press@{} hold {} got from f={} to f={} (x={:.1f}) -- committed, "
                      "{} inputs so far",
                      cur.pressFrame,
                      cur.holdFrames,
                      top.deathFrame,
                      d,
                      deathX,
                      committed.size());
            buildNodeFromDeath(d);
            startNextCandidateOrBacktrack();
            return;
        }

        // No progress -- next candidate at this decision point (or backtrack).
        startNextCandidateOrBacktrack();
    }

    void Pathfinder::buildNodeFromDeath(uint32_t d) {
        Node n;
        n.deathFrame = d;
        n.committedBefore = committed.size();

        // Candidates must start strictly after the last committed input so
        // committed holds and new presses never overlap (v1: strictly
        // sequential, non-overlapping inputs, player 1 only).
        uint32_t lastCommitted = committed.empty() ? 0 : committed.back().m_frame;
        int64_t start = std::max<int64_t>((int64_t)d - windowFrames, (int64_t)lastCommitted + 1);
        start = std::max<int64_t>(start, 1);

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

        for (int64_t f = (int64_t)d - 1; f >= start; --f)
            for (int hold : kHoldSet)
                n.cands.push_back({(uint32_t)f, hold});

        log::info("[Pathfinder] decision point @f={} : {} candidates over [{}..{}], restore {}",
                  d,
                  n.cands.size(),
                  start,
                  (int64_t)d - 1,
                  n.fullResetInstead ? std::string("full reset")
                                     : fmt::format("ckpt@f={}", n.ckpt.frame));
        stack.push_back(std::move(n));
        depth = stack.size();
    }

    void Pathfinder::startNextCandidateOrBacktrack() {
        while (true) {
            if (stack.empty()) {
                log::info("[Pathfinder] exhausted every branch after {} runs -- giving up, best {:.1f}%",
                          runs,
                          bestPct);
                finish(false);
                return;
            }
            Node& top = stack.back();
            if (top.next < top.cands.size()) {
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
        auto* gb = GucciEngine::get();
        auto* pl = PlayLayer::get();

        active = false;
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
            if (gb->replayName.empty())
                gb->replayName = "pathfinder";
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
