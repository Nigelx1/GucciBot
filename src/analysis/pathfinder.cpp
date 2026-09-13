#include "analysis/pathfinder.hpp"

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
