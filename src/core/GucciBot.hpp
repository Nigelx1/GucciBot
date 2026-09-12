#pragma once

#define GB_BUILD_LABEL                                                                     \
    "2026-09-12-c (Nigel's ask: an update checker on launch. Best-effort, one-shot GET "         \
    "against GitHub's releases API; if the latest tag is numerically newer than MOD_VERSION, "  \
    "shows a popup with a direct link, otherwise silent (offline/rate-limited/API-shape-change "\
    "all fail quietly -- purely informational, never worth bothering the user about on its "    \
    "own). Popup creation is explicitly queued onto the main thread with by-value string "      \
    "captures, not called straight from the coroutine's own continuation -- cheap insurance "   \
    "given this project's actual history with file-picker coroutines (issues #1/#3).)"

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

#include "core/action_types.hpp"
#include "legacy_renderer.hpp"
#include "core/checkpoint_player.hpp"

using namespace geode::prelude;

namespace FMOD {
    class ChannelGroup;
}

// Everything below is GucciBot's own code -- namespaced so it doesn't
// collide with GD/Geode/library symbols. Geode's $modify(...) hook classes
// are declared OUTSIDE this namespace (Geode expects them in global scope)
// and reach in via `using namespace gucci;` in their own translation units.
namespace gucci {

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

        // GD's own fast-rand global (see hook_playlayer.cpp's
        // updateRandomSeedOnReset, address 0x6c2e90) only used to get
        // rewound to the value it had at the START OF THE ATTEMPT on every
        // reset, never to the value it actually had at a given checkpoint's
        // frame. Harmless for Calculate, which always replays the same
        // fixed macro the same way every time -- a real problem for a
        // future search-based feature (Pathfinder) that restores the SAME
        // checkpoint from many different explored branches, since any
        // RNG-consuming, physics-relevant object (e.g. a Random trigger)
        // between attempt-start and the checkpoint would desync downstream
        // otherwise. Captured/restored alongside everything else here.
        uint64_t m_rngState = 0;
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

    // Real per-attempt click scoring against a loaded macro's press/release
    // intervals. Each real click is matched to the nearest press or release
    // that hasn't already been claimed by an earlier click -- NOT the
    // nearest by raw time delta, which lets two close real clicks both match
    // the same macro press and silently over/under-count. answeredPress/
    // answeredRelease are parallel to whichever clickIntervalsSec vector this
    // score is scoring against; reset() re-sizes and clears both whenever the
    // loaded macro (or its interval count) changes.
    struct ClickIndicatorScore {
        std::vector<bool> answeredPress;
        std::vector<bool> answeredRelease;
        int perfect = 0, ok = 0, miss = 0;
        int lastDeltaFrames = 0;
        bool hasLastReading = false;

        void reset(size_t intervalCount) {
            answeredPress.assign(intervalCount, false);
            answeredRelease.assign(intervalCount, false);
            perfect = ok = miss = 0;
            hasLastReading = false;
        }
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
        // Nigel's condition with ToastexGD for GucciBot's public release
        // (2026-09-05): GucciBot only runs if ToastyReplay Lite is present
        // in the mods folder (installed is enough -- it can be disabled).
        // Checked once per game launch in initialize(), not continuously
        // polled while playing, since Geode mods can't be added/removed
        // without a restart anyway. This flag records WHY enabled stayed
        // false, for the GUI to surface later if it ever grows a banner
        // for this specifically -- not surfaced anywhere yet beyond the
        // one-time startup notification.
        bool ttrRequirementMissing = false;
        // A THIRD state alongside "missing"/"fine": ToastyReplay Lite is
        // installed (satisfies the requirement above) but also currently
        // *enabled*, which is what actually crashed macro playback for a
        // real user (anticroom's Discord report, 2026-09-08, screenshot
        // showed him telling someone to "install Toasty, disable it, then
        // use gucci" -- a real requirement that was never surfaced in-app,
        // so people were finding out about it from crashes, not GucciBot
        // itself). Both mods running live at once isn't safe; stand down
        // the same way the missing-entirely case does, with different
        // wording, rather than letting the crash happen and saying nothing.
        bool ttrEnabledConflict = false;
        static constexpr const char* kTtrModId = "toastexgd.toastyreplay-lite";
        // Shared so the wording only lives in one place -- shown once at
        // startup (initialize()) and again every time someone tries to
        // open the menu while it's still missing (hacks/keybinds.cpp),
        // since the startup one is easy to miss.
        static void showTtrMissingNotification();
        static void showTtrEnabledNotification();
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

        // Video Mode -- a review/playback overlay, not a live-gameplay one.
        // No level needs to be open: the video plays full-screen, riding the
        // exact same scrub/loop/pause clock as the click bar
        // (jupiterClickBarPosSec), offset by jupiterVideoOffsetSec seconds so
        // "video timestamp" and "macro time zero" can be manually aligned.
        // Always plays the bundled JMF showcase video -- the "choose a
        // different video" picker was removed 2026-08-31 (Nigel: it crashed
        // on a single click, every time, "ditch the button" -- not a
        // double-click race like the earlier picker crash, something more
        // fundamentally broken about that specific flow) rather than chased
        // further, since the bundled video already covers the actual use
        // case with zero setup.
        bool jupiterVideoModeEnabled = false;
        float jupiterVideoOffsetSec = 0.f;
        float jupiterVideoOpacity = 0.6f;
        // Alignment tool, added 2026-08-31 (Nigel: "any easier way to fix
        // up the delay stuff, debug slider is tedious... heres a scroll bar
        // for the video, scroll until right at the first click"). When
        // active, drawJupiterVideoOverlay shows jupiterVideoAlignScrubSec
        // directly instead of the click bar's live clock, so the video can
        // be scrubbed by hand to find the exact moment of the macro's first
        // click; a button then sets jupiterVideoOffsetSec FROM that scrub
        // position automatically, replacing trial-and-error slider nudging.
        // Not persisted -- a one-shot alignment aid, not an ongoing
        // setting (jupiterVideoOffsetSec, the actual result, already is).
        bool jupiterVideoAlignToolActive = false;
        float jupiterVideoAlignScrubSec = 0.f;

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

        ClickIndicatorScore jupiterClickScore;

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

        ClickIndicatorScore trainerClickScore;

        // Shared PERFECT/OK windows for both Jupiter and Trainer click
        // scoring, in milliseconds -- not frames, so they mean the same thing
        // regardless of the loaded macro's own TPS.
        float clickIndicatorPerfectMs = 20.f;
        float clickIndicatorOkMs = 60.f;

        bool loadTrainerMacro(const std::string& stem);

        // Scores one real click (press or release) against `intervals`
        // (a macro's own press/release-time-pairs, in seconds) using
        // `score`'s nearest-unanswered matching. Updates `score`'s running
        // perfect/ok/miss tally and marks the match answered. Returns the
        // signed frame delta (negative = early) for display, or nullopt if
        // every interval of the requested kind is already answered (a real
        // miss with nothing left to compare against).
        std::optional<int> scoreRealClick(const std::vector<std::pair<double, double>>& intervals,
                                          ClickIndicatorScore& score,
                                          double clickTimeSec,
                                          bool isPress,
                                          double tps);

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

        // Juice's ask (2026-09-02): once an Alignment-Independent run has
        // results, let the in-level marker overlay show THOSE numbers
        // instead of Time-Based/Recovery Range's, toggleable so both stay
        // available if a Time-Based run has also been done. Purely a
        // display switch -- doesn't change what either method measures or
        // touch fwMarks/fwAiResults themselves.
        bool fwOverlayShowAlignmentIndependent = false;

        // Juice's "circle skin" ask (osu!mania reference: image0.jpg) --
        // an alternate marker style for the frame-window overlay. A small
        // fixed-size filled dot marks the macro's exact click timing (the
        // "note"); a separate unfilled ring around it grows with how
        // lenient that click's window is, so a wide-open input reads as an
        // obviously bigger halo at a glance instead of a same-size marker
        // with a different number next to it. Purely additive rendering --
        // doesn't touch fwMarks/analysis at all, default off.
        bool fwCircleSkinEnabled = false;
        float fwCircleSkinDotRadius = 5.f;
        float fwCircleSkinRadiusPerFrame = 2.2f;
        float fwCircleSkinMaxRadius = 60.f;

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
            gb::ActionType type = gb::ActionType::Jump;
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

        enum class FwState {
            Idle,
            Capturing,
            Probing,
            DebugPause,
            Finishing,
            // Alignment-Independent method states -- entered from Capturing
            // instead of Probing when fwUseAlignmentIndependent is set. See
            // the "Alignment-Independent frame-window method" block below.
            AiBuildPred,
            AiSweepX,
            AiContinuation
        };
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
        bool fwProbeNegCounting = true;
        bool fwProbePosCounting = true;
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

        // ---- Alignment-Independent frame-window method (Juice's spec,
        // GD_alignment_independent_frame_window_spec.txt, 2026-09-01) ----
        // Separate, independently-selectable method -- does not replace or
        // modify Time-Based/Recovery Range above (spec's own hard
        // requirement). Reuses the SAME capture pass (fwCapStack) as the
        // existing method, since that capture already gives the p=0/nominal
        // predecessor checkpoint for free; only the post-capture sweep is
        // new. V1 scope deliberately, per the spec's own "Performance
        // Strategy" section: correctness first. NOT implemented yet, on
        // purpose, pending a confirmed-correct V1 and Nigel/Juice's say-so:
        // predecessor-state caching beyond the free p=0 reuse, adaptive Z,
        // result caching, checkpointing for long macros, and (the big one)
        // parallel workers -- this analyzer drives ONE live PlayLayer via
        // checkpoint restore, so "parallel" would need multiple simulator
        // instances that don't exist here, not a thread pool bolted on.
        // Continuation depth is capped at 0 or 1 (not arbitrary N) for the
        // same reason. Ask before adding any of these.
        bool fwUseAlignmentIndependent = false;
        int fwAiZ = 3;
        int fwAiContinuationDepth = 1; // 0 or 1 only in V1
        float fwAiClusterRatio = 1.15f;
        float fwAiDominantThreshold = 0.5f;

        enum class FwAiStatus { Dead, MissedTarget, Partial, Viable, DeadEnd };

        struct FwAiInputResult {
            uint32_t frame = 0;
            bool player2 = false;
            bool isRelease = false;
            float x = 0.f, y = 0.f;
            float percent = 0.f;
            int macroWindow = 0;           // this click's Time-Based/Recovery result, for comparison
            bool hasMacroMatch = false;    // false = Time-Based/Recovery hasn't measured this click at all (not "measured as 0")
            int representativeWindow = -1; // -1 = no dominant cluster, fell back to macroWindow
            int observedMin = 0, observedMax = 0;
            int validAlignments = 0, totalAlignments = 0;
            int dominantSupport = 0;
            float dominantPercent = 0.f;
            float sensitivity = 0.f;
            std::vector<int> perAlignmentShift;  // raw data, index-aligned with the next vector
            std::vector<int> perAlignmentWindow; // -- never discarded after picking the representative
        };
        std::vector<FwAiInputResult> fwAiResults;
        bool fwAiHasData = false;

        // -- live pipeline state, meaningful only while fwState is one of
        // the Ai* states below --
        size_t fwAiClickIdx = 0;
        int fwAiPredShift = 0;
        int fwAiPredMaxNeg = 0, fwAiPredMaxPos = 0;
        std::vector<int> fwAiValidPredShifts;
        std::vector<StoredFrame> fwAiValidPredCkpts;
        size_t fwAiAlignIdx = 0;
        int fwAiXShift = 0;
        int fwAiXPhase = -1; // -1 nominal-first, 0 negative sweep, 1 positive sweep
        int fwAiXMaxNeg = 0, fwAiXMaxPos = 0;
        bool fwAiXNegContiguous = true, fwAiXPosContiguous = true;
        int fwAiXLow = 0, fwAiXHigh = 0;   // contiguous span -- logging only, NOT the reported window (see fwAiXValidCount)
        int fwAiXValidCount = 0;           // the actual per-alignment window: a raw count, matching fwProbeValidCount
        std::vector<int> fwAiWindowPerAlign;
        // All three Ai* probe legs (AiBuildPred/AiSweepX/AiContinuation) use
        // RELATIVE tick counting from their own restore point, same as the
        // legacy method's fwProbeFrame/fwProbeHorizon -- deliberately NOT
        // absolute-frame comparison, since a checkpoint restore's first tick
        // isn't guaranteed to already show updater.getFrame() caught up to
        // the checkpoint's own recorded frame (the legacy Probing state
        // never relies on that equivalence either; matching it here rather
        // than assuming otherwise).
        uint32_t fwAiProbeFrame = 0;
        uint32_t fwAiProbeHorizon = 0;
        bool fwAiWantContCkpt = false;  // this leg should snapshot a mid-run checkpoint for depth-1 continuation
        uint32_t fwAiContCkptFrame = 0; // relative tick (matching fwAiProbeFrame) to snapshot it at
        bool fwAiContCkptTaken = false;
        StoredFrame fwAiContBaseCkpt;
        FwAiStatus fwAiPendingStatus = FwAiStatus::Dead; // status the current X shift reached before any continuation check
        bool fwAiInContinuation = false;
        int fwAiContStepIdx = 0;

        // Juice's ask (2026-09-02): let him pick a specific tested (predecessor
        // alignment, X shift) branch and watch it actually play out, at
        // regular speed, to see what it tested and where it died -- this is
        // also spec section 25's "Debug/Exhaustive mode" requirement, which
        // V1 shipped without. Reuses the SAME fwDebugMode toggle as the
        // legacy method's debug marks, populated separately here. Each
        // branch stores its own predecessor checkpoint directly (not just an
        // index) so "Go" can restore it without re-deriving anything.
        struct FwAiDebugBranch {
            size_t clickIdx = 0;
            int predShift = 0;
            int xShift = 0;
            FwAiStatus status = FwAiStatus::Dead;
            float x = 0.f, y = 0.f;
            StoredFrame predCkpt;
        };
        std::vector<FwAiDebugBranch> fwAiDebugBranches;
        void debugTeleportToAiBranch(size_t idx);

        // Nigel's ask (2026-09-02): a marker showing where the icon actually
        // starts from for each predecessor alignment that passed as valid --
        // i.e. the position baked into that alignment's own checkpoint,
        // where every one of its X-shift tests restores from. Directly
        // answers Juice's earlier "i dont understand what the predecessor
        // phase is measuring" -- this makes it visible. Unlike the pass/
        // fail circles (fwDebugMarks, reset per-alignment), these
        // accumulate for the WHOLE CLICK so the spread across every valid
        // alignment is visible at once, clearing only when moving to a new
        // click. Read directly off the stored checkpoint's baked-in
        // position -- no extra simulation needed.
        struct FwAiStartMark {
            float x = 0.f, y = 0.f;
            int predShift = 0;
        };
        std::vector<FwAiStartMark> fwAiStartMarks;
        void fwAiRecordAlignmentStartMark();

        void fwAiBeginClick();
        void fwAiBeginPredShift();
        void fwAiConcludePredShift(bool diedBeforeTarget);
        void fwAiBeginAlignSweep();
        void fwAiBeginXShift();
        void fwAiConcludeXShift(FwAiStatus status);
        void fwAiBeginContinuationCandidate();
        void fwAiAdvanceXSweep(bool viable);
        void fwAiFinishAlignment();
        void fwAiFinishClick();
        void fwAiFinishAnalysis();
        int fwAiNominalFirstOffset(int stepIdx) const;

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
        std::unordered_set<std::string> lemonadeMacros;
        std::unordered_set<std::string> brrrMacros;
        std::unordered_set<std::string> wakaMacros;
        std::unordered_set<std::string> youngstaMacros;
        std::unordered_set<std::string> knockerzMacros;
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

} // namespace gucci
