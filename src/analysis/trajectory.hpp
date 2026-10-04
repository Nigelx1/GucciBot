#pragma once

// Look-ahead forks of the player: path preview, Frame Extrapolation, Prevent
// Death's look-ahead, Find Best Tick, the sub-tick preview, the Classic
// pathfinder's ranking and its agency map, and the MCP survival probe.
//
// 2026-10-01: the previous implementation was derived from ToastyReplay,
// whose author withdrew permission to use it, and was removed. Rebuilt
// 2026-10-03 on Absense's simulation of the player, which GucciBot already
// carries for its pathfinder (absense/trajectory: Trajectory, its copies of
// the player, its World of the level's triggers). This service decides when
// and with what settings to ask it, and turns its answers into what the
// callers above expect. The copies are Absense's: one pair per level, made
// the first time a feature here needs them, and taken down with the level.
//
// Nothing here includes Absense's headers, which bring in its Silicate names.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace gucci {

    inline constexpr int kAgencyProbeFrames = 6;

    // Whether pressing or letting go on this tick changes anything: both are
    // run for a few ticks from here and compared.
    struct AgencyResult {
        bool matters = false;
        float divergence = 0.0f;  // the furthest apart the two paths got, in level units
        int holdSurvived = 0;
        int releaseSurvived = 0;
    };

    class TrajectoryPredictionService {
    public:
        static TrajectoryPredictionService& get();

        // Makes the copies for this level now, so the first question does not
        // pay for it in the middle of a physics step.
        void attach(PlayLayer* pl);
        // The drawn lines are rebuilt on the next frame whatever has changed.
        void markDirty();

        // Once per drawn frame (and from the level's set-up, reset and quit,
        // with nullptr on quit): draws or hides the path preview.
        void updatePreview(PlayLayer* pl);
        // The sub-tick preview is showing: the path preview steps aside.
        void setOverlaySuppressed(bool suppressed);

        // After every real tick (the frame midhook): the copies are set up
        // from the real player afresh, and the moving objects are sampled.
        void onRealTick();

        // How many ticks a copy of `player` lives, up to `maxTicks`, with
        // `input`: 1 holds (pressing now if the button is up), -1 lets go, 0
        // keeps whatever is held. -1 when no copy could run.
        int survivesFor(PlayLayer* pl, PlayerObject* player, int maxTicks, int input);
        // The same with a script: (tick offset, hold) events, offsets counted
        // from the current frame as GucciBot frames are (an input at the
        // current frame + 1 lands on the first simulated tick). Before the
        // first event the copy holds what the real player holds. `path`, when
        // given, receives the copy's position after every tick it ran.
        int survivesScript(PlayLayer* pl, PlayerObject* player, int maxTicks,
                           std::vector<std::pair<int, bool>> const& events,
                           std::vector<cocos2d::CCPoint>* path = nullptr);
        // The object id of what killed the last run's copy, -1 when it was
        // not an object or it lived.
        int lastForkKillerId() const;

        // Where `player` is after one tick holding (or not): Frame
        // Extrapolation. Worked out once per tick and kept for the drawn
        // frames in between.
        bool predictStep(PlayLayer* pl, PlayerObject* player, bool hold, cocos2d::CCPoint& position,
                         float& rotation);

        // Pressing and letting go compared over `frames` ticks (the Classic
        // pathfinder, once per tick it plays). Also handed to the agency map.
        bool probeAgency(PlayLayer* pl, PlayerObject* player, AgencyResult& out, int frames);

        // The sub-tick preview: where the player is `fraction` into the next
        // tick, and the hold / release paths from there.
        struct SubtickPose {
            cocos2d::CCPoint position;
            cocos2d::CCRect hitbox;
            cocos2d::CCRect innerHitbox;
            float rotation = 0.f;
            bool died = false;
        };
        bool extrapolateSubtick(PlayLayer* pl, PlayerObject* player, float fraction, SubtickPose& out);
        void traceSubtickBranch(PlayLayer* pl, PlayerObject* player, float fraction, bool hold,
                                cocos2d::CCDrawNode* node, cocos2d::ccColor4F color, float width);

        // `player` is one of the copies, not a real player.
        bool ownsPreviewPlayer(PlayerObject* player) const;
        // A copy of the player is being stepped right now (a drawn line, a
        // scripted run or a sub-tick question).
        bool isActiveSimulation() const;
        // A copy died where the caller's hook caught it first.
        void noteSimulatedDeath(PlayerObject* player, GameObject* object = nullptr);

        cocos2d::ccColor4F holdColor(bool player2) const {
            return player2 ? cocos2d::ccColor4F{0.2f, 0.5f, 0.95f, 1.f} : cocos2d::ccColor4F{0.3f, 0.9f, 0.35f, 1.f};
        }
        cocos2d::ccColor4F releaseColor() const { return {0.55f, 0.05f, 0.05f, 1.f}; }

    private:
        // Whether a question can be asked now (a level, a live player, no one
        // else driving the copies), and the copies made when it can.
        bool ready(PlayLayer* pl, PlayerObject* player);
        // The copies take the real player afresh when it has changed since
        // they last did (a tick ran, or the level was reset).
        void sync(PlayLayer* pl, bool force);
        // Applies GucciBot's path preview settings to Absense's.
        void applySettings();

        bool m_suppressed = false;
        // What the real player was when the copies last took it.
        struct Key {
            uint32_t progress = 0;
            uint32_t resets = 0;
            PlayLayer* layer = nullptr;
            bool operator==(Key const&) const = default;
        };
        Key m_synced;
        bool m_haveSync = false;
        // The settings last handed to Absense, so a change redraws the lines.
        bool m_appliedMoving = true;
        int m_appliedEvery = -1;
        // Frame Extrapolation's answer for this tick, per player and input.
        struct Step {
            bool valid = false;
            Key key;
            bool hold = false;
            cocos2d::CCPoint position;
            float rotation = 0.f;
        };
        Step m_step[2];
    };

} // namespace gucci
