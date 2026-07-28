#pragma once

// ── BUILD LABEL ──────────────────────────────────────────────────────────────
// Bump this every time we ship a build to Juice so he can confirm in the log
// (first lines on startup) that he's testing the latest. Format: YYYY-MM-DD + letter.
#define GB_BUILD_LABEL "2026-07-04-e (CALC = RENDERER REGIME. [SLOPE] REND data cracked it: renderer catches f=1735 (ys=-5.425) via FAST path (fast=1, runFastLockDelta) at dt=sl->getDt()=1/60; CALC on slow path (fast=0) missed (ys=0.127) at every fixed dt. Fix: (1) useFastLockDelta returns true during fwAnalyzing (forces runFastLockDelta) -- prior -b had the right idea but patched the wrong condition (m_lockDelta&&Performance was the false gate), so -b was a no-op; -e bypasses the whole gate. (2) drawScene CALC branch feeds sl->getDt() (renderer's dt) not getPhysicsDt(). CALC now bit-identical to renderer's physics path: fast + sl->getDt(). FIXED dt -> not speedhack-sensitive. -c (real-dt) and -d (1/240 slow) were wrong turns, reverted. Verify [SLOPE]: CALC catches f=1735 (ys~-5.4) like REND. See CALC_SLOPE_EXIT.md.)"
// ─────────────────────────────────────────────────────────────────────────────
// GucciBot.hpp — GucciBot 10.0
// Master header. No slc, no glaze, no ReplayEngine.

#include <Geode/Geode.hpp>
#include <filesystem>
#include <functional>
#include <forward_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "action_types.hpp"
#include "renderer.hpp"

using namespace geode::prelude;

// BotTheme defined in gui.hpp

// ─────────────────────────────────────────────────────────────────────────────
// GucciScheduler
// ─────────────────────────────────────────────────────────────────────────────

class GucciScheduler {
public:
    using JobId = uint64_t;
    struct Job { double interval; double elapsed; std::function<void()> fn; };

    JobId schedule(double interval, std::function<void()> fn);
    void  unschedule(JobId id);
    void  reschedule(JobId id, double interval);
    void  update(float dt);

private:
    std::unordered_map<JobId, Job> m_jobs;
    JobId m_nextId = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
// GucciPracticeFix — checkpoint/practice system
// ─────────────────────────────────────────────────────────────────────────────

struct SavedCheckpointState {
    CheckpointObject* m_checkpoint  = nullptr;
    uint64_t          m_frameOffset = 0;

    // P1 state
    cocos2d::CCPoint m_p1Position;
    float  m_p1Rotation = 0.f;
    double m_p1XVel = 0.0, m_p1YVel = 0.0;
    bool   m_p1IsUpsideDown = false;
    bool   m_p1JumpBuffered = false;
    bool   m_p1IsOnGround = false;
    bool   m_p1WasOnGround = false;
    int    m_p1GameMode = 0;
    int    m_p1Left = 0, m_p1Right = 0;
    bool   m_p1ControlsDisabled = false;

    // P2 state
    cocos2d::CCPoint m_p2Position;
    float  m_p2Rotation = 0.f;
    double m_p2XVel = 0.0, m_p2YVel = 0.0;
    bool   m_p2IsUpsideDown = false;
    bool   m_p2JumpBuffered = false;
    bool   m_p2IsOnGround = false;
    int    m_p2GameMode = 0;

    // GJGameState fields
    GJGameState m_gameState;
};

struct StoredFrame {
    SavedCheckpointState state;
    uint64_t             frame = 0;
};

class GucciPracticeFix {
public:
    std::vector<SavedCheckpointState> m_savedCheckpoints;
    std::vector<StoredFrame>          m_storedFrames;
    std::vector<std::pair<CheckpointObject*, CheckpointObject*>> m_platformerCheckpoints;

    bool m_loadCheckpoint       = false;
    bool m_isBackstep           = false;
    bool m_hasDiedNormally      = false;
    bool m_shouldLoadPlatformer = false;

    // Forced state for backwards step
    SavedCheckpointState* m_forcedState = nullptr;

    void saveCurrent(CheckpointObject* cp, uint64_t frameOffset);
    void saveState(CheckpointObject* cp, uint64_t frameOffset);
    void restorePreviousFrame(std::function<void(CheckpointObject*)> loadFn);
    void applyLatest();
    void applyCheckpoint(const SavedCheckpointState& state);
    void dropLastStoredFrame();
    void clearStoredFrames() { m_storedFrames.clear(); }
    void clearPlatformer(bool full);
    bool canRestoreState() const { return m_storedFrames.size() > 1; }
    void updatePlatformerInputs(cocos2d::CCArray* queuedButtons) { (void)queuedButtons; }
    void registerBrokenObject(GameObject* obj) { (void)obj; }
};

// ─────────────────────────────────────────────────────────────────────────────
// GucciReplaySystem
// ─────────────────────────────────────────────────────────────────────────────

class GucciReplaySystem {
public:
    gb::ActionAtom m_actionAtom;
    size_t         m_inputIndex   = 0;
    uint64_t       m_startingSeed = 0;
    uint64_t       m_startingSeedThisAttempt = 0;
    uint64_t       m_shakeRandomState = 0;
    std::string    m_replayName   = "";

    bool m_mirrorInputs        = false;
    bool m_mirrorInverted      = false;
    bool m_maintainGravity     = false;
    bool m_ignoreInputs        = false;
    bool m_forceNextInput      = false;
    bool m_flipProcessingInputs = false;

    std::unordered_map<int, gb::Action> m_lastInputs;

    GucciScheduler::JobId m_autosaveJobId = 0;

    [[nodiscard]] std::optional<gb::Action> getCurrentQueuedInput() const;
    [[nodiscard]] std::optional<gb::Action> getNextInput(uint32_t frame);

    void advanceInputIndex() { m_inputIndex++; }
    void onReset(uint32_t respawnFrame, uint32_t deathFrame);  // v10.2: clip inputs after respawn frame
    void onExit() { m_inputIndex = 0; }

    bool hasFlippedControls() {
        return GameManager::get()->getGameVariable("0010");
    }
    bool playerFlipped(bool player2) { return player2 ^ hasFlippedControls(); }

    void save(const std::filesystem::path& path, bool noOverwrite = false);
    void load(const std::filesystem::path& path);
    std::filesystem::path getCurrentPath() const;
    void backupExisting(const std::filesystem::path& path);
    void createBackup();
};

// ─────────────────────────────────────────────────────────────────────────────
// GucciUpdater — Silicate's physics step engine (direct port)
// ─────────────────────────────────────────────────────────────────────────────

class GucciUpdater {
public:
    enum class LockDeltaMode : int { Performance = 0, Accuracy = 1 };

    // Timing
    double   m_tps             = 240.0;
    double   m_speedhack       = 1.0;
    double   m_tpsOverflow     = 0.0;
    bool     m_shouldRender    = true;
    bool     m_realTime        = false;
    uint32_t m_maxUPR          = 10;
    bool     m_useVisualUpdates = false;

    // Frame counter
    uint32_t m_frame              = 0;
    uint64_t m_frameOnLastAttempt = 0;
    int      savedStepCount       = 0;
    int      totalStepCount       = 1;
    int      estimatedStepCount   = 1;
    float    currentDelta         = 0.f;
    float    m_lastTfp            = 0.f;

    // Lock delta
    bool          m_lockDelta     = true;
    LockDeltaMode m_lockDeltaMode = LockDeltaMode::Accuracy;

    // Pause / frame advance
    bool m_paused       = false;
    bool m_stepOnce_    = false;
    bool m_onlyRefresh  = false;

    // Silicate features
    bool m_backwardsStepping = false;
    bool m_ssbFix            = true;
    bool m_extrapolateFrames = false;
    bool m_layoutMode        = false;
    bool m_speedhackAudio    = false;
    bool m_allowedToProcessActions = true;

    // Intentional death
    bool  m_canDie        = false;
    bool  m_inputIsDeath  = false;
    bool  m_fullReset     = false;
    bool  m_expectsDeath  = false;
    bool  m_isAutoFlipped = false;
    bool  m_predicting    = false;

    // Prevent death
    bool  m_preventDeath       = false;
    bool  m_autoFlipOnDeath    = false;
    bool  m_fullGamePrediction = false;
    float m_acceptablePrediction = 0.9f;

    // Respawn
    int m_respawnTimer = 0;
    uint32_t m_maxBackstepFrames = 60;

    // Camera / extrapolation
    cocos2d::CCPoint m_lastCameraPos;
    cocos2d::CCPoint m_currentCameraPos;
    float m_lastPlayerX    = 0.f;
    float m_currentPlayerX = 0.f;

    void* m_actionMgr = nullptr;
    std::forward_list<std::function<void(float)>> m_frozenScheduledFunctions;

    // Accessors
    double   getPhysicsDt() const { return 1.0 / m_tps; }
    float    getTimeWarp() const;
    uint32_t getFrame() const;

    void incrementFrame()     { m_frame++; }
    void resetFrame()         { m_frame = 0; }
    void setFrame(uint32_t f) { m_frame = f; }
    void setTps(double tps)   { if (tps > 0.0) m_tps = tps; }
    void setPaused(bool p)    { m_paused = p; }
    void togglePaused()       { m_paused = !m_paused; }
    bool consumeStep()        { bool s = m_stepOnce_; m_stepOnce_ = false; return s; }
    bool isPaused() const     { return m_paused; }
    bool isLockDelta() const  { return m_lockDelta; }
    bool useFastLockDelta() const;

    void calculateSteps(float dt, float targetDt);
    void breakLoop();
    void runUpdates(std::function<void(float)> update, float realDt, bool frozen);
    void runFrozenTick();
    void backwardsStep(int n = 1);
    void updateAudioSpeedhack();
    void scheduleFrozenFunction(std::function<void(float)> fn) {
        m_frozenScheduledFunctions.push_front(fn);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HudConfig
// ─────────────────────────────────────────────────────────────────────────────

struct HudConfig {
    bool  enabled    = false;
    bool  showFrame  = true;
    bool  showTPS    = false;
    bool  showX      = false;
    bool  showY      = false;
    bool  showXVel   = false;
    bool  showYVel   = false;
    bool  showRot    = false;
    bool  showState  = false;
    bool  bigFont    = false;
    float scale      = 1.0f;
    float opacity    = 1.0f;
    int   anchor     = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BotSettingsPreset
// ─────────────────────────────────────────────────────────────────────────────

struct BotSettingsPreset {
    std::string name;
    double  tps               = 240.0;
    double  speedhack         = 1.0;
    bool    lockDelta         = true;
    int     lockDeltaMode     = 0;
    bool    backwardsStepping = false;
    uint32_t maxBackstepFrames = 60;
    bool    ssbFix            = true;
    bool    extrapolateFrames = false;
    bool    preventDeath      = false;
    bool    autoFlipOnDeath   = false;
    bool    maintainGravity   = false;
    bool    mirrorInputs      = false;
    bool    noclip            = false;
    double  autosaveInterval  = 60.0;
    bool    autosaveAtInterval = false;
    bool    autosaveAtLevelEnd = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// GucciEngine — master singleton
// ─────────────────────────────────────────────────────────────────────────────

class GucciEngine {
public:
    static GucciEngine* get() { static GucciEngine inst; return &inst; }

    enum class Mode { Idle, Recording, Playing };

    // Systems
    GucciUpdater      updater;
    GucciReplaySystem replay;
    GucciPracticeFix  practiceFix;
    GucciScheduler    scheduler;
    Renderer          renderer;
    HudConfig         hud;

    // Engine state
    bool        enabled    = false;
    Mode        mode       = Mode::Idle;
    double      userTpsSaved = 0.0;   // v10.1: user's TPS saved before playback clobbers it (0 = nothing saved)
    std::string replayName = "";

    bool isIdle()      const { return mode == Mode::Idle; }
    bool isRecording() const { return mode == Mode::Recording; }
    bool isPlaying()   const { return mode == Mode::Playing; }
    void setMode(Mode m);
    bool beginResumeRecording();

    // Noclip
    bool  noclipEnabled         = false;
    bool  noclipDeathFlash      = true;
    float noclipThreshold       = 0.f;
    float noclipDeathColorR     = 1.f;
    float noclipDeathColorG     = 0.3f;
    float noclipDeathColorB     = 0.3f;
    bool  noclipDeathBlocked    = false;
    float noclipAccuracy        = 0.f;

    // v8.12 MoreHacks (safe batch)
    bool  hackHideAttempts   = false;  // hide the attempt counter label
    bool  hackHidePercentage = false;  // hide the percentage label
    bool  hackNoSpikeFlash   = false;  // suppress the white death-flash
    bool  hackAutoRetry      = false;  // auto-restart shortly after death
    float hackAutoRetryDelay = 0.5f;   // seconds before auto-retry fires
    bool  hackRespawnInstant = false;  // zero out respawn delay
    bool  hackForcePlatformer= false;  // request platformer-style controls (cosmetic toggle)
    float pendingAutoRetry   = 0.f;    // >0: seconds remaining until auto-retry fires
    bool  noclipAccuracyVisible = false;

    // Hacks
    bool showHitboxes          = false;
    bool hitboxTrail           = false;
    bool hitboxOnDeath         = false;
    int  hitboxTrailLength     = 240;
    bool pathPreview           = false;
    int  pathLength            = 240;
    bool layoutMode            = false;
    bool noMirrorEffect        = false;
    bool noMirrorRecordingOnly = false;
    bool audioPitchEnabled     = false;
    bool rngLocked             = false;
    bool protectedMode         = false;

    // Frame-window tracker (v8.7) — render entry in namespace gbfw (framewindow.cpp)
    bool  fwEnabledLive   = false;   // draw markers during live play
    bool  fwEnabledRender = false;   // draw markers into renders

    // Practice Range overlay (v10.2): world-pinned vertical green bars at each
    // macro Jump click during playback (held click = bright). Entry in gbpr.
    bool  practiceRangeEnabled = false;
    int   fwMaxWindow     = 25;      // max window shown = 2*sweep (scoop 12 -> 24); slider max + clamp enforce it
    // One analyzed click: world position + measured frame-window width.
    struct FrameWindowMark { float x; float y; int window; bool player2; uint32_t frame; float percent; };  // v10.1: percent = level progress at this click
    std::vector<FrameWindowMark> fwMarks;   // results of the last analysis
    bool  fwHasData       = false;   // analysis has been run for current macro
    // v8.8 analyzer: capture (click frame -> player world pos) during playback,
    // so the windowing pass has a real position + state per click. Filled by
    // the playback path; consumed by analyzeFrameWindows().
    struct FwClickSample { uint32_t frame; float x; float y; bool player2; bool release; };
    std::vector<FwClickSample> fwClickSamples;
    bool  fwSampling      = false;   // collect samples during this playback
    int   fwSweepRange    = 12;      // ± frames probed per click (analysis depth)
    // v10.2 (Juice's proposal): analyzer tuning options.
    int   fwMaxFramesMeasured = 240; // max frames to simulate per probe before giving up
    int   fwLookaheadDepth    = 1;   // # of subsequent macro inputs to play through before judging survival
    int   fwSimSpeed          = 1;   // simulation speed multiplier (frames advanced per tick)
    bool  fwAnalyzing     = false;   // sweep in progress: death is intercepted, not real
    bool  fwProbeDied     = false;   // set by intercepted destroyPlayer during a probe
    float fwSavedMusicVolume = 0.f;  // analysis music mute: pre-mute music-channel volume
    bool  fwMusicMuted    = false;   // analysis music mute: currently muted

    // v10.2: ASYNC analyzer state machine. The old synchronous version pumped
    // CCScheduler in a loop, which never advanced GD physics (the player stayed
    // frozen, so every probe "survived" → always 25). Physics only advances
    // through the real per-frame pipeline (getModifiedDelta → step count →
    // midhooks). So the analyzer now drives REAL frames: it plays the macro to
    // capture checkpoints at each input, then probes each input by restoring its
    // checkpoint and letting real frames play with the input shifted.
    enum class FwState { Idle, Capturing, Probing, Finishing };
    FwState fwState        = FwState::Idle;
    size_t  fwCapIndex     = 0;      // next input to capture (Capturing)
    bool    fwCkptCreatedThisFrame = false;  // [SLOPE] marker: fwTick set this when it made a checkpoint
    size_t  fwProbeClick   = 0;      // current click being probed (Probing)
    int     fwProbeShift   = 0;      // current shift under test
    int     fwProbeLow     = 0;      // best surviving negative shift so far
    int     fwProbeHigh    = 0;      // best surviving positive shift so far
    int     fwProbePhase   = 0;      // 0 = sweeping negative, 1 = sweeping positive
    int     fwProbeFrame   = 0;      // frames elapsed in the current probe run
    int     fwProbeHorizon = 16;     // frames to let play per probe
    bool    fwProbeInjected= false;  // whether the shifted input was injected yet
    uint32_t fwProbeStartFrame = 0;  // frame the current probe started at
    void  fwTick();                  // ticked once per real frame while analyzing
    void  beginProbeRun();           // restore checkpoint, arm a probe run
    void  finishProbeClick();        // record a click's window, advance
    void  fwFinishAnalysis();        // teardown + publish results
    void  cancelAnalysis();          // interrupt-safe teardown: restore macro + clear state
    void  muteAnalysisMusic();       // silence background music during the probe sweep
    void  unmuteAnalysisMusic();     // restore background music after analysis
    void  computeProbeHorizon();     // per-click probe length (to next input)
    // v10.1: live progress for the analyzer UI. fwAnalyzeProgress in [0,1],
    // fwAnalyzeStage describes what it's doing ("capturing", "probing", "done").
    float fwAnalyzeProgress = 0.0f;
    int   fwAnalyzeCur      = 0;     // current click index being processed
    int   fwAnalyzeTotal    = 0;     // total clicks to process
    bool  fwAnalyzeRunning  = false; // true while a sweep is in flight
    std::vector<StoredFrame> fwCapStack;  // v10.2: per-click checkpoints (async)
    gb::ActionAtom fwSavedAtom;           // v10.2: macro snapshot, restored after analysis (corruption guard)
    std::string fwAnalyzeStage;      // human-readable current stage
    void  analyzeFrameWindows();     // run the checkpoint sweep over fwClickSamples

    // v8.11 frame-window tiers: a range [lo,hi] -> a marker image + a sound.
    // Tiers let you map gap sizes to different visuals/audio (e.g. 1-3 = gong +
    // triangle PNG, 4-10 = bell + circle PNG). Files live in getSaveDir()/fw_assets.
    struct FrameWindowTier {
        int   lo = 1;
        int   hi = 3;
        char  imageFile[96] = "";   // PNG filename in fw_assets/ (empty = drawn ring)
        char  soundFile[96] = "";   // audio filename in fw_assets/ (empty = silent)
        float r = 1.f, g = 0.2f, b = 0.2f;  // tint / ring color
    };
    std::vector<FrameWindowTier> fwTiers;   // user-editable; empty = gradient ring default
    // Returns the first tier whose [lo,hi] contains window, or nullptr.
    FrameWindowTier* fwTierFor(int window) {
        for (auto& t : fwTiers) if (window >= t.lo && window <= t.hi) return &t;
        return nullptr;
    }

    // Autosave
    bool   autosaveAtLevelEnd  = false;
    bool   autosaveAtInterval  = false;
    double autosaveIntervalSec = 60.0;
    bool   replayBackupsEnabled = true;
    void   applyIntervalAutosave();  // v8.3: single (re)scheduling path

    // Macro list
    std::vector<std::string>         storedMacros;
    std::set<std::string>            incompatibleMacros;
    std::unordered_set<std::string>  jaMacros;
    std::unordered_set<std::string>  giddeyMacros;
    std::unordered_set<std::string>  toosiiMacros;
    std::unordered_set<std::string>  bamMacros;
    std::unordered_set<std::string>  sexyyMacros;

    // Settings presets
    std::vector<BotSettingsPreset> settingsPresets;
    void saveBotSettingsPreset(const std::string& name);
    bool loadBotSettingsPreset(const std::string& name);
    void deleteBotSettingsPreset(const std::string& name);

    // Diff viewer — compare two saved macros frame by frame (capped at 500)
    struct DiffEntry { int frame = -1; std::string description; };
    std::vector<DiffEntry> diffMacros(const std::string& a, const std::string& b);

    // Macro surgery (v8.4)
    bool trimMacro(const std::string& name, int startTick, int endTick, bool rebase);
    bool mergeMacros(const std::string& a, const std::string& b, int gapTicks);

    // Start pos
    std::string startPosWarning;

    std::string loadedMacroLevelName;  // v8.5: metadata of the loaded macro
    // Replay tooling (gui.cpp)
    void recordTpsChange(double tps);
    bool convertToBRR(const std::string& name);
    uint32_t rngSeedVal   = 0;
    bool  fastPlayback    = false;
    // v8.3: removed the float "autosaveInterval" alias and the duplicate
    // "autosaveJobId" — the GUI was editing/rescheduling these while the
    // engine scheduled and persisted autosaveIntervalSec + replay.m_autosaveJobId.

    // Level length (for noclip accuracy)
    float m_levelLength = 0.f;

    // Init
    void initialize();
    void reloadMacroList();

    std::filesystem::path getReplayDir()  const { return Mod::get()->getSaveDir() / "replays"; }
    std::filesystem::path getPresetsDir() const { return Mod::get()->getSaveDir() / "presets"; }

private:
    GucciEngine() = default;
};

// Frame-window tracker render entry (impl in framewindow.cpp)
namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRecording);
    void playTierSound(int window);
}

// Practice Range overlay render entry (impl in practicerange.cpp)
namespace gbpr {
    void renderPracticeRange(PlayLayer* pl);
}
