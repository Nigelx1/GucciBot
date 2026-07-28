#pragma once

#define GB_BUILD_LABEL "2026-07-28-e (SLOPE-EXIT: hijack theory CONCLUSIVELY DISPROVEN. guccibot_hijack.log came back with 18.3M lines (5M during an actual Calculate run, pathPreview/survivalIndicator on the whole time) and ZERO isRealPlayer=1 -- the ghost-sim flag never once overlaps with real-player collision handling. Three theories now eliminated with real data: dt/substep mismatch (-b), checkpoint/probe-restart (-c), ghost-sim hijack (-d). No 4th hypothesis queued. Removed all the disproven-theory instrumentation (hijack log + sim=/pv=/si= slope-log fields) so it stops generating gigabytes per session for no reason -- guccibot_slope.log is back to its original 2026-07-04 format. Original CALC_SLOPE_EXIT.md symptom (CALC undershoots a y-launch impulse REND/PLAY both catch, e.g. Bloodbath f=1718) remains unexplained. Do not stack a 4th guess on top blind -- see chat for what's actually been verified vs still open.)"

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
        struct FrameWindowMark { float x; float y; int window; bool player2; uint32_t frame; float percent; };
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
    void  finishProbeClick();
    void  fwFinishAnalysis();
    void  cancelAnalysis();
    void  muteAnalysisMusic();
    void  unmuteAnalysisMusic();
    void  computeProbeHorizon();
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
