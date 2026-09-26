#ifndef ANALYSIS_FRAMEWINDOW_HPP
#define ANALYSIS_FRAMEWINDOW_HPP

#include <cocos2d.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lstar.hpp"
#include "shim.hpp"
#include "sound.hpp"

class PlayLayer;
class CheckpointObject;

struct FrameWindowMark {
    uint32_t frame = 0;
    int window = 0;
    int64_t low = 0;
    int64_t high = 0;
    bool clampedByNeighbour = false;
    bool unbounded = false;
    float subframe = 0.f;
    bool hidden = false;
    bool player2 = false;
    bool release = false;
    bool desynced = false;
    float percent = 0.f;
    bool cbf = false;
    bool bufferAssisted = false;
    bool setupGroup = false;
    char gamemode = 'C';
    int setupLow = 0;
    int setupHigh = 0;
    int64_t hz = 0;
    bool solid = true;
    int holes = 0;
    bool saturatedLow = false;
    bool saturatedHigh = false;
    bool splitWindow = false;
    int splitShift = 0;
    bool splitChecked = false;
    cocos2d::CCPoint position{};
    float dependent = 0.f;
    float dependentMin = 0.f;
    float dependentMax = 0.f;
};

struct FrameWindowMessage {
    std::string text;
    int startIndex = 0;
    int span = 1;
    float scale = 0.6f;
    float offsetY = 60.f;
    bool enabled = true;
};

class FrameWindowAnalyzer {
   public:
    enum class ShiftMode { Retime, Buffer };

    static constexpr int MAX_SWEEP = 32;
    static constexpr int MIN_HORIZON = 12;
    static constexpr float POSITION_EPSILON = 0.0005f;
    static constexpr int IMPOSSIBLE_SWEEP_TICKS = 3;
    static constexpr int MAX_RESYNC_FAILURES = 2;
    static constexpr int64_t DEAD_RUN_MARGIN = 2;
    static constexpr int64_t MAX_CBF_SLOTS = 134217728;
    static constexpr int64_t MAX_CBF_HZ = 32000000000LL;
    static constexpr long MAX_SETUP_COMBOS = 20000;
    static constexpr int64_t MAX_RESOLVABLE_SLOTS = 524288;

    enum class Algorithm : int {
        TimeBased = 0,
        RecoveryRange = 1,
    };

    struct Report {
        bool ok = false;
        std::string message;
    };

    Report start(PlayLayer* pl);
    Report startRange(PlayLayer* pl, uint32_t from, uint32_t to);

    struct PlayheadInput {
        bool valid = false;
        uint32_t frame = 0;
        bool player2 = false;
        bool release = false;
        int number = 0;
        int mark = -1;
    };

    PlayheadInput playheadInput() const;
    void applyLabel(int window, float cbf);
    Report testPlayhead(PlayLayer* pl, int count);
    void tick(PlayLayer* pl);
    void cancel();
    void render(PlayLayer* pl);
    void updateProgressOverlay(PlayLayer* pl);

    bool running() const { return m_running; }
    // True only during the capture pass -- the run that establishes the
    // macro's ground truth. Exposed so the slope log can tag those frames
    // CALC and be diffed against a PLAY run frame for frame.
    bool capturing() const;

    bool armsTicks() const { return m_running && !m_subtickMacro; }

    bool returning() const { return m_trip.active; }
    uint32_t returnFrame() const { return m_trip.frame; }
    float returnProgress() const;
    void stopReturn();

    bool isRestoring() const { return m_restoring; }
    bool onSuppressedDeath(cocos2d::CCNode* player, cocos2d::CCNode* killer);

    void notePress(uint32_t frame, bool player2, bool splittable,
                   bool onGround, double yVelocity);
    uint32_t legPressFrame() const { return m_legPressFrame; }

    int displayWindow(FrameWindowMark const& mk) const;
    bool cameraLocked() const;
    bool hideSpawnEffects() const;
    void setCameraTarget(cocos2d::CCPoint p);
    cocos2d::CCPoint cameraPoint();

   private:
    void reassertHeldButtons(uint32_t frame);

    static constexpr size_t PLAYER_BYTES = sizeof(PlayerObject);
    std::vector<unsigned char> m_snapP1;
    long long m_stateDiffRestores = 0;
    long long m_stateDiffCachedOnly = 0;
    long long m_stateDiffReal = 0;
    void captureStateBytes(PlayLayer* pl);
    void reportStateDiff(PlayLayer* pl);
    void hardResetToStart();
    bool m_triedHardReset = false;
    bool m_noclip = false;
    bool m_noclipNextAdvance = false;
    std::vector<cocos2d::CCPoint> m_capturePath;
    bool m_pathDiverged = false;
    int m_resyncFailures = 0;
    bool m_resyncGaveUp = false;
    int64_t m_fine = 1;
    double m_baseTps = 240.0;
    int64_t m_coarseWindow = 0;
    int m_coarseBufferSurvivors = 0;
    bool m_captureNoclip = false;
    cocos2d::CCPoint m_camFrom{};
    cocos2d::CCPoint m_camTo{};
    std::chrono::steady_clock::time_point m_camStart{};
    bool m_camHasTarget = false;

    uint32_t m_captureDeathFrame = UINT32_MAX;
    std::vector<uint32_t> m_captureDeaths;

    static constexpr size_t MAX_CAPTURE_DEATHS = 256;
    void noteCaptureDeath(uint32_t frame);
    uint32_t captureDeathInSpan() const;
    bool captureDiedInSpan() const;
    bool m_forceFullSweep = false;
    ShiftMode m_shiftMode = ShiftMode::Retime;
    bool m_bufferTried = false;
    int m_bufferSurvivors = 0;

   public:
    float progress() const;
    std::string const& status() const { return m_status; }
    std::string const& stage() const { return m_stage; }

    void clearDisplay() { this->resetDisplay(); }

    std::vector<FrameWindowMark> const& results() const { return m_results; }
    double resultsTps() const { return m_resultsTps > 0.0 ? m_resultsTps : 240.0; }
    std::vector<lstar::Input> precisionInputs(bool cbf = true) const;
    std::string describe() const;
    std::vector<FrameWindowMark>& editResults() { return m_results; }
    std::vector<FrameWindowMessage>& messages() { return m_messages; }
    void markEdited() {
        m_generation++;
        this->resetDisplay();
    }
    static std::vector<std::pair<std::string, cocos2d::ccColor3B>> parseColored(
        std::string const& text);
    int measuredCount() const { return m_measured; }
    int skippedCount() const { return m_skipped; }
    int desyncCount() const { return m_desynced; }

    bool saveResults(std::filesystem::path const& path) const;
    bool loadResults(std::filesystem::path const& path);

    void clear() {
        m_results.clear();
        m_messages.clear();
        m_measured = 0;
        m_skipped = 0;
        m_desynced = 0;
        m_filtered = 0;
        m_generation++;
    }

    static cocos2d::ccColor3B colorForWindow(int window);

    void checkCaptureAgainstTrail(PlayLayer* pl, uint32_t frame);
    struct MarkerNode {
        cocos2d::CCNode* m_node = nullptr;
        cocos2d::CCPoint m_world{};
    };
    std::vector<MarkerNode> m_markerNodes;

    cocos2d::CCNode* markerContainer(PlayLayer* pl);
    void trackMarker(cocos2d::CCNode* node, cocos2d::CCPoint world);
    void dropMarkers();

    void resetDisplay();
    FrameWindowTier const* tierFor(int window) const;
    static std::string formatWindow(int window);

    SLValuePtr<bool> m_statePlayerDiff = SLValue<bool>::create(
        "framewindow.state_player_diff",
        &SLSettings::get()->frameWindow.statePlayerDiff);
    SLValuePtr<bool> m_analysisOverlay = SLValue<bool>::create(
        "framewindow.analysis_overlay",
        &SLSettings::get()->frameWindow.analysisOverlay);
    SLValuePtr<bool> m_verbose = SLValue<bool>::create(
        "framewindow.verbose", &SLSettings::get()->frameWindow.verbose);
    SLValuePtr<bool> m_showMarkers = SLValue<bool>::create(
        "framewindow.show_markers",
        &SLSettings::get()->frameWindow.showMarkers);
    SLValuePtr<int> m_stepBatch = SLValue<int>::create(
        "framewindow.step_batch", &SLSettings::get()->frameWindow.stepBatch);
    SLValuePtr<bool> m_showTiming = SLValue<bool>::create(
        "framewindow.show_timing", &SLSettings::get()->frameWindow.showTiming);
    // GucciBot: anticroom's precision readout and our L* HUD are the same
    // feature. His toggle drives ours, so there is one switch and one readout.
    SLValuePtr<bool> m_showPrecision = SLValue<bool>::create(
        "framewindow.show_precision",
        &SLSettings::get()->frameWindow.lstarHud);
    SLValuePtr<int> m_labelWindow = SLValue<int>::create(
        "framewindow.label_window",
        &SLSettings::get()->frameWindow.labelWindow);
    SLValuePtr<float> m_labelCbf = SLValue<float>::create(
        "framewindow.label_cbf", &SLSettings::get()->frameWindow.labelCbf);
    SLValuePtr<int> m_labelTestCount = SLValue<int>::create(
        "framewindow.label_test_count",
        &SLSettings::get()->frameWindow.labelTestCount);
    SLValuePtr<bool> m_labelReleases = SLValue<bool>::create(
        "framewindow.label_releases",
        &SLSettings::get()->frameWindow.labelReleases);
    SLValuePtr<bool> m_labelApply = SLValue<bool>::create(
        "framewindow.label_apply", &SLSettings::get()->frameWindow.labelApply);
    SLValuePtr<bool> m_labelTest = SLValue<bool>::create(
        "framewindow.label_test", &SLSettings::get()->frameWindow.labelTest);
    SLValuePtr<int64_t> m_cbfInputHz = SLValue<int64_t>::create(
        "framewindow.cbf_input_hz",
        &SLSettings::get()->frameWindow.cbfInputHz);
    SLValuePtr<bool> m_cbfWholeMarkers = SLValue<bool>::create(
        "framewindow.cbf_whole_markers",
        &SLSettings::get()->frameWindow.cbfWholeMarkers);
    SLValuePtr<int> m_cbfReadoutThreshold = SLValue<int>::create(
        "framewindow.cbf_readout_threshold",
        &SLSettings::get()->frameWindow.cbfReadoutThreshold);
    SLValuePtr<bool> m_cbfTickGround = SLValue<bool>::create(
        "framewindow.cbf_tick_ground",
        &SLSettings::get()->frameWindow.cbfTickGround);
    SLValuePtr<bool> m_entrySweep = SLValue<bool>::create(
        "framewindow.entry_sweep",
        &SLSettings::get()->frameWindow.entrySweep);
    SLValuePtr<bool> m_dependentSearch = SLValue<bool>::create(
        "framewindow.dependent_search",
        &SLSettings::get()->frameWindow.dependentSearch);
    SLValuePtr<bool> m_showSetupRange = SLValue<bool>::create(
        "framewindow.show_setup_range",
        &SLSettings::get()->frameWindow.showSetupRange);
    SLValuePtr<bool> m_showHzReadout = SLValue<bool>::create(
        "framewindow.show_hz_readout",
        &SLSettings::get()->frameWindow.showHzReadout);
    SLValuePtr<bool> m_setupHoldModes = SLValue<bool>::create(
        "framewindow.setup_hold_modes",
        &SLSettings::get()->frameWindow.setupHoldModes);
    SLValuePtr<bool> m_markSetupVarying = SLValue<bool>::create(
        "framewindow.mark_setup_varying",
        &SLSettings::get()->frameWindow.markSetupVarying);
    SLValuePtr<bool> m_subframeBisect = SLValue<bool>::create(
        "framewindow.subframe_bisect",
        &SLSettings::get()->frameWindow.subframeBisect);
    SLValuePtr<bool> m_jointSetupSweep = SLValue<bool>::create(
        "framewindow.joint_setup_sweep",
        &SLSettings::get()->frameWindow.jointSetupSweep);
    SLValuePtr<bool> m_subframeAll = SLValue<bool>::create(
        "framewindow.subframe_all", &SLSettings::get()->frameWindow.subframeAll);
    SLValuePtr<int> m_subframeScanPercent = SLValue<int>::create(
        "framewindow.subframe_scan_percent",
        &SLSettings::get()->frameWindow.subframeScanPercent);
    SLValuePtr<int> m_subframeDecimals = SLValue<int>::create(
        "framewindow.subframe_decimals",
        &SLSettings::get()->frameWindow.subframeDecimals);
    SLValuePtr<bool> m_showDesynced = SLValue<bool>::create(
        "framewindow.show_desynced", &SLSettings::get()->frameWindow.showDesynced);
    SLValuePtr<bool> m_showHud = SLValue<bool>::create(
        "framewindow.show_hud", &SLSettings::get()->frameWindow.showHud);
    SLValuePtr<bool> m_playSounds = SLValue<bool>::create(
        "framewindow.play_sounds",
        &SLSettings::get()->frameWindow.playSounds);
    SLValuePtr<float> m_soundVolume = SLValue<float>::create(
        "framewindow.sound_volume",
        &SLSettings::get()->frameWindow.soundVolume);
    SLValuePtr<float> m_markerRadius = SLValue<float>::create(
        "framewindow.marker_radius",
        &SLSettings::get()->frameWindow.markerRadius);
    SLValuePtr<float> m_markerScale = SLValue<float>::create(
        "framewindow.marker_scale", &SLSettings::get()->frameWindow.markerScale);

    SLValuePtr<bool> m_enabled = SLValue<bool>::create(
        "framewindow.enabled", &SLSettings::get()->frameWindow.enabled);
    SLValuePtr<int> m_algorithm = SLValue<int>::create(
        "framewindow.algorithm", &SLSettings::get()->frameWindow.algorithm);
    SLValuePtr<int> m_sweepRange = SLValue<int>::create(
        "framewindow.sweep_range", &SLSettings::get()->frameWindow.sweepRange);
    SLValuePtr<int> m_maxFrames = SLValue<int>::create(
        "framewindow.max_frames", &SLSettings::get()->frameWindow.maxFrames);
    SLValuePtr<int> m_slack = SLValue<int>::create(
        "framewindow.slack", &SLSettings::get()->frameWindow.slack);
    SLValuePtr<int> m_recoveryRange = SLValue<int>::create(
        "framewindow.recovery_range",
        &SLSettings::get()->frameWindow.recoveryRange);
    SLValuePtr<bool> m_subframeProbe = SLValue<bool>::create(
        "framewindow.subframe_probe",
        &SLSettings::get()->frameWindow.subframeProbe);
    SLValuePtr<int> m_tightThreshold = SLValue<int>::create(
        "framewindow.tight_threshold",
        &SLSettings::get()->frameWindow.tightThreshold);
    SLValuePtr<bool> m_analysisVisuals = SLValue<bool>::create(
        "framewindow.analysis_visuals",
        &SLSettings::get()->frameWindow.analysisVisuals);
    SLValuePtr<bool> m_lockCamera = SLValue<bool>::create(
        "framewindow.lock_camera", &SLSettings::get()->frameWindow.lockCamera);
    SLValuePtr<bool> m_hideSpawnEffects = SLValue<bool>::create(
        "framewindow.hide_spawn_effects",
        &SLSettings::get()->frameWindow.hideSpawnEffects);
    SLValuePtr<int> m_budgetMs = SLValue<int>::create(
        "framewindow.budget_ms", &SLSettings::get()->frameWindow.budgetMs);
    SLValuePtr<bool> m_turbo = SLValue<bool>::create(
        "framewindow.turbo", &SLSettings::get()->frameWindow.turbo);
    SLValuePtr<int> m_turboBudgetMs = SLValue<int>::create(
        "framewindow.turbo_budget_ms",
        &SLSettings::get()->frameWindow.turboBudgetMs);
    SLValuePtr<bool> m_adaptiveBudget = SLValue<bool>::create(
        "framewindow.adaptive_budget",
        &SLSettings::get()->frameWindow.adaptiveBudget);
    SLValuePtr<int> m_budgetSharePercent = SLValue<int>::create(
        "framewindow.budget_share_percent",
        &SLSettings::get()->frameWindow.budgetSharePercent);
    SLValuePtr<int> m_maxBudgetMs = SLValue<int>::create(
        "framewindow.max_budget_ms",
        &SLSettings::get()->frameWindow.maxBudgetMs);
    SLValuePtr<bool> m_fullRangeSweep = SLValue<bool>::create(
        "framewindow.full_range",
        &SLSettings::get()->frameWindow.fullRangeSweep);
    SLValuePtr<bool> m_testAllReleases = SLValue<bool>::create(
        "framewindow.all_releases",
        &SLSettings::get()->frameWindow.testAllReleases);
    SLValuePtr<bool> m_testShipReleases = SLValue<bool>::create(
        "framewindow.ship_releases",
        &SLSettings::get()->frameWindow.testShipReleases);
    SLValuePtr<bool> m_orbAwareReleaseSkip = SLValue<bool>::create(
        "framewindow.orb_release_skip",
        &SLSettings::get()->frameWindow.orbAwareReleaseSkip);

   private:
    struct Sample {
        uint32_t frame = 0;
        cocos2d::CCPoint position{};
        bool player2 = false;
        bool release = false;
        bool orbDash = false;
        bool orbNonDash = false;
        slc::ActionType type = slc::ActionType::Jump;
        size_t actionIndex = 0;
        char gamemode = 'C';
    };

    enum class Stage {
        Idle,
        Capture,
        ResolveSetup,
        Advance,
        Snapshot,
        Probe,
        Recover,
        Rewind,
        Dependent,
        Finish,
    };

    enum class StepResult {
        Reached,
        Died,
        OutOfBudget,
    };

    enum class Phase { Nominal, Earlier, Later, Subframe };

    StepResult stepToward(PlayLayer* pl, uint32_t until);

    bool collectSamples();
    void foldSwiftClicks();
    void applyOrbAwareSkip();

    void beginClick();
    void beginShift(int64_t shift, ShiftMode mode = ShiftMode::Retime);
    bool bufferShiftValid(int64_t shift) const;
    int64_t nextBisectShift() const;
    bool startSubframeProbe();
    void concludeShift(bool survived);
    void advanceSweep(bool survived);
    void noteSweepStep(bool survived, bool counting);
    void beginRecovery();
    void finishClick();
    void abortClick(char const* why);
    void nextClick();
    void finish(std::string message, bool ok);

    bool restoreToBranch();
    void releaseCheckpoint();
    long negRoom(size_t idx) const;


    static constexpr size_t NO_INDEX = static_cast<size_t>(-1);
    size_t nextSampleFor(size_t idx) const;
    size_t prevActionFor(size_t ai, bool player2) const;
    size_t nextActionFor(size_t ai, bool player2) const;
    size_t pairedReleaseFor(size_t ai) const;
    long posRoom(size_t idx) const;

    double m_resultsTps = 240.0;
    int m_timingMark = -1;
    int m_timingShown = -1;

    int64_t m_coarseLow = 0;
    int64_t m_coarseHigh = 0;
    bool m_coarseNominalDied = false;
    int64_t m_coarseMaxNeg = 0;
    int64_t m_coarseMaxPos = 0;

    bool m_restoring = false;
    int m_lastKillerId = -1;
    cocos2d::CCPoint m_lastKillerPos{};

    static constexpr float TRAIL_TOLERANCE = 1.0f;
    bool m_trailChecked = false;
    bool m_trailDiverged = false;
    size_t m_trailCursor = 0;

    bool m_trackingPath = false;

    void splitSlots(int64_t slots, int& tickShift,
                    double& fraction) const;

    std::vector<bool> m_inSetupGroup;

    static bool isSetupMode(char gamemode);
    static bool isHoldMode(char gamemode);
    bool setupVarying(FrameWindowMark const& mk) const;
    FrameWindowTier const* tierForFrames(float frames) const;
    FrameWindowTier const* visibleTierFor(FrameWindowMark const& mk) const;
    std::string decorate(FrameWindowMark const& mk, std::string text) const;

    struct SetupGroup {
        size_t first;
        size_t count;
    };
    std::vector<SetupGroup> m_setupGroups;
    size_t m_setupGroupCursor = 0;

    std::vector<int> m_setupFollowerRange;
    int m_setupLeaderShift = 0;
    std::vector<int> m_setupCombo;
    bool m_setupLegActive = false;
    uint32_t m_setupLegTarget = 0;

    std::vector<std::pair<int, std::vector<int>>> m_setupSurvivors;
    std::vector<std::pair<int, int>> m_setupRange;

    std::vector<std::pair<int, int>> m_sampleBand;
    bool m_entryActive = false;
    bool m_entryDone = false;
    int m_entryOffset = 0;
    std::vector<int> m_entryQueue;
    int m_entryMin = 0;
    int m_entryMax = 0;
    bool m_entryFailed = false;
    size_t m_entryPrevAi = 0;
    uint32_t m_entryBranch = 0;
    FrameWindowMark m_entryMark;

    FrameWindowMark buildMark() const;
    bool advanceEntryPass(FrameWindowMark const& mk);

    bool m_setupBestFound = false;
    int m_setupBestDeviation = 0;
    int m_setupBestLeaderShift = 0;
    std::vector<int> m_setupBestCombo;
    long long m_stepNanos = 0;
    long long m_stepCount = 0;
    long long m_fineLegs = 0;
    bool m_warnedResolution = false;
    long long m_restoreNanos = 0;
    long long m_restoreCount = 0;
    long long m_tickNanos = 0;

    std::chrono::steady_clock::time_point m_lastTickCall{};
    bool m_haveLastTickCall = false;

    std::vector<FrameWindowMark> m_results;
    std::vector<FrameWindowMessage> m_messages;
    std::vector<char> m_messageSpawned;
    void spawnMessages(PlayLayer* pl, uint32_t frame);
    std::vector<Sample> m_samples;
    std::vector<uint32_t> m_originalFrames;
    std::vector<double> m_originalOffsets;
    bool m_subtickMacro = false;

    int64_t placeSubtick(size_t k, int64_t slots, int64_t ticks);
    void restoreOffsets();

    bool m_running = false;
    std::string m_status;
    std::string m_stage;

    Stage m_stageId = Stage::Idle;
    size_t m_index = 0;
    size_t m_total = 0;
    int m_measured = 0;
    int m_skipped = 0;
    int m_desynced = 0;
    int m_filtered = 0;
    bool m_desyncPending = false;

    int m_sweep = 6;
    int m_horizon = 240;
    int m_slack_ = 3;
    int m_recovery = 4;
    Algorithm m_algo = Algorithm::TimeBased;

    uint32_t m_recorded = 0;
    int64_t m_maxNeg = 0;
    int64_t m_maxPos = 0;
    bool m_clamped = false;
    uint32_t m_branchFrame = 0;

    Phase m_phase = Phase::Nominal;
    int64_t m_shift = 0;
    int64_t m_releaseFrame = -1;
    std::vector<int64_t> m_tailArm;
    static constexpr size_t MAX_DEAD_TRACKED = 256;
    std::vector<int64_t> m_deadShifts;

    int64_t m_low = 0;
    int64_t m_high = 0;
    int64_t m_validCount = 0;
    bool m_bisect = false;
    bool m_scanning = false;
    int64_t m_scanStep = 1;
    int64_t m_islandSeed = 0;
    int64_t m_scanLastDead = 0;
    bool m_gallopping = false;
    int64_t m_gallopStep = 1;
    int64_t m_bisectLo = 0;
    int64_t m_bisectHi = 0;

    bool m_negCounting = true;
    bool m_posCounting = true;
    bool m_nominalDied = false;
    int64_t m_deadRun = 0;
    bool m_splitWindow = false;
    int64_t m_splitShift = 0;

    uint32_t m_legPressFrame = UINT32_MAX;
    bool m_legPressBuffered = false;
    bool m_legPressSeen = false;
    bool m_fineCrossedNeighbour = false;
    bool m_legPressOnGround = false;
    double m_legPressYVel = 0.0;
    std::map<uint32_t, bool> m_tickBuffered;

    bool m_testActive = false;
    uint32_t m_testTarget = 0;
    bool m_restoreFailed = false;
    bool m_probeDied = false;

    bool m_recoveryActive = false;
    int m_recoveryOffset = 0;

    bool m_savedCanDie = false;
    bool m_savedExpectsDeath = false;
    bool m_wasPaused = false;
    float m_savedMusicVolume = 1.f;
    float m_savedSfxVolume = 1.f;
    bool m_mutedAudio = false;

    void muteAudio();
    void unmuteAudio();

    CheckpointObject* m_cpObject = nullptr;
    SavedCheckpoint m_saved;
    bool m_haveCheckpoint = false;

    std::vector<tbuf::Sample> m_trailP1;
    std::vector<tbuf::Sample> m_trailP2;

    uint32_t m_captureHeartbeat = 0;
    uint32_t m_legCounter = 0;

    uint32_t m_generation = 0;

    void updateDisplay(PlayLayer* pl);
    std::chrono::steady_clock::time_point m_runStart{};
    void updateTiming(PlayLayer* pl, FrameWindowMark const* mk);
    void refreshTiming(PlayLayer* pl);

    static bool setupChainable(Sample const& s);
    void detectSetupGroups();
    void deriveSetupRanges(SetupGroup const& sg);
    void beginSetupGroup(SetupGroup const& sg);
    void beginSetupLeg(SetupGroup const& sg);
    bool advanceSetupCombo(SetupGroup const& sg);
    void commitSetupGroup(SetupGroup const& sg);
    void applySetupFrames(SetupGroup const& sg, int leaderShift,
                          std::vector<int> const& combo);
    std::string formatSubframe(float frames) const;
    void spawnMarker(PlayLayer* pl, FrameWindowMark const& mk,
                     FrameWindowTier const* tier);
    std::optional<cocos2d::CCPoint> dualTwin(PlayLayer* pl,
                                             FrameWindowMark const& mk) const;
    void rebuildHud(PlayLayer* pl);
    void updateLStarHud(PlayLayer* pl);
    // Frames of the inputs handed to the L* solver, in the order it sorted
    // them, so m_lstarFrames[i] pairs with result().m_perInput[i]. Needed to
    // show the RUNNING value at the frame the player has reached.
    std::vector<uint32_t> m_lstarFrames;
    void refreshHudCounts(PlayLayer* pl);
    void updateHudFlash(PlayLayer* pl, uint32_t frame, double tps);
    void updatePrecisionReadout(PlayLayer* pl, uint32_t frame);

    uint32_t m_precisionGeneration = UINT32_MAX;

    bool m_partial = false;
    uint32_t m_probeFrom = 0;
    uint32_t m_probeTo = UINT32_MAX;
    std::vector<FrameWindowMark> m_keptResults;
    std::vector<FrameWindowMessage> m_keptMessages;

    bool inProbeRange(size_t index) const;
    void mergeKept();

    static constexpr int DEP_POINTS = 7;
    static constexpr int DEP_SCAN_PER_TICK = 8;

    struct DependentPair {
        size_t a = 0;
        size_t b = 0;
        size_t markB = 0;
        double aLo = 0.0;
        double aHi = 0.0;
    };

    std::vector<DependentPair> m_depPairs;
    size_t m_depCursor = 0;
    int64_t m_depRes = 1;
    bool m_depBaseReady = false;
    bool m_depAdvancing = false;
    bool m_depLegActive = false;
    uint32_t m_depLegTarget = 0;
    int64_t m_depLegShift = 0;
    std::vector<double> m_depPoints;
    size_t m_depPoint = 0;
    std::vector<int64_t> m_depWidths;
    std::vector<int64_t> m_depScan;
    std::vector<char> m_depScanAlive;
    size_t m_depScanAt = 0;
    bool m_depEdgesSet = false;
    bool m_depFound = false;
    int64_t m_depLow = 0;
    int64_t m_depLowDead = 0;
    int64_t m_depAlive = 0;
    int64_t m_depDead = 0;
    std::string m_depFinishMessage;

    bool beginDependentPass(std::string message);
    bool stepDependent(PlayLayer* pl);
    void startDependentPoint();
    bool nextDependentShift(int64_t& shift);
    void launchDependentLeg(int64_t shift);
    void recordDependentLeg(bool survived);
    void finishDependentPair();
    void resetActions();
    void placeAt(size_t k, double pos);
    size_t resultFor(size_t sample) const;

    static constexpr int TRIP_BUDGET_MS = 100;
    static constexpr uint32_t TRIP_STALL_STEPS = 2000;
    static constexpr double TRIP_NOTE_SECONDS = 3.0;

    struct Trip {
        bool pending = false;
        bool active = false;
        uint32_t frame = 0;
        std::vector<uint32_t> checkpoints;
        size_t placed = 0;
        size_t expected = 0;
        bool record = false;
        bool paused = false;
        bool backstep = false;
        bool died = false;
        uint32_t stalled = 0;
        std::vector<tbuf::Sample> trail[2];
    };

    Trip m_trip;
    std::string m_tripTitle;
    std::string m_tripDetail;
    bool m_tripOk = true;
    std::chrono::steady_clock::time_point m_tripNoteAt{};

    void beginTrip();
    void stepTrip(PlayLayer* pl);
    void endTrip(PlayLayer* pl, bool arrived, std::string title,
                 std::string detail);
    void updateTripLabel(PlayLayer* pl);

    static constexpr double HUD_FLASH_SECONDS = 0.53;

    int m_hudFlashTier = -1;
    uint32_t m_hudFlashFrame = 0;
    bool m_hudFlashActive = false;
    void cullOffscreen(PlayLayer* pl);
    void recountUpTo(uint32_t frame);

    uint32_t m_lastFrame = 0;
    bool m_haveLastFrame = false;
    int m_lastSpawn1P = -1;
    int m_lastSpawn2P = -1;
    std::map<int, int> m_hudCounts;
    uint32_t m_hudBuiltGeneration = UINT32_MAX;
    size_t m_hudBuiltTiers = 0;
};

#endif  // ANALYSIS_FRAMEWINDOW_HPP
