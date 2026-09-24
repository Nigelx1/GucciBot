#ifndef _edit_core_hpp
#define _edit_core_hpp

// The frame editor's rules, with nothing else attached.
//
// Idea from Absense, which keeps its macro editing in a core with its own
// test file rather than inside the UI. The point is not tidiness: the editor
// writes macros back out verbatim, so a rule that is slightly wrong here
// produces a macro that plays differently from what the editor drew, and the
// only way anyone finds out is a desync in a real run.
//
// So this header has no ImGui, no Geode and no PlayLayer -- it is pure data
// in, data out, which is what makes edit_core_test.hpp able to assert on it.
// frame_editor.cpp uses these same functions, so the tests exercise the real
// editor and not a parallel copy of it.

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace gucci::editcore {

    // One press or release. `frame` is absolute from level start.
    struct Input {
        int32_t frame = 0;
        int actionType = 0;
        bool player2 = false;
        bool pressed = false;
        float stepOffset = 0.0f;
        size_t originalIndex = 0;
    };

    // A press paired with its release: one bar in the editor's timeline.
    struct Segment {
        int32_t startFrame = 0;
        int32_t endFrame = 0;
        bool player2 = false;
        int actionType = 0;
        size_t pressIndex = 0;
        size_t releaseIndex = 0;
        // False means the press was never released -- either it runs to the
        // end of the macro, or another press arrived on the same lane while it
        // was still down. The second case is malformed input, kept visible
        // rather than quietly repaired so the editor can draw it.
        bool hasRelease = true;
    };

    // Pairs presses with releases, per lane. A lane is (player, action type):
    // holds on different lanes are independent and may overlap freely.
    //
    // twoPlayerMode false folds everything onto player 1, which is what the
    // editor does when the macro has no second player.
    inline std::vector<Segment> buildSegments(std::vector<Input> const& inputs,
                                              bool twoPlayerMode,
                                              int32_t maxFrame) {
        std::vector<Segment> segments;

        std::vector<size_t> sorted(inputs.size());
        for (size_t i = 0; i < inputs.size(); i++)
            sorted[i] = i;
        std::stable_sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
            return inputs[a].frame < inputs[b].frame;
        });

        int const maxPlayer = twoPlayerMode ? 1 : 0;
        for (int player = 0; player <= maxPlayer; player++) {
            for (int act = 0; act <= 3; act++) {
                bool const isP2 = (player == 1);
                int openPress = -1;
                int32_t openFrame = 0;

                for (size_t si = 0; si < sorted.size(); si++) {
                    size_t const idx = sorted[si];
                    auto const& inp = inputs[idx];
                    bool const inputIsP2 = twoPlayerMode ? inp.player2 : false;
                    if (inputIsP2 != isP2 || inp.actionType != act)
                        continue;

                    if (inp.pressed) {
                        if (openPress >= 0) {
                            // A press while one is already down. Close the
                            // open one at this press so it stays visible, and
                            // mark it unreleased -- it is not a real hold.
                            Segment seg;
                            seg.startFrame = openFrame;
                            seg.endFrame = inp.frame;
                            seg.player2 = isP2;
                            seg.actionType = act;
                            seg.pressIndex = (size_t)openPress;
                            seg.releaseIndex = idx;
                            seg.hasRelease = false;
                            segments.push_back(seg);
                        }
                        openPress = static_cast<int>(idx);
                        openFrame = inp.frame;
                    } else {
                        if (openPress >= 0) {
                            Segment seg;
                            seg.startFrame = openFrame;
                            seg.endFrame = inp.frame;
                            seg.player2 = isP2;
                            seg.actionType = act;
                            seg.pressIndex = (size_t)openPress;
                            seg.releaseIndex = idx;
                            seg.hasRelease = true;
                            segments.push_back(seg);
                            openPress = -1;
                        }
                    }
                }

                if (openPress >= 0) {
                    Segment seg;
                    seg.startFrame = openFrame;
                    seg.endFrame = maxFrame;
                    seg.player2 = isP2;
                    seg.actionType = act;
                    seg.pressIndex = (size_t)openPress;
                    seg.releaseIndex = (size_t)openPress;
                    seg.hasRelease = false;
                    segments.push_back(seg);
                }
            }
        }

        std::sort(segments.begin(), segments.end(), [](Segment const& a, Segment const& b) {
            return a.startFrame < b.startFrame;
        });
        return segments;
    }

    // The nearest frames a hold may not cross, on its own lane.
    //
    // This is what stops a drag from producing press-press-release-release.
    // That shape is not a mangled drawing -- applyToBRR writes the inputs out
    // verbatim in frame order, so playback sees a press while already held and
    // then ends the hold on the FIRST release. The editor draws two holds and
    // the macro plays one short one, with nothing to see until a run desyncs.
    struct LaneBounds {
        int32_t lowerExclusive = -1;          // last frame used by the hold before
        int32_t upperExclusive = INT32_MAX;   // first frame used by the hold after
    };

    inline LaneBounds laneBoundsFor(std::vector<Input> const& inputs,
                                    bool twoPlayerMode,
                                    size_t pressIdx,
                                    size_t releaseIdx) {
        LaneBounds b;
        if (pressIdx >= inputs.size())
            return b;

        auto const& press = inputs[pressIdx];
        bool const lane2 = twoPlayerMode ? press.player2 : false;
        int32_t const from = press.frame;
        int32_t const to =
            (releaseIdx != pressIdx && releaseIdx < inputs.size()) ? inputs[releaseIdx].frame : from;

        for (size_t i = 0; i < inputs.size(); i++) {
            if (i == pressIdx || i == releaseIdx)
                continue;
            auto const& o = inputs[i];
            bool const oLane2 = twoPlayerMode ? o.player2 : false;
            if (oLane2 != lane2 || o.actionType != press.actionType)
                continue;

            if (o.frame <= from)
                b.lowerExclusive = std::max(b.lowerExclusive, o.frame);
            if (o.frame >= to)
                b.upperExclusive = std::min(b.upperExclusive, o.frame);
        }
        return b;
    }

    // Dragging a whole hold sideways. Returns the delta actually allowed:
    // clamped at frame 0 and at whatever neighbours the lane has.
    inline int32_t clampSegmentMove(std::vector<Input> const& inputs,
                                    bool twoPlayerMode,
                                    size_t pressIdx,
                                    size_t releaseIdx,
                                    int32_t origPressFrame,
                                    int32_t origReleaseFrame,
                                    int32_t delta) {
        if (pressIdx >= inputs.size())
            return 0;
        bool const paired = (releaseIdx != pressIdx && releaseIdx < inputs.size());
        int32_t const endFrame = paired ? origReleaseFrame : origPressFrame;

        if (origPressFrame + delta < 0)
            delta = -origPressFrame;

        auto const b = laneBoundsFor(inputs, twoPlayerMode, pressIdx, releaseIdx);
        if (origPressFrame + delta <= b.lowerExclusive)
            delta = b.lowerExclusive + 1 - origPressFrame;
        if (endFrame + delta >= b.upperExclusive)
            delta = b.upperExclusive - 1 - endFrame;
        // A lane so crowded that both clamps fight leaves the hold where it is
        // rather than inverting it.
        if (origPressFrame + delta < 0 || (paired && origPressFrame + delta >= endFrame + delta))
            return 0;
        return delta;
    }

    // Dragging the left edge: the press moves, the release stays.
    inline int32_t clampEdgeLeft(std::vector<Input> const& inputs,
                                 bool twoPlayerMode,
                                 size_t pressIdx,
                                 size_t releaseIdx,
                                 int32_t wantFrame) {
        if (pressIdx >= inputs.size())
            return wantFrame;
        wantFrame = std::max(wantFrame, (int32_t)0);
        if (releaseIdx != pressIdx && releaseIdx < inputs.size()) {
            int32_t const releaseFrame = inputs[releaseIdx].frame;
            if (wantFrame >= releaseFrame)
                wantFrame = releaseFrame - 1;
        }
        auto const b = laneBoundsFor(inputs, twoPlayerMode, pressIdx, releaseIdx);
        if (wantFrame <= b.lowerExclusive)
            wantFrame = b.lowerExclusive + 1;
        return std::max(wantFrame, (int32_t)0);
    }

    // Dragging the right edge: the release moves, the press stays.
    inline int32_t clampEdgeRight(std::vector<Input> const& inputs,
                                  bool twoPlayerMode,
                                  size_t pressIdx,
                                  size_t releaseIdx,
                                  int32_t wantFrame) {
        if (pressIdx >= inputs.size())
            return wantFrame;
        int32_t const pressFrame = inputs[pressIdx].frame;
        wantFrame = std::max(wantFrame, (int32_t)0);
        if (wantFrame <= pressFrame)
            wantFrame = pressFrame + 1;
        auto const b = laneBoundsFor(inputs, twoPlayerMode, pressIdx, releaseIdx);
        if (wantFrame >= b.upperExclusive)
            wantFrame = b.upperExclusive - 1;
        return std::max(wantFrame, pressFrame + 1);
    }

    // Does this input list play back the way the editor draws it? True means
    // every lane strictly alternates press, release, press, release.
    //
    // Nothing in the editor may leave this false: the moment it does, the
    // macro and the picture have parted company.
    inline bool lanesAlternate(std::vector<Input> const& inputs, bool twoPlayerMode) {
        std::vector<size_t> sorted(inputs.size());
        for (size_t i = 0; i < inputs.size(); i++)
            sorted[i] = i;
        std::stable_sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
            return inputs[a].frame < inputs[b].frame;
        });

        int const maxPlayer = twoPlayerMode ? 1 : 0;
        for (int player = 0; player <= maxPlayer; player++) {
            for (int act = 0; act <= 3; act++) {
                bool const isP2 = (player == 1);
                bool held = false;
                for (size_t idx : sorted) {
                    auto const& inp = inputs[idx];
                    bool const inputIsP2 = twoPlayerMode ? inp.player2 : false;
                    if (inputIsP2 != isP2 || inp.actionType != act)
                        continue;
                    if (inp.pressed == held)
                        return false;  // press while held, or release while not
                    held = inp.pressed;
                }
            }
        }
        return true;
    }

} // namespace gucci::editcore

#endif
