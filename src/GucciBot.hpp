#pragma once

#define GB_BUILD_LABEL                                                                            \
    "2026-08-26-b (clang-format pass, no functional change -- GWDdoS's exact provided config, "   \
    "added "                                                                                      \
    "as .clang-format at the repo root. One real gotcha found and fixed: PointerAlignment: Left " \
    "alone doesn't reliably apply on declarations clang-format finds ambiguous -- it falls back " \
    "to "                                                                                         \
    "DERIVING alignment from the file's own existing (inconsistent) style instead, a known "      \
    "clang-format quirk, not a mistake in the given config. Added DerivePointerAlignment: false " \
    "so "                                                                                         \
    "Left actually applies consistently; verified on a small file (FMOD::ChannelGroup* master, "  \
    "not "                                                                                        \
    "*master) before running codebase-wide. Ran clang-format -i across every .cpp/.hpp/.h under " \
    "src/. Namespace+folder reorg (also requested, GWDdoS: both, grouped by feature not layer) "  \
    "is a "                                                                                       \
    "separate, much larger, much riskier change -- attempted next if there's room tonight, its "  \
    "own "                                                                                        \
    "build+commit either way, not bundled with this one. Global-externs question from earlier "   \
    "wasn't answered yet, left untouched. Compiles clean, NOT yet confirmed in-game.)"

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
#include "checkpoint_player.hpp"

using namespace geode::prelude;

namespace FMOD {
    class ChannelGroup;
}

void logFrameIncrement(const char* callSite, uint32_t frame, PlayerObject* p = nullptr);
void logCalcDeathTrace(const std::string& line);

class GucciScheduler {
public:
    using JobId = uint64_t;
    struct Job {
        double interval;
        double elapsed;
        std::function<void()> fn;
    };

    JobId schedule(double interval, std::function<void()> fn);
    void unschedule(JobId id);
    void reschedule(JobId id, double interval);
    void update(float dt);

private:
    std::unordered_map<JobId, Job> m_jobs;
    JobId m_nextId = 1;
};

struct SavedCheckpointState {
    CheckpointObject* m_checkpoint = nullptr;
    uint64_t m_frameOffset = 0;

    SavedPlayerCheckpoint m_player1;
    SavedPlayerCheckpoint m_player2;

    GJGameState m_gameState;

    std::vector<GameObject*> m_brokenObjects;
};

struct StoredFrame {
    SavedCheckpointState state;
    uint64_t frame = 0;
};

class GucciPracticeFix {
public:
    std::vector<SavedCheckpointState> m_savedCheckpoints;
    std::vector<StoredFrame> m_storedFrames;
    std::vector<std::pair<CheckpointObject*, CheckpointObject*>> m_platformerCheckpoints;

    bool m_loadCheckpoint = false;
    bool m_isBackstep = false;
    bool m_hasDiedNormally = false;
    bool m_shouldLoadPlatformer = false;

    SavedCheckpointState* m_forcedState = nullptr;

    // Live, cumulative list of objects GD has destroyed during the current
    // attempt (hook_gjbasegamelayer.cpp's destroyObject calls
    // registerBrokenObject on every one). Each checkpoint freezes its own
    // copy into SavedCheckpointState::m_brokenObjects; applyCheckpoint()
    // re-neutralizes (disabled+invisible) everything in that frozen list
    // after every restore. A destroyed object can't be trusted to behave
    // correctly if a restore brings level state back to before its
    // destruction. registerBrokenObject was a silent no-op stub for a long
    // time -- detection fired constantly and did nothing -- don't let this
    // regress back to that.
    std::vector<GameObject*> m_brokenObjects;

    // Deferred checkpoint capture -- see storeCheckpoint (hook_playlayer.cpp)
    // and frameUpdateMidhook (engine_updater.cpp) for why this exists and
    // why the timing/ordering there is deliberate, not incidental.
    CheckpointObject* m_pendingCaptureCp = nullptr;
    uint64_t m_pendingCaptureFrameOffset = 0;
    int m_pendingCaptureStage = 0;

    void saveCurrent(CheckpointObject* cp, uint64_t frameOffset);
    void saveState(CheckpointObject* cp, uint64_t frameOffset);
    void restorePreviousFrame(std::function<void(CheckpointObject*)> loadFn);
    void applyLatest();
    void applyCheckpoint(SavedCheckpointState& state);
    void dropLastStoredFrame();
    void clearStoredFrames() {
        m_storedFrames.clear();
    }
    void clearPlatformer(bool full);
    bool canRestoreState() const {
        return m_storedFrames.size() > 1;
    }
    void updatePlatformerInputs(cocos2d::CCArray* queuedButtons) {
        (void)queuedButtons;
    }
    void registerBrokenObject(GameObject* obj) {
        m_brokenObjects.push_back(obj);
    }
};

struct MacroPathSample {
    float p1x = 0.f, p1y = 0.f;
    float p1XVel = 0.f, p1YVel = 0.f;
    float p1Rot = 0.f;
    bool p1OnGround = false, p1UpsideDown = false, p1Dashing = false;
    bool p1OrbDash = false, p1OrbNonDash = false;
    char gamemode1 = 'C';

    float p2x = 0.f, p2y = 0.f;
    float p2XVel = 0.f, p2YVel = 0.f;
    float p2Rot = 0.f;
    bool p2OnGround = false, p2UpsideDown = false, p2Dashing = false;
    bool p2OrbDash = false, p2OrbNonDash = false;
    char gamemode2 = 'C';

    bool hasP2 = false;
};

class GucciReplaySystem {
public:
    gb::ActionAtom m_actionAtom;
    size_t m_inputIndex = 0;
    uint64_t m_startingSeed = 0;
    uint64_t m_startingSeedThisAttempt = 0;
    uint64_t m_shakeRandomState = 0;
    std::string m_replayName = "";

    std::vector<MacroPathSample> m_pathSamples;
    bool m_pathSamplesDirty = false;
    void savePathSamplesNow();

    std::vector<std::pair<double, double>> m_clickIntervalsSec;
    double m_clickBarTps = 240.0;
    void buildClickIntervals(double tps);

    float m_trainerBestX = 0.f;
    void saveTrainerProgressNow();

    bool m_mirrorInputs = false;
    bool m_mirrorInverted = false;
    bool m_maintainGravity = false;
    bool m_ignoreInputs = false;
    bool m_forceNextInput = false;
    bool m_flipProcessingInputs = false;

    std::unordered_map<int, gb::Action> m_lastInputs;

    bool m_suppressNextRelease[2] = {false, false};

    GucciScheduler::JobId m_autosaveJobId = 0;

    [[nodiscard]] std::optional<gb::Action> getCurrentQueuedInput() const;
    [[nodiscard]] std::optional<gb::Action> getNextInput(uint32_t frame);

    void advanceInputIndex() {
        m_inputIndex++;
    }
    void onReset(uint32_t respawnFrame, uint32_t deathFrame);
    void onExit() {
        m_inputIndex = 0;
    }

    bool hasFlippedControls() {
        return GameManager::get()->getGameVariable("0010");
    }
    bool playerFlipped(bool player2) {
        return player2 ^ hasFlippedControls();
    }

    void save(const std::filesystem::path& path, bool noOverwrite = false);
    void load(const std::filesystem::path& path);
    std::filesystem::path getCurrentPath() const;
    void backupExisting(const std::filesystem::path& path);
    void createBackup();
};

class GucciUpdater {
public:
    double m_tps = 240.0;
    double m_speedhack = 1.0;
    double m_tpsOverflow = 0.0;
    bool m_shouldRender = true;
    bool m_realTime = false;
    uint32_t m_maxUPR = 10;
    bool m_useVisualUpdates = false;

    uint32_t m_frame = 0;
    uint64_t m_frameOnLastAttempt = 0;
    int savedStepCount = 0;
    int totalStepCount = 1;
    int estimatedStepCount = 1;
    float currentDelta = 0.f;
    float m_lastTfp = 0.f;
    bool m_logFrameIncrements = false;

    bool m_lockDelta = true;

    bool m_paused = false;
    bool m_stepOnce_ = false;
    bool m_onlyRefresh = false;

    bool m_backwardsStepping = false;
    bool m_ssbFix = true;
    bool m_extrapolateFrames = false;
    bool m_layoutMode = false;
    bool m_speedhackAudio = false;
    bool m_allowedToProcessActions = true;

    bool m_canDie = false;
    bool m_inputIsDeath = false;
    bool m_fullReset = false;
    bool m_expectsDeath = false;
    bool m_isAutoFlipped = false;
    bool m_predicting = false;

    bool m_preventDeath = false;
    bool m_autoFlipOnDeath = false;
    bool m_fullGamePrediction = false;
    float m_acceptablePrediction = 0.9f;

    int m_respawnTimer = 0;
    uint32_t m_maxBackstepFrames = 60;

    cocos2d::CCPoint m_lastCameraPos;
    cocos2d::CCPoint m_currentCameraPos;
    float m_lastPlayerX = 0.f;
    float m_currentPlayerX = 0.f;

    void* m_actionMgr = nullptr;
    std::forward_list<std::function<void(float)>> m_frozenScheduledFunctions;

    double getPhysicsDt() const {
        return 1.0 / m_tps;
    }
    float getTimeWarp() const;
    uint32_t getFrame() const;

    void incrementFrame() {
        m_frame++;
    }
    void resetFrame() {
        m_frame = 0;
    }
    void setFrame(uint32_t f) {
        m_frame = f;
    }
    void setTps(double tps) {
        if (tps > 0.0)
            m_tps = tps;
    }
    void setPaused(bool p) {
        m_paused = p;
    }
    void togglePaused() {
        m_paused = !m_paused;
    }
    bool consumeStep() {
        bool s = m_stepOnce_;
        m_stepOnce_ = false;
        return s;
    }
    bool isPaused() const {
        return m_paused;
    }
    bool isLockDelta() const {
        return m_lockDelta;
    }
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
    bool enabled = false;
    bool showFrame = true;
    bool showTPS = false;
    bool showX = false;
    bool showY = false;
    bool showXVel = false;
    bool showYVel = false;
    bool showRot = false;
    bool showState = false;
    bool bigFont = false;
    float scale = 1.0f;
    float opacity = 1.0f;
    int anchor = 0;
};

struct BotSettingsPreset {
    std::string name;
    double tps = 240.0;
    double speedhack = 1.0;
    bool lockDelta = true;
    int lockDeltaMode = 0;
    bool backwardsStepping = false;
    uint32_t maxBackstepFrames = 60;
    bool ssbFix = true;
    bool extrapolateFrames = false;
    bool preventDeath = false;
    bool autoFlipOnDeath = false;
    bool maintainGravity = false;
    bool mirrorInputs = false;
    bool noclip = false;
    double autosaveInterval = 60.0;
    bool autosaveAtInterval = false;
    bool autosaveAtLevelEnd = false;
};

class GucciEngine {
public:
    static GucciEngine* get() {
        static GucciEngine inst;
        return &inst;
    }

    enum class Mode { Idle, Recording, Playing };

    GucciUpdater updater;
    GucciReplaySystem replay;
    GucciPracticeFix practiceFix;
    GucciScheduler scheduler;
    Renderer renderer;
    HudConfig hud;

    bool enabled = false;
    Mode mode = Mode::Idle;
    double userTpsSaved = 0.0;
    std::string replayName = "";

    bool isIdle() const {
        return mode == Mode::Idle;
    }
    bool isRecording() const {
        return mode == Mode::Recording;
    }
    bool isPlaying() const {
        return mode == Mode::Playing;
    }
    void setMode(Mode m);
    bool beginResumeRecording();

    bool noclipEnabled = false;
    bool noclipDeathFlash = true;
    float noclipThreshold = 0.f;
    float noclipDeathColorR = 1.f;
    float noclipDeathColorG = 0.3f;
    float noclipDeathColorB = 0.3f;
    bool noclipDeathBlocked = false;
    float noclipAccuracy = 0.f;

    bool hackHideAttempts = false;
    bool hackHidePercentage = false;
    bool hackNoSpikeFlash = false;
    bool hackAutoRetry = false;
    float hackAutoRetryDelay = 0.5f;
    bool hackRespawnInstant = false;
    bool hackForcePlatformer = false;
    float pendingAutoRetry = 0.f;
    bool noclipAccuracyVisible = false;

    bool showHitboxes = false;
    bool hitboxTrail = false;
    bool hitboxOnDeath = false;
    int hitboxTrailLength = 240;
    bool pathPreview = false;
    int pathLength = 240;
    bool survivalIndicator = false;
    int indicatorLookahead = 20;
    int indicatorStyle = 0;
    float indicatorOpacity = 0.9f;
    float indicatorSafeColorR = 0.25f, indicatorSafeColorG = 0.95f, indicatorSafeColorB = 0.35f;
    float indicatorDangerColorR = 0.95f, indicatorDangerColorG = 0.25f,
          indicatorDangerColorB = 0.25f;
    bool indicatorSoundEnabled = false;
    bool indicatorFlashEnabled = true;
    bool accuracyHudEnabled = false;
    int accuracyGoodClicks = 0;
    int accuracyTotalClicks = 0;
    int currentStreak = 0;
    int bestStreak = 0;

    bool showMacroPath = false;
    float macroPathMarkerSize = 8.f;
    float macroPathLineOpacity = 0.6f;

    bool trainerRevealEnabled = true;
    float trainerRevealBuffer = 40.f;

    std::string jupiterNotes;
    std::string jupiterSegmentsRaw;

    struct TrainerMacroData {
        bool loaded = false;
        std::string levelName;
        int32_t levelId = 0;
        std::vector<std::pair<double, double>> clickIntervalsSec;
        double clickBarTps = 240.0;
        std::vector<MacroPathSample> pathSamples;
    };
    TrainerMacroData jupiterMacro;

    bool jupiterClickBarPaused = true;
    double jupiterClickBarPosSec = 0.0;
    double jupiterClickBarLastRealTime = 0.0;

    bool jupiterClickBarLoop = false;

    bool jupiterClickBarPageVisible = false;

    std::vector<double> jupiterClickBarMyClicks;
    std::vector<double> jupiterClickBarMyReleases;

    bool jupiterClickBarEnabled = true;
    float jupiterClickBarWindow = 2.f;

    bool jupiterMusicEnabled = true;
    float jupiterMusicOffsetSec = 0.f;

    int jupiterAttemptCount = 0;
    float jupiterSessionBestPct = 0.f;
    std::vector<float> jupiterDeathPcts;

    bool jupiterLoopEnabled = false;
    int jupiterLoopStartIdx = -1;
    int jupiterLoopEndIdx = -1;

    bool jupiterGhostEnabled = true;
    bool jupiterBestGhostEnabled = false;
    bool jupiterScrubActive = false;
    float jupiterScrubPercent = 0.f;

    bool jupiterDeviationHolding = false;
    int jupiterLastDeviationFrames = 0;
    bool jupiterHasDeviationReading = false;

    TrainerMacroData trainerMacro;
    std::string trainerMacroName;

    std::string trainerNotes;
    std::string trainerSegmentsRaw;

    bool trainerClickBarPaused = true;
    double trainerClickBarPosSec = 0.0;
    double trainerClickBarLastRealTime = 0.0;
    bool trainerClickBarLoop = false;
    bool trainerClickBarPageVisible = false;
    std::vector<double> trainerClickBarMyClicks;
    std::vector<double> trainerClickBarMyReleases;
    bool trainerClickBarEnabled = true;
    float trainerClickBarWindow = 2.f;

    bool trainerMusicEnabled = false;
    bool trainerMusicImported = false;
    float trainerMusicOffsetSec = 0.f;

    int trainerAttemptCount = 0;
    float trainerSessionBestPct = 0.f;
    std::vector<float> trainerDeathPcts;

    bool trainerLoopEnabled = false;
    int trainerLoopStartIdx = -1;
    int trainerLoopEndIdx = -1;

    bool trainerGhostEnabled = true;
    bool trainerBestGhostEnabled = false;
    bool trainerScrubActive = false;
    float trainerScrubPercent = 0.f;

    bool trainerDeviationHolding = false;
    int trainerLastDeviationFrames = 0;
    bool trainerHasDeviationReading = false;

    bool loadTrainerMacro(const std::string& stem);

    bool layoutMode = false;
    bool noMirrorEffect = false;
    bool noMirrorRecordingOnly = false;
    bool audioPitchEnabled = false;
    bool rngLocked = false;
    bool protectedMode = false;

    bool fwEnabledLive = false;
    bool fwEnabledRender = false;
    bool fwLegendEnabled = false;
    float fwLegendScale = 1.f;
    float fwRingBoldness = 2.2f;

    bool practiceRangeEnabled = false;
    int fwMaxWindow = 25;
    struct FrameWindowMark {
        float x;
        float y;
        int window;
        bool player2;
        uint32_t frame;
        float percent;
        bool manual = false;
        bool isRelease = false;
    };
    std::vector<FrameWindowMark> fwMarks;
    bool fwHasData = false;
    bool fwTestShipReleases = true;
    bool fwOrbAwareReleaseSkip = true;
    struct FwClickSample {
        uint32_t frame;
        float x;
        float y;
        bool player2;
        bool release;
        bool orbDash = false;
        bool orbNonDash = false;
    };
    std::vector<FwClickSample> fwClickSamples;
    bool fwSampling = false;
    int fwSweepRange = 12;
    int fwMaxFramesMeasured = 240;
    int fwSimSpeed = 1;
    int fwSlackWindow = 3;
    bool fwFullRangeSweep = false;
    bool fwAnalyzing = false;
    bool fwProbeDied = false;
    float fwSavedMusicVolume = 0.f;
    float fwSavedEffectsVolume = 0.f;
    bool fwMusicMuted = false;

    enum class FwState { Idle, Capturing, Probing, DebugPause, Finishing };
    FwState fwState = FwState::Idle;
    size_t fwCapIndex = 0;
    size_t fwXYIndex = 0;
    bool fwCkptCreatedThisFrame = false;
    size_t fwProbeClick = 0;
    int fwProbeShift = 0;
    int fwProbeLow = 0;
    int fwProbeHigh = 0;
    int fwProbePhase = 0;
    int fwProbeFrame = 0;
    int fwProbeHorizon = 16;
    bool fwProbeHasNext = false;
    uint32_t fwProbeNextFrame = 0;
    bool fwProbeNextIsRelease = false;
    bool fwProbeNextPlayer2 = false;
    float fwProbeNextX = 0.f, fwProbeNextY = 0.f;
    bool fwPositionCheckEnabled = false;
    float fwPositionSlack = 50.f;
    int fwProbeWindowHigh = 0;
    std::set<int> fwProbeTestedShifts;
    bool fwProbeNegContiguous = true;
    bool fwProbePosContiguous = true;
    int fwProbeValidCount = 0;
    int fwProbeMaxNegShift = 0;
    int fwProbeMaxPosShift = 0;
    bool fwUseRecoveryRangeAlgorithm = false;
    int fwRecoveryRange = 4;
    enum class FwProbeSubPhase { Reaching, RecoveryCandidate };
    FwProbeSubPhase fwProbeSubPhase = FwProbeSubPhase::Reaching;
    int fwRecoveryOffset = 0;
    bool fwDebugMode = false;
    int fwDebugSlowdown = 30;
    int fwDebugPauseRemaining = 0;
    bool fwDelayMarkerCapture = false;
    struct FwDebugMark {
        float x = 0.f, y = 0.f;
        bool survived = false;
        uint32_t macroFrame = 0;
        uint32_t testedFrame = 0;
        int inputNumber = 0;
        bool isRelease = false;
        bool player2 = false;
        size_t clickIndex = 0;
    };
    std::vector<FwDebugMark> fwDebugMarks;
    void debugPauseOrContinue(bool survived);
    void debugTeleportToMark(size_t markIndex);
    void fwTick();
    void beginProbeRun();
    void beginOrSkipProbeClick();
    void beginShiftTest();
    void advanceOffsetSweep(bool survived);
    void beginShiftTestRecovery();
    void beginProbeRunReach();
    void beginRecoveryCandidate();
    void advanceRecoverySweep(bool survived);
    bool fwHasManualMarkAt(uint32_t frame, bool player2) const;
    void finishProbeClick();
    void fwFinishAnalysis();
    void cancelAnalysis();
    void muteAnalysisMusic();
    void unmuteAnalysisMusic();
    void computeProbeHorizon();
    void saveFwMarksNow();
    float fwAnalyzeProgress = 0.0f;
    int fwAnalyzeCur = 0;
    int fwAnalyzeTotal = 0;
    bool fwAnalyzeRunning = false;
    std::vector<StoredFrame> fwCapStack;
    gb::ActionAtom fwSavedAtom;
    std::string fwAnalyzeStage;
    void analyzeFrameWindows();

    enum class FwMarkerShape { Circle = 0, Star = 1, Spiral = 2, Polygon = 3 };
    enum class FwFillStyle { Inverted = 0, Normal = 1 };

    struct FrameWindowTier {
        int lo = 1;
        int hi = 3;
        char imageFile[96] = "";
        char soundFile[96] = "";
        float r = 1.f, g = 0.2f, b = 0.2f;
        FwMarkerShape shape = FwMarkerShape::Circle;
        int polygonSides = 5;
        float polygonCornerRadius = 0.f;
        FwFillStyle fillStyle = FwFillStyle::Inverted;
        bool noBorder = false;
        float strokeSize = 2.2f;
        float volume = 1.f;
        float sizeScale = 1.f;
        bool markerPulseEnabled = false;
        float markerPulseColor[3] = {1.f, 1.f, 1.f};
        float markerPulseFadeIn = 0.1f, markerPulseHold = 0.3f, markerPulseFadeOut = 0.5f;
        bool textPulseEnabled = false;
        float textPulseColor[3] = {1.f, 1.f, 1.f};
        float textPulseFadeIn = 0.1f, textPulseHold = 0.3f, textPulseFadeOut = 0.5f;
        char legendGroup[32] = "";
    };
    std::vector<FrameWindowTier> fwTiers;
    FrameWindowTier* fwTierFor(int window) {
        for (auto& t : fwTiers)
            if (window >= t.lo && window <= t.hi)
                return &t;
        return nullptr;
    }

    bool autosaveAtLevelEnd = false;
    bool autosaveAtInterval = false;
    double autosaveIntervalSec = 60.0;
    bool replayBackupsEnabled = true;
    void applyIntervalAutosave();

    std::vector<std::string> storedMacros;
    std::set<std::string> incompatibleMacros;
    std::unordered_set<std::string> jaMacros;
    std::unordered_set<std::string> giddeyMacros;
    std::unordered_set<std::string> toosiiMacros;
    std::unordered_set<std::string> bamMacros;
    std::unordered_set<std::string> sexyyMacros;
    std::unordered_set<std::string> juiceMacros;
    std::unordered_set<std::string> butlerMacros;
    std::unordered_set<std::string> saweetieMacros;
    std::unordered_set<std::string> maybachMacros;
    std::unordered_set<std::string> romoMacros;
    std::unordered_set<std::string> grizzleyMacros;
    std::unordered_set<std::string> redKingdomMacros;
    std::unordered_map<std::string, std::unordered_set<std::string>> customThemeMacrosByExt;

    std::vector<BotSettingsPreset> settingsPresets;
    void saveBotSettingsPreset(const std::string& name);
    bool loadBotSettingsPreset(const std::string& name);
    void deleteBotSettingsPreset(const std::string& name);

    struct DiffEntry {
        int frame = -1;
        std::string description;
    };
    std::vector<DiffEntry> diffMacros(const std::string& a, const std::string& b);

    bool trimMacro(const std::string& name, int startTick, int endTick, bool rebase);
    bool mergeMacros(const std::string& a, const std::string& b, int gapTicks);

    std::string startPosWarning;

    std::string loadedMacroLevelName;
    void recordTpsChange(double tps);
    bool convertToBRR(const std::string& name);
    uint32_t rngSeedVal = 0;
    bool fastPlayback = false;

    float m_levelLength = 0.f;

    void initialize();
    void reloadMacroList();

    std::filesystem::path getReplayDir() const {
        return Mod::get()->getSaveDir() / "replays";
    }
    std::filesystem::path getPresetsDir() const {
        return Mod::get()->getSaveDir() / "presets";
    }

private:
    GucciEngine() = default;
};

namespace gbfw {
    void renderFrameWindows(PlayLayer* pl, bool isRecording);
    void playTierSound(int window);
    FMOD::ChannelGroup* frameWindowChannelGroup();
} // namespace gbfw

namespace gbpr {
    void renderPracticeRange(PlayLayer* pl);
}
