#pragma once

// Compatibility seam for anticroom's frame-window analyzer.
//
// The analyzer arrived as Silicate source and is kept here as close to
// verbatim as it can be, so that when anticroom ships a fix we can drop the
// new file in and re-apply rather than re-port. Everything it reaches for on
// the Silicate side is defined below in GucciBot's terms: Bot::get() becomes
// GucciEngine, SLValue becomes a pointer to a field of our own settings
// struct, and slc::Action is gb::Action (same fields -- his analyzer was
// ported out of GucciBot in the first place, which is why 14 of the 17
// symbols it wants already matched by name).
//
// Keep this file as the ONLY place that knows about both naming worlds. If
// something here starts needing real logic rather than a rename, that's a
// sign it belongs in the engine instead.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/GucciBot.hpp"
#include "core/action_types.hpp"
#include "core/checkpoint_player.hpp"

// --- Silicate's action type is ours under a different name ------------------

namespace slc {
    using Action = gucci::gb::Action;
    using ActionType = gucci::gb::ActionType;
}

using gucci::SavedPlayerCheckpoint;

// His SavedCheckpoint and our SavedCheckpointState are the same idea with the
// same two player halves; only the frame field is spelled differently
// (m_frame vs m_frameOffset), which is renamed at the call sites.
using SavedCheckpoint = gucci::SavedCheckpointState;

class FrameWindowAnalyzer;

// --- trail buffer -----------------------------------------------------------

// Silicate draws the player's path with a buffer of per-frame rects. GucciBot
// has no equivalent, and the analyzer uses it for two things:
//
//   1. Saving the trail before a run and restoring it after, so analysis
//      doesn't wipe what the player was looking at. With no trail, nothing to
//      save -- a genuine no-op, not a hidden failure.
//   2. checkCaptureAgainstTrail(), a desync check comparing the captured
//      position against the trail rect. It already self-disables on an empty
//      stream (see framewindow.cpp, "if (trail.empty())"), so it simply does
//      not run.
//
// (2) is a real check we lose, but GucciBot has its own version of the same
// idea in m_pathSamples/fwOffTrack. Wiring checkCaptureAgainstTrail to that is
// a phase-2 job; it is listed as such rather than left to look finished.
namespace tbuf {
    struct Rect {
        float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    };

    struct Sample {
        uint32_t frame = 0;
        Rect rect;
    };
}

class TrailBufferStub {
public:
    std::vector<tbuf::Sample> stream(int) const { return {}; }
    void loadSamples(std::vector<tbuf::Sample> const&,
                     std::vector<tbuf::Sample> const&) {}
};

// --- settings ---------------------------------------------------------------

struct FrameWindowTier {
    int id = 1;
    int minWindow = 0;
    int maxWindow = 999;
    std::string text = "";
    std::string audioPath = "";
    std::array<float, 4> color = {1.f, 1.f, 1.f, 1.f};
    bool showInHud = true;
};

// Copied field-for-field from anticroom's settings.hpp. Defaults are his --
// with one deliberate exception noted at subframeProbe.
struct FrameWindowSettings {
    bool enabled = true;
    int algorithm = 0;
    int sweepRange = 14;
    int maxFrames = 480;
    int slack = 2;
    int recoveryRange = 8;

    // HIS DEFAULT IS true. Held off here on Nigel's standing instruction: the
    // CBF sub-tick windows have a known bug on the cube that anticroom is
    // still chasing (as of 2026-09-20 he has instrumentation in but no fix --
    // notePress/m_tickBuffered is written and never read). m_fine stays 1
    // while this is false, which keeps the whole sub-tick path inert, so
    // flipping this to true is the entire "turn CBF on" switch once he lands
    // the fix. Don't flip it without asking Nigel.
    bool subframeProbe = false;

    int64_t cbfInputHz = 24000;
    bool cbfWholeMarkers = true;
    int cbfReadoutThreshold = 4;
    bool cbfTickGround = true;
    double lstarRespawn = 0.0;
    double lstarTarget = 86400.0;
    double lstarNerve = 0.0;
    double lstarFatigue = 0.0;
    double lstarCps = 0.0;
    bool lstarUseNerve = false;
    bool lstarUseFatigue = false;
    bool lstarUseCps = false;
    bool subframeBisect = true;
    bool subframeAll = false;
    int subframeScanPercent = 5;
    int tightThreshold = 4;
    bool jointSetupSweep = true;
    bool entrySweep = false;
    bool showSetupRange = true;
    bool markSetupVarying = false;
    bool setupHoldModes = true;
    bool showHzReadout = true;
    bool analysisVisuals = true;
    bool lockCamera = true;
    bool hideSpawnEffects = true;
    int budgetMs = 8;
    bool adaptiveBudget = true;
    int budgetSharePercent = 35;
    int maxBudgetMs = 60;
    int stepBatch = 35;
    bool fullRangeSweep = false;
    bool testShipReleases = true;
    bool testAllReleases = false;
    bool orbAwareReleaseSkip = true;
    bool showLabels = true;
    bool showTotals = true;
    bool verbose = true;
    bool analysisOverlay = false;
    bool statePlayerDiff = false;
    bool showMarkers = true;
    bool showDesynced = false;
    bool showTiming = true;
    int subframeDecimals = 2;
    bool showHud = true;
    bool playSounds = true;
    float soundVolume = 1.f;
    float markerRadius = 11.f;
    float markerScale = 0.5f;
    std::vector<FrameWindowTier> tiers = {
        {1, 0, 1, "", "", {0.996f, 0.310f, 0.314f, 1.f}, true},
        {2, 2, 2, "", "", {1.000f, 0.702f, 0.333f, 1.f}, true},
        {3, 3, 3, "", "", {0.992f, 0.996f, 0.471f, 1.f}, true},
        {4, 4, 4, "", "", {0.996f, 0.996f, 0.996f, 1.f}, true},
        {5, 5, 6, "", "", {0.545f, 0.996f, 0.545f, 1.f}, true},
        {6, 7, 8, "", "", {0.553f, 0.780f, 0.996f, 1.f}, true},
        {7, 9, 12, "", "", {0.471f, 0.467f, 0.996f, 1.f}, true},
    };
};

class SLSettings {
public:
    static SLSettings* get() {
        static SLSettings inst;
        return &inst;
    }

    FrameWindowSettings frameWindow;
};

// --- setting handles --------------------------------------------------------

// Silicate's SLValue is a named, persisted handle onto a settings field. Here
// it is just the pointer: persistence stays GucciBot's job, so the analyzer
// reads and writes live settings and whatever saves them keeps working. The
// key is retained because his UI and logs print it.
template <typename T>
class SLValue {
public:
    static std::shared_ptr<SLValue<T>> create(char const* key, T* backing) {
        return std::shared_ptr<SLValue<T>>(new SLValue<T>(key, backing));
    }

    T& inner() { return *m_backing; }
    T const& inner() const { return *m_backing; }
    std::string const& key() const { return m_key; }

private:
    SLValue(char const* key, T* backing) : m_key(key), m_backing(backing) {}

    std::string m_key;
    T* m_backing = nullptr;
};

template <typename T>
using SLValuePtr = std::shared_ptr<SLValue<T>>;

// --- the bot ----------------------------------------------------------------

// His analyzer talks to Bot::get()->updater() / ->replaySystem(). Both are
// plain members on GucciEngine, and the member names inside them already
// match, so this is pure forwarding.
class Bot {
public:
    static Bot* get() {
        static Bot inst;
        return &inst;
    }

    gucci::GucciUpdater& updater() { return gucci::GucciEngine::get()->updater; }
    gucci::GucciReplaySystem& replaySystem() {
        return gucci::GucciEngine::get()->replay;
    }
    gucci::GucciPracticeFix& practiceFix() {
        return gucci::GucciEngine::get()->practiceFix;
    }

    // Silicate keeps the analyzer as a member of Bot. Here it is a function
    // local static defined in shim.cpp, because GucciBot.hpp cannot include
    // framewindow.hpp (framewindow.hpp includes this file, which includes
    // GucciBot.hpp) -- so it is forward declared and handed back by reference.
    FrameWindowAnalyzer& frameWindow();
    bool isPlaying() const { return gucci::GucciEngine::get()->isPlaying(); }

    TrailBufferStub& trailBuffer() { return m_trail; }

private:
    TrailBufferStub m_trail;
};
