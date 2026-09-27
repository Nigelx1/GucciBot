#pragma once

// Sub-tick frame advance -- anticroom's SubtickPreview (trajectory/subtick.*,
// "slc count" drop, 2026-09-26), ported 2026-09-27.
//
// While frame-advancing a CBF macro (or recording one), stepping forward moves
// through the tick in splits instead of whole ticks, and shows where the
// player's hitbox would be at that split, with the hold and release paths
// branching from there. The last split runs the real tick. With CBF Recording
// and Tick Splitting on, pressing at a split records the input at that point
// in the tick.
//
// His draws with his trajectory's extrapolate/extrapolateBranch; GucciBot's
// trajectory is its own design, so those two are rebuilt on it
// (TrajectoryPredictionService::extrapolateSubtick / traceSubtickBranch).

#include <chrono>
#include <cstdint>

class PlayLayer;

namespace scbf {

    class SubtickPreview {
    public:
        static SubtickPreview& get();

        bool usable() const;
        int splits() const;
        int split() const { return m_split; }
        // Where in the tick the preview is stepped to, 0 when it is not.
        double fraction() const;

        // A frame-advance step forward. False: the preview moved one split and
        // the real tick must not run. True: take the real step.
        bool forward();
        // A step back. True: the preview moved back within the tick and the
        // real backstep must not run.
        bool back();

        // Once per drawn frame.
        void update(PlayLayer* pl);

    private:
        int stride();
        void hide(PlayLayer* pl);
        void draw(PlayLayer* pl);

        int m_split = 0;
        uint32_t m_frame = 0;
        int m_drawnSplit = -1;
        uint32_t m_drawnFrame = UINT32_MAX;
        bool m_dimmed = false;
        bool m_cbfInUse = false;
        double m_carry = 0.0;
        std::chrono::steady_clock::time_point m_lastStep{};
    };

} // namespace scbf
