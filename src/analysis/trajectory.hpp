#pragma once

// Look-ahead forks of the player (path preview, Frame Extrapolation, Prevent
// Death's look-ahead, the Classic pathfinder's ranking, the sub-tick preview).
//
// 2026-10-01: the previous implementation was derived from ToastyReplay, whose
// author withdrew permission to use it, and was removed. Until a new one is
// written this service runs no forks: every query answers "no fork could run"
// and the features built on it stay idle. The interface is unchanged so the
// callers keep compiling.

#include <Geode/Geode.hpp>

#include <utility>
#include <vector>

namespace gucci {

    inline constexpr int kAgencyProbeFrames = 6;

    struct AgencyResult {
        bool matters = false;
        float divergence = 0.0f;
        int holdSurvived = 0;
        int releaseSurvived = 0;
    };

    class TrajectoryPredictionService {
    public:
        static TrajectoryPredictionService& get();

        bool isActiveSimulation() const { return false; }
        bool isProcessingOrbTouch() const { return false; }
        void markDirty() {}
        void clearOverlay() {}
        void attach(PlayLayer*) {}
        void detach() {}
        void updatePreview(PlayLayer*) {}
        bool probeAgency(PlayLayer*, PlayerObject*, AgencyResult&, int) { return false; }
        bool predictStep(PlayLayer*, PlayerObject*, bool, cocos2d::CCPoint&, float&) { return false; }

        struct SubtickPose {
            cocos2d::CCPoint position;
            cocos2d::CCRect hitbox;
            cocos2d::CCRect innerHitbox;
            float rotation = 0.f;
            bool died = false;
        };
        bool extrapolateSubtick(PlayLayer*, PlayerObject*, float, SubtickPose&) { return false; }
        void traceSubtickBranch(PlayLayer*, PlayerObject*, float, bool, cocos2d::CCDrawNode*, cocos2d::ccColor4F, float) {}
        void setOverlaySuppressed(bool) {}

        // -1: no fork could run.
        int survivesFor(PlayLayer*, PlayerObject*, int, int) { return -1; }
        int survivesScript(PlayLayer*, PlayerObject*, int, std::vector<std::pair<int, bool>> const&,
                           std::vector<cocos2d::CCPoint>* = nullptr) {
            return -1;
        }

        cocos2d::ccColor4F holdColor(bool player2) const {
            return player2 ? cocos2d::ccColor4F{0.2f, 0.5f, 0.95f, 1.f} : cocos2d::ccColor4F{0.3f, 0.9f, 0.35f, 1.f};
        }
        cocos2d::ccColor4F releaseColor() const { return {0.55f, 0.05f, 0.05f, 1.f}; }
        float lastProbeStep() const { return 0.f; }
        int lastForkKillerId() const { return 0; }

        void captureFrameDelta(float) {}
        void noteSimulatedDeath(PlayerObject*, GameObject* = nullptr) {}
        bool ownsPreviewPlayer(PlayerObject*) const { return false; }
        void onRealClick(bool, bool) {}
    };

} // namespace gucci
