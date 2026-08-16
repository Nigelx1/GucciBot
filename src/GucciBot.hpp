#pragma once

#define GB_BUILD_LABEL "2026-08-16-d (1.1: moved the Trainer tab to sit right next to JMF (index 7) instead of last -- HUD/Settings/Credits shifted to 8/9/10 accordingly (names[] in both drawTabBar and drawMegaHackWindow, the switch in drawTabContent; jupiterActive's activeTab==6 check is untouched, JMF didn't move). activeTab isn't persisted so there's no stale-index migration concern. Updated about.md for the 1.1 feature set: title/intro now mention the general Trainer tab alongside JMF, and added a Trainer (Any Macro) section describing it. Compiles clean.)"

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

struct SavedCheckpointState {
    CheckpointObject* m_checkpoint  = nullptr;
    uint64_t          m_frameOffset = 0;

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

        cocos2d::CCPoint m_p2Position;
    float  m_p2Rotation = 0.f;
    double m_p2XVel = 0.0, m_p2YVel = 0.0;
    bool   m_p2IsUpsideDown = false;
    bool   m_p2JumpBuffered = false;
    bool   m_p2IsOnGround = false;
    int    m_p2GameMode = 0;

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

struct MacroPathSample {
    float p1x = 0.f, p1y = 0.f;
    float p1XVel = 0.f, p1YVel = 0.f;
    float p1Rot = 0.f;
    bool  p1OnGround = false, p1UpsideDown = false, p1Dashing = false;
    char  gamemode1 = 'C';

    float p2x = 0.f, p2y = 0.f;
    float p2XVel = 0.f, p2YVel = 0.f;
    float p2Rot = 0.f;
    bool  p2OnGround = false, p2UpsideDown = false, p2Dashing = false;
    char  gamemode2 = 'C';

    bool hasP2 = false;
};

class GucciReplaySystem {
public:
    gb::ActionAtom m_actionAtom;
    size_t         m_inputIndex   = 0;
    uint64_t       m_startingSeed = 0;
    uint64_t       m_startingSeedThisAttempt = 0;
    uint64_t       m_shakeRandomState = 0;
    std::string    m_replayName   = "";

    // Ground-truth position/gamemode per frame, captured live while recording
    // (index == frame). Used to draw the macro's path + click/release markers
    // without re-simulating anything -- see MacroPathOverlay (macropath.cpp).
    std::vector<MacroPathSample> m_pathSamples;
    bool m_pathSamplesDirty = false; // true when playback backfilled samples not yet on disk
    void savePathSamplesNow();       // writes m_pathSamples to the current macro's sidecar

    // Click-rhythm bar (Jupiter tab): press/release intervals in seconds, built
    // ONCE at load() time from the freshly-loaded action list, before any live
    // reset/respawn bookkeeping can clip or clear m_actionAtom. Deliberately a
    // separate, never-mutated copy -- reading the live m_actionAtom mid-playback
    // would tie a GUI feature to the same fragile reset logic that broke
    // intentional-death playback (see CLAUDE.md P1). m_clickBarTps is the tps
    // the macro was actually recorded at, captured alongside so the bar's timing
    // stays correct even if the user changes tps live during practice.
    std::vector<std::pair<double,double>> m_clickIntervalsSec;
    double m_clickBarTps = 240.0;
    void buildClickIntervals(double tps); // called once from load(), pairs press/release per (type,player2)

    // Trainer Mode: furthest x-position ever actually reached while this macro's
    // path overlay was active as a reference (ratchets up only, persisted
    // per-macro). Used to progressively reveal the path/markers instead of
    // showing the whole level's answer key immediately -- see macropath.cpp.
    float m_trainerBestX = 0.f;
    void saveTrainerProgressNow();

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
    void onReset(uint32_t respawnFrame, uint32_t deathFrame);
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

class GucciUpdater {
public:
    enum class LockDeltaMode : int { Performance = 0, Accuracy = 1 };

        double   m_tps             = 240.0;
    double   m_speedhack       = 1.0;
    double   m_tpsOverflow     = 0.0;
    bool     m_shouldRender    = true;
    bool     m_realTime        = false;
    uint32_t m_maxUPR          = 10;
    bool     m_useVisualUpdates = false;

        uint32_t m_frame              = 0;
    uint64_t m_frameOnLastAttempt = 0;
    int      savedStepCount       = 0;
    int      totalStepCount       = 1;
    int      estimatedStepCount   = 1;
    float    currentDelta         = 0.f;
    float    m_lastTfp            = 0.f;

        bool          m_lockDelta     = true;
    LockDeltaMode m_lockDeltaMode = LockDeltaMode::Accuracy;

        bool m_paused       = false;
    bool m_stepOnce_    = false;
    bool m_onlyRefresh  = false;

        bool m_backwardsStepping = false;
    bool m_ssbFix            = true;
    bool m_extrapolateFrames = false;
    bool m_layoutMode        = false;
    bool m_speedhackAudio    = false;
    bool m_allowedToProcessActions = true;

        bool  m_canDie        = false;
    bool  m_inputIsDeath  = false;
    bool  m_fullReset     = false;
    bool  m_expectsDeath  = false;
    bool  m_isAutoFlipped = false;
    bool  m_predicting    = false;

        bool  m_preventDeath       = false;
    bool  m_autoFlipOnDeath    = false;
    bool  m_fullGamePrediction = false;
    float m_acceptablePrediction = 0.9f;

        int m_respawnTimer = 0;
    uint32_t m_maxBackstepFrames = 60;

        cocos2d::CCPoint m_lastCameraPos;
    cocos2d::CCPoint m_currentCameraPos;
    float m_lastPlayerX    = 0.f;
    float m_currentPlayerX = 0.f;

    void* m_actionMgr = nullptr;
    std::forward_list<std::function<void(float)>> m_frozenScheduledFunctions;

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

class GucciEngine {
public:
    static GucciEngine* get() { static GucciEngine inst; return &inst; }

    enum class Mode { Idle, Recording, Playing };

        GucciUpdater      updater;
    GucciReplaySystem replay;
    GucciPracticeFix  practiceFix;
    GucciScheduler    scheduler;
    Renderer          renderer;
    HudConfig         hud;

        bool        enabled    = false;
    Mode        mode       = Mode::Idle;
    double      userTpsSaved = 0.0;
    std::string replayName = "";

    bool isIdle()      const { return mode == Mode::Idle; }
    bool isRecording() const { return mode == Mode::Recording; }
    bool isPlaying()   const { return mode == Mode::Playing; }
    void setMode(Mode m);
    bool beginResumeRecording();

        bool  noclipEnabled         = false;
    bool  noclipDeathFlash      = true;
    float noclipThreshold       = 0.f;
    float noclipDeathColorR     = 1.f;
    float noclipDeathColorG     = 0.3f;
    float noclipDeathColorB     = 0.3f;
    bool  noclipDeathBlocked    = false;
    float noclipAccuracy        = 0.f;

        bool  hackHideAttempts   = false;
    bool  hackHidePercentage = false;
    bool  hackNoSpikeFlash   = false;
    bool  hackAutoRetry      = false;
    float hackAutoRetryDelay = 0.5f;
    bool  hackRespawnInstant = false;
    bool  hackForcePlatformer= false;
    float pendingAutoRetry   = 0.f;
    bool  noclipAccuracyVisible = false;

        bool showHitboxes          = false;
    bool hitboxTrail           = false;
    bool hitboxOnDeath         = false;
    int  hitboxTrailLength     = 240;
    bool pathPreview           = false;
    int  pathLength            = 240;
    bool survivalIndicator     = false;
    int  indicatorLookahead    = 20;
    int  indicatorStyle        = 0; // 0=Ring 1=Classic 2=Converge 3=Pulse
    float indicatorOpacity     = 0.9f;
    float indicatorSafeColorR  = 0.25f, indicatorSafeColorG = 0.95f, indicatorSafeColorB = 0.35f;
    float indicatorDangerColorR= 0.95f, indicatorDangerColorG= 0.25f, indicatorDangerColorB= 0.25f;
    bool  indicatorSoundEnabled= false;
    bool  indicatorFlashEnabled= true;
    bool  accuracyHudEnabled   = false;
    int   accuracyGoodClicks   = 0;
    int   accuracyTotalClicks  = 0;
    int   currentStreak        = 0;
    int   bestStreak           = 0;

    // Macro path preview: draws the loaded macro's recorded path + click/release
    // markers (from GucciReplaySystem::m_pathSamples), independent of live prediction.
    bool  showMacroPath        = false;
    float macroPathMarkerSize  = 8.f;
    float macroPathLineOpacity = 0.6f;

    // Trainer Mode (generic mechanism, currently only surfaced via the dedicated
    // Jupiter tab): progressively reveals the path/markers only up to the furthest
    // point ever actually reached (GucciReplaySystem::m_trainerBestX), instead of
    // showing the whole recorded run immediately.
    bool  trainerRevealEnabled = true;
    float trainerRevealBuffer  = 40.f;

    // "Nigel's very special Jupiter my Favourite Trainer" -- a dedicated tab that's
    // just the above mechanisms (macro path + trainer reveal), auto-scoped to one
    // specific level, with personal notes and named segments layered on top.
    std::string jupiterNotes;
    std::string jupiterSegmentsRaw; // "label,x;label,x;..."

    // Dedicated Jupiter-only practice data (click bar timing, ghost path),
    // loaded ONCE at startup straight from its own hidden file -- see
    // loadJupiterMacroData in engine_core.cpp. Deliberately separate from
    // `replay`, which drives real bot playback/recording used throughout the
    // rest of the mod: Jupiter's data must never touch that, or the general
    // macro list/selection UI, or `mode`/`isPlaying()` -- it was doing all
    // three when it went through replay.load(), which is why loading it broke
    // the ability to actually play a macro normally afterward.
    // Generic name (not "JupiterMacroData") because it's also used by the
    // general Trainer tab (trainerMacro, below) to hold whichever of the
    // user's own macros they've picked -- same isolated, side-effect-free
    // loading approach (loadTrainerMacroData in engine_core.cpp), just
    // pointed at an arbitrary path instead of the one bundled Jupiter file.
    struct TrainerMacroData {
        bool loaded = false;
        std::string levelName; // from the macro file itself; often EMPTY for
                                // macros converted from .gdr/.json/legacy .brr
                                // -- nothing in this codebase sets BRRMacro::
                                // levelName before persisting those formats
        int32_t levelId = 0;
        std::vector<std::pair<double,double>> clickIntervalsSec;
        double clickBarTps = 240.0;
        std::vector<MacroPathSample> pathSamples;
    };
    TrainerMacroData jupiterMacro;

    // Click bar transport: pause/resume/reset/skim. Position tracked as a
    // seconds offset into the macro's timeline, advanced by real elapsed
    // wall-clock time each frame while not paused (see drawJupiterClickBar).
    // Starts paused at the beginning -- and auto-pauses back at the
    // beginning again once a full pass finishes, rather than looping
    // seamlessly forever.
    bool   jupiterClickBarPaused       = true;
    double jupiterClickBarPosSec       = 0.0;
    double jupiterClickBarLastRealTime = 0.0;

    // Loop toggle: ON wraps back to 0 and keeps playing automatically at the
    // end of a pass (clearing your own click/release marks each time, fresh
    // comparison per lap); OFF just stops at the end and leaves your marks
    // in place until you Reset or leave the tab.
    bool jupiterClickBarLoop = false;

    // True only during frames where the Click Trainer page is actually
    // rendering -- reset to false unconditionally at the top of
    // MenuInterface::drawInterface() every frame, set true only inside
    // drawJupiterClickTrainerPage. Used to gate keybinds.cpp's click-mark
    // tracking so it can't go stale the way a sticky "page open" navigation
    // flag could (see the comment in drawInterface).
    bool jupiterClickBarPageVisible = false;

    // Your own real presses -- click (mouse, via ImGui, same path menu
    // buttons already use so it's known to work) and spacebar/up arrow/W
    // (GD's standard jump bindings, via a real CCKeyboardDispatcher hook --
    // NOT ImGui::IsKeyPressed, which this GD+ImGui integration doesn't
    // reliably deliver game keys to, same reason this codebase already has
    // its own keyboard dispatcher hook in keybinds.cpp instead of relying on
    // ImGui for game-related keys). Timestamped in click-bar-timeline
    // seconds, rendered as white lines scrolling alongside the macro's own
    // (yellow) marks. Cleared on Reset, on leaving the tab, and -- only
    // while Loop is on -- at each loop reset too.
    std::vector<double> jupiterClickBarMyClicks;
    std::vector<double> jupiterClickBarMyReleases;

    // Click-rhythm bar: a fixed center line with the macro's upcoming click/hold
    // windows scrolling toward it at constant real-time speed, independent of
    // in-level speed portals.
    bool  jupiterClickBarEnabled = true;
    float jupiterClickBarWindow  = 2.f; // total seconds of window visible across the bar

    // Synced level music (resources/jupiter_music.mp3): plays while actually
    // in Jupiter My Favourite, seeked to match the current frame position
    // (frame 0 = song position 0, no offset) rather than just played once
    // from the start -- see JupiterGhostOverlay in jupiterghost.cpp.
    bool  jupiterMusicEnabled = true;
    // Manual sync correction, seconds. Positive = music plays later relative
    // to gameplay (delays the read position); negative = earlier.
    float jupiterMusicOffsetSec = 0.f;

    // Attempt/PB tracker + death heatmap: session-only (not persisted across GD
    // restarts, unlike m_trainerBestX which IS persisted per-macro). Populated
    // from PlayLayer::destroyPlayer -- see hook_playlayer.cpp.
    int   jupiterAttemptCount    = 0;
    float jupiterSessionBestPct  = 0.f;
    std::vector<float> jupiterDeathPcts;

    // Segment looping: restart back to a chosen segment's frame on death,
    // instead of the level start, while active.
    bool  jupiterLoopEnabled = false;
    int   jupiterLoopStartIdx = -1; // index into parseJupiterSegments(jupiterSegmentsRaw)
    int   jupiterLoopEndIdx   = -1;

    // Ghost overlay + scrub/jump-to-% (JupiterGhostOverlay, jupiterghost.hpp/cpp).
    // Deliberately camera/player-untouched: real teleportation would need the
    // checkpoint system or a raw position+velocity+rotation state slam, both of
    // which risk destabilizing the same fragile reset machinery flagged above --
    // this just draws a marker at the recorded (or scrubbed) position instead,
    // which is pure rendering with no gameplay-state risk at all.
    bool  jupiterGhostEnabled     = true;  // macro's ghost, from replay.m_pathSamples
    bool  jupiterBestGhostEnabled = false; // your own best-attempt ghost this session
    bool  jupiterScrubActive      = false; // true: ghosts follow jupiterScrubFrame; false: follow the live frame
    float jupiterScrubPercent     = 0.f;   // 0-100, the UI-facing scrub position

    // Click deviation readout: compares your live clicks (Click Trainer page)
    // against the nearest macro click in replay.m_clickIntervalsSec.
    bool  jupiterDeviationHolding = false; // last-seen jump-hold state, to detect press edges
    int   jupiterLastDeviationFrames = 0;  // signed: negative = early, positive = late
    bool  jupiterHasDeviationReading = false;

    // General "Trainer" tab -- same toolset as JMF (Click Trainer, Ghosts,
    // Segments, Stats, Music) but scoped to whichever ONE of the user's own
    // saved macros is currently loaded into it, swappable at will, instead
    // of hardcoded to the one bundled Jupiter macro. See loadTrainerMacro()
    // and loadTrainerMacroData() in engine_core.cpp, drawTrainerTab()/
    // drawTrainerClickTrainerPage() in gui.cpp, and trainerghost.hpp/cpp
    // (a parallel, duplicated JupiterGhostOverlay/JupiterMusicSync -- kept
    // separate rather than parameterizing the Jupiter versions, since those
    // are stateful singletons wired straight into PlayLayer's init/onQuit).
    TrainerMacroData trainerMacro;
    std::string trainerMacroName; // bare stem matching a storedMacros entry, "" = none loaded

    std::string trainerNotes;
    std::string trainerSegmentsRaw;

    bool   trainerClickBarPaused       = true;
    double trainerClickBarPosSec       = 0.0;
    double trainerClickBarLastRealTime = 0.0;
    bool   trainerClickBarLoop         = false;
    bool   trainerClickBarPageVisible  = false;
    std::vector<double> trainerClickBarMyClicks;
    std::vector<double> trainerClickBarMyReleases;
    bool  trainerClickBarEnabled = true;
    float trainerClickBarWindow  = 2.f;

    // Imported music: unlike Jupiter's bundled resources/jupiter_music.mp3,
    // this is a copy of whatever the user picks, stored at a fixed location
    // (getSaveDir()/trainer_music.mp3) so a later move/rename/delete of the
    // original file they picked can't break playback.
    bool        trainerMusicEnabled   = false;
    bool        trainerMusicImported  = false; // true once a file has been copied in
    float       trainerMusicOffsetSec = 0.f;

    int   trainerAttemptCount    = 0;
    float trainerSessionBestPct  = 0.f;
    std::vector<float> trainerDeathPcts;

    bool  trainerLoopEnabled  = false;
    int   trainerLoopStartIdx = -1;
    int   trainerLoopEndIdx   = -1;

    bool  trainerGhostEnabled     = true;
    bool  trainerBestGhostEnabled = false;
    bool  trainerScrubActive      = false;
    float trainerScrubPercent     = 0.f;

    bool  trainerDeviationHolding = false;
    int   trainerLastDeviationFrames = 0;
    bool  trainerHasDeviationReading = false;

    // Resolves stem -> a real file under getReplayDir(), parses it (GBR6 or
    // legacy BRR) into trainerMacro, and remembers the pick. false if the
    // stem can't be found/parsed (trainerMacro is left at a cleared default).
    bool loadTrainerMacro(const std::string& stem);

    bool layoutMode            = false;
    bool noMirrorEffect        = false;
    bool noMirrorRecordingOnly = false;
    bool audioPitchEnabled     = false;
    bool rngLocked             = false;
    bool protectedMode         = false;

        bool  fwEnabledLive   = false;
    bool  fwEnabledRender = false;

            bool  practiceRangeEnabled = false;
    int   fwMaxWindow     = 25;
        struct FrameWindowMark {
        float x; float y; int window; bool player2; uint32_t frame; float percent;
        // true if this entry was hand-entered/edited (or hand-edited on top of a
        // Calculate result) rather than purely Calculate-computed. analyzeFrameWindows()
        // preserves these across a fresh Calculate run instead of clearing them, and the
        // probing loop skips re-measuring any click a manual mark already covers.
        bool manual = false;
    };
    std::vector<FrameWindowMark> fwMarks;
    bool  fwHasData       = false;
                struct FwClickSample { uint32_t frame; float x; float y; bool player2; bool release; };
    std::vector<FwClickSample> fwClickSamples;
    bool  fwSampling      = false;
    int   fwSweepRange    = 12;
        int   fwMaxFramesMeasured = 240;
    int   fwLookaheadDepth    = 1;
    int   fwSimSpeed          = 1;
    bool  fwAnalyzing     = false;
    bool  fwProbeDied     = false;
    float fwSavedMusicVolume = 0.f;
    bool  fwMusicMuted    = false;

                                enum class FwState { Idle, Capturing, Probing, Finishing };
    FwState fwState        = FwState::Idle;
    size_t  fwCapIndex     = 0;
    bool    fwCkptCreatedThisFrame = false;
    size_t  fwProbeClick   = 0;
    int     fwProbeShift   = 0;
    int     fwProbeLow     = 0;
    int     fwProbeHigh    = 0;
    int     fwProbePhase   = 0;
    int     fwProbeFrame   = 0;
    int     fwProbeHorizon = 16;
    bool    fwProbeInjected= false;
    uint32_t fwProbeStartFrame = 0;
    void  fwTick();
    void  beginProbeRun();
    void  beginOrSkipProbeClick(); // advances fwProbeClick past any manually-covered clicks, then starts probing the next one (or finishes if none remain)
    bool  fwHasManualMarkAt(uint32_t frame, bool player2) const;
    void  finishProbeClick();
    void  fwFinishAnalysis();
    void  cancelAnalysis();
    void  muteAnalysisMusic();
    void  unmuteAnalysisMusic();
    void  computeProbeHorizon();
    void  saveFwMarksNow(); // persists fwMarks (Calculate results + manual entries) for the current macro
            float fwAnalyzeProgress = 0.0f;
    int   fwAnalyzeCur      = 0;
    int   fwAnalyzeTotal    = 0;
    bool  fwAnalyzeRunning  = false;
    std::vector<StoredFrame> fwCapStack;
    gb::ActionAtom fwSavedAtom;
    std::string fwAnalyzeStage;
    void  analyzeFrameWindows();

                struct FrameWindowTier {
        int   lo = 1;
        int   hi = 3;
        char  imageFile[96] = "";
        char  soundFile[96] = "";
        float r = 1.f, g = 0.2f, b = 0.2f;
    };
    std::vector<FrameWindowTier> fwTiers;
        FrameWindowTier* fwTierFor(int window) {
        for (auto& t : fwTiers) if (window >= t.lo && window <= t.hi) return &t;
        return nullptr;
    }

        bool   autosaveAtLevelEnd  = false;
    bool   autosaveAtInterval  = false;
    double autosaveIntervalSec = 60.0;
    bool   replayBackupsEnabled = true;
    void   applyIntervalAutosave();

        std::vector<std::string>         storedMacros;
    std::set<std::string>            incompatibleMacros;
    std::unordered_set<std::string>  jaMacros;
    std::unordered_set<std::string>  giddeyMacros;
    std::unordered_set<std::string>  toosiiMacros;
    std::unordered_set<std::string>  bamMacros;
    std::unordered_set<std::string>  sexyyMacros;

        std::vector<BotSettingsPreset> settingsPresets;
    void saveBotSettingsPreset(const std::string& name);
    bool loadBotSettingsPreset(const std::string& name);
    void deleteBotSettingsPreset(const std::string& name);

        struct DiffEntry { int frame = -1; std::string description; };
    std::vector<DiffEntry> diffMacros(const std::string& a, const std::string& b);

        bool trimMacro(const std::string& name, int startTick, int endTick, bool rebase);
    bool mergeMacros(const std::string& a, const std::string& b, int gapTicks);

        std::string startPosWarning;

    std::string loadedMacroLevelName;
        void recordTpsChange(double tps);
    bool convertToBRR(const std::string& name);
    uint32_t rngSeedVal   = 0;
    bool  fastPlayback    = false;

        float m_levelLength = 0.f;

        void initialize();
    void reloadMacroList();

    std::filesystem::path getReplayDir()  const { return Mod::get()->getSaveDir() / "replays"; }
    std::filesystem::path getPresetsDir() const { return Mod::get()->getSaveDir() / "presets"; }

private:
    GucciEngine() = default;
};

namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRecording);
    void playTierSound(int window);
}

namespace gbpr {
    void renderPracticeRange(PlayLayer* pl);
}
