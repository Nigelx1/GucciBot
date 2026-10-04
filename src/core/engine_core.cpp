#include "core/GucciBot.hpp"
#include "analysis/ac/framewindow.hpp"
#include "ui/ui.hpp"
#include <fmt/format.h>
#include "core/brr_format.hpp"
#include "core/gbr6_format.hpp"
#include "core/standdown.hpp"
#include "hacks/hud.hpp"
#include "tools/selfcheck.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/utils/Task.hpp>
#include <Geode/utils/web.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

using namespace geode::prelude;

namespace gucci {

    namespace fs = std::filesystem;

    GucciScheduler::JobId GucciScheduler::schedule(double interval, std::function<void()> fn) {
        JobId id = m_nextId++;
        m_jobs[id] = {interval, 0.0, fn};
        return id;
    }
    void GucciScheduler::unschedule(JobId id) {
        m_jobs.erase(id);
    }
    void GucciScheduler::reschedule(JobId id, double interval) {
        auto it = m_jobs.find(id);
        if (it != m_jobs.end())
            it->second.interval = interval;
    }
    void GucciScheduler::update(float dt) {
        for (auto& [id, job] : m_jobs) {
            job.elapsed += dt;
            if (job.elapsed >= job.interval) {
                job.elapsed = 0.0;
                job.fn();
            }
        }
    }

    // Builds a checkpoint state and hands it back WITHOUT storing it, matching
    // Silicate's PracticeFix::createCheckpoint. saveCurrent below is the
    // storing version and is unchanged in behaviour -- this is a straight
    // extraction of the capture half, so that a caller can hold a private
    // state and restore to it (resetWithState) without it entering
    // m_savedCheckpoints and confusing the normal respawn path.
    //
    // Deliberately NOT touched while extracting: what and when this captures.
    // See the checkpoint-capture-drift work -- capture timing here is its own
    // open question and must not be quietly altered by a refactor.
    SavedCheckpointState GucciPracticeFix::createCheckpoint(CheckpointObject* cp,
                                                            uint64_t frameOffset) {
        SavedCheckpointState state;
        if (!cp)
            return state;
        auto* pl = PlayLayer::get();
        if (!pl)
            return state;

        state.m_checkpoint = cp;
        state.m_frameOffset = frameOffset;
        state.m_gameState = pl->m_gameState;

        auto* p1 = pl->m_player1;
        auto* p2 = pl->m_player2;

        state.m_player1 = SavedPlayerCheckpoint::create(p1);
        state.m_player2 = SavedPlayerCheckpoint::create(p2);

        state.m_brokenObjects = m_brokenObjects;

        // Same GD fast-rand global updateRandomSeedOnReset() rewinds on
        // every reset -- see the comment on SavedCheckpointState::m_rngState
        // for why this needs to be captured per-checkpoint now.
        state.m_rngState = *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90);
        // Every other random source a replay depends on, captured with it.
        state.m_teleportRandomState = GucciEngine::get()->replay.m_teleportRandomState;
        state.m_advRandStates.clear();
        state.m_advRandStates.reserve(m_advancedRandom.size());
        for (auto const& r : m_advancedRandom)
            state.m_advRandStates.push_back(r.m_randomState ? *r.m_randomState : 0);

        // Level state, alongside the player state. Without these a restore
        // rewinds the player into a level that never rewound -- see the
        // comment on SavedCheckpointState.
        if (pl->m_effectManager)
            state.m_persistentItemMap = pl->m_effectManager->m_persistentItemCountMap;
        state.m_varianceValues = pl->m_varianceValues;
        // Captured but no longer restored -- see applyCheckpoint. Kept so the
        // data is there if the restore side is ever revisited.
        state.m_calcNonEffectObjects = pl->m_calcNonEffectObjects;
        state.m_calcNonEffectObjectsSize = pl->m_calcNonEffectObjectsSize;
        state.m_hasLevelState = true;

        return state;
    }

    void GucciPracticeFix::saveCurrent(CheckpointObject* cp, uint64_t frameOffset) {
        if (!cp)
            return;
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        // Capture lives in createCheckpoint now -- one capture, one moment.
        SavedCheckpointState state = this->createCheckpoint(cp, frameOffset);

        // A checkpoint is saved twice: provisionally the instant it is placed
        // (storeCheckpoint), then again two ticks later once that frame's
        // physics have settled, which is the accurate one. The second save
        // REPLACES the first rather than stacking a second entry for the same
        // checkpoint -- otherwise one placement would leave two entries and
        // every respawn would be a checkpoint behind.
        if (!m_savedCheckpoints.empty() && m_savedCheckpoints.back().m_checkpoint == cp) {
            m_savedCheckpoints.back() = state;
            if (!m_storedFrames.empty() && m_storedFrames.back().state.m_checkpoint == cp) {
                m_storedFrames.back().state = state;
                m_storedFrames.back().frame = frameOffset;
            }
            return;
        }

        m_savedCheckpoints.push_back(state);

        StoredFrame sf;
        sf.state = state;
        sf.frame = frameOffset;
        m_storedFrames.push_back(sf);
    }

    void GucciPracticeFix::saveState(CheckpointObject* cp, uint64_t frameOffset) {
        saveCurrent(cp, frameOffset);
    }

    // Until 2026-09-27 the per-tick Backwards Stepping save went through
    // saveState, i.e. saveCurrent -- so every tick was ALSO filed as a
    // practice checkpoint. With Backwards Stepping on, a death respawned at
    // the last tick instead of restarting (handleResetWithCheckpoints takes
    // the newest saved checkpoint); after stepping back, the newer ticks
    // stayed in that list; neither list was capped, so a full level snapshot
    // piled up 240 times a second; and every tick's retained checkpoint object
    // leaked. The "Back Step Count" setting was read by nothing. This is
    // Silicate's PracticeFix::saveState: the store alone, deduped per frame,
    // capped at m_maxBackstepFrames (0 = store nothing), oldest out first.
    void GucciPracticeFix::saveBackstepFrame(CheckpointObject* cp, uint64_t frameOffset) {
        if (!cp)
            return;
        if (!PlayLayer::get()) {
            cp->release();
            return;
        }
        size_t const cap = GucciEngine::get()->updater.m_maxBackstepFrames;
        if (cap == 0 || (!m_storedFrames.empty() && m_storedFrames.back().frame == frameOffset)) {
            cp->release();
            return;
        }
        while (m_storedFrames.size() >= cap) {
            auto& oldest = m_storedFrames.front();
            if (oldest.owned && oldest.state.m_checkpoint)
                oldest.state.m_checkpoint->release();
            m_storedFrames.erase(m_storedFrames.begin());
        }

        StoredFrame sf;
        sf.state = this->createCheckpoint(cp, frameOffset);
        sf.frame = frameOffset;
        sf.owned = true;
        m_storedFrames.push_back(sf);
    }

    void GucciPracticeFix::clearStoredFrames() {
        for (auto& f : m_storedFrames)
            if (f.owned && f.state.m_checkpoint)
                f.state.m_checkpoint->release();
        m_storedFrames.clear();
    }

    // Silicate's restorePreviousFrame: the newest stored frame is applied and
    // taken off the store (the next tick stores it again).
    //
    // This used to drop the newest and apply the one behind it, as if the
    // newest were where the game already is. It isn't: a frame is stored at
    // the START of the tick after it (earlyUpdateMidhook), so while paused
    // the newest entry is already one tick back. Every step back went two
    // ticks the first time, Prevent Death landed a tick before the tick it
    // meant, and the best-tick search one before the best tick. The frame
    // counter came from the dropped entry, so it did not match the player
    // either (handleResetWithCheckpoints now reads the entry applied here).
    void GucciPracticeFix::restorePreviousFrame(std::function<void(CheckpointObject*)> loadFn) {
        if (m_storedFrames.empty())
            return;
        StoredFrame frame = m_storedFrames.back();
        m_storedFrames.pop_back();
        if (frame.state.m_checkpoint)
            loadFn(frame.state.m_checkpoint);
        applyCheckpoint(frame.state);
        if (frame.owned && frame.state.m_checkpoint)
            frame.state.m_checkpoint->release();
    }

    void GucciPracticeFix::applyLatest() {
        if (m_savedCheckpoints.empty())
            return;
        applyCheckpoint(m_savedCheckpoints.back());
    }

    void GucciPracticeFix::applyCheckpoint(SavedCheckpointState& state) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        pl->m_gameState = state.m_gameState;

        auto* p1 = pl->m_player1;
        auto* p2 = pl->m_player2;
        if (p1)
            state.m_player1.apply(p1);
        if (p2)
            state.m_player2.apply(p2);

        // Rewind the level alongside the player. Guarded on m_hasLevelState so
        // a checkpoint captured before this existed -- one loaded from an old
        // save, or created by a path that does not fill it -- restores exactly
        // as it used to instead of stamping the level with a zeroed variance
        // table and an empty object list.
        if (state.m_hasLevelState) {
            if (pl->m_effectManager)
                pl->m_effectManager->m_persistentItemCountMap = state.m_persistentItemMap;
            pl->m_varianceValues = state.m_varianceValues;

            // m_calcNonEffectObjects is a list of RAW POINTERS into the level's
            // working set, and GD's own loadFromCheckpoint -- which runs just
            // before this -- has already rebuilt it for the frame being
            // restored to. Writing a captured copy over that replaces GD's
            // fresh list with a stale one.
            //
            // Silicate restores it, but Silicate's checkpoints are its own;
            // here GD's native restore has already done the work, so this was
            // undoing it. Suspected cause of the capture pass dying at frame
            // 193 in build -r where it previously reached 3943. The two
            // value-typed fields above are kept: they are plain data, GD does
            // not rebuild them, and they are what a restore genuinely loses.
        }
        if (GucciEngine::get()->updater.m_logFrameIncrements) {
            logFrameIncrement(
                "applyCheckpoint(restored, label)", (uint32_t)state.m_frameOffset, p1);
        }

        m_brokenObjects = state.m_brokenObjects;
        for (auto* obj : m_brokenObjects) {
            if (!obj)
                continue;
            obj->m_isDisabled = true;
            obj->m_isDisabled2 = true;
            obj->setOpacity(0.f);
        }

        // Runs after updateRandomSeedOnReset() (hook_playlayer.cpp) has
        // already rewound this to the attempt-start value earlier in the
        // same resetLevel() call -- restoring the checkpoint's own captured
        // value here is what actually fixes the per-checkpoint RNG gap.
        *reinterpret_cast<uint64_t*>(geode::base::get() + 0x6c2e90) = state.m_rngState;
        GucciEngine::get()->replay.m_teleportRandomState = state.m_teleportRandomState;
        for (size_t i = 0;
             i < state.m_advRandStates.size() && i < m_advancedRandom.size(); i++)
            if (m_advancedRandom[i].m_randomState)
                *m_advancedRandom[i].m_randomState = state.m_advRandStates[i];
    }

    void GucciPracticeFix::dropLastStoredFrame() {
        if (m_storedFrames.empty())
            return;
        if (auto& last = m_storedFrames.back(); last.owned && last.state.m_checkpoint)
            last.state.m_checkpoint->release();
        m_storedFrames.pop_back();
    }

    // Restore to one specific captured state rather than to whatever the
    // normal reset path would pick. m_forcedState is only non-null for the
    // duration of the resetLevel() call below; hook_playlayer.cpp's
    // loadFromCheckpoint and the checkpoint-array path both check it and use
    // that state instead of their usual choice. Ported from Silicate's
    // PracticeFix::resetWithState 2026-09-20 -- GucciBot had m_forcedState
    // declared but nothing ever set OR read it, so this whole mechanism was
    // dead code until now.
    void GucciPracticeFix::resetWithState(const SavedCheckpointState& state) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        m_forcedState = const_cast<SavedCheckpointState*>(&state);
        pl->resetLevel();
        m_forcedState = nullptr;
    }

    // m_advancedRandom is ported as of 2026-09-22 and cleared here. Silicate
    // also clears m_brokenOpacity, which GucciBot still does not have.
    void GucciPracticeFix::removeAll() {
        m_brokenObjects.clear();
        m_advancedRandom.clear();
        m_savedCheckpoints.clear();
    }

    void GucciPracticeFix::clearPlatformer(bool full) {
        m_platformerCheckpoints.clear();
        m_shouldLoadPlatformer = false;
        if (full) {
            m_savedCheckpoints.clear();
            this->clearStoredFrames();
        }
    }

    std::optional<gb::Action> GucciReplaySystem::getCurrentQueuedInput() const {
        if (m_inputIndex >= m_actionAtom.length())
            return std::nullopt;
        return m_actionAtom.m_actions[m_inputIndex];
    }

    std::optional<gb::Action> GucciReplaySystem::getNextInput(uint32_t frame) {
        if (m_inputIndex >= m_actionAtom.length())
            return std::nullopt;
        auto& input = m_actionAtom.m_actions[m_inputIndex];
        if (input.m_frame == frame) {
            m_inputIndex++;
            return input;
        }
        return std::nullopt;
    }

    // The macro's buttons on the respawn tick are the ones the respawned
    // attempt starts with. A button the macro still holds (its last event
    // before the respawn a press) that the respawned player is not holding --
    // a checkpoint of GD's own keeps no buttons -- is released on this tick.
    void GucciReplaySystem::releaseButtonsNotHeldAfterRespawn(uint32_t frame) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        // One lane per button of each player, as Check Macro walks them; a
        // death or restart in the macro lets go of everything.
        std::array<bool, 6> held{};
        for (auto const& a : m_actionAtom.m_actions) {
            if (a.m_type == gb::ActionType::Death || a.m_type == gb::ActionType::Restart ||
                a.m_type == gb::ActionType::RestartFull) {
                held.fill(false);
                continue;
            }
            int const button = static_cast<int>(a.m_type);
            if (button < 1 || button > 3)
                continue;
            held[(a.m_player2 ? 3 : 0) + button - 1] = a.m_holding;
        }
        bool const twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
        for (int lane = 0; lane < 6; lane++) {
            if (!held[lane])
                continue;
            bool const p2 = lane >= 3;
            int const button = lane % 3 + 1;
            // A lane's flag is stored flipped (see addInputToReplay), and it
            // only moves player 2 in a two-player level.
            auto* who = (twoPlayer && playerFlipped(p2)) ? pl->m_player2 : pl->m_player1;
            if (!who || (bool)who->m_holdingButtons[button])
                continue;
            m_actionAtom.addAction(frame, static_cast<gb::ActionType>(button), false, p2);
            log::info("[GucciBot] Recording: respawned without button {} held (lane p{}) -- "
                      "released @ frame {}",
                      button,
                      p2 ? 2 : 1,
                      frame);
        }
    }

    void GucciReplaySystem::onReset(uint32_t respawnFrame, uint32_t deathFrame) {
        auto* gb = GucciEngine::get();
        if (gb->isRecording() && !gb->fwAnalyzing) {
            size_t before = m_actionAtom.length();
            if (respawnFrame > 0) {
                m_actionAtom.clipFrom(respawnFrame + 1);
                size_t after = m_actionAtom.length();
                // A press still held at the checkpoint is part of the path
                // there: every checkpoint GucciBot restores puts the held
                // buttons back (SavedPlayerCheckpoint keeps m_holdingButtons),
                // and what the player does after the respawn is recorded as
                // usual (restoreHoldOnReset). The 1.4 cleanup for dying
                // mid-click came from before that: it removed such a press as
                // "dangling" and suppressed one release, so the replay lost the
                // hold up to the checkpoint and every later attempt from it
                // recorded a release with no press before it (issue #14; the
                // log shows a press at 159 removed on a respawn at 408). Only a
                // button the respawned player is not holding is let go of, on
                // the respawn tick, as the attempt did. A restore to an exact
                // earlier state (a Backwards Stepping step, Absense's
                // pathfinder going back) puts everything back as it was.
                bool const exactRestore =
                    gb->practiceFix.m_loadCheckpoint || gb->practiceFix.m_forcedState != nullptr;
                if (!exactRestore)
                    releaseButtonsNotHeldAfterRespawn(respawnFrame + 1);
                m_inputIndex = m_actionAtom.length();
                if (m_pathSamples.size() > (size_t)respawnFrame + 1)
                    m_pathSamples.resize((size_t)respawnFrame + 1);
                if (before != after)
                    log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                              "— deleted {} stale input(s) after checkpoint (kept {})",
                              deathFrame,
                              respawnFrame,
                              before - after,
                              after);
                else
                    log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                              "— nothing after checkpoint to delete (kept all {})",
                              deathFrame,
                              respawnFrame,
                              after);
            } else {
                m_actionAtom.m_actions.clear();
                m_pathSamples.clear();
                m_inputIndex = 0;
                log::info("[GucciBot] Recording: died@{}, full restart (no checkpoint) "
                          "— cleared {} input(s), re-recording from frame 0",
                          deathFrame,
                          before);
            }
        } else {
            if (m_actionAtom.empty()) {
                m_inputIndex = 0;
                return;
            }
            // getNextInput matches the frame EXACTLY, and playback looks a
            // frame ahead (lookupFrame = frame + 1). So an action stored at
            // frame F is dispatched while the game is at F-1 -- meaning by the
            // time the game is AT F, that action has already been applied and
            // will never be matched again.
            //
            // Starting the index at ">= respawnFrame" therefore parks it on an
            // action it can never consume, and because the index only advances
            // on a match, it jams there and every later input is blocked too.
            // Restoring to a frame that happens to hold an input silently ends
            // the macro.
            //
            // It survived because a respawn rarely lands exactly on an input
            // -- but anticroom's analyzer restores to a click's own
            // neighbourhood hundreds of times, and in dense sections the branch
            // frame IS an input frame. That is why spam sections measured
            // nothing while sparse ones were fine.
            //
            // Scoped to the analyzer for now: normal playback has the same
            // latent bug, but changing respawn behaviour for every macro needs
            // its own change and its own test.
            bool const exclusive = GucciEngine::get()->analyzerOwnsRun();
            m_inputIndex = static_cast<size_t>(std::distance(
                m_actionAtom.m_actions.begin(),
                std::find_if(m_actionAtom.m_actions.begin(),
                             m_actionAtom.m_actions.end(),
                             [respawnFrame, exclusive](const gb::Action& a) {
                                 return exclusive ? a.m_frame > respawnFrame
                                                  : a.m_frame >= respawnFrame;
                             })));
        }
    }

    fs::path GucciReplaySystem::getCurrentPath() const {
        auto* gb = GucciEngine::get();
        auto dir = gb->getReplayDir();
        for (auto& ext : ui::knownMacroExtensions()) {
            std::error_code ec;
            auto candidate = dir / (gb->replayName + ext);
            if (fs::exists(candidate, ec))
                return candidate;
        }
        return dir / (gb->replayName + ".brrr");
    }

    void GucciReplaySystem::backupExisting(const fs::path& path) {
        if (!fs::exists(path))
            return;
        auto backup = path;
        backup.replace_extension(".brrr.bak");
        std::error_code ec;
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
    }

    void GucciReplaySystem::createBackup() {
        backupExisting(getCurrentPath());
    }

    // Juice's file-organization ask: .fw/.path/.trainer sidecars used to sit
    // directly next to every macro in replays/ (macro.brrr, macro.brrr.fw,
    // macro.brrr.path, macro.brrr.trainer, times every macro you've ever
    // saved) -- "the macro folder looks atrocious". Sidecars now live in
    // their own replays/sidecars/ subfolder instead, named after the macro's
    // own filename. Scoped to relocating them, not merging the three
    // formats into one container -- Juice's own phrasing ("or at least put
    // them all in different folders") explicitly allows the lower-risk
    // version, and three independent binary formats getting merged is a lot
    // more surface area for a subtle bug than three path computations are.
    //
    // Migration is lazy and per-file: the first time a macro's sidecar is
    // touched after this update, if it's still sitting in the old flat
    // location and hasn't already been moved, it gets moved (renamed, or
    // copy+delete if rename can't cross whatever boundary is in the way)
    // into the new subfolder. No bulk migration step, no "did the migration
    // run" state to track -- every access path (save or load) goes through
    // this same function, so it self-heals the first time each macro is
    // touched.
    static fs::path sidecarPath(const fs::path& macroPath, const char* ext) {
        auto dir = macroPath.parent_path() / "sidecars";
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto newPath = dir / (macroPath.filename().string() + ext);
        auto oldPath = fs::path(macroPath.string() + ext);
        if (!fs::exists(newPath, ec) && fs::exists(oldPath, ec)) {
            fs::rename(oldPath, newPath, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(oldPath, newPath, fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    fs::remove(oldPath, ec);
            }
        }
        return newPath;
    }

    static fs::path fwSidecarPath(const fs::path& macroPath) {
        return sidecarPath(macroPath, ".fw");
    }

    // anticroom's analyzer stores results as JSON, so they get their own
    // extension rather than overwriting a 1.7.2 .fw sidecar. An old .fw stays
    // on disk untouched, which matters if this branch is ever rolled back.
    static fs::path fwAcSidecarPath(const fs::path& macroPath) {
        return sidecarPath(macroPath, ".fwac");
    }

    static fs::path pathSamplesSidecarPath(const fs::path& macroPath) {
        return sidecarPath(macroPath, ".path");
    }

    static void savePathSamples(const fs::path& macroPath,
                                const std::vector<MacroPathSample>& samples) {
        auto sc = pathSamplesSidecarPath(macroPath);
        std::error_code ec;
        if (samples.empty()) {
            fs::remove(sc, ec);
            return;
        }
        std::ofstream f(sc, std::ios::binary);
        if (!f)
            return;
        f.write("GBPS", 4);
        uint8_t ver = 3;
        f.write((const char*)&ver, 1);
        uint32_t n = (uint32_t)samples.size();
        f.write((const char*)&n, 4);
        for (auto const& s : samples) {
            f.write((const char*)&s.p1x, 4);
            f.write((const char*)&s.p1y, 4);
            f.write((const char*)&s.p1XVel, 4);
            f.write((const char*)&s.p1YVel, 4);
            f.write((const char*)&s.p1Rot, 4);
            uint8_t p1flags = (s.p1OnGround ? 1 : 0) | (s.p1UpsideDown ? 2 : 0) |
                              (s.p1Dashing ? 4 : 0) | (s.p1OrbDash ? 8 : 0) |
                              (s.p1OrbNonDash ? 16 : 0);
            f.write((const char*)&p1flags, 1);
            f.write(&s.gamemode1, 1);

            f.write((const char*)&s.p2x, 4);
            f.write((const char*)&s.p2y, 4);
            f.write((const char*)&s.p2XVel, 4);
            f.write((const char*)&s.p2YVel, 4);
            f.write((const char*)&s.p2Rot, 4);
            uint8_t p2flags = (s.p2OnGround ? 1 : 0) | (s.p2UpsideDown ? 2 : 0) |
                              (s.p2Dashing ? 4 : 0) | (s.hasP2 ? 8 : 0) | (s.p2OrbDash ? 16 : 0) |
                              (s.p2OrbNonDash ? 32 : 0);
            f.write((const char*)&p2flags, 1);
            f.write(&s.gamemode2, 1);
        }
    }

    static void loadPathSamples(const fs::path& macroPath, std::vector<MacroPathSample>& samples) {
        samples.clear();
        auto sc = pathSamplesSidecarPath(macroPath);
        if (!fs::exists(sc))
            return;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBPS", 4) != 0)
            return;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver != 3)
            return;
        uint32_t n = 0;
        f.read((char*)&n, 4);
        {
            constexpr std::streamoff kRecordBytes = 44;
            auto curPos = f.tellg();
            f.seekg(0, std::ios::end);
            auto endPos = f.tellg();
            f.seekg(curPos);
            uint64_t maxRecords =
                (curPos >= 0 && endPos > curPos) ? (uint64_t)(endPos - curPos) / kRecordBytes : 0;
            samples.reserve(std::min<uint64_t>(n, maxRecords));
        }
        for (uint32_t i = 0; i < n; ++i) {
            MacroPathSample s;
            f.read((char*)&s.p1x, 4);
            f.read((char*)&s.p1y, 4);
            f.read((char*)&s.p1XVel, 4);
            f.read((char*)&s.p1YVel, 4);
            f.read((char*)&s.p1Rot, 4);
            uint8_t p1flags = 0;
            f.read((char*)&p1flags, 1);
            s.p1OnGround = p1flags & 1;
            s.p1UpsideDown = p1flags & 2;
            s.p1Dashing = p1flags & 4;
            s.p1OrbDash = p1flags & 8;
            s.p1OrbNonDash = p1flags & 16;
            f.read(&s.gamemode1, 1);

            f.read((char*)&s.p2x, 4);
            f.read((char*)&s.p2y, 4);
            f.read((char*)&s.p2XVel, 4);
            f.read((char*)&s.p2YVel, 4);
            f.read((char*)&s.p2Rot, 4);
            uint8_t p2flags = 0;
            f.read((char*)&p2flags, 1);
            s.p2OnGround = p2flags & 1;
            s.p2UpsideDown = p2flags & 2;
            s.p2Dashing = p2flags & 4;
            s.hasP2 = p2flags & 8;
            s.p2OrbDash = p2flags & 16;
            s.p2OrbNonDash = p2flags & 32;
            f.read(&s.gamemode2, 1);

            if (!f)
                break;
            samples.push_back(s);
        }
        log::info("[GucciBot] Macro path: loaded {} sample(s) from sidecar", samples.size());
    }

    static void loadJupiterMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
        out = {};

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return;
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.empty())
            return;

        auto* legacy = BRRMacro::deserialize(bytes);
        if (!legacy)
            return;
        if (legacy->inputs.empty()) {
            delete legacy;
            return;
        }

        double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
        std::unordered_map<int, uint32_t> openPress;
        for (auto& inp : legacy->inputs) {
            int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
            if (inp.isPressed()) {
                openPress[key] = (uint32_t)inp.tick;
            } else {
                auto it = openPress.find(key);
                if (it != openPress.end()) {
                    out.clickIntervalsSec.push_back({it->second / tps, (double)inp.tick / tps});
                    openPress.erase(it);
                }
            }
        }
        out.clickBarTps = tps;
        delete legacy;

        loadPathSamples(path, out.pathSamples);
        out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
        log::info("[GucciBot] Jupiter macro data: {} click interval(s), {} path sample(s)",
                  out.clickIntervalsSec.size(),
                  out.pathSamples.size());
    }

    static void loadTrainerMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
        out = {};

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return;
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.size() < 4)
            return;

        if (std::memcmp(bytes.data(), "GBR6", 4) == 0) {
            auto result = GBR6File::deserialize(bytes.data(), bytes.size());
            if (!result)
                return;
            double tps = result->header.tps > 0.f ? (double)result->header.tps : 240.0;
            out.levelName = result->header.levelName;

            std::unordered_map<int, uint32_t> openPress;
            auto pairUp = [&](std::vector<GBR6Input> const& inputs, bool p2) {
                for (auto const& inp : inputs) {
                    int key = (int)inp.button * 2 + (p2 ? 1 : 0);
                    if (inp.pressed) {
                        openPress[key] = inp.frame;
                    } else {
                        auto it = openPress.find(key);
                        if (it != openPress.end()) {
                            out.clickIntervalsSec.push_back(
                                {it->second / tps, (double)inp.frame / tps});
                            openPress.erase(it);
                        }
                    }
                }
            };
            pairUp(result->p1Inputs, false);
            pairUp(result->p2Inputs, true);
            out.clickBarTps = tps;
        } else if (bytes.size() >= 4 && bytes[0] == 'B' && bytes[1] == 'R' && bytes[2] == 'R' &&
                   bytes[3] == '\0') {
            auto* legacy = BRRMacro::deserialize(bytes);
            if (!legacy)
                return;
            if (legacy->inputs.empty()) {
                delete legacy;
                return;
            }

            double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
            out.levelName = legacy->levelName;
            out.levelId = legacy->levelId;
            std::unordered_map<int, uint32_t> openPress;
            for (auto& inp : legacy->inputs) {
                int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
                if (inp.isPressed()) {
                    openPress[key] = (uint32_t)inp.tick;
                } else {
                    auto it = openPress.find(key);
                    if (it != openPress.end()) {
                        out.clickIntervalsSec.push_back({it->second / tps, (double)inp.tick / tps});
                        openPress.erase(it);
                    }
                }
            }
            out.clickBarTps = tps;
            delete legacy;
        } else {
            return;
        }

        loadPathSamples(path, out.pathSamples);
        out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
        log::info(
            "[GucciBot] Trainer macro data: {} click interval(s), {} path sample(s), level='{}'",
            out.clickIntervalsSec.size(),
            out.pathSamples.size(),
            out.levelName);
    }

    std::optional<int> GucciEngine::scoreRealClick(const std::vector<std::pair<double, double>>& intervals,
                                                   ClickIndicatorScore& score,
                                                   double clickTimeSec,
                                                   bool isPress,
                                                   double tps) {
        if (score.answeredPress.size() != intervals.size())
            score.reset(intervals.size());

        auto& answered = isPress ? score.answeredPress : score.answeredRelease;
        double best = 0.0;
        int bestIdx = -1;
        for (size_t i = 0; i < intervals.size(); i++) {
            if (answered[i])
                continue;
            double target = isPress ? intervals[i].first : intervals[i].second;
            double d = clickTimeSec - target;
            if (bestIdx < 0 || std::fabs(d) < std::fabs(best)) {
                best = d;
                bestIdx = (int)i;
            }
        }
        if (bestIdx < 0) {
            score.miss++;
            return std::nullopt;
        }
        answered[bestIdx] = true;

        double deltaMs = best * 1000.0;
        if (std::fabs(deltaMs) <= clickIndicatorPerfectMs)
            score.perfect++;
        else if (std::fabs(deltaMs) <= clickIndicatorOkMs)
            score.ok++;
        else
            score.miss++;

        int deltaFrames = (int)std::lround(best * tps);
        score.lastDeltaFrames = deltaFrames;
        score.hasLastReading = true;
        return deltaFrames;
    }

    bool GucciEngine::loadTrainerMacro(const std::string& stem) {
        auto dir = getReplayDir();
        fs::path found;
        for (auto& ext : ui::knownMacroExtensions()) {
            std::error_code ec;
            auto candidate = dir / (stem + ext);
            if (fs::exists(candidate, ec)) {
                found = candidate;
                break;
            }
        }
        if (found.empty())
            return false;

        loadTrainerMacroData(found, trainerMacro);
        if (!trainerMacro.loaded)
            return false;
        trainerMacroName = stem;
        Mod::get()->setSavedValue("trainer_macro_name", stem);
        log::info("[GucciBot] Trainer: loaded macro '{}'", stem);
        return true;
    }

    void GucciReplaySystem::savePathSamplesNow() {
        savePathSamples(getCurrentPath(), m_pathSamples);
        log::info("[GucciBot] Macro path: backfilled {} sample(s) saved for '{}'",
                  m_pathSamples.size(),
                  m_replayName);
    }

    static fs::path trainerSidecarPath(const fs::path& macroPath) {
        return sidecarPath(macroPath, ".trainer");
    }

    static void saveTrainerProgress(const fs::path& macroPath, float bestX) {
        std::ofstream f(trainerSidecarPath(macroPath), std::ios::binary);
        if (!f)
            return;
        f.write("GBTP", 4);
        uint8_t ver = 1;
        f.write((const char*)&ver, 1);
        f.write((const char*)&bestX, 4);
    }

    static float loadTrainerProgress(const fs::path& macroPath) {
        auto sc = trainerSidecarPath(macroPath);
        if (!fs::exists(sc))
            return 0.f;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return 0.f;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBTP", 4) != 0)
            return 0.f;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver != 1)
            return 0.f;
        float bestX = 0.f;
        f.read((char*)&bestX, 4);
        return f ? bestX : 0.f;
    }

    void GucciReplaySystem::saveTrainerProgressNow() {
        saveTrainerProgress(getCurrentPath(), m_trainerBestX);
    }

    // Results follow the macro. GucciBot's own analyzer wrote a sidecar on
    // every save and read it back on load; that went out with it in build -e,
    // so a measured macro came back blank. anticroom's analyzer has the same
    // pair -- they were simply never wired to anything.
    static void saveAcResults(const fs::path& macroPath) {
        auto& acfw = ::Bot::get()->frameWindow();
        auto const sc = fwAcSidecarPath(macroPath);
        std::error_code ec;
        if (acfw.results().empty()) {
            fs::remove(sc, ec);
            return;
        }
        if (acfw.saveResults(sc))
            log::info("[GucciBot] frame windows: saved {} result(s) alongside {}",
                      acfw.results().size(),
                      macroPath.filename().string());
    }

    static void loadAcResults(const fs::path& macroPath) {
        auto& acfw = ::Bot::get()->frameWindow();
        acfw.clear();
        auto const sc = fwAcSidecarPath(macroPath);
        if (!fs::exists(sc))
            return;
        if (acfw.loadResults(sc))
            log::info("[GucciBot] frame windows: loaded {} result(s) alongside {}",
                      acfw.results().size(),
                      macroPath.filename().string());
    }

    static void saveFwMarks(const fs::path& macroPath) {
        auto* gb = GucciEngine::get();
        auto sc = fwSidecarPath(macroPath);
        std::error_code ec;
        if (gb->fwMarks.empty()) {
            fs::remove(sc, ec);
            return;
        }
        std::ofstream f(sc, std::ios::binary);
        if (!f)
            return;
        f.write("GBFW", 4);
        uint8_t ver = 3;
        f.write((const char*)&ver, 1);
        uint32_t n = (uint32_t)gb->fwMarks.size();
        f.write((const char*)&n, 4);
        for (auto const& mk : gb->fwMarks) {
            int32_t w = mk.window;
            uint8_t p2 = mk.player2 ? 1 : 0;
            uint8_t man = mk.manual ? 1 : 0;
            uint8_t rel = mk.isRelease ? 1 : 0;
            f.write((const char*)&mk.x, 4);
            f.write((const char*)&mk.y, 4);
            f.write((const char*)&w, 4);
            f.write((const char*)&p2, 1);
            f.write((const char*)&mk.frame, 4);
            f.write((const char*)&mk.percent, 4);
            f.write((const char*)&man, 1);
            f.write((const char*)&rel, 1);
        }
    }

    static void loadFwMarks(const fs::path& macroPath) {
        auto* gb = GucciEngine::get();
        gb->fwMarks.clear();
        gb->fwHasData = false;
        auto sc = fwSidecarPath(macroPath);
        if (!fs::exists(sc))
            return;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBFW", 4) != 0)
            return;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver < 1 || ver > 3)
            return;
        uint32_t n = 0;
        f.read((char*)&n, 4);
        for (uint32_t i = 0; i < n; ++i) {
            float x = 0, y = 0, pct = 0;
            int32_t w = 0;
            uint8_t p2 = 0;
            uint32_t fr = 0;
            uint8_t man = 0;
            uint8_t rel = 0;
            f.read((char*)&x, 4);
            f.read((char*)&y, 4);
            f.read((char*)&w, 4);
            f.read((char*)&p2, 1);
            f.read((char*)&fr, 4);
            f.read((char*)&pct, 4);
            if (ver >= 2)
                f.read((char*)&man, 1);
            if (ver >= 3)
                f.read((char*)&rel, 1);
            if (!f)
                break;
            gb->fwMarks.push_back({x, y, (int)w, p2 != 0, fr, pct, man != 0, rel != 0});
        }
        gb->fwHasData = !gb->fwMarks.empty();
        log::info("[GucciBot] Frame-window: loaded {} persisted mark(s) from sidecar",
                  gb->fwMarks.size());
    }

    void GucciEngine::saveFwMarksNow() {
        saveFwMarks(replay.getCurrentPath());
        saveAcResults(replay.getCurrentPath());
    }

    bool GucciEngine::fwHasManualMarkAt(uint32_t frame, bool player2) const {
        for (auto const& mk : fwMarks)
            if (mk.manual && mk.frame == frame && mk.player2 == player2)
                return true;
        return false;
    }

    void GucciReplaySystem::save(const fs::path& path, bool noOverwrite) {
        if (noOverwrite && fs::exists(path))
            return;

        auto* gb = GucciEngine::get();
        m_replayName = gb->replayName;

        std::vector<GBR6Input> p1, p2;
        for (auto& a : m_actionAtom.m_actions) {
            if (!a.isInput())
                continue;
            GBR6Input inp;
            inp.frame = a.m_frame;
            inp.button = static_cast<uint8_t>(a.m_type);
            inp.pressed = a.m_holding;
            inp.player2 = a.m_player2;
            if (a.m_player2)
                p2.push_back(inp);
            else
                p1.push_back(inp);
        }

        GBR6Header hdr;
        hdr.tps = static_cast<float>(gb->updater.m_tps);
        hdr.name = m_replayName;
        hdr.rngSeed = m_startingSeed;
        hdr.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        if (auto* pl = PlayLayer::get()) {
            if (pl->m_level) {
                hdr.levelName = pl->m_level->m_levelName;
                hdr.flags |= GBR6_HAS_LEVELNAME;
            }
        }

        auto f = GBR6File::fromInputs(hdr, p1, p2);

        for (auto& a : m_actionAtom.m_actions) {
            auto t = a.m_type;
            if (t == gb::ActionType::Death || t == gb::ActionType::Restart ||
                t == gb::ActionType::RestartFull) {
                f.deaths.push_back({a.m_frame, static_cast<uint8_t>(t)});
            }
        }
        if (!f.deaths.empty())
            f.header.flags |= GBR6_HAS_DEATHS;

        for (auto const& a : m_actionAtom.m_actions) {
            if (!a.isInput() || !(a.m_subtick > 0.0 && a.m_subtick < 1.0))
                continue;
            f.subticks.push_back({a.m_frame,
                                  static_cast<uint8_t>(a.m_type),
                                  a.m_holding,
                                  a.m_player2,
                                  a.m_subtick});
        }
        if (!f.subticks.empty())
            f.header.flags |= GBR6_HAS_SUBTICK;

        if (!f.saveToPath(path)) {
            log::error("[GucciBot] Failed to save to {}", path.string());
        } else if (!f.deaths.empty()) {
            log::info("[GucciBot] Saved with {} intentional-death marker(s)", f.deaths.size());
        }
        saveFwMarks(path);
        saveAcResults(path);
        savePathSamples(path, m_pathSamples);
        saveTrainerProgress(path, m_trainerBestX);
    }

    void GucciReplaySystem::buildClickIntervals(double tps) {
        m_clickIntervalsSec.clear();
        m_clickBarTps = tps > 0.0 ? tps : 240.0;
        std::unordered_map<int, uint32_t> openPress;
        for (auto const& a : m_actionAtom.m_actions) {
            if (!a.isInput())
                continue;
            int key = (int)a.m_type * 2 + (a.m_player2 ? 1 : 0);
            if (a.m_holding) {
                openPress[key] = a.m_frame;
            } else {
                auto it = openPress.find(key);
                if (it != openPress.end()) {
                    m_clickIntervalsSec.push_back(
                        {it->second / m_clickBarTps, a.m_frame / m_clickBarTps});
                    openPress.erase(it);
                }
            }
        }
        log::info("[GucciBot] Click bar: built {} interval(s) at {} tps",
                  m_clickIntervalsSec.size(),
                  m_clickBarTps);
    }

    void GucciReplaySystem::load(const fs::path& path) {
        if (!fs::exists(path)) {
            log::error("[GucciBot] File not found: {}", path.string());
            return;
        }

        auto* gb = GucciEngine::get();

        char magic[4] = {};
        {
            std::ifstream f(path, std::ios::binary);
            f.read(magic, 4);
        }

        if (std::memcmp(magic, "GBR6", 4) == 0) {
            auto result = GBR6File::loadFromPath(path);
            if (!result) {
                log::error("[GucciBot] Failed to load GBR6: {}", path.string());
                return;
            }
            auto& f = *result;
            gb->updater.setTps(f.header.tps);
            m_initialTPS = f.header.tps;
            m_startingSeed = f.header.rngSeed;
            m_replayName = f.header.name;
            gb->loadedMacroLevelName = f.header.levelName;
            m_actionAtom.clear();
            m_inputIndex = 0;
            for (auto& inp : f.p1Inputs)
                m_actionAtom.addAction(
                    inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, false);
            for (auto& inp : f.p2Inputs)
                m_actionAtom.addAction(
                    inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, true);
            for (auto& d : f.deaths)
                m_actionAtom.addAction(d.frame, static_cast<gb::ActionType>(d.type), false, false);
            size_t placedSubticks = 0;
            for (auto const& st : f.subticks) {
                for (auto& a : m_actionAtom.m_actions) {
                    if (a.m_frame == st.frame && (uint8_t)a.m_type == st.button &&
                        a.m_holding == st.pressed && a.m_player2 == st.player2) {
                        a.m_subtick = st.offset;
                        placedSubticks++;
                        break;
                    }
                }
            }
            if (!f.subticks.empty())
                log::info("[GucciBot] Loaded {} sub-tick offset(s) ({} matched an input)",
                          f.subticks.size(), placedSubticks);
            std::stable_sort(m_actionAtom.m_actions.begin(), m_actionAtom.m_actions.end());
            gb->setMode(GucciEngine::Mode::Playing);
            log::info("[GucciBot] Loaded GBR6: {} inputs, {} death marker(s)",
                      m_actionAtom.length(),
                      f.deaths.size());
            loadFwMarks(path);
            loadAcResults(path);
            loadPathSamples(path, m_pathSamples);
            m_trainerBestX = loadTrainerProgress(path);
            buildClickIntervals(gb->updater.m_tps);
            return;
        }

        auto* legacy = BRRMacro::loadFromDisk(path.stem().string());
        if (legacy && !legacy->inputs.empty()) {
            m_actionAtom.clear();
            m_inputIndex = 0;
            for (auto& inp : legacy->inputs) {
                gb::ActionType t = gb::ActionType::Jump;
                if (inp.actionType == 2)
                    t = gb::ActionType::Left;
                if (inp.actionType == 3)
                    t = gb::ActionType::Right;
                m_actionAtom.addAction(
                    static_cast<uint32_t>(inp.tick), t, inp.isPressed(), inp.isPlayer2());
            }
            gb->updater.setTps(legacy->framerate);
            m_initialTPS = legacy->framerate;
            gb->setMode(GucciEngine::Mode::Playing);
            log::info("[GucciBot] Loaded legacy BRR: {} inputs", m_actionAtom.length());
            loadFwMarks(path);
            loadAcResults(path);
            m_pathSamples.clear();
            m_trainerBestX = 0.f;
            buildClickIntervals(gb->updater.m_tps);
            delete legacy;
            return;
        }
        delete legacy;
        log::error("[GucciBot] Unknown format: {}", path.string());
    }

    void GucciEngine::setMode(Mode m) {
        if (fwAnalyzing && m != Mode::Playing && fwState != FwState::Finishing) {
            cancelAnalysis();
        }
        Mode prev = mode;
        mode = m;
        if (m == Mode::Recording) {
            fwEnabledLive = false;
            fwLegendEnabled = false;
            Mod::get()->setSavedValue("fw_live", false);
            Mod::get()->setSavedValue("fw_legend", false);
        }
        if (m == Mode::Playing) {
            if (prev != Mode::Playing)
                userTpsSaved = updater.m_tps;
            replay.m_inputIndex = 0;
            fwClickSamples.clear();
            fwSampling = true;
            // Diagnostic for the "calculated correctly, failed on playback" bug
            // -- confirms what's actually in the atom the instant playback
            // starts, before blaming timing/consumption logic further.
            auto& acts = replay.m_actionAtom.m_actions;
            std::string first5;
            for (size_t i = 0; i < acts.size() && i < 5; ++i)
                first5 += fmt::format("{}({},h={}) ", acts[i].m_frame, (int)acts[i].m_type,
                                      acts[i].m_holding ? 1 : 0);
            log::info("[PLAY-START] {} actions in atom, inputIndex reset to 0, first 5: {}",
                      acts.size(),
                      first5);
        } else {
            if (prev == Mode::Playing && userTpsSaved > 0.0) {
                updater.setTps(userTpsSaved);
                userTpsSaved = 0.0;
            }
            fwSampling = false;
        }
    }

    bool GucciEngine::beginResumeRecording() {
        if (!isPlaying() || replay.m_actionAtom.empty())
            return false;
        auto* pl = PlayLayer::get();
        if (!pl)
            return false;

        uint32_t now = updater.getFrame();
        auto& atom = replay.m_actionAtom;
        atom.clipFrom(now);
        if (replay.m_pathSamples.size() > (size_t)now)
            replay.m_pathSamples.resize((size_t)now);

        bool held[2][4] = {};
        for (auto const& a : atom.m_actions)
            if (a.isInput())
                held[a.m_player2 ? 1 : 0][(uint8_t)a.m_type] = a.m_holding;

        setMode(Mode::Recording);

        for (int p = 0; p < 2; ++p)
            for (int b = 1; b <= 3; ++b)
                if (held[p][b])
                    pl->handleButton(false, b, p == 0);

        log::info(
            "[GucciBot] Resume recording @ frame {} — {} actions kept", now, atom.m_actions.size());
        return true;
    }

    void GucciEngine::reloadMacroList() {
        storedMacros.clear();
        incompatibleMacros.clear();
        jaMacros.clear();
        giddeyMacros.clear();
        toosiiMacros.clear();
        bamMacros.clear();
        sexyyMacros.clear();
        juiceMacros.clear();
        butlerMacros.clear();
        saweetieMacros.clear();
        maybachMacros.clear();
        romoMacros.clear();
        grizzleyMacros.clear();
        redKingdomMacros.clear();
        lemonadeMacros.clear();
        brrrMacros.clear();
        wakaMacros.clear();
        youngstaMacros.clear();
        knockerzMacros.clear();
        customThemeMacrosByExt.clear();

        auto dir = getReplayDir();
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
            return;
        }

        auto known = ui::knownMacroExtensions();
        std::error_code ec;
        for (auto& it : fs::directory_iterator(dir, ec)) {
            if (!it.is_regular_file())
                continue;
            auto ext = it.path().extension().string();
            auto stem = it.path().stem().string();
            if (std::find(known.begin(), known.end(), ext) != known.end()) {
                storedMacros.push_back(stem);
                if (ext == ".ja")
                    jaMacros.insert(stem);
                if (ext == ".giddey")
                    giddeyMacros.insert(stem);
                if (ext == ".toosii")
                    toosiiMacros.insert(stem);
                if (ext == ".bam")
                    bamMacros.insert(stem);
                if (ext == ".sexyy")
                    sexyyMacros.insert(stem);
                if (ext == ".juice")
                    juiceMacros.insert(stem);
                if (ext == ".butler")
                    butlerMacros.insert(stem);
                if (ext == ".saweetie")
                    saweetieMacros.insert(stem);
                if (ext == ".maybach")
                    maybachMacros.insert(stem);
                if (ext == ".romo")
                    romoMacros.insert(stem);
                if (ext == ".grizzley")
                    grizzleyMacros.insert(stem);
                if (ext == ".redkingdom")
                    redKingdomMacros.insert(stem);
                if (ext == ".lemonade")
                    lemonadeMacros.insert(stem);
                if (ext == ".icebrrr")
                    brrrMacros.insert(stem);
                if (ext == ".waka")
                    wakaMacros.insert(stem);
                if (ext == ".youngsta")
                    youngstaMacros.insert(stem);
                if (ext == ".knockerz")
                    knockerzMacros.insert(stem);
                if (!ext.empty()) {
                    std::string bare = ext.substr(1);
                    // This isBuiltin list is a SIXTH copy of the same set
                    // (customtheme.cpp's own comment on the class only
                    // names five) -- found while adding Waka/Youngsta/
                    // Knockerz, 2026-09-03. Missing this one specifically
                    // would silently register every macro saved under a
                    // new built-in extension as if it were a user custom
                    // theme instead.
                    bool isBuiltin = ext == ".brrr" || ext == ".toosii" || ext == ".ja" ||
                                     ext == ".giddey" || ext == ".bam" || ext == ".sexyy" ||
                                     ext == ".juice" || ext == ".butler" || ext == ".saweetie" ||
                                     ext == ".maybach" || ext == ".romo" || ext == ".grizzley" ||
                                     ext == ".redkingdom" || ext == ".lemonade" || ext == ".icebrrr" ||
                                     ext == ".waka" || ext == ".youngsta" || ext == ".knockerz";
                    if (!isBuiltin)
                        customThemeMacrosByExt[bare].insert(stem);
                }
            } else if (ext == ".gdr" || ext == ".xd" || ext == ".json" || ext == ".brr") {
                incompatibleMacros.insert(stem);
            }
        }
        std::sort(storedMacros.begin(), storedMacros.end());
    }

    void GucciEngine::applyIntervalAutosave() {
        if (autosaveIntervalSec < 1.0)
            autosaveIntervalSec = 1.0;
        if (replay.m_autosaveJobId == 0) {
            replay.m_autosaveJobId = scheduler.schedule(autosaveIntervalSec, [this] {
                if (autosaveAtInterval && isRecording() && !replay.m_actionAtom.empty()) {
                    auto path = replay.getCurrentPath();
                    if (replayBackupsEnabled)
                        replay.backupExisting(path);
                    replay.save(path);
                }
            });
        } else {
            scheduler.reschedule(replay.m_autosaveJobId, autosaveIntervalSec);
        }
    }

    bool GucciEngine::trimMacro(const std::string& name, int startTick, int endTick, bool rebase) {
        if (endTick <= startTick)
            return false;
        BRRMacro* m = BRRMacro::loadFromDisk(name);
        if (!m)
            return false;
        std::vector<BRRInput> kept;
        kept.reserve(m->inputs.size());
        for (auto const& in : m->inputs) {
            if (in.tick < startTick || in.tick > endTick)
                continue;
            BRRInput c = in;
            if (rebase)
                c.tick -= startTick;
            kept.push_back(c);
        }
        if (kept.empty()) {
            delete m;
            return false;
        }
        m->inputs = std::move(kept);
        m->anchors.clear();
        m->checkpoints.clear();
        m->deathFrames.clear();
        m->attemptStartTicks.clear();
        m->name = name + "_trim";
        m->persistedName.clear();
        m->persist();
        delete m;
        reloadMacroList();
        log::info("[GucciBot] Trimmed '{}' [{}..{}] -> '{}_trim'", name, startTick, endTick, name);
        return true;
    }

    bool GucciEngine::mergeMacros(const std::string& a, const std::string& b, int gapTicks) {
        BRRMacro* ma = BRRMacro::loadFromDisk(a);
        BRRMacro* mb = BRRMacro::loadFromDisk(b);
        if (!ma || !mb || ma->inputs.empty() || mb->inputs.empty()) {
            delete ma;
            delete mb;
            return false;
        }
        int32_t base = ma->inputs.back().tick + (gapTicks > 0 ? gapTicks : 0);
        for (auto in : mb->inputs) {
            in.tick += base;
            ma->inputs.push_back(in);
        }
        ma->anchors.clear();
        ma->checkpoints.clear();
        ma->deathFrames.clear();
        ma->attemptStartTicks.clear();
        ma->name = a + "_merged";
        ma->persistedName.clear();
        ma->persist();
        delete ma;
        delete mb;
        reloadMacroList();
        log::info("[GucciBot] Merged '{}' + '{}' (B offset {}) -> '{}_merged'", a, b, base, a);
        return true;
    }

    void GucciEngine::recordTpsChange(double tps) {
        if (!isRecording()) {
            log::warn("[GucciBot] recordTpsChange: not recording, ignored");
            return;
        }
        replay.m_actionAtom.addTpsChange(updater.m_frame, tps);
        log::info("[GucciBot] Recorded TPS change to {} at frame {}", tps, updater.m_frame);
    }

    namespace {

        struct GdrJsonInput {
            long long frame = 0;
            int button = 1;
            bool player2 = false;
            bool down = false;
        };

        static bool
        gdrJsonExtract(const std::string& text, double& framerate, std::vector<GdrJsonInput>& out) {
            {
                auto p = text.find("\"framerate\"");
                if (p != std::string::npos) {
                    p = text.find(':', p);
                    if (p != std::string::npos) {
                        try {
                            double fr = std::stod(text.substr(p + 1, 32));
                            if (fr > 0)
                                framerate = fr;
                        } catch (...) {
                        }
                    }
                }
            }
            auto ip = text.find("\"inputs\"");
            if (ip == std::string::npos)
                return false;
            auto arrStart = text.find('[', ip);
            if (arrStart == std::string::npos)
                return false;

            size_t i = arrStart + 1;
            int depth = 1;
            while (i < text.size() && depth > 0) {
                char c = text[i];
                if (c == '[') {
                    depth++;
                    i++;
                    continue;
                }
                if (c == ']') {
                    depth--;
                    i++;
                    continue;
                }
                if (c == '{') {
                    size_t j = i + 1;
                    int od = 1;
                    while (j < text.size() && od > 0) {
                        if (text[j] == '{')
                            od++;
                        else if (text[j] == '}')
                            od--;
                        j++;
                    }
                    std::string obj = text.substr(i, j - i);
                    auto num = [&](const char* k, double& val) -> bool {
                        auto p = obj.find(std::string("\"") + k + "\"");
                        if (p == std::string::npos)
                            return false;
                        p = obj.find(':', p);
                        if (p == std::string::npos)
                            return false;
                        try {
                            val = std::stod(obj.substr(p + 1, 32));
                        } catch (...) {
                            return false;
                        }
                        return true;
                    };
                    auto boolean = [&](const char* k, bool& val) -> bool {
                        auto p = obj.find(std::string("\"") + k + "\"");
                        if (p == std::string::npos)
                            return false;
                        p = obj.find(':', p);
                        if (p == std::string::npos)
                            return false;
                        auto rest = obj.substr(p + 1, 8);
                        if (rest.find("true") != std::string::npos) {
                            val = true;
                            return true;
                        }
                        if (rest.find("false") != std::string::npos) {
                            val = false;
                            return true;
                        }
                        try {
                            val = std::stod(rest) != 0.0;
                        } catch (...) {
                            return false;
                        }
                        return true;
                    };
                    GdrJsonInput in;
                    double v = 0;
                    bool bv = false;
                    if (num("frame", v))
                        in.frame = static_cast<long long>(v);
                    if (num("btn", v) || num("button", v))
                        in.button = static_cast<int>(v);
                    if (boolean("2p", bv) || boolean("player2", bv))
                        in.player2 = bv;
                    if (boolean("down", bv) || boolean("hold", bv) || boolean("holding", bv))
                        in.down = bv;
                    out.push_back(in);
                    i = j;
                    continue;
                }
                i++;
            }
            return !out.empty();
        }

        namespace gdrmsgpack {

            static constexpr size_t kMaxDepth = 64;

            static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth = 0);

            static bool skipN(const std::vector<uint8_t>& b, size_t& i, size_t n) {
                if (i + n > b.size())
                    return false;
                i += n;
                return true;
            }
            static bool skipContainer(
                const std::vector<uint8_t>& b, size_t& i, size_t count, bool isMap, size_t depth) {
                if (depth > kMaxDepth)
                    return false;
                size_t n = isMap ? count * 2 : count;
                for (size_t k = 0; k < n; k++)
                    if (!skipValue(b, i, depth + 1))
                        return false;
                return true;
            }
            static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth) {
                if (depth > kMaxDepth)
                    return false;
                if (i >= b.size())
                    return false;
                uint8_t t = b[i++];
                if (t <= 0x7f)
                    return true;
                if (t >= 0xe0)
                    return true;
                if (t >= 0x80 && t <= 0x8f)
                    return skipContainer(b, i, t & 0x0f, true, depth + 1);
                if (t >= 0x90 && t <= 0x9f)
                    return skipContainer(b, i, t & 0x0f, false, depth + 1);
                if (t >= 0xa0 && t <= 0xbf)
                    return skipN(b, i, t & 0x1f);
                switch (t) {
                case 0xc0:
                case 0xc2:
                case 0xc3:
                    return true;
                case 0xc4: {
                    if (i >= b.size())
                        return false;
                    uint8_t n = b[i++];
                    return skipN(b, i, n);
                }
                case 0xc5: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipN(b, i, n);
                }
                case 0xc6: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipN(b, i, n);
                }
                case 0xca:
                    return skipN(b, i, 4);
                case 0xcb:
                    return skipN(b, i, 8);
                case 0xcc:
                    return skipN(b, i, 1);
                case 0xcd:
                    return skipN(b, i, 2);
                case 0xce:
                    return skipN(b, i, 4);
                case 0xcf:
                    return skipN(b, i, 8);
                case 0xd0:
                    return skipN(b, i, 1);
                case 0xd1:
                    return skipN(b, i, 2);
                case 0xd2:
                    return skipN(b, i, 4);
                case 0xd3:
                    return skipN(b, i, 8);
                case 0xd9: {
                    if (i >= b.size())
                        return false;
                    uint8_t n = b[i++];
                    return skipN(b, i, n);
                }
                case 0xda: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipN(b, i, n);
                }
                case 0xdb: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipN(b, i, n);
                }
                case 0xdc: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipContainer(b, i, n, false, depth + 1);
                }
                case 0xdd: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipContainer(b, i, n, false, depth + 1);
                }
                case 0xde: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipContainer(b, i, n, true, depth + 1);
                }
                case 0xdf: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipContainer(b, i, n, true, depth + 1);
                }
                default:
                    return false;
                }
            }

            struct Header {
                size_t count = SIZE_MAX;
                bool isMap = false;
            };
            static Header readContainerHeader(const std::vector<uint8_t>& b, size_t& i) {
                Header h;
                if (i >= b.size())
                    return h;
                uint8_t t = b[i];
                if (t >= 0x80 && t <= 0x8f) {
                    h.count = t & 0x0f;
                    h.isMap = true;
                    i++;
                    return h;
                }
                if (t >= 0x90 && t <= 0x9f) {
                    h.count = t & 0x0f;
                    h.isMap = false;
                    i++;
                    return h;
                }
                if (t == 0xdc) {
                    if (i + 3 > b.size())
                        return h;
                    h.count = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    h.isMap = false;
                    i += 3;
                    return h;
                }
                if (t == 0xdd) {
                    if (i + 5 > b.size())
                        return h;
                    h.count = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                       ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    h.isMap = false;
                    i += 5;
                    return h;
                }
                if (t == 0xde) {
                    if (i + 3 > b.size())
                        return h;
                    h.count = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    h.isMap = true;
                    i += 3;
                    return h;
                }
                if (t == 0xdf) {
                    if (i + 5 > b.size())
                        return h;
                    h.count = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                       ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    h.isMap = true;
                    i += 5;
                    return h;
                }
                return h;
            }

            static bool readString(const std::vector<uint8_t>& b, size_t& i, std::string& out) {
                if (i >= b.size())
                    return false;
                uint8_t t = b[i];
                size_t len;
                if (t >= 0xa0 && t <= 0xbf) {
                    len = t & 0x1f;
                    i++;
                } else if (t == 0xd9) {
                    if (i + 2 > b.size())
                        return false;
                    len = b[i + 1];
                    i += 2;
                } else if (t == 0xda) {
                    if (i + 3 > b.size())
                        return false;
                    len = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                } else if (t == 0xdb) {
                    if (i + 5 > b.size())
                        return false;
                    len = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                   ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                } else
                    return false;
                if (i + len > b.size())
                    return false;
                out.assign((const char*)&b[i], len);
                i += len;
                return true;
            }

            static bool readNumber(const std::vector<uint8_t>& b, size_t& i, double& out) {
                if (i >= b.size())
                    return false;
                uint8_t t = b[i];
                if (t <= 0x7f) {
                    out = t;
                    i++;
                    return true;
                }
                if (t >= 0xe0) {
                    out = (double)(int8_t)t;
                    i++;
                    return true;
                }
                switch (t) {
                case 0xcc:
                    if (i + 2 > b.size())
                        return false;
                    out = b[i + 1];
                    i += 2;
                    return true;
                case 0xcd:
                    if (i + 3 > b.size())
                        return false;
                    out = (double)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                    return true;
                case 0xce:
                    if (i + 5 > b.size())
                        return false;
                    out = (double)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                   ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                    return true;
                case 0xcf: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t v = 0;
                    for (int k = 0; k < 8; k++)
                        v = (v << 8) | b[i + 1 + k];
                    out = (double)v;
                    i += 9;
                    return true;
                }
                case 0xd0:
                    if (i + 2 > b.size())
                        return false;
                    out = (double)(int8_t)b[i + 1];
                    i += 2;
                    return true;
                case 0xd1:
                    if (i + 3 > b.size())
                        return false;
                    out = (double)(int16_t)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                    return true;
                case 0xd2:
                    if (i + 5 > b.size())
                        return false;
                    out =
                        (double)(int32_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                          ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                    return true;
                case 0xd3: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t v = 0;
                    for (int k = 0; k < 8; k++)
                        v = (v << 8) | b[i + 1 + k];
                    out = (double)(int64_t)v;
                    i += 9;
                    return true;
                }
                case 0xca: {
                    if (i + 5 > b.size())
                        return false;
                    uint32_t u = ((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                 ((uint32_t)b[i + 3] << 8) | b[i + 4];
                    float f;
                    std::memcpy(&f, &u, 4);
                    out = f;
                    i += 5;
                    return true;
                }
                case 0xcb: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t u = 0;
                    for (int k = 0; k < 8; k++)
                        u = (u << 8) | b[i + 1 + k];
                    double d;
                    std::memcpy(&d, &u, 8);
                    out = d;
                    i += 9;
                    return true;
                }
                default:
                    return false;
                }
            }

            static bool readBool(const std::vector<uint8_t>& b, size_t& i, bool& out) {
                if (i >= b.size())
                    return false;
                if (b[i] == 0xc2) {
                    out = false;
                    i++;
                    return true;
                }
                if (b[i] == 0xc3) {
                    out = true;
                    i++;
                    return true;
                }
                double n;
                size_t save = i;
                if (readNumber(b, i, n)) {
                    out = n != 0.0;
                    return true;
                }
                i = save;
                return false;
            }

        } // namespace gdrmsgpack

        static bool gdrBinaryExtract(const std::vector<uint8_t>& bytes,
                                     double& framerate,
                                     std::vector<GdrJsonInput>& out) {
            using namespace gdrmsgpack;
            size_t i = 0;
            auto top = readContainerHeader(bytes, i);
            if (top.count == SIZE_MAX || !top.isMap)
                return false;

            for (size_t k = 0; k < top.count; k++) {
                std::string key;
                if (!readString(bytes, i, key))
                    return false;

                if (key == "inputs") {
                    auto arr = readContainerHeader(bytes, i);
                    if (arr.count == SIZE_MAX || arr.isMap)
                        return false;
                    for (size_t e = 0; e < arr.count; e++) {
                        auto obj = readContainerHeader(bytes, i);
                        if (obj.count == SIZE_MAX || !obj.isMap)
                            return false;
                        GdrJsonInput in;
                        for (size_t f = 0; f < obj.count; f++) {
                            std::string fk;
                            if (!readString(bytes, i, fk))
                                return false;
                            if (fk == "frame") {
                                double v;
                                if (readNumber(bytes, i, v))
                                    in.frame = (long long)v;
                                else
                                    return false;
                            } else if (fk == "btn" || fk == "button") {
                                double v;
                                if (readNumber(bytes, i, v))
                                    in.button = (int)v;
                                else
                                    return false;
                            } else if (fk == "2p" || fk == "player2") {
                                bool v;
                                if (readBool(bytes, i, v))
                                    in.player2 = v;
                                else
                                    return false;
                            } else if (fk == "down" || fk == "hold" || fk == "holding") {
                                bool v;
                                if (readBool(bytes, i, v))
                                    in.down = v;
                                else
                                    return false;
                            } else {
                                if (!skipValue(bytes, i))
                                    return false;
                            }
                        }
                        out.push_back(in);
                    }
                } else if (key == "framerate" || key == "fps") {
                    double v;
                    size_t save = i;
                    if (readNumber(bytes, i, v)) {
                        if (v > 0)
                            framerate = v;
                    } else {
                        i = save;
                        if (!skipValue(bytes, i))
                            return false;
                    }
                } else {
                    if (!skipValue(bytes, i))
                        return false;
                }
            }
            return !out.empty();
        }

    } // namespace

    bool GucciEngine::convertToBRR(const std::string& name) {
        auto dir = getReplayDir();
        auto isNative = [](const std::string& e) {
            auto known = ui::knownMacroExtensions();
            return std::find(known.begin(), known.end(), e) != known.end();
        };

        fs::path src;
        std::error_code ec;
        for (auto& it : fs::directory_iterator(dir, ec)) {
            if (!it.is_regular_file())
                continue;
            if (it.path().stem().string() != name)
                continue;
            if (isNative(it.path().extension().string()))
                continue;
            src = it.path();
            break;
        }
        if (src.empty()) {
            log::warn("[GucciBot] convertToBRR: no legacy file found for '{}'", name);
            return false;
        }

        std::ifstream f(src, std::ios::binary | std::ios::ate);
        if (!f) {
            log::warn("[GucciBot] convertToBRR: can't open {}", src.string());
            return false;
        }
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.empty()) {
            log::warn("[GucciBot] convertToBRR: {} is empty", src.string());
            return false;
        }

        if (auto* parsed = BRRMacro::deserialize(bytes)) {
            parsed->name = name;
            parsed->persistedName.clear();
            parsed->persist();
            delete parsed;
            reloadMacroList();
            log::info("[GucciBot] Converted '{}' (BRR payload) to native format", name);
            return true;
        }

        std::string text(bytes.begin(), bytes.end());
        if (text.find('{') != std::string::npos && text.find("\"inputs\"") != std::string::npos) {
            double framerate = 240.0;
            std::vector<GdrJsonInput> gdrInputs;
            if (gdrJsonExtract(text, framerate, gdrInputs)) {
                BRRMacro out;
                out.name = name;
                out.framerate = framerate;
                for (auto& gi : gdrInputs) {
                    BRRInput bi;
                    bi.tick = static_cast<int32_t>(std::max(0LL, gi.frame));
                    bi.actionType = static_cast<uint8_t>(std::clamp(gi.button, 1, 3));
                    bi.setPlayer2(gi.player2);
                    bi.setPressed(gi.down);
                    out.inputs.push_back(bi);
                }
                std::sort(
                    out.inputs.begin(), out.inputs.end(), [](const BRRInput& x, const BRRInput& y) {
                        return x.tick < y.tick;
                    });
                out.persist();
                reloadMacroList();
                log::info("[GucciBot] Converted '{}' (GDR JSON, {} inputs) to native format",
                          name,
                          out.inputs.size());
                return true;
            }
        }

        {
            double framerate = 240.0;
            std::vector<GdrJsonInput> gdrInputs;
            if (gdrBinaryExtract(bytes, framerate, gdrInputs)) {
                BRRMacro out;
                out.name = name;
                out.framerate = framerate;
                for (auto& gi : gdrInputs) {
                    BRRInput bi;
                    bi.tick = static_cast<int32_t>(std::max(0LL, gi.frame));
                    bi.actionType = static_cast<uint8_t>(std::clamp(gi.button, 1, 3));
                    bi.setPlayer2(gi.player2);
                    bi.setPressed(gi.down);
                    out.inputs.push_back(bi);
                }
                std::sort(
                    out.inputs.begin(), out.inputs.end(), [](const BRRInput& x, const BRRInput& y) {
                        return x.tick < y.tick;
                    });
                out.persist();
                reloadMacroList();
                log::info(
                    "[GucciBot] Converted '{}' (GDR binary/msgpack, {} inputs) to native format",
                    name,
                    out.inputs.size());
                return true;
            }
        }

        log::warn("[GucciBot] convertToBRR: unsupported format: {}", src.string());
        return false;
    }

    std::vector<GucciEngine::DiffEntry> GucciEngine::diffMacros(const std::string& a,
                                                                const std::string& b) {
        std::vector<DiffEntry> out;
        constexpr size_t kMaxDiffs = 500;

        BRRMacro* ma = BRRMacro::loadFromDisk(a);
        BRRMacro* mb = BRRMacro::loadFromDisk(b);
        if (!ma || !mb) {
            if (!ma)
                out.push_back({-1, "Could not load macro A: " + a});
            if (!mb)
                out.push_back({-1, "Could not load macro B: " + b});
            delete ma;
            delete mb;
            return out;
        }

        if (ma->framerate != mb->framerate)
            out.push_back(
                {-1, fmt::format("Framerate differs: A={} vs B={}", ma->framerate, mb->framerate)});
        if (ma->inputs.size() != mb->inputs.size())
            out.push_back({-1,
                           fmt::format("Input count differs: A={} vs B={}",
                                       ma->inputs.size(),
                                       mb->inputs.size())});

        auto describe = [](const BRRInput& in) {
            const char* btn = in.actionType == 2 ? "left" : in.actionType == 3 ? "right" : "jump";
            return fmt::format("f{} {} {} {}",
                               in.tick,
                               btn,
                               in.isPressed() ? "press" : "release",
                               in.isPlayer2() ? "P2" : "P1");
        };

        size_t n = std::min(ma->inputs.size(), mb->inputs.size());
        for (size_t i = 0; i < n && out.size() < kMaxDiffs; ++i) {
            const auto& A = ma->inputs[i];
            const auto& B = mb->inputs[i];
            if (A.tick != B.tick || A.actionType != B.actionType ||
                A.isPressed() != B.isPressed() || A.isPlayer2() != B.isPlayer2()) {
                out.push_back({static_cast<int>(A.tick),
                               fmt::format("#{}: A[{}] vs B[{}]", i, describe(A), describe(B))});
            }
        }

        if (out.size() >= kMaxDiffs) {
            out.push_back({-1, "... truncated at 500 differences."});
        } else if (ma->inputs.size() != mb->inputs.size()) {
            const auto& longer = ma->inputs.size() > mb->inputs.size() ? *ma : *mb;
            const char* tag = ma->inputs.size() > mb->inputs.size() ? "A" : "B";
            size_t extra = longer.inputs.size() - n;
            out.push_back({static_cast<int>(longer.inputs[n].tick),
                           fmt::format("{} has {} extra input(s) starting at [{}]",
                                       tag,
                                       extra,
                                       describe(longer.inputs[n]))});
        }

        delete ma;
        delete mb;
        return out;
    }

    // Every preset field in one place, so saving, reading from disk and loading
    // can't drift apart again. They had: a preset captured 16 settings, wrote 8
    // to disk (lockDeltaMode among them, never captured, so always 0) and read
    // 5 back at launch, and loading set maintain gravity to false instead of
    // the preset's value and left 6 captured settings out.
    static BotSettingsPreset capturePreset(GucciEngine& e, std::string const& name) {
        BotSettingsPreset p;
        p.name = name;
        p.tps = e.updater.m_tps;
        p.speedhack = e.updater.m_speedhack;
        p.lockDelta = e.updater.m_lockDelta;
        p.lockDeltaMode = static_cast<int>(e.updater.m_lockDeltaMode);
        p.backwardsStepping = e.updater.m_backwardsStepping;
        p.maxBackstepFrames = e.updater.m_maxBackstepFrames;
        p.ssbFix = e.updater.m_ssbFix;
        p.extrapolateFrames = e.updater.m_extrapolateFrames;
        p.preventDeath = e.updater.m_preventDeath;
        p.autoFlipOnDeath = e.updater.m_autoFlipOnDeath;
        p.maintainGravity = e.replay.m_maintainGravity;
        p.mirrorInputs = e.replay.m_mirrorInputs;
        p.noclip = e.noclipEnabled;
        p.autosaveInterval = e.autosaveIntervalSec;
        p.autosaveAtInterval = e.autosaveAtInterval;
        p.autosaveAtLevelEnd = e.autosaveAtLevelEnd;
        return p;
    }

    // Version 2 files hold every field. Older ones hold a few, and their
    // lockDeltaMode was never captured (always 0), so it is ignored there.
    static matjson::Value presetToJson(BotSettingsPreset const& p) {
        return matjson::makeObject({
            {"version", 2},
            {"name", p.name},
            {"tps", p.tps},
            {"speedhack", p.speedhack},
            {"lockDelta", p.lockDelta},
            {"lockDeltaMode", p.lockDeltaMode},
            {"backwardsStepping", p.backwardsStepping},
            {"maxBackstepFrames", static_cast<int>(p.maxBackstepFrames)},
            {"ssbFix", p.ssbFix},
            {"extrapolateFrames", p.extrapolateFrames},
            {"preventDeath", p.preventDeath},
            {"autoFlipOnDeath", p.autoFlipOnDeath},
            {"maintainGravity", p.maintainGravity},
            {"mirrorInputs", p.mirrorInputs},
            {"noclip", p.noclip},
            {"autosaveInterval", p.autosaveInterval},
            {"autosaveAtInterval", p.autosaveAtInterval},
            {"autosaveAtLevelEnd", p.autosaveAtLevelEnd},
        });
    }

    // Fields the file lacks keep what `p` already holds.
    static void presetFromJson(matjson::Value const& v, BotSettingsPreset& p) {
        auto number = [&](const char* key, double& out) {
            if (auto n = v[key].asDouble())
                out = n.unwrap();
        };
        auto flag = [&](const char* key, bool& out) {
            if (auto b = v[key].asBool())
                out = b.unwrap();
        };
        number("tps", p.tps);
        number("speedhack", p.speedhack);
        flag("lockDelta", p.lockDelta);
        if (v["version"].asInt().unwrapOr(1) >= 2) {
            double mode = p.lockDeltaMode;
            number("lockDeltaMode", mode);
            p.lockDeltaMode = std::clamp(static_cast<int>(mode), 0, 1);
        }
        flag("backwardsStepping", p.backwardsStepping);
        double frames = p.maxBackstepFrames;
        number("maxBackstepFrames", frames);
        p.maxBackstepFrames = static_cast<uint32_t>(std::clamp(frames, 1.0, 100000.0));
        flag("ssbFix", p.ssbFix);
        flag("extrapolateFrames", p.extrapolateFrames);
        flag("preventDeath", p.preventDeath);
        flag("autoFlipOnDeath", p.autoFlipOnDeath);
        flag("maintainGravity", p.maintainGravity);
        flag("mirrorInputs", p.mirrorInputs);
        flag("noclip", p.noclip);
        number("autosaveInterval", p.autosaveInterval);
        flag("autosaveAtInterval", p.autosaveAtInterval);
        flag("autosaveAtLevelEnd", p.autosaveAtLevelEnd);
    }

    void GucciEngine::saveBotSettingsPreset(const std::string& name) {
        BotSettingsPreset p = capturePreset(*this, name);
        auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(), [&](auto& x) {
            return x.name == name;
        });
        if (it != settingsPresets.end())
            *it = p;
        else
            settingsPresets.push_back(p);

        auto dir = getPresetsDir();
        fs::create_directories(dir);
        std::ofstream f(dir / (name + ".json"));
        f << presetToJson(p).dump();
    }

    bool GucciEngine::loadBotSettingsPreset(const std::string& name) {
        auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(), [&](auto& x) {
            return x.name == name;
        });
        if (it == settingsPresets.end())
            return false;
        auto const& p = *it;
        updater.setTps(p.tps);
        updater.m_speedhack = p.speedhack;
        updater.m_lockDelta = p.lockDelta;
        updater.m_lockDeltaMode = static_cast<GucciUpdater::LockDeltaMode>(std::clamp(p.lockDeltaMode, 0, 1));
        updater.m_backwardsStepping = p.backwardsStepping;
        updater.m_maxBackstepFrames = p.maxBackstepFrames;
        updater.m_ssbFix = p.ssbFix;
        updater.m_extrapolateFrames = p.extrapolateFrames;
        updater.m_preventDeath = p.preventDeath;
        updater.m_autoFlipOnDeath = p.autoFlipOnDeath;
        replay.m_maintainGravity = p.maintainGravity;
        replay.m_mirrorInputs = p.mirrorInputs;
        noclipEnabled = p.noclip;
        autosaveIntervalSec = p.autosaveInterval;
        autosaveAtInterval = p.autosaveAtInterval;
        autosaveAtLevelEnd = p.autosaveAtLevelEnd;
        applyIntervalAutosave();

        // Saved under the keys loadEngineSettings reads, so a loaded preset is
        // still in force after a restart; it used to last only until then.
        auto* mod = Mod::get();
        mod->setSavedValue<float>("eng_tick_rate", static_cast<float>(updater.m_tps));
        mod->setSavedValue<float>("eng_speed", static_cast<float>(p.speedhack));
        mod->setSavedValue<bool>("feat_lock_delta", p.lockDelta);
        mod->setSavedValue<int>("updater_lockDeltaMode", static_cast<int>(updater.m_lockDeltaMode));
        mod->setSavedValue<bool>("feat_backwards_step", p.backwardsStepping);
        mod->setSavedValue<int>("feat_back_step_count", static_cast<int>(p.maxBackstepFrames));
        mod->setSavedValue<bool>("feat_scroll_speed_fix", p.ssbFix);
        mod->setSavedValue<bool>("feat_frame_extrapolation", p.extrapolateFrames);
        mod->setSavedValue<bool>("feat_prevent_death", p.preventDeath);
        mod->setSavedValue<bool>("feat_auto_flip", p.autoFlipOnDeath);
        mod->setSavedValue<bool>("feat_maintain_gravity", p.maintainGravity);
        mod->setSavedValue<bool>("feat_mirror_inputs", p.mirrorInputs);
        mod->setSavedValue<bool>("hack_noclip", p.noclip);
        mod->setSavedValue<double>("feat_autosave_interval_sec", autosaveIntervalSec);
        mod->setSavedValue<bool>("feat_autosave_interval", p.autosaveAtInterval);
        mod->setSavedValue<bool>("feat_autosave_end", p.autosaveAtLevelEnd);
        return true;
    }

    void GucciEngine::deleteBotSettingsPreset(const std::string& name) {
        settingsPresets.erase(std::remove_if(settingsPresets.begin(),
                                             settingsPresets.end(),
                                             [&](auto& p) {
                                                 return p.name == name;
                                             }),
                              settingsPresets.end());
        std::error_code ec;
        fs::remove(getPresetsDir() / (name + ".json"), ec);
    }

    // A popup rather than a Notification: the toast widget doesn't wrap, and
    // this is a paragraph (Nigel caught it cut off, 2026-09-06).
    void GucciEngine::showStandDownNotification() {
        auto const* bot = standDownBot();
        if (!bot)
            return;
        createQuickPopup(
            "GucciBot Is Standing Down",
            fmt::format("<co>{}</c> is <cr>enabled</c>, {}", bot->name, bot->message),
            "Got It", nullptr,
            [](auto, bool) {});
    }

    // NO UPDATE CHECKER HERE ON PURPOSE -- removed in build 2026-09-13-b after
    // it crashed the game on launch for real users (v1.6.5 shipped with it).
    //
    // It was a `geode::Task<void>` coroutine doing
    // `co_await web::WebRequest().get(...)`. That mixes two different async
    // systems: `geode::Task<T>` (Task.hpp -- Handle/shared_ptr/Status) and
    // `arc` futures (`WebFuture` is an `arc::Pollable`). Awaiting an arc
    // pollable from inside a geode::Task coroutine leaves arc without a valid
    // Context/waker, and it blows up dereferencing it --
    // `arc::Context::cloneWaker` reading 0xFFFFFFFFFFFFFFFF.
    //
    // Note the crash signature is the SAME family as the file-picker crashes
    // in GitHub issues #1/#3 (`arc::Context::shouldCoopYield`, same garbage
    // pointer, same `Pollable::await_suspend` from a `geode::Task` coroutine's
    // resume). `importFwAssetFilesTask` and friends in gui.cpp are
    // `geode::Task<int>` doing `co_await file::pick/pickMany(...)`, which
    // return arc futures -- the identical mismatch. Strongly suggests the
    // re-entrancy guards shipped for those were treating a symptom.
    //
    // If an update checker comes back, don't re-await an arc future from a
    // geode::Task. Use an arc-native coroutine, the event-based
    // WebResponseEvent API, or a plain sync request off the main thread.

    // See the comment on the declaration in GucciBot.hpp for why this exists.
    //
    // `pick` prefers the canonical key and falls back to the legacy one only
    // when the canonical is absent, which is what makes this a migration rather
    // than a reset. Defaults match what MenuInterface::loadSettings() used,
    // because that loader is the one whose value actually won in the end --
    // keeping them means this change moves WHEN settings load, not what they
    // are. (Two defaults genuinely disagreed between the loaders: ssbFix
    // true/false and autosave-at-level-end false/true. The GUI's won before, so
    // the GUI's is kept here. Whether those are the RIGHT defaults is a
    // separate question and a separate change.)
    void GucciEngine::loadEngineSettings() {
        auto* mod = Mod::get();
        auto pick = [mod](auto def, const char* key, const char* legacy) {
            using T = decltype(def);
            if (!mod->hasSavedValue(key) && legacy && mod->hasSavedValue(legacy))
                return mod->getSavedValue<T>(legacy, def);
            return mod->getSavedValue<T>(key, def);
        };

        // Timing. Written as float by the GUI, so read as float -- asking for a
        // double back out of a float slot returns the default instead.
        updater.m_tps = pick(240.f, "eng_tick_rate", "updater_tps");
        updater.m_speedhack = pick(1.f, "eng_speed", "updater_speedhack");
        updater.m_lockDelta = pick(true, "feat_lock_delta", "updater_lockDelta");
        updater.m_lockDeltaMode = static_cast<GucciUpdater::LockDeltaMode>(
            pick((int)GucciUpdater::LockDeltaMode::Accuracy, "updater_lockDeltaMode", nullptr));
        updater.m_highTpsPrecision = pick(false, "updater_highTpsPrecision", nullptr);
        updater.m_speedhackAudio = pick(true, "feat_speedhack_audio", "updater_speedhackAudio");
        updater.m_inputFps = std::max(0.0, pick(0.0, "feat_input_fps", nullptr));
        // Frame pacing (Silicate's real_time / max_upr / dynamic_upr /
        // target_fps). These had no loader and no GUI -- fixed at their
        // defaults.
        updater.m_realTime = pick(false, "updater_real_time", nullptr);
        updater.m_maxUPR = (uint32_t)std::max(1, pick(10, "updater_max_upr", nullptr));
        updater.m_dynamicUpr = pick(false, "updater_dynamic_upr", nullptr);
        updater.m_fpsTarget = std::max(1.0, pick(60.0, "updater_fps_target", nullptr));

        // Playback features.
        updater.m_ssbFix = pick(false, "feat_scroll_speed_fix", "updater_ssbFix");
        updater.m_backwardsStepping = pick(false, "feat_backwards_step", "updater_backwardsStepping");
        updater.m_maxBackstepFrames = (uint32_t)pick(120, "feat_back_step_count", nullptr);
        updater.m_extrapolateFrames =
            pick(false, "feat_frame_extrapolation", "updater_extrapolateFrames");
        updater.m_preventDeath = pick(false, "feat_prevent_death", "updater_preventDeath");
        updater.m_fullGamePrediction = pick(false, "feat_prevent_death_trajectory", nullptr);
        updater.m_acceptablePrediction =
            std::clamp(pick(0.9f, "feat_best_tick_threshold", nullptr), 0.f, 1.f);
        updater.m_autoFlipOnDeath = pick(false, "feat_auto_flip", "updater_autoFlipOnDeath");
        replay.m_mirrorInputs = pick(false, "feat_mirror_inputs", "replay_mirrorInputs");
        replay.m_mirrorInverted = pick(false, "feat_mirror_inverted", nullptr);
        replay.m_maintainGravity = pick(false, "feat_maintain_gravity", "replay_maintainGravity");
        replay.m_scbfRecording = pick(false, "scbf_recording", nullptr);
        replay.m_scbfTickSplit = pick(true, "scbf_tick_split", nullptr);
        replay.m_subtickPreview = pick(false, "scbf_subtick_preview", nullptr);
        replay.m_subtickSplits = pick(24000, "scbf_subtick_splits", nullptr);

        // Hacks.
        noclipEnabled = pick(false, "hack_noclip", nullptr);
        noclipThreshold = pick(0.f, "hack_noclipThreshold", nullptr);
        noclipDeathFlash = pick(true, "hack_noclip_flash", nullptr);
        showHitboxes = pick(false, "hack_hitboxes", nullptr);
        pathPreview = pick(false, "hack_trajectory", nullptr);
        pathLength = pick(312, "hack_trajectory_len", nullptr);
        pathMovingObjects = pick(false, "hack_trajectory_moving", nullptr);
        pathMoveStepInterval = std::clamp(pick(1, "hack_trajectory_move_interval", nullptr), 1, 30);
        layoutMode = pick(false, "hack_layout_mode", "hack_layoutMode");
        noMirrorEffect = pick(false, "hack_no_mirror", "hack_noMirror");
        audioPitchEnabled = pick(true, "hack_audio_pitch", "hack_audioPitch");
        practiceRangeEnabled = pick(false, "practice_range", nullptr);

        // Saving.
        autosaveAtLevelEnd = pick(true, "feat_autosave_end", "autosave_atLevelEnd");
        autosaveAtInterval = pick(false, "feat_autosave_interval", "autosave_atInterval");
        autosaveIntervalSec = pick(180.0, "feat_autosave_interval_sec", "autosave_interval");
        replayBackupsEnabled = pick(true, "feat_replay_backups", "replay_backups");

        // HUD. The readouts' keys come from hacks/hud.hpp's list, which the
        // HUD and its card use too; the defaults are HudConfig's.
        HudConfig const hudDefaults;
        hud.enabled = pick(hudDefaults.enabled, hud::kKeyEnabled, nullptr);
        for (auto const& r : hud::kReadouts)
            hud.*r.field = pick(hudDefaults.*r.field, r.key, r.legacy);
        hud.bigFont = pick(hudDefaults.bigFont, hud::kKeyBigFont, nullptr);
        hud.scale = std::clamp(pick(hudDefaults.scale, hud::kKeyScale, nullptr), hud::kMinScale, hud::kMaxScale);
        hud.opacity =
            std::clamp(pick(hudDefaults.opacity, hud::kKeyOpacity, nullptr), hud::kMinOpacity, hud::kMaxOpacity);
        hud.anchor = std::clamp(pick(hudDefaults.anchor, hud::kKeyAnchor, nullptr), 0, hud::AnchorCount - 1);
    }

    void GucciEngine::initialize() {
        fs::create_directories(getReplayDir());
        fs::create_directories(getPresetsDir());

        {
            auto jupDir = Mod::get()->getSaveDir() / "jupiter";
            fs::create_directories(jupDir);

            auto findIn = [](fs::path const& dir, std::string const& stem) -> fs::path {
                for (auto& ext : ui::knownMacroExtensions()) {
                    std::error_code ec;
                    auto candidate = dir / (stem + ext);
                    if (fs::exists(candidate, ec))
                        return candidate;
                }
                return {};
            };

            auto hidden = findIn(jupDir, "jupiter_my_favourite");
            if (!hidden.empty())
                loadJupiterMacroData(hidden, jupiterMacro);

            if (!jupiterMacro.loaded) {
                std::error_code rmEc;
                if (!hidden.empty())
                    fs::remove(hidden, rmEc);

                auto bundled = Mod::get()->getResourcesDir() / "jupiter_my_favourite.gdr";
                std::error_code ec;
                if (fs::exists(bundled, ec)) {
                    const std::string seedStem = "__guccibot_jupiter_seed";
                    auto seedDest = getReplayDir() / (seedStem + ".gdr");
                    fs::copy_file(bundled, seedDest, fs::copy_options::overwrite_existing, ec);
                    convertToBRR(seedStem);
                    auto converted = findIn(getReplayDir(), seedStem);
                    if (!converted.empty()) {
                        auto dest =
                            jupDir / ("jupiter_my_favourite" + converted.extension().string());
                        fs::rename(converted, dest, ec);
                        if (!ec)
                            hidden = dest;
                    }
                    fs::remove(seedDest, ec);
                    reloadMacroList();
                    if (!hidden.empty())
                        loadJupiterMacroData(hidden, jupiterMacro);
                }
            }

            // Diagnostic added 2026-08-31: Nigel reported the Click Trainer's
            // Resume button doing nothing, traced to drawJupiterClickBar's
            // "No click data yet." early-out when jupiterMacro.clickIntervalsSec
            // is empty -- but a standalone byte-for-byte replica of
            // loadJupiterMacroData's own parsing logic, run against the exact
            // .brrr file sitting in his save folder, decoded it cleanly (467
            // real inputs, 233 matched click pairs, zero unmatched). So the
            // default macro file itself isn't the problem -- this records
            // what the LIVE game actually ends up with in jupiterMacro after
            // this whole block runs, dedicated file since Geode's own
            // console log isn't persisted anywhere reachable on this machine.
            std::ofstream jmLog(Mod::get()->getSaveDir() / "guccibot_jupitermacro.log",
                                std::ios::trunc);
            if (jmLog) {
                jmLog << "hidden_path_found=" << (!hidden.empty() ? hidden.string() : "<none>")
                      << "\n";
                jmLog << "jupiterMacro.loaded=" << (jupiterMacro.loaded ? 1 : 0) << "\n";
                jmLog << "jupiterMacro.clickIntervalsSec.size()="
                      << jupiterMacro.clickIntervalsSec.size() << "\n";
                jmLog << "jupiterMacro.pathSamples.size()=" << jupiterMacro.pathSamples.size()
                      << "\n";
            }
        }

        auto* mod = Mod::get();
        this->loadEngineSettings();

        std::error_code ec;
        for (auto& entry : fs::directory_iterator(getPresetsDir(), ec)) {
            if (entry.path().extension() != ".json")
                continue;
            std::ifstream fin(entry.path());
            std::string json((std::istreambuf_iterator<char>(fin)), {});
            auto parsed = matjson::parse(json);
            if (!parsed)
                continue;
            // Anything an older preset file lacks keeps the settings this
            // launch started with, rather than the struct's defaults.
            BotSettingsPreset p = capturePreset(*this, entry.path().stem().string());
            presetFromJson(parsed.unwrap(), p);
            settingsPresets.push_back(p);
        }

        applyIntervalAutosave();

        reloadMacroList();

        // Another bot GucciBot can't run alongside is enabled -- see
        // standingDown's comment in GucciBot.hpp. Asked of the mod list, not of
        // what has loaded so far: this runs while GucciBot loads, which can be
        // before the other mod has (isModLoaded would miss it).
        if (auto const* bot = standDownBot()) {
            enabled = false;
            standingDown = true;
            log::warn("[GucciBot] Standing down -- {} ({}) is enabled. Turn it off to use GucciBot.",
                      bot->name, bot->id);
            showStandDownNotification();
        } else {
            enabled = true;
            standingDown = false;
        }

        log::info("[GucciBot] ========================================");
        log::info("[GucciBot] BUILD: {} | compiled {} {}", GB_BUILD_LABEL, __DATE__, __TIME__);
        log::info("[GucciBot] ========================================");
        log::info("[GucciBot] " MOD_VERSION " initialized — {} macros", storedMacros.size());

        // Standing down, no midhooks or patches went in on purpose
        // (util_midhook, engine_updater.cpp): expect none.
        gbcheck::run(standingDown ? 0 : 5, standingDown ? 0 : 4, GBR6_VERSION, BRR_FORMAT_VERSION,
                     MOD_VERSION);
    }

    static PauseLayer* findOpenPauseLayerRecursive(CCNode* node) {
        if (!node)
            return nullptr;
        if (auto* p = typeinfo_cast<PauseLayer*>(node))
            return p;
        if (auto* kids = node->getChildren()) {
            for (auto* child : CCArrayExt<CCNode*>(kids)) {
                if (!child)
                    continue;
                if (auto* found = findOpenPauseLayerRecursive(child))
                    return found;
            }
        }
        return nullptr;
    }
    static PauseLayer* findOpenPauseLayer() {
        return findOpenPauseLayerRecursive(CCDirector::sharedDirector()->getRunningScene());
    }

    // Kept from the removed analyzer: Pathfinder uses these to silence the
    // game while it runs a headless search. They are audio helpers rather
    // than analyzer logic -- the fw* names on their state are historical.
    void GucciEngine::muteAnalysisMusic() {
        if (fwMusicMuted)
            return;
        auto* fmod = FMODAudioEngine::sharedEngine();
        if (!fmod)
            return;
        fwSavedMusicVolume = fmod->getBackgroundMusicVolume();
        fwSavedEffectsVolume = fmod->getEffectsVolume();
        fmod->setBackgroundMusicVolume(0.f);
        fmod->setEffectsVolume(0.f);
        fwMusicMuted = true;
    }

    void GucciEngine::unmuteAnalysisMusic() {
        if (!fwMusicMuted)
            return;
        auto* fmod = FMODAudioEngine::sharedEngine();
        if (fmod) {
            fmod->setBackgroundMusicVolume(fwSavedMusicVolume);
            fmod->setEffectsVolume(fwSavedEffectsVolume);
        }
        fwMusicMuted = false;
    }

    // These two keep their names because their meaning is unchanged -- start
    // and cancel a frame-window analysis -- but the analysis they drive is now
    // anticroom's. Every existing caller (setMode, the pause/quit paths, the
    // UI) keeps working without knowing which analyzer is underneath.
    void GucciEngine::saveAcFrameWindowResults() {
        auto const path = replay.getCurrentPath();
        if (path.empty())
            return;
        saveAcResults(path);
    }

    // Loading a macro already swaps its windows in (loadAcResults clears
    // first). This is for the paths that drop a macro without loading
    // another -- starting a fresh recording, deleting the loaded macro --
    // which used to leave the old macro's windows drawn over the level.
    void GucciEngine::forgetMacroWindows() {
        if (::Bot::get()->frameWindow().running())
            return;
        ::Bot::get()->frameWindow().clear();
        fwMarks.clear();
        fwHasData = false;
    }

    bool GucciEngine::analyzerOwnsRun() const {
        return ::Bot::get()->frameWindow().running();
    }

    // GitHub issue #8: releases coming back as grey "?" markers. A "?" means
    // the analyzer could not measure that click at all -- replaying the macro
    // UNSHIFTED from the checkpoint died, so there was no baseline to compare
    // shifted runs against (framewindow.cpp, mk.desynced).
    //
    // The reporter's crash log from issue #7 lists, all enabled at once:
    // syzzi.click_between_frames, toastexgd.its-all-frame-perfects,
    // c0nscious.frame_window and claude.frame-window, on top of GucciBot 1.8.
    // Two of those show up in that crash's own stack hooking the very
    // functions the analyzer drives (its-all-frame-perfects sits directly
    // above us on PlayLayer::loadFromCheckpoint).
    //
    // That breaks the analyzer by construction. It works by replaying a macro
    // from a checkpoint and requiring the replay to reproduce the capture
    // exactly; that only holds if GucciBot alone decides how many physics
    // steps a frame gets and when inputs land in them. Click Between Frames
    // splits the step itself, which 1.8 also now does internally -- two
    // splitters do not compose, and the replay steps differently from the
    // capture it is being checked against. Releases on a wave show it first
    // because a wave flips direction on hold state every sub-step.
    //
    // This is a warning, not a refusal: it is not certain to be the whole
    // cause of #8, and refusing to run would break setups that are fine. But
    // a silent grey "?" gives the user nothing to act on, which is how this
    // was reported in the first place.
    // Silicate's "CBF fix" (bot/bot.cpp, also in Absense), made polite.
    //
    // Syzzi's Click Between Frames and chizz's Superb Input Precision both
    // apply inputs themselves, part-way through a tick, instead of through
    // GD's button queue -- which is where GucciBot records and replays them.
    // With either active, a macro can come out missing clicks or play them on
    // other ticks. Silicate sets CBF's soft toggle ("Disable CBF") and turns
    // its physics bypass off, and disables Superb Input Precision, at every
    // launch, for good.
    //
    // Here they are paused only while GucciBot is recording or playing, and
    // put back exactly as they were the moment it goes idle -- people play
    // with CBF and only sometimes use the bot. CBF applies both settings live
    // (listenForSettingChanges, and its toggleMod handles a mid-attempt
    // switch), so no restart is involved. What they were is kept in saved
    // values, so a crash mid-recording is undone on the next launch: the
    // first frame is idle, and idle restores.
    void GucciEngine::syncInputMods() {
        bool const want = enabled && mode != Mode::Idle;
        // The first call always runs, so a pause a crash left behind is
        // undone on the first frame.
        if (inputModsSynced && want == inputModsPausedState)
            return;
        inputModsSynced = true;
        inputModsPausedState = want;

        auto* self = Mod::get();
        auto* cbfMod = Loader::get()->getInstalledMod("syzzi.click_between_frames");
        auto* sip = Loader::get()->getInstalledMod("chizz.superb-input-precision");
        bool const paused = self->getSavedValue<bool>("inputmods_paused", false);

        if (want && !paused) {
            std::string what;
            if (cbfMod) {
                bool const soft = cbfMod->getSettingValue<bool>("soft-toggle");
                bool const bypass = cbfMod->getSettingValue<bool>("physics-bypass");
                self->setSavedValue("inputmods_cbf_soft", soft);
                self->setSavedValue("inputmods_cbf_bypass", bypass);
                if (!soft || bypass) {
                    cbfMod->setSettingValue<bool>("soft-toggle", true);
                    cbfMod->setSettingValue<bool>("physics-bypass", false);
                    what = "Click Between Frames";
                }
            }
            if (sip) {
                bool const on = sip->getSettingValue<bool>("mod-enabled");
                self->setSavedValue("inputmods_sip_enabled", on);
                if (on) {
                    sip->setSettingValue<bool>("mod-enabled", false);
                    what += what.empty() ? "Superb Input Precision" : " and Superb Input Precision";
                }
            }
            self->setSavedValue("inputmods_paused", true);
            inputModsPaused = what;
            if (!what.empty())
                log::info("[GucciBot] paused {} while recording/playing", what);
        } else if (!want && paused) {
            if (cbfMod) {
                cbfMod->setSettingValue<bool>(
                    "soft-toggle", self->getSavedValue<bool>("inputmods_cbf_soft", false));
                cbfMod->setSettingValue<bool>(
                    "physics-bypass", self->getSavedValue<bool>("inputmods_cbf_bypass", false));
            }
            if (sip)
                sip->setSettingValue<bool>(
                    "mod-enabled", self->getSavedValue<bool>("inputmods_sip_enabled", true));
            self->setSavedValue("inputmods_paused", false);
            if (!inputModsPaused.empty())
                log::info("[GucciBot] put {} back as it was", inputModsPaused);
            inputModsPaused.clear();
        }
    }

    const std::string& GucciEngine::analyzerConflicts() {
        if (fwAcConflictsChecked) return fwAcConflicts;
        fwAcConflictsChecked = true;

        static constexpr std::pair<const char*, const char*> kKnown[] = {
            {"syzzi.click_between_frames",
             "splits the physics step, which GucciBot 1.8 also does itself"},
            {"toastexgd.its-all-frame-perfects",
             "hooks the same checkpoint and reset path the analyzer drives"},
            {"c0nscious.frame_window", "is another frame-window analyzer"},
            {"claude.frame-window", "is another frame-window analyzer"},
            {"peony.silicate",
             "is the engine GucciBot is built from, hooking the same functions twice"},
            {"zilko.xdbot", "is another macro bot replaying inputs"},
        };

        std::string out;
        for (auto const& [id, why] : kKnown) {
            if (!Loader::get()->isModLoaded(id)) continue;
            if (!out.empty()) out += "\n";
            out += fmt::format("  - {} ({})", id, why);
        }
        if (!out.empty())
            fwAcConflicts = fmt::format(
                "These mods are enabled and change how frames are stepped, so "
                "Calculate's numbers may be wrong and some clicks may come back "
                "as a grey \"?\" it could not measure:\n{}\n"
                "Disable them before measuring.",
                out);
        return fwAcConflicts;
    }

    void GucciEngine::analyzeFrameWindows() {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        // Starting a run from behind an open pause menu crashes: the analyzer
        // immediately resets the level and steps physics, which GD does not
        // expect while a PauseLayer is up holding references into the scene.
        // His start() checks six things but not this one. GucciBot's own
        // analyzer dismissed the menu first, and that handling was removed
        // along with it -- restored here, in front of his start(), so
        // Calculate works from the pause menu the way it always did.
        if (auto* pause = findOpenPauseLayer()) {
            pause->onResume(nullptr);
            log::info("[GucciBot] frame windows: dismissed the pause menu before analysis");
        }
        pl->m_isPaused = false;

        auto const r = ::Bot::get()->frameWindow().start(pl);
        if (auto const& conflicts = this->analyzerConflicts(); !conflicts.empty()) {
            log::warn("[GucciBot] frame windows: {}", conflicts);
            fwEngineLog(fmt::format("[fw][conflict] {}", conflicts));
        }
        fwAcReport = r.message;
        fwAcOk = r.ok;
        log::info("[GucciBot] frame windows: start ok={} msg={}", r.ok, r.message);
    }

    void GucciEngine::cancelAnalysis() {
        auto& acfw = ::Bot::get()->frameWindow();
        if (!acfw.running())
            return;
        acfw.cancel();
        fwAcReport = acfw.status();
        fwAcOk = false;
    }

    // GucciBot's own frame-window analyzer lived here and was removed
    // 2026-09-20: anticroom's analyzer (src/analysis/ac/) replaces it
    // wholesale rather than sitting alongside it. See git history at
    // build 2026-09-20-d for the last version that had both.

} // namespace gucci
